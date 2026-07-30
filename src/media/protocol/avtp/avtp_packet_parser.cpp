#include "media/protocol/avtp/avtp_packet_parser.h"

#include <algorithm>

namespace media::avtp {
namespace {

// AVTP 可以直接跟在以太网头后，也可能被 VLAN tag 包起来。
// 当前 parser 只需要定位真实 EtherType 和 AVTP header 起始偏移。
constexpr std::uint16_t kEtherTypeVlan = 0x8100;
constexpr std::uint16_t kEtherTypeProviderBridge = 0x88A8;
constexpr std::uint16_t kEtherTypeQinQ = 0x9100;
constexpr std::size_t kEthernetHeaderSize = 14;
constexpr std::size_t kMaxVlanTags = 2;

std::uint16_t ReadBe16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(data[0]) << 8) |
        static_cast<std::uint16_t>(data[1]));
}

std::uint32_t ReadBe32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

std::uint64_t ReadBe64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(data[i]);
    }
    return value;
}

bool IsVlanEtherType(std::uint16_t ether_type) {
    return ether_type == kEtherTypeVlan ||
           ether_type == kEtherTypeProviderBridge ||
           ether_type == kEtherTypeQinQ;
}

int AafSampleRateFromCode(std::uint8_t code) {
    // AAF nominal sample rate 代码。当前现场包里 code=2，即 16 kHz。
    switch (code) {
        case 1: return 8000;
        case 2: return 16000;
        case 3: return 32000;
        case 4: return 44100;
        case 5: return 48000;
        case 6: return 88200;
        case 7: return 96000;
        case 8: return 176400;
        case 9: return 192000;
        case 10: return 24000;
        default: return 0;
    }
}

void SetError(ParseError* error, ParseError value) {
    if (error) {
        *error = value;
    }
}

} // namespace

