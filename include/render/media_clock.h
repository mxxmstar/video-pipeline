#pragma once

#include <cstdint>
#include <mutex>

#include "render/playback_clock.h"

namespace render {

/// @brief 当前播放时钟来源。
enum class MediaClockSource {
    System,
    Audio,
};

/// @brief 一次播放时钟读取结果。
struct MediaClockSnapshot {
    std::int64_t position_us{0};
    MediaClockSource source{MediaClockSource::System};
    bool valid{false};
};

/// @brief 阶段 4 的统一播放时钟入口。
///
/// MediaClock 不直接访问 WASAPI，也不直接理解视频帧队列。它只维护两类时间：
/// 1. fallback_clock_：基于 steady_clock 的系统播放时钟，用于无音频或音频不可用场景。
/// 2. last_audio_pts_us_：音频 renderer 上报的真实播放位置，用于有音频时作为 master。
///
/// 这样 AV sync 只需要向 MediaClock 读取“当前 master 播放到哪里”，不用关心 master
/// 来自声卡 padding 估算还是系统时间。
class MediaClock {
public:
    /// @brief 启动 fallback 系统时钟。
    void Start(std::int64_t start_pts_us = 0);

    /// @brief 暂停 fallback 系统时钟。
    void Pause();

    /// @brief 恢复 fallback 系统时钟。
    void Resume();

    /// @brief 重置系统时钟和音频 master 快照。
    void Reset(std::int64_t pts_us = 0);

    /// @brief 重新锚定 fallback 系统时钟的位置。
    void SetSystemPositionUs(std::int64_t pts_us);

    /// @brief 更新音频 master 位置。
    ///
    /// audio_pts_us <= 0 通常表示音频 renderer 尚未建立有效播放位置，此时不会切到
    /// audio master。
    void UpdateAudioPosition(std::int64_t audio_pts_us);

    /// @brief 读取当前播放位置。
    ///
    /// prefer_audio=true 且已有有效音频位置时返回 audio master；否则返回系统时钟。
    MediaClockSnapshot Snapshot(bool prefer_audio) const;

    bool HasAudioPosition() const;

private:
    PlaybackClock fallback_clock_;
    mutable std::mutex mutex_;
    std::int64_t last_audio_pts_us_{0};
    bool has_audio_position_{false};
};

} // namespace render
