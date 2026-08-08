#pragma once

namespace render {

/// @brief 音视频同步配置。
struct AvSyncConfig {
    /// 是否启用视频 PTS 调度。关闭后视频线程取到帧就立即渲染。
    bool enabled{true};

    /// 启用音视频时，音频首帧是否等待视频首帧进入 renderer。
    /// 默认开启，避免音频设备先建立播放进度造成固定起播偏移；视频 renderer
    /// 开始处理首帧后，音频线程仍可独立继续提交，不会被慢视频帧长期阻塞。
    bool wait_audio_for_video_start{true};

    /// 视频帧落后 master clock 超过该阈值时，认为已经过期。
    int late_threshold_ms{80};

    /// 视频相对 audio master 允许的最大滞后。超过后进入追赶，连续丢弃旧视频帧；
    /// 0 表示关闭该保护。该值通常大于 late_threshold_ms，给稳定播放保留短时抖动。
    int max_video_lag_ms{0};

    /// 视频帧领先 master clock 超过该阈值时，短暂等待。
    int early_threshold_ms{20};

    /// 单次等待的最大时长。保持较小可以让窗口事件和 Stop/Pause 更快响应。
    int max_wait_ms{20};

    /// 是否丢弃过期视频帧。低延迟预览建议开启，文件播放可按需关闭。
    bool drop_late_video_frames{true};
};

} // namespace render
