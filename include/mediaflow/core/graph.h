#pragma once

#include "mediaflow/core/executor.h"
#include "mediaflow/core/node.h"
#include "mediaflow/core/types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * @file graph.h
 * @brief MediaFlow 的节点上下文、边调度和 Graph 生命周期实现。
 *
 * 数据流经过 Edge 的 Transport 进入目标 NodeContext。Queue Edge 只向目标
 * Executor 投递一个 Drain 任务，Drain 再把有限批次的消息放入节点自己的
 * Dispatch 队列。这样可以同时控制边队列和节点任务队列，避免慢节点造成
 * 无界内存增长。
 */
namespace mediaflow {

/// 节点运行期间累积的原子计数器。
struct NodeMetrics {
    std::atomic<std::uint64_t> enqueued{0};  ///< 进入节点输入边的消息数。
    std::atomic<std::uint64_t> processed{0}; ///< 成功完成的节点任务数。
    std::atomic<std::uint64_t> dropped{0};   ///< 因背压丢弃的消息数。
    std::atomic<std::uint64_t> rejected{0};  ///< 关闭或任务队列满导致的拒绝数。
    std::atomic<std::uint64_t> errors{0};    ///< 业务 Handler 抛出异常的次数。
    std::atomic<std::size_t> pending_tasks{0};
    std::atomic<std::size_t> max_pending_tasks{0};

    /// 在不暴露原子变量的情况下生成一致的监控快照。
    NodeMetricsSnapshot Snapshot() const {
        return NodeMetricsSnapshot{
            enqueued.load(),
            processed.load(),
            dropped.load(),
            rejected.load(),
            errors.load(),
            pending_tasks.load(),
            max_pending_tasks.load(),
        };
    }
};

/// @brief 记录 Edge 或 Node Dispatch 中的媒体成本。
///
/// 一个 Node 可能同时接收多条 Edge，因此这里同时维护总量和按轨道的量；
/// 时间跨度只在同一 generation 内计算，避免重连后的时间戳互相污染。
class InFlightCostTracker final {
public:
    void Clear() {
        for (auto& state : states_) {
            state = State{};
        }
    }

    void Add(const QueueItemCost& cost) {
        AddToState(states_[0], cost);
        AddToState(states_[TrackIndex(cost.track)], cost);
    }

    void Remove(const QueueItemCost& cost) {
        RemoveFromState(states_[0], cost);
        RemoveFromState(states_[TrackIndex(cost.track)], cost);
    }

    QueueMetricsSnapshot Total() const { return Snapshot(states_[0]); }

    QueueMetricsSnapshot ForTrack(QueueTrack track) const {
        return Snapshot(states_[TrackIndex(track)]);
    }

private:
    struct Bounds {
        std::multiset<std::int64_t> starts;
        std::multiset<std::int64_t> ends;
    };

    struct State {
        std::size_t bytes{0};
        std::int64_t span_us{0};
        std::size_t high_items{0};
        std::size_t high_bytes{0};
        std::int64_t high_span_us{0};
        std::size_t items{0};
        std::map<std::uint64_t, Bounds> bounds;
    };

    static std::size_t TrackIndex(QueueTrack track) {
        switch (track) {
        case QueueTrack::Audio: return 2;
        case QueueTrack::Video: return 3;
        case QueueTrack::Unknown: return 1;
        }
        return 1;
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

    static void RecomputeSpan(State& state) {
        state.span_us = 0;
        for (const auto& [generation, bounds] : state.bounds) {
            (void)generation;
            if (!bounds.starts.empty() && !bounds.ends.empty()) {
                state.span_us = std::max(
                    state.span_us,
                    Difference(*bounds.starts.begin(), *bounds.ends.rbegin()));
            }
        }
    }

    static void AddToState(State& state, const QueueItemCost& cost) {
        ++state.items;
        const auto max_bytes = (std::numeric_limits<std::size_t>::max)();
        state.bytes = cost.bytes > max_bytes - state.bytes
                          ? max_bytes
                          : state.bytes + cost.bytes;
        if (cost.timestamp_us != kNoQueueTimestamp) {
            auto& bounds = state.bounds[cost.generation];
            bounds.starts.insert(cost.timestamp_us);
            bounds.ends.insert(EndTimestamp(cost));
        }
        RecomputeSpan(state);
        state.high_items = std::max(state.high_items, state.items);
        state.high_bytes = std::max(state.high_bytes, state.bytes);
        state.high_span_us = std::max(state.high_span_us, state.span_us);
    }

    static void RemoveFromState(State& state, const QueueItemCost& cost) {
        if (state.items > 0) --state.items;
        state.bytes = cost.bytes > state.bytes ? 0 : state.bytes - cost.bytes;
        if (cost.timestamp_us != kNoQueueTimestamp) {
            auto bounds_it = state.bounds.find(cost.generation);
            if (bounds_it != state.bounds.end()) {
                auto& bounds = bounds_it->second;
                auto start = bounds.starts.find(cost.timestamp_us);
                if (start != bounds.starts.end()) bounds.starts.erase(start);
                auto end = bounds.ends.find(EndTimestamp(cost));
                if (end != bounds.ends.end()) bounds.ends.erase(end);
                if (bounds.starts.empty()) state.bounds.erase(bounds_it);
            }
        }
        RecomputeSpan(state);
    }

    static QueueMetricsSnapshot Snapshot(const State& state) {
        return {state.items, state.bytes, state.span_us, state.high_items,
                state.high_bytes, state.high_span_us, 0, 0, 0, 0};
    }

    std::array<State, 4> states_;
};

/// 单条 Edge 的原子统计，统计范围独立于目标节点。
struct EdgeMetrics {
    std::atomic<std::uint64_t> accepted{0};
    std::atomic<std::uint64_t> dropped_newest{0};
    std::atomic<std::uint64_t> dropped_oldest{0};
    std::atomic<std::uint64_t> rejected{0};
    std::atomic<std::uint64_t> schedules{0};
    std::atomic<std::uint64_t> drain_batches{0};
    std::atomic<std::uint64_t> drained{0};
    std::atomic<std::size_t> queue_size{0};
    std::atomic<std::size_t> queue_high_watermark{0};
    std::atomic<std::size_t> queue_bytes{0};
    std::atomic<std::size_t> queue_bytes_high_watermark{0};
    std::atomic<std::int64_t> queue_span_us{0};
    std::atomic<std::int64_t> queue_span_high_watermark_us{0};

