#include "media/protocol/rtsp/rtsp_builders.h"
#include "media/protocol/rtsp/rtsp_connection.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

namespace {

struct ConnectionEvents {
    std::mutex mutex;
    std::condition_variable condition;
    int request_count{0};
    int frame_count{0};
    std::uint8_t last_channel{0};
    std::vector<std::uint8_t> last_payload;
};

bool WaitForRequest(ConnectionEvents& events) {
    std::unique_lock<std::mutex> lock(events.mutex);
    return events.condition.wait_for(
        lock,
        std::chrono::seconds(2),
        [&events]() { return events.request_count == 1; });
}

bool WaitForFrame(ConnectionEvents& events) {
    std::unique_lock<std::mutex> lock(events.mutex);
    return events.condition.wait_for(
        lock,
        std::chrono::seconds(2),
        [&events]() { return events.frame_count == 1; });
}

bool ReadExactWithTimeout(boost::asio::ip::tcp::socket& socket,
                          std::vector<std::uint8_t>& data) {
    // 使用非阻塞读取并设置明确截止时间，避免连接层测试在写入异常时永久等待。
    socket.non_blocking(true);
    std::size_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(2);
    while (received < data.size() &&
           std::chrono::steady_clock::now() < deadline) {
        boost::system::error_code ec;
        const auto count = socket.read_some(
            boost::asio::buffer(data.data() + received,
                                data.size() - received),
            ec);
        if (!ec && count != 0) {
            received += count;
            continue;
        }
        if (ec == boost::asio::error::would_block ||
            ec == boost::asio::error::try_again) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        return false;
    }
    return received == data.size();
}

void TestFragmentedRequestAndSerializedWrites() {
    boost::asio::io_context server_io;
    boost::asio::ip::tcp::acceptor acceptor(
        server_io,
        {boost::asio::ip::tcp::v4(), 0});

    // acceptor 绑定到 0.0.0.0 时，local_endpoint() 返回的地址仍是未指定地址。
    // 客户端测试显式连接回环地址，避免 Windows 将 0.0.0.0 视为不可连接目标。
    const auto listen_endpoint = acceptor.local_endpoint();
    const boost::asio::ip::tcp::endpoint endpoint(
        boost::asio::ip::address_v4::loopback(), listen_endpoint.port());

    ConnectionEvents events;
    std::shared_ptr<RtspConnection> connection;
    std::mutex connection_mutex;
    std::condition_variable connection_condition;
    bool accepted = false;

    acceptor.async_accept(
        [&connection,
         &connection_mutex,
         &connection_condition,
         &accepted,
         &events](boost::system::error_code ec,
                  boost::asio::ip::tcp::socket socket) {
            assert(!ec);
            auto accepted_connection = std::make_shared<RtspConnection>(
                std::move(socket),
                [&events](RtspRequest request) {
                    assert(request.method == "OPTIONS");
                    std::lock_guard<std::mutex> lock(events.mutex);
                    ++events.request_count;
                    events.condition.notify_all();
                },
                [&events](std::uint8_t channel,
                          std::vector<std::uint8_t> payload) {
                    std::lock_guard<std::mutex> lock(events.mutex);
                    ++events.frame_count;
                    events.last_channel = channel;
                    events.last_payload = std::move(payload);
                    events.condition.notify_all();
                },
                RtspConnection::ParseErrorHandler{},
                [](const boost::system::error_code&) {});
            {
                std::lock_guard<std::mutex> lock(connection_mutex);
                connection = std::move(accepted_connection);
                accepted = true;
            }
            connection_condition.notify_all();
            connection->Start();
        });

    std::thread server_thread([&server_io]() { server_io.run(); });

    boost::asio::io_context client_io;
    boost::asio::ip::tcp::socket client(client_io);
    client.connect(endpoint);
    {
        std::unique_lock<std::mutex> lock(connection_mutex);
        assert(connection_condition.wait_for(
            lock,
            std::chrono::seconds(2),
            [&accepted]() { return accepted; }));
    }

    // 将一个 RTSP request 拆成两次 TCP 写入，验证 parser 能正确等待终止空行。
    const std::string first_part =
        "OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n";
    const std::string second_part = "\r\n";
    boost::asio::write(client, boost::asio::buffer(first_part));
    boost::asio::write(client, boost::asio::buffer(second_part));
    assert(WaitForRequest(events));

    // RTSP response 与 interleaved frame 必须共享一个写队列，并按入队顺序发送。
    const std::vector<std::uint8_t> payload{1, 2, 3, 4};
    const auto response = RtspResponseBuilder::Build(200, "OK", "1");
    const auto response_size = response.size();
    connection->SendRtsp(response);
    connection->SendInterleaved(1, payload);

    std::vector<std::uint8_t> received(response_size + 4 + payload.size());
    assert(ReadExactWithTimeout(client, received));
    assert(std::equal(response.begin(), response.end(), received.begin()));
    assert(received[response_size] == '$');
    assert(received[response_size + 1] == 1);
    assert(received[response_size + 2] == 0);
    assert(received[response_size + 3] == payload.size());
    assert(std::equal(payload.begin(),
                      payload.end(),
                      received.begin() +
                          static_cast<std::ptrdiff_t>(response_size + 4)));

    // 发送一个完整的 interleaved frame，验证读取侧能提取 channel 和 payload。
    const std::vector<std::uint8_t> incoming_payload{9, 8, 7};
    std::vector<std::uint8_t> incoming_frame(4 + incoming_payload.size());
    incoming_frame[0] = '$';
    incoming_frame[1] = 3;
    incoming_frame[2] = 0;
    incoming_frame[3] = static_cast<std::uint8_t>(incoming_payload.size());
    std::copy(incoming_payload.begin(),
              incoming_payload.end(),
              incoming_frame.begin() + 4);
    boost::asio::write(client, boost::asio::buffer(incoming_frame));
    assert(WaitForFrame(events));
    {
        std::lock_guard<std::mutex> lock(events.mutex);
        assert(events.last_channel == 3);
        assert(events.last_payload == incoming_payload);
    }

    // 关闭请求必须在现有写队列排空后执行，避免截断 response 或 frame。
    connection->CloseAfterFlush();
    connection.reset();
    client.close();
    acceptor.close();
    server_io.stop();
    server_thread.join();
}

} // namespace

int main() {
    TestFragmentedRequestAndSerializedWrites();
    return 0;
}
