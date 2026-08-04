#include "media/protocol/rtsp/rtsp_multicast_publisher.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/ip/multicast.hpp>

namespace {

using boost::asio::ip::udp;

std::uint16_t ReadU16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8) |
        static_cast<std::uint16_t>(data[1]));
}

void WriteU16(std::uint8_t* data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>(value >> 8);
    data[1] = static_cast<std::uint8_t>(value & 0xffU);
}

void WriteU32(std::uint8_t* data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>(value >> 24);
    data[1] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
    data[2] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    data[3] = static_cast<std::uint8_t>(value & 0xffU);
}

std::uint16_t FindFreeEvenPort() {
    // 先取得一个系统分配的端口，再检查相邻的奇数端口是否也可绑定。
    // publisher 的 RTCP 端口固定为 RTP+1，测试必须避免拿到奇数 RTP 端口。
    for (int attempt = 0; attempt < 64; ++attempt) {
        boost::asio::io_context io;
        udp::socket rtp(io);
        boost::system::error_code ec;
        rtp.open(udp::v4(), ec);
        if (ec) {
            continue;
        }
        rtp.bind({udp::v4(), 0}, ec);
        if (ec) {
            continue;
        }
        const auto port = rtp.local_endpoint().port();
        if (port >= 65534 || (port & 1U) != 0) {
            continue;
        }

        udp::socket rtcp(io);
        rtcp.open(udp::v4(), ec);
        if (ec) {
            continue;
        }
        rtcp.bind({udp::v4(), static_cast<std::uint16_t>(port + 1)}, ec);
        if (!ec) {
            return port;
        }
    }
    return 52004;
}

void BindAndJoin(udp::socket& socket,
                 const boost::asio::ip::address_v4& group,
                 std::uint16_t port) {
    boost::system::error_code ec;
    socket.open(udp::v4(), ec);
    assert(!ec);
    socket.set_option(boost::asio::socket_base::reuse_address(true), ec);
    assert(!ec);
    socket.bind({udp::v4(), port}, ec);
    assert(!ec);

    // publisher 使用 loopback source 时也会按 loopback interface 加入组播组。
    // 某些 Windows 网络栈不接受显式 interface，因此保留无 interface 的回退。
    socket.set_option(boost::asio::ip::multicast::join_group(
        group, boost::asio::ip::address_v4::loopback()), ec);
    if (ec) {
        socket.set_option(boost::asio::ip::multicast::join_group(group), ec);
    }
    assert(!ec);
    socket.non_blocking(true, ec);
    assert(!ec);
}