    /// 更新当前队列深度，并用 CAS 保留历史最高水位。
    void UpdateQueueSize(std::size_t size) {
        queue_size.store(size);
        auto previous = queue_high_watermark.load();
        while (size > previous &&
               !queue_high_watermark.compare_exchange_weak(previous, size)) {
        }
    }

    void UpdateQueueState(const QueueMetricsSnapshot& state) {
        UpdateQueueSize(state.items);
        queue_bytes.store(state.bytes);
        queue_span_us.store(state.span_us);

        auto previous_bytes = queue_bytes_high_watermark.load();
        while (state.bytes > previous_bytes &&
               !queue_bytes_high_watermark.compare_exchange_weak(
                   previous_bytes, state.bytes)) {
        }
        auto previous_span = queue_span_high_watermark_us.load();
        while (state.span_us > previous_span &&
               !queue_span_high_watermark_us.compare_exchange_weak(
                   previous_span, state.span_us)) {
        }
    }

    EdgeMetricsSnapshot Snapshot() const {
        EdgeMetricsSnapshot snapshot{
            accepted.load(),
            dropped_newest.load(),
            dropped_oldest.load(),
            rejected.load(),
            schedules.load(),
            drain_batches.load(),
            drained.load(),
            queue_size.load(),
            queue_high_watermark.load(),
        };
        snapshot.budget = QueueMetricsSnapshot{
            queue_size.load(), queue_bytes.load(), queue_span_us.load(),
            queue_high_watermark.load(), queue_bytes_high_watermark.load(),
            queue_span_high_watermark_us.load(), 0, 0, 0, 0};
        return snapshot;
    }
};

/**
 * @brief 节点的运行上下文。
 *
 * NodeContext 持有节点、执行器、端口表和任务队列。Serialized 模式下，
 * dispatch_active_ 保证同一时刻只有一个泵任务在 Executor 中，多个上游
 * Edge 的消息会按进入顺序依次执行。Concurrent 模式跳过内部串行队列，
 * 但仍通过 max_pending_tasks 限制任务总量。
 */
class NodeContext final : public std::enable_shared_from_this<NodeContext> {
public:
    using Task = std::function<void()>;

    NodeContext(std::string node_id,
                std::shared_ptr<INode> node,
                std::shared_ptr<IExecutor> executor,
                NodeOptions options)
        : id(std::move(node_id)),
          node(std::move(node)),
          executor(std::move(executor)),
          options(options),
          metrics(std::make_shared<NodeMetrics>()) {}

    /**
     * 将业务任务加入节点执行队列。
     *
     * 返回 false 表示节点已关闭、任务队列达到上限或 Executor 不再接收
     * 任务。调用者不应在 false 后继续假设消息会被处理。
     */
    bool Dispatch(Task task) {
        return Dispatch(std::move(task), DispatchPriority{}, QueueItemCost{});
    }

    /// 将带媒体分类的任务加入节点 Dispatch 队列。
    ///
    /// 当节点队列已满且启用视频优先策略时，视频任务可以替换尚未执行的
    /// 音频任务；这样 Edge Drain 不会把保护策略只停留在外层 Transport。
    bool Dispatch(Task task, DispatchPriority priority) {
        return Dispatch(std::move(task), priority, QueueItemCost{});
    }

