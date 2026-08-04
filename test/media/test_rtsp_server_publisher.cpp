#include "media/decoder/ffmpeg_decoder.h"
#include "media/encoder/ffmpeg_encoder.h"
#include "media/puller/ffmpeg_puller.h"
#include "media/publisher/i_publisher.h"
#include "media/simple_buffer.h"

#if defined(VIDEO_PIPELINE_HAS_INFERENCE) && VIDEO_PIPELINE_HAS_INFERENCE
#include "engine/openvino_engine.h"
#include "model/yolo_model.h"
#include "session/session.h"
#endif

#if defined(VIDEO_PIPELINE_HAS_COMMON_PROCESS) && VIDEO_PIPELINE_HAS_COMMON_PROCESS
#include "common/process/process.h"
#endif

#include <boost/asio/ip/multicast.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef VIDEO_PIPELINE_SOURCE_DIR
#define VIDEO_PIPELINE_SOURCE_DIR "."
#endif

namespace {

constexpr std::uint16_t kTcpTestPort = 18554;
constexpr std::uint16_t kUdpTestPort = 18555;
constexpr std::uint16_t kMulticastTestPort = 18556;
constexpr std::uint16_t kAudioVideoTestPort = 18557;
constexpr std::uint16_t kAudioUdpTestPort = 18558;

// 摄像头端到端手动测试配置全部写死在这里，不再从命令行读取参数。
constexpr std::uint16_t kManualRtspServerPort = 7852;
#if defined(VIDEO_PIPELINE_RUN_CAMERA) && VIDEO_PIPELINE_RUN_CAMERA
// 手动摄像头 target 显式打开该宏；协议 CTest target 保持关闭，避免进入无限摄像头循环。
constexpr bool kRunCameraAudioVideoPipeline = true;
#else
constexpr bool kRunCameraAudioVideoPipeline = false;
#endif
constexpr const char* kMulticastAddress = "239.255.42.1";
constexpr const char* kManualCameraSourceUrl = "rtsp://192.168.66.83/live/mainstream";
constexpr const char* kManualRtspServerPath = "/camera/mainstream";
constexpr const char* kManualZlmStreamPath = "/live/video_pipeline_av_test";
constexpr int kManualConnectTimeoutMs = 5000;
constexpr int kManualReadTimeoutMs = 5000;
constexpr int kManualFallbackFps = 30;
constexpr int kManualTargetBitrate = 2'000'000;
constexpr unsigned short kZlmHttpPort = 8888;
constexpr unsigned short kZlmRtspPort = 554;
constexpr unsigned short kZlmRtcSignalingPort = 13000;
constexpr unsigned short kZlmRtcSignalingSslPort = 13001;
const std::filesystem::path kManualZlmMediaServerPath =
    std::filesystem::path{VIDEO_PIPELINE_SOURCE_DIR} /
    "third_apps/win32/zlmediakit/MediaServer.exe";
const std::filesystem::path kYolov5ModelPath =
    std::filesystem::path{VIDEO_PIPELINE_SOURCE_DIR} / "models/yolov5/yolov5s.xml";

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
        config.rtsp.enable_multicast = true;
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

PublisherConfig MakeAudioVideoConfig(std::uint16_t port,
                                     bool enable_udp = false) {
    auto config = MakeConfig(port, enable_udp);

    MediaTrackConfig audio_track;
    audio_track.track_id = 1;
    audio_track.media_type = MediaType::AUDIO;
    audio_track.codec_type = CodecType::AAC;
    audio_track.sample_rate = 48000;
    audio_track.channels = 2;
    audio_track.time_base_num = 1;
    audio_track.time_base_den = 1000000;
    audio_track.rtp_payload_type = 97;
    audio_track.rtp_clock_rate = 48000;
    // AAC-LC, 48kHz, stereo AudioSpecificConfig。
    audio_track.extra_data = {0x11, 0x90};
    config.tracks.push_back(std::move(audio_track));
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
        // RTSP multicast 配置约定 RTP 使用偶数端口，紧邻的奇数端口用于 RTCP。
        // 系统随机分配的端口可能是奇数，不能直接把它作为 RTP/RTCP 端口对。
        if (rtp_port >= 65534 || (rtp_port % 2) != 0) {
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

    auto fallback_port = FindFreeUdpPort();
    if (fallback_port >= 65534) {
        fallback_port = 5004;
    }
    if ((fallback_port % 2) != 0) {
        --fallback_port;
    }
    return fallback_port;
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

MediaPacket MakeAacPacket() {
    std::vector<std::uint8_t> data{
        0x21, 0x10, 0x04, 0x60, 0x8c, 0x1c, 0x20, 0x00,
    };

    MediaPacket packet;
    packet.type = MediaType::AUDIO;
    packet.codec = CodecType::AAC;
    packet.stream_index = 1;
    packet.pts = 40000;
    packet.dts = 40000;
    packet.time_base = Rational{1, 1000000};
    packet.buffer = std::make_shared<SimpleBuffer>(std::move(data));
    return packet;
}

int ResolveManualFps(const VideoStreamInfo& video_info) {
    if (std::isfinite(video_info.fps) && video_info.fps >= 1.0f) {
        return std::clamp(static_cast<int>(std::lround(video_info.fps)), 1, 60);
    }
    return kManualFallbackFps;
}

std::string TrimLeftText(std::string value) {
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), [](unsigned char ch) {
                    return ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n';
                }));
    return value;
}

