#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>

#include "media/protocol/h264_bitstream.h"
#include "media/protocol/h264_rtp_packetizer.h"
#include "media/protocol/i_protocol.h"

struct RtspSessionContext;
class RtspSessionManager;
class RtspMulticastPublisher;


class RtspServerProtocol : public IProtocol,
                           public std::enable_shared_from_this<RtspServerProtocol> {
public:
    RtspServerProtocol();
    ~RtspServerProtocol() override;

    RtspServerProtocol(const RtspServerProtocol&) = delete;
    RtspServerProtocol& operator=(const RtspServerProtocol&) = delete;

    PublisherResult Start(
        const PublisherConfig& config,
        const std::vector<MediaTrackConfig>& tracks) override;
    PublisherResult Write(const EncodedAccessUnit& access_unit) override;
    void Stop() override;

    std::string GetOutputUrl() const override;
    PublisherStats GetStats() const override;

    std::string BuildSdp(const std::string& host_for_sdp) const;

private:
    void AcceptNext();
    void OnAccepted(boost::system::error_code ec,
                    boost::asio::ip::tcp::socket socket);
    void UpdateH264ParameterSets(const EncodedAccessUnit& access_unit);
    void AddRtcpReceiverReportsReceived(std::uint64_t count);

    PublisherConfig config_;
    std::vector<MediaTrackConfig> tracks_;
    H264ParameterSets h264_parameter_sets_;
    H264RtpPacketizer h264_packetizer_;

    boost::asio::io_context io_;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
    std::unique_ptr<WorkGuard> work_guard_;
    std::thread io_thread_;

    mutable std::mutex mutex_;
    // S3 由独立 manager 持有活动 session；façade 不直接维护 registry、ID 或关闭回收。
    std::shared_ptr<RtspSessionContext> session_context_;
    std::shared_ptr<RtspSessionManager> session_manager_;
    // S6 将 server 级 multicast socket、共享 sender 和 RR feedback 完整封装；
    // façade 只保留 publisher 指针，不直接暴露任何 UDP 细节。
    std::shared_ptr<RtspMulticastPublisher> multicast_publisher_;
    PublisherStats stats_;
    bool started_{false};
};
