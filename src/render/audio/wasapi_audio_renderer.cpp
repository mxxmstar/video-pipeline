#include "render/audio/wasapi_audio_renderer.h"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstring>
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
    // COM 接口对象都用 Release() 释放。这里统一把指针置空，避免重复释放。
    if (ptr) {
        ptr->Release();
        ptr = nullptr;
    }
}

std::string HResultString(HRESULT hr) {
    // 只保留 HRESULT 十六进制值，便于和微软文档/调试器错误码对照。
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(hr));
    return buffer;
}

bool IsExtensibleSubFormat(const WAVEFORMATEX* format, const GUID& guid) {
    // WASAPI mix format 常见 WAVE_FORMAT_EXTENSIBLE。真实采样格式需要看
    // WAVEFORMATEXTENSIBLE::SubFormat，而不是只看 wFormatTag。
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

    // 当前 renderer 第一版只支持 WASAPI 常见的 float32 和 signed 16-bit PCM。
    // 如果系统 mix format 是其他格式，应在这里显式返回 Unknown，让 Init() 失败。
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
    // 对 extensible format 优先使用 Windows 声道掩码。FFmpeg swresample 可以
    // 接受该布局值，避免 mono/stereo 以外的设备布局被误判。
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
    // Init 支持重复调用，因此先完整释放旧设备和旧线程，再保存新配置。
    Shutdown();
    config_ = config;
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        last_error_.clear();
    }

    // 初始化顺序很重要：
    // 1. COM 是 MMDeviceEnumerator/AudioClient 的基础。
    // 2. 设备初始化后才能拿到 mix format。
    // 3. 重采样器需要根据 mix format 配置目标采样率/声道/格式。
    if (!InitializeCom() || !InitializeDevice() || !InitializeResampler()) {
        ReleaseResources();
        return false;
    }

    // PCM 队列是 render/audio 策略层，不关心 WASAPI API；容量来自播放配置。
    pcm_queue_.Reset(static_cast<std::size_t>(
        std::max(2, config_.queue_capacity_chunks)));

    // shared-mode event callback 流启动前，先把整个设备 buffer 标记为静音。
    // 这样 audio_client_->Start() 后即使第一批 PCM 还没到，也不会播放未初始化数据。
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

    // 先启动内部设备线程，再 Start() WASAPI。Start() 后设备会开始触发事件，
    // 线程已经就绪可以立即响应。
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

    // Render() 所在的是 RenderSession 的 audio worker 线程。这里只做 CPU
    // 重采样和入队，不直接等待声卡可写空间，避免被 WASAPI 设备节奏反向阻塞。
    filter::audio::PcmFrame pcm;
    if (!resampler_.Process(frame, pcm)) {
        SetError("audio resample failed: " + resampler_.LastError());
        return false;
    }

    return EnqueuePcm(std::move(pcm));
}

void WasapiAudioRenderer::Shutdown() {
    ReleaseResources();
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_.clear();
}

int64_t WasapiAudioRenderer::PlayedPtsUs() const {
    if (!initialized_ || sample_rate_ <= 0) {
        return 0;
    }

    // WASAPI padding 表示已经提交但尚未播放的设备帧数。AudioPcmQueue 用
    // played_frames - padding 估算真正出声的位置。
    return pcm_queue_.PlayedPtsUs(sample_rate_, CurrentPaddingFrames());
}

