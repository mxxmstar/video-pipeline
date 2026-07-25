#pragma once

#include <string>

#include "media/media_frame.h"
#include "render/render_frame.h"

namespace render {

/// @brief 将当前工程的 MediaFrame 转换为 RGBA8 像素缓冲。
///
/// 转换器只依赖 MediaFrame 的 CPU buffer 和 VideoFrameMeta：
/// - width/height/pixel_format 通过 MediaFrame 访问器读取；
/// - stride/plane offset 优先使用元数据，缺省时按 packed 布局推导；
/// - 输出固定为 RgbaFrame，便于 OpenGL 纹理上传。
class FrameConverter {
public:
    /// @brief 转换一帧视频为 RGBA。
    /// @param frame 输入视频帧，必须是 MediaType::VIDEO。
    /// @param out 输出 RGBA 帧，成功时会被重置为输入帧尺寸。
    /// @param error 可选错误信息，失败时写入可读原因。
    /// @return true 表示转换成功。
    bool ConvertToRgba(const MediaFrame& frame, RgbaFrame& out, std::string* error = nullptr) const;
};

} // namespace render
