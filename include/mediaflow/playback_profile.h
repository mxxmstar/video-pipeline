#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "mediaflow/media_pipeline_config.h"

namespace mediaflow {

/// @brief 面向业务调用方的播放模式。
///
/// LowLatencyPreview 优先保持实时性，StablePlayback 优先吸收短时网络和调度
/// 抖动，Custom 用于调用方在预设值基础上明确覆盖缓存参数。
enum class PlaybackMode {
    LowLatencyPreview,
    StablePlayback,
    Custom,
};

/// @brief 音视频播放策略的公开配置。
///
/// 调用方只需要选择模式，并按业务允许的延迟调整 playback_buffer_ms。该配置会
/// 统一生成 MediaFlow 队列预算；渲染模块再使用同一配置生成 RenderSessionConfig，
/// 避免音频缓存已经放宽而视频队列仍保持低延时容量。
struct PlaybackProfile {
    /// 当前预设的语义标识。调用方应通过工厂函数取得对应默认值。
    PlaybackMode mode{PlaybackMode::LowLatencyPreview};

    /// 设备 buffer 之外允许保留的额外播放缓存。
    int playback_buffer_ms{120};

    /// 压缩包入口用于吸收网络到达抖动的媒体时间窗口。
    int network_jitter_buffer_ms{250};

    /// WASAPI 等音频设备的目标 buffer 时长。
    int audio_device_buffer_ms{100};

    /// 队列容量自动推导时使用的预估视频帧率。
    int expected_video_frame_rate{25};

    /// 0 表示按有效播放缓存和预估帧率推导；非 0 时覆盖视频帧队列容量。
    std::size_t max_video_queue_frames{0};

    /// 0 表示按有效播放缓存推导；非 0 时覆盖音频帧队列容量。
    std::size_t max_audio_queue_frames{0};

    /// @brief 创建保持实时性的预览配置。
    static PlaybackProfile LowLatencyPreview() {
        return {};
    }

    /// @brief 创建优先吸收短时抖动的稳定播放配置。
    static PlaybackProfile StablePlayback() {
        PlaybackProfile profile;
        profile.mode = PlaybackMode::StablePlayback;
        profile.playback_buffer_ms = 400;
        profile.network_jitter_buffer_ms = 1'000;
        profile.audio_device_buffer_ms = 200;
        return profile;
    }

    /// @brief 创建可由调用方覆盖各字段的自定义配置。
    static PlaybackProfile Custom() {
        PlaybackProfile profile;
        profile.mode = PlaybackMode::Custom;
        return profile;
    }

    /// @brief 配置值是否能生成有效的队列和渲染参数。
    bool IsValid() const {
        return playback_buffer_ms >= 0 && network_jitter_buffer_ms >= 0 &&
               audio_device_buffer_ms > 0 && expected_video_frame_rate > 0;
    }

    /// @brief 音频设备 buffer 加上额外播放缓存后的有效播放窗口。
    int EffectivePlaybackBufferMs() const {
        const auto device_buffer = std::max(0, audio_device_buffer_ms);
        const auto playback_buffer = std::max(0, playback_buffer_ms);
        const auto max_value = (std::numeric_limits<int>::max)();
        if (device_buffer > max_value - playback_buffer) {
            return max_value;
        }
        return device_buffer + playback_buffer;
    }

    /// @brief 根据播放窗口推导视频渲染队列容量。
    std::size_t ResolveVideoQueueFrames() const {
        if (max_video_queue_frames != 0) {
            return max_video_queue_frames;
        }

        const auto frames = static_cast<std::size_t>(
            (static_cast<std::int64_t>(EffectivePlaybackBufferMs()) *
                 std::max(1, expected_video_frame_rate) +
             999) /
            1'000);
        // 一个额外帧覆盖调度相位差，同时保持原有低延时队列的最小容量。
        return std::max<std::size_t>(6, frames + 1);
    }

    /// @brief 根据播放窗口推导 RenderSession 的音频帧队列容量。
    std::size_t ResolveAudioQueueFrames() const {
        if (max_audio_queue_frames != 0) {
            return max_audio_queue_frames;
        }

        // AAC/PCM 回调通常接近 20 ms 一帧；50 帧保留现有低延时默认值的余量。
        const auto frames = static_cast<std::size_t>(
            (EffectivePlaybackBufferMs() + 19) / 20);
        return std::max<std::size_t>(50, frames + 8);
    }

    /// @brief 根据播放窗口推导 WASAPI 内部 PCM chunk 队列容量。
    std::size_t ResolveAudioPcmQueueChunks() const {
        const auto chunks = static_cast<std::size_t>(
            (EffectivePlaybackBufferMs() + 19) / 20);
        return std::max<std::size_t>(16, chunks + 8);
    }

    /// @brief 生成与本播放策略一致的 MediaFlow 四条媒体边配置。
    MediaFlowPipelineConfig MakePipelineConfig() const {
        MediaFlowPipelineConfig config;
        const auto jitter_us = static_cast<std::int64_t>(
            std::max(0, network_jitter_buffer_ms)) * 1'000;
        const auto playback_us = static_cast<std::int64_t>(
            EffectivePlaybackBufferMs()) * 1'000;

        // 压缩包队列处理网络到达抖动；解码帧队列只承接即将播放的媒体时间。
        config.video_packet.max_span_us = jitter_us;
        config.audio_packet.max_span_us = jitter_us;
        config.video_frame.max_span_us = std::max<std::int64_t>(
            250'000, playback_us);
        config.audio_frame.max_span_us = std::max<std::int64_t>(
            500'000, playback_us);
        config.video_frame.max_items = std::max<std::size_t>(
            8, ResolveVideoQueueFrames());
        config.audio_frame.max_items = std::max<std::size_t>(
            64, ResolveAudioQueueFrames());
        return config;
    }
};

} // namespace mediaflow
