#include "media/protocol/rtsp/rtsp_multicast_publisher.h"

#include "common/log/logger.h"

#include <array>
#include <utility>

#include <boost/asio/ip/multicast.hpp>
#include <boost/asio/post.hpp>

namespace {

std::uint32_t ReadU32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

} // namespace

RtspMulticastPublisher::RtspMulticastPublisher(
    boost::asio::any_io_executor io_executor,
    RtspServerOptions options,
    std::uint8_t payload_type,
    std::uint32_t clock_rate,
    ReceiverReportHandler on_receiver_reports)
    : io_executor_(std::move(io_executor)),
      options_(std::move(options)),
      payload_type_(payload_type),
      clock_rate_(clock_rate == 0 ? 90000 : clock_rate),
      on_receiver_reports_(std::move(on_receiver_reports)) {
}

bool RtspMulticastPublisher::IsIpv4Multicast(
    const boost::asio::ip::address& address) {
    if (!address.is_v4()) {
        return false;
    }
    const auto bytes = address.to_v4().to_bytes();
    return bytes[0] >= 224 && bytes[0] <= 239;
}

bool RtspMulticastPublisher::Configure(
    RtspTransportSpec& transport_spec,
    const std::string& source_address) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) {
        LOG_WARN("RTSP multicast publisher rejected Configure after Close");
        return false;
    }
    if (ready_) {
        // 组播是共享传输，后续客户端复用同一个组播地址、端口和 RTP 序列空间。
        transport_spec = transport_spec_;
        return true;
    }

    auto destination_text = transport_spec.destination.empty()
        ? options_.multicast_address
        : transport_spec.destination;
    if (destination_text.empty()) {
        LOG_WARN("RTSP SETUP multicast rejected: empty destination");
        return false;
    }

    boost::system::error_code ec;
    const auto destination = boost::asio::ip::make_address(destination_text, ec);
    if (ec || !IsIpv4Multicast(destination)) {
        LOG_WARN("RTSP SETUP multicast rejected destination '{}': {}",
                 destination_text,
                 ec ? ec.message() : "not an IPv4 multicast address");
        return false;
    }

    if (transport_spec.server_rtp_port == 0) {
        transport_spec.server_rtp_port = options_.multicast_rtp_port;
    }
    if (transport_spec.server_rtcp_port == 0) {
        transport_spec.server_rtcp_port = options_.multicast_rtcp_port;
    }
    if (transport_spec.server_rtp_port == 0 ||
        transport_spec.server_rtcp_port == 0) {
        LOG_WARN("RTSP SETUP multicast rejected: invalid RTP/RTCP port");
        return false;
    }
    if (transport_spec.ttl == 0) {
        transport_spec.ttl = options_.multicast_ttl == 0
            ? std::uint8_t{16}
            : options_.multicast_ttl;
    }

    auto bind_address = boost::asio::ip::address{
        boost::asio::ip::address_v4::any()};
    boost::system::error_code source_ec;
    const auto source = boost::asio::ip::make_address(source_address, source_ec);
    if (!source_ec && source.is_v4()) {
        bind_address = source;
        transport_spec.source = source.to_string();
    }

    // 三只 socket 先在局部 shared_ptr 中完成全部 open/bind/option；只有
    // 最后一步成功才写入成员，因此任意失败路径都不会留下半初始化资源。
    auto rtp_socket = std::make_shared<boost::asio::ip::udp::socket>(io_executor_);
    auto rtcp_socket = std::make_shared<boost::asio::ip::udp::socket>(io_executor_);
    auto rtcp_receiver_socket =
        std::make_shared<boost::asio::ip::udp::socket>(io_executor_);

    rtp_socket->open(boost::asio::ip::udp::v4(), ec);
    if (ec) {
        LOG_WARN("RTSP SETUP multicast RTP socket open failed: {}", ec.message());
        return false;
    }
    rtcp_socket->open(boost::asio::ip::udp::v4(), ec);
    if (ec) {
        LOG_WARN("RTSP SETUP multicast RTCP socket open failed: {}", ec.message());
        return false;
    }
    rtcp_receiver_socket->open(boost::asio::ip::udp::v4(), ec);
    if (ec) {
        LOG_WARN("RTSP SETUP multicast RTCP receiver socket open failed: {}",
                 ec.message());
        return false;
    }

    // RTP 和 RTCP SR 是发送 socket：绑定本地源地址的临时端口，然后发到组播组。
    rtp_socket->bind({bind_address, 0}, ec);
    if (ec) {
        LOG_WARN("RTSP SETUP multicast RTP socket bind failed: {}", ec.message());
        return false;
    }
    rtcp_socket->bind({bind_address, 0}, ec);
    if (ec) {
        LOG_WARN("RTSP SETUP multicast RTCP socket bind failed: {}", ec.message());
        return false;
    }

    // RTCP RR 接收 socket 必须绑定宣告的组播 RTCP 端口并加入 group。
    rtcp_receiver_socket->set_option(
        boost::asio::socket_base::reuse_address(true), ec);
    if (ec) {
        LOG_WARN("RTSP SETUP multicast RTCP receiver set reuse failed: {}",
                 ec.message());
    }
    rtcp_receiver_socket->bind(
        {boost::asio::ip::udp::v4(), transport_spec.server_rtcp_port}, ec);
    if (ec) {
        LOG_WARN("RTSP SETUP multicast RTCP receiver bind failed: {}", ec.message());
        return false;
    }

    if (bind_address.is_v4() &&
        bind_address.to_v4() != boost::asio::ip::address_v4::any()) {
        rtcp_receiver_socket->set_option(
            boost::asio::ip::multicast::join_group(destination.to_v4(),
                                                   bind_address.to_v4()),
            ec);
        if (ec) {
            rtcp_receiver_socket->set_option(
                boost::asio::ip::multicast::join_group(destination.to_v4()), ec);
        }
    } else {
        rtcp_receiver_socket->set_option(
            boost::asio::ip::multicast::join_group(destination.to_v4()), ec);
    }
    if (ec) {
        LOG_WARN("RTSP SETUP multicast RTCP receiver join group failed: {}",
                 ec.message());
        return false;
    }

    for (auto* socket : {rtp_socket.get(), rtcp_socket.get()}) {
        socket->set_option(boost::asio::ip::multicast::hops(transport_spec.ttl), ec);
        if (ec) {
            LOG_WARN("RTSP SETUP multicast set TTL failed: {}", ec.message());
        }
        socket->set_option(boost::asio::ip::multicast::enable_loopback(true), ec);
        if (ec) {
            LOG_WARN("RTSP SETUP multicast enable loopback failed: {}", ec.message());
        }
        if (bind_address.is_v4() &&
            bind_address.to_v4() != boost::asio::ip::address_v4::any()) {
            socket->set_option(
                boost::asio::ip::multicast::outbound_interface(bind_address.to_v4()),
                ec);
            if (ec) {
                LOG_WARN("RTSP SETUP multicast outbound interface failed: {}",
                         ec.message());
            }
        }
    }

    transport_spec.destination = destination.to_string();
    rtp_endpoint_ = {destination, transport_spec.server_rtp_port};
    rtcp_endpoint_ = {destination, transport_spec.server_rtcp_port};
    transport_spec_ = transport_spec;
    rtp_socket_ = std::move(rtp_socket);
    rtcp_socket_ = std::move(rtcp_socket);
    rtcp_receiver_socket_ = std::move(rtcp_receiver_socket);
    sender_ = std::make_unique<RtpSender>(
        RtpSender::CreateDefault(payload_type_, clock_rate_));
    feedback_by_reporter_.clear();
    receiver_reports_received_ = 0;
    ready_ = true;

    const auto log_address = rtp_endpoint_.address().to_string();
    const auto log_port = rtp_endpoint_.port();
    const auto log_ttl = transport_spec.ttl;
    lock.unlock();
    StartRtcpReceive();

    LOG_INFO("RTSP SETUP shared multicast transport: destination={}:{} ttl={}",
             log_address,
             log_port,
             static_cast<int>(log_ttl));
    return true;
}

