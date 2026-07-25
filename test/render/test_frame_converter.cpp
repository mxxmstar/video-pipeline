#include "render/frame_converter.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "media/simple_buffer.h"

namespace {

int PlaneCountFor(PixelFormat format) {
    switch (format) {
        case PixelFormat::kNV12:
        case PixelFormat::kNV21:
            return 2;
        case PixelFormat::kI420:
            return 3;
        case PixelFormat::kBGR24:
        case PixelFormat::kRGB24:
        case PixelFormat::kGRAY8:
            return 1;
        default:
            return 0;
    }
}

MediaFrame MakeFrame(int width,
                     int height,
                     PixelFormat format,
                     std::vector<std::uint8_t> data) {
    // 当前工程通过 VideoFrameMeta 保存视频帧的尺寸、格式、plane 信息。
    // 这里按最小元数据构造测试帧，让 FrameConverter 覆盖“缺省 offset/stride 推导”路径。
    VideoFrameMeta video_meta;
    video_meta.pixel_format = format;
    video_meta.width = width;
    video_meta.height = height;
    video_meta.plane_count = PlaneCountFor(format);

    MediaFrame frame;
    frame.type = MediaType::VIDEO;
    frame.meta = video_meta;
    frame.buffer = std::make_shared<SimpleBuffer>(std::move(data));
    return frame;
}

void ExpectPixel(const render::RgbaFrame& frame,
                 int x,
                 int y,
                 std::uint8_t r,
                 std::uint8_t g,
                 std::uint8_t b) {
    const std::size_t offset =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) +
         static_cast<std::size_t>(x)) * 4;
    assert(frame.pixels[offset + 0] == r);
    assert(frame.pixels[offset + 1] == g);
    assert(frame.pixels[offset + 2] == b);
    assert(frame.pixels[offset + 3] == 255);
}

void TestRgb24() {
    render::FrameConverter converter;
    render::RgbaFrame out;
    auto frame = MakeFrame(2, 1, PixelFormat::kRGB24, {1, 2, 3, 10, 20, 30});

    assert(converter.ConvertToRgba(frame, out));
    ExpectPixel(out, 0, 0, 1, 2, 3);
    ExpectPixel(out, 1, 0, 10, 20, 30);
}

void TestBgr24WithStrideAndOffset() {
    render::FrameConverter converter;
    render::RgbaFrame out;
    std::vector<std::uint8_t> data{
        0, 0,
        1, 2, 3, 4, 5, 6, 0, 0,
        7, 8, 9, 10, 11, 12, 0, 0,
    };
    auto frame = MakeFrame(2, 2, PixelFormat::kBGR24, std::move(data));

    // 显式设置 plane offset/stride，覆盖真实解码帧常见的 padding 布局。
    auto& video_meta = std::get<VideoFrameMeta>(frame.meta);
    video_meta.plane_info[0].offset = 2;
    video_meta.plane_info[0].stride = 8;

    assert(converter.ConvertToRgba(frame, out));
    ExpectPixel(out, 0, 0, 3, 2, 1);
    ExpectPixel(out, 1, 0, 6, 5, 4);
    ExpectPixel(out, 0, 1, 9, 8, 7);
    ExpectPixel(out, 1, 1, 12, 11, 10);
}

void TestGray8() {
    render::FrameConverter converter;
    render::RgbaFrame out;
    auto frame = MakeFrame(2, 1, PixelFormat::kGRAY8, {8, 240});

    assert(converter.ConvertToRgba(frame, out));
    ExpectPixel(out, 0, 0, 8, 8, 8);
    ExpectPixel(out, 1, 0, 240, 240, 240);
}

void TestNv12() {
    render::FrameConverter converter;
    render::RgbaFrame out;
    auto frame = MakeFrame(2, 2, PixelFormat::kNV12, {
        81, 81,
        81, 81,
        90, 240,
    });

    assert(converter.ConvertToRgba(frame, out));
    ExpectPixel(out, 0, 0, 255, 0, 0);
    ExpectPixel(out, 1, 1, 255, 0, 0);
}

void TestNv21() {
    render::FrameConverter converter;
    render::RgbaFrame out;
    auto frame = MakeFrame(2, 2, PixelFormat::kNV21, {
        81, 81,
        81, 81,
        240, 90,
    });

    assert(converter.ConvertToRgba(frame, out));
    ExpectPixel(out, 0, 0, 255, 0, 0);
    ExpectPixel(out, 1, 1, 255, 0, 0);
}

void TestI420() {
    render::FrameConverter converter;
    render::RgbaFrame out;
    auto frame = MakeFrame(2, 2, PixelFormat::kI420, {
        16, 16,
        16, 16,
        128,
        128,
    });

    assert(converter.ConvertToRgba(frame, out));
    ExpectPixel(out, 0, 0, 0, 0, 0);
    ExpectPixel(out, 1, 1, 0, 0, 0);
}

void TestInvalidBufferFails() {
    render::FrameConverter converter;
    render::RgbaFrame out;
    auto frame = MakeFrame(2, 2, PixelFormat::kRGB24, {1, 2, 3});

    assert(!converter.ConvertToRgba(frame, out));
}

} // namespace

int main() {
    TestRgb24();
    TestBgr24WithStrideAndOffset();
    TestGray8();
    TestNv12();
    TestNv21();
    TestI420();
    TestInvalidBufferFails();
    return 0;
}
