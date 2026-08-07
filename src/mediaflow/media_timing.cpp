#include "mediaflow/media_timing.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mediaflow {

namespace {

constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;

std::int64_t SaturatingAdd(std::int64_t left, std::int64_t right) {
    const auto max_value = (std::numeric_limits<std::int64_t>::max)();
    const auto min_value = (std::numeric_limits<std::int64_t>::min)();
    if (right > 0 && left > max_value - right) {
        return max_value;
    }
    if (right < 0 && left < min_value - right) {
        return min_value;
    }
    return left + right;
}

std::int64_t SaturatingSubtract(std::int64_t left, std::int64_t right) {
    const auto max_value = (std::numeric_limits<std::int64_t>::max)();
    const auto min_value = (std::numeric_limits<std::int64_t>::min)();
    if (right > 0 && left < min_value + right) {
        return min_value;
    }
    if (right < 0 && left > max_value + right) {
        return max_value;
    }
    return left - right;
}

std::int64_t NonNegative(std::int64_t value) {
    return std::max<std::int64_t>(0, value);
}

} // namespace

void UnifiedClock::Start(std::uint64_t generation,
                         std::int64_t start_pts_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_ = generation;
    reference_position_us_ = start_pts_us;
    reference_time_ = SteadyClock::now();
    running_ = true;
    paused_ = false;
    has_audio_position_ = false;
    audio_position_us_ = 0;
}

void UnifiedClock::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || paused_) {
        return;
    }
    reference_position_us_ = SystemPositionUsLocked(SteadyClock::now());
    reference_time_ = SteadyClock::now();
    paused_ = true;
}

void UnifiedClock::Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || !paused_) {
        return;
    }
    reference_time_ = SteadyClock::now();
    paused_ = false;
}

void UnifiedClock::Reset(std::uint64_t generation,
                         std::int64_t position_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_ = generation;
    reference_position_us_ = position_us;
    reference_time_ = SteadyClock::now();
    running_ = false;
    paused_ = false;
    has_audio_position_ = false;
    audio_position_us_ = 0;
    ++discontinuity_;
}

void UnifiedClock::NotifyDiscontinuity(std::uint64_t generation,
                                       std::int64_t position_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    generation_ = generation;
    reference_position_us_ = position_us;
    reference_time_ = SteadyClock::now();
    // discontinuity 表示时间轴边界，而不是停止状态。重连后的调度器需要
    // 能立即读取一个有效的 system clock，随后再由音频轨道校准 master。
    running_ = true;
    paused_ = false;
    has_audio_position_ = false;
    audio_position_us_ = 0;
    ++discontinuity_;
}

void UnifiedClock::SetSystemPositionUs(std::int64_t position_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    reference_position_us_ = position_us;
    reference_time_ = SteadyClock::now();
}

void UnifiedClock::UpdateAudioPosition(std::uint64_t generation,
                                       std::int64_t audio_pts_us) {
    if (audio_pts_us == kNoTimestamp) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (generation != generation_) {
        // 新代次的第一个音频位置就是新的时间轴锚点，不能与旧连接的
        // audio_position_us_ 继续比较。
        generation_ = generation;
        reference_position_us_ = audio_pts_us;
        reference_time_ = SteadyClock::now();
        has_audio_position_ = false;
        ++discontinuity_;
    }

    audio_position_us_ = audio_pts_us;
    has_audio_position_ = true;
    reference_position_us_ = audio_pts_us;
    reference_time_ = SteadyClock::now();
    running_ = true;
    paused_ = false;
}

ClockSnapshot UnifiedClock::Snapshot(bool prefer_audio) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (prefer_audio && has_audio_position_) {
        return {audio_position_us_, ClockSource::Audio, generation_,
                discontinuity_, true};
    }
    return {SystemPositionUsLocked(SteadyClock::now()), ClockSource::System,
            generation_, discontinuity_, running_ || has_audio_position_};
}

bool UnifiedClock::HasAudioPosition() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_audio_position_;
}

bool UnifiedClock::IsRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_ && !paused_;
}

std::int64_t UnifiedClock::SystemPositionUsLocked(
    SteadyClock::time_point now) const {
    if (!running_ || paused_) {
        return reference_position_us_;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        now - reference_time_);
    return SaturatingAdd(reference_position_us_,
                         std::max<std::int64_t>(0, elapsed.count()));
}

std::int64_t TimestampToMicroseconds(std::int64_t timestamp,
                                     const Rational& time_base) {
    if (timestamp == kNoTimestamp || !IsValidTimeBase(time_base)) {
        return kNoTimestamp;
    }

    // long double 能覆盖整数乘除过程的范围，随后通过边界检查避免把
    // 超范围结果转换为实现相关的 int64_t 值。正常媒体时间戳不会接近边界，
    // 该处理同时覆盖高频采样时钟和很大的容器时间基。
    const long double value =
        static_cast<long double>(timestamp) *
        static_cast<long double>(time_base.num) *
        static_cast<long double>(kMicrosecondsPerSecond) /
        static_cast<long double>(time_base.den);
    const auto max_value = (std::numeric_limits<std::int64_t>::max)();
    const auto min_value = (std::numeric_limits<std::int64_t>::min)();
    if (!std::isfinite(value) || value <= static_cast<long double>(min_value) ||
        value >= static_cast<long double>(max_value)) {
        return kNoTimestamp;
    }
    return static_cast<std::int64_t>(value);
}