void RtspMulticastPublisher::Publish(const RtpPayload& payload,
                                     std::uint8_t payload_type) {
    std::shared_ptr<boost::asio::ip::udp::socket> rtp_socket;
    std::shared_ptr<boost::asio::ip::udp::socket> rtcp_socket;
    std::shared_ptr<std::vector<std::uint8_t>> packet;
    std::shared_ptr<std::vector<std::uint8_t>> report;
    boost::asio::ip::udp::endpoint rtp_endpoint;
    boost::asio::ip::udp::endpoint rtcp_endpoint;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ready_ || !sender_ || !rtp_socket_ || !rtp_socket_->is_open()) {
            return;
        }
        auto rtp_packet = sender_->BuildRtpPacket(payload, payload_type);
        if (rtp_packet.empty()) {
            return;
        }
        rtp_socket = rtp_socket_;
        rtcp_socket = rtcp_socket_;
        rtp_endpoint = rtp_endpoint_;
        rtcp_endpoint = rtcp_endpoint_;
        packet = std::make_shared<std::vector<std::uint8_t>>(std::move(rtp_packet));
        if (sender_->ShouldSendSenderReport()) {
            report = std::make_shared<std::vector<std::uint8_t>>(
                sender_->BuildSenderReport());
        }
    }

    boost::asio::post(
        io_executor_,
        [rtp_socket, rtp_endpoint, packet]() {
            if (!rtp_socket || !rtp_socket->is_open()) {
                return;
            }
            rtp_socket->async_send_to(
                boost::asio::buffer(*packet),
                rtp_endpoint,
                [packet](boost::system::error_code ec, std::size_t) {
                    if (ec && ec != boost::asio::error::operation_aborted) {
                        LOG_WARN("RTSP multicast RTP send failed: {}", ec.message());
                    }
                });
        });

    if (report && rtcp_socket) {
        boost::asio::post(
            io_executor_,
            [rtcp_socket, rtcp_endpoint, report]() {
                if (!rtcp_socket || !rtcp_socket->is_open()) {
                    return;
                }
                rtcp_socket->async_send_to(
                    boost::asio::buffer(*report),
                    rtcp_endpoint,
                    [report](boost::system::error_code ec, std::size_t) {
                        if (ec && ec != boost::asio::error::operation_aborted) {
                            LOG_WARN("RTSP multicast RTCP sender report send failed: {}",
                                     ec.message());
                        }
                    });
            });
    }
}

