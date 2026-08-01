#include "media/protocol/ffmpeg_mux_protocol.h"

#include "common/log/logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}



namespace {

// FFmpeg 的错误码是负数整数，直接打印可读性很差；统一转换为 FFmpeg 提供的
// 文本描述，便于 PublisherResult 和日志同时携带同一份底层错误信息。
std::string ErrorString(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_make_error_string(buf, sizeof(buf), err);
    return buf;
}

// RTSP URL 需要显式设置 rtsp muxer 和 rtsp_transport 选项；其它协议仍让
// libavformat 根据 format_name 或 URL 自己选择输出格式。
bool StartsWithRtsp(const std::string& url) {
    return url.rfind("rtsp://", 0) == 0 || url.rfind("rtsps://", 0) == 0;
}

} // namespace

FfmpegMuxProtocol::~FfmpegMuxProtocol() {
    Stop();
}

PublisherResult FfmpegMuxProtocol::Start(
    const PublisherConfig& config,
    const std::vector<MediaTrackConfig>& tracks) {
    // Start 既是首次启动入口，也是重连时的会话重建入口。先清理旧 context，
    // 确保本次启动不会复用已经关闭或半初始化的 FFmpeg 资源。
    Stop();

    if (config.url.empty() || tracks.empty()) {
        LOG_ERROR("FfmpegMuxProtocol: invalid config");
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidConfiguration,
            "FFmpeg mux protocol requires an output URL and tracks");
    }

    config_ = config;
    tracks_ = tracks;
    stats_ = {};
    waiting_for_keyframe_ = false;

    // format_name 为空时保留 FFmpeg 的 URL 推断能力，但 RTSP URL 显式指定 rtsp
    // 可以避免仅凭 URL 或协议探测得到不稳定的输出格式。
    const char* format_name = config_.ffmpeg.format_name.empty()
        ? (StartsWithRtsp(config_.url) ? "rtsp" : nullptr)
        : config_.ffmpeg.format_name.c_str();

    int ret = avformat_alloc_output_context2(
        &fmt_ctx_, nullptr, format_name, config_.url.c_str());
    if (ret < 0 || !fmt_ctx_) {
        LOG_ERROR("FfmpegMuxProtocol: alloc output context failed: {}",
                  ErrorString(ret));
        FreeContext(false);
        return PublisherResult::Failure(
            PublisherErrorCode::ResourceOpenFailed,
            "failed to allocate FFmpeg output context: " + ErrorString(ret),
            ret);
    }

    // interrupt_callback 只在后续 FFmpeg I/O 调用期间被激活；opaque 指回当前
    // protocol 实例，用于读取本次操作的 deadline。
    fmt_ctx_->interrupt_callback.callback = &FfmpegMuxProtocol::InterruptCallback;
    fmt_ctx_->interrupt_callback.opaque = this;

    // 必须先创建 AVStream，再创建 bitstream filter，因为 filter 的输入 codecpar
    // 和 time_base 来自对应 AVStream。
    auto streams_result = BuildStreams(tracks_);
    if (!streams_result) {
        FreeContext(false);
        return streams_result;
    }

    auto filters_result = BuildBitstreamFilters();
    if (!filters_result) {
        FreeContext(false);
        return filters_result;
    }

    AVDictionary* opts = nullptr;
    if (StartsWithRtsp(config_.url) && !config_.ffmpeg.rtsp_transport.empty()) {
        // rtsp_transport 是 FFmpeg RTSP muxer 认识的选项；非 RTSP 输出不应携带它。
        av_dict_set(&opts, "rtsp_transport", config_.ffmpeg.rtsp_transport.c_str(), 0);
    }
    if (config_.ffmpeg.io_timeout_ms > 0) {
        // FFmpeg 的 rw_timeout 单位是微秒，而项目配置统一使用毫秒，因此这里做
        // 单位换算。AVIOInterruptCB 还会提供一层对阻塞调用的主动截止控制。
        av_dict_set_int(&opts,
                        "rw_timeout",
                        static_cast<int64_t>(config_.ffmpeg.io_timeout_ms) * 1000,
                        0);
    }

    if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
        // 某些输出格式不需要外部 AVIOContext；只有需要文件/网络 I/O 的 muxer 才打开 URL。
        BeginIoOperation();
        ret = avio_open2(&fmt_ctx_->pb, config_.url.c_str(),
                         AVIO_FLAG_WRITE, nullptr, &opts);
        EndIoOperation();
        if (ret < 0) {
            LOG_ERROR("FfmpegMuxProtocol: open output url failed: {}",
                      ErrorString(ret));
            av_dict_free(&opts);
            FreeContext(false);
            return PublisherResult::Failure(
                PublisherErrorCode::ConnectionFailed,
                "failed to open FFmpeg output URL: " + ErrorString(ret),
                ret);
        }
    }

    // header 阶段通常会完成远端 RTSP/RTMP 会话协商。只有 header 成功后，packet
    // 才有合法的输出 stream 和 muxer 上下文可以写入。
    BeginIoOperation();
    ret = avformat_write_header(fmt_ctx_, &opts);
    EndIoOperation();
    av_dict_free(&opts);
    if (ret < 0) {
        LOG_ERROR("FfmpegMuxProtocol: write header failed: {}", ErrorString(ret));
        FreeContext(false);
        return PublisherResult::Failure(
            PublisherErrorCode::RemoteRejected,
            "remote endpoint rejected the FFmpeg output header: " +
                ErrorString(ret),
            ret);
    }

    header_written_ = true;
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
        const auto* stream = fmt_ctx_->streams[i];
        LOG_INFO("FfmpegMuxProtocol: output stream {} time_base={}/{}",
                 stream->index,
                 stream->time_base.num,
                 stream->time_base.den);
    }
    LOG_INFO("FfmpegMuxProtocol: started {}", config_.url);
    return PublisherResult::Success();
}

