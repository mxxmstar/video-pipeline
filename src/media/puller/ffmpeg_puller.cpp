#include "media/puller/ffmpeg_puller.h"
#include "media/stream/stream_info.h"

#include "common/log/logger.h"
#include "media/ffmpeg_packet_buffer.h"

#include <algorithm>
#include <cctype>
#include <cerrno>

// Windows 兼容: ETIMEDOUT 在 Windows 上可能未定义
#ifndef ETIMEDOUT
  #ifdef WSAETIMEDOUT
    #define ETIMEDOUT WSAETIMEDOUT
  #else
    #define ETIMEDOUT 10060  // Windows 超时错误码
  #endif
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
}

namespace {

AVRational ToAVRational(const Rational& rational) {
    return AVRational{
        rational.num > 0 ? rational.num : 1,
        rational.den > 0 ? rational.den : AV_TIME_BASE,
    };
}

int64_t TimestampToUs(int64_t timestamp, const Rational& time_base) {
    if (timestamp == AV_NOPTS_VALUE) {
        return 0;
    }

    constexpr AVRational kMicrosecondTimeBase{1, AV_TIME_BASE};
    return av_rescale_q(timestamp, ToAVRational(time_base), kMicrosecondTimeBase);
}

} // namespace

// ── ctor / dtor ────────────────────────────────────────────────────

FFmpegPuller::FFmpegPuller() {
}

FFmpegPuller::~FFmpegPuller() {
    Close();
}

std::string FFmpegPuller::BuildRtspTransportOption() const {
    std::string transport = rtsp_transport_.empty() ? "udp" : rtsp_transport_;
    std::transform(transport.begin(), transport.end(), transport.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    if (!rtsp_auto_switch_tcp_)
        return transport;

    if (transport.find("tcp") != std::string::npos)
        return transport;

    if (transport == "udp")
        return "udp+tcp";

    return transport + "+tcp";
}

// ── IPuller ─────────────────────────────────────────────────────────

bool FFmpegPuller::Open(const std::string& url) {
    // 1. 分配 FFmpeg 格式上下文
    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) {
        LOG_ERROR("avformat_alloc_context failed");
        return false;
    }

    const int rtsp_io_timeout_ms = rtsp_auto_switch_tcp_
        ? rtsp_auto_switch_timeout_ms_
        : read_timeout_ms_;
    const int open_timeout_ms = std::max(connect_timeout_ms_, rtsp_io_timeout_ms);

    // 2. 设置中断回调（超时控制）
    interrupt_ctx_.interrupted = false;
    interrupt_ctx_.timed_out = false;
    interrupt_ctx_.start_time  = std::chrono::steady_clock::now();
    interrupt_ctx_.timeout_ms  = open_timeout_ms;

    fmt_ctx_->interrupt_callback.callback = [](void* ctx) -> int {
        auto* ic = static_cast<InterruptContext*>(ctx);
        if (ic->interrupted.load())
            return 1;
        auto now   = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - ic->start_time).count();
        if (ic->timeout_ms > 0 && elapsed > ic->timeout_ms) {
            ic->timed_out = true;
            LOG_WARN("FFmpeg IO timeout after {} ms", elapsed);
            return 1;
        }
        return 0;
    };
    fmt_ctx_->interrupt_callback.opaque = &interrupt_ctx_;

    // 3. 设置传输选项
    AVDictionary* opts = nullptr;
    const std::string rtsp_transport = BuildRtspTransportOption();
    av_dict_set(&opts, "rtsp_transport", rtsp_transport.c_str(), 0);
    av_dict_set_int(&opts, "stimeout", static_cast<int64_t>(rtsp_io_timeout_ms) * 1000, 0);
    av_dict_set_int(&opts, "timeout",  static_cast<int64_t>(rtsp_io_timeout_ms) * 1000, 0);
    if (low_latency_) {
        av_dict_set(&opts, "fflags", "nobuffer", 0);
        av_dict_set(&opts, "flags",  "low_delay", 0);
    }
    if (!username_.empty())
        av_dict_set(&opts, "user",     username_.c_str(), 0);
    if (!password_.empty())
        av_dict_set(&opts, "password", password_.c_str(), 0);

    // 4. 打开输入
    int ret = avformat_open_input(&fmt_ctx_, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char buf[AV_ERROR_MAX_STRING_SIZE];
        av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, ret);
        LOG_ERROR("avformat_open_input failed: {}", buf);
        Close();
        return false;
    }

    // 5. 查找流信息
    ret = avformat_find_stream_info(fmt_ctx_, nullptr);
    if (ret < 0) {
        char buf[AV_ERROR_MAX_STRING_SIZE];
        av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, ret);
        LOG_ERROR("avformat_find_stream_info failed: {}", buf);
        Close();
        return false;
    }

    // 6. 遍历所有流，缓存编码参数和流信息
    for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
        AVStream* stream = fmt_ctx_->streams[i];
        AVCodecParameters* codecpar = stream->codecpar;
        
        if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO ||
            codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            
            // 缓存编码参数
            codecpars_[static_cast<int>(i)] = codecpar;
            
            // 构造 MediaStreamInfo
            MediaStreamInfo info;
            info.stream_index = static_cast<int>(i);
            info.codec_type = MapCodecID(codecpar->codec_id);
            info.time_base.num = stream->time_base.num;
            info.time_base.den = stream->time_base.den;

            if (codecpar->extradata && codecpar->extradata_size > 0) {
                info.extra_data.assign(
                    codecpar->extradata,
                    codecpar->extradata + codecpar->extradata_size);
            }
            
            const int stream_info_idx = static_cast<int>(cached_info_.stream_infos.size());
            if (codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                info.media_type = MediaType::VIDEO;
                VideoStreamInfo video_info;
                video_info.width = codecpar->width;
                video_info.height = codecpar->height;
                // 计算 FPS
                if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
                    video_info.fps = static_cast<float>(stream->avg_frame_rate.num) / 
                                     static_cast<float>(stream->avg_frame_rate.den);
                } else if (stream->r_frame_rate.num > 0 && stream->r_frame_rate.den > 0) {
                    video_info.fps = static_cast<float>(stream->r_frame_rate.num) / 
                                     static_cast<float>(stream->r_frame_rate.den);
                }
                info.detail = video_info;
                cached_info_.video_stream_idx_ = stream_info_idx;
            } else if (codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                info.media_type = MediaType::AUDIO;
                AudioStreamInfo audio_info;
                audio_info.sample_rate = codecpar->sample_rate;
                audio_info.channels = codecpar->ch_layout.nb_channels;
                audio_info.channel_layout = codecpar->ch_layout.u.mask;
                info.detail = audio_info;
                cached_info_.audio_stream_idx_ = stream_info_idx;
            }
            
            cached_info_.stream_infos.push_back(info);
        }
    }
    
    // 至少需要一个视频流或音频流
    if (!cached_info_.HasVideoStream() && !cached_info_.HasAudioStream()) {
        LOG_ERROR("no video or audio stream found in {}", url);
        Close();
        return false;
    }
    
    // 7. 打印流信息
    LOG_INFO("Stream count: video={}, audio={}", 
             cached_info_.HasVideoStream() ? 1 : 0,
             cached_info_.HasAudioStream() ? 1 : 0);
    cached_info_.DumpStream();

    return true;
}