bool StartsWithText(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

bool CanConnectTcp(const std::string& host, unsigned short port) {
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket(io);
    boost::system::error_code ec;
    socket.connect({boost::asio::ip::make_address(host), port}, ec);
    return !ec;
}

#if defined(VIDEO_PIPELINE_HAS_COMMON_PROCESS) && VIDEO_PIPELINE_HAS_COMMON_PROCESS
// 给手动测试生成一份临时 ZLMediaKit 配置，避免修改 third_apps 中的原始 config.ini。
std::filesystem::path MakeZlmRuntimeConfig(
    const std::filesystem::path& media_server) {
    const auto source = media_server.parent_path() / "config.ini";
    if (!std::filesystem::exists(source)) {
        throw std::runtime_error("ZLMediaKit config.ini not found: " + source.string());
    }

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto output = std::filesystem::temp_directory_path() /
                  ("video_pipeline_camera_zlm_" + std::to_string(stamp) + ".ini");

    std::ifstream in(source, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open ZLMediaKit config: " +
                                 source.string());
    }

    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to create runtime config: " +
                                 output.string());
    }

    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        const auto trimmed = TrimLeftText(line);
        if (!trimmed.empty() && trimmed.front() == '[') {
            const auto end = trimmed.find(']');
            section = end == std::string::npos ? std::string{} : trimmed.substr(0, end + 1);
        }

        if (section == "[rtc]" && StartsWithText(trimmed, "signalingPort=")) {
            out << "signalingPort=" << kZlmRtcSignalingPort << "\n";
            continue;
        }
        if (section == "[rtc]" && StartsWithText(trimmed, "signalingSslPort=")) {
            out << "signalingSslPort=" << kZlmRtcSignalingSslPort << "\n";
            continue;
        }

        out << line << "\n";
    }

    return output;
}

// 进程守卫负责启动/复用 ZLMediaKit，并在本测试拥有进程时自动清理。
class ZlmServerGuard {
public:
    explicit ZlmServerGuard(std::filesystem::path media_server)
        : process_(io_.get_executor()),
          media_server_(std::move(media_server)) {
    }

    ~ZlmServerGuard() {
        Stop();
    }

