#include "media/protocol/audio_rtp_packetizer.h"

#include <algorithm>

namespace {

constexpr std::size_t kAacAuHeaderSize = 4;

void WriteU16(std::uint8_t* data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[1] = static_cast<std::uint8_t>(value & 0xFF);
}

std::size_t AdtsHeaderSize(const std::uint8_t* data, std::size_t size) {
    // 只检查 syncword、layer/version 无关的保护位和最小长度；旧路径不
    // 解析 frame_length，避免把合法但由上游裁剪的 access unit 静默截断。
    if (size < 7 || data[0] != 0xFF || (data[1] & 0xF0) != 0xF0) {
        return 0;
    }

    const bool protection_absent = (data[1] & 0x01) != 0;
    // protection_absent=0 时 CRC 占用额外两个字节，ADTS header 为 9 字节。
    const auto header_size = protection_absent ? std::size_t{7} : std::size_t{9};
    return size >= header_size ? header_size : 0;
}

std::vector<RtpPayload> PacketizeAac(const EncodedAccessUnit& access_unit) {
    if (!access_unit.encoded_data || !access_unit.encoded_data->Data() ||
        access_unit.encoded_data->Size() == 0) {
        return {};
    }

    const auto* data = access_unit.encoded_data->Data();
    auto size = access_unit.encoded_data->Size();
    // RTSP/RTP 发送的是 AAC access unit；如果上游给的是 ADTS 帧，需要先剥掉 ADTS 头。
    const auto adts_header_size = AdtsHeaderSize(data, size);
    if (adts_header_size > 0) {
        data += adts_header_size;
        size -= adts_header_size;
    }
    if (size == 0) {
        return {};
    }

    RtpPayload payload;
    payload.timestamp = static_cast<std::uint32_t>(access_unit.pts);
    payload.marker = true;
    payload.payload.resize(kAacAuHeaderSize + size);

    // RFC 3640 AAC-hbr：AU-headers-length=16bit，单个 AU-header 使用 13bit size + 3bit index。
    WriteU16(payload.payload.data(), 16);
    const auto au_header = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(size) & 0x1FFFU) << 3U);
    WriteU16(payload.payload.data() + 2, au_header);
    std::copy(data, data + size, payload.payload.begin() + kAacAuHeaderSize);
    return {std::move(payload)};
}

std::vector<RtpPayload> PacketizeRawAudio(const EncodedAccessUnit& access_unit) {
    if (!access_unit.encoded_data || !access_unit.encoded_data->Data() ||
        access_unit.encoded_data->Size() == 0) {
        return {};
    }

    // G711 RTP payload 不带 AU header，编码后的每个字节直接作为 RTP payload。
    RtpPayload payload;
    payload.timestamp = static_cast<std::uint32_t>(access_unit.pts);
    payload.marker = true;
    const auto* data = access_unit.encoded_data->Data();
    const auto size = access_unit.encoded_data->Size();
    payload.payload.assign(data, data + size);
    return {std::move(payload)};
}

} // namespace

std::vector<RtpPayload> AudioRtpPacketizer::Packetize(
    const EncodedAccessUnit& access_unit) {
    if (access_unit.codec_type == CodecType::AAC) {
        return PacketizeAac(access_unit);
    }
    if (access_unit.codec_type == CodecType::G711A ||
        access_unit.codec_type == CodecType::G711U) {
        return PacketizeRawAudio(access_unit);
    }
    return {};
}
