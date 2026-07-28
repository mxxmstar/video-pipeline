#pragma once

namespace render::audio {

/// @brief 音频播放配置。
///
/// 第一版使用 WASAPI shared mode 和系统默认播放设备。采样率/声道/格式由
/// WASAPI mix format 决定，filter/audio 会负责把解码帧转换到该格式。
struct AudioRenderConfig {
    /// WASAPI shared-mode 设备 buffer 的目标时长。默认偏低延迟；真实 RTSP 预览
    /// 如果存在网络抖动或同进程编码负载，可以在测试/上层配置中适当调大。
    int buffer_duration_ms{100};

    /// WasapiAudioRenderer 内部 PCM chunk 队列容量。每个 chunk 通常对应一帧
    /// 解码音频重采样后的数据；容量越大越抗突发，代价是最坏情况下播放延迟更高。
    int queue_capacity_chunks{16};

    /// true 表示没有默认播放设备时启动失败；false 表示降级为只预览视频，并把
    /// 后续提交的音频帧计入丢弃统计。
    bool fail_if_device_unavailable{false};
};

} // namespace render::audio
