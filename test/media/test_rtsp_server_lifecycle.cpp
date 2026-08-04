#include "media/protocol/rtsp_server_protocol.h"

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

#include <boost/uuid/detail/md5.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

namespace {

std::uint16_t FindFreeTcpPort() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(io);
    boost::system::error_code ec;
    acceptor.open(boost::asio::ip::tcp::v4(), ec);
    assert(!ec);
    acceptor.bind({boost::asio::ip::address_v4::loopback(), 0}, ec);
    assert(!ec);
    return acceptor.local_endpoint().port();
}

PublisherConfig MakeConfig(std::uint16_t port) {
    PublisherConfig config;
    config.mode = PublishMode::PullServer;
    config.protocol = PublishProtocol::RtspServer;
    config.listen_host = "127.0.0.1";
    config.listen_port = port;
    config.stream_path = "/live/lifecycle";
    config.rtsp.enable_tcp_interleaved = true;
    config.rtsp.enable_udp = false;
    return config;
}

std::vector<MediaTrackConfig> MakeTracks() {
    MediaTrackConfig track;
    track.track_id = 0;
    track.media_type = MediaType::VIDEO;
    track.codec_type = CodecType::H264;
    track.rtp_payload_type = 96;
    track.rtp_clock_rate = 90000;
    return {track};
}

EncodedAccessUnit MakeAccessUnit() {
    EncodedAccessUnit access_unit;
    access_unit.track_id = 0;
    access_unit.media_type = MediaType::VIDEO;
    access_unit.codec_type = CodecType::H264;
    access_unit.nals.push_back(NalUnit{{0x65, 0x88, 0x84, 0x21}});
    return access_unit;
}

bool WaitForPeerClose(boost::asio::ip::tcp::socket& socket) {
    socket.non_blocking(true);
    std::array<std::uint8_t, 64> buffer{};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        boost::system::error_code ec;
        socket.read_some(boost::asio::buffer(buffer), ec);
        if (ec == boost::asio::error::eof ||
            ec == boost::asio::error::connection_reset) {
            return true;
        }
        if (ec == boost::asio::error::would_block ||
            ec == boost::asio::error::try_again) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (!ec) {
            continue;
        }
        return false;
    }
    return false;
}

bool ReadRtspResponse(boost::asio::ip::tcp::socket& socket,
                      std::string& response) {
    socket.non_blocking(true);
    std::array<char, 1024> buffer{};
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        boost::system::error_code ec;
        const auto count = socket.read_some(boost::asio::buffer(buffer), ec);
        if (!ec && count != 0) {
            response.append(buffer.data(), count);
            if (response.find("\r\n\r\n") != std::string::npos) {
                return true;
            }
            continue;
        }
        if (ec == boost::asio::error::would_block ||
            ec == boost::asio::error::try_again) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        return false;
    }
    return false;
}

