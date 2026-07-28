#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>

namespace render {

/// @brief 基于 steady_clock 的单调播放时钟。
///
/// 阶段 2 先提供线程安全的系统时钟基线；阶段 4 接入 AV sync 后，有音频时将由
/// IAudioRenderer::PlayedPtsUs() 校准，无音频时继续使用该时钟驱动视频节奏。
class PlaybackClock {
public:
    /// @brief 从指定媒体时间重新开始计时。
    void Start(std::int64_t start_pts_us = 0);

    /// @brief 暂停计时，并固定当前播放位置。
    void Pause();

    /// @brief 从暂停位置继续计时。
    void Resume();

    /// @brief 停止并把播放位置重置为指定值。
    void Reset(std::int64_t pts_us = 0);

    /// @brief 用外部媒体时钟校准当前播放位置。
    void SetPositionUs(std::int64_t pts_us);

    /// @brief 返回当前播放位置，单位为微秒。
    std::int64_t PositionUs() const;

    bool IsRunning() const;
    bool IsPaused() const;

private:
    using SteadyClock = std::chrono::steady_clock;

    /// 调用方必须已经持有 mutex_。
    std::int64_t PositionUsLocked(SteadyClock::time_point now) const;

    mutable std::mutex mutex_;
    SteadyClock::time_point reference_time_{SteadyClock::now()};
    std::int64_t reference_pts_us_{0};
    bool running_{false};
    bool paused_{false};
};

} // namespace render
