#include "render/audio/audio_buffer_fill_policy.h"
#include "render/audio/audio_pcm_queue.h"
#include "render/audio/pcm_chunk.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

render::audio::PcmChunk MakeChunk(std::uint32_t samples, std::int64_t pts_us) {
    render::audio::PcmChunk chunk;
    chunk.nb_samples = samples;
    chunk.pts_us = pts_us;
    chunk.duration_us = static_cast<std::int64_t>(samples) * 1000;
    chunk.data.assign(static_cast<std::size_t>(samples) * 2, 0);
    return chunk;
}

bool TestQueueDropsOldestChunk() {
    render::audio::AudioPcmQueue queue;
    queue.Reset(2);

    queue.Enqueue(MakeChunk(10, 1000));
    queue.Enqueue(MakeChunk(20, 2000));
    queue.Enqueue(MakeChunk(30, 3000));

    auto stats = queue.GetStats();
    if (stats.submitted_pcm_frames != 60 ||
        stats.dropped_pcm_frames != 10 ||
        stats.dropped_pcm_chunks != 1 ||
        stats.queued_pcm_frames != 50 ||
        stats.queued_pcm_chunks != 2) {
        std::cerr << "unexpected queue stats after overflow\n";
        return false;
    }

    render::audio::PcmChunk first;
    render::audio::PcmChunk second;
    if (!queue.Pop(first) || !queue.Pop(second)) {
        std::cerr << "failed to pop expected chunks\n";
        return false;
    }

    stats = queue.GetStats();
    return first.pts_us == 2000 &&
           second.pts_us == 3000 &&
           stats.queued_pcm_frames == 0 &&
           stats.queued_pcm_chunks == 0;
}

bool TestPlayedPtsUsesDevicePadding() {
    render::audio::AudioPcmQueue queue;
    queue.Reset(4);
    queue.Enqueue(MakeChunk(20, 1'000'000));

    render::audio::PcmChunk chunk;
    if (!queue.Pop(chunk)) {
        return false;
    }

    queue.AddPlayedFrames(20);
    return queue.PlayedPtsUs(1000, 5) == 1'015'000;
}

bool TestFillPolicyUsesOnlyAvailablePcm() {
    render::audio::AudioBufferFillPolicy policy;

    const render::audio::AudioBufferFillContext no_pcm_context{
        1000,
        0,
        0,
        48000,
    };
    const auto no_pcm = policy.Plan(no_pcm_context);
    if (!no_pcm.silence_only || no_pcm.frames_to_request != 480) {
        std::cerr << "no-pcm policy should request a short silence quantum\n";
        return false;
    }

    const render::audio::AudioBufferFillContext enough_pcm_context{
        1000,
        100,
        960,
        48000,
    };
    const auto enough_pcm = policy.Plan(enough_pcm_context);
    if (enough_pcm.silence_only || enough_pcm.frames_to_request != 1000) {
        std::cerr << "enough-pcm policy should fill all available frames\n";
        return false;
    }

    const render::audio::AudioBufferFillContext partial_pcm_context{
        1000,
        100,
        200,
        48000,
    };
    const auto partial_pcm = policy.Plan(partial_pcm_context);
    if (partial_pcm.silence_only || partial_pcm.frames_to_request != 300) {
        std::cerr << "partial-pcm policy should not request the whole buffer\n";
        return false;
    }

    const render::audio::AudioBufferFillContext no_space_context{
        0,
        100,
        200,
        48000,
    };
    const auto no_space = policy.Plan(no_space_context);
    return no_space.frames_to_request == 0 && !no_space.silence_only;
}

} // namespace

int main() {
    if (!TestQueueDropsOldestChunk()) {
        std::cerr << "TestQueueDropsOldestChunk failed\n";
        return 1;
    }
    if (!TestPlayedPtsUsesDevicePadding()) {
        std::cerr << "TestPlayedPtsUsesDevicePadding failed\n";
        return 1;
    }
    if (!TestFillPolicyUsesOnlyAvailablePcm()) {
        std::cerr << "TestFillPolicyUsesOnlyAvailablePcm failed\n";
        return 1;
    }

    std::cout << "Audio render strategy tests passed\n";
    return 0;
}
