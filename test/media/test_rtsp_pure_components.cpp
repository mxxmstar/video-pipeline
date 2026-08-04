#include "media/protocol/audio_rtp_packetizer.h"
#include "media/protocol/rtcp_packet_codec.h"
#include "media/protocol/rtsp/rtsp_builders.h"
#include "media/simple_buffer.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

EncodedAccessUnit MakeAudioAccessUnit(
    CodecType codec,
    std::vector<std::uint8_t> bytes,
    std::int64_t pts = 1234) {
    EncodedAccessUnit access_unit;
    access_unit.media_type = MediaType::AUDIO;
    access_unit.codec_type = codec;
    access_unit.pts = pts;
    access_unit.encoded_data =
        std::make_shared<SimpleBuffer>(std::move(bytes));
    return access_unit;
}

void TestResponseBuilder() {
    const auto response = RtspResponseBuilder::Build(
        200,
        "OK",
        "7",
        {{"Session", "abcd"}},
        "v=0\r\n",
        "application/sdp");
    assert(response.rfind("RTSP/1.0 200 OK\r\n", 0) == 0);
    assert(response.find("CSeq: 7\r\n") != std::string::npos);
    assert(response.find("Session: abcd\r\n") != std::string::npos);
    assert(response.find("Content-Type: application/sdp\r\n") != std::string::npos);
    assert(response.find("Content-Length: 5\r\n") != std::string::npos);
    assert(response.ends_with("\r\nv=0\r\n"));

    const auto empty_response = RtspResponseBuilder::Build(400, "Bad Request", {});
    assert(empty_response.find("Content-Length: 0\r\n") != std::string::npos);
    assert(empty_response.find("CSeq:") == std::string::npos);

    // 纯 builder 不判断状态码语义，只需保证所有常用错误状态仍按原有
    // status-line 序列化，真正的状态机决策留在 ClientSession。
    for (const auto& status : {
             std::pair{405, std::string{"Method Not Allowed"}},
             std::pair{455, std::string{"Method Not Valid In This State"}},
             std::pair{461, std::string{"Unsupported Transport"}},
             std::pair{505, std::string{"RTSP Version Not Supported"}}}) {
        const auto status_response =
            RtspResponseBuilder::Build(status.first, status.second, "8");
        assert(status_response.rfind(
                   "RTSP/1.0 " + std::to_string(status.first) + " " +
                       status.second + "\r\n",
                   0) == 0);
    }
}

void TestSdpBuilder() {
    PublisherConfig config;
    config.rtsp.enable_udp = true;
    config.rtsp.enable_multicast = true;
    config.rtsp.multicast_address = "239.255.42.1";
    config.rtsp.multicast_rtp_port = 5004;
    config.rtsp.multicast_ttl = 16;

    MediaTrackConfig video;
    video.track_id = 0;
    video.media_type = MediaType::VIDEO;
    video.codec_type = CodecType::H264;
    video.rtp_payload_type = 96;
    video.rtp_clock_rate = 90000;

    MediaTrackConfig audio;
    audio.track_id = 1;
    audio.media_type = MediaType::AUDIO;
    audio.codec_type = CodecType::AAC;
    audio.rtp_payload_type = 97;
    audio.rtp_clock_rate = 48000;
    audio.channels = 2;
    audio.extra_data = {0x11, 0x90};

    H264ParameterSets parameter_sets;
    parameter_sets.sps = {0x67, 0x42, 0xe0, 0x1f};
    parameter_sets.pps = {0x68, 0xce, 0x06, 0xe2};
    const auto sdp = RtspSdpBuilder::Build(
        config, {video, audio}, parameter_sets, "127.0.0.1");

    assert(sdp.find("c=IN IP4 239.255.42.1/16\r\n") != std::string::npos);
    assert(sdp.find("a=type:broadcast\r\n") != std::string::npos);
    assert(sdp.find("m=video 5004 RTP/AVP 96\r\n") != std::string::npos);
    assert(sdp.find("a=control:track0\r\n") != std::string::npos);
    assert(sdp.find("MPEG4-GENERIC/48000/2\r\n") != std::string::npos);
    assert(sdp.find("config=1190\r\n") != std::string::npos);

    // 关闭 multicast 后，SDP 必须回到原来的 unicast 描述：连接地址为
    // 0.0.0.0，媒体端口保持 0，由 SETUP 的 Transport 决定实际端口。
    config.rtsp.enable_multicast = false;
    const auto unicast_sdp = RtspSdpBuilder::Build(
        config, {video}, H264ParameterSets{}, "192.0.2.10");
    assert(unicast_sdp.find("o=- 0 1 IN IP4 192.0.2.10\r\n") != std::string::npos);
    assert(unicast_sdp.find("c=IN IP4 0.0.0.0\r\n") != std::string::npos);
    assert(unicast_sdp.find("m=video 0 RTP/AVP 96\r\n") != std::string::npos);
    // 缺少 SPS/PPS 时仍生成 H264 fmtp，但不能伪造 sprop-parameter-sets。
    assert(unicast_sdp.find("sprop-parameter-sets=") == std::string::npos);

    // G711 是静态 payload 外也允许的显式 RTP 描述，验证 codec 分支没有
    // 因为新增 AAC/H264 builder 而被遗漏。
    auto g711 = audio;
    g711.codec_type = CodecType::G711A;
    g711.rtp_clock_rate = 8000;
    g711.channels = 1;
    const auto g711_sdp = RtspSdpBuilder::Build(
        config, {g711}, H264ParameterSets{}, "127.0.0.1");
    assert(g711_sdp.find("a=rtpmap:97 PCMA/8000/1\r\n") != std::string::npos);
}

