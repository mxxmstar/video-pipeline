#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

/**
 * @file types.h
 * @brief MediaFlow 公共枚举、边配置和节点配置。
 *
 * 这些类型只描述调度和传输语义，不依赖具体媒体类型。这样 Graph 可以
 * 处理 int、MediaPacket、MediaFrame 或用户自定义消息，而不需要重复定义
 * 一套节点和队列接口。
 */
namespace mediaflow {

/// @brief 队列成本中表示“没有可靠媒体时间戳”的统一值。
///
/// 该值与媒体模块的 kNoTimestamp 使用相同数值，但核心模块不依赖具体媒体类型。
inline constexpr std::int64_t kNoQueueTimestamp =
    (std::numeric_limits<std::int64_t>::min)();

/// @brief 消息所属的媒体轨道，用于 Edge/Dispatch 总在途统计。
enum class QueueTrack {
    Unknown,
    Audio,
    Video,
};

/// @brief 单条消息占用队列预算的成本。
///
/// bytes 表示本队列对消息载荷的逻辑计费，不推断 shared_ptr 在进程中的全局引用数。
/// timestamp_us 和 duration_us 已统一换算为微秒；没有可靠时间戳时使用
/// kNoQueueTimestamp，队列仍会继续使用 items/bytes 两个维度限流。
struct QueueItemCost {
    std::size_t bytes{0};
    std::int64_t timestamp_us{kNoQueueTimestamp};
    std::int64_t duration_us{0};
    std::uint64_t generation{0};
    QueueTrack track{QueueTrack::Unknown};
};

/// @brief 队列的多维硬上限及水位配置。
///
/// 任一已启用维度达到上限都会触发对应背压策略。值为 0 表示该维度不启用；
/// EdgeOptions 未显式配置任何维度时，会退回兼容的 capacity 消息数上限。
struct QueueBudget {
    std::size_t max_items{0};
    std::size_t max_bytes{0};
    std::int64_t max_span_us{0};
    std::uint32_t high_watermark_percent{80};
    std::uint32_t low_watermark_percent{60};

