#pragma once

#include <string>

namespace render {

/// @brief OpenGL 视频渲染窗口配置。
///
/// 第一阶段只迁移本地窗口渲染能力，后续如果要嵌入 Tauri/Qt/其他 UI，
/// 可以继续扩展这里或新增平台相关的 renderer 实现。
struct RenderConfig {
    /// GLFW 创建窗口时使用的逻辑宽度。
    int window_width{1280};

    /// GLFW 创建窗口时使用的逻辑高度。
    int window_height{720};

    /// 原生窗口标题。
    std::string title{"video-pipeline renderer"};

    /// 是否显示窗口；测试或后台初始化时可以设为 false。
    bool visible{true};

    /// 是否启用垂直同步，开启后渲染节奏跟随显示器刷新率。
    bool vsync{true};

    /// 是否允许 ESC 键关闭窗口。
    bool close_on_escape{true};
};

} // namespace render
