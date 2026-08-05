#pragma once

#include "mediaflow/core/port.h"

#include <string>
#include <utility>

/**
 * @file node.h
 * @brief MediaFlow 节点生命周期接口和常用节点基类。
 *
 * 节点只实现业务生命周期和消息处理，线程归属由 Graph 为节点注入的
 * Executor 决定。SourceNode、TransformNode 和 SinkNode 提供最常见的单
 * 输入/单输出形式；多输入、多输出节点可以直接实现 INode 并注册命名端口。
 */
namespace mediaflow {

/// Graph 管理的最小业务节点接口。
class INode {
public:
    virtual ~INode() = default;

    /// 注册节点端口，失败表示节点结构不能加入 Graph。
    virtual bool RegisterPorts(PortRegistry& registry) = 0;
    /// 分配资源，但不应开始主动生产消息。
    virtual bool Init() { return true; }
    /// 开始工作；Source 通常在此阶段开始 Emit。
    virtual bool Start() { return true; }
    /// 停止生产和接收新的业务工作。
    virtual void Stop() {}
    /// 释放 Init 阶段分配的资源。
    virtual void Deinit() {}
    /// 返回便于日志定位的节点名称。
    virtual std::string Name() const = 0;
};

/// 无输入、只有一个默认输出端口 out 的源节点。
template <typename T>
class SourceNode : public INode {
public:
    /// 获取默认输出端口，Graph 会通过端口注册表连接它。
    OutputPort<T>& Output() {
        return output_;
    }

    /// 注册默认输出端口 out。
    bool RegisterPorts(PortRegistry& registry) override {
        return registry.Output("out", output_);
    }

protected:
    /// 向所有下游发送一条消息；Source 不负责等待或重试背压结果。
    void Emit(T value) {
        output_.Send(std::move(value));
    }

private:
    OutputPort<T> output_;
};

/// 只有一个默认输入端口 in 的终端节点。
template <typename T>
class SinkNode : public INode {
public:
    /// 构造时绑定输入 Handler，实际调用发生在目标 Executor 上。
    SinkNode() {
        input_.SetHandler([this](T value) {
            Process(std::move(value));
        });
    }

    /// 获取默认输入端口。
    InputPort<T>& Input() {
        return input_;
    }

    /// 注册默认输入端口 in。
    bool RegisterPorts(PortRegistry& registry) override {
        return registry.Input("in", input_);
    }

protected:
    /// 子类实现具体消费逻辑。
    virtual void Process(T value) = 0;

private:
    InputPort<T> input_;
};

/// 一个输入 in 和一个输出 out 的同步业务变换节点。
template <typename In, typename Out>
class TransformNode : public INode {
public:
    /// 构造时将输入绑定到 Process；Process 仍在 Graph 注入的 Executor 上执行。
    TransformNode() {
        input_.SetHandler([this](In value) {
            Process(std::move(value));
        });
    }

    /// 获取默认输入端口。
    InputPort<In>& Input() {
        return input_;
    }

    /// 获取默认输出端口。
    OutputPort<Out>& Output() {
        return output_;
    }

    /// 注册默认输入和输出端口。
    bool RegisterPorts(PortRegistry& registry) override {
        return registry.Input("in", input_) && registry.Output("out", output_);
    }

protected:
    /// 子类实现变换逻辑。
    virtual void Process(In value) = 0;

    void Emit(Out value) {
        output_.Send(std::move(value));
    }

private:
    InputPort<In> input_;
    OutputPort<Out> output_;
};

} // namespace mediaflow
    /// 将变换结果发送给下游。
