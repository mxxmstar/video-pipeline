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
#include <boost/asio/ip/udp.hpp>

#include "media/protocol/h264_bitstream.h"
#include "media/protocol/h264_rtp_packetizer.h"
#include "media/protocol/i_protocol.h"
#include "media/protocol/rtsp_transport_spec.h"



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
    bool ConfigureSharedMulticastTransport(RtspTransportSpec& transport_spec,
                                           const std::string& source_address);
    void CloseSharedMulticastTransport();
    void SendMulticastRtpPayload(const RtpPayload& payload,
                                 std::uint8_t payload_type);
    std::uint16_t GetMulticastSequence() const;
    std::uint32_t GetMulticastLastRtpTimestamp() const;

    PublisherConfig config_;
    std::vector<MediaTrackConfig> tracks_;
    H264ParameterSets h264_parameter_sets_;
    H264RtpPacketizer h264_packetizer_;

    boost::asio::io_context io_;
    std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
    using WorkGuard = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
    std::unique_ptr<WorkGuard> work_guard_;
    std::thread io_thread_;

    std::shared_ptr<boost::asio::ip::udp::socket> multicast_rtp_socket_;
    std::shared_ptr<boost::asio::ip::udp::socket> multicast_rtcp_socket_;
    boost::asio::ip::udp::endpoint multicast_rtp_endpoint_;
    boost::asio::ip::udp::endpoint multicast_rtcp_endpoint_;
    RtspTransportSpec multicast_transport_spec_;
    std::uint32_t multicast_ssrc_{0};
    std::uint16_t multicast_sequence_{0};
    std::uint32_t multicast_last_rtp_timestamp_{0};
    bool multicast_ready_{false};

    mutable std::mutex mutex_;
    mutable std::vector<std::weak_ptr<ClientSession>> sessions_;
    std::uint64_t next_session_id_{1};
    PublisherStats stats_;
    bool started_{false};
};
