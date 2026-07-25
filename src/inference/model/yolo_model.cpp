#include "model/yolo_model.h"

#include <memory>
#include <string>
#include <utility>

#include "inference/inference_logger.h"
#include "inferenceinfo/result.h"
#include "postprocessor/openvino_postprocessor.h"
#include "preprocessor/openvino_preprocessor.h"

namespace {

int OptionInt(const ModelConfig& config, const std::string& key, int fallback) {
    const auto it = config.options.find(key);
    if (it == config.options.end()) {
        return fallback;
    }

    try {
        const auto value = std::stoi(it->second);
        return value > 0 ? value : fallback;
    } catch (const std::exception&) {
        return fallback;
    }
}

}  // namespace

bool YoloModel::Initialize(const ModelConfig& config) {
    model_meta_ = {};
    model_meta_.name = config.name;
    model_meta_.task = TaskType::DETECT;
    model_meta_.input_width = OptionInt(config, "input_width", 640);
    model_meta_.input_height = OptionInt(config, "input_height", 640);
    model_meta_.input_format = PixelFormat::kUnknown;
    model_meta_.dynamic_shape = config.dynamic_shape;
    model_meta_.class_count = config.class_count;

    if (!resetPreprocessor() || !resetPostprocessor()) {
        initialized_ = false;
        return false;
    }

    initialized_ = true;
    return true;
}

bool YoloModel::resetPreprocessor() {
    if (model_meta_.input_width <= 0 || model_meta_.input_height <= 0) {
        LOG_MAIN_ERROR_AT("YOLO input size is invalid: {}x{}",
                          model_meta_.input_width,
                          model_meta_.input_height);
        return false;
    }

    auto preprocessor = std::make_unique<OpenVinoYoloPreprocessor>();
    OpenVinoYoloPreprocessorConfig preprocessor_config;
    preprocessor_config.input_width = static_cast<uint32_t>(model_meta_.input_width);
    preprocessor_config.input_height = static_cast<uint32_t>(model_meta_.input_height);
    preprocessor_config.pixel_format = model_meta_.input_format;

    if (!preprocessor->Initialize(preprocessor_config)) {
        return false;
    }

    preprocessor_ = std::move(preprocessor);
    return true;
}

bool YoloModel::resetPostprocessor() {
    auto postprocessor = std::make_unique<YoloPostprocessor>();
    OpenVinoYoloPostprocessorConfig postprocessor_config;
    postprocessor_config.input_width = model_meta_.input_width;
    postprocessor_config.input_height = model_meta_.input_height;

    if (!postprocessor->Initialize(postprocessor_config)) {
        return false;
    }

    postprocessor_ = std::move(postprocessor);
    return true;
}

const ModelMeta& YoloModel::GetModelMeta() const {
    return model_meta_;
}

bool YoloModel::UpdateModelMeta(const ModelMeta& meta) {
    auto next = model_meta_;
    if (!meta.name.empty()) {
        next.name = meta.name;
    }
    next.task = meta.task;
    if (meta.input_width > 0) {
        next.input_width = meta.input_width;
    }
    if (meta.input_height > 0) {
        next.input_height = meta.input_height;
    }
    if (meta.input_format != PixelFormat::kUnknown) {
        next.input_format = meta.input_format;
    }
    next.dynamic_shape = meta.dynamic_shape;
    if (meta.class_count > 0) {
        next.class_count = meta.class_count;
    }

    const auto size_changed = next.input_width != model_meta_.input_width ||
                              next.input_height != model_meta_.input_height ||
                              next.input_format != model_meta_.input_format;
    model_meta_ = std::move(next);

    if (initialized_ && size_changed && (!resetPreprocessor() || !resetPostprocessor())) {
        initialized_ = false;
        return false;
    }

    LOG_MAIN_INFO_AT("YOLO model meta updated: input={}x{}",
                     model_meta_.input_width,
                     model_meta_.input_height);
    return true;
}

TensorFrame YoloModel::Preprocess(const MediaFrame& frame) {
    if (!initialized_ || !preprocessor_) {
        LOG_MAIN_ERROR_AT("YoloModel is not initialized");
        return {};
    }
    return preprocessor_->Process(frame);
}

FrameResult YoloModel::Postprocess(const TensorFrame& output) {
    if (!initialized_ || !postprocessor_) {
        LOG_MAIN_ERROR_AT("YoloModel postprocessor is not initialized");
        return {};
    }
    return postprocessor_->Process(output);
}
