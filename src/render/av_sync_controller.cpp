#include "render/av_sync_controller.h"

#include <algorithm>

namespace render {

namespace {

std::int64_t MillisecondsToUs(int value_ms) {
    return static_cast<std::int64_t>(std::max(0, value_ms)) * 1000;
}

} // namespace

AvSyncController::AvSyncController(AvSyncConfig config)
    : config_(config) {}

void AvSyncController::SetConfig(AvSyncConfig config) {
    config_ = config;
}

const AvSyncConfig& AvSyncController::Config() const {
    return config_;
}

AvSyncDecision AvSyncController::Decide(
    std::int64_t video_pts_us,
    const MediaClockSnapshot& clock) const {
    AvSyncDecision decision;
    if (!config_.enabled || !clock.valid) {
        return decision;
    }

    decision.delta_us = video_pts_us - clock.position_us;
    const auto late_threshold_us = MillisecondsToUs(config_.late_threshold_ms);
    const auto early_threshold_us = MillisecondsToUs(config_.early_threshold_ms);

    if (config_.drop_late_video_frames &&
        decision.delta_us < -late_threshold_us) {
        decision.action = AvSyncAction::Drop;
        return decision;
    }

    if (decision.delta_us > early_threshold_us) {
        const auto max_wait_us = MillisecondsToUs(config_.max_wait_ms);
        const auto wait_us =
            std::min<std::int64_t>(decision.delta_us - early_threshold_us,
                                   std::max<std::int64_t>(1000, max_wait_us));
        decision.action = AvSyncAction::Wait;
        decision.wait_duration = std::chrono::milliseconds(
            std::max<std::int64_t>(1, wait_us / 1000));
    }

    return decision;
}

} // namespace render
