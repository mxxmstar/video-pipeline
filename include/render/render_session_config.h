#pragma once

#include <cstddef>

#include "render/av_sync_config.h"
#include "render/audio/audio_render_config.h"
#include "render/render_config.h"
#include "render/video_pts_normalizer.h"

namespace render {

/// @brief 一次音视频渲染会话的完整配置。
///
/// RenderSession 只消费解码后的 MediaFrame，不负责拉流和解码。视频窗口与音频
/// 设备均由 session 的工作线程初始化，调用方可以安全地从解码回调提交帧。
struct RenderSessionConfig {
    /// 视频窗口和 OpenGL 行为配置。
    RenderConfig video;

    /// WASAPI 播放设备配置。
    audio::AudioRenderConfig audio;

    /// 是否创建并驱动视频 renderer。
    bool enable_video{true};

    /// 是否创建并驱动音频 renderer。
    bool enable_audio{true};

    /// 视频待渲染队列的最大帧数。队列满时丢弃最旧帧，优先保证低延迟。
    std::size_t max_video_queue_frames{6};

    /// 音频待播放队列的最大帧数。第一版同样丢弃最旧帧，阶段 3 将改为 ring buffer。
    std::size_t max_audio_queue_frames{50};

    /// 目标端到端延迟，暂作为后续 AV sync 和缓冲策略的配置入口。
    int target_latency_ms{120};

    /// 是否允许在低延迟场景丢弃过期视频帧；阶段 2 用于控制队列满时的策略。
    bool drop_late_video_frames{true};

    /// 音视频同步策略配置。阶段 4 第一版用于控制视频帧的早到等待和晚到丢弃。
    AvSyncConfig av_sync;

    /// 是否在双轨进入渲染队列前使用同一个媒体 PTS 原点。
    ///
    /// 开启后双轨直播以首个可显示视频帧作为共同原点，并丢弃该点之前的音频
    /// preroll；这样不会在等待首个视频关键帧时先播放一段没有画面的音频。
    bool normalize_track_pts{true};

    /// 视频 PTS 归一化配置。阶段 4 后续小步用于处理缺失、倒退和大跳变 PTS。
    VideoPtsNormalizerConfig video_pts;
};

} // namespace render