    /// 将媒体成本随任务传入 Dispatch，统计 Edge 排空后的真实在途数据。
    bool Dispatch(Task task, DispatchPriority priority, QueueItemCost cost) {
        if (!task) {
            return false;
        }
        if (cost.track == QueueTrack::Unknown) {
            cost.track = priority.is_audio ? QueueTrack::Audio
                                           : priority.is_video
                                                 ? QueueTrack::Video
                                                 : QueueTrack::Unknown;
        }

        if (options.execution_mode == NodeExecutionMode::Concurrent) {
            {
                std::lock_guard<std::mutex> lock(dispatch_mutex_);
                // Concurrent 模式没有 deque，pending_tasks_ 同时承担有界计数职责。
                if (dispatch_closed_ || pending_tasks_.load() >= options.max_pending_tasks) {
                    metrics->rejected.fetch_add(1);
                    return false;
                }
                pending_tasks_.fetch_add(1);
                pending_costs_.Add(cost);
                UpdateHighWatermark();
            }

            auto self = shared_from_this();
            if (!executor || !executor->Post([self, task = std::move(task),
                                              cost]() mutable {
                    self->RunTask(std::move(task), cost);
                })) {
                std::lock_guard<std::mutex> lock(dispatch_mutex_);
                pending_tasks_.fetch_sub(1);
                pending_costs_.Remove(cost);
                metrics->rejected.fetch_add(1);
                return false;
            }
            return true;
        }

        bool should_schedule = false;
        {
            std::lock_guard<std::mutex> lock(dispatch_mutex_);
            if (dispatch_closed_) {
                metrics->rejected.fetch_add(1);
                return false;
            }

            if (pending_tasks_.load() >= options.max_pending_tasks) {
                if (priority.is_control) {
                    auto victim = std::find_if(
                        dispatch_queue_.begin(), dispatch_queue_.end(),
                        [](const DispatchTask& queued) {
                            return !queued.priority.is_control;
                        });
                    if (victim == dispatch_queue_.end()) {
                        metrics->rejected.fetch_add(1);
                        return false;
                    }
                    pending_costs_.Remove(victim->cost);
                    dispatch_queue_.erase(victim);
                    pending_tasks_.fetch_sub(1);
                    metrics->dropped.fetch_add(1);
                } else if (!options.prefer_video_keyframes || !priority.is_video) {
                    if (options.prefer_video_keyframes && priority.is_audio) {
                        metrics->dropped.fetch_add(1);
                    }
                    metrics->rejected.fetch_add(1);
                    return false;
                }

                if (!priority.is_control) {
                    auto victim = std::find_if(
                        dispatch_queue_.begin(), dispatch_queue_.end(),
                        [](const DispatchTask& queued) {
                            return queued.priority.is_audio &&
                                   !queued.priority.is_control;
                        });
                if (victim == dispatch_queue_.end() && priority.is_keyframe) {
                    victim = std::find_if(
                        dispatch_queue_.begin(), dispatch_queue_.end(),
                        [](const DispatchTask& queued) {
                            return queued.priority.is_video &&
                                   !queued.priority.is_control &&
                                   !queued.priority.is_keyframe;
                        });
                }
                if (victim == dispatch_queue_.end()) {
                    metrics->rejected.fetch_add(1);
                    return false;
                }
                const auto victim_cost = victim->cost;
                dispatch_queue_.erase(victim);
                // 被替换的任务仍占用一个 pending 槽位；先释放旧任务的计数，
                // 再在下面为新任务增加计数，保持队列长度与 pending_tasks_ 一致。
                pending_tasks_.fetch_sub(1);
                pending_costs_.Remove(victim_cost);
                metrics->dropped.fetch_add(1);
                }
            }

            dispatch_queue_.push_back(
                DispatchTask{std::move(task), priority, cost});
            pending_tasks_.fetch_add(1);
            pending_costs_.Add(cost);
            UpdateHighWatermark();
            if (!dispatch_active_) {
                dispatch_active_ = true;
                should_schedule = true;
            }
        }

        // 只有从“无泵任务”切换到“有泵任务”时才需要 Post，避免每条消息
        // 都向 Executor 投递一个 Drain 任务。
        if (should_schedule && !ScheduleSerialized()) {
            CloseDispatch();
            return false;
        }
        return true;
    }

    /// 打开节点任务入口，并清理上一个 Graph 生命周期的排队任务。
    void OpenDispatch() {
        std::lock_guard<std::mutex> lock(dispatch_mutex_);
        dispatch_queue_.clear();
        pending_tasks_.store(0);
        pending_costs_.Clear();
        dispatch_active_ = false;
        dispatch_closed_ = false;
    }

    /// 关闭节点任务入口，清理尚未开始的串行任务。
    void CloseDispatch() {
        std::size_t discarded = 0;
        {
            std::lock_guard<std::mutex> lock(dispatch_mutex_);
            dispatch_closed_ = true;
            discarded = dispatch_queue_.size();
            for (const auto& task : dispatch_queue_) {
                pending_costs_.Remove(task.cost);
            }
            dispatch_queue_.clear();
            dispatch_active_ = false;
        }
        if (discarded > 0) {
            pending_tasks_.fetch_sub(discarded);
        }
    }

    /// 返回当前节点尚未完成的业务任务数量，供 GracefulStop 建立停止屏障。
    std::size_t PendingTasks() const {
        return pending_tasks_.load();
    }

    NodeMetricsSnapshot MetricsSnapshot() const {
        auto snapshot = metrics->Snapshot();
        std::lock_guard<std::mutex> lock(dispatch_mutex_);
        snapshot.pending_tasks = pending_tasks_.load();
        snapshot.pending = pending_costs_.Total();
        snapshot.audio_pending = pending_costs_.ForTrack(QueueTrack::Audio);
        snapshot.video_pending = pending_costs_.ForTrack(QueueTrack::Video);
        snapshot.unknown_pending = pending_costs_.ForTrack(QueueTrack::Unknown);
        return snapshot;
    }

    const std::string id;                         ///< Graph 内唯一节点 ID。
    const std::shared_ptr<INode> node;             ///< 业务节点对象。
    const std::shared_ptr<IExecutor> executor;     ///< 节点任务使用的执行器。
    const NodeOptions options;                     ///< 节点并发和队列配置。
    const std::shared_ptr<NodeMetrics> metrics;    ///< 节点共享统计对象。
    std::unordered_map<std::string, PortBinding> ports;  ///< 已注册端口。

private:
    /// 向 Executor 投递一个“执行一个串行任务”的泵任务。
    bool ScheduleSerialized() {
        auto self = shared_from_this();
        return executor && executor->Post([self]() {
            self->RunSerializedOne();
        });
    }

