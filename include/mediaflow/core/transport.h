#pragma once

#include "mediaflow/core/types.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

/**
 * @file transport.h
 * @brief MediaFlow 节点之间的消息传输通道。
 *
 * QueueTransport 使用 mutex + condition_variable 实现一个正确性优先的
 * 有界多生产者/多消费者队列。当前实现没有假设调用方满足 SPSC 约束，
 * 因此 DropOldest 不会在生产线程额外执行 pop，不会把一个 SPSC 队列错误
 * 地变成两个消费者。
 */
namespace mediaflow {

/**
 * @brief Transport 的统一接口。
 *
 * Open/Close 是可重复生命周期的一部分。Graph::Start 会打开并清空通道，
 * Graph::Stop 会关闭通道；关闭后 Send 必须返回 Closed，而不是静默丢失。
 */
template <typename T>
class ITransport {
public:
    using NotifyCallback = std::function<void()>;
    using SendResultCallback = std::function<void(MailboxPushResult)>;

    virtual ~ITransport() = default;
    /// 发送消息并返回精确的背压结果。
    virtual MailboxPushResult Send(T value) = 0;
    /// 非阻塞取出一条消息；没有消息时返回 nullopt。
    virtual std::optional<T> TryReceive() = 0;
    /// 打开通道，QueueTransport 同时清空上一次生命周期残留消息。
    virtual void Open() = 0;
    /// 关闭通道并唤醒可能正在等待空间的生产者。
    virtual void Close() = 0;
    virtual bool Empty() const = 0;
    virtual std::size_t Size() const = 0;
    /// 设置消息入队后触发的调度通知。
    virtual void SetNotifyCallback(NotifyCallback callback) = 0;
    /// 设置用于 NodeMetrics 统计的发送结果回调。
    virtual void SetSendResultCallback(SendResultCallback callback) = 0;
};

/**
 * @brief 带背压策略的有界异步队列。
 *
 * Send 的锁只保护队列状态，回调在解锁后执行，避免用户回调重入
 * Transport 时形成锁反转。Block 策略使用条件变量等待空间；Close 会
 * 唤醒等待者，使停止流程不会永久阻塞在发送端。
 */
template <typename T>
class QueueTransport final : public ITransport<T> {
public:
    /// capacity 为 0 时自动调整为 1，避免永远无法入队。
    explicit QueueTransport(std::size_t capacity = 64,
                            BackpressurePolicy policy = BackpressurePolicy::DropNewest)
        : capacity_(capacity == 0 ? 1 : capacity),
          policy_(policy) {}

    /// 按配置执行入队、丢新消息或淘汰队首旧消息。
    MailboxPushResult Send(T value) override {
        MailboxPushResult result = MailboxPushResult::Closed;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (closed_) {
                result = MailboxPushResult::Closed;
            } else if (policy_ == BackpressurePolicy::Block) {
                // 谓词同时检查 closed_，这样 Close 可以打断等待。
                space_cv_.wait(lock, [this]() {
                    return closed_ || queue_.size() < capacity_;
                });
                if (!closed_) {
                    queue_.push_back(std::move(value));
                    result = MailboxPushResult::Accepted;
                }
            } else if (queue_.size() < capacity_) {
                queue_.push_back(std::move(value));
                result = MailboxPushResult::Accepted;
            } else if (policy_ == BackpressurePolicy::DropOldest) {
                // 该队列由 mutex 保护，生产侧淘汰旧消息不会破坏消费者状态。
                queue_.pop_front();
                queue_.push_back(std::move(value));
                result = MailboxPushResult::DroppedOldest;
            } else {
                result = MailboxPushResult::DroppedNewest;
            }
        }

        // 先唤醒可能等待空间的线程，再触发外部回调；回调本身不持有队列锁。
        space_cv_.notify_all();
        if (send_result_callback_) {
            send_result_callback_(result);
        }
        if ((result == MailboxPushResult::Accepted ||
             result == MailboxPushResult::DroppedOldest) &&
            notify_callback_) {
            notify_callback_();
        }
        return result;
    }