PublisherResult FfmpegMuxProtocol::Write(
    const EncodedAccessUnit& access_unit) {
    if (!fmt_ctx_ || !header_written_) {
        LOG_ERROR("FfmpegMuxProtocol: Write called before Start");
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidState,
            "FFmpeg mux protocol must be started before writing");
    }

    // 远端断线后新会话从任意 P/B 帧开始通常无法解码。重连成功后先拒绝非关键帧，
    // 直到收到一个带 SPS/PPS 或已能独立解码的关键视频帧。
    if (waiting_for_keyframe_ &&
        (access_unit.media_type != MediaType::VIDEO || !access_unit.keyframe)) {
        return PublisherResult::Failure(
            PublisherErrorCode::RuntimeDisconnected,
            "FFmpeg output is waiting for a video keyframe after reconnect");
    }

    // 先尝试正常写入；只有明确返回 RuntimeDisconnected 才进入重连流程，格式错误、
    // 内存错误等不会被误判为网络断线。
    auto result = WriteEncodedPacket(access_unit);
    if (result) {
        if (access_unit.media_type == MediaType::VIDEO && access_unit.keyframe) {
            waiting_for_keyframe_ = false;
        }
        return result;
    }

    if (result.code != PublisherErrorCode::RuntimeDisconnected ||
        config_.ffmpeg.reconnect_attempts <= 0) {
        return result;
    }

    // Reconnect 会重新发送 header 并重建所有 filter。当前导致断线的 packet 会在
    // 重连成功后再次尝试，但视频非关键帧仍遵循上面的关键帧门槛。
    auto reconnect_result = Reconnect();
    if (!reconnect_result) {
        return reconnect_result;
    }

    waiting_for_keyframe_ = access_unit.media_type == MediaType::VIDEO;
    if (waiting_for_keyframe_ && !access_unit.keyframe) {
        return PublisherResult::Failure(
            PublisherErrorCode::RuntimeDisconnected,
            "FFmpeg output reconnected; waiting for the next video keyframe");
    }

    result = WriteEncodedPacket(access_unit);
    if (result) {
        waiting_for_keyframe_ = false;
    }
    return result;
}

