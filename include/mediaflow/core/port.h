#pragma once

#include "mediaflow/core/transport.h"

#include <any>
#include <functional>
#include <memory>
#include <string>
#include <typeindex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * @file port.h
 * @brief MediaFlow 的类型安全输入端口、输出端口和端口注册表。
 *
 * Port 只负责保存节点边界，不决定线程调度。Graph 会根据注册表中的
 * 类型信息校验连接，避免把不同消息类型的端口错误地连在一起。
 */
namespace mediaflow {

/// 节点接收消息的端口。Handler 通常由 SinkNode/TransformNode 设置。
template <typename T>
class InputPort {
public:
    using Type = T;
    using Handler = std::function<void(T)>;

    /// 设置消息处理函数；同一端口通常只设置一次。
    void SetHandler(Handler handler) {
        handler_ = std::move(handler);
    }

    /// 由 Graph Edge 调用，将消息交给节点业务处理。
    void Receive(T value) {
        if (handler_) {
            handler_(std::move(value));
        }
    }

private:
    Handler handler_;
};

/**
 * @brief 节点发送消息的端口。
 *
 * 单下游时使用 move，避免复制消息；多下游时要求 T 可复制，保证每个
 * 下游都获得一份合法的消息对象。对于 MediaFrame/MediaPacket，推荐 T
 * 使用 shared_ptr，使 fan-out 只复制指针而不复制媒体载荷。
 */
template <typename T>
class OutputPort {
public:
    using Type = T;

    /// 由 Graph 添加一条下游边。
    void AddTransport(std::shared_ptr<ITransport<T>> transport) {
        transports_.push_back(std::move(transport));
    }

    /// 将消息发送到所有下游，返回是否至少有一个下游接受消息。
    bool Send(T value) {
        if (transports_.empty()) {
            return true;
        }

        if (transports_.size() == 1) {
            return IsAccepted(transports_.front()->Send(std::move(value)));
        }

        if constexpr (!std::is_copy_constructible_v<T>) {
            return false;
        } else {
            bool accepted = true;
            for (auto& transport : transports_) {
                accepted = IsAccepted(transport->Send(value)) && accepted;
            }
            return accepted;
        }
    }

private:
    /// 被淘汰旧消息的 DropOldest 仍视为当前消息已成功进入队列。
    static bool IsAccepted(MailboxPushResult result) {
        return result == MailboxPushResult::Accepted ||
               result == MailboxPushResult::DroppedOldest;
    }

    std::vector<std::shared_ptr<ITransport<T>>> transports_;
};

/// 端口方向，用于 Graph::Connect 的结构校验。
enum class PortDirection {
    Input,
    Output,
};

/// 端口的运行时描述；port 指向节点自身拥有的 InputPort/OutputPort。
struct PortBinding {
    PortDirection direction;
    std::type_index type{typeid(void)};
    void* port{nullptr};
};

/// 节点创建时注册端口，Graph 保存注册结果用于后续连接。
class PortRegistry {
public:
    /// 注册一个输入端口；同名端口重复注册会返回 false。
    template <typename T>
    bool Input(const std::string& name, InputPort<T>& port) {
        return Register(name, PortBinding{PortDirection::Input, typeid(T), &port});
    }

    template <typename T>
    bool Output(const std::string& name, OutputPort<T>& port) {
        return Register(name, PortBinding{PortDirection::Output, typeid(T), &port});
    }

    /// 按名称查找端口描述。
    const PortBinding* Find(const std::string& name) const {
        auto it = bindings_.find(name);
        return it == bindings_.end() ? nullptr : &it->second;
    }

    /// 返回完整注册表，NodeContext 会复制它并在连接阶段使用。
    const std::unordered_map<std::string, PortBinding>& Bindings() const {
        return bindings_;
    }

private:
    bool Register(const std::string& name, PortBinding binding) {
        return bindings_.emplace(name, binding).second;
    }

    std::unordered_map<std::string, PortBinding> bindings_;
};

} // namespace mediaflow
    /// 注册一个输出端口；同名端口重复注册会返回 false。
