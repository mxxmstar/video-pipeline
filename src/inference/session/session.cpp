#include "session/session.h"

#include <cstddef>
#include <limits>
#include <vector>

#include "inference/inference_logger.h"

namespace {

bool ToPositiveInt(int64_t value, int& output) {
    if (value <= 0 || value > static_cast<int64_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    output = static_cast<int>(value);
    return true;
}

bool ResolveImageInputSize(const TensorShape& shape, int& width, int& height) {
    if (shape.dims.size() < 2) {
        return false;
    }

    if (shape.dims.size() == 4) {
        const auto nchw_channels = shape.dims[1];
        if (nchw_channels == 1 || nchw_channels == 3 || nchw_channels == 4) {
            return ToPositiveInt(shape.dims[3], width) && ToPositiveInt(shape.dims[2], height);
        }

        const auto nhwc_channels = shape.dims[3];
        if (nhwc_channels == 1 || nhwc_channels == 3 || nhwc_channels == 4) {
            return ToPositiveInt(shape.dims[2], width) && ToPositiveInt(shape.dims[1], height);
        }
    }

    return ToPositiveInt(shape.dims[shape.dims.size() - 1], width) &&
           ToPositiveInt(shape.dims[shape.dims.size() - 2], height);
}

}  // namespace

bool InferenceSession::Initialize(std::shared_ptr<IModel> model,
                                  std::shared_ptr<IInferenceEngine> engine) {
    if (!model || !engine) {
        LOG_MAIN_ERROR_AT("InferenceSession requires non-null model and engine");
        return false;
    }

    model_ = std::move(model);
    engine_ = std::move(engine);

    const auto model_desc = engine_->GetModelDesc();
    if (!model_desc.inputs.empty()) {
        auto meta = model_->GetModelMeta();
        meta.dynamic_shape = model_desc.inputs.front().dynamic_shape;
        if (!ResolveImageInputSize(model_desc.inputs.front().shape, meta.input_width, meta.input_height)) {
            LOG_MAIN_WARN_AT("Failed to resolve model input shape from inference engine, keep current model input size: {}x{}",
                             meta.input_width,
                             meta.input_height);
        }
        if (!model_->UpdateModelMeta(meta)) {
            LOG_MAIN_ERROR_AT("Failed to update model meta from inference engine");
            model_.reset();
            engine_.reset();
            return false;
        }
    }

    return true;
}

FrameResult InferenceSession::Infer(const MediaFrame& frame) {
    FrameResult result;
    result.frame_id = 0;
    result.pts = static_cast<uint64_t>(frame.time.pts_us);

    if (!model_ || !engine_) {
        LOG_MAIN_ERROR_AT("InferenceSession is not initialized");
        return result;
    }

    auto input = model_->Preprocess(frame);
    TensorFrame output;
    if (!engine_->Infer(input, output)) {
        LOG_MAIN_ERROR_AT("InferenceSession engine inference failed");
        return result;
    }

    result = model_->Postprocess(output);
    result.pts = static_cast<uint64_t>(frame.time.pts_us);
    return result;
}
