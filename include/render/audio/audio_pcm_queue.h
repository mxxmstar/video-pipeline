#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>

#include "common/queue/mpmc_queue.h"
#include "render/audio/audio_render_stats.h"
#include "render/audio/pcm_chunk.h"

namespace render::audio {

/// @brief 音频渲染层的有界 PCM 队列和统计器。
///
/// 这个类只关心“已经转换到设备格式的 PCM chunk 如何排队、溢出时如何丢弃、
/// 播放了多少帧、是否发生 underrun”。它不调用 WASAPI，也不依赖具体音频设备。
/// 这样播放策略可以留在 render/audio 层，而 WasapiAudioRenderer 只负责设备 API。
class AudioPcmQueue {
public:
    AudioPcmQueue() = default;

    /// @brief 重置队列容量和全部统计。
    void Reset(std::size_t capacity_chunks);

    /// @brief 清空队列并归零统计，用于 renderer 停机或重新初始化。
    void Clear();

    /// @brief 写入一个 PCM chunk；队列满时丢弃最旧 chunk，再写入新 chunk。
    ///
    /// 预览场景优先实时性，因此满队列时不阻塞上游。返回 true 表示调用流程
    /// 可以继续；即使发生丢弃，也会通过 GetStats() 暴露。
    bool Enqueue(PcmChunk&& chunk);

    /// @brief 尝试取出一个 PCM chunk 供设备线程写入。
    bool Pop(PcmChunk& chunk);

    /// @brief 记录一段已经成功提交给 WASAPI 的真实 PCM。
    ///
    /// @param pts_us 该段第一个采样帧对应的媒体 PTS，允许为 kNoTimestamp。
    ///
    /// 设备播放位置不能再只依赖“首个 PTS + 累计帧数”：补静音、丢弃旧 chunk 或
    /// 上游时间戳跳变都会使这个假设失效。WASAPI 写线程应在每次 ReleaseBuffer
    /// 成功后按实际写入顺序调用本接口，保留可查询的分块时间线。
    void RecordDeviceMediaFrames(std::uint32_t frames, std::int64_t pts_us);

    /// @brief 记录一段已经成功提交给 WASAPI 的补静音帧。
    ///
    /// 静音没有源媒体 PTS；播放位置落在该段时会保留最后一个已知媒体位置，而不是
    /// 伪造连续 PTS 并掩盖音频 underrun。
    void RecordDeviceSilenceFrames(std::uint32_t frames);

    /// @brief 记录一次 PCM 不足导致的短静音补偿。
    void AddUnderrun();

    /// @brief 当前排队的 PCM 帧数近似值，用于设备填充策略估算本次可写量。
    std::int64_t QueuedFrames() const;

    /// @brief 根据分块设备时间线和当前 device padding 返回已播放媒体 PTS。
    ///
    /// 在第一段媒体 PCM 尚未到达实际播放位置，或当前位置只对应补静音且没有历史媒体
    /// PTS 时返回 kNoTimestamp。0 是合法的媒体起点，调用方必须只按哨兵判断无效。
    std::int64_t PlayedPtsUs(int sample_rate, int padding_frames) const;

    /// @brief 返回一份线程安全统计快照。
    AudioRenderStats GetStats() const;

private:
    struct DeviceTimelineSegment {
        std::uint64_t start_frame{0};
        std::uint64_t end_frame{0};
        std::int64_t pts_us{0};
        bool has_media_pts{false};
    };

    void RecordDeviceFrames(std::uint32_t frames,
                            std::int64_t pts_us,
                            bool has_media_pts);

    void DropQueuedChunkStats(const PcmChunk& chunk);
    void DropUnqueuedChunkStats(const PcmChunk& chunk);

    std::unique_ptr<BoundedMpmcQueue<PcmChunk>> queue_;
    std::mutex queue_mutex_;
    std::int64_t capacity_chunks_{0};
    std::atomic<std::int64_t> submitted_frames_{0};
    std::atomic<std::int64_t> queued_frames_{0};
    std::atomic<std::int64_t> queued_chunks_{0};
    /// 已成功写入设备的帧数，包含真实 PCM 和补静音；配合 WASAPI padding 可计算
    /// 当前物理播放位置在 device_timeline_ 中的 frame offset。
    std::atomic<std::uint64_t> device_written_frames_{0};
    std::atomic<std::int64_t> dropped_frames_{0};
    std::atomic<std::int64_t> dropped_chunks_{0};
    std::atomic<std::int64_t> underruns_{0};
    mutable std::mutex timeline_mutex_;
    mutable std::deque<DeviceTimelineSegment> device_timeline_;
    mutable std::int64_t last_played_pts_us_{0};
    mutable bool has_last_played_pts_{false};
};

} // namespace render::audio
