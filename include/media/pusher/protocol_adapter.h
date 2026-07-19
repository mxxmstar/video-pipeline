#pragma once

#include "media/media_packet.h"
#include "media/pusher/pusher_config.h"

class IProtocolAdapter {
public:
    virtual ~IProtocolAdapter() = default;

    virtual bool Connect(const PusherConfig& config) = 0;
    virtual bool Send(const MediaPacket& pkt) = 0;
    virtual bool Close() = 0;
};
