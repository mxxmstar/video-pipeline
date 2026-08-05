#include "media/protocol/rtsp/rtsp_connection.h"

#include "common/log/logger.h"
#include "media/protocol/rtsp/rtsp_builders.h"

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>

#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>

namespace {

constexpr std::size_t kTcpInterleavedHeaderSize = 4;

std::uint16_t ReadU16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8) |
        static_cast<std::uint16_t>(data[1]));
}

void WriteU16(std::uint8_t* data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[1] = static_cast<std::uint8_t>(value & 0xFF);
}

} // namespace

RtspConnection::RtspConnection(
    boost::asio::ip::tcp::socket socket,
    RequestHandler on_request,
    InterleavedFrameHandler on_interleaved_frame,
    ParseErrorHandler on_parse_error,
    ClosedHandler on_closed,
    RtspConnectionOptions options)
    : socket_(std::move(socket)),
      on_request_(std::move(on_request)),
      on_interleaved_frame_(std::move(on_interleaved_frame)),
      on_parse_error_(std::move(on_parse_error)),
      on_closed_(std::move(on_closed)),
      options_(options) {
}

void RtspConnection::Start() {
    // accept 回调已经运行在同一个 io_context；直接启动可避免把首次 read
    // 延迟到下一轮事件循环，确保 session 注册后立即开始消费客户端请求。
    StartOnExecutor();
}

void RtspConnection::StartOnExecutor() {
    if (started_ || closed_) {
        return;
    }

    // 连接对象只允许一个 read loop；重复 Start 不能再挂第二个
    // async_read_some，否则同一个 read_chunk_ 会被并发覆盖。
    started_ = true;
    DoRead();
}

void RtspConnection::SendRtsp(std::string response) {
    std::vector<std::uint8_t> bytes(response.begin(), response.end());
    auto self = shared_from_this();
    boost::asio::dispatch(socket_.get_executor(),
                           [self, bytes = std::move(bytes)]() mutable {
                               self->EnqueueWrite(std::move(bytes));
                           });
}

void RtspConnection::SendInterleaved(
    std::uint8_t channel,
    std::vector<std::uint8_t> payload) {
    auto self = shared_from_this();
    boost::asio::dispatch(
        socket_.get_executor(),
        [self, channel, payload = std::move(payload)]() mutable {
            self->EnqueueWrite(
                self->BuildInterleavedFrame(channel, payload));
        });
}

void RtspConnection::CloseAfterFlush() {
    auto self = shared_from_this();
    boost::asio::dispatch(socket_.get_executor(),
                           [self]() { self->CloseAfterFlushOnExecutor(); });
}

void RtspConnection::Close() {
    auto self = shared_from_this();
    boost::asio::dispatch(socket_.get_executor(),
                           [self]() { self->CloseOnExecutor(); });
}

boost::asio::ip::tcp::endpoint RtspConnection::LocalEndpoint(
    boost::system::error_code& ec) const {
    return socket_.local_endpoint(ec);
}

boost::asio::ip::tcp::endpoint RtspConnection::RemoteEndpoint(
    boost::system::error_code& ec) const {
    return socket_.remote_endpoint(ec);
}

void RtspConnection::DoRead() {
    auto self = shared_from_this();
    socket_.async_read_some(
        boost::asio::buffer(read_chunk_),
        [self](boost::system::error_code ec, std::size_t bytes) {
            if (ec) {
                self->CloseOnExecutor(ec);
                return;
            }

            self->read_buffer_.insert(self->read_buffer_.end(),
                                      self->read_chunk_.data(),
                                      self->read_chunk_.data() + bytes);
            self->ProcessReadBuffer();
            if (!self->closed_ && !self->close_after_write_) {
                self->DoRead();
            }
        });
}

void RtspConnection::ProcessReadBuffer() {
    while (!read_buffer_.empty() && !closed_) {
        if (read_buffer_[0] == '$') {
            // RTSP over TCP 的二进制 frame 可能在任意字节处分片；先保留
            // 不完整 header/payload，下一次 read 再继续，不能提前消费。
            if (read_buffer_.size() < kTcpInterleavedHeaderSize) {
                return;
            }

            const auto channel = read_buffer_[1];
            const auto length = ReadU16(read_buffer_.data() + 2);
            const auto frame_size = kTcpInterleavedHeaderSize + length;
            if (read_buffer_.size() < frame_size) {
                return;
            }

            std::vector<std::uint8_t> payload(
                read_buffer_.begin() + kTcpInterleavedHeaderSize,
                read_buffer_.begin() + static_cast<std::ptrdiff_t>(frame_size));
            read_buffer_.erase(
                read_buffer_.begin(),
                read_buffer_.begin() + static_cast<std::ptrdiff_t>(frame_size));

            // 空 frame 与旧 session 行为一致：只完成 framing，不向 RTCP
            // 解析器传入空数据；未知 channel 也交给上层决定是否忽略。
            if (on_interleaved_frame_ && !payload.empty()) {
                on_interleaved_frame_(channel, std::move(payload));
            }
            continue;
        }

        RtspRequest request;
        // parser 一次只消费一条完整 request，剩余数据留在同一 buffer，
        // 因而支持 request pipelining 以及文本和 interleaved frame 交错。
        const auto parse_result = request_parser_.Parse(
            std::span<const std::uint8_t>{read_buffer_}, request);
        if (parse_result.status == RtspRequestParseStatus::NeedMoreData) {
            return;
        }
        if (parse_result.status == RtspRequestParseStatus::Error) {
            read_buffer_.clear();
            if (on_parse_error_) {
                on_parse_error_(parse_result);
            } else {
                // 没有上层 session 时仍返回可识别的 RTSP 400，并在写完后
                // 关闭，保证 connection 单独使用也不会无限等待畸形输入。
                const auto response =
                    RtspResponseBuilder::Build(400, "Bad Request", {});
                EnqueueWrite(
                    std::vector<std::uint8_t>(response.begin(), response.end()));
                CloseAfterFlushOnExecutor();
            }
            return;
        }

        read_buffer_.erase(
            read_buffer_.begin(),
            read_buffer_.begin() +
                static_cast<std::ptrdiff_t>(parse_result.consumed));
        if (on_request_) {
            on_request_(std::move(request));
        }
    }
}

