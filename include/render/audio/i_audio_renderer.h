#pragma once

#include <cstdint>

#include "media/media_frame.h"
#include "render/audio/audio_render_config.h"
#include "render/audio/audio_render_stats.h"

namespace render::audio {

class IAudioRenderer {
public:
    virtual ~IAudioRenderer() = default;

    virtual bool Init(const AudioRenderConfig& config) = 0;
    virtual bool Render(const MediaFrame& frame) = 0;
    virtual void Shutdown() = 0;

    virtual int64_t PlayedPtsUs() const = 0;
    virtual AudioRenderStats GetStats() const {
        return {};
    }
};

} // namespace render::audio
