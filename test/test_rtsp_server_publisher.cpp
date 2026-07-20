#include "media/publisher/i_publisher.h"
#include "media/simple_buffer.h"

#include <boost/asio/ip/tcp.hpp>
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

constexpr std::uint16_t kTestPort = 18554;

PublisherConfig MakeConfig() {
    PublisherConfig config;
    config.mode = PublishMode::PullServer;
    config.protocol = PublishProtocol::RtspServer;
    config.listen_host = "127.0.0.1";
    config.listen_port = kTestPort;
    config.stream_path = "/live/test";

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

} // namespace

int main() {
    auto config = MakeConfig();
    auto publisher = IPublisher::Create(config);
    assert(publisher);
    assert(publisher->Start(config));

    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket(io);
    socket.connect({boost::asio::ip::make_address("127.0.0.1"), kTestPort});
    socket.non_blocking(true);

    const std::string url = "rtsp://127.0.0.1:" + std::to_string(kTestPort) + "/live/test";
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
    return 0;
}
