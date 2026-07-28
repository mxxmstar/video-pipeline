#pragma once

#include <cstddef>
#include <cstdint>

namespace render {

/// @brief RenderSession 的运行统计快照。
///
/// GetStats() 返回结构体副本，因此调用方读取统计时不需要持有 session 内部锁。
struct RenderStats {
    std::int64_t submitted_video_frames{0};
    std::int64_t submitted_audio_frames{0};
    std::int64_t rendered_video_frames{0};
    std::int64_t rendered_audio_frames{0};
    std::int64_t dropped_video_frames{0};
    std::int64_t dropped_audio_frames{0};
    std::int64_t audio_underruns{0};
    std::int64_t audio_dropped_pcm_frames{0};
    std::int64_t playback_pts_us{0};
    std::size_t video_queue_size{0};
    std::size_t audio_queue_size{0};
    std::size_t audio_renderer_queue_size{0};
    std::int64_t audio_renderer_queued_frames{0};
};

} // namespace render