    /// 取出一条任务执行，完成后按需安排下一条任务。
    void RunSerializedOne() {
        DispatchTask pending;
        {
            std::lock_guard<std::mutex> lock(dispatch_mutex_);
            if (dispatch_queue_.empty()) {
                dispatch_active_ = false;
                return;
            }
            pending = std::move(dispatch_queue_.front());
            dispatch_queue_.pop_front();
        }

        RunTask(std::move(pending.task), pending.cost);

        bool should_schedule = false;
        {
            std::lock_guard<std::mutex> lock(dispatch_mutex_);
            if (dispatch_queue_.empty() || dispatch_closed_) {
                dispatch_active_ = false;
            } else {
                should_schedule = true;
            }
        }

        if (should_schedule && !ScheduleSerialized()) {
            CloseDispatch();
        }
    }

    /// 统一执行任务并把异常转换成 metrics，不让一个节点异常打穿线程池。
    void RunTask(Task task, const QueueItemCost& cost) {
        try {
            task();
            metrics->processed.fetch_add(1);
        } catch (...) {
            metrics->errors.fetch_add(1);
        }
        std::lock_guard<std::mutex> lock(dispatch_mutex_);
        pending_tasks_.fetch_sub(1);
        pending_costs_.Remove(cost);
    }

    /// 使用 CAS 更新 pending task 的历史最高水位。
    void UpdateHighWatermark() {
        const auto current = pending_tasks_.load();
        auto previous = metrics->max_pending_tasks.load();
        while (current > previous &&
               !metrics->max_pending_tasks.compare_exchange_weak(previous, current)) {
        }
    }

    mutable std::mutex dispatch_mutex_;  ///< 保护 deque 和 Dispatch 状态。
    struct DispatchTask {
        Task task;
        DispatchPriority priority;
        QueueItemCost cost;
    };

    std::deque<DispatchTask> dispatch_queue_; ///< Serialized 模式的有界任务队列。
    std::atomic<std::size_t> pending_tasks_{0};
    InFlightCostTracker pending_costs_;
    bool dispatch_active_{false};         ///< 是否已有泵任务在 Executor 中。
    bool dispatch_closed_{true};          ///< 初始关闭，Graph::Start 时打开。
};

/// Edge 的非模板接口，允许 Graph 统一管理不同消息类型的边。
class IEdge : public std::enable_shared_from_this<IEdge> {
public:
    virtual ~IEdge() = default;
    virtual const std::string& Id() const = 0;
    virtual EdgeMetricsSnapshot Metrics() const = 0;
    virtual void Open() = 0;       ///< 打开并清空底层 Transport。
    virtual void Close() = 0;      ///< 关闭 Transport，拒绝新的发送。
    virtual bool Empty() const = 0;
    /// Edge 既没有排队消息，也没有正在执行的 Drain 任务时才算真正空闲。
    virtual bool IsIdle() const = 0;
    virtual void Schedule() = 0;   ///< 请求向目标 Executor 投递 Drain。
    virtual void Drain() = 0;      ///< 批量取出 Transport 消息并提交给节点。
};

/**
 * @brief 连接输出端口和目标输入端口的强类型 Edge。
 *
 * scheduled_ 是一个 CAS 门闩：同一条边即使短时间收到多次入队通知，
 * 也只会有一个 Drain 任务在目标 Executor 中排队。Drain 结束后先释放
 * 门闩，再检查队列是否仍有消息，覆盖“Drain 结束瞬间又有新消息入队”的
 * 竞态窗口。
 */
template <typename T>
class Edge final : public IEdge {
public:
    /// 构造 Transport 和发送结果统计回调；通知回调延后到 Initialize 绑定。
    Edge(std::shared_ptr<NodeContext> destination,
         InputPort<T>* input,
         EdgeOptions options)
        : destination_(std::move(destination)),
          input_(input),
          options_(options),
          metrics_(std::make_shared<EdgeMetrics>()) {
        if (options_.transport == TransportKind::ExecutorDispatch) {
            auto direct = std::make_shared<ExecutorDispatchTransport<T>>();
            direct->SetResultConsumer(
                [destination = destination_, input = input_,
                 edge_track = options_.track](T value) {
                // ExecutorDispatch 没有 Edge::Drain 这一层，必须在这里保留
                // 媒体分类，否则视频任务会退化为普通 Dispatch，无法使用
                // NodeContext 的视频优先保护策略。
                const DispatchPriority priority{
                    QueueItemTraits<T>::IsAudio(value),
                    QueueItemTraits<T>::IsVideo(value),
                    QueueItemTraits<T>::IsKeyframe(value),
                    QueueItemTraits<T>::IsControl(value),
                };
                auto cost = QueueItemTraits<T>::Cost(value);
                if (edge_track != QueueTrack::Unknown) cost.track = edge_track;
                const bool accepted = destination->Dispatch(
                    [input, value = std::move(value)]() mutable {
                        input->Receive(std::move(value));
                    },
                    priority, cost);
                // MailboxPushResult 没有单独的“Node 队列满”枚举；对发送方
                // 统一表现为当前消息被丢弃，NodeMetrics 仍保留 rejected 计数。
                return accepted ? MailboxPushResult::Accepted
                                : MailboxPushResult::DroppedNewest;
            });
            transport_ = std::move(direct);
        } else {
            transport_ = std::make_shared<QueueTransport<T>>(
                options_.capacity,
                options_.backpressure,
                options_.budget);
        }

        // Transport 会持有回调；回调不能再强引用 Transport，否则边销毁后会
        // 形成 transport -> callback -> transport 的环，导致队列和媒体载荷泄漏。
        std::weak_ptr<ITransport<T>> weak_transport = transport_;
        transport_->SetSendResultCallback([
                edge_metrics = metrics_,
                node_metrics = destination_->metrics,
                weak_transport](MailboxPushResult result) {
            switch (result) {
            case MailboxPushResult::Accepted:
                edge_metrics->accepted.fetch_add(1);
                node_metrics->enqueued.fetch_add(1);
                break;
            case MailboxPushResult::DroppedOldest:
                edge_metrics->accepted.fetch_add(1);
                edge_metrics->dropped_oldest.fetch_add(1);
                node_metrics->enqueued.fetch_add(1);
                node_metrics->dropped.fetch_add(1);
                break;
            case MailboxPushResult::DroppedNewest:
                edge_metrics->dropped_newest.fetch_add(1);
                node_metrics->dropped.fetch_add(1);
                break;
            case MailboxPushResult::Closed:
                edge_metrics->rejected.fetch_add(1);
                node_metrics->rejected.fetch_add(1);
                break;
            }
            if (auto transport = weak_transport.lock()) {
                edge_metrics->UpdateQueueState(transport->Metrics());
            }
        });
    }

