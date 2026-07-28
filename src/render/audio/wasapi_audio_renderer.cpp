#include "render/audio/wasapi_audio_renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <system_error>
#include <string>
#include <thread>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <mmreg.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>

namespace render::audio {
namespace {

template <typename T>
void SafeRelease(T*& ptr) {
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

std::string HResultString(HRESULT hr) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(hr));
    return buffer;
}

bool IsExtensibleSubFormat(const WAVEFORMATEX* format, const GUID& guid) {
    if (!format || format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return false;
    }
    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    return IsEqualGUID(extensible->SubFormat, guid) != 0;
}

SampleFormat SampleFormatFromWaveFormat(const WAVEFORMATEX* format) {
    if (!format) {
        return SampleFormat::Unknown;
    }

    if ((format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) ||
        (format->wBitsPerSample == 32 &&
         IsExtensibleSubFormat(format, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))) {
        return SampleFormat::FLT;
    }
    if ((format->wFormatTag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16) ||
        (format->wBitsPerSample == 16 &&
         IsExtensibleSubFormat(format, KSDATAFORMAT_SUBTYPE_PCM))) {
        return SampleFormat::S16;
    }
    return SampleFormat::Unknown;
}

std::uint64_t ChannelLayoutFromWaveFormat(const WAVEFORMATEX* format) {
    if (!format) {
        return 0;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return extensible->dwChannelMask;
    }

    if (format->nChannels == 1) {
        return 4; // AV_CH_LAYOUT_MONO
    }
    if (format->nChannels == 2) {
        return 3; // AV_CH_LAYOUT_STEREO
    }
    return 0;
}

} // namespace

WasapiAudioRenderer::~WasapiAudioRenderer() {
    Shutdown();
}

bool WasapiAudioRenderer::Init(const AudioRenderConfig& config) {
    Shutdown();
    config_ = config;
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_.clear();
    }

    if (!InitializeCom() || !InitializeDevice() || !InitializeResampler()) {
        ReleaseResources();
        return false;
    }

    pcm_queue_ = std::make_unique<BoundedMpmcQueue<PcmChunk>>(
        static_cast<std::size_t>(std::max(2, config_.queue_capacity_chunks)));

    std::uint8_t* silence = nullptr;
    HRESULT hr = render_client_->GetBuffer(buffer_frame_count_, &silence);
    if (FAILED(hr)) {
        SetError("IAudioRenderClient::GetBuffer failed: " + HResultString(hr));
        ReleaseResources();
        return false;
    }
    hr = render_client_->ReleaseBuffer(buffer_frame_count_, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr)) {
        SetError("IAudioRenderClient::ReleaseBuffer failed: " + HResultString(hr));
        ReleaseResources();
        return false;
    }

    if (!StartAudioThread()) {
        ReleaseResources();
        return false;
    }

    hr = audio_client_->Start();
    if (FAILED(hr)) {
        SetError("IAudioClient::Start failed: " + HResultString(hr));
        ReleaseResources();
        return false;
    }

    started_ = true;
    initialized_ = true;
    return true;
}

bool WasapiAudioRenderer::Render(const MediaFrame& frame) {
    if (!initialized_) {
        SetError("WasapiAudioRenderer is not initialized");
        return false;
    }

    filter::audio::PcmFrame pcm;
    if (!resampler_.Process(frame, pcm)) {
        SetError("audio resample failed: " + resampler_.LastError());
        return false;
    }

    if (!first_pts_set_) {
        first_pts_us_.store(pcm.pts_us);
        first_pts_set_.store(true);
    }
    return EnqueuePcm(std::move(pcm));
}

void WasapiAudioRenderer::Shutdown() {
    ReleaseResources();
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_.clear();
}

int64_t WasapiAudioRenderer::PlayedPtsUs() const {
    if (!initialized_ || sample_rate_ <= 0 || !first_pts_set_) {
        return 0;
    }

    const auto padding = CurrentPaddingFrames();
    const auto device_played_frames =
        std::max<int64_t>(0, played_frames_.load() - std::max(0, padding));
    return first_pts_us_.load() + device_played_frames * 1'000'000LL / sample_rate_;
}

AudioRenderStats WasapiAudioRenderer::GetStats() const {
    AudioRenderStats stats;
    stats.submitted_pcm_frames = submitted_frames_.load();
    stats.queued_pcm_frames = std::max<int64_t>(0, queued_frames_.load());
    stats.played_pcm_frames = played_frames_.load();
    stats.dropped_pcm_frames = dropped_frames_.load();
    stats.dropped_pcm_chunks = dropped_chunks_.load();
    stats.underruns = underruns_.load();
    stats.queued_pcm_chunks =
        static_cast<std::size_t>(std::max<int64_t>(0, queued_chunks_.load()));
    return stats;
}

