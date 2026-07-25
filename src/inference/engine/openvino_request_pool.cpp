#include "engine/openvino_request_pool.h"
#include "inference/inference_logger.h"

bool OpenVinoInferRequestPool::Initialize(ov::CompiledModel& model, uint32_t request_count) {
    if (request_count == 0) {
        LOG_MAIN_ERROR_AT("request_count must be greater than 0");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = false;
    total_count_ = 0;

    if (!idle_requests_.empty()) {
        LOG_MAIN_WARN_AT("OpenVINO request pool already initialized, clearing...");
        while (!idle_requests_.empty()) {
            idle_requests_.pop();
        }
    }

    for (uint32_t i = 0; i < request_count; ++i) {
        try {
            auto request = std::make_shared<ov::InferRequest>(model.create_infer_request());
            idle_requests_.push(std::move(request));
        } catch (const std::exception& e) {
            LOG_MAIN_ERROR_AT("Failed to create infer request {}: {}", i, e.what());
            while (!idle_requests_.empty()) {
                idle_requests_.pop();
            }
            total_count_ = 0;
            return false;
        }
    }

    total_count_ = idle_requests_.size();
    LOG_MAIN_INFO_AT("OpenVINO request pool initialized with {} requests", total_count_);
    return true;
}

std::shared_ptr<ov::InferRequest> OpenVinoInferRequestPool::Acquire() {
    std::unique_lock<std::mutex> lock(mutex_);

    cv_.wait(lock, [this]() { return shutdown_ || !idle_requests_.empty(); });
    if (shutdown_ || idle_requests_.empty()) {
        return nullptr;
    }

    auto request = std::move(idle_requests_.front());
    idle_requests_.pop();

    return request;
}

void OpenVinoInferRequestPool::Release(std::shared_ptr<ov::InferRequest> request) {
    if (!request) {
        LOG_MAIN_WARN_AT("Release nullptr request, ignored");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        idle_requests_.push(std::move(request));
    }

    cv_.notify_one();
}

void OpenVinoInferRequestPool::Shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutdown_ = true;
    }

    cv_.notify_all();
}

bool OpenVinoInferRequestPool::WaitAll() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]() { return idle_requests_.size() >= total_count_; });

    return true;
}

size_t OpenVinoInferRequestPool::IdleCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return idle_requests_.size();
}

size_t OpenVinoInferRequestPool::TotalCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_count_;
}