PublisherResult FfmpegMuxProtocol::WriteEncodedPacket(
    const EncodedAccessUnit& access_unit) {
    // 该函数把“packet 构造”和“可选格式转换”串起来，实际网络写入集中在
    // WriteAvPacket，便于所有写路径统一处理超时、错误码和统计。
    AVPacket* out = av_packet_alloc();
    if (!out) {
        LOG_ERROR("FfmpegMuxProtocol: av_packet_alloc failed");
        return PublisherResult::Failure(
            PublisherErrorCode::InternalError,
            "failed to allocate an FFmpeg packet");
    }

    auto packet_result = BuildPacket(access_unit, out);
    if (!packet_result) {
        av_packet_free(&out);
        return packet_result;
    }

    const int stream_index = out->stream_index;
    auto filter_it = bitstream_filters_.find(stream_index);
    PublisherResult result;
    if (filter_it == bitstream_filters_.end()) {
        // 没有显式 filter 时保持输入 packet 格式不变，避免改变已有发布行为。
        result = WriteAvPacket(out);
    } else {
        auto* filter = filter_it->second;
        // av_bsf_send_packet 将 packet 送入 filter。发送完成后释放 out 当前引用，
        // 再由 receive_packet 取得 filter 产生的一个或多个输出 packet。
        int ret = av_bsf_send_packet(filter, out);
        if (ret < 0) {
            av_packet_free(&out);
            return PublisherResult::Failure(
                PublisherErrorCode::InvalidMediaPacket,
                "FFmpeg bitstream filter rejected the packet: " +
                    ErrorString(ret),
                ret);
        }
        av_packet_unref(out);

        result = PublisherResult::Success();
        while (true) {
            ret = av_bsf_receive_packet(filter, out);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                // filter 当前没有更多输出并不表示错误；部分 filter 会缓存 packet，
                // 这里等待后续输入继续产生输出。
                break;
            }
            if (ret < 0) {
                result = PublisherResult::Failure(
                    PublisherErrorCode::InvalidMediaPacket,
                    "FFmpeg bitstream filter failed: " + ErrorString(ret),
                    ret);
                break;
            }

            // 某些 filter 可能改写 packet 的元数据，但它仍属于原来的输出 stream。
            out->stream_index = stream_index;
            result = WriteAvPacket(out);
            av_packet_unref(out);
            if (!result) {
                break;
            }
        }
    }
    av_packet_free(&out);
    return result;
}

PublisherResult FfmpegMuxProtocol::WriteAvPacket(AVPacket* packet) {
    if (!packet || !fmt_ctx_ || !header_written_) {
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidState,
            "FFmpeg output packet cannot be written before the header");
    }

    // 所有真正可能阻塞的 packet I/O 都经过同一组 deadline 和 interrupt callback。
    BeginIoOperation();
    const int ret = av_write_frame(fmt_ctx_, packet);
    EndIoOperation();
    const auto packet_size = packet->size > 0
        ? static_cast<std::uint64_t>(packet->size)
        : 0;
    if (ret < 0) {
        // av_write_frame 失败通常意味着远端连接已不可用；上层 Write 会依据该错误码
        // 决定是否重连。packet 构造或 filter 错误则在更早阶段返回其它错误码。
        LOG_ERROR("FfmpegMuxProtocol: write packet failed: {}", ErrorString(ret));
        return PublisherResult::Failure(
            PublisherErrorCode::RuntimeDisconnected,
            "FFmpeg output failed while publishing: " + ErrorString(ret),
            ret);
    }

    // 只有 FFmpeg 接受写入后才累计统计；经过 filter 后以最终输出 packet 的大小为准。
    ++stats_.packets_published;
    stats_.bytes_published += packet_size;
    return PublisherResult::Success();
}

void FfmpegMuxProtocol::Stop() {
    FreeContext(true);
}

std::string FfmpegMuxProtocol::GetOutputUrl() const {
    return config_.url;
}

PublisherStats FfmpegMuxProtocol::GetStats() const {
    return stats_;
}

