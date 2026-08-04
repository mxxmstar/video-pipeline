#include "media/protocol/rtsp/rtsp_media_transport.h"

#include "common/log/logger.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <utility>

#include <boost/asio/dispatch.hpp>

namespace {

class TcpInterleavedTransport final
    : public IRtspMediaTransport,
      public std::enable_shared_from_this<TcpInterleavedTransport> {
public:
    TcpInterleavedTransport(RtspTransportSpec spec,
                            std::weak_ptr<RtspConnection> connection)
        : spec_(std::move(spec)), connection_(std::move(connection)) {
    }

    bool SendRtp(std::vector<std::uint8_t> packet) override {
        return Send(static_cast<std::uint8_t>(spec_.rtp_channel),
                    std::move(packet));
    }

    bool SendRtcp(std::vector<std::uint8_t> packet) override {
        return Send(static_cast<std::uint8_t>(spec_.rtcp_channel),
                    std::move(packet));
    }

    RtspTransportSpec GetTransportSpec() const override {
        return spec_;
    }

    bool IsTcpInterleaved() const override {
        return true;
    }

    void Close() override {
        // TCP socket 的真正关闭仍由 session/connection 负责；这里先切断
        // 媒体发送入口，防止 pipeline 线程在 TEARDOWN 后继续排队 RTP。
        closed_.store(true);
    }

private:
    bool Send(std::uint8_t channel, std::vector<std::uint8_t> packet) {
        if (closed_.load()) {
            return false;
        }

        // RtspConnection 是 TCP 的唯一 writer；transport 不自行构造 frame，
        // 这样 RTSP response、RTP 和 RTCP 始终共享同一条串行写队列。
        const auto connection = connection_.lock();
        if (!connection) {
            return false;
        }
        connection->SendInterleaved(channel, std::move(packet));
        return true;
    }

    RtspTransportSpec spec_;
    std::weak_ptr<RtspConnection> connection_;
    std::atomic_bool closed_{false};
};

class UdpUnicastTransport final
    : public IRtspMediaTransport,
      public std::enable_shared_from_this<UdpUnicastTransport> {
public:
    UdpUnicastTransport(RtspTransportSpec spec,
                        RtspMediaTransportContext context,
                        boost::system::error_code& ec)
        : spec_(std::move(spec)),
          context_(std::move(context)),
          rtp_socket_(context_.io_executor),
          rtcp_socket_(context_.io_executor) {
        Initialize(ec);
    }

    bool SendRtp(std::vector<std::uint8_t> packet) override {
        return EnqueueDatagram(rtp_socket_, rtp_endpoint_, std::move(packet), "RTP");
    }

    bool SendRtcp(std::vector<std::uint8_t> packet) override {
        return EnqueueDatagram(rtcp_socket_, rtcp_endpoint_, std::move(packet), "RTCP");
    }

    RtspTransportSpec GetTransportSpec() const override {
        return spec_;
    }

    bool IsTcpInterleaved() const override {
        return false;
    }

    // 构造函数返回后才具备 shared_ptr 控制块，receive loop 必须在此之后启动。
    void StartReceiveLoop() {
        StartReceive();
    }

    void Close() override {
        if (closed_.exchange(true)) {
            return;
        }

        auto self = shared_from_this();
        boost::asio::dispatch(context_.io_executor,
                               [self]() { self->CloseOnExecutor(); });
    }

private:
    void Initialize(boost::system::error_code& ec) {
        // SETUP 已经完成 transport header 语法校验；此处只做运行时地址族、
        // socket 和端口资源校验。任何中途失败都关闭已打开的另一只 socket，
        // factory 随后只返回 error，不把不完整的 transport 交给 session。
        auto destination = context_.remote_endpoint.address();
        if (!spec_.destination.empty()) {
            destination = boost::asio::ip::make_address(spec_.destination, ec);
            if (ec) {
                return;
            }
        }

        if (destination.is_unspecified() ||
            destination.is_v4() != context_.remote_endpoint.address().is_v4()) {
            ec = boost::system::errc::make_error_code(
                boost::system::errc::address_family_not_supported);
            return;
        }

        const auto protocol = destination.is_v6()
            ? boost::asio::ip::udp::v6()
            : boost::asio::ip::udp::v4();
        auto bind_address = destination.is_v6()
            ? boost::asio::ip::address{boost::asio::ip::address_v6::any()}
            : boost::asio::ip::address{boost::asio::ip::address_v4::any()};
        if (!context_.local_endpoint.address().is_unspecified() &&
            context_.local_endpoint.address().is_v4() == destination.is_v4()) {
            bind_address = context_.local_endpoint.address();
        }

        rtp_socket_.open(protocol, ec);
        if (ec) {
            return;
        }
        rtcp_socket_.open(protocol, ec);
        if (ec) {
            CloseOnExecutor();
            return;
        }
        rtp_socket_.bind({bind_address, 0}, ec);
        if (ec) {
            CloseOnExecutor();
            return;
        }
        rtcp_socket_.bind({bind_address, 0}, ec);
        if (ec) {
            CloseOnExecutor();
            return;
        }

        const auto rtp_local = rtp_socket_.local_endpoint(ec);
        if (ec) {
            CloseOnExecutor();
            return;
        }
        const auto rtcp_local = rtcp_socket_.local_endpoint(ec);
        if (ec) {
            CloseOnExecutor();
            return;
        }

        spec_.server_rtp_port = rtp_local.port();
        spec_.server_rtcp_port = rtcp_local.port();
        if (!rtp_local.address().is_unspecified()) {
            spec_.source = rtp_local.address().to_string();
        }
        rtp_endpoint_ = {destination, spec_.client_rtp_port};
        rtcp_endpoint_ = {destination, spec_.client_rtcp_port};
        expected_rtcp_sender_ = rtcp_endpoint_;
    }

    bool EnqueueDatagram(boost::asio::ip::udp::socket& socket,
                         const boost::asio::ip::udp::endpoint& endpoint,
                         std::vector<std::uint8_t> packet,
                         const char* packet_name) {
        if (closed_.load() || packet.empty()) {
            return false;
        }

        auto self = shared_from_this();
        auto data = std::make_shared<std::vector<std::uint8_t>>(std::move(packet));
        // packet 由 shared_ptr 保持到 async_send_to 回调结束；调用方可以在
        // SendRtp/SendRtcp 返回后立即释放自己的 vector，不会产生悬空 buffer。
        boost::asio::dispatch(
            context_.io_executor,
            [self, &socket, endpoint, data, packet_name]() {
                if (self->closed_.load() || !socket.is_open()) {
                    return;
                }
                socket.async_send_to(
                    boost::asio::buffer(*data),
                    endpoint,
                    [self, data, packet_name](boost::system::error_code ec,
                                               std::size_t) {
                        if (ec && !self->closed_.load() &&
                            ec != boost::asio::error::operation_aborted) {
                            LOG_WARN("RTSP UDP {} send failed: {}",
                                     packet_name,
                                     ec.message());
                        }
                    });
            });
        return true;
    }

    void StartReceive() {
        if (closed_.load() || !rtcp_socket_.is_open()) {
            return;
        }

        // 同一时刻只挂一个 RTCP receive；回调完成后再续订，保证 read buffer
        // 和 sender endpoint 不会被并发 receive 覆盖。
        auto self = shared_from_this();
        rtcp_socket_.async_receive_from(
            boost::asio::buffer(rtcp_read_buffer_),
            rtcp_sender_endpoint_,
            [self](boost::system::error_code ec, std::size_t bytes) {
                if (ec) {
                    if (ec != boost::asio::error::operation_aborted &&
                        !self->closed_.load()) {
                        LOG_DEBUG("RTSP UDP RTCP receive failed: {}", ec.message());
                    }
                    return;
                }

                // 只接受 SETUP 中宣告的 client RTCP endpoint，避免任意 UDP
                // 来源把无关的 RTCP report 注入当前 session。
                if (self->rtcp_sender_endpoint_ == self->expected_rtcp_sender_) {
                    std::vector<std::uint8_t> packet(
                        self->rtcp_read_buffer_.begin(),
                        self->rtcp_read_buffer_.begin() +
                            static_cast<std::ptrdiff_t>(bytes));
                    if (self->context_.on_rtcp) {
                        self->context_.on_rtcp(std::move(packet), "udp-unicast");
                    }
                } else {
                    LOG_DEBUG("RTSP UDP RTCP ignored from unexpected endpoint: {}:{}",
                              self->rtcp_sender_endpoint_.address().to_string(),
                              self->rtcp_sender_endpoint_.port());
                }

                self->StartReceive();
            });
    }

    void CloseOnExecutor() {
        boost::system::error_code ignored;
        rtcp_socket_.cancel(ignored);
        rtcp_socket_.close(ignored);
        rtp_socket_.cancel(ignored);
        rtp_socket_.close(ignored);
    }

    RtspTransportSpec spec_;
    RtspMediaTransportContext context_;
    boost::asio::ip::udp::socket rtp_socket_;
    boost::asio::ip::udp::socket rtcp_socket_;
    boost::asio::ip::udp::endpoint rtp_endpoint_;
    boost::asio::ip::udp::endpoint rtcp_endpoint_;
    boost::asio::ip::udp::endpoint expected_rtcp_sender_;
    boost::asio::ip::udp::endpoint rtcp_sender_endpoint_;
    std::array<std::uint8_t, 2048> rtcp_read_buffer_{};
    std::atomic_bool closed_{false};
};

} // namespace

