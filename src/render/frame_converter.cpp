#include "render/frame_converter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>

namespace render {
namespace {

// 统一写入错误信息，避免每个分支都重复判断 error 是否为空。
void SetError(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
}

int ClampByte(int value) {
    return std::clamp(value, 0, 255);
}

// 使用 BT.601 limited-range 的常见整数近似公式。
// 当前解码链路主要面向常规 YUV420 视频帧，后续如需 BT.709/BT.2020，
// 可以把 color range / matrix 信息扩展到 VideoFrameMeta 后再分支处理。
struct PlaneView {
    const std::uint8_t* data{nullptr};
    int stride{0};
};

// 从 MediaFrame 中解析某个 plane 的首地址和 stride。
//
// 当前工程的 FFmpegFrameBuffer 会把 AVFrame 压成连续 packed buffer；
// decoder 同时会在 VideoFrameMeta 中填充 plane offset/stride。为了兼容
// 手工构造的测试帧或旧数据源，这里在元数据缺省时按常见 packed 布局推导。
bool ResolvePlane(const MediaFrame& frame,
                  int plane,
                  std::size_t fallback_offset,
                  int fallback_stride,
                  int row_bytes,
                  int rows,
                  PlaneView& out,
                  std::string* error) {
    const auto* base = frame.buffer ? frame.buffer->Data() : nullptr;
    const std::size_t size = frame.buffer ? frame.buffer->Size() : 0;
    if (!base || size == 0) {
        SetError(error, "frame buffer is empty");
        return false;
    }

    const int stride = frame.Stride(plane) > 0 ? frame.Stride(plane) : fallback_stride;
    if (stride < row_bytes || row_bytes <= 0 || rows <= 0) {
        SetError(error, "invalid plane stride");
        return false;
    }

    std::size_t offset = fallback_offset;
    if (plane == 0) {
        offset = frame.PlaneOffset(0) > 0 ? static_cast<std::size_t>(frame.PlaneOffset(0))
                                          : std::size_t{0};
    } else if (frame.PlaneOffset(plane) > 0) {
        offset = static_cast<std::size_t>(frame.PlaneOffset(plane));
    }

    const std::size_t last_byte = offset +
        static_cast<std::size_t>(stride) * static_cast<std::size_t>(rows - 1) +
        static_cast<std::size_t>(row_bytes);
    if (last_byte > size) {
        SetError(error, "plane exceeds frame buffer");
        return false;
    }

    out.data = base + offset;
    out.stride = stride;
    return true;
}

// RGB24/BGR24 都是单 plane packed 布局，只差通道顺序；各路径直接写入
// 当前行的 RGBA 地址，避免在高分辨率帧上反复计算完整输出下标。
bool ConvertPackedRgb(const MediaFrame& frame,
                      RgbaFrame& out,
                      int width,
                      int height,
                      bool bgr,
                      std::string* error) {
    const int row_bytes = width * 3;
    PlaneView plane;
    if (!ResolvePlane(frame, 0, 0, row_bytes, row_bytes, height, plane, error)) {
        return false;
    }

    out.Resize(width, height);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src = plane.data + static_cast<std::size_t>(y) * plane.stride;
        std::uint8_t* dst = out.pixels.data() +
                           static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            const std::size_t src_index = static_cast<std::size_t>(x) * 3;
            const std::size_t dst_index = static_cast<std::size_t>(x) * 4;
            const std::uint8_t c0 = src[src_index + 0];
            const std::uint8_t c1 = src[src_index + 1];
            const std::uint8_t c2 = src[src_index + 2];
            dst[dst_index + 0] = bgr ? c2 : c0;
            dst[dst_index + 1] = c1;
            dst[dst_index + 2] = bgr ? c0 : c2;
            dst[dst_index + 3] = 255;
        }
    }
    return true;
}

// GRAY8 是单 plane 灰度图，RGB 三个通道写入同一个亮度值。
bool ConvertGray(const MediaFrame& frame,
                 RgbaFrame& out,
                 int width,
                 int height,
                 std::string* error) {
    PlaneView plane;
    if (!ResolvePlane(frame, 0, 0, width, width, height, plane, error)) {
        return false;
    }

    out.Resize(width, height);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src = plane.data + static_cast<std::size_t>(y) * plane.stride;
        std::uint8_t* dst = out.pixels.data() +
                           static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            const std::uint8_t value = src[x];
            const std::size_t dst_index = static_cast<std::size_t>(x) * 4;
            dst[dst_index + 0] = value;
            dst[dst_index + 1] = value;
            dst[dst_index + 2] = value;
            dst[dst_index + 3] = 255;
        }
    }
    return true;
}

