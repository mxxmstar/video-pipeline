#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

#include <openvino/openvino.hpp>

/// @brief OpenVINO 推理请求池
class OpenVinoInferRequestPool {
public:
    /// @brief 初始化推理池
    /// @param model 编译后的模型
    /// @param request_count 推理请求数量
    /// @return 是否初始化成功
    bool Initialize(ov::CompiledModel& model, uint32_t request_count);
    /// @brief 获取推理请求
    std::shared_ptr<ov::InferRequest> Acquire();
    /// @brief 释放推理请求
    void Release(std::shared_ptr<ov::InferRequest> request);
    /// @brief 停止接收新的推理请求并唤醒等待线程
    void Shutdown();

    /// @brief 等待所有异步推理请求完成
    bool WaitAll();
    
    /// @brief 获取空闲推理请求数量
    size_t IdleCount() const;
    /// @brief 获取总推理请求数量
    size_t TotalCount() const;

private:
    /// @brief 空闲推理请求队列
    std::queue<std::shared_ptr<ov::InferRequest>> idle_requests_;
    /// @brief 互斥锁
    mutable std::mutex mutex_;
    /// @brief 条件变量
    std::condition_variable cv_;
    /// @brief 总推理请求数量
    size_t total_count_{0};
    /// @brief 是否已经关闭请求池
    bool shutdown_{false};
};

/// @brief request_pool 的 RAII 包装
class RequestLease {
public:
    RequestLease(std::shared_ptr<OpenVinoInferRequestPool> pool, std::shared_ptr<ov::InferRequest> request)
        : pool_(std::move(pool)),
          request_(std::move(request)) {
    }

    ~RequestLease() {
        if (pool_ && request_) {
            pool_->Release(std::move(request_));
        }
    }

    bool Valid() const {
        return request_ != nullptr;
    }

    ov::InferRequest& operator*() {
        return *request_;
    }

    ov::InferRequest* operator->() {
        return request_.get();
    }

private:
    std::shared_ptr<OpenVinoInferRequestPool> pool_;
    std::shared_ptr<ov::InferRequest> request_;
};
