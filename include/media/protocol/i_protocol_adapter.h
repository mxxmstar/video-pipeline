#pragma once

#include <memory>
#include <string>

#include "media/media_packet.h"
#include "media/publisher/i_publisher.h"



class IProtocolAdapter {
public:
    virtual ~IProtocolAdapter() = default;

    virtual bool Open(const PublisherConfig& config) = 0;
    virtual bool Send(const MediaPacket& packet) = 0;
    virtual void Close() = 0;

    virtual std::string GetOutputUrl() const = 0;
    virtual PublisherStats GetStats() const = 0;
};

std::unique_ptr<IProtocolAdapter> CreateProtocolAdapter(PublishProtocol protocol);


