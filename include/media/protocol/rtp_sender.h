#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "media/protocol/h264_rtp_packetizer.h"

/// @brief per-track RTP sender 的初始化配置。
///
/// SSRC 和 initial_sequence 可以由测试显式注入；生产代码使用
/// RtpSender::CreateDefault() 生成随机值。clock_rate 当前作为 track 时钟
/// 快照保留，后续可用于更精确的 timestamp/NTP 校验，但不改变现有 SR 字节语义。
struct RtpSenderConfig {
    std::uint8_t payload_type{96};
    std::uint32_t ssrc{0};
    std::uint16_t initial_sequence{0};
    std::uint32_t clock_rate{90000};
    std::chrono::milliseconds sender_report_interval{5000};
    std::function<std::chrono::steady_clock::time_point()> now;
};

/// @brief RTP sender 的只读统计快照。
///
/// `next_sequence` 是下一个 RTP packet 将使用的 sequence；因此它与 RTSP
/// PLAY response 的 RTP-Info 语义一致。packet/octet count 在生成 RTP packet
/// 时递增，octet 只包含 RTP payload，不包含 12 字节 RTP header。
struct RtpSenderSnapshot {
    std::uint8_t payload_type{96};
    std::uint32_t ssrc{0};
    std::uint16_t next_sequence{0};
    std::uint32_t last_rtp_timestamp{0};
    std::uint32_t packet_count{0};
    std::uint32_t octet_count{0};
    std::uint32_t clock_rate{90000};
    bool has_rtp_packet{false};
};

/// @brief 集中管理单条 RTP track 的 header、序列和 RTCP sender 状态。
///
/// sender 不拥有 socket、不知道 TCP/UDP，也不调用 transport；调用方先用
/// BuildRtpPacket() 生成完整 RTP packet，再把返回 bytes 交给
/// `IRtspMediaTransport`。所有状态方法都应在所属 track 的串行执行边界调用，
/// 但对象本身不强制绑定某个 Asio executor，便于纯单元测试。
class RtpSender {
public:
    explicit RtpSender(RtpSenderConfig config = {});

    /// 使用随机 SSRC/sequence 创建生产 sender；测试请直接使用构造配置。
    static RtpSender CreateDefault(
        std::uint8_t payload_type,
        std::uint32_t clock_rate);

    /// 构造 RTP v2 header 和 payload。空 payload 返回空 vector，且不修改
    /// sequence、timestamp 或 sender counters；sequence/timestamp 按 uint16/32
    /// 自然回绕，保持 RFC 3550 的网络字段语义。
    std::vector<std::uint8_t> BuildRtpPacket(
        const RtpPayload& payload,
        std::optional<std::uint8_t> payload_type = std::nullopt);

    /// 判断当前 sender report 是否到期。首个 RTP packet 后立即到期，之后
    /// 按配置间隔判断；无 RTP packet 时始终返回 false。
    bool ShouldSendSenderReport();

    /// 使用当前 sender snapshot 构建 RTCP SR；没有 RTP packet 时返回空 vector。
    std::vector<std::uint8_t> BuildSenderReport() const;

    RtpSenderSnapshot Snapshot() const;

private:
    RtpSenderConfig config_;
    std::uint16_t next_sequence_{0};
    std::uint32_t last_rtp_timestamp_{0};
    std::uint32_t packet_count_{0}; ///< 累计发送的 RTP 包数量
    std::uint32_t octet_count_{0}; ///< 累计发送的 RTP payload 字节数(不包含12字节的 RTP header)
    std::chrono::steady_clock::time_point next_sender_report_time_{};
    bool has_rtp_packet_{false};
};

