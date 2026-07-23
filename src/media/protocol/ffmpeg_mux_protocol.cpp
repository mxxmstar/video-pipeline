#include "media/protocol/ffmpeg_mux_protocol.h"

#include "common/log/logger.h"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}



namespace {

std::string ErrorString(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_make_error_string(buf, sizeof(buf), err);
    return buf;
}

bool StartsWithRtsp(const std::string& url) {
    return url.rfind("rtsp://", 0) == 0 || url.rfind("rtsps://", 0) == 0;
}

} // namespace

FfmpegMuxProtocol::~FfmpegMuxProtocol() {
    Stop();
}

bool FfmpegMuxProtocol::Start(const PublisherConfig& config,
                              const std::vector<MediaTrackConfig>& tracks) {
    Stop();

    if (config.url.empty() || tracks.empty()) {
        LOG_ERROR("FfmpegMuxProtocol: invalid config");
        return false;
    }

    config_ = config;
    tracks_ = tracks;
    stats_ = {};

    const char* format_name = config_.ffmpeg.format_name.empty()
        ? (StartsWithRtsp(config_.url) ? "rtsp" : nullptr)
        : config_.ffmpeg.format_name.c_str();

    int ret = avformat_alloc_output_context2(
        &fmt_ctx_, nullptr, format_name, config_.url.c_str());
    if (ret < 0 || !fmt_ctx_) {
        LOG_ERROR("FfmpegMuxProtocol: alloc output context failed: {}",
                  ErrorString(ret));
        FreeContext(false);
        return false;
    }

    if (!BuildStreams(tracks_)) {
        FreeContext(false);
        return false;
    }

    AVDictionary* opts = nullptr;
    if (StartsWithRtsp(config_.url) && !config_.ffmpeg.rtsp_transport.empty()) {
        av_dict_set(&opts, "rtsp_transport", config_.ffmpeg.rtsp_transport.c_str(), 0);
    }

    if (!(fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&fmt_ctx_->pb, config_.url.c_str(),
                         AVIO_FLAG_WRITE, nullptr, &opts);
        if (ret < 0) {
            LOG_ERROR("FfmpegMuxProtocol: open output url failed: {}",
                      ErrorString(ret));
            av_dict_free(&opts);
            FreeContext(false);
            return false;
        }
    }

    ret = avformat_write_header(fmt_ctx_, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        LOG_ERROR("FfmpegMuxProtocol: write header failed: {}", ErrorString(ret));
        FreeContext(false);
        return false;
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
    return true;
}

bool FfmpegMuxProtocol::Write(const EncodedAccessUnit& access_unit) {
    if (!fmt_ctx_ || !header_written_) {
        LOG_ERROR("FfmpegMuxProtocol: Write called before Start");
        return false;
    }

    AVPacket* out = av_packet_alloc();
    if (!out) {
        LOG_ERROR("FfmpegMuxProtocol: av_packet_alloc failed");
        return false;
    }

    if (!BuildPacket(access_unit, out)) {
        av_packet_free(&out);
        return false;
    }

    const int ret = av_write_frame(fmt_ctx_, out);
    const auto packet_size = out->size > 0 ? static_cast<std::uint64_t>(out->size) : 0;
    av_packet_free(&out);
    if (ret < 0) {
        LOG_ERROR("FfmpegMuxProtocol: write packet failed: {}", ErrorString(ret));
        return false;
    }

    ++stats_.packets_published;
    stats_.bytes_published += packet_size;
    return true;
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

bool FfmpegMuxProtocol::BuildStreams(const std::vector<MediaTrackConfig>& tracks) {
    track_to_stream_index_.clear();

    for (const auto& track : tracks) {
        AVStream* stream = avformat_new_stream(fmt_ctx_, nullptr);
        if (!stream) {
            LOG_ERROR("FfmpegMuxProtocol: avformat_new_stream failed");
            return false;
        }

        track_to_stream_index_[track.track_id] = stream->index;
        stream->time_base = AVRational{
            track.time_base_num > 0 ? track.time_base_num : 1,
            track.time_base_den > 0 ? track.time_base_den : 1000000,
        };

        AVCodecParameters* par = stream->codecpar;
        par->codec_id = static_cast<AVCodecID>(MapCodecType(track.codec_type));
        par->codec_tag = 0;

        if (par->codec_id == AV_CODEC_ID_NONE) {
            LOG_ERROR("FfmpegMuxProtocol: unsupported codec {}",
                      static_cast<int>(track.codec_type));
            return false;
        }

        if (track.media_type == MediaType::VIDEO) {
            par->codec_type = AVMEDIA_TYPE_VIDEO;
            par->width = track.width;
            par->height = track.height;
        } else if (track.media_type == MediaType::AUDIO) {
            par->codec_type = AVMEDIA_TYPE_AUDIO;
            par->sample_rate = track.sample_rate;
#if LIBAVUTIL_VERSION_MAJOR >= 57
            av_channel_layout_uninit(&par->ch_layout);
            av_channel_layout_default(&par->ch_layout, track.channels);
#else
            par->channels = track.channels;
            par->channel_layout = av_get_default_channel_layout(track.channels);
#endif
        } else {
            LOG_ERROR("FfmpegMuxProtocol: unsupported media type {}",
                      static_cast<int>(track.media_type));
            return false;
        }

        if (!track.extra_data.empty()) {
            const auto size = track.extra_data.size();
            par->extradata = static_cast<std::uint8_t*>(
                av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
            if (!par->extradata) {
                LOG_ERROR("FfmpegMuxProtocol: allocate extradata failed");
                return false;
            }
            std::memcpy(par->extradata, track.extra_data.data(), size);
            par->extradata_size = static_cast<int>(size);
        }
    }

    return true;
}

bool FfmpegMuxProtocol::BuildPacket(const EncodedAccessUnit& access_unit,
                                    AVPacket* out) const {
    if (!out) {
        return false;
    }

    const auto stream_it = track_to_stream_index_.find(access_unit.track_id);
    if (stream_it == track_to_stream_index_.end()) {
        LOG_ERROR("FfmpegMuxProtocol: unknown track id {}", access_unit.track_id);
        return false;
    }

    av_packet_unref(out);

    int ret = 0;
    if (access_unit.backend.type == BackendHandle::FFMPEG && access_unit.backend.ptr) {
        auto* src = static_cast<AVPacket*>(access_unit.backend.ptr);
        ret = av_packet_ref(out, src);
        if (ret < 0) {
            LOG_ERROR("FfmpegMuxProtocol: packet ref failed: {}", ErrorString(ret));
            return false;
        }
    } else {
        if (!access_unit.encoded_data || !access_unit.encoded_data->Data() ||
            access_unit.encoded_data->Size() == 0) {
            LOG_ERROR("FfmpegMuxProtocol: access unit buffer is empty");
            return false;
        }

        const auto size = static_cast<int>(access_unit.encoded_data->Size());
        ret = av_new_packet(out, size);
        if (ret < 0) {
            LOG_ERROR("FfmpegMuxProtocol: packet allocation failed: {}",
                      ErrorString(ret));
            return false;
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

    const auto* stream = fmt_ctx_->streams[out->stream_index];
    const AVRational source_time_base{
        access_unit.time_base.num > 0 ? access_unit.time_base.num : 1,
        access_unit.time_base.den > 0 ? access_unit.time_base.den : 1000000,
    };
    av_packet_rescale_ts(out, source_time_base, stream->time_base);

    if (access_unit.keyframe) {
        out->flags |= AV_PKT_FLAG_KEY;
    } else {
        out->flags &= ~AV_PKT_FLAG_KEY;
    }

    return true;
}

void FfmpegMuxProtocol::FreeContext(bool write_trailer) {
    if (fmt_ctx_ && write_trailer && header_written_) {
        const int ret = av_write_trailer(fmt_ctx_);
        if (ret < 0) {
            LOG_WARN("FfmpegMuxProtocol: write trailer failed: {}", ErrorString(ret));
        }
    }

    if (fmt_ctx_) {
        if (fmt_ctx_->pb && !(fmt_ctx_->oformat->flags & AVFMT_NOFILE)) {
            const int ret = avio_closep(&fmt_ctx_->pb);
            if (ret < 0) {
                LOG_WARN("FfmpegMuxProtocol: close output failed: {}", ErrorString(ret));
            }
        }

        avformat_free_context(fmt_ctx_);
        fmt_ctx_ = nullptr;
    }

    tracks_.clear();
    track_to_stream_index_.clear();
    header_written_ = false;
    config_ = {};
}
