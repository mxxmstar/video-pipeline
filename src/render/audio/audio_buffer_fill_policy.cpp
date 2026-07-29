#include "render/audio/audio_buffer_fill_policy.h"

#include <algorithm>
#include <limits>

namespace render::audio {

AudioBufferFillRequest AudioBufferFillPolicy::Plan(
    const AudioBufferFillContext& context) const {
    AudioBufferFillRequest request;
    if (context.available_frames == 0) {
        return request;
    }

    const auto queued_available = std::max<std::int64_t>(0, context.queued_frames);
    request.frames_to_request = std::min<std::uint32_t>(
        context.available_frames,
        context.active_remaining_frames + static_cast<std::uint32_t>(
            std::min<std::int64_t>(
                queued_available,
                std::numeric_limits<std::uint32_t>::max())));

    if (request.frames_to_request == 0) {
        const auto silence_quantum =
            static_cast<std::uint32_t>(std::max(1, context.sample_rate / 100));
        request.frames_to_request = std::min(context.available_frames, silence_quantum);
        request.silence_only = true;
    }

    return request;
}

} // namespace render::audio
