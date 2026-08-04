#include "media/protocol/rtsp/rtsp_session.h"

#include <cassert>
#include <memory>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace {

void TestSessionManagerOwnershipAndRemoval() {
    boost::asio::io_context io;
    auto context = std::make_shared<RtspSessionContext>();
    context->io_executor = io.get_executor();
    context->config.rtsp.enable_tcp_interleaved = true;
    context->config.rtsp.enable_udp = false;
    context->tracks = {};

    auto manager = std::make_shared<RtspSessionManager>(context);
    auto first = manager->Create(boost::asio::ip::tcp::socket(io));
    auto second = manager->Create(boost::asio::ip::tcp::socket(io));

    // manager 必须持有活动 session 的 shared_ptr，且每个连接都拿到不重复的 ID。
    assert(first->Id() != second->Id());
    assert(manager->Size() == 2);
    assert(manager->Snapshot().size() == 2);

    // Stop 通过 connection executor 投递关闭；closed callback 再由 manager 移除。
    first->Stop();
    io.run();
    assert(manager->Size() == 1);
    assert(manager->Snapshot().front()->Id() == second->Id());

    manager->Clear();
    assert(manager->Size() == 0);
}

} // namespace

int main() {
    TestSessionManagerOwnershipAndRemoval();
    return 0;
}