int FfmpegMuxProtocol::MapCodecType(CodecType type) {
    // 这里仅做项目枚举到 FFmpeg 枚举的静态映射，不负责检查容器是否接受该 codec
    // 组合；容器兼容性由 avformat_write_header 在启动阶段最终确认。
    switch (type) {
        case CodecType::H264:
            return AV_CODEC_ID_H264;
        case CodecType::H265:
            return AV_CODEC_ID_HEVC;
        case CodecType::AAC:
            return AV_CODEC_ID_AAC;
        case CodecType::OPUS:
            return AV_CODEC_ID_OPUS;
        case CodecType::G711A:
            return AV_CODEC_ID_PCM_ALAW;
        case CodecType::G711U:
            return AV_CODEC_ID_PCM_MULAW;
        default:
            return AV_CODEC_ID_NONE;
    }
}

PublisherResult FfmpegMuxProtocol::BuildStreams(
    const std::vector<MediaTrackConfig>& tracks) {
    // 每次 Start 都从零建立映射。stream index 由 FFmpeg 按创建顺序分配，不能把
    // track_id 直接当作 AVStream 下标保存。
    track_to_stream_index_.clear();

    for (const auto& track : tracks) {
        AVStream* stream = avformat_new_stream(fmt_ctx_, nullptr);
        if (!stream) {
            LOG_ERROR("FfmpegMuxProtocol: avformat_new_stream failed");
            return PublisherResult::Failure(
                PublisherErrorCode::InternalError,
                "failed to allocate an FFmpeg output stream");
        }

        track_to_stream_index_[track.track_id] = stream->index;

        // 上游时间戳以 access_unit.time_base 表示，写入前会被重标定到这里的输出
        // time_base。沿用轨道配置可以让音视频分别保持合适的时间精度。
        stream->time_base = AVRational{
            track.time_base_num > 0 ? track.time_base_num : 1,
            track.time_base_den > 0 ? track.time_base_den : 1000000,
        };

        // codecpar 描述的是“已经编码的输入”，因此这里只填 codec、尺寸、采样率和
        // extradata，不创建编码器，也不对 packet 内容做解码/重编码。
        AVCodecParameters* par = stream->codecpar;
        par->codec_id = static_cast<AVCodecID>(MapCodecType(track.codec_type));
        par->codec_tag = 0;

        if (par->codec_id == AV_CODEC_ID_NONE) {
            LOG_ERROR("FfmpegMuxProtocol: unsupported codec {}",
                      static_cast<int>(track.codec_type));
            return PublisherResult::Failure(
                PublisherErrorCode::UnsupportedCodec,
                "FFmpeg mux protocol does not support the configured codec");
        }

        if (track.media_type == MediaType::VIDEO) {
            par->codec_type = AVMEDIA_TYPE_VIDEO;
            par->width = track.width;
            par->height = track.height;
        } else if (track.media_type == MediaType::AUDIO) {
            par->codec_type = AVMEDIA_TYPE_AUDIO;
            par->sample_rate = track.sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
            // FFmpeg 新版使用 AVChannelLayout；保留旧版分支是为了兼容项目支持的
            // 不同 libavutil ABI。
            av_channel_layout_uninit(&par->ch_layout);
            av_channel_layout_default(&par->ch_layout, track.channels);
#else
            par->channels = track.channels;
            par->channel_layout = av_get_default_channel_layout(track.channels);
#endif
        } else {
            LOG_ERROR("FfmpegMuxProtocol: unsupported media type {}",
                      static_cast<int>(track.media_type));
            return PublisherResult::Failure(
                PublisherErrorCode::InvalidConfiguration,
                "FFmpeg output track has an unsupported media type");
        }

        if (!track.extra_data.empty()) {
            // extradata（如 H264/H265 的 SPS/PPS 或 AAC AudioSpecificConfig）通常会
            // 被 muxer 用于生成 header/SDP。FFmpeg 要求尾部保留 padding 字节。
            const auto size = track.extra_data.size();
            par->extradata = static_cast<std::uint8_t*>(
                av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!par->extradata) {
                LOG_ERROR("FfmpegMuxProtocol: allocate extradata failed");
                return PublisherResult::Failure(
                    PublisherErrorCode::InternalError,
                    "failed to allocate FFmpeg codec extradata");
            }
            std::memcpy(par->extradata, track.extra_data.data(), size);
            par->extradata_size = static_cast<int>(size);
        }
    }

    return PublisherResult::Success();
}

