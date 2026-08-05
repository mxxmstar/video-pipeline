#include "mediaflow/mediaflow.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
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

    // 停止后重新启动同一 Graph，验证 Transport 和 Executor 都能复用。
    graph.Stop();
    assert(graph.GetState() == Graph::State::Stopped);
    assert(graph.Start());
    assert(sink->WaitFor(10));
    assert(sink->Values().size() == 10);
    graph.Stop();
}

} // namespace

int main() {
    TestQueueBackpressure();
    TestGraphStartStopStart();
    return 0;
}
