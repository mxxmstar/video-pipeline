#pragma once

#include <chrono>
#include <cstdint>

#include "render/av_sync_config.h"
#include "render/media_clock.h"

namespace render {

/// @brief 视频帧相对 master clock 的调度动作。
enum class AvSyncAction {
    Render,
    Drop,
    Wait,
};

/// @brief AV sync 对单个视频帧的调度结果。
struct AvSyncDecision {
    AvSyncAction action{AvSyncAction::Render};
    std::int64_t delta_us{0};
    std::chrono::milliseconds wait_duration{0};
};

/// @brief 纯 AV sync 决策器。
///
/// 该类不持有队列、不睡眠、不调用 renderer，只根据“视频帧 PTS”和“当前 master
/// clock 位置”给出 Render/Drop/Wait 决策，便于单元测试和后续替换策略。
class AvSyncController {
public:
    AvSyncController() = default;
    explicit AvSyncController(AvSyncConfig config);

    void SetConfig(AvSyncConfig config);
    const AvSyncConfig& Config() const;

    AvSyncDecision Decide(std::int64_t video_pts_us,
                          const MediaClockSnapshot& clock) const;

private:
    AvSyncConfig config_;
};

} // namespace render
