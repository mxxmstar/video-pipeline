#include "media/protocol/rtsp/rtsp_media_transport.h"

#include <array>
#include <cassert>
#include <chrono>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>

namespace {

using Udp = boost::asio::ip::udp;

void Pump(boost::asio::io_context& io) {
    io.poll();
    io.restart();
}

bool ReceiveEventually(boost::asio::io_context& io,
                       Udp::socket& socket,
                       std::vector<std::uint8_t>& output) {
    socket.non_blocking(true);
    std::array<std::uint8_t, 2048> buffer{};
    Udp::endpoint sender;
    for (int attempt = 0; attempt < 100; ++attempt) {
        Pump(io);
        boost::system::error_code ec;
        const auto bytes = socket.receive_from(
            boost::asio::buffer(buffer), sender, 0, ec);
        if (!ec && bytes != 0) {
            output.assign(buffer.begin(), buffer.begin() +
                                           static_cast<std::ptrdiff_t>(bytes));
            return true;
        }
        if (ec != boost::asio::error::would_block &&
            ec != boost::asio::error::try_again) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return false;
}

void TestFactoryRejectsIncompleteTransport() {
    boost::asio::io_context io;
    RtspMediaTransportContext context;
    context.io_executor = io.get_executor();
    context.remote_endpoint = {boost::asio::ip::address_v4::loopback(), 8554};

    RtspTransportSpec unknown;
    auto result = RtspMediaTransportFactory::Create(unknown, context);
    assert(!result);
    assert(!result.transport);

    RtspTransportSpec udp;
    udp.mode = RtspTransportMode::UdpUnicast;
    result = RtspMediaTransportFactory::Create(udp, context);
    assert(!result);
    assert(!result.transport);
}

void TestUdpTransportSendReceiveAndClose() {
    boost::asio::io_context io;

    Udp::socket rtp_receiver(io, {Udp::v4(), 0});
    Udp::socket rtcp_receiver(io, {Udp::v4(), 0});
    const auto client_rtp_port = rtp_receiver.local_endpoint().port();
    const auto client_rtcp_port = rtcp_receiver.local_endpoint().port();

    std::vector<std::uint8_t> received_rtcp;
    std::size_t rtcp_callback_count = 0;
    RtspMediaTransportContext context;
    context.io_executor = io.get_executor();
    context.remote_endpoint = {boost::asio::ip::address_v4::loopback(), 8554};
    context.local_endpoint = {boost::asio::ip::address_v4::loopback(), 8554};
    context.on_rtcp = [&received_rtcp, &rtcp_callback_count](
                          std::vector<std::uint8_t> packet,
                          std::string) {
        received_rtcp = std::move(packet);
        ++rtcp_callback_count;
    };

    RtspTransportSpec requested;
    requested.mode = RtspTransportMode::UdpUnicast;
    requested.client_rtp_port = client_rtp_port;
    requested.client_rtcp_port = client_rtcp_port;
    const auto result = RtspMediaTransportFactory::Create(requested, context);
    assert(result);
    assert(result.response_spec.server_rtp_port != 0);
    assert(result.response_spec.server_rtcp_port != 0);

    const auto transport = result.transport;
    assert(!transport->IsTcpInterleaved());
    assert(transport->SendRtp({0x80, 0x60, 0x01}));
    std::vector<std::uint8_t> received_rtp;
    assert(ReceiveEventually(io, rtp_receiver, received_rtp));
    assert(received_rtp == std::vector<std::uint8_t>({0x80, 0x60, 0x01}));

    assert(transport->SendRtcp({0x80, 0xc8, 0x00, 0x01}));
    std::vector<std::uint8_t> received_sender_report;
    assert(ReceiveEventually(io, rtcp_receiver, received_sender_report));
    assert(received_sender_report ==
           std::vector<std::uint8_t>({0x80, 0xc8, 0x00, 0x01}));

    const Udp::endpoint server_rtcp_endpoint(
        boost::asio::ip::address_v4::loopback(),
        result.response_spec.server_rtcp_port);
    rtcp_receiver.send_to(boost::asio::buffer(std::array<std::uint8_t, 4>{
                                 0x80, 0xc9, 0x00, 0x01}),
                           server_rtcp_endpoint);
    for (int attempt = 0; attempt < 20 && received_rtcp.empty(); ++attempt) {
        Pump(io);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(received_rtcp == std::vector<std::uint8_t>({0x80, 0xc9, 0x00, 0x01}));

    // 非 SETUP client_port 的来源必须被忽略，不能改变已接受的 RR 快照。
    Udp::socket unexpected_sender(io, {Udp::v4(), 0});
    unexpected_sender.send_to(boost::asio::buffer(std::array<std::uint8_t, 4>{
                                      0x80, 0xc9, 0x00, 0x01}),
                               server_rtcp_endpoint);
    for (int attempt = 0; attempt < 20; ++attempt) {
        Pump(io);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    assert(rtcp_callback_count == 1);

    transport->Close();
    transport->Close();
    assert(!transport->SendRtp({0x80}));
    unexpected_sender.close();
    rtp_receiver.close();
    rtcp_receiver.close();
}

} // namespace

int main() {
    TestFactoryRejectsIncompleteTransport();
    TestUdpTransportSendReceiveAndClose();
    return 0;
}
