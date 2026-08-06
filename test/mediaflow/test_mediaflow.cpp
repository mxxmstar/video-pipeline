#include "mediaflow/mediaflow.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace mediaflow;

// 最小源节点：在 Start 阶段产生一组整数，验证 SourceNode 到 Edge 的发送路径。
class NumberSource final : public SourceNode<int> {
public:
    explicit NumberSource(int count) : count_(count) {}

    std::string Name() const override {
        return "number-source";
    }

    bool Start() override {
        for (int value = 1; value <= count_; ++value) {
            Emit(value);
        }
        return true;
    }

private:
    int count_;
};

class DoubleNode final : public TransformNode<int, int> {
public:
    std::string Name() const override {
        return "double";
    }

protected:
    void Process(int value) override {
        Emit(value * 2);
    }
};

class CollectSink final : public SinkNode<int> {
public:
    std::string Name() const override {
        return "collector";
    }

    bool WaitFor(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(2), [this, count]() {
            return values_.size() >= count;
        });
    }

    std::vector<int> Values() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return values_;
    }

protected:
    void Process(int value) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            values_.push_back(value);
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<int> values_;
};

/// 只有生命周期行为的测试节点，用来验证 Graph 的阶段性回滚顺序。
struct LifecycleCounters {
    bool init_result{true};
    bool start_result{true};
    int init_calls{0};
    int start_calls{0};
    int stop_calls{0};
    int deinit_calls{0};
};

class LifecycleNode final : public INode {
public:
    explicit LifecycleNode(std::shared_ptr<LifecycleCounters> counters)
        : counters_(std::move(counters)) {}

    std::string Name() const override {
        return "lifecycle";
    }

    bool RegisterPorts(PortRegistry&) override {
        // 生命周期测试不需要数据端口，空注册表仍是合法节点。
        return true;
    }

    bool Init() override {
        ++counters_->init_calls;
        return counters_->init_result;
    }

    bool Start() override {
        ++counters_->start_calls;
        return counters_->start_result;
    }

    void Stop() override {
        ++counters_->stop_calls;
    }

    void Deinit() override {
        ++counters_->deinit_calls;
    }

private:
    std::shared_ptr<LifecycleCounters> counters_;
};

/// 启动阶段主动失败的执行器，用于验证 Graph 不会遗漏已初始化节点的清理。
class FailingExecutor final : public IExecutor {
public:
    bool Start() override {
        ++start_calls;
        return false;
    }

    void Stop() override {
        ++stop_calls;
    }

    bool Post(Task) override {
        return false;
    }

    bool IsRunning() const override {
        return false;
    }

    int start_calls{0};
    int stop_calls{0};
};

/// 具有 long 类型端口的节点，用于验证 Graph::Connect 的运行时类型检查。
class LongSource final : public SourceNode<long> {
public:
    std::string Name() const override {
        return "long-source";
    }
};

class LongSink final : public SinkNode<long> {
public:
    std::string Name() const override {
        return "long-sink";
    }

protected:
    void Process(long) override {}
};

/// 可让测试等待第一个消息进入 Handler 的节点，用于制造确定的背压场景。
class BlockingSink final : public SinkNode<int> {
public:
    std::string Name() const override {
        return "blocking-sink";
    }

    bool WaitUntilEntered() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(2), [this]() {
            return entered_;
        });
    }

    void Release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        cv_.notify_all();
    }

protected:
    void Process(int) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            entered_ = true;
        }
        cv_.notify_all();

        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() {
            return released_;
        });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool entered_{false};
    bool released_{false};
};

