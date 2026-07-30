#include "media/protocol/avtp/avtp_packet_parser.h"
#include "media/puller/avtp_puller.h"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {

// 构造一个最小以太网 + AVTP/CVF/H.264 包。
// 该包只用于验证 parser 的字段解释，不依赖真实网卡或 Npcap。
std::vector<std::uint8_t> MakeEthernetCvfFrame(std::uint8_t subtype) {
    std::vector<std::uint8_t> frame = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xaa, 0x02, 0x54, 0x08, 0x69, 0xca,
        0x22, 0xf0,
        subtype,
        0x81,
        0x10,
        0x00,
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x01,
        0x12, 0x34, 0x56, 0x78,
        media::avtp::kCvfFormatRfc,
        media::avtp::kCvfFormatSubtypeH264,
        0x00,
        0x00,
        0x00,
        0x05,
        0x10,
        0x00,
        0x00, 0x00, 0x00, 0x01, 0x65,
    };
    return frame;
}

std::vector<std::uint8_t> MakeEthernetAafFrame() {
    // 这个 AAF header 取自现场 pcap 的设备画像：
    // format=0x04、NSR=0x2(16kHz)、channels=1、bit_depth=16、payload=320。
    std::vector<std::uint8_t> frame = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xaa, 0x02, 0x54, 0x08, 0x69, 0xca,
        0x22, 0xf0,
        media::avtp::kSubtypeAaf,
        0x81,
        0x6c,
        0x00,
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x01,
        0x4e, 0x33, 0xbf, 0xb6,
        0x04, 0x20, 0x01, 0x10,
        0x01, 0x40, 0x00, 0x00,
    };
    frame.resize(frame.size() + 320, 0xd5);
    return frame;
}

void TestParseStandardCvfH264() {
    // subtype=0x03 应按标准 CVF 视频解析，并能得到 H.264 format_subtype。
    const auto frame = MakeEthernetCvfFrame(media::avtp::kSubtypeCvf);
    media::avtp::ParsedCvfPacket packet;
    media::avtp::ParseError error = media::avtp::ParseError::None;

    const bool ok = media::avtp::AvtpPacketParser::Parse(
        frame.data(), frame.size(), packet, &error);
    assert(ok);
    assert(error == media::avtp::ParseError::None);
    assert(packet.has_ethernet_header);
    assert(packet.subtype == media::avtp::kSubtypeCvf);
    assert(packet.stream_id == 0xaabbccddeeff0001ULL);
    assert(packet.format == media::avtp::kCvfFormatRfc);
    assert(packet.format_subtype == media::avtp::kCvfFormatSubtypeH264);
    assert(packet.marker);
    assert(packet.media_payload_size == 5);
    assert(media::avtp::StartsWithAnnexBStartCode(packet.media_payload,
                                                  packet.media_payload_size));
}

void TestAafIsNotParsedAsCvf() {
    // subtype=0x02 是 AAF 音频。这个测试防止以后又把 AAF 误当 CVF 视频。
    const auto frame = MakeEthernetCvfFrame(media::avtp::kSubtypeAaf);
    media::avtp::ParsedCvfPacket packet;
    media::avtp::ParseError error = media::avtp::ParseError::None;

    const bool ok = media::avtp::AvtpPacketParser::Parse(
        frame.data(), frame.size(), packet, &error);
    assert(!ok);
    assert(error == media::avtp::ParseError::UnsupportedSubtype);
}

void TestParseAafAudio() {
    const auto frame = MakeEthernetAafFrame();
    media::avtp::ParsedAafPacket packet;
    media::avtp::ParseError error = media::avtp::ParseError::None;

    const bool ok = media::avtp::AvtpPacketParser::ParseAaf(
        frame.data(), frame.size(), packet, &error);
    assert(ok);
    assert(error == media::avtp::ParseError::None);
    assert(packet.has_ethernet_header);
    assert(packet.subtype == media::avtp::kSubtypeAaf);
    assert(packet.stream_id == 0xaabbccddeeff0001ULL);
    assert(packet.format == 0x04);
    assert(packet.nominal_sample_rate_code == 0x02);
    assert(packet.sample_rate == 16000);
    assert(packet.channels_per_frame == 1);
    assert(packet.bit_depth == 16);
    assert(packet.payload_size == 320);
    assert(packet.payload && packet.payload[0] == 0xd5);
}

void TestParseUrlToConfig() {
    // URL 入口必须和结构化 Config 保持一致，后续配置系统可以直接复用。
    AvtpPuller::Config config;
    std::string error;
    const bool ok = AvtpPuller::ParseUrl(
        "avtp://default?src=aa:02:54:08:69:ca&stream=0xaabbccddeeff0001"
        "&format=h265&width=1920&height=1080&fps=25&audio=g711u"
        "&audio_rate=16000&audio_channels=1&probe_timeout=500",
        config,
        &error);

    assert(ok);
    assert(error.empty());
    assert(config.device == "default");
    assert(config.source_mac.has_value());
    assert(config.stream_id == 0xaabbccddeeff0001ULL);
    assert(config.format == AvtpPuller::PayloadFormat::H265);
    assert(config.width == 1920);
    assert(config.height == 1080);
    assert(config.fps == 25.0F);
    assert(config.enable_audio);
    assert(config.audio_codec == CodecType::G711U);
    assert(config.audio_sample_rate == 16000);
    assert(config.audio_channels == 1);
    assert(config.probe_timeout_ms == 500);
}

} // namespace

int main() {
    TestParseStandardCvfH264();
    TestAafIsNotParsedAsCvf();
    TestParseAafAudio();
    TestParseUrlToConfig();
    return 0;
}
