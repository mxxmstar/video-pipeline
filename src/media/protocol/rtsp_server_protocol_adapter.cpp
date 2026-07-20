#include "media/protocol/rtsp_server_protocol_adapter.h"

#include "common/log/logger.h"
#include "media/protocol/rtsp_server_protocol.h"

#include <algorithm>
#include <cstdint>
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

} // namespace

RtspServerProtocolAdapter::RtspServerProtocolAdapter(std::unique_ptr<IProtocol> protocol)
    : protocol_(std::move(protocol)) {
}

RtspServerProtocolAdapter::~RtspServerProtocolAdapter() {
    Close();
}

bool RtspServerProtocolAdapter::Open(const PublisherConfig& config) {
    Close();

    config_ = config;
    if (config_.tracks.empty() || config_.tracks.front().codec_type != CodecType::H264) {
        LOG_ERROR("RtspServerProtocolAdapter: MVP supports one H264 video track");
        return false;
    }

    avcc_length_size_ = H264Bitstream::ParseAvccLengthSize(
        config_.tracks.front().extra_data);
    have_timestamp_origin_ = false;
    timestamp_origin_us_ = 0;

    if (!protocol_) {
        protocol_ = std::make_unique<RtspServerProtocol>();
    }

    opened_ = protocol_->Start(config_, config_.tracks);
    return opened_;
}

bool RtspServerProtocolAdapter::Send(const MediaPacket& packet) {
    if (!opened_ || !protocol_) {
        return false;
    }

    if (packet.type != MediaType::UNKNOWN && packet.type != MediaType::VIDEO) {
        return true;
    }
    if (packet.codec != CodecType::UNKNOWN && packet.codec != CodecType::H264) {
        LOG_ERROR("RtspServerProtocolAdapter: unsupported codec {}",
                  static_cast<int>(packet.codec));
        return false;
    }
    if (!packet.buffer || !packet.buffer->Data() || packet.buffer->Size() == 0) {
        LOG_ERROR("RtspServerProtocolAdapter: empty packet buffer");
        return false;
    }

    auto access_unit = ToAccessUnit(packet);
    if (access_unit.nals.empty()) {
        LOG_ERROR("RtspServerProtocolAdapter: failed to split H264 packet");
        return false;
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

EncodedAccessUnit RtspServerProtocolAdapter::ToAccessUnit(const MediaPacket& packet) {
    EncodedAccessUnit access_unit;
    access_unit.track_id = 0;
    access_unit.media_type = MediaType::VIDEO;
    access_unit.codec_type = CodecType::H264;
    access_unit.pts = ToRtpTimestamp(packet);
    access_unit.dts = access_unit.pts;
    access_unit.duration = 0;
    access_unit.time_base = Rational{1, 90000};
    access_unit.encoded_data = packet.buffer;
    access_unit.backend = packet.backend;
    access_unit.nals = H264Bitstream::SplitPacket(
        packet.buffer->Data(),
        packet.buffer->Size(),
        avcc_length_size_);
    access_unit.keyframe = packet.keyframe || IsH264Keyframe(access_unit.nals);
    return access_unit;
}

std::uint32_t RtspServerProtocolAdapter::ToRtpTimestamp(const MediaPacket& packet) {
    const auto pts_us = TimestampToUs(packet.pts, packet.time_base);
    if (!have_timestamp_origin_) {
        timestamp_origin_us_ = pts_us;
        have_timestamp_origin_ = true;
    }

    const auto delta_us = pts_us - timestamp_origin_us_;
    if (delta_us <= 0) {
        return 0;
    }

    return static_cast<std::uint32_t>(delta_us * 90000 / 1000000);
}


