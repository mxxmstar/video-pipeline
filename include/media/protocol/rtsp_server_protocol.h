#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>

#include "media/protocol/h264_bitstream.h"
#include "media/protocol/h264_rtp_packetizer.h"
#include "media/protocol/i_protocol.h"
#include "media/protocol/rtsp/rtsp_transport_spec.h"



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
    void RemoveSession(std::uint64_t session_id);

private:
    class ClientSession;
    struct MulticastRtcpFeedback {
        std::uint32_t media_ssrc{0};
        std::uint8_t fraction_lost{0};
        std::int32_t cumulative_lost{0};
        std::uint32_t extended_highest_sequence{0};
        std::uint32_t jitter{0};
        std::uint32_t last_sender_report{0};
        std::uint32_t delay_since_last_sender_report{0};
        std::uint64_t reports_received{0};
    };

    void AcceptNext();
    void OnAccepted(boost::system::error_code ec,
                    boost::asio::ip::tcp::socket socket);
    void SnapshotSessions(std::vector<std::shared_ptr<ClientSession>>& sessions) const;
    void UpdateH264ParameterSets(const EncodedAccessUnit& access_unit);
    void AddRtcpReceiverReportsReceived(std::uint64_t count);
    bool ConfigureSharedMulticastTransport(RtspTransportSpec& transport_spec,
                                           const std::string& source_address);
    void CloseSharedMulticastTransport();
    void SendMulticastRtpPayload(const RtpPayload& payload,
                                 std::uint8_t payload_type);
    void SendMulticastRtcpSenderReport();
    void StartMulticastRtcpReceive();
    void HandleMulticastRtcpPacket(
        const std::uint8_t* data,
        std::size_t size,
        const boost::asio::ip::udp::endpoint& sender_endpoint);
    void ParseMulticastRtcpReceiverReport(
        const std::uint8_t* packet,
        std::size_t packet_size,
        std::uint8_t report_count,
        const boost::asio::ip::udp::endpoint& sender_endpoint);
    void ParseMulticastRtcpSenderReport(
        const std::uint8_t* packet,
        std::size_t packet_size,
        std::uint8_t report_count,
        const boost::asio::ip::udp::endpoint& sender_endpoint);
    void ParseMulticastRtcpReportBlocks(
        const std::uint8_t* blocks,
        std::uint8_t report_count,
        std::uint32_t reporter_ssrc,
        const char* packet_type_name,
        const boost::asio::ip::udp::endpoint& sender_endpoint);
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
    std::shared_ptr<boost::asio::ip::udp::socket> multicast_rtcp_receiver_socket_;
    boost::asio::ip::udp::endpoint multicast_rtp_endpoint_;
    boost::asio::ip::udp::endpoint multicast_rtcp_endpoint_;
    boost::asio::ip::udp::endpoint multicast_rtcp_sender_endpoint_;
    std::array<std::uint8_t, 2048> multicast_rtcp_read_buffer_{};
    RtspTransportSpec multicast_transport_spec_;
    std::uint32_t multicast_ssrc_{0};
    std::uint16_t multicast_sequence_{0};
    std::uint32_t multicast_last_rtp_timestamp_{0};
    std::uint32_t multicast_rtcp_packet_count_{0};
    std::uint32_t multicast_rtcp_octet_count_{0};
    std::chrono::steady_clock::time_point multicast_next_rtcp_sr_time_{};
    std::unordered_map<std::uint32_t, MulticastRtcpFeedback>
        multicast_rtcp_feedback_by_reporter_;
    bool multicast_ready_{false};

    mutable std::mutex mutex_;
    mutable std::vector<std::weak_ptr<ClientSession>> sessions_;
    std::uint64_t next_session_id_{1};
    PublisherStats stats_;
    bool started_{false};
};