bool IsSameMac(const MacAddress& lhs, const MacAddress& rhs) {
    return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

MacAddress ZeroMac() {
    return MacAddress{};
}

bool StartsWithAnnexBStartCode(const std::uint8_t* data, std::size_t size) {
    return data &&
           ((size >= 4 && data[0] == 0 && data[1] == 0 &&
             data[2] == 0 && data[3] == 1) ||
            (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1));
}

bool AvtpPacketParser::Parse(const std::uint8_t* data,
                             std::size_t size,
                             ParsedCvfPacket& packet,
                             ParseError* error) {
    packet = {};
    SetError(error, ParseError::None);

    if (!data) {
        SetError(error, ParseError::TooShort);
        return false;
    }

    // 先尝试按完整以太网帧解析；失败后再允许测试或上游组件直接传入
    // 已经剥离以太网头的 CVF PDU。
    std::size_t avtp_offset = 0;
    ParsedCvfPacket ethernet_packet;
    ParseError ethernet_error = ParseError::None;
    if (ParseEthernetPrefix(data, size, ethernet_packet, avtp_offset,
                            &ethernet_error)) {
        packet = ethernet_packet;
        return ParseCvfPdu(data + avtp_offset,
                           size - avtp_offset,
                           packet,
                           error);
    }

    if (size >= 1 && data[0] == kSubtypeCvf) {
        return ParseCvfPdu(data, size, packet, error);
    }

    SetError(error, ethernet_error);
    return false;
}

bool AvtpPacketParser::ParseEthernetPrefix(const std::uint8_t* data,
                                           std::size_t size,
                                           ParsedCvfPacket& packet,
                                           std::size_t& avtp_offset,
                                           ParseError* error) {
    packet = {};
    avtp_offset = 0;

    if (!data || size < kEthernetHeaderSize) {
        SetError(error, ParseError::TooShort);
        return false;
    }

    std::copy(data, data + 6, packet.destination_mac.begin());
    std::copy(data + 6, data + 12, packet.source_mac.begin());

    std::uint16_t ether_type = ReadBe16(data + 12);
    std::size_t offset = kEthernetHeaderSize;
    // 兼容单层 VLAN 和 QinQ。超过两层的场景暂不支持，避免错误越界。
    for (std::size_t tag = 0; tag < kMaxVlanTags && IsVlanEtherType(ether_type);
         ++tag) {
        if (size < offset + 4) {
            SetError(error, ParseError::TooShort);
            return false;
        }
        ether_type = ReadBe16(data + offset + 2);
        offset += 4;
    }

    if (ether_type != kEtherTypeAvtp) {
        SetError(error, ParseError::UnsupportedEtherType);
        return false;
    }

    packet.has_ethernet_header = true;
    packet.ether_type = ether_type;
    packet.avtp_offset = offset;
    avtp_offset = offset;
    SetError(error, ParseError::None);
    return true;
}

bool AvtpPacketParser::ParseEthernetPrefix(const std::uint8_t* data,
                                           std::size_t size,
                                           ParsedAafPacket& packet,
                                           std::size_t& avtp_offset,
                                           ParseError* error) {
    packet = {};
    avtp_offset = 0;

    if (!data || size < kEthernetHeaderSize) {
        SetError(error, ParseError::TooShort);
        return false;
    }

    std::copy(data, data + 6, packet.destination_mac.begin());
    std::copy(data + 6, data + 12, packet.source_mac.begin());

    std::uint16_t ether_type = ReadBe16(data + 12);
    std::size_t offset = kEthernetHeaderSize;
    for (std::size_t tag = 0; tag < kMaxVlanTags && IsVlanEtherType(ether_type);
         ++tag) {
        if (size < offset + 4) {
            SetError(error, ParseError::TooShort);
            return false;
        }
        ether_type = ReadBe16(data + offset + 2);
        offset += 4;
    }

    if (ether_type != kEtherTypeAvtp) {
        SetError(error, ParseError::UnsupportedEtherType);
        return false;
    }

    packet.has_ethernet_header = true;
    packet.ether_type = ether_type;
    packet.avtp_offset = offset;
    avtp_offset = offset;
    SetError(error, ParseError::None);
    return true;
}

bool AvtpPacketParser::ParseCvfPdu(const std::uint8_t* data,
                                   std::size_t size,
                                   ParsedCvfPacket& packet,
                                   ParseError* error) {
    const bool had_ethernet = packet.has_ethernet_header;
    const MacAddress destination = packet.destination_mac;
    const MacAddress source = packet.source_mac;
    const std::uint16_t ether_type = packet.ether_type;
    const std::size_t avtp_offset = packet.avtp_offset;

    ParsedCvfPacket parsed;
    parsed.has_ethernet_header = had_ethernet;
    parsed.destination_mac = destination;
    parsed.source_mac = source;
    parsed.ether_type = ether_type;
    parsed.avtp_offset = avtp_offset;

    if (!data || size < kCvfHeaderSize) {
        SetError(error, ParseError::TooShort);
        return false;
    }

    parsed.subtype = data[0];
    // 标准 AVTP 中 subtype=0x03 才是 CVF 视频。subtype=0x02 是 AAF 音频，
    // 不能按视频包解释，否则会把音频/填充数据误当视频配置。
    if (parsed.subtype != kSubtypeCvf) {
        SetError(error, ParseError::UnsupportedSubtype);
        return false;
    }

    // CVF header 前 16 字节包含 stream id、sequence、timestamp 等
    // AVTP 通用时序字段；后 8 字节才是 CVF 格式和 payload 信息。
    parsed.stream_id_valid = (data[1] & 0x80U) != 0;
    parsed.version = static_cast<std::uint8_t>((data[1] & 0x70U) >> 4);
    parsed.media_clock_restart = (data[1] & 0x08U) != 0;
    parsed.timestamp_valid = (data[1] & 0x01U) != 0;
    if (parsed.version != 0) {
        SetError(error, ParseError::UnsupportedVersion);
        return false;
    }

    parsed.sequence_num = data[2];
    parsed.timestamp_uncertain = (data[3] & 0x01U) != 0;
    parsed.stream_id = ReadBe64(data + 4);
    parsed.avtp_timestamp = ReadBe32(data + 12);
    parsed.format = data[16];
    parsed.format_subtype = data[17];
    if (parsed.format != kCvfFormatRfc) {
        SetError(error, ParseError::UnsupportedFormat);
        return false;
    }

    parsed.stream_data_length = ReadBe16(data + 20);
    parsed.payload_timestamp_valid = (data[22] & 0x20U) != 0;
    parsed.marker = (data[22] & 0x10U) != 0;
    parsed.event = static_cast<std::uint8_t>(data[22] & 0x0FU);

    if (static_cast<std::size_t>(parsed.stream_data_length) >
        size - kCvfHeaderSize) {
        SetError(error, ParseError::StreamDataLengthTooLarge);
        return false;
    }

    parsed.payload = data + kCvfHeaderSize;
    parsed.payload_size = parsed.stream_data_length;
    parsed.media_payload = parsed.payload;
    parsed.media_payload_size = parsed.payload_size;

    // 标准 CVF payload 可直接交给组帧器。仅当检测到已知厂商 magic 时，
    // 才剥离私有头和可选 RTP-like 头，统一输出 media_payload。
    if (parsed.payload_size >= kCustomPayloadHeaderSize) {
        const std::uint32_t magic = ReadBe32(parsed.payload + 4);
        parsed.has_custom_payload_header =
            magic == kCustomPayloadMagic || magic == kCustomPayloadMagicAlt;
    }

    if (parsed.has_custom_payload_header) {
        parsed.custom_payload_length = ReadBe32(parsed.payload);
        parsed.custom_magic = ReadBe32(parsed.payload + 4);
        parsed.media_payload = parsed.payload + kCustomPayloadHeaderSize;
        parsed.media_payload_size = parsed.payload_size - kCustomPayloadHeaderSize;

        if (parsed.payload_size >= kCustomPayloadHeaderSize + kRtpHeaderSize) {
            const std::uint8_t* rtp_header =
                parsed.payload + kCustomPayloadHeaderSize;
            parsed.custom_rtp_timestamp = ReadBe32(rtp_header + 4);
            parsed.custom_ssrc = ReadBe32(rtp_header + 8);
            parsed.media_payload =
                parsed.payload + kCustomPayloadHeaderSize + kRtpHeaderSize;
            parsed.media_payload_size =
                parsed.payload_size - kCustomPayloadHeaderSize - kRtpHeaderSize;
        }
    }

    packet = parsed;
    SetError(error, ParseError::None);
    return true;
}

bool AvtpPacketParser::ParseAaf(const std::uint8_t* data,
                                std::size_t size,
                                ParsedAafPacket& packet,
                                ParseError* error) {
    packet = {};
    SetError(error, ParseError::None);

    if (!data) {
        SetError(error, ParseError::TooShort);
        return false;
    }

    std::size_t avtp_offset = 0;
    ParsedAafPacket ethernet_packet;
    ParseError ethernet_error = ParseError::None;
    if (ParseEthernetPrefix(data, size, ethernet_packet, avtp_offset,
                            &ethernet_error)) {
        packet = ethernet_packet;
        return ParseAafPdu(data + avtp_offset,
                           size - avtp_offset,
                           packet,
                           error);
    }

    if (size >= 1 && data[0] == kSubtypeAaf) {
        return ParseAafPdu(data, size, packet, error);
    }

    SetError(error, ethernet_error);
    return false;
}

bool AvtpPacketParser::ParseAafPdu(const std::uint8_t* data,
                                   std::size_t size,
                                   ParsedAafPacket& packet,
                                   ParseError* error) {
    const bool had_ethernet = packet.has_ethernet_header;
    const MacAddress destination = packet.destination_mac;
    const MacAddress source = packet.source_mac;
    const std::uint16_t ether_type = packet.ether_type;
    const std::size_t avtp_offset = packet.avtp_offset;

    ParsedAafPacket parsed;
    parsed.has_ethernet_header = had_ethernet;
    parsed.destination_mac = destination;
    parsed.source_mac = source;
    parsed.ether_type = ether_type;
    parsed.avtp_offset = avtp_offset;

    if (!data || size < kAafHeaderSize) {
        SetError(error, ParseError::TooShort);
        return false;
    }

    parsed.subtype = data[0];
    if (parsed.subtype != kSubtypeAaf) {
        SetError(error, ParseError::UnsupportedSubtype);
        return false;
    }

    parsed.stream_id_valid = (data[1] & 0x80U) != 0;
    parsed.version = static_cast<std::uint8_t>((data[1] & 0x70U) >> 4);
    parsed.media_clock_restart = (data[1] & 0x08U) != 0;
    parsed.timestamp_valid = (data[1] & 0x01U) != 0;
    if (parsed.version != 0) {
        SetError(error, ParseError::UnsupportedVersion);
        return false;
    }

    parsed.sequence_num = data[2];
    parsed.timestamp_uncertain = (data[3] & 0x01U) != 0;
    parsed.stream_id = ReadBe64(data + 4);
    parsed.avtp_timestamp = ReadBe32(data + 12);

    // AAF format_info:
    // byte16=format, byte17 高 4 bit=nominal sample rate,
    // byte18..19 低 10 bit≈channels_per_frame/bit depth 组合。
    // 现场包表现为 04 20 01 10：format=4、NSR=2(16k)、channels=1、bit_depth=16。
    parsed.format = data[16];
    parsed.nominal_sample_rate_code = static_cast<std::uint8_t>((data[17] & 0xF0U) >> 4);
    parsed.sample_rate = AafSampleRateFromCode(parsed.nominal_sample_rate_code);
    parsed.channels_per_frame = data[18];
    parsed.bit_depth = data[19];

    parsed.stream_data_length = ReadBe16(data + 20);
    parsed.sparse_timestamp = (data[22] & 0x10U) != 0;
    parsed.event = static_cast<std::uint8_t>(data[22] & 0x0FU);

    if (static_cast<std::size_t>(parsed.stream_data_length) >
        size - kAafHeaderSize) {
        SetError(error, ParseError::StreamDataLengthTooLarge);
        return false;
    }

    parsed.payload = data + kAafHeaderSize;
    parsed.payload_size = parsed.stream_data_length;

    packet = parsed;
    SetError(error, ParseError::None);
    return true;
}

const char* AvtpPacketParser::ErrorToString(ParseError error) {
    switch (error) {
        case ParseError::None:
            return "none";
        case ParseError::TooShort:
            return "packet too short";
        case ParseError::UnsupportedEtherType:
            return "unsupported EtherType";
        case ParseError::UnsupportedSubtype:
            return "unsupported AVTP subtype";
        case ParseError::UnsupportedVersion:
            return "unsupported AVTP version";
        case ParseError::UnsupportedFormat:
            return "unsupported CVF format";
        case ParseError::StreamDataLengthTooLarge:
            return "stream data length exceeds packet size";
    }
    return "unknown parse error";
}

} // namespace media::avtp
