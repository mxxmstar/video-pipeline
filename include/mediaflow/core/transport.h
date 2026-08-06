#pragma once

#include "mediaflow/core/types.h"

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
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

/// @brief 描述队列元素是否属于需要优先保护的媒体类别。
///
/// 核心队列不依赖具体媒体消息。媒体模块可以为自己的消息类型提供特化，
/// 让 PreferVideoKeyframes 只在明确知道音频、视频和关键帧语义时生效；
/// 其他类型仍然按 DropNewest 处理，避免核心层猜测业务字段。
template <typename T>
struct QueueItemTraits {
    static bool IsAudio(const T&) { return false; }
    static bool IsVideo(const T&) { return false; }
    static bool IsKeyframe(const T&) { return false; }
    static bool IsControl(const T&) { return false; }
    static QueueItemCost Cost(const T&) { return {}; }
};

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
    using CapacityReasonCallback = std::function<void(QueueLimitReason)>;

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
    virtual QueueMetricsSnapshot Metrics() const { return {}; }
    /// 设置消息入队后触发的调度通知。
    virtual void SetNotifyCallback(NotifyCallback callback) = 0;
    /// 设置用于 NodeMetrics 统计的发送结果回调。
    virtual void SetSendResultCallback(SendResultCallback callback) = 0;
    virtual void SetCapacityReasonCallback(CapacityReasonCallback callback) {
        (void)callback;
    }
};

/**
 * @brief 带背压策略的有界异步队列。
 *
 * Send 的锁只保护队列状态，回调在解锁后执行，避免用户回调重入
 * Transport 时形成锁反转。Block 策略使用条件变量等待空间；Close 会
 * 唤醒等待者，使停止流程不会永久阻塞在发送端。
 */
