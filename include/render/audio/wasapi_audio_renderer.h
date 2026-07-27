#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "filter/audio/audio_resample_filter.h"
#include "render/audio/i_audio_renderer.h"

struct IAudioClient;
struct IAudioRenderClient;
struct IMMDevice;
struct IMMDeviceEnumerator;

namespace render::audio {

/// @brief Windows WASAPI shared-mode 音频播放器。
///
/// 第一版是同步写入模型：Render(frame) 内部完成重采样并把 PCM 写入 WASAPI
/// shared buffer；当设备缓冲满时最多短暂等待可写事件。后续需要更低延迟或更强
/// 抗抖动时，可以把 Render 改成入队，另起音频线程消费 ring buffer。
class WasapiAudioRenderer final : public IAudioRenderer {
public:
    WasapiAudioRenderer() = default;
    ~WasapiAudioRenderer() override;

    bool Init(const AudioRenderConfig& config) override;
    bool Render(const MediaFrame& frame) override;
    void Shutdown() override;

    int64_t PlayedPtsUs() const override;
    const std::string& LastError() const {
        return last_error_;
    }

private:
    bool InitializeCom();
    bool InitializeDevice();
    bool InitializeResampler();
    bool WritePcm(const filter::audio::PcmFrame& pcm);
    int CurrentPaddingFrames() const;
    void SetError(std::string message);
    void ReleaseResources();

    AudioRenderConfig config_;
    filter::audio::AudioResampleFilter resampler_;

    IMMDeviceEnumerator* enumerator_{nullptr};
    IMMDevice* device_{nullptr};
    IAudioClient* audio_client_{nullptr};
    IAudioRenderClient* render_client_{nullptr};
    void* mix_format_{nullptr};
    void* buffer_event_{nullptr};

    std::atomic<int64_t> submitted_frames_{0};
    int64_t first_pts_us_{0};
    int sample_rate_{0};
    int channels_{0};
    int bytes_per_frame_{0};
    std::uint32_t buffer_frame_count_{0};
    bool first_pts_set_{false};
    bool com_initialized_{false};
    bool started_{false};
    bool initialized_{false};
    std::string last_error_;
};

} // namespace render::audio
