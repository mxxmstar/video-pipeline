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
#include <utility>
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

bool ReadInterleavedFrames(boost::asio::ip::tcp::socket& socket,
                           std::vector<std::vector<std::uint8_t>>& frames,
                           std::size_t min_frame_count) {
    std::array<std::uint8_t, 4096> chunk{};
    std::vector<std::uint8_t> buffer;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

    while (std::chrono::steady_clock::now() < deadline) {
        boost::system::error_code ec;
        const auto n = socket.read_some(boost::asio::buffer(chunk), ec);
        if (!ec && n > 0) {
            buffer.insert(buffer.end(), chunk.data(), chunk.data() + n);

            while (true) {
                auto dollar = std::find(buffer.begin(),
                                        buffer.end(),
                                        static_cast<std::uint8_t>('$'));
                if (dollar == buffer.end()) {
                    buffer.clear();
                    break;
                }

                if (dollar != buffer.begin()) {
                    buffer.erase(buffer.begin(), dollar);
                }
                if (buffer.size() < 4) {
                    break;
                }

                const auto length = static_cast<std::uint16_t>(
                    (buffer[2] << 8) | buffer[3]);
                const auto frame_size = 4 + length;
                if (buffer.size() < frame_size) {
                    break;
                }

                frames.emplace_back(buffer.begin(),
                                    buffer.begin() +
                                        static_cast<std::ptrdiff_t>(frame_size));
                buffer.erase(buffer.begin(),
                             buffer.begin() +
                                 static_cast<std::ptrdiff_t>(frame_size));
                if (frames.size() >= min_frame_count) {
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

    return frames.size() >= min_frame_count;
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

bool IsRtcpSenderReport(const std::vector<std::uint8_t>& packet,
                        std::size_t offset = 0) {
    return packet.size() >= offset + 28 &&
           packet[offset] == 0x80 &&
           packet[offset + 1] == 200 &&
           packet[offset + 2] == 0 &&
           packet[offset + 3] == 6;
}

std::uint16_t ReadU16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) |
                                      static_cast<std::uint16_t>(data[1]));
}

std::uint32_t ReadU32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

void WriteU16(std::uint8_t* data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[1] = static_cast<std::uint8_t>(value & 0xFF);
}

void WriteU32(std::uint8_t* data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    data[1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    data[2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[3] = static_cast<std::uint8_t>(value & 0xFF);
}

std::vector<std::uint8_t> MakeRtcpReceiverReport(std::uint32_t media_ssrc,
                                                 std::uint16_t highest_sequence) {
    std::vector<std::uint8_t> packet(32);
    packet[0] = 0x81; // V=2, RC=1
    packet[1] = 201;  // RR
    WriteU16(packet.data() + 2, 7);
    WriteU32(packet.data() + 4, 0x01020304); // receiver SSRC
    WriteU32(packet.data() + 8, media_ssrc);
    packet[12] = 0; // fraction lost
    packet[13] = 0;
    packet[14] = 0;
    packet[15] = 0; // cumulative lost
    WriteU32(packet.data() + 16, highest_sequence);
    WriteU32(packet.data() + 20, 0); // jitter
    WriteU32(packet.data() + 24, 0); // LSR
    WriteU32(packet.data() + 28, 0); // DLSR
    return packet;
}

std::vector<std::uint8_t> MakeRtcpSdesCname() {
    const std::string cname = "video-pipeline-test";
    std::vector<std::uint8_t> packet;
    packet.resize(4 + 4 + 2 + cname.size() + 1);
    packet[0] = 0x81; // V=2, SC=1
    packet[1] = 202;  // SDES
    WriteU32(packet.data() + 4, 0x01020304);
    packet[8] = 1; // CNAME
    packet[9] = static_cast<std::uint8_t>(cname.size());
    std::copy(cname.begin(), cname.end(), packet.begin() + 10);
    packet.back() = 0;

    while ((packet.size() % 4) != 0) {
        packet.push_back(0);
    }
    WriteU16(packet.data() + 2,
             static_cast<std::uint16_t>((packet.size() / 4) - 1));
    return packet;
}

std::vector<std::uint8_t> MakeCompoundReceiverReport(
    std::uint32_t media_ssrc,
    std::uint16_t highest_sequence) {
    auto rr = MakeRtcpReceiverReport(media_ssrc, highest_sequence);
    auto sdes = MakeRtcpSdesCname();
    rr.insert(rr.end(), sdes.begin(), sdes.end());
    return rr;
}

std::vector<std::uint8_t> MakeInterleavedFrame(
    std::uint8_t channel,
    const std::vector<std::uint8_t>& packet) {
    std::vector<std::uint8_t> frame(4 + packet.size());
    frame[0] = '$';
    frame[1] = channel;
    WriteU16(frame.data() + 2, static_cast<std::uint16_t>(packet.size()));
    std::copy(packet.begin(), packet.end(), frame.begin() + 4);
    return frame;
}

std::pair<std::uint16_t, std::uint16_t> ParseServerPorts(
    const std::string& response) {
    const auto marker = response.find("server_port=");
    assert(marker != std::string::npos);
    const auto value_begin = marker + std::string{"server_port="}.size();
    const auto dash = response.find('-', value_begin);
    assert(dash != std::string::npos);
    const auto value_end = response.find_first_of(";\r\n", dash + 1);
    const auto rtp_port =
        static_cast<std::uint16_t>(std::stoi(response.substr(value_begin,
                                                            dash - value_begin)));
    const auto rtcp_port =
        static_cast<std::uint16_t>(std::stoi(response.substr(
            dash + 1,
            value_end == std::string::npos ? std::string::npos : value_end - dash - 1)));
    return {rtp_port, rtcp_port};
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

std::uint16_t FindFreeUdpPortPair() {
    for (int attempt = 0; attempt < 64; ++attempt) {
        boost::asio::io_context io;
        boost::asio::ip::udp::socket rtp_socket(io);
        rtp_socket.open(boost::asio::ip::udp::v4());
        rtp_socket.bind({boost::asio::ip::udp::v4(), 0});

        const auto rtp_port = rtp_socket.local_endpoint().port();
        if (rtp_port == 65535) {
            continue;
        }

        boost::system::error_code ec;
        boost::asio::ip::udp::socket rtcp_socket(io);
        rtcp_socket.open(boost::asio::ip::udp::v4(), ec);
        if (ec) {
            continue;
        }
        rtcp_socket.bind({boost::asio::ip::udp::v4(),
                          static_cast<std::uint16_t>(rtp_port + 1)},
                         ec);
        if (!ec) {
            return rtp_port;
        }
    }

    return FindFreeUdpPort();
}

void BindAndJoinMulticastSocket(boost::asio::ip::udp::socket& socket,
                                std::uint16_t port) {
    boost::system::error_code ec;
    socket.open(boost::asio::ip::udp::v4(), ec);
    assert(!ec);
    socket.set_option(boost::asio::socket_base::reuse_address(true), ec);
    assert(!ec);
    socket.bind({boost::asio::ip::udp::v4(), port}, ec);
    assert(!ec);
    // 优先加入 loopback 接口，失败时回退到系统默认组播接口。
    socket.set_option(boost::asio::ip::multicast::join_group(
        boost::asio::ip::make_address_v4(kMulticastAddress),
        boost::asio::ip::address_v4::loopback()),
        ec);
    if (ec) {
        socket.set_option(boost::asio::ip::multicast::join_group(
            boost::asio::ip::make_address_v4(kMulticastAddress)),
            ec);
    }
    assert(!ec);
    // 测试里需要把客户端 RR 发回 loopback 组播组，显式指定接口可减少系统路由差异。
    socket.set_option(
        boost::asio::ip::multicast::outbound_interface(
            boost::asio::ip::address_v4::loopback()),
        ec);
    if (ec) {
        socket.set_option(boost::asio::ip::multicast::enable_loopback(true), ec);
    }
    socket.non_blocking(true);
}

bool WaitForRtcpReceiverReports(IPublisher& publisher,
                                std::uint64_t expected_count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        if (publisher.GetStats().rtcp_receiver_reports_received >= expected_count) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return publisher.GetStats().rtcp_receiver_reports_received >= expected_count;
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

    std::vector<std::vector<std::uint8_t>> frames;
    assert(ReadInterleavedFrames(socket, frames, 4));

    const auto rtp_frame = std::find_if(
        frames.begin(),
        frames.end(),
        [](const auto& frame) {
            return frame.size() >= 4 + 12 + 1 &&
                   frame[0] == '$' &&
                   frame[1] == 0 &&
                   (frame[5] & 0x7F) == 96;
        });
    assert(rtp_frame != frames.end());
    const auto media_ssrc = ReadU32(rtp_frame->data() + 12);
    const auto highest_sequence = ReadU16(rtp_frame->data() + 6);

    const auto rtcp_frame = std::find_if(
        frames.begin(),
        frames.end(),
        [](const auto& frame) {
            return frame.size() >= 4 + 28 &&
                   frame[0] == '$' &&
                   frame[1] == 1 &&
                   IsRtcpSenderReport(frame, 4);
        });
    assert(rtcp_frame != frames.end());

    const auto receiver_report =
        MakeCompoundReceiverReport(media_ssrc, highest_sequence);
    const auto receiver_report_frame = MakeInterleavedFrame(1, receiver_report);
    boost::asio::write(socket, boost::asio::buffer(receiver_report_frame));

    SendRequest(socket,
                "GET_PARAMETER " + url + " RTSP/1.0\r\n"
                "CSeq: 4\r\n"
                "Session: 00000001\r\n\r\n");
    response = ReadUntilHeader(socket);
    assert(response.find("RTSP/1.0 200 OK") != std::string::npos);
    assert(WaitForRtcpReceiverReports(*publisher, 1));

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
    rtcp_socket.non_blocking(true);

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
    const auto server_ports = ParseServerPorts(response);
    const auto server_rtcp_port = server_ports.second;

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
    const auto media_ssrc = ReadU32(packet.data() + 8);
    const auto highest_sequence = ReadU16(packet.data() + 2);

    std::vector<std::uint8_t> rtcp_packet;
    assert(ReadUdpPacket(rtcp_socket, rtcp_packet));
    assert(IsRtcpSenderReport(rtcp_packet));

    const auto receiver_report =
        MakeCompoundReceiverReport(media_ssrc, highest_sequence);
    rtcp_socket.send_to(
        boost::asio::buffer(receiver_report),
        {boost::asio::ip::make_address("127.0.0.1"), server_rtcp_port});

    SendRequest(rtsp_socket,
                "GET_PARAMETER " + url + " RTSP/1.0\r\n"
                "CSeq: 4\r\n"
                "Session: 00000001\r\n\r\n");
    response = ReadUntilHeader(rtsp_socket);
    assert(response.find("RTSP/1.0 200 OK") != std::string::npos);
    assert(WaitForRtcpReceiverReports(*publisher, 1));

    publisher->Stop();
}

void TestUdpMulticastPublisher() {
    const auto multicast_rtp_port = FindFreeUdpPortPair();
    auto config = MakeConfig(kMulticastTestPort, true, multicast_rtp_port);
    auto publisher = IPublisher::Create(config);
    assert(publisher);
    assert(publisher->Start(config));

    boost::asio::io_context io;
    boost::asio::ip::udp::socket rtp_socket(io);
    boost::asio::ip::udp::socket rtcp_socket(io);
    BindAndJoinMulticastSocket(rtp_socket, multicast_rtp_port);
    BindAndJoinMulticastSocket(rtcp_socket, multicast_rtp_port + 1);

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

    std::vector<std::uint8_t> first_rtp_packet;
    assert(ReadUdpPacket(rtp_socket, first_rtp_packet));
    assert(first_rtp_packet.size() >= 12 + 1);
    assert(first_rtp_packet[0] == 0x80);
    assert((first_rtp_packet[1] & 0x7F) == 96);
    const auto media_ssrc = ReadU32(first_rtp_packet.data() + 8);
    const auto highest_sequence = ReadU16(first_rtp_packet.data() + 2);

    const auto extra_packet_count = DrainUdpPackets(
        rtp_socket,
        std::chrono::milliseconds(200),
        std::chrono::milliseconds(200));
    const auto packet_count = 1 + extra_packet_count;
    assert(packet_count >= 1);
    assert(packet_count <= 3);

    std::vector<std::uint8_t> rtcp_packet;
    assert(ReadUdpPacket(rtcp_socket, rtcp_packet));
    assert(IsRtcpSenderReport(rtcp_packet));

    const auto receiver_report =
        MakeCompoundReceiverReport(media_ssrc, highest_sequence);
    rtcp_socket.send_to(
        boost::asio::buffer(receiver_report),
        {boost::asio::ip::make_address(kMulticastAddress),
         static_cast<std::uint16_t>(multicast_rtp_port + 1)});
    assert(WaitForRtcpReceiverReports(*publisher, 1));

    publisher->Stop();
}

} // namespace

int main() {
    TestTcpInterleavedPublisher();
    TestUdpUnicastPublisher();
    TestUdpMulticastPublisher();
    return 0;
}
