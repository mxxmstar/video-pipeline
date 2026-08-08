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
    /// 超过最大视频滞后上限后，为追赶 audio master 丢弃的帧数。
    std::int64_t video_catch_up_dropped_frames{0};
    /// 进入视频追赶状态的次数。
    std::int64_t video_catch_up_events{0};
    /// 当前是否正在连续丢弃滞后视频帧。
    bool video_catch_up_active{false};
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
    /// 使用音频 master 采样到的视频显示同步误差次数。
    std::int64_t video_audio_master_sync_samples{0};
    /// 最近一次同步误差，定义为视频显示 PTS - 音频已播放 PTS，单位微秒。
    /// 正值表示视频领先，负值表示视频落后。
    std::int64_t last_video_audio_master_error_us{0};
    /// 已采样误差中的最小值，单位微秒。
    std::int64_t min_video_audio_master_error_us{0};
    /// 已采样误差中的最大值，单位微秒。
    std::int64_t max_video_audio_master_error_us{0};
    /// 已采样误差绝对值之和，调用方可除以样本数得到平均绝对误差。
    std::int64_t total_abs_video_audio_master_error_us{0};
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
