#pragma once

#include <string>
#include <vector>

#include "media/protocol/protocol_types.h"
#include "media/publisher/i_publisher.h"



class IProtocol {
public:
    virtual ~IProtocol() = default;

    virtual PublisherResult Start(
        const PublisherConfig& config,
        const std::vector<MediaTrackConfig>& tracks) = 0;
    virtual PublisherResult Write(const EncodedAccessUnit& access_unit) = 0;
    virtual void Stop() = 0;

    virtual std::string GetOutputUrl() const = 0;
    virtual PublisherStats GetStats() const = 0;
};