std::string Md5HexForTest(const std::string& value) {
    boost::uuids::detail::md5 hash;
    hash.process_bytes(value.data(), value.size());
    boost::uuids::detail::md5::digest_type digest{};
    hash.get_digest(digest);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

std::string ExtractDigestNonce(const std::string& response) {
    const auto marker = response.find("nonce=\"");
    assert(marker != std::string::npos);
    const auto begin = marker + std::string{"nonce=\""}.size();
    const auto end = response.find('"', begin);
    assert(end != std::string::npos);
    return response.substr(begin, end - begin);
}

void SendOptions(boost::asio::ip::tcp::socket& socket,
                 const std::string& authorization = {}) {
    std::string request =
        "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n";
    if (!authorization.empty()) {
        request += "Authorization: " + authorization + "\r\n";
    }
    request += "\r\n";
    boost::asio::write(socket, boost::asio::buffer(request));
}

void TestBasicAndDigestAuthentication() {
    {
        RtspServerProtocol protocol;
        auto config = MakeConfig(FindFreeTcpPort());
        config.rtsp.auth_mode = RtspAuthMode::Basic;
        config.rtsp.auth_username = "admin";
        config.rtsp.auth_password = "secret";
        assert(protocol.Start(config, MakeTracks()));

        boost::asio::io_context client_io;
        boost::asio::ip::tcp::socket client(client_io);
        client.connect({boost::asio::ip::address_v4::loopback(),
                        config.listen_port});
        SendOptions(client);
        std::string response;
        assert(ReadRtspResponse(client, response));
        assert(response.find("RTSP/1.0 401 Unauthorized") != std::string::npos);
        assert(response.find("WWW-Authenticate: Basic realm=\"video-pipeline\"") !=
               std::string::npos);

        response.clear();
        SendOptions(client, "Basic YWRtaW46c2VjcmV0");
        assert(ReadRtspResponse(client, response));
        assert(response.find("RTSP/1.0 200 OK") != std::string::npos);
        protocol.Stop();
    }

    {
        RtspServerProtocol protocol;
        auto config = MakeConfig(FindFreeTcpPort());
        config.rtsp.auth_mode = RtspAuthMode::Digest;
        config.rtsp.auth_username = "admin";
        config.rtsp.auth_password = "secret";
        config.rtsp.auth_realm = "test-realm";
        assert(protocol.Start(config, MakeTracks()));

        boost::asio::io_context client_io;
        boost::asio::ip::tcp::socket client(client_io);
        client.connect({boost::asio::ip::address_v4::loopback(),
                        config.listen_port});
        SendOptions(client);
        std::string response;
        assert(ReadRtspResponse(client, response));
        assert(response.find("RTSP/1.0 401 Unauthorized") != std::string::npos);
        const auto nonce = ExtractDigestNonce(response);

        const std::string uri = "*";
        const std::string nc = "00000001";
        const std::string cnonce = "abcdef0123456789";
        const std::string qop = "auth";
        const auto ha1 = Md5HexForTest("admin:test-realm:secret");
        const auto ha2 = Md5HexForTest("OPTIONS:" + uri);
        const auto digest = Md5HexForTest(
            ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);
        const auto authorization =
            "Digest username=\"admin\", realm=\"test-realm\", nonce=\"" +
            nonce + "\", uri=\"*\", algorithm=MD5, qop=auth, nc=" + nc +
            ", cnonce=\"" + cnonce + "\", response=\"" + digest + "\"";

        response.clear();
        SendOptions(client, authorization);
        assert(ReadRtspResponse(client, response));
        assert(response.find("RTSP/1.0 200 OK") != std::string::npos);

        // 同一 nonce 下重复使用相同 nc 必须被拒绝，避免抓到一条合法
        // Authorization 后无限重放 RTSP method。
        response.clear();
        SendOptions(client, authorization);
        assert(ReadRtspResponse(client, response));
        assert(response.find("RTSP/1.0 401 Unauthorized") != std::string::npos);
        protocol.Stop();
    }
}

void TestConnectionLimitsAndStatistics() {
    {
        RtspServerProtocol protocol;
        auto config = MakeConfig(FindFreeTcpPort());
        config.rtsp.max_connections = 1;
        assert(protocol.Start(config, MakeTracks()));

        boost::asio::io_context client_io;
        boost::asio::ip::tcp::socket first(client_io);
        boost::asio::ip::tcp::socket second(client_io);
        first.connect({boost::asio::ip::address_v4::loopback(),
                       config.listen_port});
        second.connect({boost::asio::ip::address_v4::loopback(),
                        config.listen_port});
        // 第二个连接在进入 registry 前被全局容量拒绝；第一个连接仍然
        // 保持活动，证明 pending accept 不会错误释放已有 session。
        assert(WaitForPeerClose(second));
        const auto stats = protocol.GetStats();
        assert(stats.clients_connected == 1);
        assert(stats.connections_accepted == 1);
        assert(stats.connections_rejected == 1);
        assert(stats.connections_rejected_by_capacity == 1);
        first.close();
        protocol.Stop();
    }

    {
        RtspServerProtocol protocol;
        auto config = MakeConfig(FindFreeTcpPort());
        config.rtsp.connection_attempts_per_address = 1;
        assert(protocol.Start(config, MakeTracks()));

        boost::asio::io_context client_io;
        boost::asio::ip::tcp::socket first(client_io);
        boost::asio::ip::tcp::socket second(client_io);
        first.connect({boost::asio::ip::address_v4::loopback(),
                       config.listen_port});
        second.connect({boost::asio::ip::address_v4::loopback(),
                        config.listen_port});
        assert(WaitForPeerClose(second));
        const auto stats = protocol.GetStats();
        assert(stats.connections_accepted == 1);
        assert(stats.connections_rejected_by_rate_limit == 1);
        first.close();
        protocol.Stop();
    }

    {
        RtspServerProtocol protocol;
        auto config = MakeConfig(FindFreeTcpPort());
        config.rtsp.auth_mode = RtspAuthMode::Basic;
        config.rtsp.auth_username = "admin";
        config.rtsp.auth_password = "secret";
        config.rtsp.auth_failures_per_address = 2;
        assert(protocol.Start(config, MakeTracks()));

        boost::asio::io_context client_io;
        boost::asio::ip::tcp::socket client(client_io);
        client.connect({boost::asio::ip::address_v4::loopback(),
                        config.listen_port});
        const std::string wrong_auth = "Basic YWRtaW46d3Jvbmc=";
        std::string response;
        SendOptions(client, wrong_auth);
        assert(ReadRtspResponse(client, response));
        assert(response.find("RTSP/1.0 401 Unauthorized") != std::string::npos);
        response.clear();
        SendOptions(client, wrong_auth);
        assert(ReadRtspResponse(client, response));
        assert(response.find("RTSP/1.0 401 Unauthorized") != std::string::npos);
        assert(WaitForPeerClose(client));
        const auto stats = protocol.GetStats();
        assert(stats.auth_failures == 2);
        assert(stats.auth_failures_rejected == 1);
        protocol.Stop();
    }
}

void TestIdleTimeoutAndAddressAllowlist() {
    {
        RtspServerProtocol protocol;
        auto config = MakeConfig(FindFreeTcpPort());
        config.rtsp.session_idle_timeout_ms = 50;
        assert(protocol.Start(config, MakeTracks()));

        boost::asio::io_context client_io;
        boost::asio::ip::tcp::socket client(client_io);
        client.connect({boost::asio::ip::address_v4::loopback(),
                        config.listen_port});
        // 未发送 RTSP request 的空 session 必须在有限时间内释放，而不是
        // 一直占用 manager 和 TCP socket。
        assert(WaitForPeerClose(client));
        protocol.Stop();
    }

    {
        RtspServerProtocol protocol;
        auto config = MakeConfig(FindFreeTcpPort());
        config.rtsp.allowed_client_addresses = {"127.0.0.2"};
        assert(protocol.Start(config, MakeTracks()));

        boost::asio::io_context client_io;
        boost::asio::ip::tcp::socket client(client_io);
        client.connect({boost::asio::ip::address_v4::loopback(),
                        config.listen_port});
        // 当前客户端来自 127.0.0.1，而 allowlist 只允许 127.0.0.2；连接
        // 在进入 session registry 前被关闭，不能收到 RTSP response。
        assert(WaitForPeerClose(client));
        assert(protocol.GetStats().clients_connected == 0);
        protocol.Stop();
    }
}

void TestFailureRollbackAndRestart() {
    RtspServerProtocol protocol;
    const auto tracks = MakeTracks();

    // invalid listen host 在 acceptor 创建前失败；后续合法 Start 必须能够
    // 重新建立完整 listener，说明失败路径没有把半初始化对象提交给 façade。
    auto invalid_config = MakeConfig(FindFreeTcpPort());
    invalid_config.listen_host = "not-an-ip-address";
    const auto invalid_result = protocol.Start(invalid_config, tracks);
    assert(!invalid_result);
    assert(invalid_result.code == PublisherErrorCode::InvalidConfiguration);

    const auto valid_config = MakeConfig(FindFreeTcpPort());
    assert(protocol.Start(valid_config, tracks));
    assert(protocol.Write(MakeAccessUnit()));
    assert(protocol.GetStats().packets_published == 1);

    protocol.Stop();
    protocol.Stop(); // Stop 的二次调用不能等待不存在的线程或重复关闭资源。
    const auto stopped_write = protocol.Write(MakeAccessUnit());
    assert(!stopped_write);
    assert(stopped_write.code == PublisherErrorCode::InvalidState);

    // 同一个 protocol 实例停止后可以重新 Start；端口重新绑定成功即可证明
    // acceptor、work guard 和 io_context 的生命周期已经完整收敛。
    assert(protocol.Start(valid_config, tracks));
    protocol.Stop();
}

void TestConcurrentWriteAndStop() {
    RtspServerProtocol protocol;
    const auto config = MakeConfig(FindFreeTcpPort());
    const auto tracks = MakeTracks();
    assert(protocol.Start(config, tracks));

    const auto access_unit = MakeAccessUnit();
    std::atomic_bool keep_writing{true};
    std::thread writer([&]() {
        while (keep_writing.load(std::memory_order_relaxed)) {
            // Write 只把 payload 和 session/publisher 快照投递到 executor；
            // Stop 并发改变 started_ 时，已排队 handler 会自行丢弃。
            (void)protocol.Write(access_unit);
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    protocol.Stop();
    keep_writing.store(false, std::memory_order_relaxed);
    writer.join();
    assert(!protocol.Write(access_unit));
}

} // namespace

int main() {
    TestBasicAndDigestAuthentication();
    TestConnectionLimitsAndStatistics();
    TestIdleTimeoutAndAddressAllowlist();
    TestFailureRollbackAndRestart();
    TestConcurrentWriteAndStop();
    return 0;
}
