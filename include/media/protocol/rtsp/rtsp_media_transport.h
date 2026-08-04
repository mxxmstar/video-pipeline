#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>

#include "media/protocol/rtsp/rtsp_connection.h"
#include "media/protocol/rtsp/rtsp_transport_spec.h"

/// @brief transport 收到 RTCP compound packet 时通知上层的回调。
///
/// 回调在 transport 所属 Asio executor 上执行。调用方如果需要访问 session
/// 状态，应使用 weak_ptr 捕获并在回调中重新 lock，不能保存原始引用。
using RtspRtcpReceiveHandler =
    std::function<void(std::vector<std::uint8_t>, std::string)>;

/// @brief 创建媒体 transport 时所需的窄化依赖。
///
/// 该结构只描述 socket 所需的运行时边界，不包含 RTSP method、codec 或
/// server façade。UDP transport 使用 remote endpoint 推导默认 client 地址，
/// TCP transport 只使用 connection 的唯一写队列。
struct RtspMediaTransportContext {
    boost::asio::any_io_executor io_executor;
    std::weak_ptr<RtspConnection> connection;
    boost::asio::ip::tcp::endpoint local_endpoint;
    boost::asio::ip::tcp::endpoint remote_endpoint;
    RtspRtcpReceiveHandler on_rtcp;
};

/// @brief session 对 server 级 multicast publisher 的只读订阅描述。
///
/// 该值对象只用于 SETUP response、播放状态和 track 路由判断；它不拥有
/// socket，也不会让每个客户端重复发送同一份 multicast RTP。
struct RtspMulticastSubscription {
    RtspTransportSpec response_spec;
};

/// @brief RTSP SETUP 后的媒体传输抽象。
///
/// SendRtp/SendRtcp/Close 可由非 Asio 线程调用；实现会把实际 socket 操作
/// 投递到自己的 executor。GetTransportSpec() 返回 immutable response snapshot，
/// 不暴露 socket、endpoint 或可变发送队列。重复 Close 必须安全且无副作用。
class IRtspMediaTransport {
public:
    virtual ~IRtspMediaTransport() = default;

    virtual bool SendRtp(std::vector<std::uint8_t> packet) = 0;
    virtual bool SendRtcp(std::vector<std::uint8_t> packet) = 0;
    virtual RtspTransportSpec GetTransportSpec() const = 0;
    virtual bool IsTcpInterleaved() const = 0;
    virtual void Close() = 0;
};

/// @brief transport 创建结果；失败时不会返回半初始化 transport。
struct RtspMediaTransportCreateResult {
    std::shared_ptr<IRtspMediaTransport> transport;
    RtspTransportSpec response_spec;
    std::string error;

    explicit operator bool() const {
        return transport != nullptr && error.empty();
    }
};

/// @brief 按已解析的 RtspTransportSpec 创建 TCP/UDP unicast transport。
///
/// multicast 不在这里创建每客户端 socket；session 只保存 multicast
/// subscription，server 级 publisher 负责共享发送资源。
class RtspMediaTransportFactory {
public:
    static RtspMediaTransportCreateResult Create(
        RtspTransportSpec requested,
        const RtspMediaTransportContext& context);
};