    bool IsValid() const {
        return max_span_us >= 0 && high_watermark_percent <= 100 &&
               low_watermark_percent <= high_watermark_percent;
    }
};

/// @brief 本次入队因哪一种容量约束受到影响。
enum class QueueLimitReason {
    None,
    Items,
    Bytes,
    Span,
    Oversized,
};

/// @brief QueueTransport 的线程安全状态快照。
struct QueueMetricsSnapshot {
    std::size_t items{0};
    std::size_t bytes{0};
    std::int64_t span_us{0};
    std::size_t items_high_watermark{0};
    std::size_t bytes_high_watermark{0};
    std::int64_t span_high_watermark_us{0};
    std::uint64_t limit_items{0};
    std::uint64_t limit_bytes{0};
    std::uint64_t limit_span{0};
    std::uint64_t oversized{0};
};

/// 队列已满时的处理策略。
enum class BackpressurePolicy {
    Block,       ///< 等待消费者释放空间，适合不能丢失的控制消息。
    DropNewest,  ///< 丢弃刚到达的消息，适合不希望旧数据被覆盖的队列。
    DropOldest,  ///< 丢弃队首旧消息，适合低延迟视频帧队列。
    PreferVideoKeyframes, ///< 混合媒体队列满时优先保留视频，尤其是视频关键帧。
};

/// Transport::Send 的精确结果，用于业务判断和指标统计。
enum class MailboxPushResult {
    Accepted,       ///< 消息已进入队列或已交给下游。
    DroppedNewest,  ///< 当前消息被丢弃，队列内容未改变。
    DroppedOldest,  ///< 旧消息被替换，当前消息已进入队列。
    Closed,         ///< 通道已关闭，消息没有进入下游。
};

/// 节点之间采用的传输方式。
enum class TransportKind {
    Queue,             ///< 异步有界队列，发送方和接收方通过 Executor 解耦。
    ExecutorDispatch,  ///< 不缓存消息，但仍投递到下游 Executor。
    Direct = ExecutorDispatch,  ///< 兼容旧名称，实际语义是 ExecutorDispatch。
};

/// 节点内部任务的并发模型。
enum class NodeExecutionMode {
    Serialized,  ///< 同一节点同一时刻只执行一个任务，默认模式。
    Concurrent,  ///< 允许多个任务同时进入节点，要求业务组件自身线程安全。
};

/// 一条 Graph 边的传输和调度配置。
struct EdgeOptions {
    TransportKind transport{TransportKind::Queue};  ///< 边的传输类型。
    std::size_t capacity{64};                       ///< QueueTransport 的最大消息数。
    BackpressurePolicy backpressure{BackpressurePolicy::DropNewest};
    std::size_t max_batch_size{32};                 ///< 一次 Drain 最多提交的消息数。
    std::int64_t max_drain_time_us{0};              ///< Drain 时间预算，0 表示不限制。
    QueueBudget budget{};                            ///< 可选多维队列预算；全 0 时兼容 capacity。
    QueueTrack track{QueueTrack::Unknown};           ///< Edge 的轨道标签，用于总在途统计。
};

/// 节点内部 Dispatch 队列的配置。
struct NodeOptions {
    NodeExecutionMode execution_mode{NodeExecutionMode::Serialized};
    std::size_t max_pending_tasks{64};  ///< 有界任务数，防止慢节点无限积压。
    bool prefer_video_keyframes{false}; ///< 节点任务队列满时优先保护视频关键帧。
};

/// @brief 节点 Dispatch 队列对媒体任务的最小分类信息。
struct DispatchPriority {
    bool is_audio{false};
    bool is_video{false};
    bool is_keyframe{false};
    bool is_control{false};
};

/// NodeMetrics 的线程安全快照，避免调用方直接读取原子变量。
struct NodeMetricsSnapshot {
    std::uint64_t enqueued{0};          ///< 进入该节点输入边的消息数。
    std::uint64_t processed{0};         ///< 已成功执行的节点任务数。
    std::uint64_t dropped{0};            ///< 因背压被丢弃的消息数。
    std::uint64_t rejected{0};           ///< 因关闭或队列满而拒绝的消息数。
    std::uint64_t errors{0};             ///< 节点任务抛出异常的次数。
    std::size_t pending_tasks{0};        ///< 当前等待执行或正在执行的任务数。
    std::size_t max_pending_tasks{0};    ///< 从打开节点以来观察到的最大任务数。
    QueueMetricsSnapshot pending{};      ///< 当前 Node Dispatch 的总在途成本。
    QueueMetricsSnapshot audio_pending{};
    QueueMetricsSnapshot video_pending{};
    QueueMetricsSnapshot unknown_pending{};
};

/// Graph 构建或启动失败的原因，保留 bool API 同时提供可诊断信息。
enum class FlowErrorCode {
    None,
    InvalidArgument,
    InvalidOptions,
    DuplicateNode,
    GraphRunning,
    NodeRegistrationFailed,
    NodeNotFound,
    PortNotFound,
    PortDirectionMismatch,
    PortTypeMismatch,
    NodeInitFailed,
    ExecutorStartFailed,
    NodeStartFailed,
};

struct FlowError {
    FlowErrorCode code{FlowErrorCode::None};
    std::string message;

    bool Ok() const {
        return code == FlowErrorCode::None;
    }
};

/// 单条 Edge 的运行统计，避免所有拥塞都汇总到目标节点后无法定位。
struct EdgeMetricsSnapshot {
    std::uint64_t accepted{0};
    std::uint64_t dropped_newest{0};
    std::uint64_t dropped_oldest{0};
    std::uint64_t rejected{0};
    std::uint64_t schedules{0};
    std::uint64_t drain_batches{0};
    std::uint64_t drained{0};
    std::size_t queue_size{0};
    std::size_t queue_high_watermark{0};
    QueueMetricsSnapshot budget{};       ///< Edge 队列的 items/bytes/span 快照。
};

} // namespace mediaflow