template <typename T>
class LegacyQueueTransport final : public ITransport<T> {
public:
    /// capacity 为 0 时自动调整为 1，避免永远无法入队。
    explicit LegacyQueueTransport(
        std::size_t capacity = 64,
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
            } else if (policy_ == BackpressurePolicy::PreferVideoKeyframes) {
                // 音频突发不能挤掉已经排队的视频。视频到达时先淘汰音频，
                // 仍无空间时再淘汰视频非关键帧；视频关键帧在队列全为关键帧时
                // 宁可拒绝当前包并留下可观测的 DroppedNewest 统计。
                const bool incoming_audio = QueueItemTraits<T>::IsAudio(value);
                const bool incoming_video = QueueItemTraits<T>::IsVideo(value);
                const bool incoming_keyframe = QueueItemTraits<T>::IsKeyframe(value);
                const bool incoming_control = QueueItemTraits<T>::IsControl(value);

                if (incoming_control) {
                    // EOS、FlushDone 等控制消息不能被普通媒体包永久挡住。
                    // 优先淘汰非控制消息，保留队列中已经存在的控制边界。
                    auto victim = std::find_if(queue_.begin(), queue_.end(),
                        [](const T& item) {
                            return !QueueItemTraits<T>::IsControl(item);
                        });
                    if (victim != queue_.end()) {
                        queue_.erase(victim);
                        queue_.push_back(std::move(value));
                        result = MailboxPushResult::DroppedOldest;
                    } else {
                        result = MailboxPushResult::DroppedNewest;
                    }
                } else if (incoming_audio) {
                    result = MailboxPushResult::DroppedNewest;
                } else if (incoming_video) {
                    auto victim = std::find_if(queue_.begin(), queue_.end(),
                        [](const T& item) {
                            return QueueItemTraits<T>::IsAudio(item);
                        });
                    if (victim == queue_.end() && incoming_keyframe) {
                        victim = std::find_if(queue_.begin(), queue_.end(),
                            [](const T& item) {
                                return QueueItemTraits<T>::IsVideo(item) &&
                                       !QueueItemTraits<T>::IsKeyframe(item);
                            });
                    }
                    if (victim != queue_.end()) {
                        queue_.erase(victim);
                        queue_.push_back(std::move(value));
                        result = MailboxPushResult::DroppedOldest;
                    } else {
                        result = MailboxPushResult::DroppedNewest;
                    }
                } else {
                    result = MailboxPushResult::DroppedNewest;
                }
            } else {
                // DropNewest 仍然保持普通媒体包的原有语义，但媒体边界可以
                // 将 EOS 等控制消息声明为 IsControl，从而避免满载时丢失
                // 生命周期边界。控制消息替换一条普通媒体消息并保留 FIFO
                // 中其余内容，发送方会收到 DroppedOldest 结果。
                if (QueueItemTraits<T>::IsControl(value)) {
                    auto victim = std::find_if(queue_.begin(), queue_.end(),
                        [](const T& item) {
                            return !QueueItemTraits<T>::IsControl(item);
                        });
                    if (victim != queue_.end()) {
                        queue_.erase(victim);
                        queue_.push_back(std::move(value));
                        result = MailboxPushResult::DroppedOldest;
                    } else {
                        result = MailboxPushResult::DroppedNewest;
                    }
                } else {
                    result = MailboxPushResult::DroppedNewest;
                }
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
class QueueTransport final : public ITransport<T> {
public:
    explicit QueueTransport(std::size_t capacity = 64,
                            BackpressurePolicy policy = BackpressurePolicy::DropNewest,
                            QueueBudget budget = {})
        : policy_(policy), budget_(NormalizeBudget(capacity, budget)) {}

    MailboxPushResult Send(T value) override {
        const QueueItemCost cost = QueueItemTraits<T>::Cost(value);
        const bool is_control = QueueItemTraits<T>::IsControl(value);
        MailboxPushResult result = MailboxPushResult::Closed;
        QueueLimitReason reason = QueueLimitReason::None;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (closed_) {
                result = MailboxPushResult::Closed;
            } else {
                RecordTimestampLocked(cost, is_control);
            }
            if (!closed_ && policy_ == BackpressurePolicy::Block) {
                space_cv_.wait(lock, [this, &cost]() {
                    return closed_ || FitsLocked(cost, nullptr);
                });
                if (!closed_) {
                    AppendLocked(std::move(value), cost);
                    result = MailboxPushResult::Accepted;
                }
            } else if (!closed_ && FitsLocked(cost, &reason)) {
                AppendLocked(std::move(value), cost);
                result = MailboxPushResult::Accepted;
            } else if (!closed_) {
                RecordLimitLocked(reason);
                if (policy_ == BackpressurePolicy::DropOldest) {
                    if (MakeRoomByDroppingOldestLocked(value, cost, reason)) {
                        AppendLocked(std::move(value), cost);
                        result = MailboxPushResult::DroppedOldest;
                    } else {
                        result = MailboxPushResult::DroppedNewest;
                    }
                } else if (policy_ == BackpressurePolicy::PreferVideoKeyframes) {
                    if (MakeRoomByPriorityLocked(value, cost, reason)) {
                        AppendLocked(std::move(value), cost);
                        result = MailboxPushResult::DroppedOldest;
                    } else {
                        result = MailboxPushResult::DroppedNewest;
                    }
                } else if (QueueItemTraits<T>::IsControl(value)) {
                    result = MailboxPushResult::DroppedNewest;
                    auto victim = FindNonControlLocked();
                    if (victim != queue_.end()) {
                        RecordDroppedItemLocked(*victim);
                        EraseLocked(victim);
                        if (FitsLocked(cost, nullptr)) {
                            AppendLocked(std::move(value), cost);
                            result = MailboxPushResult::DroppedOldest;
                        }
                    }
                } else {
                    result = MailboxPushResult::DroppedNewest;
                }
            }
            if (result == MailboxPushResult::DroppedNewest &&
                QueueItemTraits<T>::IsKeyframe(value)) {
                ++dropped_keyframes_;
            }
        }

        space_cv_.notify_all();
        if (reason != QueueLimitReason::None && capacity_reason_callback_) {
            capacity_reason_callback_(reason);
        }
        if (send_result_callback_) {
            send_result_callback_(result);
        }
        if ((result == MailboxPushResult::Accepted ||
             result == MailboxPushResult::DroppedOldest) && notify_callback_) {
            notify_callback_();
        }
        return result;
    }

    std::optional<T> TryReceive() override {
        std::optional<T> value;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!queue_.empty()) {
                value.emplace(std::move(queue_.front().value));
                EraseLocked(queue_.begin());
            }
        }
        space_cv_.notify_all();
        return value;
    }

    void Open() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ClearCurrentLocked();
            closed_ = false;
        }
        space_cv_.notify_all();
    }

    void Close() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            ClearCurrentLocked();
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

    QueueMetricsSnapshot Metrics() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return MetricsLocked();
    }

    void SetNotifyCallback(typename ITransport<T>::NotifyCallback callback) override {
        notify_callback_ = std::move(callback);
    }

    void SetSendResultCallback(typename ITransport<T>::SendResultCallback callback) override {
        send_result_callback_ = std::move(callback);
    }

    void SetCapacityReasonCallback(
        typename ITransport<T>::CapacityReasonCallback callback) override {
        capacity_reason_callback_ = std::move(callback);
    }

    std::size_t Capacity() const { return budget_.max_items; }
    const QueueBudget& Budget() const { return budget_; }

