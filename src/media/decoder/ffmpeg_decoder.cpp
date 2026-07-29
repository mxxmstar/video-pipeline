/// @file ffmpeg_decoder.cpp
/// FFmpeg 软件解码器实现。

#include "media/decoder/ffmpeg_decoder.h"
#include "media/ffmpeg_frame_buffer.h"

#include "common/log/logger.h"

#include <cstddef>
#include <cstring>
#include <limits>

extern "C" {
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
}

namespace {

AVRational ToAVRational(const Rational& rational) {
    return AVRational{
        rational.num > 0 ? rational.num : 1,
        rational.den > 0 ? rational.den : AV_TIME_BASE,
    };
}

bool IsValidTimeBase(AVRational time_base) {
    return time_base.num > 0 && time_base.den > 0;
}

AVRational FrameTimeBase(const AVFrame* frame, const MediaStreamInfo& stream_info) {
    if (frame && IsValidTimeBase(frame->time_base)) {
        return frame->time_base;
    }
    return ToAVRational(stream_info.time_base);
}

int64_t TimestampToUs(int64_t timestamp, AVRational time_base) {
    if (timestamp == AV_NOPTS_VALUE) {
        return 0;
    }

    constexpr AVRational kMicrosecondTimeBase{1, AV_TIME_BASE};
    return av_rescale_q(timestamp, time_base, kMicrosecondTimeBase);
}

int64_t FramePts(const AVFrame* frame) {
    if (!frame) {
        return AV_NOPTS_VALUE;
    }
    return frame->pts != AV_NOPTS_VALUE ? frame->pts : frame->best_effort_timestamp;
}

} // namespace

// ── ctor / dtor ────────────────────────────────────────────────────

FFmpegDecoder::~FFmpegDecoder() {
    Close();
}

// ── IDecoder ─────────────────────────────────────────────────────────

bool FFmpegDecoder::Open(const MediaStreamInfo& info) {
    if (info.codec_type == CodecType::UNKNOWN) {
        LOG_ERROR("FFmpegDecoder:Open rejected: unknown codec");
        return false;
    }

    // 1. CodecType -> AVCodecID
    AVCodecID codec_id = AV_CODEC_ID_NONE;
    switch (info.codec_type) {
        case CodecType::H264:  codec_id = AV_CODEC_ID_H264; break;
        case CodecType::H265:  codec_id = AV_CODEC_ID_HEVC; break;
        case CodecType::AAC:   codec_id = AV_CODEC_ID_AAC;  break;
        case CodecType::OPUS:  codec_id = AV_CODEC_ID_OPUS; break;
        case CodecType::G711A: codec_id = AV_CODEC_ID_PCM_ALAW; break;
        case CodecType::G711U: codec_id = AV_CODEC_ID_PCM_MULAW; break;
        case CodecType::JPEG:  codec_id = AV_CODEC_ID_MJPEG; break;
        default:
            LOG_ERROR("FFmpegDecoder:Open: unsupported codec type {}", static_cast<int>(info.codec_type));
            return false;
    }

    // 2. 查找解码器
    const AVCodec* decoder = avcodec_find_decoder(codec_id);
    if (!decoder) {
        LOG_ERROR("FFmpegDecoder:Open: avcodec_find_decoder failed for id={}", static_cast<int>(codec_id));
        return false;
    }

    // 3. 分配解码器上下文
    codec_ctx_ = avcodec_alloc_context3(decoder);
    if (!codec_ctx_) {
        LOG_ERROR("FFmpegDecoder:Open: avcodec_alloc_context3 failed");
        return false;
    }

    // 4. 设置解码参数
    if (info.media_type == MediaType::VIDEO) {
        codec_ctx_->width       = info.get_detail<VideoStreamInfo>().width;
        codec_ctx_->height      = info.get_detail<VideoStreamInfo>().height;
        codec_ctx_->pix_fmt     = AV_PIX_FMT_NONE;   // 由解码器自动检测
        codec_ctx_->thread_count = 1;                  // 单线程，保证确定性
    } else if (info.media_type == MediaType::AUDIO) {
        const auto& audio_info = info.get_detail<AudioStreamInfo>();
        codec_ctx_->sample_rate    = audio_info.sample_rate;
        codec_ctx_->ch_layout.nb_channels = audio_info.channels;
        codec_ctx_->ch_layout.u.mask = audio_info.channel_layout;
        codec_ctx_->sample_fmt   = AV_SAMPLE_FMT_NONE; // 由解码器自动检测
    }

    // 5. 设置 extradata（SPS/PPS 等）
    codec_ctx_->pkt_timebase = ToAVRational(info.time_base);

    if (!info.extra_data.empty()) {
        codec_ctx_->extradata_size = static_cast<int>(info.extra_data.size());
        codec_ctx_->extradata = static_cast<uint8_t*>(
            av_malloc(info.extra_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!codec_ctx_->extradata) {
            LOG_ERROR("FFmpegDecoder:Open: av_malloc for extradata failed");
            avcodec_free_context(&codec_ctx_);
            return false;
        }
        std::memcpy(codec_ctx_->extradata, info.extra_data.data(),
                    info.extra_data.size());
        std::memset(codec_ctx_->extradata + info.extra_data.size(), 0,
                    AV_INPUT_BUFFER_PADDING_SIZE);
    }

    // 6. 打开解码器
    int ret = avcodec_open2(codec_ctx_, decoder, nullptr);
    if (ret < 0) {
        char buf[AV_ERROR_MAX_STRING_SIZE];
        av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, ret);
        LOG_ERROR("FFmpegDecoder:Open: avcodec_open2 failed: {}", buf);
        avcodec_free_context(&codec_ctx_);
        return false;
    }

    stream_info_ = info;
    if (info.media_type == MediaType::VIDEO) {
        info.Dump();
    } else if (info.media_type == MediaType::AUDIO) {
        info.Dump();
    }
    return true;
}