PublisherResult FfmpegMuxProtocol::BuildBitstreamFilters() {
    // filter 是按配置显式启用的，而不是根据 packet 内容自动猜测。自动猜测容易把
    // 已经是 Annex-B 的 H264 再转换一次，因此把格式判断责任留给配置/上游。
    for (const auto& [track_id, filter_name] :
         config_.ffmpeg.bitstream_filters) {
        const auto track_it = std::find_if(
            tracks_.begin(),
            tracks_.end(),
            [track_id](const MediaTrackConfig& track) {
                return track.track_id == track_id;
            });
        const auto stream_it = track_to_stream_index_.find(track_id);
        if (track_it == tracks_.end() || stream_it == track_to_stream_index_.end()) {
            return PublisherResult::Failure(
                PublisherErrorCode::InvalidConfiguration,
                "FFmpeg bitstream filter references an unknown track");
        }

        // 先把项目 track_id 转成 FFmpeg stream，再在对应的 codec 参数上初始化 filter。
        const auto* filter = av_bsf_get_by_name(filter_name.c_str());
        if (!filter) {
            return PublisherResult::Failure(
                PublisherErrorCode::UnsupportedCodec,
                "FFmpeg bitstream filter is not available: " + filter_name);
        }

        AVBSFContext* context = nullptr;
        int ret = av_bsf_alloc(filter, &context);
        if (ret < 0 || !context) {
            return PublisherResult::Failure(
                PublisherErrorCode::InternalError,
                "failed to allocate FFmpeg bitstream filter: " +
                    ErrorString(ret),
                ret);
        }

        const auto* stream = fmt_ctx_->streams[stream_it->second];
        // AVBSFContext 需要知道输入 codec 和时间基；否则 filter 无法正确解析或保留
        // packet 的时序信息。
        ret = avcodec_parameters_copy(context->par_in, stream->codecpar);
        if (ret < 0) {
            av_bsf_free(&context);
            return PublisherResult::Failure(
                PublisherErrorCode::InternalError,
                "failed to copy codec parameters to FFmpeg bitstream filter: " +
                    ErrorString(ret),
                ret);
        }
        context->time_base_in = stream->time_base;

        ret = av_bsf_init(context);
        if (ret < 0) {
            av_bsf_free(&context);
            return PublisherResult::Failure(
                PublisherErrorCode::UnsupportedCodec,
                "failed to initialize FFmpeg bitstream filter '" +
                    filter_name + "': " + ErrorString(ret),
                ret);
        }

        bitstream_filters_[stream_it->second] = context;
        LOG_INFO("FfmpegMuxProtocol: enabled bitstream filter '{}' for track {}",
                 filter_name,
                 track_id);
    }

    return PublisherResult::Success();
}

