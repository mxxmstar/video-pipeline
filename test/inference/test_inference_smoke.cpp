#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "media/media_frame.h"
#include "media/simple_buffer.h"
#include "model/yolo_model.h"

namespace {

MediaFrame MakeI420Frame(int width, int height) {
    const auto y_size = static_cast<size_t>(width * height);
    const auto uv_size = y_size / 4;

    std::vector<uint8_t> data(y_size + uv_size * 2);
    std::fill(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(y_size), uint8_t{16});
    std::fill(data.begin() + static_cast<std::ptrdiff_t>(y_size), data.end(), uint8_t{128});

    VideoFrameMeta video_meta;
    video_meta.pixel_format = PixelFormat::kI420;
    video_meta.width = width;
    video_meta.height = height;
    video_meta.plane_count = 3;
    video_meta.plane_info[0].offset = 0;
    video_meta.plane_info[0].stride = width;
    video_meta.plane_info[0].size = static_cast<int32_t>(y_size);
    video_meta.plane_info[1].offset = static_cast<int32_t>(y_size);
    video_meta.plane_info[1].stride = width / 2;
    video_meta.plane_info[1].size = static_cast<int32_t>(uv_size);
    video_meta.plane_info[2].offset = static_cast<int32_t>(y_size + uv_size);
    video_meta.plane_info[2].stride = width / 2;
    video_meta.plane_info[2].size = static_cast<int32_t>(uv_size);

    MediaFrame frame;
    frame.type = MediaType::VIDEO;
    frame.time.pts_us = 12345;
    frame.meta = video_meta;
    frame.buffer = std::make_shared<SimpleBuffer>(std::move(data));
    return frame;
}

}  // namespace

int main() {
    YoloModel model;

    ModelConfig config;
    config.name = "smoke-yolo";
    config.class_count = 80;
    config.options["input_width"] = "640";
    config.options["input_height"] = "640";

    if (!model.Initialize(config)) {
        std::cerr << "YoloModel initialization failed\n";
        return 1;
    }

    auto tensor_frame = model.Preprocess(MakeI420Frame(640, 480));
    if (tensor_frame.Empty()) {
        std::cerr << "Preprocess returned an empty TensorFrame\n";
        return 1;
    }

    const auto* y = tensor_frame.FindTensor("y");
    const auto* u = tensor_frame.FindTensor("u");
    const auto* v = tensor_frame.FindTensor("v");
    if (!y || !u || !v) {
        std::cerr << "Expected I420 y/u/v tensors\n";
        return 1;
    }

    if (tensor_frame.tensor_meta_.src_width != 640 ||
        tensor_frame.tensor_meta_.src_height != 480 ||
        tensor_frame.tensor_meta_.input_width != 640 ||
        tensor_frame.tensor_meta_.input_height != 640) {
        std::cerr << "Unexpected tensor metadata\n";
        return 1;
    }

    std::cout << "Inference smoke test passed\n";
    return 0;
}
