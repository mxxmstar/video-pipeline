#include "media/protocol/rtp_sender.h"

#include "media/protocol/rtcp_packet_codec.h"

#include <algorithm>
#include <cstddef>
#include <random>
#include <utility>

namespace {

constexpr std::size_t kRtpHeaderSize = 12;

std::uint32_t RandomU32() {
    static thread_local std::mt19937 generator{std::random_device{}()};
    return std::uniform_int_distribution<std::uint32_t>{}(generator);
}

std::uint16_t RandomU16() {
    static thread_local std::mt19937 generator{std::random_device{}()};
    return std::uniform_int_distribution<std::uint16_t>{}(generator);
}

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

} // namespace

RtpSender::RtpSender(RtpSenderConfig config)
    : config_(std::move(config)),
      next_sequence_(config_.initial_sequence) {
    if (config_.clock_rate == 0) {
        config_.clock_rate = 90000;
    }
    if (config_.sender_report_interval.count() <= 0) {
        config_.sender_report_interval = std::chrono::seconds{5};
    }
    if (!config_.now) {
        config_.now = []() { return std::chrono::steady_clock::now(); };
    }
}

RtpSender RtpSender::CreateDefault(std::uint8_t payload_type,
                                    std::uint32_t clock_rate) {
    RtpSenderConfig config;
    config.payload_type = payload_type;
    config.ssrc = RandomU32();
    config.initial_sequence = RandomU16();
    config.clock_rate = clock_rate;
    return RtpSender(std::move(config));
}

std::vector<std::uint8_t> RtpSender::BuildRtpPacket(
    const RtpPayload& payload,
    std::optional<std::uint8_t> payload_type) {
    if (payload.payload.empty()) {
        return {};
    }

    // 先分配固定 header，再复制 payload；vector 的所有权直接交给上层
    // transport，sender 自身只保留计数和最近 timestamp，不保存媒体 buffer。
    std::vector<std::uint8_t> packet(kRtpHeaderSize + payload.payload.size());
    auto* rtp = packet.data();
    rtp[0] = 0x80;
    const auto type = payload_type.value_or(config_.payload_type);
    rtp[1] = static_cast<std::uint8_t>((payload.marker ? 0x80 : 0) |
                                       (type & 0x7F));
    WriteU16(rtp + 2, next_sequence_++);
    WriteU32(rtp + 4, payload.timestamp);
    WriteU32(rtp + 8, config_.ssrc);
    std::copy(payload.payload.begin(),
              payload.payload.end(),
              packet.begin() + static_cast<std::ptrdiff_t>(kRtpHeaderSize));

    last_rtp_timestamp_ = payload.timestamp;
    has_rtp_packet_ = true;
    ++packet_count_;
    octet_count_ += static_cast<std::uint32_t>(payload.payload.size());
    return packet;
}

bool RtpSender::ShouldSendSenderReport() {
    if (!has_rtp_packet_) {
        return false;
    }

    const auto now = config_.now();
    if (next_sender_report_time_ != std::chrono::steady_clock::time_point{} &&
        now < next_sender_report_time_) {
        return false;
    }

    // 首个 RTP packet 后立即允许 SR，成功判断后才推进下一次 deadline，
    // 这样 TCP/UDP 的发送分支都使用同一套 5 秒门槛。
    next_sender_report_time_ = now + config_.sender_report_interval;
    return true;
}

std::vector<std::uint8_t> RtpSender::BuildSenderReport() const {
    if (!has_rtp_packet_) {
        return {};
    }
    return RtcpPacketCodec::BuildSenderReport(
        config_.ssrc,
        last_rtp_timestamp_,
        packet_count_,
        octet_count_);
}

RtpSenderSnapshot RtpSender::Snapshot() const {
    return RtpSenderSnapshot{
        config_.payload_type,
        config_.ssrc,
        next_sequence_,
        last_rtp_timestamp_,
        packet_count_,
        octet_count_,
        config_.clock_rate,
        has_rtp_packet_};
}