void RtspMulticastPublisher::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        return;
    }
    closed_ = true;
    ready_ = false;
    CloseSocket(rtp_socket_);
    CloseSocket(rtcp_socket_);
    CloseSocket(rtcp_receiver_socket_);
    // 关闭操作完成后立即断开成员所有权。这样晚到的异步回调即使仍然持有
    // socket shared_ptr，也无法被误认为是当前配置的接收 socket。
    rtp_socket_.reset();
    rtcp_socket_.reset();
    rtcp_receiver_socket_.reset();
    sender_.reset();
    transport_spec_ = {};
    rtp_endpoint_ = {};
    rtcp_endpoint_ = {};
    feedback_by_reporter_.clear();
    receiver_reports_received_ = 0;
}

void RtspMulticastPublisher::CloseSocket(
    const std::shared_ptr<boost::asio::ip::udp::socket>& socket) {
    if (!socket) {
        return;
    }
    boost::system::error_code ignored;
    socket->cancel(ignored);
    socket->close(ignored);
}

bool RtspMulticastPublisher::IsReady() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_;
}

RtspTransportSpec RtspMulticastPublisher::GetTransportSpec() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return transport_spec_;
}

std::uint16_t RtspMulticastPublisher::GetSequence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sender_ ? sender_->Snapshot().next_sequence : 0;
}

std::uint32_t RtspMulticastPublisher::GetLastRtpTimestamp() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sender_ ? sender_->Snapshot().last_rtp_timestamp : 0;
}

RtspMulticastPublisherStatsSnapshot RtspMulticastPublisher::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    RtspMulticastPublisherStatsSnapshot stats;
    stats.ready = ready_;
    if (sender_) {
        stats.sender = sender_->Snapshot();
    }
    stats.receiver_reports_received = receiver_reports_received_;
    for (const auto& [reporter_ssrc, feedback] : feedback_by_reporter_) {
        stats.feedback_by_reporter.emplace(
            reporter_ssrc,
            RtspMulticastReceiverFeedbackSnapshot{
                feedback.report,
                feedback.reports_received});
    }
    return stats;
}