AudioRenderStats WasapiAudioRenderer::GetStats() const {
    return pcm_queue_.GetStats();
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
    // MMDeviceEnumerator 是 Windows Core Audio 获取默认设备的入口。
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator),
                                  nullptr,
                                  CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr)) {
        SetError("CoCreateInstance(MMDeviceEnumerator) failed: " + HResultString(hr));
        return false;
    }

    // eRender/eConsole 表示系统默认控制台播放设备，符合本地预览的第一版需求。
    // 后续如果需要选择设备，应把 endpoint id 暴露到 AudioRenderConfig。
    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr)) {
        SetError("GetDefaultAudioEndpoint failed: " + HResultString(hr));
        return false;
    }

    // IAudioClient 是 WASAPI stream 的核心对象。IAudioRenderClient 需要后面
    // 通过 GetService() 从它获取。
    hr = device_->Activate(__uuidof(IAudioClient),
                           CLSCTX_ALL,
                           nullptr,
                           reinterpret_cast<void**>(&audio_client_));
    if (FAILED(hr)) {
        SetError("IMMDevice::Activate(IAudioClient) failed: " + HResultString(hr));
        return false;
    }

    // mix format 是 shared mode 下系统混音器期望的格式。我们不强制设备使用
    // 输入流格式，而是通过 AudioResampleFilter 把解码帧转换到 mix format。
    WAVEFORMATEX* mix_format = nullptr;
    hr = audio_client_->GetMixFormat(&mix_format);
    if (FAILED(hr) || !mix_format) {
        SetError("IAudioClient::GetMixFormat failed: " + HResultString(hr));
        return false;
    }
    mix_format_ = mix_format;

    // block align 等于“一帧采样”的字节数，例如 stereo float32 是 8 字节。
    // 后续 memcpy 到 WASAPI buffer 时用它从 frame 数换算 byte 数。
    sample_rate_ = static_cast<int>(mix_format->nSamplesPerSec);
    channels_ = static_cast<int>(mix_format->nChannels);
    bytes_per_frame_ = static_cast<int>(mix_format->nBlockAlign);
    if (SampleFormatFromWaveFormat(mix_format) == SampleFormat::Unknown ||
        sample_rate_ <= 0 || channels_ <= 0 || bytes_per_frame_ <= 0) {
        SetError("unsupported WASAPI mix format");
        return false;
    }

    // event callback 模式下，WASAPI 会在设备可写时 signal 这个 event。
    // 我们也复用它做手动唤醒，例如新 PCM 入队或 Stop()。
    buffer_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!buffer_event_) {
        SetError("CreateEventW failed");
        return false;
    }

    // WASAPI 使用 100ns 为单位的 REFERENCE_TIME。shared mode 下实际 buffer
    // 大小可能由系统调整，最终值以 GetBufferSize() 为准。
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

    // 把 event 绑定给 IAudioClient 后，设备线程才能用 WaitForSingleObject()
    // 等待声卡需要更多数据。
    hr = audio_client_->SetEventHandle(static_cast<HANDLE>(buffer_event_));
    if (FAILED(hr)) {
        SetError("IAudioClient::SetEventHandle failed: " + HResultString(hr));
        return false;
    }

    // 获取最终 shared buffer 总帧数，用于根据 padding 计算本轮 available_frames。
    hr = audio_client_->GetBufferSize(&buffer_frame_count_);
    if (FAILED(hr) || buffer_frame_count_ == 0) {
        SetError("IAudioClient::GetBufferSize failed: " + HResultString(hr));
        return false;
    }

    // IAudioRenderClient 提供真正写 shared buffer 的 GetBuffer()/ReleaseBuffer()。
    hr = audio_client_->GetService(__uuidof(IAudioRenderClient),
                                   reinterpret_cast<void**>(&render_client_));
    if (FAILED(hr)) {
        SetError("IAudioClient::GetService(IAudioRenderClient) failed: " + HResultString(hr));
        return false;
    }
    return true;
}

bool WasapiAudioRenderer::InitializeResampler() {
    // 这里故意只创建纯 filter/audio 组件，不把设备逻辑下沉到 filter。
    // filter 负责格式转换；render/audio 负责何时播放、怎么排队和怎么写设备。
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
        // 设备写入线程内部等待 WASAPI event，因此不会像 busy loop 那样持续占 CPU。
        audio_thread_ = std::thread(&WasapiAudioRenderer::AudioThreadMain, this);
    } catch (const std::system_error& error) {
        SetError(std::string("failed to start WASAPI audio thread: ") + error.what());
        return false;
    }
    return true;
}

void WasapiAudioRenderer::StopAudioThread() {
    // Stop 时主动 signal event，确保设备线程即使正阻塞在 WaitForSingleObject()
    // 也能及时醒来看到 stop_audio_thread_。
    stop_audio_thread_.store(true);
    WakeAudioThread();
    if (audio_thread_.joinable()) {
        audio_thread_.join();
    }
}

