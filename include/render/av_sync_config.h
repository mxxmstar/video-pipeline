#pragma once

namespace render {

/// @brief 音视频同步配置。
struct AvSyncConfig {
    /// 是否启用视频 PTS 调度。关闭后视频线程取到帧就立即渲染。
    bool enabled{true};

    /// 视频帧落后 master clock 超过该阈值时，认为已经过期。
    int late_threshold_ms{80};

    /// 视频帧领先 master clock 超过该阈值时，短暂等待。
    int early_threshold_ms{20};

    /// 单次等待的最大时长。保持较小可以让窗口事件和 Stop/Pause 更快响应。
    int max_wait_ms{20};

    /// 是否丢弃过期视频帧。低延迟预览建议开启，文件播放可按需关闭。
    bool drop_late_video_frames{true};
};

} // namespace render
