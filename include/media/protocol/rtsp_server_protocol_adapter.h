#pragma once

#include <memory>
#include <unordered_map>

#include "media/protocol/h264_bitstream.h"
#include "media/protocol/i_protocol.h"
#include "media/protocol/i_protocol_adapter.h"



class RtspServerProtocolAdapter : public IProtocolAdapter {
public:
    explicit RtspServerProtocolAdapter(std::unique_ptr<IProtocol> protocol = nullptr);
    ~RtspServerProtocolAdapter() override;

    PublisherResult Open(const PublisherConfig& config) override;
    PublisherResult Send(const MediaPacket& packet) override;
    void Close() override;

    std::string GetOutputUrl() const override;
    PublisherStats GetStats() const override;

private:
    const MediaTrackConfig* FindTrackForPacket(const MediaPacket& packet) const;
    EncodedAccessUnit ToAccessUnit(const MediaPacket& packet);
    std::uint32_t ToRtpTimestamp(const MediaPacket& packet,
                                 const MediaTrackConfig& track);

    PublisherConfig config_;
    std::unique_ptr<IProtocol> protocol_;
    std::unordered_map<int, int> avcc_length_size_by_track_;
    std::unordered_map<int, H264ParameterSets> h264_parameter_sets_by_track_;
    bool opened_{false};
    std::unordered_map<int, int64_t> timestamp_origin_us_by_track_;
};
