#pragma once

#include <string>

#include "filter/audio/audio_resample_config.h"
#include "filter/audio/pcm_frame.h"
#include "media/media_frame.h"

struct SwrContext;

namespace filter::audio {

/// @brief 基于 FFmpeg swresample 的音频格式转换 filter。
///
/// 职责边界：
/// - 输入：当前工程 decoder 输出的 MediaFrame 音频帧；
/// - 输出：interleaved PcmFrame；
/// - 能力：采样格式转换、planar -> interleaved、采样率转换、声道布局转换；
/// - 不负责：声卡设备、播放线程、AV sync，这些属于 render/audio 层。
class AudioResampleFilter {
public:
    AudioResampleFilter() = default;
    ~AudioResampleFilter();

    AudioResampleFilter(const AudioResampleFilter&) = delete;
    AudioResampleFilter& operator=(const AudioResampleFilter&) = delete;

    bool Open(const AudioResampleConfig& config);
    bool Process(const MediaFrame& input, PcmFrame& output);
    void Close();

    const std::string& LastError() const {
        return last_error_;
    }

private:
    struct ResampleParams {
        SampleFormat input_format{SampleFormat::Unknown};
        int input_sample_rate{0};
        int input_channels{0};
        std::uint64_t input_channel_layout{0};
        SampleFormat output_format{SampleFormat::Unknown};
        int output_sample_rate{0};
        int output_channels{0};
        std::uint64_t output_channel_layout{0};

        bool operator==(const ResampleParams& other) const = default;
    };

    bool EnsureContext(const MediaFrame& input, ResampleParams& params);
    bool BuildParams(const MediaFrame& input, ResampleParams& params);
    void SetError(std::string message);

    AudioResampleConfig config_;
    ResampleParams params_;
    SwrContext* swr_{nullptr};
    bool opened_{false};
    std::string last_error_;
};

} // namespace filter::audio