void RtspMulticastPublisher::StartRtcpReceive() {
    std::shared_ptr<boost::asio::ip::udp::socket> socket;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || !ready_ || !rtcp_receiver_socket_ ||
            !rtcp_receiver_socket_->is_open()) {
            return;
        }
        socket = rtcp_receiver_socket_;
    }

    // 同一时刻只挂一个 receive；完成回调根据 socket identity 判断是否仍
    // 属于当前配置，Close 后的晚到 callback 不能重新进入 receive loop。
    // endpoint 使用本次 receive 独有的 shared_ptr，避免 Close() 清理成员时
    // 与操作系统写入异步 receive endpoint 发生数据竞争。
    // buffer 也属于本次 receive；即使 publisher 在 Stop() 后被析构，socket
    // 的晚到 callback 仍然持有完整的读缓冲，不会写入已释放的对象成员。
    const auto sender_endpoint =
        std::make_shared<boost::asio::ip::udp::endpoint>();
    const auto read_buffer =
        std::make_shared<std::array<std::uint8_t, 2048>>();
    const auto weak = weak_from_this();
    socket->async_receive_from(
        boost::asio::buffer(*read_buffer),
        *sender_endpoint,
        [weak, socket, sender_endpoint, read_buffer](
            boost::system::error_code ec,
            std::size_t bytes) {
            const auto self = weak.lock();
            if (!self) {
                return;
            }
            if (ec) {
                bool closed = false;
                {
                    std::lock_guard<std::mutex> lock(self->mutex_);
                    closed = self->closed_;
                }
                if (ec != boost::asio::error::operation_aborted &&
                    !closed) {
                    LOG_DEBUG("RTSP multicast RTCP receive failed: {}", ec.message());
                }
                return;
            }

            boost::asio::ip::udp::endpoint received_from;
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                if (self->closed_ || !self->ready_ ||
                    self->rtcp_receiver_socket_ != socket) {
                    return;
                }
                received_from = *sender_endpoint;
            }
            self->HandleRtcpPacket(read_buffer->data(),
                                   bytes,
                                   received_from);
            bool should_continue = false;
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                should_continue = !self->closed_ && self->ready_ &&
                                  self->rtcp_receiver_socket_ == socket;
            }
            if (should_continue) {
                self->StartRtcpReceive();
            }
        });
}

void RtspMulticastPublisher::HandleRtcpPacket(
    const std::uint8_t* data,
    std::size_t size,
    const boost::asio::ip::udp::endpoint& sender_endpoint) {
    const auto parsed = RtcpPacketCodec::ParseCompound(data, size);
    if (!parsed.valid) {
        LOG_DEBUG("RTSP multicast RTCP packet ignored: endpoint={}:{} version={}, "
                  "size={}, remaining={}",
                  sender_endpoint.address().to_string(),
                  sender_endpoint.port(),
                  static_cast<int>(parsed.invalid_version),
                  parsed.invalid_packet_size,
                  size - parsed.error_offset);
        return;
    }

    for (const auto& parsed_packet : parsed.packets) {
        const auto* packet = parsed_packet.bytes.data();
        const auto packet_size = parsed_packet.bytes.size();
        if (parsed_packet.packet_type == RtcpPacketCodec::kReceiverReportPacketType) {
            ParseReceiverReport(packet,
                                packet_size,
                                parsed_packet.report_count,
                                sender_endpoint);
        } else if (parsed_packet.packet_type == RtcpPacketCodec::kSenderReportPacketType) {
            ParseSenderReport(packet,
                              packet_size,
                              parsed_packet.report_count,
                              sender_endpoint);
        } else {
            LOG_DEBUG("RTSP multicast RTCP {} received: endpoint={}:{} size={}",
                      RtcpPacketCodec::PacketTypeName(parsed_packet.packet_type),
                      sender_endpoint.address().to_string(),
                      sender_endpoint.port(),
                      packet_size);
        }
    }
    if (parsed.trailing_bytes != 0) {
        LOG_DEBUG("RTSP multicast RTCP trailing bytes ignored: endpoint={}:{} bytes={}",
                  sender_endpoint.address().to_string(),
                  sender_endpoint.port(),
                  parsed.trailing_bytes);
    }
}

