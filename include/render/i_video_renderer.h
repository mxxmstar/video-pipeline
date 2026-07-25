#pragma once

#include "media/media_frame.h"
#include "render/render_config.h"

namespace render {

/// @brief 视频渲染器抽象接口。
///
/// 第一阶段只提供 OpenGLVideoRenderer 实现，但接口保持独立，
/// 后续可以接入 D3D、SDL、Tauri texture bridge 或无窗口测试 renderer。
class IVideoRenderer {
public:
    virtual ~IVideoRenderer() = default;

    /// @brief 初始化渲染资源，例如窗口、图形上下文、shader 和纹理缓存。
    virtual bool Init(const RenderConfig& config) = 0;

    /// @brief 渲染一帧视频。实现内部可以选择转换格式、上传纹理并交换缓冲。
    virtual bool Render(const MediaFrame& frame) = 0;

    /// @brief 处理窗口事件。对于 GLFW 实现，需要定期调用以响应关闭/键盘事件。
    virtual void PollEvents() = 0;

    /// @brief 判断窗口或渲染器是否已经请求关闭。
    virtual bool ShouldClose() const = 0;

    /// @brief 释放窗口、图形上下文和 GPU 资源。
    virtual void Shutdown() = 0;
};

} // namespace render