private:
    struct QueueEntry {
        T value;
        QueueItemCost cost;
    };

    struct TimeBounds {
        std::multiset<std::int64_t> starts;
        std::multiset<std::int64_t> ends;
    };

    static QueueBudget NormalizeBudget(std::size_t capacity, QueueBudget budget) {
        if (budget.max_items == 0 && budget.max_bytes == 0 &&
            budget.max_span_us == 0) {
            budget.max_items = capacity == 0 ? 1 : capacity;
        }
        return budget;
    }

    static std::int64_t EndTimestamp(const QueueItemCost& cost) {
        if (cost.timestamp_us == kNoQueueTimestamp || cost.duration_us <= 0) {
            return cost.timestamp_us;
        }
        const auto max_value = (std::numeric_limits<std::int64_t>::max)();
        return cost.timestamp_us > max_value - cost.duration_us
                   ? max_value
                   : cost.timestamp_us + cost.duration_us;
    }

    static std::int64_t Difference(std::int64_t start, std::int64_t end) {
        if (end <= start) return 0;
        const long double difference = static_cast<long double>(end) -
                                       static_cast<long double>(start);
        const auto max_value = (std::numeric_limits<std::int64_t>::max)();
        return difference >= static_cast<long double>(max_value)
                   ? max_value
                   : static_cast<std::int64_t>(difference);
    }

    static bool ReachesPercent(std::size_t value,
                                std::size_t limit,
                                std::uint32_t percent) {
        return value > 0 && limit > 0 &&
               static_cast<long double>(value) * 100.0L >=
                   static_cast<long double>(limit) * percent;
    }

    static bool ReachesPercent(std::int64_t value,
                                std::int64_t limit,
                                std::uint32_t percent) {
        return value > 0 && limit > 0 &&
               static_cast<long double>(value) * 100.0L >=
                   static_cast<long double>(limit) * percent;
    }

    static bool ExceedsPercent(std::size_t value,
                               std::size_t limit,
                               std::uint32_t percent) {
        return limit > 0 && static_cast<long double>(value) * 100.0L >
                                static_cast<long double>(limit) * percent;
    }

    static bool ExceedsPercent(std::int64_t value,
                               std::int64_t limit,
                               std::uint32_t percent) {
        return limit > 0 && static_cast<long double>(value) * 100.0L >
                                static_cast<long double>(limit) * percent;
    }

    bool AtOrAboveHighWatermarkLocked() const {
        return ReachesPercent(queue_.size(), budget_.max_items,
                               budget_.high_watermark_percent) ||
               ReachesPercent(bytes_, budget_.max_bytes,
                               budget_.high_watermark_percent) ||
               ReachesPercent(span_us_, budget_.max_span_us,
                               budget_.high_watermark_percent);
    }

    bool AtOrBelowLowWatermarkLocked() const {
        return !ExceedsPercent(queue_.size(), budget_.max_items,
                               budget_.low_watermark_percent) &&
               !ExceedsPercent(bytes_, budget_.max_bytes,
                               budget_.low_watermark_percent) &&
               !ExceedsPercent(span_us_, budget_.max_span_us,
                               budget_.low_watermark_percent);
    }

    void UpdateWatermarkStateLocked() {
        if (!high_watermark_active_ && AtOrAboveHighWatermarkLocked()) {
            high_watermark_active_ = true;
            ++high_watermark_enters_;
        } else if (high_watermark_active_ && AtOrBelowLowWatermarkLocked()) {
            high_watermark_active_ = false;
            ++high_watermark_leaves_;
        }
    }

    void RecordTimestampLocked(const QueueItemCost& cost, bool is_control) {
        if (is_control) return;
        if (cost.timestamp_us == kNoQueueTimestamp) {
            if (cost.bytes > 0 || cost.duration_us > 0) {
                ++timestamp_invalid_;
            }
            return;
        }

        auto [it, inserted] = latest_timestamp_by_generation_.try_emplace(
            cost.generation, cost.timestamp_us);
        if (!inserted) {
            if (cost.timestamp_us < it->second) {
                ++timestamp_discontinuity_;
            } else {
                it->second = cost.timestamp_us;
            }
        }
    }

    void AddCostLocked(const QueueItemCost& cost) {
        const auto max_bytes = (std::numeric_limits<std::size_t>::max)();
        bytes_ = cost.bytes > max_bytes - bytes_ ? max_bytes : bytes_ + cost.bytes;
        if (cost.timestamp_us != kNoQueueTimestamp) {
            auto& bounds = time_bounds_[cost.generation];
            bounds.starts.insert(cost.timestamp_us);
            bounds.ends.insert(EndTimestamp(cost));
        }
        RecomputeSpanLocked();
    }

    void RemoveCostLocked(const QueueItemCost& cost) {
        bytes_ = cost.bytes > bytes_ ? 0 : bytes_ - cost.bytes;
        if (cost.timestamp_us != kNoQueueTimestamp) {
            auto bounds_it = time_bounds_.find(cost.generation);
            if (bounds_it != time_bounds_.end()) {
                auto& bounds = bounds_it->second;
                auto start = bounds.starts.find(cost.timestamp_us);
                if (start != bounds.starts.end()) bounds.starts.erase(start);
                auto end = bounds.ends.find(EndTimestamp(cost));
                if (end != bounds.ends.end()) bounds.ends.erase(end);
                if (bounds.starts.empty()) time_bounds_.erase(bounds_it);
            }
        }
        RecomputeSpanLocked();
    }

    void RecomputeSpanLocked() {
        span_us_ = 0;
        for (const auto& [generation, bounds] : time_bounds_) {
            (void)generation;
            if (!bounds.starts.empty() && !bounds.ends.empty()) {
                span_us_ = std::max(span_us_, Difference(*bounds.starts.begin(),
                                                         *bounds.ends.rbegin()));
            }
        }
    }

    std::int64_t SpanWithLocked(const QueueItemCost& cost) const {
        if (cost.timestamp_us == kNoQueueTimestamp) return span_us_;

        std::int64_t result = 0;
        bool has_generation = false;
        for (const auto& [generation, bounds] : time_bounds_) {
            auto start = *bounds.starts.begin();
            auto end = *bounds.ends.rbegin();
            if (generation == cost.generation) {
                has_generation = true;
                start = std::min(start, cost.timestamp_us);
                end = std::max(end, EndTimestamp(cost));
            }
            result = std::max(result, Difference(start, end));
        }
        if (!has_generation) {
            result = std::max(result, Difference(cost.timestamp_us,
                                                 EndTimestamp(cost)));
        }
        return result;
    }

    bool FitsLocked(const QueueItemCost& cost, QueueLimitReason* reason) const {
        if (reason) *reason = QueueLimitReason::None;
        if (budget_.max_items > 0 && queue_.size() >= budget_.max_items) {
            if (reason) *reason = QueueLimitReason::Items;
            return false;
        }
        if (budget_.max_bytes > 0) {
            if (cost.bytes > budget_.max_bytes) {
                if (reason) *reason = QueueLimitReason::Oversized;
                return false;
            }
            if (bytes_ > budget_.max_bytes - cost.bytes) {
                if (reason) *reason = QueueLimitReason::Bytes;
                return false;
            }
        }
        if (budget_.max_span_us > 0) {
            const auto candidate_span = SpanWithLocked(cost);
            if (candidate_span > budget_.max_span_us) {
                const auto item_span = cost.timestamp_us == kNoQueueTimestamp
                                           ? 0
                                           : Difference(cost.timestamp_us,
                                                        EndTimestamp(cost));
                if (reason) {
                    *reason = queue_.empty() && item_span > budget_.max_span_us
                                  ? QueueLimitReason::Oversized
                                  : QueueLimitReason::Span;
                }
                return false;
            }
        }
        return true;
    }

    void AppendLocked(T value, const QueueItemCost& cost) {
        queue_.push_back(QueueEntry{std::move(value), cost});
        AddCostLocked(cost);
        items_high_watermark_ = std::max(items_high_watermark_, queue_.size());
        bytes_high_watermark_ = std::max(bytes_high_watermark_, bytes_);
        span_high_watermark_us_ = std::max(span_high_watermark_us_, span_us_);
        UpdateWatermarkStateLocked();
    }

    void EraseLocked(typename std::deque<QueueEntry>::iterator position) {
        RemoveCostLocked(position->cost);
        queue_.erase(position);
        UpdateWatermarkStateLocked();
    }

    void ClearCurrentLocked() {
        queue_.clear();
        time_bounds_.clear();
        latest_timestamp_by_generation_.clear();
        bytes_ = 0;
        span_us_ = 0;
        UpdateWatermarkStateLocked();
    }

    typename std::deque<QueueEntry>::iterator FindNonControlLocked() {
        return std::find_if(queue_.begin(), queue_.end(),
            [](const QueueEntry& entry) {
                return !QueueItemTraits<T>::IsControl(entry.value);
            });
    }

    void RecordLimitLocked(QueueLimitReason reason) {
        switch (reason) {
        case QueueLimitReason::Items: ++limit_items_; break;
        case QueueLimitReason::Bytes: ++limit_bytes_; break;
        case QueueLimitReason::Span: ++limit_span_; break;
        case QueueLimitReason::Oversized: ++oversized_; break;
        case QueueLimitReason::None: break;
        }
    }

    void RecordDroppedItemLocked(const QueueEntry& entry) {
        if (QueueItemTraits<T>::IsKeyframe(entry.value)) {
            ++dropped_keyframes_;
        }
    }

    bool MakeRoomByDroppingOldestLocked(const T& value,
                                        const QueueItemCost& cost,
                                        QueueLimitReason reason) {
        if (reason == QueueLimitReason::Oversized) return false;
        while (!FitsLocked(cost, nullptr)) {
            // 控制边界不能被普通低延迟消息淘汰；没有普通消息可换时拒绝当前消息。
            auto victim = FindNonControlLocked();
            if (victim == queue_.end()) return false;
            RecordDroppedItemLocked(*victim);
            EraseLocked(victim);
        }
        return true;
    }

    bool MakeRoomByPriorityLocked(const T& value,
                                  const QueueItemCost& cost,
                                  QueueLimitReason reason) {
        if (reason == QueueLimitReason::Oversized ||
            QueueItemTraits<T>::IsAudio(value)) {
            return false;
        }
        while (!FitsLocked(cost, nullptr)) {
            auto victim = queue_.end();
            if (QueueItemTraits<T>::IsControl(value)) {
                victim = FindNonControlLocked();
            } else if (QueueItemTraits<T>::IsVideo(value)) {
                victim = std::find_if(queue_.begin(), queue_.end(),
                    [](const QueueEntry& entry) {
                        return QueueItemTraits<T>::IsAudio(entry.value);
                    });
                if (victim == queue_.end() &&
                    QueueItemTraits<T>::IsKeyframe(value)) {
                    victim = std::find_if(queue_.begin(), queue_.end(),
                        [](const QueueEntry& entry) {
                            return QueueItemTraits<T>::IsVideo(entry.value) &&
                                   !QueueItemTraits<T>::IsKeyframe(entry.value);
                        });
                }
            }
            if (victim == queue_.end()) return false;
            RecordDroppedItemLocked(*victim);
            EraseLocked(victim);
        }
        return true;
    }

    QueueMetricsSnapshot MetricsLocked() const {
        return {queue_.size(), bytes_, span_us_, items_high_watermark_,
                bytes_high_watermark_, span_high_watermark_us_, limit_items_,
                limit_bytes_, limit_span_, oversized_, high_watermark_active_,
                high_watermark_enters_, high_watermark_leaves_,
                dropped_keyframes_, timestamp_invalid_,
                timestamp_discontinuity_};
    }

    BackpressurePolicy policy_;
    QueueBudget budget_;
    mutable std::mutex mutex_;
    std::condition_variable space_cv_;
    std::deque<QueueEntry> queue_;
    std::map<std::uint64_t, TimeBounds> time_bounds_;
    std::map<std::uint64_t, std::int64_t> latest_timestamp_by_generation_;
    std::size_t bytes_{0};
    std::int64_t span_us_{0};
    std::size_t items_high_watermark_{0};
    std::size_t bytes_high_watermark_{0};
    std::int64_t span_high_watermark_us_{0};
    std::uint64_t limit_items_{0};
    std::uint64_t limit_bytes_{0};
    std::uint64_t limit_span_{0};
    std::uint64_t oversized_{0};
    bool high_watermark_active_{false};
    std::uint64_t high_watermark_enters_{0};
    std::uint64_t high_watermark_leaves_{0};
    std::uint64_t dropped_keyframes_{0};
    std::uint64_t timestamp_invalid_{0};
    std::uint64_t timestamp_discontinuity_{0};
    bool closed_{false};
    typename ITransport<T>::NotifyCallback notify_callback_;
    typename ITransport<T>::SendResultCallback send_result_callback_;
    typename ITransport<T>::CapacityReasonCallback capacity_reason_callback_;
};

