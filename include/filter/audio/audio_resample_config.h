#pragma once

#include <cstdint>

#include "media/media_frame.h"

namespace filter::audio {

/// @brief 音频重采样/重排配置。
///
/// output_sample_rate/output_channels 为 0 时表示跟随输入帧。
/// output_channel_layout 为 0 时使用 output_channels 对应的 FFmpeg 默认布局。
/// 第一版强制输出 interleaved 格式；如果传入 planar 格式，会自动折叠到 packed 等价格式。
struct AudioResampleConfig {
    int output_sample_rate{48000};
    int output_channels{2};
    std::uint64_t output_channel_layout{0};
    SampleFormat output_sample_format{SampleFormat::S16};
};

} // namespace filter::audio
