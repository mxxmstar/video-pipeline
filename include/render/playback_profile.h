#pragma once

#include "mediaflow/playback_profile.h"
#include "render/render_session_config.h"

namespace render {

/// @brief 将公开播放策略转换为渲染会话配置。
///
/// 调用方可在返回后补充窗口大小、标题等展示参数；缓存、队列和同步策略应继续
/// 由 PlaybackProfile 集中决定，避免音视频采用互相矛盾的容量设置。
RenderSessionConfig MakeRenderSessionConfig(
    const mediaflow::PlaybackProfile& profile);

} // namespace render
