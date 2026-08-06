#include "mediaflow/mediaflow.h"
#include "media/simple_buffer.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct CostedMessage {
    std::size_t bytes{0};
    std::int64_t timestamp_us{mediaflow::kNoQueueTimestamp};
    std::int64_t duration_us{0};
    std::uint64_t generation{0};
};

namespace mediaflow {

template <>
struct QueueItemTraits<::CostedMessage> {
    static bool IsAudio(const ::CostedMessage&) { return false; }
    static bool IsVideo(const ::CostedMessage&) { return false; }
    static bool IsKeyframe(const ::CostedMessage&) { return false; }
    static bool IsControl(const ::CostedMessage&) { return false; }

    static QueueItemCost Cost(const ::CostedMessage& message) {
        return {message.bytes, message.timestamp_us, message.duration_us,
                message.generation};
    }
};

} // namespace mediaflow

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

class MediaBurstSource final : public SourceNode<MediaPacketMessage> {
public:
    std::string Name() const override {
        return "media-burst-source";
    }

    /// 测试需要在首个任务阻塞后再注入突发包，因此 Source 不在 Start 中发送。
    bool Start() override {
        return true;
    }

    bool Send(MediaPacketMessage message) {
        return Emit(std::move(message));
    }
};

class BlockingMediaSink final : public SinkNode<MediaPacketMessage> {
public:
    std::string Name() const override {
        return "blocking-media-sink";
    }

    bool WaitUntilEntered() {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [this]() {
            return entered_;
        });
    }

    void Release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

    bool WaitForCount(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2),
                                   [this, count]() {
                                       return messages_.size() >= count;
                                   });
    }

    std::vector<MediaPacketMessage> Messages() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return messages_;
    }

protected:
    void Process(MediaPacketMessage message) override {
        bool block = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            messages_.push_back(std::move(message));
            if (!entered_) {
                entered_ = true;
                block = true;
            }
        }
        condition_.notify_all();

        if (block) {
            std::unique_lock<std::mutex> lock(mutex_);
            condition_.wait(lock, [this]() {
                return released_;
            });
        }
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<MediaPacketMessage> messages_;
    bool entered_{false};
    bool released_{false};
};

MediaPacketMessage MakePriorityMessage(MediaType type, bool keyframe) {
    auto packet = std::make_shared<MediaPacket>();
    packet->type = type;
    packet->codec = type == MediaType::VIDEO ? CodecType::H264 : CodecType::AAC;
    packet->stream_index = type == MediaType::VIDEO ? 1 : 2;
    packet->keyframe = keyframe;
    MediaPacketMessage message;
    message.packet = std::move(packet);
    message.generation = 1;
    return message;
}

MediaPacketMessage MakeCostedMediaMessage(MediaType type,
                                          std::int64_t pts,
                                          std::int64_t duration,
                                          std::size_t bytes,
                                          std::uint64_t generation = 1) {
    auto packet = std::make_shared<MediaPacket>();
    packet->type = type;
    packet->codec = type == MediaType::VIDEO ? CodecType::H264 : CodecType::AAC;
    packet->stream_index = type == MediaType::VIDEO ? 1 : 2;
    packet->pts = pts;
    packet->dts = pts;
    packet->duration = duration;
    packet->time_base = Rational{1, 1000};
    packet->keyframe = type == MediaType::VIDEO;
    packet->buffer = std::make_shared<SimpleBuffer>(
        std::vector<std::uint8_t>(bytes, 0x01));

    MediaPacketMessage message;
    message.packet = std::move(packet);
    message.generation = generation;
    return message;
}

MediaPacketMessage MakeEosMessage(std::uint64_t generation = 1) {
    MediaPacketMessage message;
    message.generation = generation;
    message.eos = true;
    return message;
}

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
    assert(drop_newest.Metrics().limit_items == 1);

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