RtspMediaTransportCreateResult RtspMediaTransportFactory::Create(
    RtspTransportSpec requested,
    const RtspMediaTransportContext& context) {
    RtspMediaTransportCreateResult result;
    result.response_spec = requested;

    if (requested.mode == RtspTransportMode::TcpInterleaved) {
        if (requested.rtp_channel > 255 || requested.rtcp_channel > 255) {
            result.error = "interleaved channel exceeds one-byte RTSP frame field";
            return result;
        }
        if (context.connection.expired()) {
            result.error = "TCP interleaved connection is unavailable";
            return result;
        }
        result.transport = std::make_shared<TcpInterleavedTransport>(
            requested,
            context.connection);
        return result;
    }

    if (requested.mode == RtspTransportMode::UdpUnicast) {
        if (requested.client_rtp_port == 0 || requested.client_rtcp_port == 0) {
            result.error = "UDP unicast transport requires client_port";
            return result;
        }
        boost::system::error_code ec;
        // 构造阶段同步 bind，成功后才启动 receive loop；这保证 SETUP 的
        // response port 与实际 socket 一一对应，失败时没有半初始化 track。
        auto transport = std::shared_ptr<UdpUnicastTransport>(
            new UdpUnicastTransport(requested, context, ec));
        if (ec) {
            result.error = "failed to initialize UDP unicast transport: " +
                           ec.message();
            return result;
        }
        transport->StartReceiveLoop();
        result.response_spec = transport->GetTransportSpec();
        result.transport = std::move(transport);
        return result;
    }

    result.error = "transport factory does not create multicast subscriptions";
    return result;
}