    void SetId(std::string id) {
        edge_id_ = std::move(id);
    }

    const std::string& Id() const override {
        return edge_id_;
    }

    EdgeMetricsSnapshot Metrics() const override {
        return metrics_->Snapshot();
    }

    /// 把此边的 Transport 挂到源输出端口。
    void Attach(OutputPort<T>& output) {
        output.AddTransport(transport_);
    }

    /**
     * 绑定弱引用通知回调。
     *
     * 该步骤必须在 make_shared 完成后执行，不能放在构造函数中调用
     * weak_from_this()，否则得到的弱引用为空，消息永远不会触发 Drain。
     */
    void Initialize() {
        if (options_.transport == TransportKind::Queue) {
            transport_->SetNotifyCallback([weak = weak_from_this()]() {
                if (auto edge = weak.lock()) {
                    edge->Schedule();
                }
            });
        }
    }

    /// 打开边并清除上一个生命周期的排队消息。
    void Open() override {
        scheduled_.store(false);
        drain_cancelled_.store(false);
        transport_->Open();
        metrics_->UpdateQueueState(transport_->Metrics());
    }

    /// 关闭边并复位调度门闩。
    void Close() override {
        // Drain 可能正在同一个 Executor 上批量提交消息。先设置取消标志，
        // 让正在运行的 Drain 在下一轮检查时退出，再清空底层 Transport。
        drain_cancelled_.store(true);
        transport_->Close();
        scheduled_.store(false);
        metrics_->UpdateQueueState(transport_->Metrics());
    }

    bool Empty() const override {
        return transport_->Empty();
    }

    bool IsIdle() const override {
        return transport_->Empty() && !scheduled_.load();
    }

private:
    /// 只负责投递 Drain，不直接在发送线程执行消息处理。
    void Schedule() override {
        if (drain_cancelled_.load()) {
            scheduled_.store(false);
            return;
        }
        bool expected = false;
        if (!scheduled_.compare_exchange_strong(expected, true)) {
            return;
        }

        metrics_->schedules.fetch_add(1);
        auto self = shared_from_this();
        if (!destination_->executor || !destination_->executor->Post([self]() {
                self->Drain();
            })) {
            scheduled_.store(false);
            destination_->metrics->rejected.fetch_add(1);
        }
    }

    /// 在目标 Executor 上批量排空边队列。
    void Drain() override {
        if (options_.transport == TransportKind::Direct) {
            scheduled_.store(false);
            return;
        }

        const auto start_time = std::chrono::steady_clock::now();
        const auto max_batch_size = std::max<std::size_t>(1, options_.max_batch_size);
        std::size_t count = 0;
        metrics_->drain_batches.fetch_add(1);
        while (count < max_batch_size) {
            if (drain_cancelled_.load()) {
                break;
            }
            if (count > 0 && options_.max_drain_time_us > 0 &&
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start_time).count() >=
                    options_.max_drain_time_us) {
                break;
            }
            auto value = transport_->TryReceive();
            if (!value.has_value()) {
                break;
            }

            const DispatchPriority priority{
                QueueItemTraits<T>::IsAudio(*value),
                QueueItemTraits<T>::IsVideo(*value),
                QueueItemTraits<T>::IsKeyframe(*value),
                QueueItemTraits<T>::IsControl(*value),
            };
            auto cost = QueueItemTraits<T>::Cost(*value);
            if (options_.track != QueueTrack::Unknown) cost.track = options_.track;
            if (!destination_->Dispatch(
                    [input = input_, value = std::move(*value)]() mutable {
                        input->Receive(std::move(value));
                    },
                    priority, cost)) {
                destination_->metrics->rejected.fetch_add(1);
            }
            metrics_->drained.fetch_add(1);
            ++count;
        }

        metrics_->UpdateQueueState(transport_->Metrics());
        scheduled_.store(false);
        if (!drain_cancelled_.load() && !transport_->Empty()) {
            Schedule();
        }
    }

