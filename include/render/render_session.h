#pragma once

#include <memory>

#include "render/audio/i_audio_renderer.h"
#include "render/i_render_session.h"
#include "render/i_video_renderer.h"

namespace render {

/// @brief IRenderSession 的默认实现。
///
/// 默认构造函数使用 OpenGLVideoRenderer 和 WasapiAudioRenderer。带 renderer
/// 参数的构造函数用于测试或替换后端；renderer 所有权会转移给 RenderSession。
class RenderSession final : public IRenderSession {
public:
    RenderSession();
    RenderSession(std::unique_ptr<IVideoRenderer> video_renderer,
                  std::unique_ptr<audio::IAudioRenderer> audio_renderer);
    ~RenderSession() override;

    RenderSession(const RenderSession&) = delete;
    RenderSession& operator=(const RenderSession&) = delete;

    /// @brief 初始化渲染会话。只做校验并保存配置。
    /// @param config 渲染会话配置。    
    bool Init(const RenderSessionConfig& config) override;

    /// @brief 启动渲染会话。
    bool Start() override;
    
    /// @brief 提交一帧视频或音频数据。
    /// @param frame 视频或音频数据。
    /// @return 是否成功提交。    
    bool SubmitFrame(std::shared_ptr<MediaFrame> frame) override;
    void Stop() override;
    void Pause() override;
    void Resume() override;

    bool IsRunning() const override;
    bool ShouldClose() const override;
    RenderStats GetStats() const override;
    std::string LastError() const override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace render
