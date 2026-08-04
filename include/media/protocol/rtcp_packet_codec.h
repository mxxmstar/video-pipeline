#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/// @brief RTCP report block 的字段快照。
///
/// cumulative_lost 是 RFC 3550 定义的 signed 24-bit 字段，解析时已经完成
/// 符号扩展，调用方无需再处理网络字节序或补码。
struct RtcpReportBlock {
    std::uint32_t source_ssrc{0};
    std::uint8_t fraction_lost{0};
    std::int32_t cumulative_lost{0};
    std::uint32_t extended_highest_sequence{0};
    std::uint32_t jitter{0};
    std::uint32_t last_sender_report{0};
    std::uint32_t delay_since_last_sender_report{0};
};

/// @brief 一个已经完成边界校验的 RTCP packet。
struct RtcpPacket {
    std::uint8_t packet_type{0};
    std::uint8_t report_count{0};
    // bytes 包含从 RTCP header 开始的完整 packet，长度已经按 header
    // length 字段验证为 32-bit word 的整数倍。
    std::vector<std::uint8_t> bytes;
};

/// @brief compound RTCP 解析结果。
struct RtcpCompoundParseResult {
    std::vector<RtcpPacket> packets;
    std::size_t error_offset{0};
    std::size_t trailing_bytes{0};
    std::size_t invalid_packet_size{0};
    std::uint8_t invalid_version{0};
    // valid=false 时 error_offset 指向第一个无法完整验证的 packet；
    // valid=true 且 trailing_bytes>0 表示末尾存在不足 4 字节的兼容性尾部。
    bool valid{true};
};

/// @brief 无状态 RTCP 编解码工具。
///
/// codec 只负责字节布局、长度边界和 report block 字段，不记录 sender
/// 统计、不拥有 timer/socket，也不输出日志。状态和错误语义由 session 或
/// multicast publisher 决定。
class RtcpPacketCodec {
public:
    static constexpr std::uint8_t kSenderReportPacketType = 200;
    static constexpr std::uint8_t kReceiverReportPacketType = 201;
    static constexpr std::uint8_t kSourceDescriptionPacketType = 202;
    static constexpr std::uint8_t kByePacketType = 203;
    static constexpr std::uint8_t kAppPacketType = 204;

    static std::vector<std::uint8_t> BuildSenderReport(
        std::uint32_t ssrc,
        std::uint32_t rtp_timestamp,
        std::uint32_t packet_count,
        std::uint32_t octet_count);

    /// 从 24 字节 report block 读取字段；data 按网络字节序排列。
    static bool ReadReportBlock(const std::uint8_t* data,
                                std::size_t size,
                                RtcpReportBlock& block);

    static RtcpCompoundParseResult ParseCompound(const std::uint8_t* data,
                                                  std::size_t size);

    static const char* PacketTypeName(std::uint8_t packet_type);
};