void FFmpegPuller::Close() {
    interrupt_ctx_.interrupted = true;
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (fmt_ctx_) {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
    }
    codecpars_.clear();
    cached_info_ = {};
    
    // 清理对象池
    std::lock_guard<std::mutex> pool_lock(pool_mutex_);
    for (auto* pkt : packet_pool_) {
        av_packet_free(&pkt);
    }
    packet_pool_.clear();
}

bool FFmpegPuller::ReadPacket(std::shared_ptr<MediaPacket>& packet) {
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (fmt_ctx_ == nullptr) {
        LOG_ERROR("fmt_ctx_ is nullptr");
        return false;
    }

    // 从对象池分配一个临时packet
    AVPacket* pkt = nullptr;
    {
        std::lock_guard<std::mutex> pool_lock(pool_mutex_);
        if (!packet_pool_.empty()) {
            pkt = packet_pool_.back();
            packet_pool_.pop_back();
        }
    }
    if (!pkt) {
        pkt = av_packet_alloc();
    } else {
        av_packet_unref(pkt);  // 清理旧数据
    }
    if (!pkt) {
        LOG_ERROR("packet pool allocate failed");
        return false;
    }

    // 2. 读取一帧
    interrupt_ctx_.timed_out = false;
    interrupt_ctx_.start_time = std::chrono::steady_clock::now();
    interrupt_ctx_.timeout_ms = read_timeout_ms_;
    int ret = av_read_frame(fmt_ctx_, pkt);
    if (ret < 0) {
        av_packet_unref(pkt);
        // 回收packet到对象池
        {
            std::lock_guard<std::mutex> pool_lock(pool_mutex_);
            packet_pool_.push_back(pkt);
        }
        if (ret == AVERROR_EOF) {
            LOG_DEBUG("av_read_frame EOF");
            return false;
        }
        if (interrupt_ctx_.interrupted.load()) {
            LOG_DEBUG("av_read_frame interrupted");
            return false;
        }
        bool transient_error = ret == AVERROR(EAGAIN) || interrupt_ctx_.timed_out.load();
#ifdef ETIMEDOUT
        transient_error = transient_error || ret == AVERROR(ETIMEDOUT);
#endif
        if (transient_error) {
            packet = nullptr;
            LOG_DEBUG("av_read_frame timeout/transient error, keep reading");
            return true;
        }
        char buf[AV_ERROR_MAX_STRING_SIZE];
        av_make_error_string(buf, AV_ERROR_MAX_STRING_SIZE, ret);
        LOG_ERROR("av_read_frame error: {}", buf);
        return false;
    }

    // 3. 只处理选中的视频流或音频流
    int stream_idx = pkt->stream_index;
    if (codecpars_.find(stream_idx) == codecpars_.end()) {
        av_packet_unref(pkt);
        // 回收packet到对象池
        {
            std::lock_guard<std::mutex> pool_lock(pool_mutex_);
            packet_pool_.push_back(pkt);
        }
        packet = nullptr;
        return true;
    }
    
    AVCodecParameters* stream_codecpar = codecpars_[stream_idx];

    // 4. 视频包：从池转移到 av_packet_alloc 分配的包，使 FFmpegPacketBuffer
    //    析构时 av_packet_free 能正确释放 AVPacket 结构体内存。
    AVPacket* owned = av_packet_alloc();
    if (!owned) {
        av_packet_unref(pkt);
        // 回收packet到对象池
        {
            std::lock_guard<std::mutex> pool_lock(pool_mutex_);
            packet_pool_.push_back(pkt);
        }
        LOG_ERROR("av_packet_alloc failed");
        return false;
    }
    av_packet_move_ref(owned, pkt);  // 转移 buffer/side-data，pkt 变空
    av_packet_unref(pkt);
    // 回收空packet到对象池
    {
        std::lock_guard<std::mutex> pool_lock(pool_mutex_);
        packet_pool_.push_back(pkt);
    }

    bool is_keyframe = (owned->flags & AV_PKT_FLAG_KEY) != 0;

    // 5. 构造 MediaPacket（零拷贝）
    auto mp = std::make_shared<MediaPacket>();
    mp->type     = (stream_codecpar->codec_type == AVMEDIA_TYPE_VIDEO) ? MediaType::VIDEO : MediaType::AUDIO;
    mp->codec    = MapCodecID(stream_codecpar->codec_id);
    mp->stream_index = stream_idx;
    const AVStream* stream = fmt_ctx_->streams[stream_idx];
    Rational packet_time_base{
        stream->time_base.num,
        stream->time_base.den,
    };
    mp->pts      = TimestampToUs(owned->pts, packet_time_base);
    mp->dts      = TimestampToUs(owned->dts, packet_time_base);
    mp->duration = TimestampToUs(owned->duration, packet_time_base);
    mp->time_base = Rational{1, AV_TIME_BASE};
    mp->keyframe = is_keyframe;

    mp->buffer = std::make_shared<FFmpegPacketBuffer>(owned);

    mp->backend.type = BackendHandle::FFMPEG;
    mp->backend.ptr  = std::static_pointer_cast<FFmpegPacketBuffer>(mp->buffer)->GetPacket();

    packet = std::move(mp);
    return true;
}

