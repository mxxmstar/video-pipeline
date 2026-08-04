#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/udp.hpp>

#include "media/publisher/publisher_config.h"
#include "media/protocol/rtcp_packet_codec.h"
#include "media/protocol/rtp_sender.h"
#include "media/protocol/rtsp/rtsp_transport_spec.h"

/// @brief 一个 multicast receiver 按 reporter SSRC 聚合后的 RR 快照。
struct RtspMulticastReceiverFeedbackSnapshot {
    RtcpReportBlock report;
    std::uint64_t reports_received{0};
};

/// @brief server 级 multicast publisher 的只读统计快照。
struct RtspMulticastPublisherStatsSnapshot {
    bool ready{false};
    RtpSenderSnapshot sender;
    std::uint64_t receiver_reports_received{0};
    std::unordered_map<std::uint32_t, RtspMulticastReceiverFeedbackSnapshot>
        feedback_by_reporter;
};

/// @brief 共享的单 H264 track multicast 发布器。
///
/// publisher 独占 RTP 发送 socket、RTCP SR 发送 socket 和 RTCP RR 接收
/// socket。多个 RTSP session 只保存 `RtspMulticastSubscription`，不会为每个
/// 客户端创建发送循环。Publish() 可由 pipeline 线程调用，实际 UDP 操作统一
/// post 到构造时的 Asio executor；Close() 幂等且关闭后不可重新 Configure。
class RtspMulticastPublisher
    : public std::enable_shared_from_this<RtspMulticastPublisher> {
public:
    using ReceiverReportHandler = std::function<void(std::uint64_t)>;

    RtspMulticastPublisher(
        boost::asio::any_io_executor io_executor,
        RtspServerOptions options,
        std::uint8_t payload_type,
        std::uint32_t clock_rate,
        ReceiverReportHandler on_receiver_reports);

    RtspMulticastPublisher(const RtspMulticastPublisher&) = delete;
    RtspMulticastPublisher& operator=(const RtspMulticastPublisher&) = delete;

    /// 配置共享组播资源。重复调用只返回首次成功配置的 immutable response。
    /// 失败时不会保留半初始化 socket，调用方可在未 Close 前重试。
    bool Configure(RtspTransportSpec& transport_spec,
                   const std::string& source_address);

    /// 接收已经由 H264 packetizer 生成的 RTP payload，并通过共享 sender
    /// 添加 header；payload 为空或 publisher 未 ready 时不发送。
    void Publish(const RtpPayload& payload, std::uint8_t payload_type);

    /// 关闭三个 socket、停止 receive loop 并清空 sender/feedback；幂等。
    void Close();

    bool IsReady() const;
    RtspTransportSpec GetTransportSpec() const;
    std::uint16_t GetSequence() const;
    std::uint32_t GetLastRtpTimestamp() const;
    RtspMulticastPublisherStatsSnapshot GetStats() const;

private:
    struct MulticastRtcpFeedback {
        RtcpReportBlock report;
        std::uint64_t reports_received{0};
    };

    static bool IsIpv4Multicast(const boost::asio::ip::address& address);

    void StartRtcpReceive();
    void HandleRtcpPacket(const std::uint8_t* data,
                          std::size_t size,
                          const boost::asio::ip::udp::endpoint& sender_endpoint);
    void ParseReceiverReport(const std::uint8_t* packet,
                             std::size_t packet_size,
                             std::uint8_t report_count,
                             const boost::asio::ip::udp::endpoint& sender_endpoint);
    void ParseSenderReport(const std::uint8_t* packet,
                           std::size_t packet_size,
                           std::uint8_t report_count,
                           const boost::asio::ip::udp::endpoint& sender_endpoint);
    void ParseReportBlocks(const std::uint8_t* blocks,
                           std::uint8_t report_count,
                           std::uint32_t reporter_ssrc,
                           const char* packet_type_name,
                           const boost::asio::ip::udp::endpoint& sender_endpoint);
    void CloseSocket(const std::shared_ptr<boost::asio::ip::udp::socket>& socket);

    boost::asio::any_io_executor io_executor_;
    RtspServerOptions options_;
    std::uint8_t payload_type_{96};
    std::uint32_t clock_rate_{90000};
    ReceiverReportHandler on_receiver_reports_;

    mutable std::mutex mutex_;
    std::shared_ptr<boost::asio::ip::udp::socket> rtp_socket_;
    std::shared_ptr<boost::asio::ip::udp::socket> rtcp_socket_;
    std::shared_ptr<boost::asio::ip::udp::socket> rtcp_receiver_socket_;
    boost::asio::ip::udp::endpoint rtp_endpoint_;
    boost::asio::ip::udp::endpoint rtcp_endpoint_;
    RtspTransportSpec transport_spec_;
    std::unique_ptr<RtpSender> sender_;
    std::unordered_map<std::uint32_t, MulticastRtcpFeedback>
        feedback_by_reporter_;
    std::uint64_t receiver_reports_received_{0};
    bool ready_{false};
    bool closed_{false};
};