void TestQueueBudgetAccounting() {
    QueueBudget bytes_budget;
    bytes_budget.max_items = 8;
    bytes_budget.max_bytes = 10;
    bytes_budget.max_span_us = 100;

    QueueTransport<CostedMessage> byte_queue(
        8, BackpressurePolicy::DropNewest, bytes_budget);
    assert(byte_queue.Send({4, 100, 10, 1}) == MailboxPushResult::Accepted);
    assert(byte_queue.Send({5, 150, 20, 1}) == MailboxPushResult::Accepted);
    auto snapshot = byte_queue.Metrics();
    assert(snapshot.items == 2);
    assert(snapshot.bytes == 9);
    assert(snapshot.span_us == 70);
    assert(byte_queue.Send({2, 200, 20, 1}) ==
           MailboxPushResult::DroppedNewest);
    snapshot = byte_queue.Metrics();
    assert(snapshot.items == 2 && snapshot.bytes == 9);
    assert(snapshot.limit_bytes == 1);

    QueueTransport<CostedMessage> drop_oldest(
        8, BackpressurePolicy::DropOldest, bytes_budget);
    assert(drop_oldest.Send({4, 100, 10, 1}) == MailboxPushResult::Accepted);
    assert(drop_oldest.Send({5, 150, 20, 1}) == MailboxPushResult::Accepted);
    assert(drop_oldest.Send({2, 200, 20, 1}) ==
           MailboxPushResult::DroppedOldest);
    snapshot = drop_oldest.Metrics();
    assert(snapshot.items == 2);
    assert(snapshot.bytes == 7);
    assert(snapshot.span_us == 70);

    QueueBudget span_budget;
    span_budget.max_items = 8;
    span_budget.max_span_us = 50;
    QueueTransport<CostedMessage> span_queue(
        8, BackpressurePolicy::DropOldest, span_budget);
    assert(span_queue.Send({1, 100, 10, 1}) == MailboxPushResult::Accepted);
    assert(span_queue.Send({1, 130, 10, 1}) == MailboxPushResult::Accepted);
    assert(span_queue.Send({1, 170, 10, 1}) ==
           MailboxPushResult::DroppedOldest);
    snapshot = span_queue.Metrics();
    assert(snapshot.items == 2);
    assert(snapshot.span_us == 50);
    assert(snapshot.limit_span == 1);

    // 不同 generation 的时间轴不能拼接为一个跨重连的大跨度。
    assert(span_queue.Send({1, 5000000, 10, 2}) ==
           MailboxPushResult::Accepted);
    snapshot = span_queue.Metrics();
    assert(snapshot.span_us == 50);

    QueueBudget oversized_budget;
    oversized_budget.max_bytes = 10;
    QueueTransport<CostedMessage> oversized_queue(
        8, BackpressurePolicy::DropOldest, oversized_budget);
    assert(oversized_queue.Send({11, 0, 0, 1}) ==
           MailboxPushResult::DroppedNewest);
    snapshot = oversized_queue.Metrics();
    assert(snapshot.items == 0 && snapshot.bytes == 0);
    assert(snapshot.oversized == 1);

    span_queue.Close();
    snapshot = span_queue.Metrics();
    assert(snapshot.items == 0 && snapshot.bytes == 0 && snapshot.span_us == 0);
    span_queue.Open();
    assert(span_queue.Send({1, 0, 10, 3}) == MailboxPushResult::Accepted);
}

void TestTrackBudgetIsolationAndControlRetention() {
    QueueBudget budget;
    budget.max_items = 2;
    budget.max_bytes = 10;
    budget.max_span_us = 100000;

    QueueTransport<MediaPacketMessage> audio_queue(
        2, BackpressurePolicy::DropOldest, budget);
    assert(audio_queue.Send(MakeCostedMediaMessage(
               MediaType::AUDIO, 0, 20, 4)) == MailboxPushResult::Accepted);
    assert(audio_queue.Send(MakeEosMessage()) == MailboxPushResult::Accepted);

    // DropOldest 只能淘汰普通媒体包，不能让 EOS 被音频突发挤出队列。
    assert(audio_queue.Send(MakeCostedMediaMessage(
               MediaType::AUDIO, 40, 20, 4)) ==
           MailboxPushResult::DroppedOldest);
    auto audio_snapshot = audio_queue.Metrics();
    assert(audio_snapshot.items == 2);
    assert(audio_snapshot.bytes == 4);
    assert(audio_snapshot.span_us == 20000);
    assert(audio_queue.TryReceive()->eos);
    assert(!audio_queue.TryReceive()->eos);

    // 音频队列达到上限不应消耗另一条视频 Edge 的独立预算。
    QueueTransport<MediaPacketMessage> video_queue(
        2, BackpressurePolicy::DropNewest, budget);
    assert(video_queue.Send(MakeCostedMediaMessage(
               MediaType::VIDEO, 0, 40, 4)) == MailboxPushResult::Accepted);
    assert(video_queue.Send(MakeCostedMediaMessage(
               MediaType::VIDEO, 40, 40, 5)) == MailboxPushResult::Accepted);
    const auto video_snapshot = video_queue.Metrics();
    assert(video_snapshot.items == 2);
    assert(video_snapshot.bytes == 9);
    assert(video_snapshot.span_us == 80000);
    assert(audio_snapshot.items == 2);
}

