#include "media/protocol/ffmpeg_protocol_adapter.h"

#include "media/protocol/ffmpeg_mux_protocol.h"

#include <utility>



FfmpegProtocolAdapter::FfmpegProtocolAdapter(std::unique_ptr<IProtocol> protocol)
    : protocol_(std::move(protocol)) {
}

FfmpegProtocolAdapter::~FfmpegProtocolAdapter() {
    Close();
}

PublisherResult FfmpegProtocolAdapter::Open(const PublisherConfig& config) {
    Close();

    config_ = config;
    if (!protocol_) {
        protocol_ = std::make_unique<FfmpegMuxProtocol>();
    }

    auto result = protocol_->Start(config_, config_.tracks);
    opened_ = result.IsSuccess();
    return result;
}

PublisherResult FfmpegProtocolAdapter::Send(const MediaPacket& packet) {
    if (!opened_ || !protocol_) {
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidState,
            "FFmpeg protocol adapter is not open");
    }

    return protocol_->Write(ToAccessUnit(packet));
}

void FfmpegProtocolAdapter::Close() {
    if (protocol_) {
        protocol_->Stop();
    }
    opened_ = false;
}

std::string FfmpegProtocolAdapter::GetOutputUrl() const {
    return protocol_ ? protocol_->GetOutputUrl() : std::string{};
}

PublisherStats FfmpegProtocolAdapter::GetStats() const {
    return protocol_ ? protocol_->GetStats() : PublisherStats{};
}

EncodedAccessUnit FfmpegProtocolAdapter::ToAccessUnit(const MediaPacket& packet) const {
    EncodedAccessUnit access_unit;
    if (config_.tracks.size() == 1) {
        access_unit.track_id = config_.tracks.front().track_id;
    } else {
        access_unit.track_id = packet.stream_index >= 0 ? packet.stream_index : 0;
    }
    access_unit.media_type = packet.type;
    access_unit.codec_type = packet.codec;
    access_unit.pts = packet.pts;
    access_unit.dts = packet.dts;
    access_unit.duration = packet.duration;
    access_unit.time_base = packet.time_base;
    access_unit.keyframe = packet.keyframe;
    access_unit.encoded_data = packet.buffer;
    access_unit.backend = packet.backend;
    return access_unit;
}