void TestAudioPacketizer() {
    const auto raw_aac = AudioRtpPacketizer::Packetize(
        MakeAudioAccessUnit(CodecType::AAC, {1, 2, 3}));
    assert(raw_aac.size() == 1);
    assert(raw_aac.front().timestamp == 1234);
    assert(raw_aac.front().marker);
    assert((raw_aac.front().payload ==
            std::vector<std::uint8_t>{0, 16, 0, 24, 1, 2, 3}));

    // MPEG-4 ADTS syncword，保护位为 1 时 header 是 7 字节，payload 应去除 header。
    const auto adts_aac = AudioRtpPacketizer::Packetize(
        MakeAudioAccessUnit(CodecType::AAC,
                            {0xff, 0xf1, 0x50, 0x80, 0x00, 0x1f, 0xfc, 9, 8}));
    assert(adts_aac.size() == 1);
    assert(adts_aac.front().payload.size() == 6);
    assert(adts_aac.front().payload[4] == 9);
    assert(adts_aac.front().payload[5] == 8);

    // protection_absent=0 表示 CRC 存在，因此合法 ADTS header 是 9 字节。
    // packetizer 只去除 header，不解析或改写 AAC access unit 本身。
    const auto crc_adts_aac = AudioRtpPacketizer::Packetize(
        MakeAudioAccessUnit(CodecType::AAC,
                            {0xff, 0xf0, 0x50, 0x80, 0x00, 0x1f, 0xfc, 0, 0,
                             10, 11}));
    assert(crc_adts_aac.size() == 1);
    assert(crc_adts_aac.front().payload.size() == 6);
    assert(crc_adts_aac.front().payload[4] == 10);
    assert(crc_adts_aac.front().payload[5] == 11);

    // 只有 8 字节的“9 字节 ADTS”属于截断输入，不能误删前 9 字节；
    // 兼容旧行为，将其按 raw AAC 交给 RFC 3640 AU header 封装。
    const auto truncated_adts = AudioRtpPacketizer::Packetize(
        MakeAudioAccessUnit(CodecType::AAC,
                            {0xff, 0xf0, 0x50, 0x80, 0x00, 0x1f, 0xfc, 0}));
    assert(truncated_adts.size() == 1);
    assert(truncated_adts.front().payload.size() == 12);
    assert(truncated_adts.front().payload[4] == 0xff);

    const auto g711 = AudioRtpPacketizer::Packetize(
        MakeAudioAccessUnit(CodecType::G711A, {7, 6, 5}));
    assert(g711.size() == 1);
    assert((g711.front().payload == std::vector<std::uint8_t>{7, 6, 5}));

    const auto g711u = AudioRtpPacketizer::Packetize(
        MakeAudioAccessUnit(CodecType::G711U, {4, 3, 2}));
    assert(g711u.size() == 1);
    assert((g711u.front().payload == std::vector<std::uint8_t>{4, 3, 2}));

    assert(AudioRtpPacketizer::Packetize(
                MakeAudioAccessUnit(CodecType::AAC, {}))
                .empty());

    assert(AudioRtpPacketizer::Packetize(
                MakeAudioAccessUnit(CodecType::H264, {1, 2}))
                .empty());
}