    /// 非阻塞取出队首消息，并通知 Block 生产者有了新的空位。
    std::optional<T> TryReceive() override {
        std::optional<T> value;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!queue_.empty()) {
                value.emplace(std::move(queue_.front()));
                queue_.pop_front();
            }
        }
        space_cv_.notify_all();
        return value;
    }

    /// 为新的 Graph 生命周期打开通道，并清除旧生命周期中的消息。
    void Open() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
            closed_ = false;
        }
        space_cv_.notify_all();
    }

    /// 关闭通道并唤醒所有等待中的 Send。
    void Close() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            // Close 是立即停止语义的一部分，释放尚未处理的消息，避免 Graph
            // 停止后仍然长期持有大帧或大 packet 的内存。
            queue_.clear();
        }
        space_cv_.notify_all();
    }

    bool Empty() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

    std::size_t Size() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    /// 注册入队通知。Graph 使用它把 Edge Drain 投递到下游 Executor。
    void SetNotifyCallback(typename ITransport<T>::NotifyCallback callback) override {
        notify_callback_ = std::move(callback);
    }

    /// 注册发送结果统计回调。
    void SetSendResultCallback(typename ITransport<T>::SendResultCallback callback) override {
        send_result_callback_ = std::move(callback);
    }

    std::size_t Capacity() const {
        return capacity_;
    }

private:
    const std::size_t capacity_;  ///< 队列最大元素数。
    BackpressurePolicy policy_;   ///< 队列满时的处理策略。
    mutable std::mutex mutex_;    ///< 保护 queue_ 和 closed_。
    std::condition_variable space_cv_;  ///< 生产者等待空间或关闭通知。
    std::deque<T> queue_;         ///< 按 FIFO 顺序保存消息。
    bool closed_{false};           ///< 关闭后拒绝新消息。
    typename ITransport<T>::NotifyCallback notify_callback_;
    typename ITransport<T>::SendResultCallback send_result_callback_;
};

/**
 * @brief 不缓存消息、但投递到下游 Executor 的 Transport。
 *
 * ExecutorDispatchTransport 仍然通过下游 NodeContext::Dispatch 把业务处理投递到
 * 目标执行器，因此它只省略中间队列，不会让媒体处理逻辑意外跑在发送方
 * 线程中。它适合低延迟或明确要求无缓冲的边，不适合可能阻塞的网络写入。
 */
template <typename T>
class ExecutorDispatchTransport final : public ITransport<T> {
public:
    using Consumer = std::function<void(T)>;

    /// 快照消费者和回调后解锁，再执行外部逻辑，避免回调重入自锁。
    MailboxPushResult Send(T value) override {
        Consumer consumer;
        typename ITransport<T>::SendResultCallback result_callback;
        bool closed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed = closed_;
            consumer = consumer_;
            result_callback = send_result_callback_;
        }

        if (closed) {
            if (result_callback) {
                result_callback(MailboxPushResult::Closed);
            }
            return MailboxPushResult::Closed;
        }

        if (result_callback) {
            result_callback(MailboxPushResult::Accepted);
        }
        if (consumer) {
            consumer(std::move(value));
        }
        return MailboxPushResult::Accepted;
    }

    /// DirectTransport 没有内部队列，因此始终没有可供 Drain 取出的消息。
    std::optional<T> TryReceive() override {
        return std::nullopt;
    }

    /// 允许 Graph 重启后重新发送消息。
    void Open() override {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = false;
    }

    /// 关闭后 Send 返回 Closed。
    void Close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }

    bool Empty() const override {
        return true;
    }

    std::size_t Size() const override {
        return 0;
    }

    void SetNotifyCallback(typename ITransport<T>::NotifyCallback callback) override {
        std::lock_guard<std::mutex> lock(mutex_);
        notify_callback_ = std::move(callback);
    }

    void SetSendResultCallback(typename ITransport<T>::SendResultCallback callback) override {
        std::lock_guard<std::mutex> lock(mutex_);
        send_result_callback_ = std::move(callback);
    }

    /// 设置下游投递函数。该函数不应执行长时间阻塞工作。
    void SetConsumer(Consumer consumer) {
        std::lock_guard<std::mutex> lock(mutex_);
        consumer_ = std::move(consumer);
    }

private:
    mutable std::mutex mutex_;
    bool closed_{false};
    Consumer consumer_;
    typename ITransport<T>::NotifyCallback notify_callback_;
    typename ITransport<T>::SendResultCallback send_result_callback_;
};

/// 兼容早期代码的名称；新代码应使用 ExecutorDispatchTransport。
template <typename T>
using DirectTransport = ExecutorDispatchTransport<T>;

} // namespace mediaflow
