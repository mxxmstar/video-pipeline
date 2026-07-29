#include "render/media_clock.h"

namespace render {

void MediaClock::Start(std::int64_t start_pts_us) {
    fallback_clock_.Start(start_pts_us);
}

void MediaClock::Pause() {
    fallback_clock_.Pause();
}

void MediaClock::Resume() {
    fallback_clock_.Resume();
}

void MediaClock::Reset(std::int64_t pts_us) {
    fallback_clock_.Reset(pts_us);

    std::lock_guard<std::mutex> lock(mutex_);
    last_audio_pts_us_ = 0;
    has_audio_position_ = false;
}

void MediaClock::SetSystemPositionUs(std::int64_t pts_us) {
    fallback_clock_.SetPositionUs(pts_us);
}

void MediaClock::UpdateAudioPosition(std::int64_t audio_pts_us) {
    if (audio_pts_us <= 0) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_audio_pts_us_ = audio_pts_us;
        has_audio_position_ = true;
    }

    // fallback clock 也同步校准到音频位置。这样音频 master 短暂不可用或切到
    // video-only 时，不会从旧的系统时钟位置突然跳变。
    fallback_clock_.SetPositionUs(audio_pts_us);
}

MediaClockSnapshot MediaClock::Snapshot(bool prefer_audio) const {
    if (prefer_audio) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (has_audio_position_) {
            return {last_audio_pts_us_, MediaClockSource::Audio, true};
        }
    }

    return {fallback_clock_.PositionUs(), MediaClockSource::System, true};
}

bool MediaClock::HasAudioPosition() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return has_audio_position_;
}

} // namespace render
