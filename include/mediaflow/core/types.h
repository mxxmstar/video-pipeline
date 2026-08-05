#pragma once

#include <cstddef>
#include <cstdint>

/**
 * @file types.h
 * @brief MediaFlow 公共枚举、边配置和节点配置。
 *
 * 这些类型只描述调度和传输语义，不依赖具体媒体类型。这样 Graph 可以
 * 处理 int、MediaPacket、MediaFrame 或用户自定义消息，而不需要重复定义
 * 一套节点和队列接口。
 */
namespace mediaflow {

/// 队列已满时的处理策略。
enum class BackpressurePolicy {
    Block,       ///< 等待消费者释放空间，适合不能丢失的控制消息。
    DropNewest,  ///< 丢弃刚到达的消息，适合不希望旧数据被覆盖的队列。
    DropOldest,  ///< 丢弃队首旧消息，适合低延迟视频帧队列。
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
    Queue,   ///< 异步有界队列，发送方和接收方通过 Executor 解耦。
    Direct,  ///< 不缓存消息，发送时直接调用下游投递函数。
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
};

/// 节点内部 Dispatch 队列的配置。
struct NodeOptions {
    NodeExecutionMode execution_mode{NodeExecutionMode::Serialized};
    std::size_t max_pending_tasks{64};  ///< 有界任务数，防止慢节点无限积压。
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
};

} // namespace mediaflow