std::string WasapiAudioRenderer::LastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

bool WasapiAudioRenderer::InitializeCom() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        com_initialized_ = true;
        return true;
    }
    if (hr == RPC_E_CHANGED_MODE) {
        // 当前线程已经由宿主用其他 apartment 初始化 COM；继续使用现有 COM 状态。
        com_initialized_ = false;
        return true;
    }
    SetError("CoInitializeEx failed: " + HResultString(hr));
    return false;
}

bool WasapiAudioRenderer::InitializeDevice() {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                  nullptr,
                                  CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr)) {
        SetError("CoCreateInstance(MMDeviceEnumerator) failed: " + HResultString(hr));
        return false;
    }

    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr)) {
        SetError("GetDefaultAudioEndpoint failed: " + HResultString(hr));
        return false;
    }

    hr = device_->Activate(__uuidof(IAudioClient),
                           CLSCTX_ALL,
                           nullptr,
                           reinterpret_cast<void**>(&audio_client_));
    if (FAILED(hr)) {
        SetError("IMMDevice::Activate(IAudioClient) failed: " + HResultString(hr));
        return false;
    }

    WAVEFORMATEX* mix_format = nullptr;
    hr = audio_client_->GetMixFormat(&mix_format);
    if (FAILED(hr) || !mix_format) {
        SetError("IAudioClient::GetMixFormat failed: " + HResultString(hr));
        return false;
    }
    mix_format_ = mix_format;

    sample_rate_ = static_cast<int>(mix_format->nSamplesPerSec);
    channels_ = static_cast<int>(mix_format->nChannels);
    bytes_per_frame_ = static_cast<int>(mix_format->nBlockAlign);
    if (SampleFormatFromWaveFormat(mix_format) == SampleFormat::Unknown ||
        sample_rate_ <= 0 || channels_ <= 0 || bytes_per_frame_ <= 0) {
        SetError("unsupported WASAPI mix format");
        return false;
    }

    buffer_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!buffer_event_) {
        SetError("CreateEventW failed");
        return false;
    }

    const REFERENCE_TIME buffer_duration =
        static_cast<REFERENCE_TIME>(std::max(20, config_.buffer_duration_ms)) * 10'000;
    hr = audio_client_->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                   AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                   buffer_duration,
                                   0,
                                   mix_format,
                                   nullptr);
    if (FAILED(hr)) {
        SetError("IAudioClient::Initialize failed: " + HResultString(hr));
        return false;
    }

    hr = audio_client_->SetEventHandle(static_cast<HANDLE>(buffer_event_));
    if (FAILED(hr)) {
        SetError("IAudioClient::SetEventHandle failed: " + HResultString(hr));
        return false;
    }

    hr = audio_client_->GetBufferSize(&buffer_frame_count_);
    if (FAILED(hr) || buffer_frame_count_ == 0) {
        SetError("IAudioClient::GetBufferSize failed: " + HResultString(hr));
        return false;
    }

    hr = audio_client_->GetService(__uuidof(IAudioRenderClient),
                                   reinterpret_cast<void**>(&render_client_));
    if (FAILED(hr)) {
        SetError("IAudioClient::GetService(IAudioRenderClient) failed: " + HResultString(hr));
        return false;
    }
    return true;
}

bool WasapiAudioRenderer::InitializeResampler() {
    const auto* mix_format = static_cast<const WAVEFORMATEX*>(mix_format_);
    filter::audio::AudioResampleConfig config;
    config.output_sample_rate = sample_rate_;
    config.output_channels = channels_;
    config.output_channel_layout = ChannelLayoutFromWaveFormat(mix_format);
    config.output_sample_format = SampleFormatFromWaveFormat(mix_format);
    return resampler_.Open(config);
}

bool WasapiAudioRenderer::StartAudioThread() {
    stop_audio_thread_.store(false);
    try {
        audio_thread_ = std::thread(&WasapiAudioRenderer::AudioThreadMain, this);
    } catch (const std::system_error& error) {
        SetError(std::string("failed to start WASAPI audio thread: ") + error.what());
        return false;
    }
    return true;
}

void WasapiAudioRenderer::StopAudioThread() {
    stop_audio_thread_.store(true);
    WakeAudioThread();
    if (audio_thread_.joinable()) {
        audio_thread_.join();
    }
}

