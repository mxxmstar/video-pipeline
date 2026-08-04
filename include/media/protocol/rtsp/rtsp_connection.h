#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <boost/asio/ip/tcp.hpp>

#include "media/protocol/rtsp/rtsp_request_parser.h"

/// @brief 单个 RTSP TCP 客户端连接的字节流和写入边界。
///
/// connection 只拥有 TCP socket、RTSP request parser、interleaved frame
/// framing 和唯一写队列，不理解 RTSP method、track、codec 或 RTP 状态。
/// 所有回调均在 socket 所属的 Asio executor 上串行调用；回调通过外部的
/// weak_ptr 捕获 connection 的上层对象，避免 connection/session 相互持有。
class RtspConnection : public std::enable_shared_from_this<RtspConnection> {
public:
    using RequestHandler = std::function<void(RtspRequest)>;
    using InterleavedFrameHandler =
        std::function<void(std::uint8_t, std::vector<std::uint8_t>)>;
    using ParseErrorHandler =
        std::function<void(const RtspRequestParseResult&)>;
    using ClosedHandler =
        std::function<void(const boost::system::error_code&)>;

    /// socket 必须已经由 accept 完成；connection 接管它的唯一生命周期。
    /// handler 不应直接捕获拥有 connection 的 shared_ptr，避免形成引用环。
    RtspConnection(boost::asio::ip::tcp::socket socket,
                   RequestHandler on_request,
                   InterleavedFrameHandler on_interleaved_frame,
                   ParseErrorHandler on_parse_error,
                   ClosedHandler on_closed);

    RtspConnection(const RtspConnection&) = delete;
    RtspConnection& operator=(const RtspConnection&) = delete;

    /// 开始唯一的 async_read_some 循环；重复调用只保留第一次启动。
    /// 调用方必须在 socket 所属的 Asio executor 上调用（当前由 accept 回调调用）。
    void Start();

    /// 将 RTSP 文本响应加入唯一写队列，调用线程可以不是 Asio 线程。
    void SendRtsp(std::string response);

    /// 添加 `$ + channel + uint16 length + payload` interleaved frame。
    /// length 使用网络字节序，payload 生命周期由写队列保持到 async_write 完成。
    void SendInterleaved(std::uint8_t channel,
                         std::vector<std::uint8_t> payload);

    /// 停止继续读入新数据，等待当前队列全部写完后再触发关闭回调。
    void CloseAfterFlush();

    /// 立即取消读写并关闭 TCP；操作幂等，OnClosed 最多回调一次。
    void Close();

    /// 以下两个 endpoint 快照只读取 socket，不暴露 socket 本身。
    boost::asio::ip::tcp::endpoint LocalEndpoint(
        boost::system::error_code& ec) const;
    boost::asio::ip::tcp::endpoint RemoteEndpoint(
        boost::system::error_code& ec) const;

private:
    void StartOnExecutor();
    void DoRead();
    void ProcessReadBuffer();
    void EnqueueWrite(std::vector<std::uint8_t> data);
    void DoWrite();
    void CloseAfterFlushOnExecutor();
    void CloseOnExecutor(const boost::system::error_code& reason = {});
    std::vector<std::uint8_t> BuildInterleavedFrame(
        std::uint8_t channel,
        const std::vector<std::uint8_t>& payload) const;

    boost::asio::ip::tcp::socket socket_;
    RtspRequestParser request_parser_;
    RequestHandler on_request_;
    InterleavedFrameHandler on_interleaved_frame_;
    ParseErrorHandler on_parse_error_;
    ClosedHandler on_closed_;

    std::array<std::uint8_t, 8192> read_chunk_{};
    std::vector<std::uint8_t> read_buffer_;
    std::deque<std::vector<std::uint8_t>> write_queue_;
    bool started_{false};
    bool writing_{false};
    bool closed_{false};
    bool close_after_write_{false};
};