MediaTiming GetMediaTiming(const MediaPacket& packet) {
    MediaTiming timing;
    timing.pts_us = TimestampToMicroseconds(packet.pts, packet.time_base);
    timing.dts_us = TimestampToMicroseconds(packet.dts, packet.time_base);
    timing.duration_us = TimestampToMicroseconds(packet.duration,
                                                 packet.time_base);
    timing.pts_valid = timing.pts_us != kNoTimestamp;
    timing.dts_valid = timing.dts_us != kNoTimestamp;
    timing.duration_valid = timing.duration_us != kNoTimestamp;
    return timing;
}

VideoPtsScheduler::VideoPtsScheduler(VideoScheduleConfig config)
    : config_(config) {}

void VideoPtsScheduler::SetConfig(VideoScheduleConfig config) {
    config_ = config;
}

const VideoScheduleConfig& VideoPtsScheduler::Config() const {
    return config_;
}

VideoScheduleDecision VideoPtsScheduler::Decide(
    std::int64_t video_pts_us,
    bool keyframe,
    std::uint64_t generation,
    const ClockSnapshot& clock) const {
    VideoScheduleDecision decision;
    decision.keyframe = keyframe;

    if (!config_.enabled || video_pts_us == kNoTimestamp || !clock.valid) {
        return decision;
    }

    // 代次不一致时禁止跨连接比较 PTS。上层应丢弃旧包或先重置时钟；
    // 这里返回 Render 仅表示“同步器不做丢弃”，不代表可以跳过代次检查。
    if (generation != clock.generation) {
        decision.clock_reset = true;
        return decision;
    }

    decision.delta_us = SaturatingSubtract(video_pts_us, clock.position_us);
    const auto late_threshold_us = NonNegative(config_.late_threshold_us);
    const auto early_threshold_us = NonNegative(config_.early_threshold_us);
    if (keyframe) {
        // 关键帧是恢复窗口的锚点，不能因为视频比音频晚就直接丢掉。
        return decision;
    }

    if (config_.drop_late_video_frames &&
        decision.delta_us < -late_threshold_us) {
        decision.action = VideoScheduleAction::Drop;
        return decision;
    }

    if (decision.delta_us > early_threshold_us) {
        const auto max_wait_us = NonNegative(config_.max_wait_us);
        decision.wait_us = std::min(decision.delta_us - early_threshold_us,
                                    std::max<std::int64_t>(1'000,
                                                           max_wait_us));
        decision.action = VideoScheduleAction::Wait;
    }
    return decision;
}

DtsInterleaver::DtsInterleaver(DtsInterleaverConfig config)
    : config_(config) {
    SetConfig(config);
}

void DtsInterleaver::SetConfig(DtsInterleaverConfig config) {
    config_ = config;
    if (config_.max_pending_packets == 0) {
        config_.max_pending_packets = 1;
    }
    config_.max_pending_span_us = NonNegative(config_.max_pending_span_us);
}

const DtsInterleaverConfig& DtsInterleaver::Config() const {
    return config_;
}

std::vector<DtsPacket> DtsInterleaver::Push(DtsPacket item) {
    std::vector<DtsPacket> output;
    if (!item.packet) {
        return output;
    }

    if (generation_ != 0 && item.generation != generation_) {
        // 不把重连前后的包按 DTS 排在同一堆中。Flush 保留旧代次数据，
        // 新代次随后从空缓存开始，调用方可以明确观察到边界。
        output = Flush();
    }
    if (generation_ == 0) {
        generation_ = item.generation;
    }

    if (!item.timing.pts_valid && !item.timing.dts_valid) {
        // 没有任何可靠时间戳的包不能伪造 DTS；先发送当前有序缓存，再按
        // 到达顺序发送它，避免用 PTS 偷换 DTS 语义。
        EmitReady(output, true);
        output.push_back(std::move(item));
        return output;
    }

    if (!item.timing.dts_valid) {
        EmitReady(output, true);
        output.push_back(std::move(item));
        return output;
    }

    pending_.push_back(std::move(item));
    EmitReady(output, false);
    return output;
}

std::vector<DtsPacket> DtsInterleaver::Flush() {
    std::vector<DtsPacket> output;
    EmitReady(output, true);
    generation_ = 0;
    return output;
}

void DtsInterleaver::Reset(std::uint64_t generation) {
    pending_.clear();
    generation_ = generation;
}

std::size_t DtsInterleaver::PendingSize() const {
    return pending_.size();
}

void DtsInterleaver::EmitReady(std::vector<DtsPacket>& output, bool force) {
    if (pending_.empty()) {
        return;
    }

    auto earliest = [&]() {
        return std::min_element(
            pending_.begin(), pending_.end(),
            [](const DtsPacket& left, const DtsPacket& right) {
                return left.timing.dts_us < right.timing.dts_us;
            });
    };

    while (!pending_.empty()) {
        const auto first = earliest();
        const auto last_dts = std::max_element(
            pending_.begin(), pending_.end(),
            [](const DtsPacket& left, const DtsPacket& right) {
                return left.timing.dts_us < right.timing.dts_us;
            });
        const bool over_count =
            pending_.size() > std::max<std::size_t>(1, config_.max_pending_packets);
        const bool over_span =
            config_.max_pending_span_us > 0 &&
            SaturatingSubtract(last_dts->timing.dts_us,
                               first->timing.dts_us) >=
                config_.max_pending_span_us;
        if (!force && !over_count && !over_span) {
            break;
        }

        output.push_back(std::move(*first));
        pending_.erase(first);
        if (!force && pending_.size() <= 1) {
            break;
        }
    }
}

} // namespace mediaflow