// NV12/NV21 都是 Y + interleaved UV 的双 plane YUV420。
// NV12 的 chroma 顺序是 U/V，NV21 的 chroma 顺序是 V/U。
bool ConvertNv(const MediaFrame& frame,
               RgbaFrame& out,
               int width,
               int height,
               bool nv21,
               std::string* error) {
    const int uv_width_bytes = ((width + 1) / 2) * 2;
    const int uv_height = (height + 1) / 2;

    PlaneView y_plane;
    if (!ResolvePlane(frame, 0, 0, width, width, height, y_plane, error)) {
        return false;
    }

    const std::size_t y_size =
        static_cast<std::size_t>(y_plane.stride) * static_cast<std::size_t>(height);
    PlaneView uv_plane;
    if (!ResolvePlane(frame, 1, y_size, uv_width_bytes, uv_width_bytes, uv_height,
                      uv_plane, error)) {
        return false;
    }

    out.Resize(width, height);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* y_row =
            y_plane.data + static_cast<std::size_t>(y) * y_plane.stride;
        const std::uint8_t* uv_row =
            uv_plane.data + static_cast<std::size_t>(y / 2) * uv_plane.stride;
        std::uint8_t* dst = out.pixels.data() +
                           static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            const int chroma = (x / 2) * 2;
            const std::uint8_t u = nv21 ? uv_row[chroma + 1] : uv_row[chroma + 0];
            const std::uint8_t v = nv21 ? uv_row[chroma + 0] : uv_row[chroma + 1];
            const int c = static_cast<int>(y_row[x]) - 16;
            const int d = static_cast<int>(u) - 128;
            const int e = static_cast<int>(v) - 128;
            const std::size_t dst_index = static_cast<std::size_t>(x) * 4;
            dst[dst_index + 0] = static_cast<std::uint8_t>(
                ClampByte((298 * c + 409 * e + 128) >> 8));
            dst[dst_index + 1] = static_cast<std::uint8_t>(
                ClampByte((298 * c - 100 * d - 208 * e + 128) >> 8));
            dst[dst_index + 2] = static_cast<std::uint8_t>(
                ClampByte((298 * c + 516 * d + 128) >> 8));
            dst[dst_index + 3] = 255;
        }
    }
    return true;
}

// I420 是 Y/U/V 三 plane YUV420，每个 chroma plane 的宽高都是亮度的一半。
bool ConvertI420(const MediaFrame& frame,
                 RgbaFrame& out,
                 int width,
                 int height,
                 std::string* error) {
    const int uv_width = (width + 1) / 2;
    const int uv_height = (height + 1) / 2;

    PlaneView y_plane;
    if (!ResolvePlane(frame, 0, 0, width, width, height, y_plane, error)) {
        return false;
    }

    const std::size_t y_size =
        static_cast<std::size_t>(y_plane.stride) * static_cast<std::size_t>(height);
    PlaneView u_plane;
    if (!ResolvePlane(frame, 1, y_size, uv_width, uv_width, uv_height, u_plane, error)) {
        return false;
    }

    const std::size_t u_size =
        static_cast<std::size_t>(u_plane.stride) * static_cast<std::size_t>(uv_height);
    PlaneView v_plane;
    if (!ResolvePlane(frame, 2, y_size + u_size, uv_width, uv_width, uv_height,
                      v_plane, error)) {
        return false;
    }

    out.Resize(width, height);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* y_row =
            y_plane.data + static_cast<std::size_t>(y) * y_plane.stride;
        const std::uint8_t* u_row =
            u_plane.data + static_cast<std::size_t>(y / 2) * u_plane.stride;
        const std::uint8_t* v_row =
            v_plane.data + static_cast<std::size_t>(y / 2) * v_plane.stride;
        std::uint8_t* dst = out.pixels.data() +
                           static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            const int c = static_cast<int>(y_row[x]) - 16;
            const int d = static_cast<int>(u_row[x / 2]) - 128;
            const int e = static_cast<int>(v_row[x / 2]) - 128;
            const std::size_t dst_index = static_cast<std::size_t>(x) * 4;
            dst[dst_index + 0] = static_cast<std::uint8_t>(
                ClampByte((298 * c + 409 * e + 128) >> 8));
            dst[dst_index + 1] = static_cast<std::uint8_t>(
                ClampByte((298 * c - 100 * d - 208 * e + 128) >> 8));
            dst[dst_index + 2] = static_cast<std::uint8_t>(
                ClampByte((298 * c + 516 * d + 128) >> 8));
            dst[dst_index + 3] = 255;
        }
    }
    return true;
}

} // namespace

bool FrameConverter::ConvertToRgba(const MediaFrame& frame,
                                   RgbaFrame& out,
                                   std::string* error) const {
    // 渲染层只接受视频帧；音频帧即使有 buffer，也没有可解释的像素元数据。
    if (frame.type != MediaType::VIDEO) {
        SetError(error, "frame is not video");
        return false;
    }

    const int width = frame.Width();
    const int height = frame.Height();
    if (width <= 0 || height <= 0) {
        SetError(error, "invalid frame size");
        return false;
    }

    // 根据当前工程统一的 PixelFormat 分发到具体转换路径。
    // 暂不支持硬件纹理句柄、RGBA 输入或其他 YUV 变体，遇到未知格式显式失败。
    switch (frame.GetPixelFormat()) {
        case PixelFormat::kRGB24:
            return ConvertPackedRgb(frame, out, width, height, false, error);
        case PixelFormat::kBGR24:
            return ConvertPackedRgb(frame, out, width, height, true, error);
        case PixelFormat::kGRAY8:
            return ConvertGray(frame, out, width, height, error);
        case PixelFormat::kNV12:
            return ConvertNv(frame, out, width, height, false, error);
        case PixelFormat::kNV21:
            return ConvertNv(frame, out, width, height, true, error);
        case PixelFormat::kI420:
            return ConvertI420(frame, out, width, height, error);
        default:
            SetError(error, "unsupported pixel format");
            return false;
    }
}

} // namespace render