    bool Start() {
        if (CanConnectTcp("127.0.0.1", kZlmHttpPort) &&
            CanConnectTcp("127.0.0.1", kZlmRtspPort)) {
            std::cout << "Reuse existing ZLMediaKit on HTTP " << kZlmHttpPort
                      << " and RTSP " << kZlmRtspPort << ".\n";
            return true;
        }

        if (!std::filesystem::exists(media_server_)) {
            std::cerr << "MediaServer.exe not found: "
                      << media_server_.string() << "\n";
            return false;
        }

        try {
            runtime_config_ = MakeZlmRuntimeConfig(media_server_);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return false;
        }

        common::process::ProcessOptions options;
        options.executable = media_server_;
        options.working_directory = media_server_.parent_path();
        options.arguments = {"-c", runtime_config_.string()};
        options.console_mode = common::process::ConsoleMode::NewConsole;
        options.window_mode = common::process::WindowMode::Hidden;

        boost::system::error_code ec;
        if (!process_.Start(options, ec)) {
            std::cerr << "Failed to start MediaServer: " << ec.message()
                      << " (" << process_.LastError() << ")\n";
            return false;
        }

        owns_process_ = true;
        std::cout << "Started MediaServer, pid=" << process_.Pid()
                  << ", config=" << runtime_config_.string() << "\n";

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline) {
            boost::system::error_code running_ec;
            if (!process_.IsRunning(running_ec)) {
                std::cerr << "MediaServer exited during startup, exit_code="
                          << process_.ExitCode() << "\n";
                return false;
            }
            if (CanConnectTcp("127.0.0.1", kZlmHttpPort) &&
                CanConnectTcp("127.0.0.1", kZlmRtspPort)) {
                std::cout << "ZLMediaKit is ready.\n";
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        std::cerr << "Timed out waiting for ZLMediaKit.\n";
        return false;
    }

    void Stop() {
        boost::system::error_code ec;
        if (owns_process_ && process_.IsRunning(ec)) {
            process_.RequestExit(ec);
            for (int i = 0; i < 20; ++i) {
                boost::system::error_code running_ec;
                if (!process_.IsRunning(running_ec)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            boost::system::error_code running_ec;
            if (process_.IsRunning(running_ec)) {
                process_.Terminate(ec);
                process_.Wait(ec);
            }
        }

        owns_process_ = false;
        if (!runtime_config_.empty()) {
            std::error_code ignored;
            std::filesystem::remove(runtime_config_, ignored);
            runtime_config_.clear();
        }
    }

private:
    boost::asio::io_context io_;
    common::process::Process process_;
    std::filesystem::path media_server_;
    std::filesystem::path runtime_config_;
    bool owns_process_{false};
};
#endif

// 摄像头链路同时发布到两个目标，状态集中放在这里，便于回调和主循环共享。
struct CameraAvPipelineState {
    std::unique_ptr<FFmpegEncoder> video_encoder;
    std::unique_ptr<FFmpegEncoder> audio_encoder;
    std::unique_ptr<IPublisher> zlm_publisher;
    std::unique_ptr<IPublisher> rtsp_server_publisher;
#if defined(VIDEO_PIPELINE_HAS_INFERENCE) && VIDEO_PIPELINE_HAS_INFERENCE
    std::shared_ptr<YoloModel> inference_model;
    std::shared_ptr<OpenVinoCpuEngine> inference_engine;
    std::unique_ptr<InferenceSession> inference_session;
#endif
    std::deque<MediaPacket> pending_audio_packets;

    int fps{kManualFallbackFps};
    int64_t frame_duration_us{1'000'000 / kManualFallbackFps};
    int64_t next_audio_pts_us{0};
    int64_t next_zlm_audio_pts_us{0};
    int64_t next_zlm_audio_packet_pts_us{0};
    int64_t zlm_audio_packet_duration_us{0};
    int decoded_video_frames{0};
    int decoded_audio_frames{0};
    int encoded_video_packets{0};
    int encoded_audio_packets{0};
    int published_video_packets{0};
    int published_audio_packets{0};
    int published_zlm_audio_packets{0};
    int read_video_packets{0};
    int read_audio_packets{0};
    int inferred_video_frames{0};
    int detected_objects{0};
    bool initialized{false};
    bool inference_initialized{false};
    bool failed{false};
    std::string error;
};

void NormalizeCameraVideoFrameTime(CameraAvPipelineState& state,
                                   MediaFrame& frame) {
    const auto frame_index =
        static_cast<int64_t>(std::max(0, state.decoded_video_frames - 1));
    const auto pts_us = frame_index * state.frame_duration_us;
    frame.time.pts_us = pts_us;
    frame.time.dts_us = pts_us;
    frame.time.duration_us = state.frame_duration_us;
}

int64_t EstimateG711DurationUs(const MediaPacket& packet,
                               const AudioStreamInfo& audio_info) {
    if (packet.duration > 0) {
        return packet.duration;
    }
    if (!packet.buffer || audio_info.sample_rate <= 0 || audio_info.channels <= 0) {
        return 0;
    }

    // G711 A-law/u-law 每个采样 1 字节；这里用包大小估算缺失 duration。
    const auto samples = static_cast<int64_t>(packet.buffer->Size()) /
                         std::max(1, audio_info.channels);
    return samples * 1'000'000LL / audio_info.sample_rate;
}

MediaPacket MakeCameraAudioPacket(CameraAvPipelineState& state,
                                  const MediaPacket& source,
                                  const AudioStreamInfo& audio_info,
                                  CodecType codec) {
    MediaPacket packet = source;
    packet.type = MediaType::AUDIO;
    packet.codec = codec;
    packet.stream_index = 1;
    packet.time_base = Rational{1, 1'000'000};
    packet.duration = EstimateG711DurationUs(source, audio_info);
    if (packet.duration <= 0) {
        packet.duration = 20'000;
    }
    packet.pts = state.next_audio_pts_us;
    packet.dts = packet.pts;
    state.next_audio_pts_us += packet.duration;
    return packet;
}

bool PublishCameraAudioPacket(CameraAvPipelineState& state,
                              const MediaPacket& audio_packet) {
    if (!state.initialized) {
        // 视频编码器要等首个解码帧才能确定输入像素格式；这之前先缓存音频包。
        state.pending_audio_packets.push_back(audio_packet);
        return true;
    }

    if (!state.rtsp_server_publisher->Publish(audio_packet)) {
        state.error = "failed to publish G711 audio packet to local RTSP server";
        return false;
    }
    ++state.published_audio_packets;
    return true;
}

bool FlushPendingCameraAudio(CameraAvPipelineState& state) {
    while (!state.pending_audio_packets.empty()) {
        auto packet = std::move(state.pending_audio_packets.front());
        state.pending_audio_packets.pop_front();
        if (!PublishCameraAudioPacket(state, packet)) {
            return false;
        }
    }
    return true;
}

#if defined(VIDEO_PIPELINE_HAS_INFERENCE) && VIDEO_PIPELINE_HAS_INFERENCE
bool IsSupportedInferencePixelFormat(PixelFormat format) {
    return format == PixelFormat::kI420 || format == PixelFormat::kNV12;
}

bool InitializeCameraInference(CameraAvPipelineState& state,
                               const MediaFrame& first_frame) {
    if (state.inference_initialized) {
        return true;
    }
    if (!std::filesystem::exists(kYolov5ModelPath)) {
        state.error = "YOLOv5 model path does not exist: " + kYolov5ModelPath.string();
        return false;
    }

    const auto pixel_format = first_frame.GetPixelFormat();
    if (!IsSupportedInferencePixelFormat(pixel_format)) {
        state.error = "decoded video pixel format is not supported by inference";
        return false;
    }

    auto model = std::make_shared<YoloModel>();
    ModelConfig model_config;
    model_config.name = "yolov5s";
    model_config.class_count = 80;
    model_config.options["input_width"] = "640";
    model_config.options["input_height"] = "640";
    if (!model->Initialize(model_config)) {
        state.error = "failed to initialize YOLOv5 model";
        return false;
    }

    auto engine = std::make_shared<OpenVinoCpuEngine>();
    EngineLoadConfig load_config;
    load_config.engine.model_path = kYolov5ModelPath.string();
    load_config.engine.backend = "OPENVINO";
    load_config.engine.device = "CPU";
    load_config.engine.request_count = 1;

    OpenVinoPreprocessConfig preprocess_config;
    preprocess_config.enabled = true;
    preprocess_config.input_pixel_format = pixel_format;
    preprocess_config.model_pixel_format = PixelFormat::kRGB24;
    preprocess_config.model_input_layout = "NCHW";
    preprocess_config.scale = 255.0f;
    load_config.preprocess = preprocess_config;

    if (!engine->LoadModel(load_config)) {
        state.error = "failed to load YOLOv5 OpenVINO model";
        return false;
    }

    auto session = std::make_unique<InferenceSession>();
    if (!session->Initialize(model, engine)) {
        state.error = "failed to initialize YOLOv5 inference session";
        engine->Release();
        return false;
    }

    state.inference_model = std::move(model);
    state.inference_engine = std::move(engine);
    state.inference_session = std::move(session);
    state.inference_initialized = true;
    std::cout << "YOLOv5 inference initialized: " << kYolov5ModelPath << "\n";
    return true;
}

bool RunCameraInference(CameraAvPipelineState& state,
                        const MediaFrame& frame) {
    if (!state.inference_initialized) {
        if (!InitializeCameraInference(state, frame)) {
            return false;
        }
    }

    const auto result = state.inference_session->Infer(frame);
    ++state.inferred_video_frames;
    state.detected_objects += static_cast<int>(result.objects.size());
    if (!result.objects.empty()) {
        std::cout << "YOLOv5 frame pts=" << result.pts
                  << ", objects=" << result.objects.size() << "\n";
    }
    return true;
}
#endif

bool PublishCameraVideoPackets(CameraAvPipelineState& state,
                               std::vector<PacketPtr>& packets) {
    for (auto& packet : packets) {
        if (!packet) {
            continue;
        }

        packet->stream_index = 0;
        packet->time_base = Rational{1, 1'000'000};
        if (packet->duration <= 0) {
            packet->duration = state.frame_duration_us;
        }

        if (!state.zlm_publisher->Publish(*packet)) {
            state.error = "failed to publish encoded H264 video packet to ZLMediaKit";
            return false;
        }
        if (!state.rtsp_server_publisher->Publish(*packet)) {
            state.error = "failed to publish encoded H264 video packet to local RTSP server";
            return false;
        }
        ++state.encoded_video_packets;
        ++state.published_video_packets;
    }
    return true;
}

bool PublishCameraZlmAudioPackets(CameraAvPipelineState& state,
                                  std::vector<PacketPtr>& packets,
                                  int64_t fallback_duration_us) {
    for (auto& packet : packets) {
        if (!packet) {
            continue;
        }

        packet->stream_index = 1;
        packet->time_base = Rational{1, 1'000'000};
        packet->pts = state.next_zlm_audio_packet_pts_us;
        packet->dts = packet->pts;
        packet->duration = state.zlm_audio_packet_duration_us > 0
            ? state.zlm_audio_packet_duration_us
            : (fallback_duration_us > 0 ? fallback_duration_us : 20'000);
        state.next_zlm_audio_packet_pts_us += packet->duration;

        if (!state.zlm_publisher->Publish(*packet)) {
            state.error = "failed to publish encoded AAC audio packet to ZLMediaKit";
            return false;
        }
        ++state.encoded_audio_packets;
        ++state.published_zlm_audio_packets;
    }
    return true;
}

PublisherConfig MakeCameraZlmPublisherConfig(const std::string& zlm_push_url,
                                             const MediaTrackConfig& video_track,
                                             const MediaTrackConfig& audio_track) {
    PublisherConfig config;
    config.mode = PublishMode::PushClient;
    config.protocol = PublishProtocol::FfmpegMux;
    config.url = zlm_push_url;
    config.ffmpeg.format_name = "rtsp";
    config.ffmpeg.rtsp_transport = "tcp";
    config.tracks.push_back(video_track);
    config.tracks.push_back(audio_track);
    return config;
}

PublisherConfig MakeCameraRtspServerPublisherConfig(
    const MediaTrackConfig& video_track,
    const MediaTrackConfig& audio_track) {
    PublisherConfig config;
    config.mode = PublishMode::PullServer;
    config.protocol = PublishProtocol::RtspServer;
    config.listen_host = "127.0.0.1";
    config.listen_port = kManualRtspServerPort;
    config.stream_path = kManualRtspServerPath;
    config.rtsp.enable_udp = true;
    config.rtsp.enable_multicast = false;
    config.tracks.push_back(video_track);
    config.tracks.push_back(audio_track);
    return config;
}

bool InitializeCameraAvPublishers(CameraAvPipelineState& state,
                                  const MediaFrame& first_frame,
                                  const VideoStreamInfo& video_info,
                                  const MediaStreamInfo& audio_stream_info) {
    const auto pixel_format = first_frame.GetPixelFormat();
    if (pixel_format == PixelFormat::kUnknown) {
        state.error = "decoded video frame pixel format is unknown";
        return false;
    }

    const int width = first_frame.Width() > 0 ? first_frame.Width() : video_info.width;
    const int height = first_frame.Height() > 0 ? first_frame.Height() : video_info.height;
    if (width <= 0 || height <= 0) {
        state.error = "invalid decoded video size";
        return false;
    }

    EncoderConfig encoder_config;
    encoder_config.media_type = MediaType::VIDEO;
    encoder_config.codec_type = CodecType::H264;
    encoder_config.video.width = width;
    encoder_config.video.height = height;
    encoder_config.video.fps_num = state.fps;
    encoder_config.video.fps_den = 1;
    encoder_config.video.pixel_format = pixel_format;
    encoder_config.video.gop_size = state.fps;
    encoder_config.video.max_b_frames = 0;
    encoder_config.bitrate = kManualTargetBitrate;
    encoder_config.time_base_num = 1;
    encoder_config.time_base_den = 1'000'000;
    encoder_config.encoder_name = "libx264";
    encoder_config.preset = "ultrafast";
    encoder_config.tune = "zerolatency";
    encoder_config.global_header = true;

    state.video_encoder = std::make_unique<FFmpegEncoder>();
    if (!state.video_encoder->Open(encoder_config)) {
        state.error = "failed to open H264 encoder";
        return false;
    }

    MediaTrackConfig video_track;
    video_track.track_id = 0;
    video_track.media_type = MediaType::VIDEO;
    video_track.codec_type = CodecType::H264;
    video_track.width = width;
    video_track.height = height;
    video_track.fps = static_cast<float>(state.fps);
    video_track.time_base_num = encoder_config.time_base_num;
    video_track.time_base_den = encoder_config.time_base_den;
    video_track.extra_data = state.video_encoder->GetExtraData();
    video_track.rtp_payload_type = 96;
    video_track.rtp_clock_rate = 90000;

    const auto& audio_info = audio_stream_info.get_detail<AudioStreamInfo>();
    EncoderConfig audio_encoder_config;
    audio_encoder_config.media_type = MediaType::AUDIO;
    audio_encoder_config.codec_type = CodecType::AAC;
    audio_encoder_config.audio.sample_rate = audio_info.sample_rate;
    audio_encoder_config.audio.channels = audio_info.channels;
    audio_encoder_config.audio.channel_layout = audio_info.channel_layout;
    audio_encoder_config.audio.sample_format = SampleFormat::S16;
    audio_encoder_config.bitrate = 64'000;
    audio_encoder_config.time_base_num = 1;
    audio_encoder_config.time_base_den = 1'000'000;
    audio_encoder_config.encoder_name = "aac";
    audio_encoder_config.global_header = true;

    state.audio_encoder = std::make_unique<FFmpegEncoder>();
    if (!state.audio_encoder->Open(audio_encoder_config)) {
        state.error = "failed to open AAC audio encoder";
        return false;
    }
    // 16 kHz PCMA/PCMU cannot use static payload type 8/0.
    MediaTrackConfig local_audio_track;
    local_audio_track.track_id = 1;
    local_audio_track.media_type = MediaType::AUDIO;
    local_audio_track.codec_type = audio_stream_info.codec_type;
    local_audio_track.sample_rate = audio_info.sample_rate;
    local_audio_track.channels = audio_info.channels;
    local_audio_track.time_base_num = 1;
    local_audio_track.time_base_den = 1'000'000;
    // 16 kHz PCMA/PCMU cannot use static payload type 8/0.
    local_audio_track.rtp_payload_type = 97;
    local_audio_track.rtp_clock_rate =
        static_cast<std::uint32_t>(audio_info.sample_rate);

    MediaTrackConfig zlm_audio_track;
    zlm_audio_track.track_id = 1;
    zlm_audio_track.media_type = MediaType::AUDIO;
    zlm_audio_track.codec_type = CodecType::AAC;
    zlm_audio_track.sample_rate = audio_info.sample_rate;
    zlm_audio_track.channels = audio_info.channels;
    zlm_audio_track.time_base_num = audio_encoder_config.time_base_num;
    zlm_audio_track.time_base_den = audio_encoder_config.time_base_den;
    zlm_audio_track.extra_data = state.audio_encoder->GetExtraData();
    state.zlm_audio_packet_duration_us =
        audio_info.sample_rate > 0
            ? 1024LL * 1'000'000LL / audio_info.sample_rate
            : 64'000;

    const std::string zlm_push_url =
        "rtsp://127.0.0.1:" + std::to_string(kZlmRtspPort) + kManualZlmStreamPath;
    auto zlm_config = MakeCameraZlmPublisherConfig(zlm_push_url,
                                                   video_track,
                                                   zlm_audio_track);
    auto rtsp_config =
        MakeCameraRtspServerPublisherConfig(video_track, local_audio_track);

    state.zlm_publisher = IPublisher::Create(zlm_config);
    if (!state.zlm_publisher || !state.zlm_publisher->Start()) {
        state.error = "failed to start FFmpeg RTSP publisher to ZLMediaKit";
        return false;
    }

    state.rtsp_server_publisher = IPublisher::Create(rtsp_config);
    if (!state.rtsp_server_publisher ||
        !state.rtsp_server_publisher->Start()) {
        state.error = "failed to start local RTSP server publisher";
        return false;
    }

    state.initialized = true;
    std::cout << "Manual camera pipeline initialized: video="
              << width << "x" << height << " @" << state.fps
              << "fps, local_audio=G711, zlm_audio=AAC "
              << audio_info.sample_rate << "Hz/" << audio_info.channels
              << "ch\n";
    std::cout << "Pushing audio/video to ZLMediaKit: " << zlm_push_url << "\n";
    std::cout << "Local RTSP play URL: "
              << state.rtsp_server_publisher->GetPlayUrl() << "\n";
    return FlushPendingCameraAudio(state);
}

// 单独的手动端到端函数：
// 1. 拉取摄像头 RTSP 音视频；
// 2. 视频解码后重新编码 H264；
// 3. 音频经过解码校验后继续发布原始 G711 包；
// 4. 视频推到 ZLMediaKit，同时启动本项目 RTSP server 发布音视频供播放器直接拉流。
void TestCameraAudioVideoDecodeEncodePublish() {
#if defined(VIDEO_PIPELINE_HAS_COMMON_PROCESS) && VIDEO_PIPELINE_HAS_COMMON_PROCESS
    ZlmServerGuard zlm(kManualZlmMediaServerPath);
    if (!zlm.Start()) {
        assert(false);
        return;
    }
#else
    std::cerr << "test_rtsp_server_publisher requires common process support "
              << "for TestCameraAudioVideoDecodeEncodePublish.\n";
    assert(false);
    return;
#endif

    FFmpegPuller puller;
    puller.SetConnectTimeoutMs(kManualConnectTimeoutMs);
    puller.SetReadTimeoutMs(kManualReadTimeoutMs);
    puller.SetRtspTransport("tcp");
    puller.SetRtspAutoSwitchToTcp(false);
    puller.SetLowLatency(true);

    std::cout << "Opening source: " << kManualCameraSourceUrl << "\n";
    if (!puller.Open(kManualCameraSourceUrl)) {
        std::cerr << "Failed to open source RTSP.\n";
        assert(false);
        return;
    }

    const auto multi_info = puller.GetStreamInfo();
    if (!multi_info.HasVideoStream() || !multi_info.HasAudioStream()) {
        std::cerr << "Source must contain both video and audio streams.\n";
        puller.Close();
        assert(false);
        return;
    }

    const auto video_stream_info =
        multi_info.stream_infos[multi_info.video_stream_idx_];
    const auto audio_stream_info =
        multi_info.stream_infos[multi_info.audio_stream_idx_];
    const auto video_detail = video_stream_info.get_detail<VideoStreamInfo>();
    const auto audio_detail = audio_stream_info.get_detail<AudioStreamInfo>();

    if (video_stream_info.codec_type != CodecType::H264) {
        std::cerr << "Expected H264 source video, got codec="
                  << static_cast<int>(video_stream_info.codec_type) << "\n";
        puller.Close();
        assert(false);
        return;
    }
    if (audio_stream_info.codec_type != CodecType::G711A &&
        audio_stream_info.codec_type != CodecType::G711U) {
        std::cerr << "Expected G711 audio source, got codec="
                  << static_cast<int>(audio_stream_info.codec_type) << "\n";
        puller.Close();
        assert(false);
        return;
    }

    std::cout << "Source video: " << video_detail.width << "x"
              << video_detail.height << " @" << video_detail.fps << "fps\n";
    std::cout << "Source audio: codec="
              << static_cast<int>(audio_stream_info.codec_type)
              << ", sample_rate=" << audio_detail.sample_rate
              << ", channels=" << audio_detail.channels << "\n";

    CameraAvPipelineState state;
    state.fps = ResolveManualFps(video_detail);
    state.frame_duration_us = 1'000'000 / state.fps;

    FFmpegDecoder video_decoder;
    FFmpegDecoder audio_decoder;
    video_decoder.SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
        if (state.failed || !frame || frame->type != MediaType::VIDEO) {
            return;
        }

        ++state.decoded_video_frames;
        // 按照视频帧率调整时间戳，确保视频帧时间间隔一致。
        NormalizeCameraVideoFrameTime(state, *frame);

        if (!state.initialized &&
            !InitializeCameraAvPublishers(state,
                                          *frame,
                                          video_detail,
                                          audio_stream_info)) {
            state.failed = true;
            return;
        }

#if defined(VIDEO_PIPELINE_HAS_INFERENCE) && VIDEO_PIPELINE_HAS_INFERENCE
        if (!RunCameraInference(state, *frame)) {
            state.failed = true;
            return;
        }
#endif

        std::vector<PacketPtr> encoded;
        if (!state.video_encoder->Encode(std::move(frame), encoded)) {
            state.failed = true;
            state.error = "failed to encode H264 video frame";
            return;
        }

        if (!PublishCameraVideoPackets(state, encoded)) {
            state.failed = true;
        }
    });

    audio_decoder.SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
        if (state.failed || !frame || frame->type != MediaType::AUDIO) {
            return;
        }

        ++state.decoded_audio_frames;
        if (!state.initialized || !state.audio_encoder) {
            return;
        }

        const int sample_rate = frame->SampleRate() > 0
            ? frame->SampleRate()
            : audio_detail.sample_rate;
        const int nb_samples = frame->NbSamples() > 0
            ? frame->NbSamples()
            : sample_rate / 50;
        const int64_t duration_us = sample_rate > 0
            ? static_cast<int64_t>(nb_samples) * 1'000'000LL / sample_rate
            : 20'000;
        frame->time.pts_us = state.next_zlm_audio_pts_us;
        frame->time.dts_us = frame->time.pts_us;
        frame->time.duration_us = duration_us;
        state.next_zlm_audio_pts_us += duration_us;

        std::vector<PacketPtr> encoded;
        if (!state.audio_encoder->Encode(std::move(frame), encoded)) {
            state.failed = true;
            state.error = "failed to encode AAC audio frame";
            return;
        }
        if (!PublishCameraZlmAudioPackets(state, encoded, duration_us)) {
            state.failed = true;
        }
    });