template <typename Predicate>
bool WaitUntil(Predicate predicate,
               std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

void TestQueueBackpressure() {
    // DropNewest 保留队列中已有的旧消息，拒绝新消息。
    QueueTransport<int> drop_newest(2, BackpressurePolicy::DropNewest);
    assert(drop_newest.Send(1) == MailboxPushResult::Accepted);
    assert(drop_newest.Send(2) == MailboxPushResult::Accepted);
    assert(drop_newest.Send(3) == MailboxPushResult::DroppedNewest);
    assert(drop_newest.Size() == 2);

    // DropOldest 淘汰最早进入队列的消息，让最新数据及时进入管线。
    QueueTransport<int> drop_oldest(2, BackpressurePolicy::DropOldest);
    assert(drop_oldest.Send(1) == MailboxPushResult::Accepted);
    assert(drop_oldest.Send(2) == MailboxPushResult::Accepted);
    assert(drop_oldest.Send(3) == MailboxPushResult::DroppedOldest);
    assert(drop_oldest.TryReceive().value() == 2);
    assert(drop_oldest.TryReceive().value() == 3);

    // Close 需要清空尚未处理的消息，并让后续发送明确返回 Closed。
    QueueTransport<int> close_transport(2, BackpressurePolicy::DropNewest);
    assert(close_transport.Send(10) == MailboxPushResult::Accepted);
    assert(close_transport.Send(11) == MailboxPushResult::Accepted);
    close_transport.Close();
    assert(close_transport.Empty());
    assert(close_transport.Size() == 0);
    assert(close_transport.Send(12) == MailboxPushResult::Closed);

    // Open 重新打开同一个 Transport 后，必须可以承载新的生命周期。
    close_transport.Open();
    assert(close_transport.Send(13) == MailboxPushResult::Accepted);
    assert(close_transport.TryReceive().value() == 13);
}

void TestVideoPriorityBackpressure() {
    auto make_message = [](MediaType type, bool keyframe, int64_t pts) {
        auto packet = std::make_shared<MediaPacket>();
        packet->type = type;
        packet->codec = type == MediaType::VIDEO ? CodecType::H264
                                                  : CodecType::AAC;
        packet->stream_index = type == MediaType::VIDEO ? 1 : 2;
        packet->pts = pts;
        packet->dts = pts;
        packet->keyframe = keyframe;
        MediaPacketMessage message;
        message.packet = std::move(packet);
        message.generation = 1;
        return message;
    };

    QueueTransport<MediaPacketMessage> queue(
        3, BackpressurePolicy::PreferVideoKeyframes);
    assert(queue.Send(make_message(MediaType::AUDIO, false, 1)) ==
           MailboxPushResult::Accepted);
    assert(queue.Send(make_message(MediaType::AUDIO, false, 2)) ==
           MailboxPushResult::Accepted);
    assert(queue.Send(make_message(MediaType::VIDEO, false, 3)) ==
           MailboxPushResult::Accepted);

    // 队列满时，视频包应淘汰音频，而不是被音频突发直接丢弃。
    assert(queue.Send(make_message(MediaType::VIDEO, true, 4)) ==
           MailboxPushResult::DroppedOldest);
    // 音频突发不会继续侵占已经为视频保留的队列空间。
    assert(queue.Send(make_message(MediaType::AUDIO, false, 5)) ==
           MailboxPushResult::DroppedNewest);
    assert(queue.Send(make_message(MediaType::VIDEO, true, 6)) ==
           MailboxPushResult::DroppedOldest);

    int video_count = 0;
    int keyframe_count = 0;
    while (auto message = queue.TryReceive()) {
        assert(message->packet);
        if (message->packet->type == MediaType::VIDEO) {
            ++video_count;
            keyframe_count += message->packet->keyframe ? 1 : 0;
        }
    }
    assert(video_count == 3);
    assert(keyframe_count == 2);
}

void TestVideoPriorityDispatchQueue() {
    auto executor = std::make_shared<AsioExecutor>("priority-dispatch", 1);
    NodeOptions options;
    options.max_pending_tasks = 3;
    options.prefer_video_keyframes = true;
    auto context = std::make_shared<NodeContext>(
        "priority-node",
        std::make_shared<LongSink>(),
        executor,
        options);
    context->OpenDispatch();
    assert(executor->Start());

    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool released = false;
    assert(context->Dispatch([&]() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&]() { return released; });
    }));
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(condition.wait_for(lock, std::chrono::seconds(2), [&]() {
            return entered;
        }));
    }

    const DispatchPriority audio_priority{true, false, false};
    const DispatchPriority keyframe_priority{false, true, true};
    assert(context->Dispatch([]() {}, audio_priority));
    assert(context->Dispatch([]() {}, audio_priority));
    // 第三个槽位到达时，视频关键帧应替换尚未执行的音频任务。
    assert(context->Dispatch([]() {}, keyframe_priority));
    // 新的音频不能再挤掉队列中已经保留的视频任务。
    assert(!context->Dispatch([]() {}, audio_priority));

    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
    }
    condition.notify_all();
    assert(WaitUntil([&]() { return context->PendingTasks() == 0; }));
    context->CloseDispatch();
    executor->Stop();
}

