#pragma once

#include <memory>

#include "media/protocol/i_protocol_adapter.h"
#include "media/publisher/i_publisher.h"



class DefaultPublisher : public IPublisher {
public:
    explicit DefaultPublisher(PublisherConfig config);
    ~DefaultPublisher() override;

    PublisherResult Start() override;
    PublisherResult Publish(const MediaPacket& packet) override;
    void Stop() override;

    std::string GetPlayUrl() const override;
    PublisherStats GetStats() const override;
    PublisherResult GetLastResult() const override;

private:
    PublisherConfig config_;
    std::unique_ptr<IProtocolAdapter> adapter_;
    PublisherResult last_result_;
    bool started_{false};
};