    if (!video_decoder.Open(video_stream_info)) {
        std::cerr << "Failed to open video decoder.\n";
        puller.Close();
        assert(false);
        return;
    }
    if (!audio_decoder.Open(audio_stream_info)) {
        std::cerr << "Failed to open audio decoder.\n";
        video_decoder.Close();
        puller.Close();
        assert(false);
        return;
    }

    while (!state.failed) {
        std::shared_ptr<MediaPacket> packet;
        if (!puller.ReadPacket(packet)) {
            state.failed = true;
            state.error = "source ReadPacket failed";
            break;
        }
        if (!packet) {
            continue;
        }

        if (packet->type == MediaType::VIDEO) {
            ++state.read_video_packets;
            if (!video_decoder.Decode(packet)) {
                state.failed = true;
                state.error = "video Decode failed";
                break;
            }
        } else if (packet->type == MediaType::AUDIO) {
            ++state.read_audio_packets;
            if (!audio_decoder.Decode(packet)) {
                state.failed = true;
                state.error = "audio Decode failed";
                break;
            }
            const auto audio_packet =
                MakeCameraAudioPacket(state, *packet, audio_detail, audio_stream_info.codec_type);
            if (!PublishCameraAudioPacket(state, audio_packet)) {
                state.failed = true;
                break;
            }
        }

        const auto published_total =
            state.published_video_packets + state.published_audio_packets;
        if (published_total > 0 && published_total % 200 == 0) {
            std::cout << "camera av pipeline: read_video=" << state.read_video_packets
                      << ", read_audio=" << state.read_audio_packets
                      << ", decoded_video=" << state.decoded_video_frames
                      << ", decoded_audio=" << state.decoded_audio_frames
                      << ", published_video=" << state.published_video_packets
                      << ", published_audio=" << state.published_audio_packets
                      << ", encoded_aac=" << state.encoded_audio_packets
                      << ", published_zlm_audio="
                      << state.published_zlm_audio_packets
                      << ", inferred_video=" << state.inferred_video_frames
                      << ", detected_objects=" << state.detected_objects << "\n";
        }
    }

#if defined(VIDEO_PIPELINE_HAS_INFERENCE) && VIDEO_PIPELINE_HAS_INFERENCE
    if (state.inference_engine) {
        state.inference_engine->Release();
    }
#endif
    if (state.zlm_publisher) {
        state.zlm_publisher->Stop();
    }
    if (state.rtsp_server_publisher) {
        state.rtsp_server_publisher->Stop();
    }
    if (state.video_encoder) {
        state.video_encoder->Close();
    }
    if (state.audio_encoder) {
        state.audio_encoder->Close();
    }
    audio_decoder.Close();
    video_decoder.Close();
    puller.Close();

