#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace render {

/// @brief 渲染前使用的统一 RGBA 中间帧。
///
/// 当前工程的解码输出可能是 NV12/NV21/I420/RGB/BGR/GRAY 等格式。
/// OpenGL 渲染层先通过 FrameConverter 转成连续的 RGBA8 内存，再上传为纹理。
/// 这个结构只表达 CPU 侧像素数据，不持有 OpenGL 纹理或窗口资源。
struct RgbaFrame {
    int width{0};
    int height{0};
    std::vector<std::uint8_t> pixels;

    /// @brief 重置尺寸并清空像素数据。
    ///
    /// pixels 始终按 width * height * 4 分配，每个像素顺序为 R/G/B/A。
    /// 当输入帧尺寸变化时，转换器会调用该函数重建缓存。
    void Reset(int new_width, int new_height) {
        width = new_width;
        height = new_height;
        pixels.assign(static_cast<std::size_t>(width) *
                          static_cast<std::size_t>(height) * 4,
                      0);
    }

    /// @brief 判断当前帧是否没有可上传的有效像素。
    bool Empty() const {
        return width <= 0 || height <= 0 || pixels.empty();
    }
};

} // namespace render