void RtspConnection::EnqueueWrite(std::vector<std::uint8_t> data) {
    if (closed_) {
        return;
    }

    // 所有发送数据都在 socket executor 上串行进入这里，因此无需额外加锁即可
    // 保证 queued_write_bytes_ 与 write_queue_ 一致。超限时不再追加新 buffer，
    // 而是立即关闭当前连接；这样一个不读数据的客户端不会持续占用服务端内存，
    // 也不会阻塞同一进程中的其他 RTSP 客户端。
    if (options_.max_write_queue_bytes != 0 &&
        (queued_write_bytes_ > options_.max_write_queue_bytes ||
         data.size() > options_.max_write_queue_bytes - queued_write_bytes_)) {
        CloseOnExecutor(boost::asio::error::make_error_code(
            boost::asio::error::no_buffer_space));
        return;
    }

    queued_write_bytes_ += data.size();
    write_queue_.push_back(std::move(data));
    if (!writing_) {
        DoWrite();
    }
}

void RtspConnection::DoWrite() {
    if (closed_ || writing_ || write_queue_.empty()) {
        if (!closed_ && write_queue_.empty() && close_after_write_) {
            CloseOnExecutor();
        }
        return;
    }

    writing_ = true;
    auto self = shared_from_this();
    boost::asio::async_write(
        socket_,
        boost::asio::buffer(write_queue_.front()),
        [self](boost::system::error_code ec, std::size_t) {
            self->writing_ = false;
            if (ec) {
                self->CloseOnExecutor(ec);
                return;
            }

            // async_write 成功后首个 buffer 已经完全离开队列，先扣减其字节数
            // 再弹出容器，避免后续入队看到虚高的内存占用。
            const auto completed_size = self->write_queue_.front().size();
            self->queued_write_bytes_ -= completed_size;
            self->write_queue_.pop_front();
            if (self->write_queue_.empty() && self->close_after_write_) {
                self->CloseOnExecutor();
                return;
            }
            self->DoWrite();
        });
}

void RtspConnection::CloseAfterFlushOnExecutor() {
    if (closed_) {
        return;
    }

    // 设置后不再继续 read；已经入队的 response/RTP/RTCP 仍按原顺序写完。
    close_after_write_ = true;
    if (!writing_ && write_queue_.empty()) {
        CloseOnExecutor();
    }
}

void RtspConnection::CloseOnExecutor(
    const boost::system::error_code& reason) {
    if (closed_) {
        return;
    }

    closed_ = true;
    close_after_write_ = true;
    // async_write 仍可能引用 queue.front()；关闭 socket 后保留正在写的
    // buffer，直到完成回调返回，避免清空 deque 造成悬空 asio buffer。
    if (!writing_) {
        write_queue_.clear();
        queued_write_bytes_ = 0;
    }
    read_buffer_.clear();

    boost::system::error_code ignored;
    socket_.cancel(ignored);
    socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);

    // 关闭回调在 socket 关闭后只触发一次；晚到的 read/write callback 会
    // 看到 closed_ 并直接返回，不能再次删除 session 或发送数据。
    if (on_closed_) {
        on_closed_(reason);
    }
}

std::vector<std::uint8_t> RtspConnection::BuildInterleavedFrame(
    std::uint8_t channel,
    const std::vector<std::uint8_t>& payload) const {
    std::vector<std::uint8_t> frame(kTcpInterleavedHeaderSize + payload.size());
    frame[0] = '$';
    frame[1] = channel;
    WriteU16(frame.data() + 2, static_cast<std::uint16_t>(payload.size()));
    std::copy(payload.begin(),
              payload.end(),
              frame.begin() +
                  static_cast<std::ptrdiff_t>(kTcpInterleavedHeaderSize));
    return frame;
}
