#pragma once

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>

#include <openvino/openvino.hpp>

/// @brief OpenVINO 鎺ㄧ悊璇锋眰姹?
class OpenVinoInferRequestPool {
public:
    /// @brief 鍒濆鍖栨帹鐞嗘睜
    /// @param model 缂栬瘧鍚庣殑妯″瀷
    /// @param request_count 鎺ㄧ悊璇锋眰鏁伴噺
    /// @return 鏄惁鍒濆鍖栨垚鍔?
    bool Initialize(ov::CompiledModel& model, uint32_t request_count);
    /// @brief 鑾峰彇鎺ㄧ悊璇锋眰
    std::shared_ptr<ov::InferRequest> Acquire();
    /// @brief 閲婃斁鎺ㄧ悊璇锋眰
    void Release(std::shared_ptr<ov::InferRequest> request);
    /// @brief 鍋滄鎺ユ敹鏂扮殑鎺ㄧ悊璇锋眰骞跺敜閱掔瓑寰呯嚎绋?
    void Shutdown();

    /// @brief 绛夊緟鎵€鏈夊紓姝ユ帹鐞嗚姹傚畬鎴?
    bool WaitAll();
    
    /// @brief 鑾峰彇绌洪棽鎺ㄧ悊璇锋眰鏁伴噺
    size_t IdleCount() const;
    /// @brief 鑾峰彇鎬绘帹鐞嗚姹傛暟閲?
    size_t TotalCount() const;

private:
    /// @brief 绌洪棽鎺ㄧ悊璇锋眰闃熷垪
    std::queue<std::shared_ptr<ov::InferRequest>> idle_requests_;
    /// @brief 浜掓枼閿?
    mutable std::mutex mutex_;
    /// @brief 鏉′欢鍙橀噺
    std::condition_variable cv_;
    /// @brief 鎬绘帹鐞嗚姹傛暟閲?
    size_t total_count_{0};
    /// @brief 鏄惁宸茬粡鍏抽棴璇锋眰姹?
    bool shutdown_{false};
};

/// @brief request_pool 鐨?RAII 鍖呰
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