void TestExecutorDispatchTransport() {
    // 该 Transport 不保存中间队列，但仍由消费者回调决定下游投递方式。
    ExecutorDispatchTransport<int> dispatch;
    int received = 0;
    dispatch.SetConsumer([&received](int value) {
        received = value;
    });
    assert(dispatch.Send(7) == MailboxPushResult::Accepted);
    assert(received == 7);
    assert(dispatch.Empty());

    dispatch.Close();
    assert(dispatch.Send(8) == MailboxPushResult::Closed);
    dispatch.Open();
    assert(dispatch.Send(9) == MailboxPushResult::Accepted);
    assert(received == 9);
}

void TestGraphValidationAndTopologyFreeze() {
    auto executor = std::make_shared<AsioExecutor>("validation", 1);
    Graph graph;

    NodeOptions invalid_node_options;
    invalid_node_options.max_pending_tasks = 0;
    assert(!graph.AddNode<LifecycleNode>("invalid-options", executor,
                                         invalid_node_options,
                                         std::make_shared<LifecycleCounters>()));
    assert(graph.LastError().code == FlowErrorCode::InvalidOptions);

    assert(!graph.AddNode<LifecycleNode>("", executor, NodeOptions{},
                                         std::make_shared<LifecycleCounters>()));
    assert(graph.LastError().code == FlowErrorCode::InvalidArgument);

    assert(graph.AddNode<NumberSource>("source", executor, NodeOptions{}, 0));
    assert(!graph.AddNode<NumberSource>("source", executor, NodeOptions{}, 0));
    assert(graph.LastError().code == FlowErrorCode::DuplicateNode);
    assert(graph.AddNode<CollectSink>("sink", executor, NodeOptions{}));
    assert(graph.AddNode<LongSource>("long-source", executor, NodeOptions{}));
    assert(graph.AddNode<LongSink>("long-sink", executor, NodeOptions{}));

    assert(!graph.Connect<int>("missing", "sink"));
    assert(graph.LastError().code == FlowErrorCode::NodeNotFound);
    assert(!graph.Connect<int>("source", "missing-port", "sink", "in"));
    assert(graph.LastError().code == FlowErrorCode::PortNotFound);
    assert(!graph.Connect<int>("sink", "in", "source", "out"));
    assert(graph.LastError().code == FlowErrorCode::PortDirectionMismatch);
    assert(!graph.Connect<int>("long-source", "long-sink"));
    assert(graph.LastError().code == FlowErrorCode::PortTypeMismatch);

    EdgeOptions invalid_edge_options;
    invalid_edge_options.capacity = 0;
    assert(!graph.Connect<int>("source", "sink", invalid_edge_options));
    assert(graph.LastError().code == FlowErrorCode::InvalidOptions);
    invalid_edge_options.capacity = 1;
    invalid_edge_options.max_batch_size = 0;
    assert(!graph.Connect<int>("source", "sink", invalid_edge_options));
    assert(graph.LastError().code == FlowErrorCode::InvalidOptions);
    invalid_edge_options.max_batch_size = 1;
    invalid_edge_options.max_drain_time_us = -1;
    assert(!graph.Connect<int>("source", "sink", invalid_edge_options));
    assert(graph.LastError().code == FlowErrorCode::InvalidOptions);

    assert(graph.Connect<int>("source", "sink"));
    assert(graph.Start());
    assert(graph.LastError().Ok());

    // 首次成功启动后拓扑冻结，运行中和停止后都不能悄悄追加节点或边。
    assert(!graph.AddNode<LifecycleNode>("late-node", executor, NodeOptions{},
                                         std::make_shared<LifecycleCounters>()));
    assert(graph.LastError().code == FlowErrorCode::GraphRunning);
    assert(!graph.Connect<int>("source", "sink"));
    assert(graph.LastError().code == FlowErrorCode::GraphRunning);
    graph.Stop();
    assert(!graph.AddNode<LifecycleNode>("after-stop", executor, NodeOptions{},
                                         std::make_shared<LifecycleCounters>()));
    assert(graph.LastError().code == FlowErrorCode::GraphRunning);
}

