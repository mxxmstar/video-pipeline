#include "render/audio/audio_pcm_queue.h"

#include <algorithm>
#include <limits>
#include <mutex>

#include "media/media_packet.h"

namespace render::audio {

void AudioPcmQueue::Reset(std::size_t capacity_chunks) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    capacity_chunks_ = static_cast<std::int64_t>(
        std::max<std::size_t>(2, capacity_chunks));
    queue_ = std::make_unique<BoundedMpmcQueue<PcmChunk>>(
        static_cast<std::size_t>(capacity_chunks_));
    submitted_frames_ = 0;
    queued_frames_ = 0;
    queued_chunks_ = 0;
    device_written_frames_ = 0;
    dropped_frames_ = 0;
    dropped_chunks_ = 0;
    underruns_ = 0;
    {
        std::lock_guard<std::mutex> timeline_lock(timeline_mutex_);
        device_timeline_.clear();
        last_played_pts_us_ = 0;
        has_last_played_pts_ = false;
    }
}

void AudioPcmQueue::Clear() {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_) {
        queue_->clear();
        queue_.reset();
    }
    capacity_chunks_ = 0;
    submitted_frames_ = 0;
    queued_frames_ = 0;
    queued_chunks_ = 0;
    device_written_frames_ = 0;
    dropped_frames_ = 0;
    dropped_chunks_ = 0;
    underruns_ = 0;
    {
        std::lock_guard<std::mutex> timeline_lock(timeline_mutex_);
        device_timeline_.clear();
        last_played_pts_us_ = 0;
        has_last_played_pts_ = false;
    }
}

bool AudioPcmQueue::Enqueue(PcmChunk&& chunk) {
    const auto chunk_frames = static_cast<std::int64_t>(chunk.nb_samples);
    submitted_frames_ += chunk_frames;

    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (!queue_) {
        DropUnqueuedChunkStats(chunk);
        return true;
    }

    while (queued_chunks_.load() >= capacity_chunks_) {
        PcmChunk dropped;
        if (!queue_->pop(dropped)) {
            break;
        }
        DropQueuedChunkStats(dropped);
    }

    if (queue_->push(std::move(chunk))) {
        queued_frames_ += chunk_frames;
        queued_chunks_ += 1;
        return true;
    }

    dropped_frames_ += chunk_frames;
    dropped_chunks_ += 1;
    return true;
}

bool AudioPcmQueue::Pop(PcmChunk& chunk) {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (!queue_ || !queue_->pop(chunk)) {
        return false;
    }

    queued_frames_ -= chunk.nb_samples;
    queued_chunks_ -= 1;
    return true;
}

void AudioPcmQueue::RecordDeviceMediaFrames(std::uint32_t frames,
                                            std::int64_t pts_us) {
    RecordDeviceFrames(frames, pts_us, pts_us != kNoTimestamp);
}

void AudioPcmQueue::RecordDeviceSilenceFrames(std::uint32_t frames) {
    RecordDeviceFrames(frames, kNoTimestamp, false);
}

