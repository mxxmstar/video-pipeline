#include "media/publisher/default_publisher.h"

#include "common/log/logger.h"

#include <utility>



namespace {

PublishProtocol ResolveProtocol(const PublisherConfig& config) {
    if (config.protocol != PublishProtocol::Auto) {
        return config.protocol;
    }

    if (config.mode == PublishMode::PullServer) {
        return PublishProtocol::RtspServer;
    }

    return PublishProtocol::FfmpegMux;
}

} // namespace

DefaultPublisher::DefaultPublisher(PublisherConfig config)
    : config_(std::move(config)) {
}

DefaultPublisher::~DefaultPublisher() {
    Stop();
}

bool DefaultPublisher::Start(const PublisherConfig& config) {
    Stop();

    config_ = config;
    config_.protocol = ResolveProtocol(config_);
    if (!config_.IsValid()) {
        LOG_ERROR("DefaultPublisher: invalid publisher config");
        return false;
    }

    adapter_ = CreateProtocolAdapter(config_.protocol);
    if (!adapter_) {
        LOG_ERROR("DefaultPublisher: unsupported publish protocol {}",
                  static_cast<int>(config_.protocol));
        return false;
    }

    started_ = adapter_->Open(config_);
    return started_;
}

bool DefaultPublisher::Publish(const MediaPacket& packet) {
    if (!started_ || !adapter_) {
        return false;
    }

    return adapter_->Send(packet);
}

void DefaultPublisher::Stop() {
    if (adapter_) {
        adapter_->Close();
        adapter_.reset();
    }
    started_ = false;
}

std::string DefaultPublisher::GetPlayUrl() const {
    return adapter_ ? adapter_->GetOutputUrl() : std::string{};
}

PublisherStats DefaultPublisher::GetStats() const {
    return adapter_ ? adapter_->GetStats() : PublisherStats{};
}

std::unique_ptr<IPublisher> IPublisher::Create(PublisherConfig config) {
    return std::make_unique<DefaultPublisher>(std::move(config));
}


