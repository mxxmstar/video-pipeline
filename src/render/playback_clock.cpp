#include "render/playback_clock.h"

#include <algorithm>

namespace render {

void PlaybackClock::Start(std::int64_t start_pts_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    reference_pts_us_ = start_pts_us;
    reference_time_ = SteadyClock::now();
    running_ = true;
    paused_ = false;
}

void PlaybackClock::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || paused_) {
        return;
    }

    // 先把已经流逝的系统时间折算进媒体时间，再冻结参考点。
    reference_pts_us_ = PositionUsLocked(SteadyClock::now());
    reference_time_ = SteadyClock::now();
    paused_ = true;
}

void PlaybackClock::Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || !paused_) {
        return;
    }

    // 暂停期间不计入媒体时间，恢复时只需重建 steady_clock 参考点。
    reference_time_ = SteadyClock::now();
    paused_ = false;
}

void PlaybackClock::Reset(std::int64_t pts_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    reference_pts_us_ = pts_us;
    reference_time_ = SteadyClock::now();
    running_ = false;
    paused_ = false;
}

void PlaybackClock::SetPositionUs(std::int64_t pts_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    reference_pts_us_ = pts_us;
    reference_time_ = SteadyClock::now();
}

std::int64_t PlaybackClock::PositionUs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return PositionUsLocked(SteadyClock::now());
}

bool PlaybackClock::IsRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

bool PlaybackClock::IsPaused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}

std::int64_t PlaybackClock::PositionUsLocked(SteadyClock::time_point now) const {
    if (!running_ || paused_) {
        return reference_pts_us_;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        now - reference_time_);
    return reference_pts_us_ + std::max<std::int64_t>(0, elapsed.count());
}

} // namespace render