void WasapiAudioRenderer::AudioThreadMain() {
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool thread_com_initialized = SUCCEEDED(com_hr);

    PcmChunk active_chunk;
    std::uint32_t active_frame_offset = 0;
    while (!stop_audio_thread_.load()) {
        WaitForSingleObject(static_cast<HANDLE>(buffer_event_), 20);
        if (stop_audio_thread_.load()) {
            break;
        }
        if (!FillWasapiBuffer(active_chunk, active_frame_offset)) {
            break;
        }
    }

    if (thread_com_initialized) {
        CoUninitialize();
    }
}

bool WasapiAudioRenderer::FillWasapiBuffer(PcmChunk& active_chunk,
                                           std::uint32_t& active_frame_offset) {
    const auto padding = CurrentPaddingFrames();
    if (padding < 0) {
        SetError("IAudioClient::GetCurrentPadding failed");
        return false;
    }
    const auto available_frames =
        buffer_frame_count_ > static_cast<std::uint32_t>(padding)
            ? buffer_frame_count_ - static_cast<std::uint32_t>(padding)
            : 0;
    if (available_frames == 0) {
        return true;
    }

    // 只向 WASAPI 申请当前确实能写出的帧数。旧实现会申请全部 available_frames，
    // PCM 不足时把剩余空间一次性填静音；当 shared buffer 比单个音频包大很多时，
    // 一个很短的输入抖动会被扩展成较长静音，听感上就是断续或“声音很怪”。
    const auto active_remaining =
        active_frame_offset < active_chunk.nb_samples
            ? active_chunk.nb_samples - active_frame_offset
            : 0;
    const auto queued_available = std::max<int64_t>(0, queued_frames_.load());
    auto frames_to_request = std::min<std::uint32_t>(
        available_frames,
        active_remaining + static_cast<std::uint32_t>(
            std::min<int64_t>(queued_available,
                              std::numeric_limits<std::uint32_t>::max())));

    bool silence_only = false;
    if (frames_to_request == 0) {
        // 完全没有 PCM 时仍写入一个很短的静音片段，避免设备 buffer 彻底饿住
        // 并造成线程忙等。这里不填满整个 available 区域，给随后到达的真实 PCM
        // 留出尽快进入声卡 buffer 的机会。
        const auto silence_quantum =
            static_cast<std::uint32_t>(std::max(1, sample_rate_ / 100));
        frames_to_request = std::min(available_frames, silence_quantum);
        silence_only = true;
        ++underruns_;
    }

    std::uint8_t* dst = nullptr;
    HRESULT hr = render_client_->GetBuffer(frames_to_request, &dst);
    if (FAILED(hr)) {
        SetError("IAudioRenderClient::GetBuffer failed: " + HResultString(hr));
        return false;
    }

    if (silence_only) {
        hr = render_client_->ReleaseBuffer(frames_to_request,
                                           AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(hr)) {
            SetError("IAudioRenderClient::ReleaseBuffer failed: " + HResultString(hr));
            return false;
        }
        played_frames_ += frames_to_request;
        WakeAudioThread();
        return true;
    }

    std::uint32_t written_frames = 0;
    bool wrote_silence = false;
    while (written_frames < frames_to_request) {
        if (active_frame_offset >= active_chunk.nb_samples) {
            active_chunk = {};
            active_frame_offset = 0;
            if (pcm_queue_ && pcm_queue_->pop(active_chunk)) {
                queued_frames_ -= active_chunk.nb_samples;
                queued_chunks_ -= 1;
            }
        }

        if (active_frame_offset < active_chunk.nb_samples) {
            const auto frames_from_chunk = std::min<std::uint32_t>(
                frames_to_request - written_frames,
                active_chunk.nb_samples - active_frame_offset);
            std::memcpy(
                dst + static_cast<std::size_t>(written_frames) * bytes_per_frame_,
                active_chunk.data.data() +
                    static_cast<std::size_t>(active_frame_offset) * bytes_per_frame_,
                static_cast<std::size_t>(frames_from_chunk) * bytes_per_frame_);
            active_frame_offset += frames_from_chunk;
            written_frames += frames_from_chunk;
            continue;
        }

        // queued_frames_ 是跨线程统计值，极端竞争下可能比实际可 pop 的 chunk
        // 略乐观。若已经向 WASAPI 申请了 buffer，就把尾部补零保证本次提交有效，
        // 但这只覆盖本次申请的小范围，不再吞掉整个设备可写空间。
        const auto silence_frames = frames_to_request - written_frames;
        std::memset(dst + static_cast<std::size_t>(written_frames) * bytes_per_frame_,
                    0,
                    static_cast<std::size_t>(silence_frames) * bytes_per_frame_);
        written_frames += silence_frames;
        wrote_silence = true;
        ++underruns_;
    }

    hr = render_client_->ReleaseBuffer(frames_to_request, 0);
    if (FAILED(hr)) {
        SetError("IAudioRenderClient::ReleaseBuffer failed: " + HResultString(hr));
        return false;
    }

    played_frames_ += frames_to_request;
    if (wrote_silence) {
        WakeAudioThread();
    }
    return true;
}

