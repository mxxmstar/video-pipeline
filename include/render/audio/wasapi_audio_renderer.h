#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "filter/audio/audio_resample_filter.h"
#include "render/audio/audio_buffer_fill_policy.h"
#include "render/audio/audio_pcm_queue.h"
#include "render/audio/i_audio_renderer.h"
#include "render/audio/pcm_chunk.h"

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

    /// @brief 初始化默认 WASAPI 播放设备、重采样器、PCM 队列和设备写入线程。
    ///
    /// Init() 必须在拥有该 renderer 的音频喂帧线程中调用。COM、IAudioClient 等
    /// WASAPI 对象在此线程创建；真正写 shared buffer 的动作由内部 audio_thread_
    /// 完成，但资源释放仍由 Shutdown()/ReleaseResources() 统一收口。
    bool Init(const AudioRenderConfig& config) override;

    /// @brief 接收一帧解码后的音频，重采样为 WASAPI mix format 后快速入队。
    ///
    /// Render() 不直接写声卡，它只完成格式适配和 PCM 入队；这样 RenderSession 的
    /// audio worker 不会被声卡 buffer 可写状态阻塞。
    bool Render(const MediaFrame& frame) override;

    /// @brief 停止设备线程并释放所有 WASAPI/COM/PCM 队列资源。
    void Shutdown() override;

    /// @brief 估算当前已经播放到的音频 PTS。
    ///
    /// 基于 AudioPcmQueue 记录的首帧 PTS、已提交给设备的帧数，以及 WASAPI 当前
    /// padding 计算。该值后续可作为阶段 4 的音频 master clock 基础。
    int64_t PlayedPtsUs() const override;

    /// @brief 获取 PCM 队列、丢帧、underrun 等音频播放诊断统计。
    AudioRenderStats GetStats() const override;

    /// @brief 最近一次初始化、重采样或 WASAPI 调用失败原因。
    std::string LastError() const;

private:
    /// @brief 初始化当前线程的 COM apartment。
    ///
    /// 如果宿主已经用其他 apartment 初始化 COM，会继续沿用现有状态；只有当前类
    /// 自己成功初始化 COM 时，ReleaseResources() 才会调用 CoUninitialize()。
    bool InitializeCom();

    /// @brief 打开默认播放设备并初始化 WASAPI shared-mode event callback 流。
    ///
    /// 该函数负责创建 IMMDeviceEnumerator、默认 render endpoint、IAudioClient、
    /// mix format、事件句柄、buffer size 和 IAudioRenderClient。
    bool InitializeDevice();

    /// @brief 按 WASAPI mix format 打开 swresample 过滤器。
    ///
    /// 重采样算法仍位于 filter/audio；这里仅把设备目标格式传给 filter。
    bool InitializeResampler();

    /// @brief 启动内部设备写入线程。
    bool StartAudioThread();

    /// @brief 请求内部设备写入线程退出，并等待线程结束。
    void StopAudioThread();

    /// @brief WASAPI 事件驱动的设备写入循环。
    ///
    /// 线程等待 buffer_event_，收到事件后查询可写帧数，并从 AudioPcmQueue 取 PCM
    /// 写入 IAudioRenderClient。该线程只处理设备 buffer，不做解码或重采样。
    void AudioThreadMain();

    /// @brief 向 WASAPI shared buffer 填充一小段 PCM 或短静音。
    ///
    /// active_chunk/active_frame_offset 用于保存上次没写完的 PCM chunk。实际申请
    /// 多少帧由 AudioBufferFillPolicy 决定，避免把短时缺口填成整段长静音。
    bool FillWasapiBuffer(PcmChunk& active_chunk,
                          std::uint32_t& active_frame_offset);

    /// @brief 把重采样结果转换为 PcmChunk 并交给 AudioPcmQueue。
    bool EnqueuePcm(filter::audio::PcmFrame&& pcm);

    /// @brief 查询 WASAPI shared buffer 中尚未播放的帧数。
    int CurrentPaddingFrames() const;

    /// @brief 主动唤醒设备写入线程。
    ///
    /// 新 PCM 入队、Stop()、或补短静音后都可以唤醒线程，让它尽快重新检查设备
    /// 可写空间。
    void WakeAudioThread();

    /// @brief 线程安全地记录最近一次错误。
    void SetError(std::string message);

    /// @brief 停止线程、关闭设备、释放 COM 对象并清空策略状态。
    void ReleaseResources();

    /// 配置由 Init() 保存，后续初始化设备、队列容量和 buffer 时长都会读取它。
    AudioRenderConfig config_;

    /// 纯数据转换组件：把任意解码音频帧转换成 WASAPI mix format。
    filter::audio::AudioResampleFilter resampler_;

    /// render/audio 策略层组件：负责 PCM chunk 排队、溢出丢弃和播放统计。
    AudioPcmQueue pcm_queue_;

    /// render/audio 策略层组件：负责决定每次向设备申请多少帧，以及何时短静音。
    AudioBufferFillPolicy fill_policy_;

    /// 内部设备写入线程；只等待 WASAPI event 并写 shared buffer。
    std::thread audio_thread_;

    /// WASAPI 设备枚举器，用于查找系统默认播放设备。
    IMMDeviceEnumerator* enumerator_{nullptr};

    /// 系统默认 render endpoint。
    IMMDevice* device_{nullptr};

    /// WASAPI 音频客户端，负责初始化 shared-mode stream、启动/停止和查询 padding。
    IAudioClient* audio_client_{nullptr};

    /// WASAPI 渲染客户端，负责 GetBuffer()/ReleaseBuffer() 写入 shared buffer。
    IAudioRenderClient* render_client_{nullptr};

    /// WASAPI 返回的 mix format，内存由 CoTaskMemFree() 释放。
    void* mix_format_{nullptr};

    /// event callback 模式下的唤醒事件，交给 IAudioClient::SetEventHandle()。
    void* buffer_event_{nullptr};

    /// 通知 audio_thread_ 退出的原子标志。
    std::atomic<bool> stop_audio_thread_{false};

    /// WASAPI mix format 中的采样率。
    int sample_rate_{0};

    /// WASAPI mix format 中的声道数。
    int channels_{0};

    /// 单个采样帧的字节数，等于 block align。
    int bytes_per_frame_{0};

    /// WASAPI shared buffer 总帧数。
    std::uint32_t buffer_frame_count_{0};

    /// 当前线程是否由本类成功 CoInitializeEx()。
    bool com_initialized_{false};

    /// IAudioClient::Start() 是否已经成功，用于停机时判断是否需要 Stop()。
    bool started_{false};

    /// 对外表示 renderer 是否已经完成初始化并可接收 Render()。
    bool initialized_{false};

    /// 保护 last_error_ 的互斥锁。
    mutable std::mutex error_mutex_;

    /// 最近一次错误文本。
    std::string last_error_;
};

} // namespace render::audio
