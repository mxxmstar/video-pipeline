#include "media/publisher/publisher_config.h"

#include <boost/asio/ip/address.hpp>

#include <cstdint>
#include <unordered_set>
#include <utility>

namespace {

PublisherResult Invalid(std::string message) {
    return PublisherResult::Failure(PublisherErrorCode::InvalidConfiguration,
                                    std::move(message));
}

bool IsVideoCodec(CodecType codec) {
    return codec == CodecType::H264 || codec == CodecType::H265 ||
           codec == CodecType::JPEG;
}

bool IsAudioCodec(CodecType codec) {
    return codec == CodecType::AAC || codec == CodecType::OPUS ||
           codec == CodecType::G711A || codec == CodecType::G711U ||
           codec == CodecType::G726;
}

bool IsFfmpegCodecSupported(CodecType codec) {
    return codec == CodecType::H264 || codec == CodecType::H265 ||
           codec == CodecType::AAC || codec == CodecType::OPUS ||
           codec == CodecType::G711A || codec == CodecType::G711U;
}

bool IsRtspServerTrackSupported(const MediaTrackConfig& track) {
    if (track.media_type == MediaType::VIDEO) {
        return track.codec_type == CodecType::H264;
    }
    if (track.media_type == MediaType::AUDIO) {
        return track.codec_type == CodecType::AAC ||
               track.codec_type == CodecType::G711A ||
               track.codec_type == CodecType::G711U;
    }
    return false;
}

} // namespace

PublisherResult MediaTrackConfig::ValidateStructure() const {
    if (track_id < 0) {
        return Invalid("track_id must be non-negative");
    }
    if (codec_type == CodecType::UNKNOWN) {
        return Invalid("track codec must not be UNKNOWN");
    }
    if (time_base_num <= 0 || time_base_den <= 0) {
        return Invalid("track time base must be positive");
    }
    if (media_type == MediaType::VIDEO) {
        if (!IsVideoCodec(codec_type)) {
            return Invalid("video track uses an audio or unsupported codec");
        }
        if (width <= 0 || height <= 0 || fps <= 0.0f) {
            return Invalid("video width, height and fps must be positive");
        }
        return PublisherResult::Success();
    }

    if (media_type == MediaType::AUDIO) {
        if (!IsAudioCodec(codec_type)) {
            return Invalid("audio track uses a video or unsupported codec");
        }
        if (sample_rate <= 0 || channels <= 0) {
            return Invalid("audio sample rate and channels must be positive");
        }
        return PublisherResult::Success();
    }

    return Invalid("publisher supports only video and audio tracks");
}

PublishProtocol ResolvePublishProtocol(const PublisherConfig& config) {
    if (config.protocol != PublishProtocol::Auto) {
        return config.protocol;
    }
    return config.mode == PublishMode::PullServer
        ? PublishProtocol::RtspServer
        : PublishProtocol::FfmpegMux;
}

PublisherResult PublisherConfig::ValidateStructure() const {
    if (tracks.empty()) {
        return Invalid("publisher requires at least one track");
    }

    std::unordered_set<int> track_ids;
    for (const auto& track : tracks) {
        auto result = track.ValidateStructure();
        if (!result) {
            return result;
        }
        if (!track_ids.insert(track.track_id).second) {
            return Invalid("publisher track_id values must be unique");
        }
    }

    return PublisherResult::Success();
}

PublisherResult PublisherConfig::ValidateProtocol() const {

    const auto resolved_protocol = ResolvePublishProtocol(*this);
    if (resolved_protocol == PublishProtocol::RtpUdp ||
        resolved_protocol == PublishProtocol::WebRtc) {
        return PublisherResult::Failure(
            PublisherErrorCode::UnsupportedProtocol,
            "selected publish protocol is not implemented");
    }

    if (resolved_protocol == PublishProtocol::FfmpegMux) {
        if (mode != PublishMode::PushClient) {
            return Invalid("FfmpegMux requires PublishMode::PushClient");
        }
        if (url.empty()) {
            return Invalid("FfmpegMux requires a non-empty output URL");
        }
        for (const auto& track : tracks) {
            if (!IsFfmpegCodecSupported(track.codec_type)) {
                return PublisherResult::Failure(
                    PublisherErrorCode::UnsupportedCodec,
                    "FfmpegMux does not support one of the configured codecs");
            }
        }
        return PublisherResult::Success();
    }

    if (resolved_protocol == PublishProtocol::RtspServer) {
        if (mode != PublishMode::PullServer) {
            return Invalid("RtspServer requires PublishMode::PullServer");
        }
        if (listen_host.empty() || listen_port == 0) {
            return Invalid("RtspServer requires a listen host and non-zero port");
        }
        boost::system::error_code address_error;
        boost::asio::ip::make_address(listen_host, address_error);
        if (address_error) {
            return Invalid("RtspServer listen host must be an IP address");
        }
        if (stream_path.empty() || stream_path.front() != '/') {
            return Invalid("RtspServer stream path must start with '/'");
        }
        if (!rtsp.enable_tcp_interleaved && !rtsp.enable_udp) {
            return Invalid("RtspServer requires at least one transport");
        }
        if (rtsp.max_payload_size <= 0) {
            return Invalid("RTSP maximum RTP payload size must be positive");
        }
        if (rtsp.enable_multicast) {
            if (!rtsp.enable_udp) {
                return Invalid("RTSP multicast requires UDP to be enabled");
            }
            if (rtsp.multicast_address.empty()) {
                return Invalid("RTSP multicast requires a destination address");
            }
            address_error.clear();
            const auto multicast_address = boost::asio::ip::make_address(
                rtsp.multicast_address,
                address_error);
            if (address_error || !multicast_address.is_v4() ||
                !multicast_address.to_v4().is_multicast()) {
                return Invalid("RTSP multicast destination must be an IPv4 multicast address");
            }
            if (rtsp.multicast_rtp_port == 0 ||
                rtsp.multicast_rtcp_port == 0 ||
                (rtsp.multicast_rtp_port & 1U) != 0 ||
                rtsp.multicast_rtcp_port != rtsp.multicast_rtp_port + 1) {
                return Invalid("RTSP multicast requires an even RTP port followed by RTCP");
            }
        }
        std::unordered_set<std::uint8_t> payload_types;
        for (const auto& track : tracks) {
            if (track.rtp_payload_type > 127) {
                return Invalid("RTSP RTP payload type must be in range 0..127");
            }
            if (track.rtp_clock_rate == 0) {
                return Invalid("RTSP RTP clock rate must be positive");
            }
            if (!payload_types.insert(track.rtp_payload_type).second) {
                return Invalid("RTSP RTP payload types must be unique");
            }
            if (!IsRtspServerTrackSupported(track)) {
                return PublisherResult::Failure(
                    PublisherErrorCode::UnsupportedCodec,
                    "RtspServer supports H264 video and AAC/G711 audio only");
            }
        }
        return PublisherResult::Success();
    }

    return PublisherResult::Failure(PublisherErrorCode::UnsupportedProtocol,
                                    "selected publish protocol is unsupported");
}

PublisherResult PublisherConfig::Validate() const {
    auto result = ValidateStructure();
    return result ? ValidateProtocol() : result;
}