template <typename T>
class ExecutorDispatchTransport final : public ITransport<T> {
public:
    using Consumer = std::function<void(T)>;
    using ResultConsumer = std::function<MailboxPushResult(T)>;

    /// 快照消费者和回调后解锁，再执行外部逻辑，避免回调重入自锁。
    MailboxPushResult Send(T value) override {
        Consumer consumer;
        ResultConsumer result_consumer;
        typename ITransport<T>::SendResultCallback result_callback;
        bool closed = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            closed = closed_;
            consumer = consumer_;
            result_consumer = result_consumer_;
            result_callback = send_result_callback_;
        }

        if (closed) {
            if (result_callback) {
                result_callback(MailboxPushResult::Closed);
            }
            return MailboxPushResult::Closed;
        }

        MailboxPushResult result = MailboxPushResult::Accepted;
        if (result_consumer) {
            result = result_consumer(std::move(value));
        } else if (consumer) {
            consumer(std::move(value));
        }
        if (result_callback) {
            result_callback(result);
        }
        return result;
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

    /// 设置可回传投递结果的消费者，供 Edge 把 Node Dispatch 的拒绝结果
    /// 传回 OutputPort，避免直投路径永远报告 Accepted。
    void SetResultConsumer(ResultConsumer consumer) {
        std::lock_guard<std::mutex> lock(mutex_);
        result_consumer_ = std::move(consumer);
    }

private:
    mutable std::mutex mutex_;
    bool closed_{false};
    Consumer consumer_;
    ResultConsumer result_consumer_;
    typename ITransport<T>::NotifyCallback notify_callback_;
    typename ITransport<T>::SendResultCallback send_result_callback_;
};

/// 兼容早期代码的名称；新代码应使用 ExecutorDispatchTransport。
template <typename T>
using DirectTransport = ExecutorDispatchTransport<T>;

} // namespace mediaflow