    if (state.failed) {
        std::cerr << "Manual camera pipeline failed: " << state.error << "\n";
        assert(false);
    }

    std::cout << "Manual camera pipeline stopped: inferred_video="
              << state.inferred_video_frames
              << ", detected_objects=" << state.detected_objects << "\n";
}

void TestTcpInterleavedPublisher() {
    auto config = MakeConfig(kTcpTestPort, false);
    auto publisher = IPublisher::Create(config);
    assert(publisher);
    assert(publisher->Start());

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

void TestTcpInterleavedAudioVideoPublisher() {
    auto config = MakeAudioVideoConfig(kAudioVideoTestPort);
    auto publisher = IPublisher::Create(config);
    assert(publisher);
    assert(publisher->Start());

    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket(io);
    socket.connect({boost::asio::ip::make_address("127.0.0.1"), kAudioVideoTestPort});
    socket.non_blocking(true);

    const std::string url =
        "rtsp://127.0.0.1:" + std::to_string(kAudioVideoTestPort) + "/live/test";
    SendRequest(socket,
                "DESCRIBE " + url + " RTSP/1.0\r\n"
                "CSeq: 1\r\n"
                "Accept: application/sdp\r\n\r\n");
    auto response = ReadUntilHeader(socket);
    assert(response.find("RTSP/1.0 200 OK") != std::string::npos);
    assert(response.find("m=video 0 RTP/AVP 96") != std::string::npos);
    assert(response.find("m=audio 0 RTP/AVP 97") != std::string::npos);
    assert(response.find("MPEG4-GENERIC/48000/2") != std::string::npos);
    assert(response.find("config=1190") != std::string::npos);
    assert(response.find("a=control:track1") != std::string::npos);

    SendRequest(socket,
                "SETUP " + url + "/track0 RTSP/1.0\r\n"
                "CSeq: 2\r\n"
                "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n");
    response = ReadUntilHeader(socket);
    assert(response.find("RTSP/1.0 200 OK") != std::string::npos);

    SendRequest(socket,
                "SETUP " + url + "/track1 RTSP/1.0\r\n"
                "CSeq: 3\r\n"
                "Transport: RTP/AVP/TCP;unicast;interleaved=2-3\r\n\r\n");
    response = ReadUntilHeader(socket);
    assert(response.find("RTSP/1.0 200 OK") != std::string::npos);
    assert(response.find("interleaved=2-3") != std::string::npos);

    SendRequest(socket,
                "PLAY " + url + " RTSP/1.0\r\n"
                "CSeq: 4\r\n"
                "Session: 00000001\r\n\r\n");
    response = ReadUntilHeader(socket);
    assert(response.find("RTSP/1.0 200 OK") != std::string::npos);
    assert(response.find("track0") != std::string::npos);
    assert(response.find("track1") != std::string::npos);

    assert(publisher->Publish(MakePacket()));
    assert(publisher->Publish(MakeAacPacket()));

    std::vector<std::vector<std::uint8_t>> frames;
    assert(ReadInterleavedFrames(socket, frames, 6));

    const auto video_rtp_frame = std::find_if(
        frames.begin(),
        frames.end(),
        [](const auto& frame) {
            return frame.size() >= 4 + 12 + 1 &&
                   frame[0] == '$' &&
                   frame[1] == 0 &&
                   (frame[5] & 0x7F) == 96;
        });
    assert(video_rtp_frame != frames.end());

    const auto audio_rtp_frame = std::find_if(
        frames.begin(),
        frames.end(),
        [](const auto& frame) {
            return frame.size() >= 4 + 12 + 4 + 1 &&
                   frame[0] == '$' &&
                   frame[1] == 2 &&
                   (frame[5] & 0x7F) == 97 &&
                   frame[16] == 0 &&
                   frame[17] == 16;
        });
    assert(audio_rtp_frame != frames.end());

    const auto audio_rtcp_frame = std::find_if(
        frames.begin(),
        frames.end(),
        [](const auto& frame) {
            return frame.size() >= 4 + 28 &&
                   frame[0] == '$' &&
                   frame[1] == 3 &&
                   IsRtcpSenderReport(frame, 4);
        });
    assert(audio_rtcp_frame != frames.end());

    publisher->Stop();
}

void TestUdpUnicastAudioTrackPublisher() {
    auto config = MakeAudioVideoConfig(kAudioUdpTestPort, true);
    auto publisher = IPublisher::Create(config);
    assert(publisher);
    assert(publisher->Start());

    boost::asio::io_context io;
    boost::asio::ip::tcp::socket rtsp_socket(io);
    rtsp_socket.connect({boost::asio::ip::make_address("127.0.0.1"),
                         kAudioUdpTestPort});
    rtsp_socket.non_blocking(true);

    boost::asio::ip::udp::socket audio_rtp_socket(io);
    audio_rtp_socket.open(boost::asio::ip::udp::v4());
    audio_rtp_socket.bind({boost::asio::ip::make_address("127.0.0.1"), 0});
    audio_rtp_socket.non_blocking(true);

    boost::asio::ip::udp::socket audio_rtcp_socket(io);
    audio_rtcp_socket.open(boost::asio::ip::udp::v4());
    audio_rtcp_socket.bind({boost::asio::ip::make_address("127.0.0.1"), 0});
    audio_rtcp_socket.non_blocking(true);

    const auto audio_client_rtp_port = audio_rtp_socket.local_endpoint().port();
    const auto audio_client_rtcp_port = audio_rtcp_socket.local_endpoint().port();
    const std::string url =
        "rtsp://127.0.0.1:" + std::to_string(kAudioUdpTestPort) + "/live/test";

    SendRequest(rtsp_socket,
                "DESCRIBE " + url + " RTSP/1.0\r\n"
                "CSeq: 1\r\n"
                "Accept: application/sdp\r\n\r\n");
    auto response = ReadUntilHeader(rtsp_socket);
    assert(response.find("RTSP/1.0 200 OK") != std::string::npos);
    assert(response.find("a=control:track1") != std::string::npos);

    // 只 SETUP 音频 track，验证 RTSP server 的音频路径不依赖视频 track 状态。
    SendRequest(rtsp_socket,
                "SETUP " + url + "/track1 RTSP/1.0\r\n"
                "CSeq: 2\r\n"
                "Transport: RTP/AVP;unicast;client_port=" +
                    std::to_string(audio_client_rtp_port) + "-" +
                    std::to_string(audio_client_rtcp_port) + "\r\n\r\n");
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
    assert(response.find("track1") != std::string::npos);

    assert(publisher->Publish(MakeAacPacket()));

    std::vector<std::uint8_t> audio_packet;
    assert(ReadUdpPacket(audio_rtp_socket, audio_packet));
    assert(audio_packet.size() >= 12 + 4 + 1);
    assert(audio_packet[0] == 0x80);
    assert((audio_packet[1] & 0x7F) == 97);
    // AAC RTP payload 前 4 字节是 RFC 3640 AU header section。
    assert(audio_packet[12] == 0);
    assert(audio_packet[13] == 16);
    const auto media_ssrc = ReadU32(audio_packet.data() + 8);
    const auto highest_sequence = ReadU16(audio_packet.data() + 2);

    std::vector<std::uint8_t> audio_rtcp_packet;
    assert(ReadUdpPacket(audio_rtcp_socket, audio_rtcp_packet));
    assert(IsRtcpSenderReport(audio_rtcp_packet));

    const auto receiver_report =
        MakeCompoundReceiverReport(media_ssrc, highest_sequence);
    audio_rtcp_socket.send_to(
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

void TestUdpUnicastPublisher() {
    auto config = MakeConfig(kUdpTestPort, true);
    auto publisher = IPublisher::Create(config);
    assert(publisher);
    assert(publisher->Start());

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
    assert(publisher->Start());

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
    if constexpr (kRunCameraAudioVideoPipeline) {
        TestCameraAudioVideoDecodeEncodePublish();
        return 0;
    }

    TestTcpInterleavedPublisher();
    TestTcpInterleavedAudioVideoPublisher();
    TestUdpUnicastAudioTrackPublisher();
    TestUdpUnicastPublisher();
    TestUdpMulticastPublisher();
    return 0;
}
