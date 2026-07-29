#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
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

    /// @brief 记录设备已经提交播放的帧数，包括真实 PCM 和短静音补偿。
    void AddPlayedFrames(std::uint32_t frames);

    /// @brief 记录一次 PCM 不足导致的短静音补偿。
    void AddUnderrun();

    /// @brief 当前排队的 PCM 帧数近似值，用于设备填充策略估算本次可写量。
    std::int64_t QueuedFrames() const;

    /// @brief 根据首帧 PTS、已提交设备帧数和当前 device padding 估算播放 PTS。
    std::int64_t PlayedPtsUs(int sample_rate, int padding_frames) const;

    /// @brief 返回一份线程安全统计快照。
    AudioRenderStats GetStats() const;

private:
    void DropQueuedChunkStats(const PcmChunk& chunk);
    void DropUnqueuedChunkStats(const PcmChunk& chunk);

    std::unique_ptr<BoundedMpmcQueue<PcmChunk>> queue_;
    std::mutex queue_mutex_;
    std::int64_t capacity_chunks_{0};
    std::atomic<std::int64_t> submitted_frames_{0};
    std::atomic<std::int64_t> queued_frames_{0};
    std::atomic<std::int64_t> queued_chunks_{0};
    std::atomic<std::int64_t> played_frames_{0};
    std::atomic<std::int64_t> dropped_frames_{0};
    std::atomic<std::int64_t> dropped_chunks_{0};
    std::atomic<std::int64_t> underruns_{0};
    std::atomic<std::int64_t> first_pts_us_{0};
    std::atomic<bool> first_pts_set_{false};
    std::mutex first_pts_mutex_;
};

} // namespace render::audio
