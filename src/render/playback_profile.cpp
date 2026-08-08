#include "render/playback_profile.h"

#include <algorithm>
#include <limits>

namespace render {

RenderSessionConfig MakeRenderSessionConfig(
    const mediaflow::PlaybackProfile& profile) {
    RenderSessionConfig config;
    config.audio.buffer_duration_ms =
        std::max(1, profile.audio_device_buffer_ms);
    config.audio.queue_capacity_chunks = static_cast<int>(std::min<std::size_t>(
        profile.ResolveAudioPcmQueueChunks(),
        static_cast<std::size_t>((std::numeric_limits<int>::max)())));
    config.max_video_queue_frames = profile.ResolveVideoQueueFrames();
    config.max_audio_queue_frames = profile.ResolveAudioQueueFrames();
    config.playback_buffer_ms = std::max(0, profile.playback_buffer_ms);

    if (profile.mode == mediaflow::PlaybackMode::StablePlayback) {
        // 稳定播放宁可积压短暂缓存，也不因临界抖动立即覆盖旧帧或丢弃晚帧。
        config.drop_late_video_frames = false;
        config.av_sync.drop_late_video_frames = false;
        config.av_sync.late_threshold_ms = 200;
        config.av_sync.max_video_lag_ms = 350;
    }
    return config;
}

} // namespace render
