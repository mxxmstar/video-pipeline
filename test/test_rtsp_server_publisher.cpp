#include "media/publisher/i_publisher.h"
#include "media/simple_buffer.h"

#include <boost/asio/ip/multicast.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint16_t kTcpTestPort = 18554;
constexpr std::uint16_t kUdpTestPort = 18555;
constexpr std::uint16_t kMulticastTestPort = 18556;
constexpr const char* kMulticastAddress = "239.255.42.1";

PublisherConfig MakeConfig(std::uint16_t port,
                           bool enable_udp,
                           std::uint16_t multicast_rtp_port = 0) {
    PublisherConfig config;
    config.mode = PublishMode::PullServer;
    config.protocol = PublishProtocol::RtspServer;
    config.listen_host = "127.0.0.1";
    config.listen_port = port;
    config.stream_path = "/live/test";
    config.rtsp.enable_udp = enable_udp;
    if (multicast_rtp_port != 0) {
        config.rtsp.multicast_address = kMulticastAddress;
        config.rtsp.multicast_rtp_port = multicast_rtp_port;
        config.rtsp.multicast_rtcp_port = multicast_rtp_port + 1;
        config.rtsp.multicast_ttl = 16;
    }

    MediaTrackConfig track;
    track.track_id = 0;
    track.media_type = MediaType::VIDEO;
    track.codec_type = CodecType::H264;
    track.width = 640;
    track.height = 360;
    track.fps = 25.0f;
    track.extra_data = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xe0, 0x1f,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x06, 0xe2,
    };
    config.tracks.push_back(track);
    return config;
}