void RtspMulticastPublisher::ParseReceiverReport(
    const std::uint8_t* packet,
    std::size_t packet_size,
    std::uint8_t report_count,
    const boost::asio::ip::udp::endpoint& sender_endpoint) {
    if (packet_size < 8U + static_cast<std::size_t>(report_count) * 24U) {
        LOG_DEBUG("RTSP multicast RTCP RR ignored: endpoint={}:{} size={}, rc={}",
                  sender_endpoint.address().to_string(),
                  sender_endpoint.port(),
                  packet_size,
                  static_cast<int>(report_count));
        return;
    }
    ParseReportBlocks(packet + 8,
                      report_count,
                      ReadU32(packet + 4),
                      "RR",
                      sender_endpoint);
}

void RtspMulticastPublisher::ParseSenderReport(
    const std::uint8_t* packet,
    std::size_t packet_size,
    std::uint8_t report_count,
    const boost::asio::ip::udp::endpoint& sender_endpoint) {
    if (packet_size < 28U + static_cast<std::size_t>(report_count) * 24U) {
        LOG_DEBUG("RTSP multicast RTCP SR ignored: endpoint={}:{} size={}, rc={}",
                  sender_endpoint.address().to_string(),
                  sender_endpoint.port(),
                  packet_size,
                  static_cast<int>(report_count));
        return;
    }
    ParseReportBlocks(packet + 28,
                      report_count,
                      ReadU32(packet + 4),
                      "SR",
                      sender_endpoint);
}

void RtspMulticastPublisher::ParseReportBlocks(
    const std::uint8_t* blocks,
    std::uint8_t report_count,
    std::uint32_t reporter_ssrc,
    const char* packet_type_name,
    const boost::asio::ip::udp::endpoint& sender_endpoint) {
    if (report_count == 0) {
        LOG_DEBUG("RTSP multicast RTCP {} received without report blocks: "
                  "endpoint={}:{} reporter_ssrc={}",
                  packet_type_name,
                  sender_endpoint.address().to_string(),
                  sender_endpoint.port(),
                  reporter_ssrc);
        return;
    }

    for (std::uint8_t index = 0; index < report_count; ++index) {
        RtcpReportBlock block;
        if (!RtcpPacketCodec::ReadReportBlock(
                blocks + static_cast<std::size_t>(index) * 24U,
                24,
                block)) {
            return;
        }

        bool matches_sender = false;
        std::uint64_t reports_received = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto sender_ssrc = sender_ ? sender_->Snapshot().ssrc : 0;
            matches_sender = ready_ && sender_ && block.source_ssrc == sender_ssrc;
            if (matches_sender) {
                // 每个 multicast receiver 用自己的 reporter SSRC 聚合最新反馈。
                auto& feedback = feedback_by_reporter_[reporter_ssrc];
                feedback.report = block;
                reports_received = ++feedback.reports_received;
                ++receiver_reports_received_;
            }
        }

        if (matches_sender && on_receiver_reports_) {
            on_receiver_reports_(1);
        }
        LOG_DEBUG("RTSP multicast RTCP {} block: endpoint={}:{} reporter_ssrc={}, "
                  "media_ssrc={}, match={}, receiver_reports={}, fraction_lost={}, "
                  "cumulative_lost={}, highest_seq={}, jitter={}, lsr={}, dlsr={}",
                  packet_type_name,
                  sender_endpoint.address().to_string(),
                  sender_endpoint.port(),
                  reporter_ssrc,
                  block.source_ssrc,
                  matches_sender,
                  reports_received,
                  static_cast<int>(block.fraction_lost),
                  block.cumulative_lost,
                  block.extended_highest_sequence,
                  block.jitter,
                  block.last_sender_report,
                  block.delay_since_last_sender_report);
    }
}
