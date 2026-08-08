#pragma once

/**
 * @file media_pipeline_config.h
 * @brief MediaFlow 媒体 Pipeline 的轨道队列配置。
 *
 * 该配置位于媒体 Pipeline 边界，而不是某个测试程序内部。设备或上层业务可以
 * 复制默认配置后只修改需要适配的轨道参数，再将生成的 EdgeOptions 交给 Graph。
 */

#include "mediaflow/core/types.h"

namespace mediaflow {

/// 一条媒体轨道的实时队列参数。
struct MediaFlowTrackQueueConfig {
    std::size_t max_items{0};
    std::uint64_t bitrate_bits_per_second{0};
    std::int64_t max_span_us{0};
    std::uint32_t burst_safety_percent{200};
    BackpressurePolicy backpressure{BackpressurePolicy::DropNewest};
    std::size_t max_batch_size{32};

    /// 根据媒体时长、码率和突发安全系数计算队列预算。
    QueueBudget MakeBudget() const {
        return QueueBudget::FromBitrate(
            max_items, bitrate_bits_per_second, max_span_us,
            burst_safety_percent);
    }

    /// 将轨道配置转换为 Graph 边配置，保证 items/bytes/span 使用同一组参数。
    EdgeOptions MakeEdgeOptions(QueueTrack track) const {
        EdgeOptions options;
        options.transport = TransportKind::Queue;
        options.capacity = max_items;
        options.backpressure = backpressure;
        options.max_batch_size = max_batch_size;
        options.budget = MakeBudget();
        options.track = track;
        return options;
    }
};

/// MediaFlow 双轨实时 Pipeline 的默认容量配置。
///
/// 压缩音频入口保留约 1 秒媒体时间，用于吸收 RTSP 接收线程的短时交付突发；
/// 解码后的音频帧仍限制为 500 ms，避免网络突发直接变成播放延迟。上层可以按
/// 设备网络特征覆盖 audio_packet.max_span_us 和对应的码率/突发系数。
struct MediaFlowPipelineConfig {
    MediaFlowTrackQueueConfig video_packet{
        256, 16'000'000, 1'000'000, 200,
        BackpressurePolicy::PreferVideoKeyframes, 64};
    MediaFlowTrackQueueConfig audio_packet{
        512, 384'000, 1'000'000, 200,
        BackpressurePolicy::DropOldest, 128};
    MediaFlowTrackQueueConfig video_frame{
        8, 0, 250'000, 200, BackpressurePolicy::DropOldest, 8};
    MediaFlowTrackQueueConfig audio_frame{
        64, 0, 500'000, 200, BackpressurePolicy::DropOldest, 32};

    /// 获取压缩包边对应的配置。
    EdgeOptions MakePacketEdgeOptions(QueueTrack track) const {
        if (track == QueueTrack::Video) {
            return video_packet.MakeEdgeOptions(track);
        }
        if (track == QueueTrack::Audio) {
            return audio_packet.MakeEdgeOptions(track);
        }
        return {};
    }

    /// 获取解码帧边对应的配置。
    EdgeOptions MakeFrameEdgeOptions(QueueTrack track) const {
        if (track == QueueTrack::Video) {
            return video_frame.MakeEdgeOptions(track);
        }
        if (track == QueueTrack::Audio) {
            return audio_frame.MakeEdgeOptions(track);
        }
        return {};
    }
};

} // namespace mediaflow