void FFmpegDecoder::Close() {
    if (codec_ctx_) {
        // 冲刷解码器残留帧
        avcodec_send_packet(codec_ctx_, nullptr);
        (void)ReceiveFrames();

        avcodec_free_context(&codec_ctx_);
        codec_ctx_ = nullptr;
    }
    stream_info_ = {};
}

bool FFmpegDecoder::Decode(std::shared_ptr<MediaPacket> packet) {
    if (!codec_ctx_) {
        LOG_ERROR("FFmpegDecoder:Decode: codec_ctx_ is null");
        return false;
    }
    if (!packet || !packet->buffer) {
        LOG_ERROR("FFmpegDecoder:Decode: invalid packet");
        return false;
    }

    // FFmpegPuller 输出的 packet 已经持有 AVPacket，可以直接复用。
    // AVTP/RTP 等输入通常只持有普通 IMediaBuffer，这里临时包装成 AVPacket。
    AVPacket* avpkt = nullptr;
    AVPacket* temporary_packet = nullptr;
    if (packet->backend.type == BackendHandle::FFMPEG) {
        avpkt = static_cast<AVPacket*>(packet->backend.ptr);
    } else {
        const std::size_t packet_size = packet->buffer->Size();
        if (!packet->buffer->Data() || packet_size == 0 ||
            packet_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            LOG_ERROR("FFmpegDecoder:Decode: invalid raw packet");
            return false;
        }

        temporary_packet = av_packet_alloc();
        if (!temporary_packet ||
            av_new_packet(temporary_packet, static_cast<int>(packet_size)) < 0) {
            av_packet_free(&temporary_packet);
            LOG_ERROR("FFmpegDecoder:Decode: AVPacket allocation failed");
            return false;
        }

        std::memcpy(temporary_packet->data, packet->buffer->Data(), packet_size);

        // MediaPacket 的时间戳单位由 packet->time_base 描述；而 libavcodec 期望
        // AVPacket 时间戳落在 codec_ctx_->pkt_timebase 下。这里做一次显式换算，
        // 使 AVTP 这类微秒时间戳，以及测试中从 FFmpegPuller 复制出来的微秒
        // MediaPacket，都能进入同一条 decoder 路径。
        const AVRational source_time_base = ToAVRational(packet->time_base);
        const AVRational decoder_time_base = IsValidTimeBase(codec_ctx_->pkt_timebase)
            ? codec_ctx_->pkt_timebase
            : source_time_base;
        const auto rescale_timestamp = [&](int64_t timestamp) -> int64_t {
            return timestamp == AV_NOPTS_VALUE
                ? AV_NOPTS_VALUE
                : av_rescale_q(timestamp, source_time_base, decoder_time_base);
        };

        temporary_packet->pts = rescale_timestamp(packet->pts);
        temporary_packet->dts = rescale_timestamp(packet->dts);
        temporary_packet->duration = rescale_timestamp(packet->duration);
        if (packet->keyframe) {
            temporary_packet->flags |= AV_PKT_FLAG_KEY;
        }
        avpkt = temporary_packet;
    }

    if (!avpkt) {
        av_packet_free(&temporary_packet);
        LOG_ERROR("backend AVPacket is null");
        return false;
    }

    const int ret = avcodec_send_packet(codec_ctx_, avpkt);
    av_packet_free(&temporary_packet);
    if (ret < 0) {
        char buf[AV_ERROR_MAX_STRING_SIZE];
        av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, ret);
        LOG_ERROR("avcodec_send_packet failed: {}", buf);
        return false;
    }

    return ReceiveFrames();
}