bool ReceivePacket(udp::socket& socket,
                   std::vector<std::uint8_t>& packet,
                   std::chrono::milliseconds timeout) {
    std::array<std::uint8_t, 2048> buffer{};
    udp::endpoint sender;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        boost::system::error_code ec;
        const auto size = socket.receive_from(boost::asio::buffer(buffer), sender, 0, ec);
        if (!ec && size != 0) {
            packet.assign(buffer.begin(), buffer.begin() + size);
            return true;
        }
        if (ec && ec != boost::asio::error::would_block &&
            ec != boost::asio::error::try_again) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

std::vector<std::uint8_t> MakeReceiverReport(std::uint32_t reporter_ssrc,
                                             std::uint32_t media_ssrc,
                                             std::uint32_t highest_sequence) {
    // 一个 RR packet 加一个 report block，字段布局对应 RFC 3550 6.4.2。
    std::vector<std::uint8_t> packet(32, 0);
    packet[0] = 0x81; // RTP version 2, RC=1
    packet[1] = 201;  // Receiver Report
    WriteU16(packet.data() + 2, 7);
    WriteU32(packet.data() + 4, reporter_ssrc);
    WriteU32(packet.data() + 8, media_ssrc);
    WriteU32(packet.data() + 16, highest_sequence);
    return packet;
}

void TestValidationAndLifecycle() {
    boost::asio::io_context io;
    RtspServerOptions options;

    options.multicast_address = "127.0.0.1";
    auto invalid_address = std::make_shared<RtspMulticastPublisher>(
        io.get_executor(),
        options,
        96,
        90000,
        RtspMulticastPublisher::ReceiverReportHandler{});
    RtspTransportSpec spec;
    spec.mode = RtspTransportMode::UdpMulticast;
    spec.multicast = true;
    spec.unicast = false;
    assert(!invalid_address->Configure(spec, "127.0.0.1"));

    options.multicast_address.clear();
    auto empty_address = std::make_shared<RtspMulticastPublisher>(
        io.get_executor(),
        options,
        96,
        90000,
        RtspMulticastPublisher::ReceiverReportHandler{});
    spec = {};
    spec.mode = RtspTransportMode::UdpMulticast;
    assert(!empty_address->Configure(spec, "127.0.0.1"));

    options.multicast_address = "239.255.0.7";
    options.multicast_rtp_port = 0;
    options.multicast_rtcp_port = 0;
    auto invalid_ports = std::make_shared<RtspMulticastPublisher>(
        io.get_executor(),
        options,
        96,
        90000,
        RtspMulticastPublisher::ReceiverReportHandler{});
    spec = {};
    spec.mode = RtspTransportMode::UdpMulticast;
    assert(!invalid_ports->Configure(spec, "127.0.0.1"));
}

void TestSharedTransportPublishAndFeedback() {
    boost::asio::io_context io;
    const auto group = boost::asio::ip::make_address_v4("239.255.0.7");
    const auto rtp_port = FindFreeEvenPort();

    RtspServerOptions options;
    options.multicast_address = group.to_string();
    options.multicast_rtp_port = rtp_port;
    options.multicast_rtcp_port = static_cast<std::uint16_t>(rtp_port + 1);
    options.multicast_ttl = 1;

    std::atomic<std::uint64_t> callback_reports{0};
    auto publisher = std::make_shared<RtspMulticastPublisher>(
        io.get_executor(),
        options,
        96,
        90000,
        [&callback_reports](std::uint64_t count) {
            callback_reports.fetch_add(count, std::memory_order_relaxed);
        });

    RtspTransportSpec first_spec;
    first_spec.mode = RtspTransportMode::UdpMulticast;
    first_spec.unicast = false;
    first_spec.multicast = true;
    assert(publisher->Configure(first_spec, "127.0.0.1"));
    assert(publisher->IsReady());

    // 后续 session 的 SETUP 只能复用第一次配置，不得修改共享 endpoint。
    RtspTransportSpec second_spec;
    second_spec.mode = RtspTransportMode::UdpMulticast;
    second_spec.unicast = false;
    second_spec.multicast = true;
    second_spec.destination = "239.255.0.8";
    second_spec.server_rtp_port = 40000;
    assert(publisher->Configure(second_spec, "127.0.0.1"));
    assert(second_spec.destination == first_spec.destination);
    assert(second_spec.server_rtp_port == first_spec.server_rtp_port);
    assert(second_spec.server_rtcp_port == first_spec.server_rtcp_port);

    udp::socket rtp_receiver(io);
    udp::socket rtcp_receiver(io);
    BindAndJoin(rtp_receiver, group, rtp_port);
    BindAndJoin(rtcp_receiver, group, static_cast<std::uint16_t>(rtp_port + 1));

    std::thread io_thread([&io]() { io.run(); });

    const auto initial_sequence = publisher->GetSequence();
    RtpPayload payload;
    payload.timestamp = 123456;
    payload.marker = true;
    payload.payload = {1, 2, 3, 4};
    publisher->Publish(payload, 96);

    std::vector<std::uint8_t> rtp_packet;
    assert(ReceivePacket(rtp_receiver, rtp_packet, std::chrono::seconds(2)));
    assert(rtp_packet.size() == 16);
    assert(rtp_packet[0] == 0x80);
    assert(rtp_packet[1] == 0xe0); // marker=1, PT=96
    assert(ReadU16(rtp_packet.data() + 2) == initial_sequence);

    const auto after_publish = publisher->GetStats();
    assert(after_publish.ready);
    assert(after_publish.sender.packet_count == 1);
    assert(after_publish.sender.octet_count == payload.payload.size());
    assert(after_publish.sender.next_sequence ==
           static_cast<std::uint16_t>(initial_sequence + 1));

    std::vector<std::uint8_t> rtcp_packet;
    // 首个 RTP packet 会触发共享 sender 的 Sender Report；确认 SR 与 RTP
    // 使用同一个 multicast RTCP endpoint，而不是被误发到 unicast 端口。
    assert(ReceivePacket(rtcp_receiver, rtcp_packet, std::chrono::seconds(2)));
    assert(rtcp_packet.size() >= 28);
    assert(rtcp_packet[0] == 0x80);
    assert(rtcp_packet[1] == 200); // Sender Report

    // RR 必须发送到组播 RTCP 端口，并且只有 source SSRC 匹配当前 sender 时才计数。
    udp::socket rr_sender(io);
    boost::system::error_code ec;
    rr_sender.open(udp::v4(), ec);
    assert(!ec);
    rr_sender.bind({boost::asio::ip::address_v4::loopback(), 0}, ec);
    assert(!ec);
    rr_sender.set_option(boost::asio::ip::multicast::outbound_interface(
        boost::asio::ip::address_v4::loopback()), ec);
    if (ec) {
        rr_sender.set_option(boost::asio::ip::multicast::enable_loopback(true), ec);
    }

    const auto reporter_ssrc = 0x11223344U;
    const auto media_ssrc = after_publish.sender.ssrc;
    auto rr = MakeReceiverReport(reporter_ssrc, media_ssrc, initial_sequence);
    rr_sender.send_to(boost::asio::buffer(rr),
                      {group, static_cast<std::uint16_t>(rtp_port + 1)},
                      0,
                      ec);
    assert(!ec);

    const auto report_deadline = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < report_deadline &&
           publisher->GetStats().receiver_reports_received < 1) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto stats = publisher->GetStats();
    assert(stats.receiver_reports_received == 1);
    assert(callback_reports.load(std::memory_order_relaxed) == 1);
    assert(stats.feedback_by_reporter.at(reporter_ssrc).reports_received == 1);

    rr = MakeReceiverReport(reporter_ssrc, media_ssrc, initial_sequence + 1);
    rr_sender.send_to(boost::asio::buffer(rr),
                      {group, static_cast<std::uint16_t>(rtp_port + 1)},
                      0,
                      ec);
    assert(!ec);
    const auto second_report_deadline = std::chrono::steady_clock::now() +
                                        std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < second_report_deadline &&
           publisher->GetStats().receiver_reports_received < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stats = publisher->GetStats();
    assert(stats.receiver_reports_received == 2);
    assert(stats.feedback_by_reporter.at(reporter_ssrc).reports_received == 2);
    assert(stats.feedback_by_reporter.at(reporter_ssrc).report.extended_highest_sequence ==
           initial_sequence + 1);

    publisher->Close();
    publisher->Close(); // Close 必须幂等，且不会重新打开接收循环。
    assert(!publisher->IsReady());
    assert(!publisher->Configure(first_spec, "127.0.0.1"));
    publisher->Publish(payload, 96);
    // Close 会释放 sender，关闭后的 stats 不再暴露旧计数；这里确认 Publish
    // 没有重新创建 sender 或重新启动发送路径。
    assert(!publisher->GetStats().ready);
    assert(publisher->GetStats().sender.packet_count == 0);

    io.stop();
    io_thread.join();
}

} // namespace

int main() {
    TestValidationAndLifecycle();
    TestSharedTransportPublishAndFeedback();
    return 0;
}