    std::shared_ptr<NodeContext> destination_;  ///< 目标节点上下文。
    InputPort<T>* input_;                       ///< 目标节点输入端口，不拥有其生命周期。
    EdgeOptions options_;                       ///< 边的队列和批处理配置。
    std::shared_ptr<ITransport<T>> transport_;  ///< 实际消息通道。
    std::string edge_id_;                       ///< 源端口到目标端口的稳定标识。
    std::shared_ptr<EdgeMetrics> metrics_;      ///< 本边独立统计。
    std::atomic<bool> scheduled_{false};        ///< 防止重复投递 Drain。
    std::atomic<bool> drain_cancelled_{false};  ///< 关闭阶段取消正在执行的 Drain。
};

/**
 * @brief 管理节点、边和执行器生命周期的有向图。
 *
 * Graph 构建阶段允许 AddNode/Connect；进入 Starting 后拓扑视为冻结。
 * Start 顺序为打开 Dispatch/边、初始化节点、启动执行器、启动节点。
 * Stop 顺序为停止节点、关闭边、关闭 Dispatch、停止执行器、反初始化节点。
 */
class Graph final {
public:
    /// Graph 的生命周期状态。
    enum class State {
        Created,
        Starting,
        Running,
        Stopping,
        Stopped,
    };

    /// 析构时执行幂等 Stop，确保拥有的节点和边被关闭。
    ~Graph() {
        Stop();
    }

    template <typename NodeType, typename... Args>
    /**
     * 创建并注册一个节点。
     *
     * NodeType 必须实现 INode，并在 RegisterPorts 中注册端口。Graph 保存
     * 节点的 shared_ptr，因此节点对象会一直存活到 Graph 销毁。
     */
    bool AddNode(const std::string& id,
                 std::shared_ptr<IExecutor> executor,
                 NodeOptions options,
                 Args&&... args) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Starting || state_ == State::Running ||
            state_ == State::Stopping || topology_frozen_) {
            SetErrorLocked(FlowErrorCode::GraphRunning,
                           "Graph topology is frozen or lifecycle is active");
            return false;
        }
        if (id.empty()) {
            SetErrorLocked(FlowErrorCode::InvalidArgument, "Node id must not be empty");
            return false;
        }
        if (nodes_.contains(id)) {
            SetErrorLocked(FlowErrorCode::DuplicateNode,
                           "Node id already exists: " + id);
            return false;
        }
        if (!executor) {
            SetErrorLocked(FlowErrorCode::InvalidArgument,
                           "Node executor must not be null: " + id);
            return false;
        }
        if (options.max_pending_tasks == 0) {
            SetErrorLocked(FlowErrorCode::InvalidOptions,
                           "max_pending_tasks must be greater than zero: " + id);
            return false;
        }

        auto node = std::make_shared<NodeType>(std::forward<Args>(args)...);
        auto context = std::make_shared<NodeContext>(id, node, std::move(executor), options);
        PortRegistry registry;
        if (!node->RegisterPorts(registry)) {
            SetErrorLocked(FlowErrorCode::NodeRegistrationFailed,
                           "Node port registration failed: " + id);
            return false;
        }

