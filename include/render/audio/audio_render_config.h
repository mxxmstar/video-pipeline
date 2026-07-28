#pragma once

namespace render::audio {

/// @brief 音频播放配置。
///
/// 第一版使用 WASAPI shared mode 和系统默认播放设备。采样率/声道/格式由
/// WASAPI mix format 决定，filter/audio 会负责把解码帧转换到该格式。
struct AudioRenderConfig {
    int buffer_duration_ms{100};
    int queue_capacity_chunks{16};
    bool fail_if_device_unavailable{false};
};

} // namespace render::audio
