#pragma once

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



class RtspServerProtocol : public IProtocol,
                           public std::enable_shared_from_this<RtspServerProtocol> {
public:
    RtspServerProtocol();
    ~RtspServerProtocol() override;

    RtspServerProtocol(const RtspServerProtocol&) = delete;
    RtspServerProtocol& operator=(const RtspServerProtocol&) = delete;

    bool Start(const PublisherConfig& config,
               const std::vector<MediaTrackConfig>& tracks) override;
    bool Write(const EncodedAccessUnit& access_unit) override;
    void Stop() override;

    std::string GetOutputUrl() const override;
    PublisherStats GetStats() const override;

    std::string BuildSdp(const std::string& host_for_sdp) const;
    void RemoveSession(std::uint64_t session_id);

private:
    class ClientSession;

    void AcceptNext();
    void OnAccepted(boost::system::error_code ec,
                    boost::asio::ip::tcp::socket socket);
    void SnapshotSessions(std::vector<std::shared_ptr<ClientSession>>& sessions) const;
    void UpdateH264ParameterSets(const EncodedAccessUnit& access_unit);

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
    mutable std::vector<std::weak_ptr<ClientSession>> sessions_;
    std::uint64_t next_session_id_{1};
    PublisherStats stats_;
    bool started_{false};
};


