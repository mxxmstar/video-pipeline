#pragma once

#include <cstdint>
#include <vector>

#include "media/media_frame.h"

namespace filter::audio {

/// @brief 音频播放/后处理阶段使用的连续 PCM 帧。
///
/// AudioResampleFilter 第一版固定输出 interleaved PCM，便于 WASAPI、
/// 文件写入、波形分析等 sink 直接消费。这里不继承 MediaFrame，
/// 是为了避免播放端再理解 planar plane/offset 等解码侧细节。
struct PcmFrame {
    int sample_rate{0};
    int channels{0};
    std::uint64_t channel_layout{0};
    SampleFormat sample_format{SampleFormat::Unknown};
    int bytes_per_sample{0};
    int nb_samples{0};
    int64_t pts_us{0};
    int64_t duration_us{0};
    std::vector<std::uint8_t> data;

    bool Empty() const {
        return sample_rate <= 0 ||
               channels <= 0 ||
               nb_samples <= 0 ||
               sample_format == SampleFormat::Unknown ||
               data.empty();
    }

    int BytesPerFrame() const {
        return channels > 0 && bytes_per_sample > 0
            ? channels * bytes_per_sample
            : 0;
    }
};

} // namespace filter::audio
