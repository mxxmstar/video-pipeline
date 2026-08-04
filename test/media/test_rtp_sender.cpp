#include "media/protocol/rtp_sender.h"

#include "media/protocol/rtcp_packet_codec.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

std::uint16_t ReadU16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8) |
        static_cast<std::uint16_t>(data[1]));
}

std::uint32_t ReadU32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

void TestRtpHeaderCountersAndWrap() {
    using Clock = std::chrono::steady_clock;
    auto now = std::make_shared<Clock::time_point>(Clock::time_point{});

    RtpSenderConfig config;
    config.payload_type = 96;
    config.ssrc = 0x01020304;
    config.initial_sequence = 0xffff;
    config.clock_rate = 90000;
    config.now = [now]() { return *now; };

    RtpSender sender(config);
    assert(sender.BuildRtpPacket(RtpPayload{}).empty());
    assert(!sender.ShouldSendSenderReport());
    assert(sender.Snapshot().packet_count == 0);

    RtpPayload first;
    first.timestamp = 0xffffffffU;
    first.marker = true;
    first.payload = {1, 2, 3};
    const auto first_packet = sender.BuildRtpPacket(first);
    assert(first_packet.size() == 15);
    assert(first_packet[0] == 0x80);
    assert(first_packet[1] == 0xe0); // marker=1, dynamic payload type 96
    assert(ReadU16(first_packet.data() + 2) == 0xffff);
    assert(ReadU32(first_packet.data() + 4) == 0xffffffffU);
    assert(ReadU32(first_packet.data() + 8) == 0x01020304U);
    assert(sender.Snapshot().next_sequence == 0);
    assert(sender.Snapshot().octet_count == 3);

    // 首包立即允许 SR；再次检查必须等到 5 秒门槛，避免 TCP/UDP 两条路径
    // 对同一 track 重复发送 sender report。
    assert(sender.ShouldSendSenderReport());
    assert(!sender.ShouldSendSenderReport());
    const auto report = sender.BuildSenderReport();
    assert(report.size() == 28);
    assert(ReadU32(report.data() + 4) == 0x01020304U);
    assert(ReadU32(report.data() + 16) == 0xffffffffU);
    assert(ReadU32(report.data() + 20) == 1);
    assert(ReadU32(report.data() + 24) == 3);

    *now += std::chrono::seconds{4};
    assert(!sender.ShouldSendSenderReport());
    *now += std::chrono::seconds{1};
    assert(sender.ShouldSendSenderReport());

    RtpPayload second;
    second.timestamp = 0;
    second.payload = {4};
    const auto second_packet = sender.BuildRtpPacket(second);
    assert(ReadU16(second_packet.data() + 2) == 0);
    assert(ReadU32(second_packet.data() + 4) == 0);
    assert(sender.Snapshot().packet_count == 2);
    assert(sender.Snapshot().octet_count == 4);
}

void TestSenderReportUsesCurrentState() {
    RtpSenderConfig config;
    config.payload_type = 8;
    config.ssrc = 9;
    config.initial_sequence = 10;
    config.clock_rate = 8000;
    RtpSender sender(config);

    RtpPayload payload;
    payload.timestamp = 1234;
    payload.payload = {0xaa};
    sender.BuildRtpPacket(payload);
    assert(sender.ShouldSendSenderReport());

    const auto report = sender.BuildSenderReport();
    const auto parsed = RtcpPacketCodec::ParseCompound(
        report.data(),
        report.size());
    // 这里验证 sender 生成的 bytes 可以直接交给 codec 解析；sender report
    // 的完整字段断言在上一个测试中覆盖，避免复制 codec 的字段读取逻辑。
    assert(parsed.valid);
    assert(parsed.packets.size() == 1);
    assert(parsed.packets.front().packet_type ==
           RtcpPacketCodec::kSenderReportPacketType);
}

} // namespace

int main() {
    TestRtpHeaderCountersAndWrap();
    TestSenderReportUsesCurrentState();
    return 0;
}
