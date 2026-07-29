#include "media/protocol/avtp/avtp_payload_assembler.h"

namespace media::avtp {

AvtpPayloadAssembler::AvtpPayloadAssembler(std::size_t max_access_unit_size)
    : max_access_unit_size_(max_access_unit_size) {
}

void AvtpPayloadAssembler::Reset() {
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

void AvtpPayloadAssembler::ResetAccessUnit() {
    current_.clear();
    first_capture_timestamp_us_ = 0;
    first_avtp_timestamp_ = 0;
}

bool AvtpPayloadAssembler::SameStream(const ParsedCvfPacket& packet) const {
    if (!has_stream_ || packet.stream_id != stream_id_) {
        return false;
    }
    if (packet.has_ethernet_header &&
        !IsSameMac(packet.source_mac, source_mac_)) {
        return false;
    }
    return true;
}

void AvtpPayloadAssembler::SwitchStream(const ParsedCvfPacket& packet) {
    // 同一个 assembler 只能维护一路 stream 的 sequence 状态。
    // 如果没有上游过滤，多路流混进来时这里会主动切换并丢弃旧缓存。
    has_stream_ = true;
    stream_id_ = packet.stream_id;
    source_mac_ = packet.has_ethernet_header ? packet.source_mac : ZeroMac();
    expected_sequence_ = static_cast<std::uint8_t>(packet.sequence_num + 1U);
    have_expected_sequence_ = true;
    dropping_until_marker_ = false;
    ResetAccessUnit();
}

bool AvtpPayloadAssembler::Append(const std::uint8_t* data, std::size_t size) {
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

AvtpPayloadAssembler::Result AvtpPayloadAssembler::DropCurrentAccessUnit(
    bool wait_until_marker) {
    // 对 H.265/JPEG 也必须严格丢弃缺片 access unit。否则 FFmpeg 可能收到
    // 半帧数据，轻则花屏，重则解码器状态被污染。
    if (!current_.empty()) {
        ++stats_.dropped_access_units;
    }
    ResetAccessUnit();
    dropping_until_marker_ = wait_until_marker;
    return Result::Dropped;
}

AvtpPayloadAssembler::Result AvtpPayloadAssembler::FinishAccessUnit(
    AvtpAccessUnit& output) {
    if (current_.empty()) {
        ResetAccessUnit();
        return Result::Dropped;
    }

    AvtpAccessUnit ready;
    ready.data = std::move(current_);
    ready.capture_timestamp_us = first_capture_timestamp_us_;
    ready.avtp_timestamp = first_avtp_timestamp_;
    output = std::move(ready);

    ++stats_.access_units;
    ResetAccessUnit();
    return Result::AccessUnitReady;
}

AvtpPayloadAssembler::Result AvtpPayloadAssembler::Push(
    const ParsedCvfPacket& packet,
    const std::uint8_t* payload,
    std::size_t payload_size,
    std::int64_t capture_timestamp_us,
    AvtpAccessUnit& output) {
    ++stats_.packets;
    stats_.payload_bytes += payload_size;

    if (!payload || payload_size == 0) {
        ++stats_.malformed_packets;
        return Result::NeedMore;
    }

    if (!SameStream(packet)) {
        SwitchStream(packet);
    } else if (have_expected_sequence_ &&
               packet.sequence_num != expected_sequence_) {
        // 8 位 sequence 允许自然回绕；只要当前值不是预期值，就说明缺片。
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
        // access unit 时间戳取第一片。后续若接入 AVTP/gPTP 时间轴，
        // 可以用 first_avtp_timestamp_ 做更准确映射。
        first_capture_timestamp_us_ = capture_timestamp_us;
        first_avtp_timestamp_ = packet.avtp_timestamp;
    }

    if (!Append(payload, payload_size)) {
        return DropCurrentAccessUnit(!packet.marker);
    }

    if (!packet.marker) {
        return Result::NeedMore;
    }
    return FinishAccessUnit(output);
}

} // namespace media::avtp
