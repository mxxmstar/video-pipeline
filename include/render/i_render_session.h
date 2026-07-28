#pragma once

#include <memory>
#include <string>

#include "media/media_frame.h"
#include "render/render_session_config.h"
#include "render/render_stats.h"

namespace render {

/// @brief 面向 pipeline 的统一音视频渲染入口。
///
/// 生命周期固定为 Init -> Start -> SubmitFrame... -> Stop。SubmitFrame 只入队，
/// 不会在调用线程中执行 OpenGL 或 WASAPI 设备操作。
class IRenderSession {
public:
    virtual ~IRenderSession() = default;

    virtual bool Init(const RenderSessionConfig& config) = 0;
    virtual bool Start() = 0;
    virtual bool SubmitFrame(std::shared_ptr<MediaFrame> frame) = 0;
    virtual void Stop() = 0;
    virtual void Pause() = 0;
    virtual void Resume() = 0;

    virtual bool IsRunning() const = 0;
    virtual bool ShouldClose() const = 0;
    virtual RenderStats GetStats() const = 0;
    virtual std::string LastError() const = 0;
};

} // namespace render
