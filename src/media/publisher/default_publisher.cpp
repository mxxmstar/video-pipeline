#include "media/publisher/default_publisher.h"

#include "common/log/logger.h"

#include <utility>



DefaultPublisher::DefaultPublisher(PublisherConfig config)
    : config_(std::move(config)) {
}

DefaultPublisher::~DefaultPublisher() {
    Stop();
}

PublisherResult DefaultPublisher::Start() {
    Stop();

    config_.protocol = ResolvePublishProtocol(config_);
    last_result_ = config_.Validate();
    if (!last_result_) {
        LOG_ERROR("DefaultPublisher: invalid publisher config: {}",
                  last_result_.message);
        return last_result_;
    }

    adapter_ = CreateProtocolAdapter(config_.protocol);
    if (!adapter_) {
        last_result_ = PublisherResult::Failure(
            PublisherErrorCode::UnsupportedProtocol,
            "no adapter is registered for the selected publish protocol");
        LOG_ERROR("DefaultPublisher: {} ({})",
                  last_result_.message,
                  static_cast<int>(config_.protocol));
        return last_result_;
    }

    last_result_ = adapter_->Open(config_);
    started_ = last_result_.IsSuccess();
    if (!started_) {
        adapter_->Close();
        adapter_.reset();
    }
    return last_result_;
}

PublisherResult DefaultPublisher::Publish(const MediaPacket& packet) {
    if (!started_ || !adapter_) {
        last_result_ = PublisherResult::Failure(
            PublisherErrorCode::InvalidState,
            "publisher must be started before publishing packets");
        return last_result_;
    }

    last_result_ = adapter_->Send(packet);
    // AwaitingKeyframe 只表示协议层已经重建连接、正在等待独立解码入口，
    // 不能把 started_ 清掉；下一关键帧必须继续进入同一个 adapter 会话。
    if (last_result_.code == PublisherErrorCode::RuntimeDisconnected) {
        started_ = false;
    }
    return last_result_;
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

PublisherResult DefaultPublisher::GetLastResult() const {
    return last_result_;
}

std::unique_ptr<IPublisher> IPublisher::Create(PublisherConfig config) {
    return std::make_unique<DefaultPublisher>(std::move(config));
}