void FFmpegDecoder::SetFrameCallback(FrameCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    frame_cb_ = std::move(cb);
}

// ── 内部 ──────────────────────────────────────────────────────────

bool FFmpegDecoder::ReceiveFrames() {
    if (!codec_ctx_)
        return false;

    int ret = 0;
    AVFrame* frame = av_frame_alloc();
    if (!frame) {
        LOG_ERROR("av_frame_alloc failed");
        return false;
    }

    while (ret >= 0) {
        ret = avcodec_receive_frame(codec_ctx_, frame);

        if (ret == AVERROR(EAGAIN)) {
            // 解码器需要更多数据，正常
            break;
        }
        if (ret == AVERROR_EOF) {
            LOG_DEBUG("EOF");
            break;
        }
        if (ret < 0) {
            char buf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, ret);
            LOG_ERROR("avcodec_receive_frame failed: {}", buf);
            av_frame_free(&frame);
            return false;
        }

        // ── 构建 FFmpegFrameBuffer（接管 frame 所有权） ──
        int size = 0;
        if (stream_info_.media_type == MediaType::VIDEO) {
            size = av_image_get_buffer_size(
                static_cast<AVPixelFormat>(frame->format),
                frame->width, frame->height, 1);
        } else if (stream_info_.media_type == MediaType::AUDIO) {
            size = av_samples_get_buffer_size(
                nullptr,
                frame->ch_layout.nb_channels,
                frame->nb_samples,
                static_cast<AVSampleFormat>(frame->format),
                1);
        }

        if (size <= 0) {
            LOG_ERROR("get buffer size failed");
            av_frame_free(&frame);
            return false;
        }

        auto fb = std::make_shared<FFmpegFrameBuffer>(frame, static_cast<size_t>(size));

        // 上面已经将frame的所有权转移给了fb,这里需要重新分配新 frame
        frame = av_frame_alloc();
        if (!frame) {
            LOG_ERROR("av_frame_alloc OOM after decode");
            av_frame_free(&frame);
            return false;
        }

        // ── 填充 MediaFrame ──
        auto mf = std::make_shared<MediaFrame>();
        const AVFrame* decoded_frame = fb->GetFrame();
        const AVRational frame_time_base = FrameTimeBase(decoded_frame, stream_info_);
        mf->time.pts_us      = TimestampToUs(FramePts(decoded_frame), frame_time_base);
        mf->time.dts_us      = TimestampToUs(decoded_frame->pkt_dts, frame_time_base);
        mf->time.duration_us = TimestampToUs(decoded_frame->duration, frame_time_base);
        mf->buffer        = fb;
        mf->backend.type  = BackendHandle::FFMPEG;
        mf->backend.ptr   = fb->GetFrame();

        if (stream_info_.media_type == MediaType::VIDEO) {
            mf->type = MediaType::VIDEO;
            VideoFrameMeta video_meta;
            const auto av_pix_fmt = static_cast<AVPixelFormat>(fb->GetFrame()->format);
            video_meta.pixel_format = MapAVPixelFormat(av_pix_fmt);
            video_meta.width        = fb->GetFrame()->width;
            video_meta.height       = fb->GetFrame()->height;
            video_meta.plane_count  = fb->GetFrame()->format >= 0 ?
                av_pix_fmt_count_planes(av_pix_fmt) : 0;

            int packed_linesizes[4] = {};
            ptrdiff_t packed_linesizes_ptrdiff[4] = {};
            size_t packed_plane_sizes[4] = {};
            if (video_meta.plane_count > 0 &&
                av_image_fill_linesizes(packed_linesizes, av_pix_fmt, video_meta.width) >= 0) {
                for (int i = 0; i < 4; ++i) {
                    packed_linesizes_ptrdiff[i] = packed_linesizes[i];
                }
                (void)av_image_fill_plane_sizes(
                    packed_plane_sizes,
                    av_pix_fmt,
                    video_meta.height,
                    packed_linesizes_ptrdiff);
            }

            size_t plane_offset = 0;
            for (int i = 0; i < video_meta.plane_count && i < 8; ++i) {
                const size_t plane_size = i < 4 ? packed_plane_sizes[i] : 0;
                video_meta.plane_info[i].offset = static_cast<int32_t>(plane_offset);
                video_meta.plane_info[i].stride = i < 4 ? packed_linesizes[i] : 0;
                video_meta.plane_info[i].size   = static_cast<int32_t>(plane_size);
                plane_offset += plane_size;
            }
            mf->meta = video_meta;
        } else if (stream_info_.media_type == MediaType::AUDIO) {
            mf->type = MediaType::AUDIO;
            AudioFrameMeta audio_meta;
            const auto av_sample_fmt = static_cast<AVSampleFormat>(fb->GetFrame()->format);
            audio_meta.sample_format = MapAVSampleFormat(av_sample_fmt);
            audio_meta.sample_rate   = fb->GetFrame()->sample_rate;
            audio_meta.channels      = fb->GetFrame()->ch_layout.nb_channels;
            audio_meta.channel_layout = fb->GetFrame()->ch_layout.u.mask;
            audio_meta.nb_samples    = fb->GetFrame()->nb_samples;
            audio_meta.bytes_per_sample = av_get_bytes_per_sample(av_sample_fmt);
            audio_meta.planar        = av_sample_fmt_is_planar(av_sample_fmt) != 0;
            audio_meta.plane_count   = audio_meta.planar ? fb->GetFrame()->ch_layout.nb_channels : 1;

            const size_t samples = audio_meta.nb_samples > 0
                ? static_cast<size_t>(audio_meta.nb_samples)
                : size_t{0};
            const size_t bytes_per_sample = audio_meta.bytes_per_sample > 0
                ? static_cast<size_t>(audio_meta.bytes_per_sample)
                : size_t{0};
            const size_t channels = audio_meta.channels > 0
                ? static_cast<size_t>(audio_meta.channels)
                : size_t{0};
            const size_t planar_plane_size = samples * bytes_per_sample;
            const size_t packed_plane_size = planar_plane_size * channels;

            for (int i = 0; i < audio_meta.plane_count && i < 8; ++i) {
                const size_t plane_size = audio_meta.planar ? planar_plane_size : packed_plane_size;
                audio_meta.planes[i].offset = static_cast<int32_t>(
                    audio_meta.planar ? static_cast<size_t>(i) * planar_plane_size : 0);
                audio_meta.planes[i].stride = static_cast<int32_t>(plane_size);
                audio_meta.planes[i].size   = static_cast<int32_t>(plane_size);
            }
            mf->meta = audio_meta;
        }

        // ── 回调通知 ──
        FrameCallback cb;
        {
            std::lock_guard<std::mutex> lock(cb_mutex_);
            cb = frame_cb_;
        }
        if (cb) {
            cb(std::move(mf));
        }
    }

    av_frame_free(&frame);
    return true;
}

