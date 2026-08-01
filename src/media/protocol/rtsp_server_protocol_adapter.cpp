#include "media/protocol/rtsp_server_protocol_adapter.h"

#include "common/log/logger.h"
#include "media/protocol/rtsp_server_protocol.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>



namespace {

int64_t TimestampToUs(int64_t timestamp, const Rational& time_base) {
    const int64_t num = time_base.num > 0 ? time_base.num : 1;
    const int64_t den = time_base.den > 0 ? time_base.den : 1000000;
    return timestamp * num * 1000000 / den;
}

bool IsH264Keyframe(const std::vector<NalUnit>& nals) {
    return std::any_of(nals.begin(), nals.end(), [](const NalUnit& nal) {
        return nal.H264Type() == 5;
    });
}

bool HasH264NalType(const std::vector<NalUnit>& nals, std::uint8_t nal_type) {
    return std::any_of(nals.begin(), nals.end(), [nal_type](const NalUnit& nal) {
        return nal.H264Type() == nal_type;
    });
}

void PrependParameterSetsToKeyframe(std::vector<NalUnit>& nals,
                                    const H264ParameterSets& parameter_sets) {
    if (!IsH264Keyframe(nals)) {
        return;
    }

    std::vector<NalUnit> prefix;
    if (!parameter_sets.sps.empty() && !HasH264NalType(nals, 7)) {
        prefix.push_back(NalUnit{parameter_sets.sps});
    }
    if (!parameter_sets.pps.empty() && !HasH264NalType(nals, 8)) {
        prefix.push_back(NalUnit{parameter_sets.pps});
    }
    if (prefix.empty()) {
        return;
    }

    prefix.insert(prefix.end(),
                  std::make_move_iterator(nals.begin()),
                  std::make_move_iterator(nals.end()));
    nals = std::move(prefix);
}

bool IsSupportedRtspTrack(const MediaTrackConfig& track) {
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

RtspServerProtocolAdapter::RtspServerProtocolAdapter(std::unique_ptr<IProtocol> protocol)
    : protocol_(std::move(protocol)) {
}

RtspServerProtocolAdapter::~RtspServerProtocolAdapter() {
    Close();
}

PublisherResult RtspServerProtocolAdapter::Open(const PublisherConfig& config) {
    Close();

    config_ = config;
    for (auto& track : config_.tracks) {
        if (track.media_type == MediaType::AUDIO && track.sample_rate > 0) {
            // 音频 RTP 时钟跟随采样率；PublisherConfig 默认 90000 只适合视频。
            track.rtp_clock_rate = static_cast<std::uint32_t>(track.sample_rate);
        }
    }
    if (config_.tracks.empty() ||
        !std::all_of(config_.tracks.begin(),
                     config_.tracks.end(),
                     IsSupportedRtspTrack)) {
        LOG_ERROR("RtspServerProtocolAdapter: supports H264 video and AAC/G711 audio tracks");
        return PublisherResult::Failure(
            PublisherErrorCode::UnsupportedCodec,
            "RTSP server adapter supports H264 video and AAC/G711 audio tracks");
    }

    avcc_length_size_by_track_.clear();
    timestamp_origin_us_by_track_.clear();
    h264_parameter_sets_by_track_.clear();
    for (const auto& track : config_.tracks) {
        if (track.codec_type == CodecType::H264) {
            avcc_length_size_by_track_[track.track_id] =
                H264Bitstream::ParseAvccLengthSize(track.extra_data);
            h264_parameter_sets_by_track_[track.track_id] =
                H264Bitstream::ExtractParameterSets(track.extra_data);
        }
    }

    if (!protocol_) {
        protocol_ = std::make_unique<RtspServerProtocol>();
    }

    auto result = protocol_->Start(config_, config_.tracks);
    opened_ = result.IsSuccess();
    return result;
}

PublisherResult RtspServerProtocolAdapter::Send(const MediaPacket& packet) {
    if (!opened_ || !protocol_) {
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidState,
            "RTSP server protocol adapter is not open");
    }

    const auto* track = FindTrackForPacket(packet);
    if (!track) {
        LOG_ERROR("RtspServerProtocolAdapter: unsupported codec {}",
                  static_cast<int>(packet.codec));
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidMediaPacket,
            "media packet does not match any configured RTSP track");
    }
    if (!packet.buffer || !packet.buffer->Data() || packet.buffer->Size() == 0) {
        LOG_ERROR("RtspServerProtocolAdapter: empty packet buffer");
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidMediaPacket,
            "media packet buffer is empty");
    }

    auto access_unit = ToAccessUnit(packet);
    if (access_unit.codec_type == CodecType::H264 && access_unit.nals.empty()) {
        LOG_ERROR("RtspServerProtocolAdapter: failed to split H264 packet");
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidMediaPacket,
            "failed to split H264 media packet");
    }

    return protocol_->Write(access_unit);
}

