#include "render/audio/audio_pcm_queue.h"

#include <algorithm>
#include <mutex>

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
    played_frames_ = 0;
    dropped_frames_ = 0;
    dropped_chunks_ = 0;
    underruns_ = 0;
    first_pts_us_ = 0;
    first_pts_set_ = false;
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
    played_frames_ = 0;
    dropped_frames_ = 0;
    dropped_chunks_ = 0;
    underruns_ = 0;
    first_pts_us_ = 0;
    first_pts_set_ = false;
}

bool AudioPcmQueue::Enqueue(PcmChunk&& chunk) {
    const auto chunk_frames = static_cast<std::int64_t>(chunk.nb_samples);
    submitted_frames_ += chunk_frames;
    if (!first_pts_set_.load()) {
        std::lock_guard<std::mutex> lock(first_pts_mutex_);
        if (!first_pts_set_.load()) {
            first_pts_us_ = chunk.pts_us;
            first_pts_set_ = true;
        }
    }

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

void AudioPcmQueue::AddPlayedFrames(std::uint32_t frames) {
    played_frames_ += frames;
}

void AudioPcmQueue::AddUnderrun() {
    ++underruns_;
}

std::int64_t AudioPcmQueue::QueuedFrames() const {
    return std::max<std::int64_t>(0, queued_frames_.load());
}

std::int64_t AudioPcmQueue::PlayedPtsUs(int sample_rate, int padding_frames) const {
    if (sample_rate <= 0 || !first_pts_set_.load()) {
        return 0;
    }

    const auto device_played_frames = std::max<std::int64_t>(
        0, played_frames_.load() - std::max(0, padding_frames));
    return first_pts_us_.load() +
           device_played_frames * 1'000'000LL / sample_rate;
}

AudioRenderStats AudioPcmQueue::GetStats() const {
    AudioRenderStats stats;
    stats.submitted_pcm_frames = submitted_frames_.load();
    stats.queued_pcm_frames = std::max<std::int64_t>(0, queued_frames_.load());
    stats.played_pcm_frames = played_frames_.load();
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