        context->ports = registry.Bindings();
        nodes_.emplace(id, std::move(context));
        node_order_.push_back(id);
        ClearErrorLocked();
        return true;
    }

    template <typename T>
    /// 连接默认 out -> in 端口。
    bool Connect(const std::string& source,
                 const std::string& destination,
                 EdgeOptions options = {}) {
        return Connect<T>(source, "out", destination, "in", options);
    }

    template <typename T>
    /// 按端口名称连接两个同类型端口，并在构建期校验方向和 typeid。
    bool Connect(const std::string& source,
                 const std::string& source_port,
                 const std::string& destination,
                 const std::string& destination_port,
                 EdgeOptions options = {}) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ == State::Starting || state_ == State::Running ||
            state_ == State::Stopping || topology_frozen_) {
            SetErrorLocked(FlowErrorCode::GraphRunning,
                           "Graph topology is frozen or lifecycle is active");
            return false;
        }
        const bool has_queue_limit = options.capacity > 0 ||
            options.budget.max_items > 0 || options.budget.max_bytes > 0 ||
            options.budget.max_span_us > 0;
        if (options.max_batch_size == 0 || options.max_drain_time_us < 0 ||
            !options.budget.IsValid() ||
            (options.transport == TransportKind::Queue && !has_queue_limit)) {
            SetErrorLocked(FlowErrorCode::InvalidOptions,
                           "Edge capacity, batch size and drain budget are invalid");
            return false;
        }

        auto source_it = nodes_.find(source);
        auto destination_it = nodes_.find(destination);
        if (source_it == nodes_.end() || destination_it == nodes_.end()) {
            SetErrorLocked(FlowErrorCode::NodeNotFound,
                           "Edge endpoint node does not exist");
            return false;
        }

        const auto* source_binding = FindPort(*source_it->second, source_port);
        const auto* destination_binding = FindPort(*destination_it->second, destination_port);
        if (!source_binding || !destination_binding) {
            SetErrorLocked(FlowErrorCode::PortNotFound,
                           "Edge endpoint port does not exist");
            return false;
        }
        if (source_binding->direction != PortDirection::Output ||
            destination_binding->direction != PortDirection::Input) {
            SetErrorLocked(FlowErrorCode::PortDirectionMismatch,
                           "Edge endpoint directions are incompatible");
            return false;
        }
        if (source_binding->type != typeid(T) ||
            destination_binding->type != typeid(T)) {
            SetErrorLocked(FlowErrorCode::PortTypeMismatch,
                           "Edge endpoint message types are incompatible");
            return false;
        }

        auto* output = static_cast<OutputPort<T>*>(source_binding->port);
        auto* input = static_cast<InputPort<T>*>(destination_binding->port);
        auto edge = std::make_shared<Edge<T>>(destination_it->second, input, options);
        edge->SetId(source + ":" + source_port + "->" + destination + ":" + destination_port);
        edge->Initialize();
        edge->Attach(*output);
        edges_.push_back(std::move(edge));
        ClearErrorLocked();
        return true;
    }

    /**
     * 启动整张 Graph。
     *
     * 任何 Init、Executor::Start 或 Node::Start 失败都会调用 Rollback，
     * 已完成的阶段按逆序撤销，避免部分启动留下线程或业务资源。
     */
    bool Start() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == State::Starting || state_ == State::Running ||
                state_ == State::Stopping) {
                SetErrorLocked(FlowErrorCode::GraphRunning,
                               "Graph is already starting, running, or stopping");
                return false;
            }
            if (nodes_.empty()) {
                SetErrorLocked(FlowErrorCode::InvalidArgument,
                               "Graph must contain at least one node");
                return false;
            }
            state_ = State::Starting;
            for (const auto& id : node_order_) {
                nodes_.at(id)->OpenDispatch();
            }
            for (auto& edge : edges_) {
                edge->Open();
            }
        }

        std::vector<std::shared_ptr<NodeContext>> initialized;
        for (const auto& id : node_order_) {
            auto context = nodes_.at(id);
            if (!context->node->Init()) {
                SetError(FlowErrorCode::NodeInitFailed,
                         "Node Init failed: " + id);
                Rollback(initialized, {}, {});
                return false;
            }
            initialized.push_back(std::move(context));
        }

        std::vector<std::shared_ptr<IExecutor>> started_executors;
        std::unordered_set<IExecutor*> seen_executors;
        for (const auto& id : node_order_) {
            auto executor = nodes_.at(id)->executor;
            if (seen_executors.insert(executor.get()).second) {
                if (!executor->Start()) {
                    SetError(FlowErrorCode::ExecutorStartFailed,
                             "Executor Start failed for node: " + id);
                    Rollback(initialized, started_executors, {});
                    return false;
                }
                started_executors.push_back(std::move(executor));
            }
        }

        // 启动顺序按节点优先级稳定排序。默认节点先启动，SourceNode 最后
        // 启动，这样 Source 的第一条消息不会早于 Decoder/Sink 的准备完成。
        std::vector<std::shared_ptr<NodeContext>> start_order;
        start_order.reserve(node_order_.size());
        for (const auto& id : node_order_) {
            start_order.push_back(nodes_.at(id));
        }
        std::stable_sort(start_order.begin(), start_order.end(),
                         [](const auto& left, const auto& right) {
                             return left->node->StartPriority() <
                                    right->node->StartPriority();
                         });

        std::vector<std::shared_ptr<NodeContext>> started_nodes;
        for (const auto& context : start_order) {
            if (!context->node->Start()) {
                SetError(FlowErrorCode::NodeStartFailed,
                         "Node Start failed: " + context->id);
                Rollback(initialized, started_executors, started_nodes);
                return false;
            }
            started_nodes.push_back(std::move(context));
        }

        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State::Running;
        topology_frozen_ = true;
        ClearErrorLocked();
        return true;
    }

    /**
     * 立即停止整张 Graph。
     *
     * 该接口会跳过排空和 Flush，关闭边后清除尚未执行的 Dispatch 任务，
     * 适合发生致命错误或调用方不需要保留尾部媒体数据的场景。需要完整
     * 传播 EOS、输出 Decoder/Encoder 尾数据时应使用 GracefulStop。
     */
    void Stop() {
        (void)StopImpl(false, std::chrono::milliseconds(0));
    }

    /**
     * 以有序方式停止 Graph。
     *
     * 该流程先停止 Source 继续生产，再等待所有 Edge 和节点任务排空；随后
     * 按节点注册顺序逐级 Flush，确保 Decoder 的残留帧先进入 Encoder，Encoder
     * 的残留 packet 再进入 Publisher。超时后仍会走资源回收，但会返回 false，
     * 调用方可以把它作为尾部数据不完整的明确告警。
     */
    bool GracefulStop(
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        return StopImpl(true, timeout);
    }

