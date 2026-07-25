#pragma once

#include <cstdint>
#include <string>

#include "render/frame_converter.h"
#include "render/i_video_renderer.h"

struct GLFWwindow;

namespace render {

/// @brief 基于 GLFW + GLAD + OpenGL 3.3 的本地视频窗口渲染器。
///
/// 渲染路径：
/// 1. FrameConverter 将 MediaFrame 转成 RGBA8 CPU 缓冲；
/// 2. UploadTexture 将 RGBA 数据上传到 OpenGL 2D 纹理；
/// 3. 一个全屏四边形采样该纹理并显示到窗口。
///
/// 注意：GLFW 窗口和 OpenGL context 通常应该在同一线程创建和使用。
/// 如果未来把它接到异步解码回调，建议单独建立 render 线程并通过队列投递帧。
class OpenGLVideoRenderer final : public IVideoRenderer {
public:
    OpenGLVideoRenderer() = default;
    ~OpenGLVideoRenderer() override;

    bool Init(const RenderConfig& config) override;
    bool Render(const MediaFrame& frame) override;
    void PollEvents() override;
    bool ShouldClose() const override;
    void Shutdown() override;

private:
    /// @brief 初始化 GLFW、创建窗口并加载 OpenGL 函数指针。
    bool CreateWindow();

    /// @brief 创建最小 shader program，用于把纹理绘制到全屏 quad。
    bool CreateProgram();

    /// @brief 创建全屏矩形的 VAO/VBO/EBO。
    bool CreateQuad();

    /// @brief 将 RGBA CPU 帧上传到 OpenGL 纹理；尺寸变化时重建纹理存储。
    bool UploadTexture(const RgbaFrame& frame);

    /// @brief 删除 OpenGL 对象。调用前需要确保当前线程拥有有效 context。
    void DestroyGlResources();

    /// @brief 记录并输出最近一次渲染错误。
    void SetError(std::string message);

    RenderConfig config_;
    FrameConverter converter_;
    RgbaFrame rgba_frame_;
    GLFWwindow* window_{nullptr};
    std::uint32_t program_{0};
    std::uint32_t vao_{0};
    std::uint32_t vbo_{0};
    std::uint32_t ebo_{0};
    std::uint32_t texture_{0};
    int texture_width_{0};
    int texture_height_{0};
    bool glfw_initialized_{false};
    bool initialized_{false};
    std::string last_error_;
};

} // namespace render