std::string ReadUntilHeader(boost::asio::ip::tcp::socket& socket) {
    std::string data;
    std::array<char, 1024> chunk{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    while (std::chrono::steady_clock::now() < deadline) {
        boost::system::error_code ec;
        const auto n = socket.read_some(boost::asio::buffer(chunk), ec);
        if (!ec && n > 0) {
            data.append(chunk.data(), n);
            if (data.find("\r\n\r\n") != std::string::npos) {
                return data;
            }
        } else if (ec == boost::asio::error::would_block ||
                   ec == boost::asio::error::try_again) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else if (ec) {
            return data;
        }
    }

    return data;
}

bool ReadInterleavedFrame(boost::asio::ip::tcp::socket& socket,
                          std::vector<std::uint8_t>& frame) {
    std::array<std::uint8_t, 2048> chunk{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    while (std::chrono::steady_clock::now() < deadline) {
        boost::system::error_code ec;
        const auto n = socket.read_some(boost::asio::buffer(chunk), ec);
        if (!ec && n > 0) {
            frame.insert(frame.end(), chunk.data(), chunk.data() + n);
            auto dollar = std::find(frame.begin(), frame.end(), static_cast<std::uint8_t>('$'));
            if (dollar != frame.end() && std::distance(dollar, frame.end()) >= 4) {
                const auto offset = static_cast<std::size_t>(std::distance(frame.begin(), dollar));
                const auto length = static_cast<std::uint16_t>(
                    (frame[offset + 2] << 8) | frame[offset + 3]);
                if (frame.size() >= offset + 4 + length) {
                    frame.erase(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(offset));
                    frame.resize(4 + length);
                    return true;
                }
            }
        } else if (ec == boost::asio::error::would_block ||
                   ec == boost::asio::error::try_again) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else if (ec) {
            return false;
        }
    }

    return false;
}

bool ReadUdpPacket(boost::asio::ip::udp::socket& socket,
                   std::vector<std::uint8_t>& packet) {
    std::array<std::uint8_t, 2048> chunk{};
    boost::asio::ip::udp::endpoint sender;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    while (std::chrono::steady_clock::now() < deadline) {
        boost::system::error_code ec;
        const auto n = socket.receive_from(boost::asio::buffer(chunk), sender, 0, ec);
        if (!ec && n > 0) {
            packet.assign(chunk.data(), chunk.data() + n);
            return true;
        }
        if (ec == boost::asio::error::would_block ||
            ec == boost::asio::error::try_again) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (ec) {
            return false;
        }
    }

    return false;
}

int DrainUdpPackets(boost::asio::ip::udp::socket& socket,
                    std::chrono::milliseconds first_packet_timeout,
                    std::chrono::milliseconds quiet_timeout) {
    int count = 0;
    std::array<std::uint8_t, 2048> chunk{};
    boost::asio::ip::udp::endpoint sender;
    auto deadline = std::chrono::steady_clock::now() + first_packet_timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        boost::system::error_code ec;
        const auto n = socket.receive_from(boost::asio::buffer(chunk), sender, 0, ec);
        if (!ec && n > 0) {
            if (n >= 13 && chunk[0] == 0x80 && (chunk[1] & 0x7F) == 96) {
                ++count;
                deadline = std::chrono::steady_clock::now() + quiet_timeout;
            }
            continue;
        }
        if (ec == boost::asio::error::would_block ||
            ec == boost::asio::error::try_again) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (ec) {
            break;
        }
    }

    return count;
}

std::uint16_t FindFreeUdpPort() {
    boost::asio::io_context io;
    boost::asio::ip::udp::socket socket(io);
    socket.open(boost::asio::ip::udp::v4());
    socket.bind({boost::asio::ip::udp::v4(), 0});
    const auto port = socket.local_endpoint().port();
    return port == 65535 ? 5004 : port;
}

void SendRequest(boost::asio::ip::tcp::socket& socket, const std::string& request) {
    boost::asio::write(socket, boost::asio::buffer(request));
}

MediaPacket MakePacket() {
    std::vector<std::uint8_t> data{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xe0, 0x1f,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x06, 0xe2,
        0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84, 0x21,
    };

    MediaPacket packet;
    packet.type = MediaType::VIDEO;
    packet.codec = CodecType::H264;
    packet.pts = 40000;
    packet.dts = 40000;
    packet.time_base = Rational{1, 1000000};
    packet.keyframe = true;
    packet.buffer = std::make_shared<SimpleBuffer>(std::move(data));
    return packet;
}

void TestTcpInterleavedPublisher() {
    auto config = MakeConfig(kTcpTestPort, false);
    auto publisher = IPublisher::Create(config);
    assert(publisher);
    assert(publisher->Start(config));

    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket(io);
    socket.connect({boost::asio::ip::make_address("127.0.0.1"), kTcpTestPort});
    socket.non_blocking(true);

    const std::string url =
        "rtsp://127.0.0.1:" + std::to_string(kTcpTestPort) + "/live/test";
    SendRequest(socket,
                "DESCRIBE " + url + " RTSP/1.0\r\n"
                "CSeq: 1\r\n"
                "Accept: application/sdp\r\n\r\n");
    auto response = ReadUntilHeader(socket);
    assert(response.find("RTSP/1.0 200 OK") != std::string::npos);
    assert(response.find("sprop-parameter-sets") != std::string::npos);

    SendRequest(socket,
                "SETUP " + url + "/track0 RTSP/1.0\r\n"
                "CSeq: 2\r\n"
                "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    response = ReadUntilHeader(socket);
    assert(response.find("RTSP/1.0 200 OK") != std::string::npos);

    SendRequest(socket,
                "PLAY " + url + " RTSP/1.0\r\n"
                "CSeq: 3\r\n"
                "Session: 00000001\r\n\r\n");
    response = ReadUntilHeader(socket);
    assert(response.find("RTSP/1.0 200 OK") != std::string::npos);

    assert(publisher->Publish(MakePacket()));

    std::vector<std::uint8_t> frame;
    assert(ReadInterleavedFrame(socket, frame));
    assert(frame.size() >= 4 + 12 + 1);
    assert(frame[0] == '$');
    assert(frame[1] == 0);
    assert((frame[5] & 0x7F) == 96);

    publisher->Stop();
}

void TestUdpUnicastPublisher() {
    auto config = MakeConfig(kUdpTestPort, true);
    auto publisher = IPublisher::Create(config);
    assert(publisher);
    assert(publisher->Start(config));

    boost::asio::io_context io;
    boost::asio::ip::tcp::socket rtsp_socket(io);
    rtsp_socket.connect({boost::asio::ip::make_address("127.0.0.1"), kUdpTestPort});
    rtsp_socket.non_blocking(true);

    boost::asio::ip::udp::socket rtp_socket(io);
    rtp_socket.open(boost::asio::ip::udp::v4());
    rtp_socket.bind({boost::asio::ip::make_address("127.0.0.1"), 0});
    rtp_socket.non_blocking(true);

    boost::asio::ip::udp::socket rtcp_socket(io);
    rtcp_socket.open(boost::asio::ip::udp::v4());
    rtcp_socket.bind({boost::asio::ip::make_address("127.0.0.1"), 0});

    const auto client_rtp_port = rtp_socket.local_endpoint().port();
    const auto client_rtcp_port = rtcp_socket.local_endpoint().port();
    const std::string url =
        "rtsp://127.0.0.1:" + std::to_string(kUdpTestPort) + "/live/test";

    SendRequest(rtsp_socket,
                "DESCRIBE " + url + " RTSP/1.0\r\n"
                "CSeq: 1\r\n"
                "Accept: application/sdp\r\n\r\n");
    auto response = ReadUntilHeader(rtsp_socket);
    assert(response.find("RTSP/1.0 200 OK") != std::string::npos);

    SendRequest(rtsp_socket,
                "SETUP " + url + "/track0 RTSP/1.0\r\n"
                "CSeq: 2\r\n"
                "Transport: RTP/AVP;unicast;client_port=" +
                    std::to_string(client_rtp_port) + "-" +
                    std::to_string(client_rtcp_port) + "\r\n\r\n");
    response = ReadUntilHeader(rtsp_socket);
    assert(response.find("RTSP/1.0 200 OK") != std::string::npos);
    assert(response.find("Transport: RTP/AVP;unicast") != std::string::npos);
    assert(response.find("server_port=") != std::string::npos);

    SendRequest(rtsp_socket,
                "PLAY " + url + " RTSP/1.0\r\n"
                "CSeq: 3\r\n"
                "Session: 00000001\r\n\r\n");
    response = ReadUntilHeader(rtsp_socket);
    assert(response.find("RTSP/1.0 200 OK") != std::string::npos);

    assert(publisher->Publish(MakePacket()));

    std::vector<std::uint8_t> packet;
    assert(ReadUdpPacket(rtp_socket, packet));
    assert(packet.size() >= 12 + 1);
    assert(packet[0] == 0x80);
    assert((packet[1] & 0x7F) == 96);

    publisher->Stop();
}

void TestUdpMulticastPublisher() {
    const auto multicast_rtp_port = FindFreeUdpPort();
    auto config = MakeConfig(kMulticastTestPort, true, multicast_rtp_port);
    auto publisher = IPublisher::Create(config);
    assert(publisher);
    assert(publisher->Start(config));

    boost::asio::io_context io;
    boost::asio::ip::udp::socket rtp_socket(io);
    boost::system::error_code ec;
    rtp_socket.open(boost::asio::ip::udp::v4(), ec);
    assert(!ec);
    rtp_socket.set_option(boost::asio::socket_base::reuse_address(true), ec);
    assert(!ec);
    rtp_socket.bind({boost::asio::ip::udp::v4(), multicast_rtp_port}, ec);
    assert(!ec);
    // 优先加入 loopback 接口，失败时回退到系统默认组播接口。
    rtp_socket.set_option(boost::asio::ip::multicast::join_group(
        boost::asio::ip::make_address_v4(kMulticastAddress),
        boost::asio::ip::address_v4::loopback()),
        ec);
    if (ec) {
        rtp_socket.set_option(boost::asio::ip::multicast::join_group(
            boost::asio::ip::make_address_v4(kMulticastAddress)),
            ec);
    }
    assert(!ec);
    rtp_socket.non_blocking(true);

    const std::string url =
        "rtsp://127.0.0.1:" + std::to_string(kMulticastTestPort) + "/live/test";

    auto connect_multicast_client =
        [&](boost::asio::ip::tcp::socket& rtsp_socket, int cseq_base) {
            rtsp_socket.connect({boost::asio::ip::make_address("127.0.0.1"),
                                 kMulticastTestPort});
            rtsp_socket.non_blocking(true);

            SendRequest(rtsp_socket,
                        "DESCRIBE " + url + " RTSP/1.0\r\n"
                        "CSeq: " + std::to_string(cseq_base) + "\r\n"
                        "Accept: application/sdp\r\n\r\n");
            auto response = ReadUntilHeader(rtsp_socket);
            assert(response.find("RTSP/1.0 200 OK") != std::string::npos);
            assert(response.find(std::string{"c=IN IP4 "} + kMulticastAddress) !=
                   std::string::npos);
            assert(response.find("a=type:broadcast") != std::string::npos);
            assert(response.find(std::string{"m=video "} +
                                 std::to_string(multicast_rtp_port)) !=
                   std::string::npos);

            SendRequest(rtsp_socket,
                        "SETUP " + url + "/track0 RTSP/1.0\r\n"
                        "CSeq: " + std::to_string(cseq_base + 1) + "\r\n"
                        "Transport: RTP/AVP;multicast\r\n\r\n");
            response = ReadUntilHeader(rtsp_socket);
            assert(response.find("RTSP/1.0 200 OK") != std::string::npos);
            assert(response.find("Transport: RTP/AVP;multicast") != std::string::npos);
            assert(response.find(std::string{"destination="} + kMulticastAddress) !=
                   std::string::npos);
            assert(response.find(std::string{"port="} +
                                 std::to_string(multicast_rtp_port) + "-" +
                                 std::to_string(multicast_rtp_port + 1)) !=
                   std::string::npos);
            assert(response.find("source=127.0.0.1") != std::string::npos);
            assert(response.find("ttl=16") != std::string::npos);

            SendRequest(rtsp_socket,
                        "PLAY " + url + " RTSP/1.0\r\n"
                        "CSeq: " + std::to_string(cseq_base + 2) + "\r\n"
                        "Session: 00000001\r\n\r\n");
            response = ReadUntilHeader(rtsp_socket);
            assert(response.find("RTSP/1.0 200 OK") != std::string::npos);
        };

    boost::asio::ip::tcp::socket rtsp_socket_1(io);
    boost::asio::ip::tcp::socket rtsp_socket_2(io);
    connect_multicast_client(rtsp_socket_1, 1);
    connect_multicast_client(rtsp_socket_2, 11);

    assert(publisher->Publish(MakePacket()));

    const auto packet_count = DrainUdpPackets(
        rtp_socket,
        std::chrono::seconds(2),
        std::chrono::milliseconds(200));
    assert(packet_count >= 1);
    assert(packet_count <= 3);

    publisher->Stop();
}

} // namespace

int main() {
    TestTcpInterleavedPublisher();
    TestUdpUnicastPublisher();
    TestUdpMulticastPublisher();
    return 0;
}
