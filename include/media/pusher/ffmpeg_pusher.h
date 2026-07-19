#pragma once

#include <memory>

#include "media/pusher/i_pusher.h"
#include "media/pusher/protocol_adapter.h"

class FFmpegPusher : public IPusher {
public:
    explicit FFmpegPusher(PusherConfig config);
    FFmpegPusher(PusherConfig config, std::unique_ptr<IProtocolAdapter> adapter);
    ~FFmpegPusher() override;

    bool Connect() override;
    bool Send(MediaPacket pkt) override;
    bool Close() override;

private:
    PusherConfig config_;
    std::unique_ptr<IProtocolAdapter> adapter_;
    bool connected_{false};
};