void AudioPcmQueue::RecordDeviceFrames(std::uint32_t frames,
                                       std::int64_t pts_us,
                                       bool has_media_pts) {
    if (frames == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(timeline_mutex_);
    const auto start_frame = device_written_frames_.load();
    const auto end_frame = start_frame + static_cast<std::uint64_t>(frames);
    // 仅合并严格连续的静音段。真实 PCM 即使 PTS 恰好连续也保持独立段，后续
    // 诊断才能确认 PTS 是否来自不同 decoder frame 或经历过跳变。
    if (!has_media_pts && !device_timeline_.empty() &&
        !device_timeline_.back().has_media_pts &&
        device_timeline_.back().end_frame == start_frame) {
        device_timeline_.back().end_frame = end_frame;
    } else {
        device_timeline_.push_back(
            {start_frame, end_frame, pts_us, has_media_pts});
    }
    device_written_frames_ = end_frame;
}

void AudioPcmQueue::AddUnderrun() {
    ++underruns_;
}

std::int64_t AudioPcmQueue::QueuedFrames() const {
    return std::max<std::int64_t>(0, queued_frames_.load());
}

std::int64_t AudioPcmQueue::PlayedPtsUs(int sample_rate, int padding_frames) const {
    if (sample_rate <= 0) {
        return kNoTimestamp;
    }

    std::lock_guard<std::mutex> lock(timeline_mutex_);
    if (device_timeline_.empty()) {
        return kNoTimestamp;
    }

    const auto written_frames = device_written_frames_.load();
    const auto non_negative_padding = static_cast<std::uint64_t>(
        std::max(0, padding_frames));
    const auto device_played_frames = written_frames > non_negative_padding
        ? written_frames - non_negative_padding
        : 0;

    // 丢弃已经完全越过的分段，但保留当前分段和一个历史媒体 PTS 快照。
    // 队列以音频帧率增长，若不在播放查询路径上回收，长时间运行会线性占用内存。
    while (device_timeline_.size() > 1 &&
           device_timeline_[1].end_frame <= device_played_frames) {
        device_timeline_.pop_front();
    }

    const DeviceTimelineSegment* segment = nullptr;
    for (const auto& item : device_timeline_) {
        if (device_played_frames < item.end_frame) {
            segment = &item;
            break;
        }
    }

    if (!segment) {
        // padding 读取与写设备线程之间允许有短暂竞争。当前位置刚好越过最新
        // 已记录段时，返回最后一个已知媒体 PTS，等待下一次写入补全时间线。
        return has_last_played_pts_ ? last_played_pts_us_ : kNoTimestamp;
    }

    if (!segment->has_media_pts) {
        // 补静音没有合法媒体时间。冻结到上一段真实 PCM 的末尾，比把静音时长
        // 伪装成源 PTS 连续更利于上层识别 underrun 和重新建立同步。
        return has_last_played_pts_ ? last_played_pts_us_ : kNoTimestamp;
    }

    const auto offset_frames = device_played_frames > segment->start_frame
        ? device_played_frames - segment->start_frame
        : 0;
    const auto max_value = (std::numeric_limits<std::int64_t>::max)();
    const auto offset_us = offset_frames >
            static_cast<std::uint64_t>(max_value / 1'000'000)
        ? max_value
        : static_cast<std::int64_t>(offset_frames) * 1'000'000LL / sample_rate;
    if (segment->pts_us > max_value - offset_us) {
        last_played_pts_us_ = max_value;
    } else {
        last_played_pts_us_ = segment->pts_us + offset_us;
    }
    has_last_played_pts_ = true;
    return last_played_pts_us_;
}

AudioRenderStats AudioPcmQueue::GetStats() const {
    AudioRenderStats stats;
    stats.submitted_pcm_frames = submitted_frames_.load();
    stats.queued_pcm_frames = std::max<std::int64_t>(0, queued_frames_.load());
    stats.played_pcm_frames = static_cast<std::int64_t>(
        std::min<std::uint64_t>(device_written_frames_.load(),
                                static_cast<std::uint64_t>(
                                    (std::numeric_limits<std::int64_t>::max)())));
    stats.dropped_pcm_frames = dropped_frames_.load();
    stats.dropped_pcm_chunks = dropped_chunks_.load();
    stats.underruns = underruns_.load();
    stats.queued_pcm_chunks =
        static_cast<std::size_t>(std::max<std::int64_t>(0, queued_chunks_.load()));
    return stats;
}

void AudioPcmQueue::DropQueuedChunkStats(const PcmChunk& chunk) {
    dropped_frames_ += chunk.nb_samples;
    dropped_chunks_ += 1;
    queued_frames_ -= chunk.nb_samples;
    queued_chunks_ -= 1;
}

void AudioPcmQueue::DropUnqueuedChunkStats(const PcmChunk& chunk) {
    dropped_frames_ += chunk.nb_samples;
    dropped_chunks_ += 1;
}

} // namespace render::audio