void TestGraphErrorRollback() {
    // Init 失败时，只对已经成功 Init 的节点执行 Deinit，执行器不应启动。
    auto init_executor = std::make_shared<AsioExecutor>("init-rollback", 1);
    auto initialized = std::make_shared<LifecycleCounters>();
    auto init_failed = std::make_shared<LifecycleCounters>();
    init_failed->init_result = false;
    Graph init_graph;
    assert(init_graph.AddNode<LifecycleNode>("initialized", init_executor,
                                             NodeOptions{}, initialized));
    assert(init_graph.AddNode<LifecycleNode>("init-failed", init_executor,
                                             NodeOptions{}, init_failed));
    assert(!init_graph.Start());
    assert(init_graph.LastError().code == FlowErrorCode::NodeInitFailed);
    assert(init_graph.GetState() == Graph::State::Stopped);
    assert(!init_executor->IsRunning());
    assert(initialized->init_calls == 1);
    assert(initialized->deinit_calls == 1);
    assert(initialized->stop_calls == 0);
    assert(init_failed->init_calls == 1);
    assert(init_failed->deinit_calls == 0);

    // 失败原因修复后，原 Graph 仍可重新启动，说明回滚没有冻结拓扑。
    init_failed->init_result = true;
    assert(init_graph.Start());
    init_graph.Stop();

    // Node Start 失败时，已启动节点需要逆序 Stop，所有已 Init 节点都要 Deinit。
    auto start_executor = std::make_shared<AsioExecutor>("start-rollback", 1);
    auto started = std::make_shared<LifecycleCounters>();
    auto start_failed = std::make_shared<LifecycleCounters>();
    start_failed->start_result = false;
    Graph start_graph;
    assert(start_graph.AddNode<LifecycleNode>("started", start_executor,
                                              NodeOptions{}, started));
    assert(start_graph.AddNode<LifecycleNode>("start-failed", start_executor,
                                              NodeOptions{}, start_failed));
    assert(!start_graph.Start());
    assert(start_graph.LastError().code == FlowErrorCode::NodeStartFailed);
    assert(start_graph.GetState() == Graph::State::Stopped);
    assert(!start_executor->IsRunning());
    assert(started->stop_calls == 1);
    assert(started->deinit_calls == 1);
    assert(start_failed->stop_calls == 0);
    assert(start_failed->deinit_calls == 1);

    // Executor Start 失败时，已完成 Init 的节点仍必须执行 Deinit。
    auto failing_executor = std::make_shared<FailingExecutor>();
    auto executor_failed = std::make_shared<LifecycleCounters>();
    Graph executor_graph;
    assert(executor_graph.AddNode<LifecycleNode>("executor-failed",
                                                 failing_executor,
                                                 NodeOptions{}, executor_failed));
    assert(!executor_graph.Start());
    assert(executor_graph.LastError().code == FlowErrorCode::ExecutorStartFailed);
    assert(executor_graph.GetState() == Graph::State::Stopped);
    assert(failing_executor->start_calls == 1);
    assert(failing_executor->stop_calls == 0);
    assert(executor_failed->init_calls == 1);
    assert(executor_failed->deinit_calls == 1);
}

