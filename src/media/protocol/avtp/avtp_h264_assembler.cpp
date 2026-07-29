#include "media/protocol/avtp/avtp_h264_assembler.h"

#include <algorithm>

namespace media::avtp {
namespace {

constexpr std::uint8_t kNalTypeMask = 0x1F;
constexpr std::uint8_t kNalTypeIdr = 5;
constexpr std::uint8_t kNalTypeSps = 7;

std::size_t FindStartCode(const std::vector<std::uint8_t>& data,
                          std::size_t offset,
                          std::size_t& start_code_size) {
    // 兼容 3 字节和 4 字节 Annex-B start code。
    for (std::size_t i = offset; i + 3 <= data.size(); ++i) {
        if (i + 4 <= data.size() && data[i] == 0 && data[i + 1] == 0 &&
            data[i + 2] == 0 && data[i + 3] == 1) {
            start_code_size = 4;
            return i;
        }
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            start_code_size = 3;
            return i;
        }
    }
    start_code_size = 0;
    return data.size();
}

} // namespace

AvtpH264Assembler::AvtpH264Assembler(std::size_t max_access_unit_size)
    : max_access_unit_size_(max_access_unit_size) {
}

void AvtpH264Assembler::Reset() {
    stats_ = {};
    current_.clear();
    has_stream_ = false;
    stream_id_ = 0;
    source_mac_ = {};
    expected_sequence_ = 0;
    have_expected_sequence_ = false;
    dropping_until_marker_ = false;
    first_capture_timestamp_us_ = 0;
    first_avtp_timestamp_ = 0;
}

void AvtpH264Assembler::ResetAccessUnit() {
    current_.clear();
    first_capture_timestamp_us_ = 0;
    first_avtp_timestamp_ = 0;
}

bool AvtpH264Assembler::SameStream(const ParsedCvfPacket& packet) const {
    if (!has_stream_ || packet.stream_id != stream_id_) {
        return false;
    }
    if (packet.has_ethernet_header &&
        !IsSameMac(packet.source_mac, source_mac_)) {
        return false;
    }
    return true;
}

void AvtpH264Assembler::SwitchStream(const ParsedCvfPacket& packet) {
    // 不同 stream/source 共用一个 assembler 会污染 sequence 状态。
    // 发现切流时立即重置，后续生产环境还应优先配置 stream/source filter。
    has_stream_ = true;
    stream_id_ = packet.stream_id;
    source_mac_ = packet.has_ethernet_header ? packet.source_mac : ZeroMac();
    expected_sequence_ = static_cast<std::uint8_t>(packet.sequence_num + 1U);
    have_expected_sequence_ = true;
    dropping_until_marker_ = false;
    ResetAccessUnit();
}

bool AvtpH264Assembler::Append(const std::uint8_t* data, std::size_t size) {
    if (!data || size == 0) {
        return false;
    }
    if (current_.size() + size > max_access_unit_size_) {
        ++stats_.malformed_packets;
        return false;
    }
    current_.insert(current_.end(), data, data + size);
    return true;
}

AvtpH264Assembler::Result AvtpH264Assembler::DropCurrentAccessUnit(
    bool wait_until_marker) {
    // sequence gap 之后继续拼接会制造损坏帧。进入 dropping_until_marker
    // 后，直到当前 access unit 的 marker 到来才恢复接收下一帧。
    if (!current_.empty()) {
        ++stats_.dropped_access_units;
    }
    ResetAccessUnit();
    dropping_until_marker_ = wait_until_marker;
    return Result::Dropped;
}

AvtpH264Assembler::Result AvtpH264Assembler::FinishAccessUnit(
    H264AccessUnit& output) {
    if (current_.empty()) {
        ResetAccessUnit();
        return Result::Dropped;
    }

    H264AccessUnit ready;
    ready.data = std::move(current_);
    ready.capture_timestamp_us = first_capture_timestamp_us_;
    ready.avtp_timestamp = first_avtp_timestamp_;
    // H.264 中 IDR 或携带 SPS 的 access unit 都可作为较安全的关键帧起点。
    ready.keyframe = ContainsH264NalType(ready.data, kNalTypeIdr) ||
                     ContainsH264NalType(ready.data, kNalTypeSps);
    output = std::move(ready);

    ++stats_.access_units;
    ResetAccessUnit();
    return Result::AccessUnitReady;
}

AvtpH264Assembler::Result AvtpH264Assembler::Push(
    const ParsedCvfPacket& packet,
    std::int64_t capture_timestamp_us,
    H264AccessUnit& output) {
    ++stats_.packets;
    stats_.payload_bytes += packet.media_payload_size;

    if (!packet.media_payload || packet.media_payload_size == 0) {
        ++stats_.malformed_packets;
        return Result::NeedMore;
    }

    if (!SameStream(packet)) {
        SwitchStream(packet);
    } else if (have_expected_sequence_ &&
               packet.sequence_num != expected_sequence_) {
        // AVTP sequence_num 是 8 位，自然溢出即可。只要不等于预期值，
        // 就认为当前 access unit 缺片，不能继续输出。
        ++stats_.lost_packets;
        expected_sequence_ = static_cast<std::uint8_t>(packet.sequence_num + 1U);
        return DropCurrentAccessUnit(!packet.marker);
    } else {
        expected_sequence_ = static_cast<std::uint8_t>(packet.sequence_num + 1U);
        have_expected_sequence_ = true;
    }

    if (dropping_until_marker_) {
        if (packet.marker) {
            dropping_until_marker_ = false;
        }
        return Result::Dropped;
    }

    if (current_.empty()) {
        // access unit 的时间戳取第一片的时间。当前优先使用 capture time，
        // 保留 AVTP timestamp 给后续 gPTP 映射使用。
        first_capture_timestamp_us_ = capture_timestamp_us;
        first_avtp_timestamp_ = packet.avtp_timestamp;
    }

    if (!Append(packet.media_payload, packet.media_payload_size)) {
        return DropCurrentAccessUnit(!packet.marker);
    }

    if (!packet.marker) {
        return Result::NeedMore;
    }
    return FinishAccessUnit(output);
}

bool ContainsH264NalType(const std::vector<std::uint8_t>& data,
                         std::uint8_t nal_type) {
    // 逐个 Annex-B NAL 扫描。这里只做轻量判断，不解析完整 SPS/PPS。
    std::size_t offset = 0;
    while (offset < data.size()) {
        std::size_t start_code_size = 0;
        const std::size_t start = FindStartCode(data, offset, start_code_size);
        if (start == data.size()) {
            return false;
        }
        const std::size_t nal_start = start + start_code_size;
        if (nal_start < data.size() &&
            (data[nal_start] & kNalTypeMask) == nal_type) {
            return true;
        }
        offset = nal_start + 1;
    }
    return false;
}

} // namespace media::avtp
