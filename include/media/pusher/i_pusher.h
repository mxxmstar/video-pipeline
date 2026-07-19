#pragma once

#include <memory>

#include "media/media_packet.h"
#include "media/pusher/pusher_config.h"

class IPusher {
public:
    virtual ~IPusher() = default;

    virtual bool Connect() = 0;
    virtual bool Send(MediaPacket pkt) = 0;
    virtual bool Close() = 0;

    virtual bool Disconnect() { return Close(); }

    static std::unique_ptr<IPusher> Create(PusherConfig config);
};
