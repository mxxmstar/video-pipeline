#pragma once

#include <memory>

#include "media/protocol/h264_bitstream.h"
#include "media/protocol/i_protocol.h"
#include "media/protocol/i_protocol_adapter.h"



class RtspServerProtocolAdapter : public IProtocolAdapter {
public:
    explicit RtspServerProtocolAdapter(std::unique_ptr<IProtocol> protocol = nullptr);
    ~RtspServerProtocolAdapter() override;

    bool Open(const PublisherConfig& config) override;
    bool Send(const MediaPacket& packet) override;
    void Close() override;

    std::string GetOutputUrl() const override;
    PublisherStats GetStats() const override;

private:
    EncodedAccessUnit ToAccessUnit(const MediaPacket& packet);
    std::uint32_t ToRtpTimestamp(const MediaPacket& packet);

    PublisherConfig config_;
    std::unique_ptr<IProtocol> protocol_;
    int avcc_length_size_{4};
    bool opened_{false};
    bool have_timestamp_origin_{false};
    int64_t timestamp_origin_us_{0};
};