void RtspServerProtocolAdapter::Close() {
    if (protocol_) {
        protocol_->Stop();
    }
    opened_ = false;
}

std::string RtspServerProtocolAdapter::GetOutputUrl() const {
    return protocol_ ? protocol_->GetOutputUrl() : std::string{};
}

PublisherStats RtspServerProtocolAdapter::GetStats() const {
    return protocol_ ? protocol_->GetStats() : PublisherStats{};
}

const MediaTrackConfig* RtspServerProtocolAdapter::FindTrackForPacket(
    const MediaPacket& packet) const {
    if (config_.tracks.empty()) {
        return nullptr;
    }

    if (config_.tracks.size() == 1) {
        const auto& track = config_.tracks.front();
        if ((packet.type == MediaType::UNKNOWN || packet.type == track.media_type) &&
            (packet.codec == CodecType::UNKNOWN || packet.codec == track.codec_type)) {
            return &track;
        }
        return nullptr;
    }

    auto by_track_id = std::find_if(
        config_.tracks.begin(),
        config_.tracks.end(),
        [&packet](const auto& track) {
            return packet.stream_index >= 0 && track.track_id == packet.stream_index;
        });
    if (by_track_id != config_.tracks.end()) {
        return &*by_track_id;
    }

    auto by_media_codec = std::find_if(
        config_.tracks.begin(),
        config_.tracks.end(),
        [&packet](const auto& track) {
            return (packet.type == MediaType::UNKNOWN ||
                    packet.type == track.media_type) &&
                   (packet.codec == CodecType::UNKNOWN ||
                    packet.codec == track.codec_type);
        });
    return by_media_codec == config_.tracks.end() ? nullptr : &*by_media_codec;
}

EncodedAccessUnit RtspServerProtocolAdapter::ToAccessUnit(const MediaPacket& packet) {
    const auto* track = FindTrackForPacket(packet);
    EncodedAccessUnit access_unit;
    if (!track) {
        return access_unit;
    }

    access_unit.track_id = track->track_id;
    access_unit.media_type = track->media_type;
    access_unit.codec_type = track->codec_type;
    access_unit.pts = ToRtpTimestamp(packet, *track);
    access_unit.dts = access_unit.pts;
    access_unit.duration = 0;
    access_unit.time_base = Rational{1, static_cast<int>(track->rtp_clock_rate)};
    access_unit.encoded_data = packet.buffer;
    access_unit.backend = packet.backend;
    if (track->codec_type == CodecType::H264) {
        const auto avcc_it = avcc_length_size_by_track_.find(track->track_id);
        const auto avcc_length_size = avcc_it == avcc_length_size_by_track_.end()
            ? 4
            : avcc_it->second;
        access_unit.nals = H264Bitstream::SplitPacket(
            packet.buffer->Data(),
            packet.buffer->Size(),
            avcc_length_size);
        auto parameter_sets_it = h264_parameter_sets_by_track_.find(track->track_id);
        if (parameter_sets_it != h264_parameter_sets_by_track_.end()) {
            PrependParameterSetsToKeyframe(access_unit.nals, parameter_sets_it->second);
        }
        access_unit.keyframe = packet.keyframe || IsH264Keyframe(access_unit.nals);
    } else {
        access_unit.keyframe = packet.keyframe;
    }
    return access_unit;
}

std::uint32_t RtspServerProtocolAdapter::ToRtpTimestamp(
    const MediaPacket& packet,
    const MediaTrackConfig& track) {
    const auto pts_us = TimestampToUs(packet.pts, packet.time_base);
    auto [it, inserted] = timestamp_origin_us_by_track_.emplace(track.track_id, pts_us);
    if (inserted) {
        it->second = pts_us;
    }

    const auto delta_us = pts_us - it->second;
    if (delta_us <= 0) {
        return 0;
    }

    const auto clock_rate = track.rtp_clock_rate == 0 ? 90000 : track.rtp_clock_rate;
    return static_cast<std::uint32_t>(delta_us * clock_rate / 1000000);
}