bool WasapiAudioRenderer::EnqueuePcm(filter::audio::PcmFrame&& pcm) {
    if (pcm.Empty() || pcm.sample_rate != sample_rate_ ||
        pcm.channels != channels_ || pcm.BytesPerFrame() != bytes_per_frame_) {
        SetError("PCM frame does not match WASAPI mix format");
        return false;
    }

    PcmChunk chunk;
    chunk.data = std::move(pcm.data);
    chunk.nb_samples = static_cast<std::uint32_t>(pcm.nb_samples);
    chunk.pts_us = pcm.pts_us;
    chunk.duration_us = pcm.duration_us;
    const auto chunk_frames = chunk.nb_samples;

    submitted_frames_ += chunk_frames;

    if (!pcm_queue_) {
        DropUnqueuedChunkStats(chunk);
        return true;
    }

    if (pcm_queue_->try_push(chunk)) {
        queued_frames_ += chunk_frames;
        queued_chunks_ += 1;
        WakeAudioThread();
        return true;
    }

    PcmChunk dropped;
    if (pcm_queue_->pop(dropped)) {
        DropQueuedChunkStats(dropped);
    }

    if (pcm_queue_->try_push(std::move(chunk))) {
        queued_frames_ += chunk_frames;
        queued_chunks_ += 1;
        WakeAudioThread();
        return true;
    }

    dropped_frames_ += chunk_frames;
    dropped_chunks_ += 1;
    return true;
}

void WasapiAudioRenderer::DropQueuedChunkStats(const PcmChunk& chunk) {
    dropped_frames_ += chunk.nb_samples;
    dropped_chunks_ += 1;
    queued_frames_ -= chunk.nb_samples;
    queued_chunks_ -= 1;
}

void WasapiAudioRenderer::DropUnqueuedChunkStats(const PcmChunk& chunk) {
    dropped_frames_ += chunk.nb_samples;
    dropped_chunks_ += 1;
}

int WasapiAudioRenderer::CurrentPaddingFrames() const {
    if (!audio_client_) {
        return 0;
    }
    UINT32 padding = 0;
    const HRESULT hr = audio_client_->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
        return -1;
    }
    return static_cast<int>(padding);
}

void WasapiAudioRenderer::WakeAudioThread() {
    if (buffer_event_) {
        SetEvent(static_cast<HANDLE>(buffer_event_));
    }
}

void WasapiAudioRenderer::SetError(std::string message) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = std::move(message);
}

void WasapiAudioRenderer::ReleaseResources() {
    StopAudioThread();
    if (audio_client_ && started_) {
        audio_client_->Stop();
    }
    started_ = false;
    initialized_ = false;

    resampler_.Close();
    SafeRelease(render_client_);
    if (mix_format_) {
        CoTaskMemFree(mix_format_);
        mix_format_ = nullptr;
    }
    SafeRelease(audio_client_);
    SafeRelease(device_);
    SafeRelease(enumerator_);
    if (buffer_event_) {
        CloseHandle(static_cast<HANDLE>(buffer_event_));
        buffer_event_ = nullptr;
    }
    if (com_initialized_) {
        CoUninitialize();
        com_initialized_ = false;
    }

    if (pcm_queue_) {
        pcm_queue_->clear();
        pcm_queue_.reset();
    }
    submitted_frames_ = 0;
    queued_frames_ = 0;
    queued_chunks_ = 0;
    played_frames_ = 0;
    dropped_frames_ = 0;
    dropped_chunks_ = 0;
    underruns_ = 0;
    first_pts_us_ = 0;
    first_pts_set_ = false;
    sample_rate_ = 0;
    channels_ = 0;
    bytes_per_frame_ = 0;
    buffer_frame_count_ = 0;
}

} // namespace render::audio