void TestEdgeMetricsAndBoundedDispatch() {
    auto metric_executor = std::make_shared<AsioExecutor>("edge-metrics", 1);
    Graph metric_graph;
    assert(metric_graph.AddNode<NumberSource>("source", metric_executor,
                                              NodeOptions{}, 0));
    assert(metric_graph.AddNode<CollectSink>("sink", metric_executor,
                                             NodeOptions{}));
    EdgeOptions edge_options;
    edge_options.capacity = 2;
    edge_options.backpressure = BackpressurePolicy::DropOldest;
    assert(metric_graph.Connect<int>("source", "sink", edge_options));

    // 在执行器启动前发送，能稳定地填满边队列并验证独立 Edge 统计。
    auto source = metric_graph.GetNode<NumberSource>("source");
    assert(source);
    assert(source->Output().Send(1));
    assert(source->Output().Send(2));
    assert(source->Output().Send(3));

    EdgeMetricsSnapshot edge_snapshot;
    assert(metric_graph.GetEdgeMetrics("source:out->sink:in", edge_snapshot));
    assert(edge_snapshot.accepted == 3);
    assert(edge_snapshot.dropped_oldest == 1);
    assert(edge_snapshot.queue_size == 2);
    assert(edge_snapshot.queue_high_watermark == 2);
    metric_graph.Stop();

    // 目标 Handler 阻塞时，节点内部 Dispatch 队列达到上限后必须拒绝新任务。
    auto bounded_executor = std::make_shared<AsioExecutor>("bounded-dispatch", 1);
    Graph bounded_graph;
    NodeOptions bounded_options;
    bounded_options.max_pending_tasks = 1;
    assert(bounded_graph.AddNode<NumberSource>("source", bounded_executor,
                                               NodeOptions{}, 0));
    assert(bounded_graph.AddNode<BlockingSink>("sink", bounded_executor,
                                               bounded_options));
    EdgeOptions bounded_edge;
    bounded_edge.capacity = 8;
    bounded_edge.max_batch_size = 8;
    assert(bounded_graph.Connect<int>("source", "sink", bounded_edge));
    assert(bounded_graph.Start());

    source = bounded_graph.GetNode<NumberSource>("source");
    auto blocking_sink = bounded_graph.GetNode<BlockingSink>("sink");
    assert(source && blocking_sink);
    assert(source->Output().Send(1));
    assert(blocking_sink->WaitUntilEntered());
    for (int value = 2; value <= 8; ++value) {
        assert(source->Output().Send(value));
    }
    blocking_sink->Release();

    NodeMetricsSnapshot node_snapshot;
    assert(WaitUntil([&]() {
        return bounded_graph.GetMetrics("sink", node_snapshot) &&
               node_snapshot.rejected > 0;
    }));
    assert(node_snapshot.max_pending_tasks == 1);
    bounded_graph.Stop();
}

void TestGraphStartStopStart() {
    // 多节点共享一个两线程执行器，验证 Graph 的执行器去重启动逻辑。
    auto executor = std::make_shared<AsioExecutor>("test", 2);
    Graph graph;
    assert(graph.AddNode<NumberSource>("source", executor, NodeOptions{}, 5));
    assert(graph.AddNode<DoubleNode>("double", executor, NodeOptions{}));
    assert(graph.AddNode<CollectSink>("sink", executor, NodeOptions{}));
    assert(graph.Connect<int>("source", "double"));
    assert(graph.Connect<int>("double", "sink"));

    // Start 会依次打开边、初始化节点、启动执行器和启动源节点。
    assert(graph.Start());
    auto sink = graph.GetNode<CollectSink>("sink");
    assert(sink);
    // 断言实际输出，防止测试只验证线程启动而没有验证消息链路。
    assert(sink->WaitFor(5));
    assert((sink->Values() == std::vector<int>{2, 4, 6, 8, 10}));

    NodeMetricsSnapshot snapshot;
    assert(graph.GetMetrics("sink", snapshot));
    assert(snapshot.processed >= 5);
    assert(graph.GetState() == Graph::State::Running);

    // 连续重启 100 次，验证 Transport、Dispatch 和 Executor 都能重复复用。
    for (std::size_t cycle = 1; cycle <= 100; ++cycle) {
        graph.Stop();
        assert(graph.GetState() == Graph::State::Stopped);
        assert(graph.Start());
        assert(sink->WaitFor(cycle * 5));
        assert(sink->Values().size() == cycle * 5);
    }
    graph.Stop();
}

} // namespace

int main() {
    TestQueueBackpressure();
    TestVideoPriorityBackpressure();
    TestVideoPriorityDispatchQueue();
    TestExecutorDispatchTransport();
    TestGraphValidationAndTopologyFreeze();
    TestGraphErrorRollback();
    TestEdgeMetricsAndBoundedDispatch();
    TestGraphStartStopStart();
    return 0;
}
