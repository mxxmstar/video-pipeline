#pragma once

#include <cstddef>
#include <cstdint>

namespace render {

/// @brief RenderSession 的运行统计快照。
///
/// GetStats() 返回结构体副本，因此调用方读取统计时不需要持有 session 内部锁。
struct RenderStats {
    /// 已提交到 RenderSession 的视频帧数。
    std::int64_t submitted_video_frames{0};
    /// 已提交到 RenderSession 的音频帧数。
    std::int64_t submitted_audio_frames{0};
    /// 已真正调用视频 renderer 渲染成功的帧数。
    std::int64_t rendered_video_frames{0};
    /// 已真正调用音频 renderer 处理成功的帧数。
    std::int64_t rendered_audio_frames{0};
    /// 视频总丢帧数，包含队列满丢旧帧和 AV sync 丢晚帧。
    std::int64_t dropped_video_frames{0};
    /// 音频 session 队列层丢帧数，不包含 WASAPI 内部 PCM 丢弃。
    std::int64_t dropped_audio_frames{0};
    /// PTS normalizer 修正过的视频帧数，用于观察上游时间戳是否稳定。
    std::int64_t normalized_video_pts_frames{0};
    /// AV sync 因视频帧严重晚到而主动丢弃的帧数。
    std::int64_t av_sync_dropped_video_frames{0};
    /// AV sync 因视频帧早到而等待的次数。
    std::int64_t av_sync_video_waits{0};
    /// AV sync 累计等待时长，单位微秒。
    std::int64_t av_sync_video_wait_us{0};
    /// 音频 renderer 上报的 underrun 次数。
    std::int64_t audio_underruns{0};
    /// 音频 renderer 内部 PCM 队列丢弃的采样帧数。
    std::int64_t audio_dropped_pcm_frames{0};
    /// 当前播放时间轴位置，单位微秒。
    std::int64_t playback_pts_us{0};
    /// 当前播放时钟来源：0 表示 system fallback，1 表示 audio master。
    int playback_clock_source{0};
    /// 当前视频 session 队列长度。
    std::size_t video_queue_size{0};
    /// 当前音频 session 队列长度。
    std::size_t audio_queue_size{0};
    /// 音频 renderer 内部 PCM chunk 队列长度。
    std::size_t audio_renderer_queue_size{0};
    /// 音频 renderer 内部 PCM 队列中尚未提交给设备的采样帧数。
    std::int64_t audio_renderer_queued_frames{0};
};

} // namespace render
