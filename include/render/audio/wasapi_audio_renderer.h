#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/queue/mpmc_queue.h"
#include "filter/audio/audio_resample_filter.h"
#include "render/audio/i_audio_renderer.h"

struct IAudioClient;
struct IAudioRenderClient;
struct IMMDevice;
struct IMMDeviceEnumerator;

namespace render::audio {

/// @brief Windows WASAPI shared-mode 音频播放器。
///
/// Render(frame) 在调用线程中完成重采样，然后把 PCM chunk 快速写入内部有界队列；
/// 真正写入 WASAPI shared buffer 的动作由 renderer 内部音频线程完成。这样上层
/// RenderSession 的音频喂帧线程不会被声卡 buffer 状态阻塞，也不会被视频渲染耗时拖慢。
class WasapiAudioRenderer final : public IAudioRenderer {
public:
    WasapiAudioRenderer() = default;
    ~WasapiAudioRenderer() override;

    bool Init(const AudioRenderConfig& config) override;
    bool Render(const MediaFrame& frame) override;
    void Shutdown() override;

    int64_t PlayedPtsUs() const override;
    AudioRenderStats GetStats() const override;
    std::string LastError() const;

private:
    struct PcmChunk {
        std::vector<std::uint8_t> data;
        std::uint32_t nb_samples{0};
        std::int64_t pts_us{0};
        std::int64_t duration_us{0};
    };

    bool InitializeCom();
    bool InitializeDevice();
    bool InitializeResampler();
    bool StartAudioThread();
    void StopAudioThread();
    void AudioThreadMain();
    bool FillWasapiBuffer(PcmChunk& active_chunk,
                          std::uint32_t& active_frame_offset);
    bool EnqueuePcm(filter::audio::PcmFrame&& pcm);
    void DropQueuedChunkStats(const PcmChunk& chunk);
    void DropUnqueuedChunkStats(const PcmChunk& chunk);
    int CurrentPaddingFrames() const;
    void WakeAudioThread();
    void SetError(std::string message);
    void ReleaseResources();

    AudioRenderConfig config_;
    filter::audio::AudioResampleFilter resampler_;
    std::unique_ptr<BoundedMpmcQueue<PcmChunk>> pcm_queue_;
    std::thread audio_thread_;

    IMMDeviceEnumerator* enumerator_{nullptr};
    IMMDevice* device_{nullptr};
    IAudioClient* audio_client_{nullptr};
    IAudioRenderClient* render_client_{nullptr};
    void* mix_format_{nullptr};
    void* buffer_event_{nullptr};

    std::atomic<bool> stop_audio_thread_{false};
    std::atomic<int64_t> submitted_frames_{0};
    std::atomic<int64_t> queued_frames_{0};
    std::atomic<int64_t> queued_chunks_{0};
    std::atomic<int64_t> played_frames_{0};
    std::atomic<int64_t> dropped_frames_{0};
    std::atomic<int64_t> dropped_chunks_{0};
    std::atomic<int64_t> underruns_{0};
    std::atomic<int64_t> first_pts_us_{0};
    std::atomic<bool> first_pts_set_{false};
    int sample_rate_{0};
    int channels_{0};
    int bytes_per_frame_{0};
    std::uint32_t buffer_frame_count_{0};
    bool com_initialized_{false};
    bool started_{false};
    bool initialized_{false};
    mutable std::mutex error_mutex_;
    std::string last_error_;
};

} // namespace render::audio
