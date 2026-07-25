#include <cstdint>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

#include "engine/openvino_engine.h"
#include "media/media_frame.h"
#include "media/simple_buffer.h"
#include "model/yolo_model.h"
#include "session/session.h"

#ifndef VIDEO_PIPELINE_SOURCE_DIR
#define VIDEO_PIPELINE_SOURCE_DIR "."
#endif

namespace {

MediaFrame MakeI420Frame(int width, int height) {
    const auto y_size = static_cast<size_t>(width * height);
    const auto uv_size = y_size / 4;

    std::vector<uint8_t> data(y_size + uv_size * 2);
    std::fill(data.begin(), data.begin() + static_cast<std::ptrdiff_t>(y_size), uint8_t{96});
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
    frame.time.pts_us = 1000;
    frame.meta = video_meta;
    frame.buffer = std::make_shared<SimpleBuffer>(std::move(data));
    return frame;
}

}  // namespace

int main() {
    const auto model_path =
        std::filesystem::path{VIDEO_PIPELINE_SOURCE_DIR} / "models/yolov5/yolov5s.xml";
    if (!std::filesystem::exists(model_path)) {
        std::cerr << "YOLOv5 model does not exist: " << model_path << "\n";
        return 1;
    }

    auto model = std::make_shared<YoloModel>();
    ModelConfig model_config;
    model_config.name = "yolov5s";
    model_config.class_count = 80;
    model_config.options["input_width"] = "640";
    model_config.options["input_height"] = "640";
    if (!model->Initialize(model_config)) {
        std::cerr << "YoloModel initialization failed\n";
        return 1;
    }

    auto engine = std::make_shared<OpenVinoCpuEngine>();
    EngineLoadConfig load_config;
    load_config.engine.model_path = model_path.string();
    load_config.engine.backend = "OPENVINO";
    load_config.engine.device = "CPU";
    load_config.engine.request_count = 1;

    OpenVinoPreprocessConfig preprocess_config;
    preprocess_config.enabled = true;
    preprocess_config.input_pixel_format = PixelFormat::kI420;
    preprocess_config.model_pixel_format = PixelFormat::kRGB24;
    preprocess_config.model_input_layout = "NCHW";
    preprocess_config.scale = 255.0f;
    load_config.preprocess = preprocess_config;

    if (!engine->LoadModel(load_config)) {
        std::cerr << "OpenVINO engine failed to load model\n";
        return 1;
    }

    InferenceSession session;
    if (!session.Initialize(model, engine)) {
        std::cerr << "InferenceSession initialization failed\n";
        return 1;
    }

    const auto result = session.Infer(MakeI420Frame(640, 640));
    std::cout << "YOLOv5 inference completed, objects=" << result.objects.size() << "\n";
    return 0;
}
