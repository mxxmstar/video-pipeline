#pragma once

#include <memory>

#include "media/protocol/i_protocol.h"
#include "media/protocol/i_protocol_adapter.h"

class FfmpegProtocolAdapter : public IProtocolAdapter {
public:
    explicit FfmpegProtocolAdapter(std::unique_ptr<IProtocol> protocol = nullptr);
    ~FfmpegProtocolAdapter() override;

    bool Open(const PublisherConfig& config) override;
    bool Send(const MediaPacket& packet) override;
    void Close() override;

    std::string GetOutputUrl() const override;
    PublisherStats GetStats() const override;

private:
    EncodedAccessUnit ToAccessUnit(const MediaPacket& packet) const;

    PublisherConfig config_;
    std::unique_ptr<IProtocol> protocol_;
    bool opened_{false};
};