void TestQueueDiagnosticsAndBudgetFormula() {
    const auto video_budget = QueueBudget::FromBitrate(
        64, 8000000, 1000000);
    assert(video_budget.max_items == 64);
    assert(video_budget.max_bytes == 2000000);
    assert(video_budget.max_span_us == 1000000);
    const auto audio_budget = QueueBudget::FromBitrate(
        128, 256000, 500000);
    assert(audio_budget.max_bytes == 32000);
    const auto unknown_bitrate_budget = QueueBudget::FromBitrate(
        16, 0, 250000);
    assert(unknown_bitrate_budget.max_bytes == 0);

    QueueBudget watermark_budget;
    watermark_budget.max_items = 4;
    watermark_budget.max_bytes = 16;
    watermark_budget.max_span_us = 100;
    watermark_budget.high_watermark_percent = 50;
    watermark_budget.low_watermark_percent = 25;
    QueueTransport<CostedMessage> watermark_queue(
        4, BackpressurePolicy::DropNewest, watermark_budget);
    assert(watermark_queue.Send({4, 100, 10, 1}) ==
           MailboxPushResult::Accepted);
    assert(watermark_queue.Send({4, 120, 10, 1}) ==
           MailboxPushResult::Accepted);
    auto snapshot = watermark_queue.Metrics();
    assert(snapshot.high_watermark_active);
    assert(snapshot.high_watermark_enters == 1);
    assert(snapshot.high_watermark_leaves == 0);
    assert(watermark_queue.TryReceive().has_value());
    snapshot = watermark_queue.Metrics();
    assert(!snapshot.high_watermark_active);
    assert(snapshot.high_watermark_leaves == 1);

    QueueTransport<CostedMessage> timestamp_queue(8);
    assert(timestamp_queue.Send({4, kNoQueueTimestamp, 0, 1}) ==
           MailboxPushResult::Accepted);
    assert(timestamp_queue.Send({4, 100, 10, 1}) ==
           MailboxPushResult::Accepted);
    assert(timestamp_queue.Send({4, 50, 10, 1}) ==
           MailboxPushResult::Accepted);
    snapshot = timestamp_queue.Metrics();
    assert(snapshot.timestamp_invalid == 1);
    assert(snapshot.timestamp_discontinuity == 1);

    QueueTransport<MediaPacketMessage> keyframe_queue(
        1, BackpressurePolicy::PreferVideoKeyframes);
    assert(keyframe_queue.Send(MakeCostedMediaMessage(
               MediaType::VIDEO, 0, 40, 4)) == MailboxPushResult::Accepted);
    assert(keyframe_queue.Send(MakeCostedMediaMessage(
               MediaType::VIDEO, 40, 40, 4)) ==
           MailboxPushResult::DroppedNewest);
    snapshot = keyframe_queue.Metrics();
    assert(snapshot.limit_items == 1);
    assert(snapshot.dropped_keyframes == 1);
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
    const QueueItemCost running_cost{9, 0, 40, 1, QueueTrack::Video};
    const QueueItemCost audio_cost{4, 100, 20, 1, QueueTrack::Audio};
    const QueueItemCost keyframe_cost{8, 200, 40, 1, QueueTrack::Video};
    assert(context->Dispatch([&]() {
        std::unique_lock<std::mutex> lock(mutex);
        entered = true;
        condition.notify_all();
        condition.wait(lock, [&]() { return released; });
    }, DispatchPriority{false, true, false}, running_cost));
    {
        std::unique_lock<std::mutex> lock(mutex);
        assert(condition.wait_for(lock, std::chrono::seconds(2), [&]() {
            return entered;
        }));
    }

    const DispatchPriority audio_priority{true, false, false};
    const DispatchPriority keyframe_priority{false, true, true};
    assert(context->Dispatch([]() {}, audio_priority, audio_cost));
    assert(context->Dispatch([]() {}, audio_priority, audio_cost));
    std::atomic<bool> eos_processed{false};
    const DispatchPriority eos_priority{false, false, false, true};
    assert(context->Dispatch(
        [&eos_processed]() { eos_processed.store(true); }, eos_priority,
        QueueItemCost{0, kNoQueueTimestamp, 0, 1, QueueTrack::Unknown}));
    // 第三个槽位到达时，视频关键帧应替换尚未执行的音频任务。
    assert(context->Dispatch([]() {}, keyframe_priority, keyframe_cost));
    // 新的音频不能再挤掉队列中已经保留的视频任务。
    assert(!context->Dispatch([]() {}, audio_priority, audio_cost));

    NodeMetricsSnapshot pending_snapshot = context->MetricsSnapshot();
    assert(pending_snapshot.pending_tasks == 3);
    assert(pending_snapshot.pending.items == 3);
    assert(pending_snapshot.pending.bytes == 17);
    assert(pending_snapshot.audio_pending.items == 0);
    assert(pending_snapshot.video_pending.items == 2);
    assert(pending_snapshot.unknown_pending.items == 1);

    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
    }
    condition.notify_all();
    assert(WaitUntil([&]() { return context->PendingTasks() == 0; }));
    pending_snapshot = context->MetricsSnapshot();
    assert(pending_snapshot.pending.items == 0);
    assert(pending_snapshot.pending.items_high_watermark == 3);
    assert(eos_processed.load());
    context->CloseDispatch();
    executor->Stop();
}