void WriteU16(std::vector<std::uint8_t>& bytes,
              std::size_t offset,
              std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 1] = static_cast<std::uint8_t>(value & 0xff);
}

void WriteU32(std::vector<std::uint8_t>& bytes,
              std::size_t offset,
              std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

void TestRtcpCodec() {
    const auto report = RtcpPacketCodec::BuildSenderReport(
        0x01020304, 0x11223344, 12, 3456);
    assert(report.size() == 28);
    assert(report[0] == 0x80);
    assert(report[1] == RtcpPacketCodec::kSenderReportPacketType);
    assert(report[2] == 0 && report[3] == 6);
    assert(report[4] == 1 && report[5] == 2 && report[6] == 3 && report[7] == 4);

    std::vector<std::uint8_t> receiver_report(32, 0);
    receiver_report[0] = 0x81; // V=2, RC=1。
    receiver_report[1] = RtcpPacketCodec::kReceiverReportPacketType;
    WriteU16(receiver_report, 2, 7);
    WriteU32(receiver_report, 4, 0xaabbccdd);
    WriteU32(receiver_report, 8, 0x01020304);
    receiver_report[12] = 7;
    receiver_report[13] = 0xff;
    receiver_report[14] = 0xff;
    receiver_report[15] = 0xfe; // signed 24-bit cumulative lost = -2。
    WriteU32(receiver_report, 16, 99);
    WriteU32(receiver_report, 20, 100);
    WriteU32(receiver_report, 24, 101);
    WriteU32(receiver_report, 28, 102);

    const auto parsed = RtcpPacketCodec::ParseCompound(
        receiver_report.data(), receiver_report.size());
    assert(parsed.valid);
    assert(parsed.packets.size() == 1);
    assert(parsed.packets.front().packet_type ==
           RtcpPacketCodec::kReceiverReportPacketType);
    RtcpReportBlock block;
    assert(RtcpPacketCodec::ReadReportBlock(
        parsed.packets.front().bytes.data() + 8, 24, block));
    assert(block.source_ssrc == 0x01020304);
    assert(block.fraction_lost == 7);
    assert(block.cumulative_lost == -2);
    assert(block.extended_highest_sequence == 99);

    // compound RTCP 允许多个 packet 首尾相接；解析器返回每个 packet 的
    // 独立字节副本，使上层可以按 packet type 分派而不越过边界。
    auto compound = report;
    compound.insert(compound.end(), receiver_report.begin(), receiver_report.end());
    const auto compound_result = RtcpPacketCodec::ParseCompound(
        compound.data(), compound.size());
    assert(compound_result.valid);
    assert(compound_result.packets.size() == 2);
    assert(compound_result.packets[0].packet_type ==
           RtcpPacketCodec::kSenderReportPacketType);
    assert(compound_result.packets[1].packet_type ==
           RtcpPacketCodec::kReceiverReportPacketType);

    // 末尾不足一个 RTCP header 的字节按旧协议路径记录为 trailing，不能
    // 被当作下一个 packet 读取，也不能触发越界访问。
    auto with_trailing = report;
    with_trailing.push_back(0xaa);
    const auto trailing = RtcpPacketCodec::ParseCompound(
        with_trailing.data(), with_trailing.size());
    assert(trailing.valid);
    assert(trailing.packets.size() == 1);
    assert(trailing.trailing_bytes == 1);

    // 错误 version 和不完整 report block 都必须在 codec 边界被拒绝，
    // session 层只负责记录日志和决定是否继续接收。
    auto invalid_version_packet = report;
    invalid_version_packet[0] = 0x40;
    const auto invalid_version = RtcpPacketCodec::ParseCompound(
        invalid_version_packet.data(), invalid_version_packet.size());
    assert(!invalid_version.valid);
    assert(invalid_version.invalid_version == 1);
    assert(!RtcpPacketCodec::ReadReportBlock(receiver_report.data() + 8, 23, block));

    auto truncated = receiver_report;
    truncated.resize(10);
    const auto invalid = RtcpPacketCodec::ParseCompound(
        truncated.data(), truncated.size());
    assert(!invalid.valid);
    assert(invalid.error_offset == 0);
}

} // namespace

int main() {
    TestResponseBuilder();
    TestSdpBuilder();
    TestAudioPacketizer();
    TestRtcpCodec();
    std::cout << "RTSP pure component tests passed\n";
    return 0;
}
