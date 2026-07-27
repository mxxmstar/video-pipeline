#include "render/audio/wasapi_audio_renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
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
    last_error_.clear();

    if (!InitializeCom() || !InitializeDevice() || !InitializeResampler()) {
        ReleaseResources();
        return false;
    }

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
        first_pts_us_ = pcm.pts_us;
        first_pts_set_ = true;
    }
    return WritePcm(pcm);
}

void WasapiAudioRenderer::Shutdown() {
    ReleaseResources();
    last_error_.clear();
}

int64_t WasapiAudioRenderer::PlayedPtsUs() const {
    if (!initialized_ || sample_rate_ <= 0 || !first_pts_set_) {
        return 0;
    }

    const auto padding = CurrentPaddingFrames();
    const auto played_frames = std::max<int64_t>(0, submitted_frames_.load() - padding);
    return first_pts_us_ + played_frames * 1'000'000LL / sample_rate_;
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

bool WasapiAudioRenderer::WritePcm(const filter::audio::PcmFrame& pcm) {
    if (pcm.Empty() || pcm.sample_rate != sample_rate_ ||
        pcm.channels != channels_ || pcm.BytesPerFrame() != bytes_per_frame_) {
        SetError("PCM frame does not match WASAPI mix format");
        return false;
    }

    const auto* src = pcm.data.data();
    std::uint32_t remaining_frames = static_cast<std::uint32_t>(pcm.nb_samples);
    std::uint32_t frame_offset = 0;

    while (remaining_frames > 0) {
        const auto padding = CurrentPaddingFrames();
        if (padding < 0) {
            return false;
        }
        const auto available_frames =
            buffer_frame_count_ > static_cast<std::uint32_t>(padding)
                ? buffer_frame_count_ - static_cast<std::uint32_t>(padding)
                : 0;

        if (available_frames == 0) {
            WaitForSingleObject(static_cast<HANDLE>(buffer_event_), 20);
            continue;
        }

        const auto frames_to_write = std::min(remaining_frames, available_frames);
        std::uint8_t* dst = nullptr;
        HRESULT hr = render_client_->GetBuffer(frames_to_write, &dst);
        if (FAILED(hr)) {
            SetError("IAudioRenderClient::GetBuffer failed: " + HResultString(hr));
            return false;
        }

        std::memcpy(dst,
                    src + static_cast<std::size_t>(frame_offset) * bytes_per_frame_,
                    static_cast<std::size_t>(frames_to_write) * bytes_per_frame_);

        hr = render_client_->ReleaseBuffer(frames_to_write, 0);
        if (FAILED(hr)) {
            SetError("IAudioRenderClient::ReleaseBuffer failed: " + HResultString(hr));
            return false;
        }

        submitted_frames_ += frames_to_write;
        frame_offset += frames_to_write;
        remaining_frames -= frames_to_write;
    }
    return true;
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

void WasapiAudioRenderer::SetError(std::string message) {
    last_error_ = std::move(message);
}

void WasapiAudioRenderer::ReleaseResources() {
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

    submitted_frames_ = 0;
    first_pts_us_ = 0;
    first_pts_set_ = false;
    sample_rate_ = 0;
    channels_ = 0;
    bytes_per_frame_ = 0;
    buffer_frame_count_ = 0;
}

} // namespace render::audio
