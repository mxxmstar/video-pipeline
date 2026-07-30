#include "media/protocol/avtp/avtp_timestamp_mapper.h"

#include <algorithm>
#include <cstdlib>

namespace media::avtp {
namespace {

// 32 位 AVTP timestamp 的一个完整周期，单位为纳秒。
constexpr std::int64_t kTimestampModulo = (std::int64_t{1} << 32);

std::int64_t AbsDifference(std::int64_t lhs, std::int64_t rhs) {
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

// C++ 整数除法向 0 截断。这里补成真正的向下取整，保证负预测值也能正确选周期。
std::int64_t FloorDiv(std::int64_t value, std::int64_t divisor) {
    std::int64_t quotient = value / divisor;
    const std::int64_t remainder = value % divisor;
    if (remainder != 0 && value < 0) {
        --quotient;
    }
    return quotient;
}

} // namespace

AvtpTimestampMapper::AvtpTimestampMapper() = default;

AvtpTimestampMapper::AvtpTimestampMapper(Config config)
    : config_(config) {
    config_.discontinuity_threshold_us =
        std::max<std::int64_t>(0, config_.discontinuity_threshold_us);
}

void AvtpTimestampMapper::Reset() {
    stats_ = {};
    initialized_ = false;
    anchor_avtp_ns_ = 0;
    anchor_capture_us_ = 0;
    last_extended_avtp_ns_ = 0;
    media_clock_restart_bits_.clear();
}

void AvtpTimestampMapper::SetAnchor(std::uint32_t avtp_timestamp,
                                    std::int64_t capture_timestamp_us) {
    // 第一个锚点不需要知道完整 gPTP epoch。后续只使用 AVTP 相对增量，输出仍放在
    // capture 微秒域中，因此能直接兼容工程当前的 time_base={1, 1000000}。
    anchor_avtp_ns_ = static_cast<std::int64_t>(avtp_timestamp);
    anchor_capture_us_ = capture_timestamp_us;
    last_extended_avtp_ns_ = anchor_avtp_ns_;
    initialized_ = true;
}

std::int64_t AvtpTimestampMapper::ExpandNearest(
    std::uint32_t avtp_timestamp,
    std::int64_t predicted_ns) const {
    const std::int64_t cycle = FloorDiv(predicted_ns, kTimestampModulo);
    std::int64_t candidate =
        cycle * kTimestampModulo + static_cast<std::int64_t>(avtp_timestamp);

    // predicted_ns 可能正好位于周期边界附近，因此同时比较前后相邻周期。
    const std::int64_t previous = candidate - kTimestampModulo;
    const std::int64_t next = candidate + kTimestampModulo;
    if (AbsDifference(previous, predicted_ns) <
        AbsDifference(candidate, predicted_ns)) {
        candidate = previous;
    }
    if (AbsDifference(next, predicted_ns) <
        AbsDifference(candidate, predicted_ns)) {
        candidate = next;
    }
    return candidate;
}

std::int64_t AvtpTimestampMapper::Map(
    std::uint64_t stream_id,
    std::uint32_t avtp_timestamp,
    std::int64_t capture_timestamp_us,
    bool timestamp_valid,
    bool timestamp_uncertain,
    bool media_clock_restart) {
    if (!timestamp_valid) {
        ++stats_.invalid_fallbacks;
        return capture_timestamp_us;
    }
    if (timestamp_uncertain) {
        ++stats_.uncertain_fallbacks;
        return capture_timestamp_us;
    }

    bool restart_event = false;
    const auto [restart_it, inserted] =
        media_clock_restart_bits_.emplace(stream_id, media_clock_restart);
    if (!inserted && restart_it->second != media_clock_restart) {
        restart_event = true;
        restart_it->second = media_clock_restart;
    }

    if (!initialized_ || restart_event) {
        if (initialized_ && restart_event) {
            ++stats_.media_clock_restarts;
        }
        SetAnchor(avtp_timestamp, capture_timestamp_us);
        ++stats_.mapped_timestamps;
        return capture_timestamp_us;
    }

    // 抓包时间只用于推测当前落在哪个 32 位周期；最终 PTS 的帧间间隔来自 AVTP。
    const std::int64_t capture_delta_us =
        capture_timestamp_us - anchor_capture_us_;
    const std::int64_t predicted_avtp_ns =
        anchor_avtp_ns_ + capture_delta_us * 1000;
    const std::int64_t extended_avtp_ns =
        ExpandNearest(avtp_timestamp, predicted_avtp_ns);
    const std::int64_t mapped_timestamp_us =
        anchor_capture_us_ + (extended_avtp_ns - anchor_avtp_ns_) / 1000;

    // 正常抓包抖动通常远小于阈值。偏差过大说明 MCR 位可能丢失、发送端时间跳变，
    // 或 capture clock 本身发生了校时；此时从当前包重新锚定比输出跳跃 PTS 更安全。
    if (config_.discontinuity_threshold_us > 0 &&
        AbsDifference(mapped_timestamp_us, capture_timestamp_us) >
            config_.discontinuity_threshold_us) {
        ++stats_.discontinuity_resets;
        SetAnchor(avtp_timestamp, capture_timestamp_us);
        ++stats_.mapped_timestamps;
        return capture_timestamp_us;
    }

    const std::int64_t previous_cycle =
        FloorDiv(last_extended_avtp_ns_, kTimestampModulo);
    const std::int64_t current_cycle =
        FloorDiv(extended_avtp_ns, kTimestampModulo);
    if (extended_avtp_ns >= last_extended_avtp_ns_ &&
        current_cycle > previous_cycle) {
        stats_.forward_wraps +=
            static_cast<std::uint64_t>(current_cycle - previous_cycle);
    }
    last_extended_avtp_ns_ = extended_avtp_ns;
    ++stats_.mapped_timestamps;
    return mapped_timestamp_us;
}

} // namespace media::avtp