PublisherResult FfmpegMuxProtocol::BuildPacket(
    const EncodedAccessUnit& access_unit,
    AVPacket* out) const {
    if (!out) {
        return PublisherResult::Failure(
            PublisherErrorCode::InternalError,
            "FFmpeg output packet is null");
    }

    // protocol 层只接受已经确定 track_id 的 access unit；这里再次检查是为了防止
    // adapter 以外的直接调用者绕过路由约束。
    const auto stream_it = track_to_stream_index_.find(access_unit.track_id);
    if (stream_it == track_to_stream_index_.end()) {
        LOG_ERROR("FfmpegMuxProtocol: unknown track id {}", access_unit.track_id);
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidMediaPacket,
            "access unit uses an unknown FFmpeg track id");
    }

    av_packet_unref(out);

    int ret = 0;
    if (access_unit.backend.type == BackendHandle::FFMPEG && access_unit.backend.ptr) {
        // 上游提供 FFmpeg AVPacket 时使用引用计数共享数据，避免对大 packet 再复制一份。
        // av_packet_ref 成功后 out 拥有独立引用，后续可以安全地 rescale 和 unref。
        auto* src = static_cast<AVPacket*>(access_unit.backend.ptr);
        ret = av_packet_ref(out, src);
        if (ret < 0) {
            LOG_ERROR("FfmpegMuxProtocol: packet ref failed: {}", ErrorString(ret));
            return PublisherResult::Failure(
                PublisherErrorCode::InternalError,
                "failed to reference the FFmpeg input packet: " +
                    ErrorString(ret),
                ret);
        }
    } else {
        // 普通 buffer 路径没有 FFmpeg 所有权信息，因此由 protocol 分配 AVPacket 并复制
        // 编码数据，保证 Send 返回后上游可以立即复用自己的 buffer。
        if (!access_unit.encoded_data || !access_unit.encoded_data->Data() ||
            access_unit.encoded_data->Size() == 0) {
            LOG_ERROR("FfmpegMuxProtocol: access unit buffer is empty");
            return PublisherResult::Failure(
                PublisherErrorCode::InvalidMediaPacket,
                "FFmpeg access unit buffer is empty");
        }

        const auto size = static_cast<int>(access_unit.encoded_data->Size());
        ret = av_new_packet(out, size);
        if (ret < 0) {
            LOG_ERROR("FfmpegMuxProtocol: packet allocation failed: {}",
                      ErrorString(ret));
            return PublisherResult::Failure(
                PublisherErrorCode::InternalError,
                "failed to allocate FFmpeg packet data: " + ErrorString(ret),
                ret);
        }
        std::memcpy(out->data,
                    access_unit.encoded_data->Data(),
                    access_unit.encoded_data->Size());
    }

    out->stream_index = stream_it->second;
    out->pts = access_unit.pts;
    out->dts = access_unit.dts;
    out->duration = access_unit.duration;
    out->pos = -1;

    // packet 的 PTS/DTS/duration 先按上游 time_base 写入，再统一转换到输出 stream
    // 的 time_base。没有有效 time_base 时使用项目默认的微秒基准。
    const auto* stream = fmt_ctx_->streams[out->stream_index];
    const AVRational source_time_base{
        access_unit.time_base.num > 0 ? access_unit.time_base.num : 1,
        access_unit.time_base.den > 0 ? access_unit.time_base.den : 1000000,
    };
    av_packet_rescale_ts(out, source_time_base, stream->time_base);

    // 关键帧标记会影响 muxer 的索引和客户端开始播放的位置，不能只依赖输入 AVPacket
    // 是否恰好带有旧 flags，因为 buffer 路径可能没有携带该标志。
    if (access_unit.keyframe) {
        out->flags |= AV_PKT_FLAG_KEY;
    } else {
        out->flags &= ~AV_PKT_FLAG_KEY;
    }

    return PublisherResult::Success();
}

PublisherResult FfmpegMuxProtocol::Reconnect() {
    // 保存配置、轨道和累计统计。Start 会清空当前会话并重新初始化统计，因此成功后
    // 必须恢复旧统计，避免一次网络断开把长期运行指标归零。
    const auto reconnect_config = config_;
    const auto reconnect_tracks = tracks_;
    const auto previous_stats = stats_;
    PublisherResult last_result = PublisherResult::Failure(
        PublisherErrorCode::RuntimeDisconnected,
        "FFmpeg output reconnect was not attempted");

    for (int attempt = 0;
         attempt < reconnect_config.ffmpeg.reconnect_attempts;
         ++attempt) {
        // 先释放旧会话，再调用 Start。Start 内部还会调用 Stop，双重清理依赖各资源
        // 释放函数的幂等性，但不会重复关闭已经置空的 AVIO/BSF 指针。
        FreeContext(false);
        last_result = Start(reconnect_config, reconnect_tracks);
        if (last_result) {
            // 重连不应清零整个发布任务的累计统计。
            stats_ = previous_stats;
            LOG_INFO("FfmpegMuxProtocol: reconnected to {} after {} attempt(s)",
                     reconnect_config.url,
                     attempt + 1);
            return PublisherResult::Success();
        }

        if (attempt + 1 < reconnect_config.ffmpeg.reconnect_attempts &&
            reconnect_config.ffmpeg.reconnect_backoff_ms > 0) {
            // 当前使用固定退避，避免断线时紧密循环占满 CPU 和远端连接尝试。
            std::this_thread::sleep_for(std::chrono::milliseconds(
                reconnect_config.ffmpeg.reconnect_backoff_ms));
        }
    }

    return PublisherResult::Failure(
        PublisherErrorCode::RuntimeDisconnected,
        "FFmpeg output reconnect failed: " + last_result.message,
        last_result.native_error);
}

