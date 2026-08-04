#include "media/protocol/rtcp_packet_codec.h"

#include <chrono>

namespace {

constexpr std::size_t kRtcpSenderReportSize = 28;
constexpr std::size_t kRtcpReportBlockSize = 24;
constexpr std::uint64_t kUnixToNtpSeconds = 2208988800ULL;

void WriteU16(std::uint8_t* data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[1] = static_cast<std::uint8_t>(value & 0xFF);
}

void WriteU32(std::uint8_t* data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    data[1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    data[2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[3] = static_cast<std::uint8_t>(value & 0xFF);
}

std::uint16_t ReadU16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) |
                                      static_cast<std::uint16_t>(data[1]));
}

std::uint32_t ReadU32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

std::int32_t ReadS24(const std::uint8_t* data) {
    auto value = (static_cast<std::uint32_t>(data[0]) << 16) |
                 (static_cast<std::uint32_t>(data[1]) << 8) |
                 static_cast<std::uint32_t>(data[2]);
    if ((value & 0x00800000U) != 0) {
        value |= 0xFF000000U;
    }
    return static_cast<std::int32_t>(value);
}

} // namespace

std::vector<std::uint8_t> RtcpPacketCodec::BuildSenderReport(
    std::uint32_t ssrc,
    std::uint32_t rtp_timestamp,
    std::uint32_t packet_count,
    std::uint32_t octet_count) {
    using namespace std::chrono;

    // NTP epoch 比 Unix epoch 早 2208988800 秒；RTCP 的 NTP timestamp
    // 和所有整数域都按 RFC 3550 使用大端网络字节序写入。
    const auto now = system_clock::now().time_since_epoch();
    const auto seconds_part = duration_cast<seconds>(now);
    const auto nanoseconds_part =
        duration_cast<nanoseconds>(now - seconds_part).count();

    const auto ntp_seconds = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(seconds_part.count()) + kUnixToNtpSeconds);
    const auto ntp_fraction = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(nanoseconds_part) << 32) / 1000000000ULL);

    // SR 固定为 28 字节：header(4)+SSRC(4)+NTP(8)+RTP timestamp(4)+
    // sender packet count(4)+sender octet count(4)，length=6 表示 7 个 word-1。
    std::vector<std::uint8_t> report(kRtcpSenderReportSize);
    report[0] = 0x80; // V=2, P=0, RC=0。
    report[1] = kSenderReportPacketType;
    // 28 字节 RTCP SR 占 7 个 32bit word，length 字段按 RFC 3550 减一。
    WriteU16(report.data() + 2, 6);
    WriteU32(report.data() + 4, ssrc);
    WriteU32(report.data() + 8, ntp_seconds);
    WriteU32(report.data() + 12, ntp_fraction);
    WriteU32(report.data() + 16, rtp_timestamp);
    WriteU32(report.data() + 20, packet_count);
    WriteU32(report.data() + 24, octet_count);
    return report;
}

bool RtcpPacketCodec::ReadReportBlock(const std::uint8_t* data,
                                      std::size_t size,
                                      RtcpReportBlock& block) {
    if (!data || size < kRtcpReportBlockSize) {
        return false;
    }

    block.source_ssrc = ReadU32(data);
    block.fraction_lost = data[4];
    block.cumulative_lost = ReadS24(data + 5);
    block.extended_highest_sequence = ReadU32(data + 8);
    block.jitter = ReadU32(data + 12);
    block.last_sender_report = ReadU32(data + 16);
    block.delay_since_last_sender_report = ReadU32(data + 20);
    return true;
}

RtcpCompoundParseResult RtcpPacketCodec::ParseCompound(
    const std::uint8_t* data,
    std::size_t size) {
    RtcpCompoundParseResult result;
    if (!data && size != 0) {
        result.valid = false;
        result.error_offset = 0;
        return result;
    }

    // 先验证每个 packet 的 4 字节 header 和 length，再复制边界内的完整
    // packet；这样上层解析 report block 时不会读到下一个 compound packet。
    std::size_t offset = 0;
    while (offset + 4 <= size) {
        const auto* packet = data + offset;
        const auto version = static_cast<std::uint8_t>(packet[0] >> 6);
        const auto report_count = static_cast<std::uint8_t>(packet[0] & 0x1F);
        const auto packet_type = packet[1];
        const auto packet_size =
            (static_cast<std::size_t>(ReadU16(packet + 2)) + 1U) * 4U;

        if (version != 2 || packet_size < 4 || packet_size > size - offset) {
            result.valid = false;
            result.error_offset = offset;
            result.invalid_packet_size = packet_size;
            result.invalid_version = version;
            return result;
        }

        RtcpPacket parsed;
        parsed.packet_type = packet_type;
        parsed.report_count = report_count;
        parsed.bytes.assign(packet, packet + packet_size);
        result.packets.push_back(std::move(parsed));
        offset += packet_size;
    }

    result.trailing_bytes = size - offset;
    return result;
}

const char* RtcpPacketCodec::PacketTypeName(std::uint8_t packet_type) {
    switch (packet_type) {
    case kSenderReportPacketType:
        return "SR";
    case kReceiverReportPacketType:
        return "RR";
    case kSourceDescriptionPacketType:
        return "SDES";
    case kByePacketType:
        return "BYE";
    case kAppPacketType:
        return "APP";
    default:
        return "UNKNOWN";
    }
}