void TestVideoPriorityExecutorDispatch() {
    auto executor = std::make_shared<AsioExecutor>("priority-direct", 1);
    NodeOptions sink_options;
    sink_options.max_pending_tasks = 3;
    sink_options.prefer_video_keyframes = true;

    Graph graph;
    assert(graph.AddNode<MediaBurstSource>("source", executor, NodeOptions{}));
    assert(graph.AddNode<BlockingMediaSink>(
        "sink", executor, sink_options));

    EdgeOptions edge_options;
    edge_options.transport = TransportKind::ExecutorDispatch;
    assert(graph.Connect<MediaPacketMessage>("source", "sink", edge_options));
    assert(graph.Start());

    auto source = graph.GetNode<MediaBurstSource>("source");
    auto sink = graph.GetNode<BlockingMediaSink>("sink");
    assert(source && sink);

    // 先占住一个正在执行的音频任务，再让音频突发填满 Node Dispatch 队列。
    assert(source->Send(MakePriorityMessage(MediaType::AUDIO, false)));
    assert(sink->WaitUntilEntered());
    assert(source->Send(MakePriorityMessage(MediaType::AUDIO, false)));
    assert(source->Send(MakePriorityMessage(MediaType::AUDIO, false)));
    // ExecutorDispatch 没有 Drain，视频分类必须在 Edge 的直接消费者中传递，
    // 否则这个关键帧会被错误地按普通满队列任务拒绝。
    assert(source->Send(MakePriorityMessage(MediaType::VIDEO, true)));
    // 关键帧已经替换了一个排队音频，新的音频应当从 OutputPort 观察到拒绝。
    assert(!source->Send(MakePriorityMessage(MediaType::AUDIO, false)));

    sink->Release();
    assert(sink->WaitForCount(3));
    const auto messages = sink->Messages();
    bool received_video_keyframe = false;
    for (const auto& message : messages) {
        if (message.packet && message.packet->type == MediaType::VIDEO &&
            message.packet->keyframe) {
            received_video_keyframe = true;
        }
    }
    assert(received_video_keyframe);
    graph.Stop();
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

void TestMediaEdgeAndDispatchInflightMetrics() {
    auto executor = std::make_shared<AsioExecutor>("media-inflight", 1);
    Graph graph;
    NodeOptions sink_options;
    sink_options.max_pending_tasks = 1;
    assert(graph.AddNode<MediaBurstSource>("source", executor, NodeOptions{}));
    assert(graph.AddNode<BlockingMediaSink>("sink", executor, sink_options));

    EdgeOptions edge_options;
    edge_options.max_batch_size = 1;
    edge_options.backpressure = BackpressurePolicy::DropNewest;
    edge_options.track = QueueTrack::Video;
    edge_options.budget.max_items = 2;
    edge_options.budget.max_bytes = 10;
    edge_options.budget.max_span_us = 100000;
    assert(graph.Connect<MediaPacketMessage>("source", "sink", edge_options));
    assert(graph.Start());

    auto source = graph.GetNode<MediaBurstSource>("source");
    auto sink = graph.GetNode<BlockingMediaSink>("sink");
    assert(source && sink);
    assert(source->Send(MakeCostedMediaMessage(
        MediaType::VIDEO, 0, 40, 2)));
    assert(sink->WaitUntilEntered());

    // Sink 的首包仍在执行时，后续两个包应停留在 Edge 队列；第三个包触发
    // DropNewest。这样可以同时核对 Edge 缓存和 Node Dispatch 在途成本。
    assert(source->Send(MakeCostedMediaMessage(
        MediaType::VIDEO, 40, 40, 3)));
    assert(source->Send(MakeCostedMediaMessage(
        MediaType::VIDEO, 80, 40, 4)));
    assert(!source->Send(MakeCostedMediaMessage(
        MediaType::VIDEO, 120, 40, 4)));

    EdgeMetricsSnapshot edge_snapshot;
    NodeMetricsSnapshot node_snapshot;
    assert(graph.GetEdgeMetrics("source:out->sink:in", edge_snapshot));
    assert(graph.GetMetrics("sink", node_snapshot));
    assert(edge_snapshot.budget.items == 2);
    assert(edge_snapshot.budget.bytes == 7);
    assert(edge_snapshot.budget.span_us == 80000);
    assert(edge_snapshot.budget.items_high_watermark == 2);
    assert(edge_snapshot.budget.high_watermark_enters == 1);
    assert(edge_snapshot.budget.dropped_keyframes == 1);
    assert(edge_snapshot.dropped_newest == 1);
    assert(node_snapshot.pending.items == 1);
    assert(node_snapshot.pending.bytes == 2);
    assert(node_snapshot.pending.span_us == 40000);
    assert(node_snapshot.audio_pending.items == 0);
    assert(node_snapshot.video_pending.items == 1);
    assert(node_snapshot.video_pending.bytes == 2);
    assert(edge_snapshot.budget.items + node_snapshot.pending.items == 3);
    assert(edge_snapshot.budget.bytes + node_snapshot.pending.bytes == 9);

    sink->Release();
    assert(sink->WaitForCount(3));
    assert(WaitUntil([&]() {
        EdgeMetricsSnapshot current_edge;
        NodeMetricsSnapshot current_node;
        return graph.GetEdgeMetrics("source:out->sink:in", current_edge) &&
               graph.GetMetrics("sink", current_node) &&
               current_edge.budget.items == 0 &&
               current_node.pending.items == 0;
    }));
    graph.Stop();
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
    TestQueueBudgetAccounting();
    TestTrackBudgetIsolationAndControlRetention();
    TestQueueDiagnosticsAndBudgetFormula();
    TestVideoPriorityBackpressure();
    TestVideoPriorityDispatchQueue();
    TestVideoPriorityExecutorDispatch();
    TestExecutorDispatchTransport();
    TestGraphValidationAndTopologyFreeze();
    TestGraphErrorRollback();
    TestEdgeMetricsAndBoundedDispatch();
    TestMediaEdgeAndDispatchInflightMetrics();
    TestGraphStartStopStart();
    return 0;
}