void FfmpegMuxProtocol::BeginIoOperation() {
    if (config_.ffmpeg.io_timeout_ms <= 0) {
        // 0 表示沿用 FFmpeg 默认行为，不启用项目侧 deadline。此时回调必须保持
        // 返回 0，否则会无条件中断所有 I/O。
        io_operation_active_ = false;
        return;
    }
    // 每个 avio_open2/avformat_write_header/av_write_frame 等操作重新计算 deadline，
    // 因此超时是“单次操作超时”，不会被前一个慢操作消耗掉。
    io_deadline_ = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(config_.ffmpeg.io_timeout_ms);
    io_operation_active_ = true;
}

void FfmpegMuxProtocol::EndIoOperation() {
    io_operation_active_ = false;
}

int FfmpegMuxProtocol::InterruptCallback(void* opaque) {
    // 回调可能在 FFmpeg 内部线程/调用栈中执行，只读取不可变的截止时间和活动标志，
    // 不在这里做日志、释放资源或发起重连等复杂操作。
    const auto* protocol = static_cast<const FfmpegMuxProtocol*>(opaque);
    if (!protocol || !protocol->io_operation_active_) {
        return 0;
    }
    return std::chrono::steady_clock::now() >= protocol->io_deadline_ ? 1 : 0;
}

void FfmpegMuxProtocol::FreeContext(bool write_trailer) {
    if (fmt_ctx_ && write_trailer && header_written_) {
        // 正常 Stop 需要写 trailer 完成容器/RTSP 会话收尾；重连和启动失败传 false，
        // 因为此时远端往往已经不可达，写 trailer 只会增加额外等待。
        BeginIoOperation();
        const int ret = av_write_trailer(fmt_ctx_);
        EndIoOperation();
        if (ret < 0) {
            LOG_WARN("FfmpegMuxProtocol: write trailer failed: {}", ErrorString(ret));
        }
    }

    if (fmt_ctx_) {
        if (fmt_ctx_->pb && !(fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
            // avio_closep 会同时关闭底层 I/O 并把 pb 置空，后续释放 context 时不会
            // 再次访问已关闭的句柄。
            BeginIoOperation();
            const int ret = avio_closep(&fmt_ctx_->pb);
            EndIoOperation();
            if (ret < 0) {
                LOG_WARN("FfmpegMuxProtocol: close output failed: {}", ErrorString(ret));
            }
        }

        // filter 必须在 output context 释放前销毁，避免遗留对 codecpar/time_base 的
        // 引用；无论 context 是否存在都执行一次清理，覆盖部分初始化失败路径。
        for (auto& [stream_index, filter] : bitstream_filters_) {
            (void)stream_index;
            av_bsf_free(&filter);
        }
        bitstream_filters_.clear();
        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
    } else {
        for (auto& [stream_index, filter] : bitstream_filters_) {
            (void)stream_index;
            av_bsf_free(&filter);
        }
        bitstream_filters_.clear();
    }

    // 清空所有会话状态，使下一次 Start 从干净状态开始；stats_ 则刻意保留，供
    // GetStats 在 Stop 后继续返回本次任务的累计发布统计。
    tracks_.clear();
    track_to_stream_index_.clear();
    header_written_ = false;
    waiting_for_keyframe_ = false;
    io_operation_active_ = false;
    config_ = {};
}