// ── 工具 ──────────────────────────────────────────────────────────

CodecType FFmpegDecoder::MapAVCodecID(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_H264: return CodecType::H264;
        case AV_CODEC_ID_HEVC: return CodecType::H265;
        case AV_CODEC_ID_AAC:  return CodecType::AAC;
        case AV_CODEC_ID_OPUS: return CodecType::OPUS;
        default:               return CodecType::UNKNOWN;
    }
}

PixelFormat FFmpegDecoder::MapAVPixelFormat(AVPixelFormat fmt) {
    switch (fmt) {
        case AV_PIX_FMT_NV12:    return PixelFormat::kNV12;
        case AV_PIX_FMT_NV21:    return PixelFormat::kNV21;
        case AV_PIX_FMT_YUV420P: return PixelFormat::kI420;
        case AV_PIX_FMT_BGR24:   return PixelFormat::kBGR24;
        case AV_PIX_FMT_RGB24:   return PixelFormat::kRGB24;
        case AV_PIX_FMT_GRAY8:   return PixelFormat::kGRAY8;
        default:                 return PixelFormat::kUnknown;
    }
}

SampleFormat FFmpegDecoder::MapAVSampleFormat(AVSampleFormat fmt) {
    switch (fmt) {
        case AV_SAMPLE_FMT_U8:   return SampleFormat::U8;
        case AV_SAMPLE_FMT_S16:  return SampleFormat::S16;
        case AV_SAMPLE_FMT_S32:  return SampleFormat::S32;
        case AV_SAMPLE_FMT_FLT:  return SampleFormat::FLT;
        case AV_SAMPLE_FMT_DBL:  return SampleFormat::DBL;
        case AV_SAMPLE_FMT_U8P:  return SampleFormat::U8P;
        case AV_SAMPLE_FMT_S16P: return SampleFormat::S16P;
        case AV_SAMPLE_FMT_S32P: return SampleFormat::S32P;
        case AV_SAMPLE_FMT_FLTP: return SampleFormat::FLTP;
        case AV_SAMPLE_FMT_DBLP: return SampleFormat::DBLP;
        default:                 return SampleFormat::Unknown;
    }
}