private:
    bool StopImpl(bool graceful, std::chrono::milliseconds timeout) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (state_ == State::Created || state_ == State::Stopped ||
                state_ == State::Stopping) {
                return true;
            }
            state_ = State::Stopping;
        }

        bool graceful_completed = true;
        if (graceful) {
            // 生产者停止本身也属于停止过程的一部分；截止时间必须在调用
            // StopProduction 前建立，不能只限制后面的排空/Flush 阶段。
            const auto graceful_deadline =
                std::chrono::steady_clock::now() + timeout;

            // 只停止生产者，不能在这里让 Decoder/Encoder 拒绝已经入队的消息。
            for (const auto& id : node_order_) {
                nodes_.at(id)->node->StopProduction();
            }

            // timeout 是整次 GracefulStop 的总预算，而不是每个节点各自拥有一
            // 份预算。否则多个 Decoder/Encoder 会让停止时间按节点数量累加。
            graceful_completed = WaitForQuiescence(graceful_deadline);
            if (graceful_completed) {
                // 节点注册顺序就是媒体链路的构建顺序；媒体节点按此顺序逐级
                // 刷新，保证上游刚产生的尾部消息仍有机会进入下游屏障。
                for (const auto& id : node_order_) {
                    if (!nodes_.at(id)->node->Flush()) {
                        graceful_completed = false;
                        break;
                    }
                    if (!WaitForQuiescence(graceful_deadline)) {
                        graceful_completed = false;
                        break;
                    }
                }
            }
        }

        // 停止顺序与启动顺序相反：Source 先停止生产，随后才让路由和解码
        // 节点拒绝新消息。这样 Immediate Stop 不会因为下游先停而把源线程
        // 留在持续发送的状态里。
        std::vector<std::shared_ptr<NodeContext>> stop_order;
        stop_order.reserve(node_order_.size());
        for (const auto& id : node_order_) {
            stop_order.push_back(nodes_.at(id));
        }
        std::stable_sort(stop_order.begin(), stop_order.end(),
                         [](const auto& left, const auto& right) {
                             return left->node->StartPriority() >
                                    right->node->StartPriority();
                         });
        for (const auto& context : stop_order) {
            context->node->Stop();
        }
        for (auto& edge : edges_) {
            edge->Close();
        }
        for (const auto& id : node_order_) {
            nodes_.at(id)->CloseDispatch();
        }

        std::unordered_set<IExecutor*> seen_executors;
        for (const auto& id : node_order_) {
            auto executor = nodes_.at(id)->executor;
            if (seen_executors.insert(executor.get()).second) {
                executor->Stop();
            }
        }

        for (auto it = node_order_.rbegin(); it != node_order_.rend(); ++it) {
            nodes_.at(*it)->node->Deinit();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State::Stopped;
        return graceful_completed;
    }

public:
    /// 获取节点对象，主要用于监控、测试和上层控制接口。
    template <typename NodeType>
    std::shared_ptr<NodeType> GetNode(const std::string& id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodes_.find(id);
        if (it == nodes_.end()) {
            return nullptr;
        }
        return std::dynamic_pointer_cast<NodeType>(it->second->node);
    }

    /// 线程安全地读取 Graph 状态。
    State GetState() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return state_;
    }

    /// 获取指定节点的线程安全指标快照。
    bool GetMetrics(const std::string& id, NodeMetricsSnapshot& snapshot) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = nodes_.find(id);
        if (it == nodes_.end()) {
            return false;
        }
        snapshot = it->second->MetricsSnapshot();
        return true;
    }

    /// 获取指定 Edge 的独立统计快照。
    bool GetEdgeMetrics(const std::string& id, EdgeMetricsSnapshot& snapshot) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& edge : edges_) {
            if (edge->Id() == id) {
                snapshot = edge->Metrics();
                return true;
            }
        }
        return false;
    }

    /// 获取最近一次构图或启动失败的结构化原因。
    FlowError LastError() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_;
    }

private:
    /// 等待节点任务和 Edge Drain 同时归零，且不超过整次停止的截止时间。
    bool WaitForQuiescence(
        std::chrono::steady_clock::time_point deadline) const {
        for (;;) {
            bool idle = true;
            for (const auto& id : node_order_) {
                if (!nodes_.at(id)->node->IsProductionStopped() ||
                    nodes_.at(id)->PendingTasks() != 0) {
                    idle = false;
                    break;
                }
            }
            if (idle) {
                for (const auto& edge : edges_) {
                    if (!edge->IsIdle()) {
                        idle = false;
                        break;
                    }
                }
            }
            if (idle) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    /// 立即停止时仍复用同一套清理逻辑，区别只在于是否执行屏障和 Flush。
    void SetError(FlowErrorCode code, std::string message) {
        std::lock_guard<std::mutex> lock(mutex_);
        SetErrorLocked(code, std::move(message));
    }

    void SetErrorLocked(FlowErrorCode code, std::string message) {
        last_error_.code = code;
        last_error_.message = std::move(message);
    }

    void ClearErrorLocked() {
        last_error_ = FlowError{};
    }

    /// 从 NodeContext 中查找端口，连接失败时返回空指针。
    static const PortBinding* FindPort(const NodeContext& context,
                                       const std::string& name) {
        auto it = context.ports.find(name);
        return it == context.ports.end() ? nullptr : &it->second;
    }

    /// 撤销 Start 已经完成的阶段，并把 Graph 置为可再次 Start 的 Stopped。
    void Rollback(const std::vector<std::shared_ptr<NodeContext>>& initialized,
                  const std::vector<std::shared_ptr<IExecutor>>& started_executors,
                  const std::vector<std::shared_ptr<NodeContext>>& started_nodes) {
        for (auto it = started_nodes.rbegin(); it != started_nodes.rend(); ++it) {
            (*it)->node->Stop();
        }
        for (auto it = started_executors.rbegin(); it != started_executors.rend(); ++it) {
            (*it)->Stop();
        }
        for (auto it = initialized.rbegin(); it != initialized.rend(); ++it) {
            (*it)->node->Deinit();
        }
        for (auto& edge : edges_) {
            edge->Close();
        }
        for (const auto& id : node_order_) {
            nodes_.at(id)->CloseDispatch();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = State::Stopped;
    }

    mutable std::mutex mutex_;  ///< 保护状态和拓扑容器。
    State state_{State::Created};  ///< 当前生命周期状态。
    bool topology_frozen_{false};  ///< 首次成功启动后禁止修改节点和边。
    FlowError last_error_;         ///< 最近一次失败原因。
    std::unordered_map<std::string, std::shared_ptr<NodeContext>> nodes_;
    std::vector<std::string> node_order_;  ///< 保证生命周期顺序稳定可控。
    std::vector<std::shared_ptr<IEdge>> edges_;  ///< Graph 拥有的全部连接边。
};

} // namespace mediaflow
