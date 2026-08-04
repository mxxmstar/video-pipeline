#include "media/protocol/rtsp_server_protocol.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>

#include <boost/asio/ip/tcp.hpp>

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
    TestFailureRollbackAndRestart();
    TestConcurrentWriteAndStop();
    return 0;
}