void WasapiAudioRenderer::AudioThreadMain() {
    // WASAPI 相关 COM 调用可能发生在设备写入线程，因此线程入口也尝试初始化 COM。
    // 如果宿主已有 apartment，这里失败也不阻断退出路径；只有成功时才 CoUninitialize。
    const HRESULT com_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool thread_com_initialized = SUCCEEDED(com_hr);

    // active_chunk 保存上一次没有写完的 PCM chunk。WASAPI 每次可写帧数不一定
    // 刚好等于一个音频包大小，所以需要跨事件保留 offset。
    PcmChunk active_chunk;
    std::uint32_t active_frame_offset = 0;
    while (!stop_audio_thread_.load()) {
        // 正常情况下由 WASAPI event 唤醒；20ms 超时是兜底，避免某些驱动没有
        // 持续 signal 时线程永久睡眠。
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
    // padding 是设备 buffer 中已经排队但尚未播放的帧数。可写帧数 =
    // buffer 总帧数 - padding。
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

    // 先计算 active_chunk 还剩多少帧，再交给策略类决定本轮最多申请多少帧。
    // 这一步把“播放策略”从 WASAPI API 调用中拆出去。
    const auto active_remaining =
        active_frame_offset < active_chunk.nb_samples
            ? active_chunk.nb_samples - active_frame_offset
            : 0;
    const AudioBufferFillContext fill_context{
        available_frames,
        active_remaining,
        pcm_queue_.QueuedFrames(),
        sample_rate_,
    };
    const auto fill_request = fill_policy_.Plan(fill_context);
    if (fill_request.frames_to_request == 0) {
        return true;
    }

    // GetBuffer() 申请的帧数必须和后续 ReleaseBuffer() 一致。这里申请的是
    // fill_policy_ 决定的帧数，而不是盲目申请全部 available_frames。
    std::uint8_t* dst = nullptr;
    HRESULT hr = render_client_->GetBuffer(fill_request.frames_to_request, &dst);
    if (FAILED(hr)) {
        SetError("IAudioRenderClient::GetBuffer failed: " + HResultString(hr));
        return false;
    }

    if (fill_request.silence_only) {
        // 完全没有 PCM 时，策略只要求写一小段短静音。使用 WASAPI 的
        // AUDCLNT_BUFFERFLAGS_SILENT 标记，避免手动 memset 整块 buffer。
        hr = render_client_->ReleaseBuffer(fill_request.frames_to_request,
                                           AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(hr)) {
            SetError("IAudioRenderClient::ReleaseBuffer failed: " + HResultString(hr));
            return false;
        }
        pcm_queue_.AddUnderrun();
        pcm_queue_.AddPlayedFrames(fill_request.frames_to_request);
        WakeAudioThread();
        return true;
    }

    std::uint32_t written_frames = 0;
    bool wrote_silence = false;
    while (written_frames < fill_request.frames_to_request) {
        if (active_frame_offset >= active_chunk.nb_samples) {
            // 当前 chunk 已写完，尝试从策略队列取下一块 PCM。取不到时 active_chunk
            // 保持空，下面会按“小范围兜底静音”处理。
            active_chunk = {};
            active_frame_offset = 0;
            pcm_queue_.Pop(active_chunk);
        }

        if (active_frame_offset < active_chunk.nb_samples) {
            // 按 frame 计算本次从 active_chunk 拷贝多少，再用 bytes_per_frame_
            // 转换为字节偏移。这样 S16/FLT、mono/stereo 都能统一处理。
            const auto frames_from_chunk = std::min<std::uint32_t>(
                fill_request.frames_to_request - written_frames,
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

        // AudioPcmQueue 暴露的是跨线程统计快照，极端竞争下可能比实际可 pop
        // 的 chunk 略乐观。若已经向 WASAPI 申请了 buffer，就把尾部补零保证
        // 本次提交有效；补零范围仍被 fill policy 限制，不会吞掉整个可写空间。
        const auto silence_frames = fill_request.frames_to_request - written_frames;
        std::memset(dst + static_cast<std::size_t>(written_frames) * bytes_per_frame_,
                    0,
                    static_cast<std::size_t>(silence_frames) * bytes_per_frame_);
        written_frames += silence_frames;
        wrote_silence = true;
        pcm_queue_.AddUnderrun();
    }

    // ReleaseBuffer(flags=0) 表示这批 buffer 中包含真实 PCM 数据。如果尾部
    // 因极端竞争补了少量 0，也仍作为普通 PCM 提交。
    hr = render_client_->ReleaseBuffer(fill_request.frames_to_request, 0);
    if (FAILED(hr)) {
        SetError("IAudioRenderClient::ReleaseBuffer failed: " + HResultString(hr));
        return false;
    }

    pcm_queue_.AddPlayedFrames(fill_request.frames_to_request);
    if (wrote_silence) {
        WakeAudioThread();
    }
    return true;
}

bool WasapiAudioRenderer::EnqueuePcm(filter::audio::PcmFrame&& pcm) {
    // 重采样器的输出必须与 WASAPI mix format 一致。这里再做一次运行期校验，
    // 防止配置错误或后续 filter 改动导致写设备时发生字节布局错配。
    if (pcm.Empty() || pcm.sample_rate != sample_rate_ ||
        pcm.channels != channels_ || pcm.BytesPerFrame() != bytes_per_frame_) {
        SetError("PCM frame does not match WASAPI mix format");
        return false;
    }

    // PcmFrame 是 filter/audio 的纯数据结构；进入 render/audio 队列前转换为
    // PcmChunk，后续策略层只看 chunk，不依赖 filter 的实现细节。
    PcmChunk chunk;
    chunk.data = std::move(pcm.data);
    chunk.nb_samples = static_cast<std::uint32_t>(pcm.nb_samples);
    chunk.pts_us = pcm.pts_us;
    chunk.duration_us = pcm.duration_us;
    const bool queued = pcm_queue_.Enqueue(std::move(chunk));
    WakeAudioThread();
    return queued;
}

int WasapiAudioRenderer::CurrentPaddingFrames() const {
    if (!audio_client_) {
        return 0;
    }
    // GetCurrentPadding() 失败时返回 -1，让 FillWasapiBuffer() 统一转成错误。
    UINT32 padding = 0;
    const HRESULT hr = audio_client_->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
        return -1;
    }
    return static_cast<int>(padding);
}

void WasapiAudioRenderer::WakeAudioThread() {
    // buffer_event_ 是 auto-reset event。SetEvent() 后最多唤醒一个等待线程，
    // 正好对应当前类唯一的 audio_thread_。
    if (buffer_event_) {
        SetEvent(static_cast<HANDLE>(buffer_event_));
    }
}

void WasapiAudioRenderer::SetError(std::string message) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = std::move(message);
}

void WasapiAudioRenderer::ReleaseResources() {
    // 释放顺序从“运行中资源”到“底层 COM 对象”：
    // 先停线程，避免线程继续访问 render_client_/audio_client_；
    // 再 Stop() 音频流；最后释放 COM 接口和事件句柄。
    StopAudioThread();
    if (audio_client_ && started_) {
        audio_client_->Stop();
    }
    started_ = false;
    initialized_ = false;

    // 重采样器可能持有 FFmpeg swr context，必须在重新初始化前关闭。
    resampler_.Close();
    SafeRelease(render_client_);
    if (mix_format_) {
        // GetMixFormat() 返回的 WAVEFORMATEX 内存由 COM 分配，必须用 CoTaskMemFree。
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
        // 只有当前对象成功初始化过 COM 时才反初始化，避免破坏宿主已有 COM apartment。
        CoUninitialize();
        com_initialized_ = false;
    }

    // 策略层状态也在这里清空，保证下一次 Init() 不继承旧播放统计和旧 PCM。
    pcm_queue_.Clear();
    sample_rate_ = 0;
    channels_ = 0;
    bytes_per_frame_ = 0;
    buffer_frame_count_ = 0;
}

} // namespace render::audio