MultiStreamInfo FFmpegPuller::GetStreamInfo() const {
    return cached_info_;
}

void FFmpegPuller::SetEventCallback(EventCallback cb) {
    event_cb_ = std::move(cb);
}

// ── 扩展配置 ────────────────────────────────────────────────────────

void FFmpegPuller::SetConnectTimeoutMs(int ms) {
    connect_timeout_ms_ = ms > 0 ? ms : 0;
}

void FFmpegPuller::SetReadTimeoutMs(int ms) {
    read_timeout_ms_ = ms > 0 ? ms : 0;
}

void FFmpegPuller::SetLowLatency(bool enable) {
    low_latency_ = enable;
}

void FFmpegPuller::SetCredentials(const std::string& username, const std::string& password) {
    username_ = username;
    password_ = password;
}

void FFmpegPuller::SetRtspTransport(const std::string& transport) {
    rtsp_transport_ = transport.empty() ? "udp" : transport;
}

void FFmpegPuller::SetRtspAutoSwitchToTcp(bool enable) {
    rtsp_auto_switch_tcp_ = enable;
}

void FFmpegPuller::SetRtspAutoSwitchTimeoutMs(int ms) {
    rtsp_auto_switch_timeout_ms_ = ms > 0 ? ms : 0;
}

// ── MapCodecID ──────────────────────────────────────────────────────

CodecType FFmpegPuller::MapCodecID(AVCodecID id) {
    switch (id) {
        case AV_CODEC_ID_H264: return CodecType::H264;
        case AV_CODEC_ID_HEVC: return CodecType::H265;
        case AV_CODEC_ID_AAC:  return CodecType::AAC;
        case AV_CODEC_ID_OPUS: return CodecType::OPUS;
        case AV_CODEC_ID_PCM_ALAW:    return CodecType::G711A;
        case AV_CODEC_ID_PCM_MULAW:   return CodecType::G711U;
        case AV_CODEC_ID_ADPCM_G726:  return CodecType::G726;
        case AV_CODEC_ID_MJPEG:       return CodecType::JPEG;
        default:
            LOG_WARN("Unknown codec ID: {}", static_cast<int>(id));
            return CodecType::UNKNOWN;
    }
}
