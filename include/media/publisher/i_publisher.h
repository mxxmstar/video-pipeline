#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "media/media_packet.h"
#include "media/publisher/publisher_config.h"



struct PublisherStats {
    std::uint64_t packets_published{0};
    std::uint64_t bytes_published{0};
    std::uint64_t clients_connected{0};
    std::uint64_t rtcp_receiver_reports_received{0};
};

class IPublisher {
public:
    virtual ~IPublisher() = default;

    /// @brief 使用 Create() 时固定的配置启动发布任务。
    virtual PublisherResult Start() = 0;
    virtual PublisherResult Publish(const MediaPacket& packet) = 0;
    virtual void Stop() = 0;

    virtual std::string GetPlayUrl() const = 0;
    virtual PublisherStats GetStats() const = 0;
    virtual PublisherResult GetLastResult() const = 0;

    static std::unique_ptr<IPublisher> Create(PublisherConfig config);
};
