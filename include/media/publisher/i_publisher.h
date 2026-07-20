#pragma once

#include <memory>
#include <string>

#include "media/media_packet.h"
#include "media/publisher/publisher_config.h"



struct PublisherStats {
    std::uint64_t packets_published{0};
    std::uint64_t bytes_published{0};
    std::uint64_t clients_connected{0};
};

class IPublisher {
public:
    virtual ~IPublisher() = default;

    virtual bool Start(const PublisherConfig& config) = 0;
    virtual bool Publish(const MediaPacket& packet) = 0;
    virtual void Stop() = 0;

    virtual std::string GetPlayUrl() const = 0;
    virtual PublisherStats GetStats() const = 0;

    static std::unique_ptr<IPublisher> Create(PublisherConfig config);
};


