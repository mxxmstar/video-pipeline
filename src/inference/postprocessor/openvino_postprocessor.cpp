#include "postprocessor/openvino_postprocessor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

#include "inference/inference_logger.h"

namespace {

constexpr float kEpsilon = 1e-6f;

const TensorPlane* FindFirstFloatTensor(const TensorFrame& output) {
    for (const auto& [_, tensor] : output.tensors_) {
        if (tensor && tensor->type == TensorType::FP32 && tensor->data && tensor->bytes > 0) {
            return tensor.get();
        }
    }
    return nullptr;
}

bool ResolveYoloShape(const TensorShape& shape,
                      size_t& count,
                      size_t& attrs,
                      bool& transposed) {
    if (shape.dims.size() == 3) {
        const auto dim1 = shape.dims[1];
        const auto dim2 = shape.dims[2];
        if (dim1 <= 0 || dim2 <= 0) {
            return false;
        }

        if (dim1 <= 256 && dim2 > dim1) {
            attrs = static_cast<size_t>(dim1);
            count = static_cast<size_t>(dim2);
            transposed = true;
        } else {
            count = static_cast<size_t>(dim1);
            attrs = static_cast<size_t>(dim2);
            transposed = false;
        }
        return true;
    }

    if (shape.dims.size() == 2) {
        const auto dim0 = shape.dims[0];
        const auto dim1 = shape.dims[1];
        if (dim0 <= 0 || dim1 <= 0) {
            return false;
        }

        if (dim0 <= 256 && dim1 > dim0) {
            attrs = static_cast<size_t>(dim0);
            count = static_cast<size_t>(dim1);
            transposed = true;
        } else {
            count = static_cast<size_t>(dim0);
            attrs = static_cast<size_t>(dim1);
            transposed = false;
        }
        return true;
    }

    return false;
}

float YoloValue(const float* data, size_t index, size_t attr, size_t count, size_t attrs, bool transposed) {
    return transposed ? data[attr * count + index] : data[index * attrs + attr];
}

bool HasObjectness(size_t attrs) {
    // Common layouts: YOLOv5 = 5 + classes, YOLOv8 = 4 + classes.
    // COCO YOLOv8 commonly has 84 attrs; COCO YOLOv5 commonly has 85 attrs.
    if (attrs == 84) {
        return false;
    }
    return attrs >= 6;
}

float Area(const DetectionBox& box) {
    return std::max(0.0f, box.x2 - box.x1) * std::max(0.0f, box.y2 - box.y1);
}

float IoU(const DetectionBox& a, const DetectionBox& b) {
    const auto ix1 = std::max(a.x1, b.x1);
    const auto iy1 = std::max(a.y1, b.y1);
    const auto ix2 = std::min(a.x2, b.x2);
    const auto iy2 = std::min(a.y2, b.y2);
    const auto iw = std::max(0.0f, ix2 - ix1);
    const auto ih = std::max(0.0f, iy2 - iy1);
    const auto inter = iw * ih;
    const auto union_area = Area(a) + Area(b) - inter;

    if (union_area <= std::numeric_limits<float>::epsilon()) {
        return 0.0f;
    }
    return inter / union_area;
}

}  // namespace

bool YoloPostprocessor::Initialize(const OpenVinoYoloPostprocessorConfig& config) {
    if (config.input_width <= 0 || config.input_height <= 0) {
        LOG_MAIN_ERROR_AT("YOLO postprocessor input size is invalid: {}x{}",
                          config.input_width,
                          config.input_height);
        return false;
    }

    conf_threshold_ = config.conf_threshold;
    nms_threshold_ = config.nms_threshold;
    tensor_meta_.input_width = config.input_width;
    tensor_meta_.input_height = config.input_height;
    tensor_meta_.src_width = config.input_width;
    tensor_meta_.src_height = config.input_height;
    tensor_meta_.letterbox = {};
    return true;
}

FrameResult YoloPostprocessor::Process(const TensorFrame& output) {
    FrameResult result;
    result.frame_id = 0;
    result.pts = 0;

    const auto& meta = output.tensor_meta_;

    // 从 TensorFrame 中复制完整的元信息
    tensor_meta_ = meta;
    // 如果原始尺寸未设置，默认回退到模型输入尺寸
    if (tensor_meta_.src_width <= 0) {
        tensor_meta_.src_width = tensor_meta_.input_width;
    }
    if (tensor_meta_.src_height <= 0) {
        tensor_meta_.src_height = tensor_meta_.input_height;
    }

    const auto* tensor = FindFirstFloatTensor(output);
    if (!tensor) {
        LOG_MAIN_WARN_AT("YOLO postprocess did not find FP32 output tensor");
        return result;
    }

    auto detections = decode(*tensor);
    detections = nms(detections);

    result.objects.reserve(detections.size());
    for (const auto& detection : detections) {
        Rectangle model_box;
        model_box.x = detection.x1;
        model_box.y = detection.y1;
        model_box.width = detection.x2 - detection.x1;
        model_box.height = detection.y2 - detection.y1;

        ObjectResult object;
        object.object.class_id = detection.class_id;
        object.object.score = detection.confidence;
        object.object.rect = restoreBox(model_box);
        result.objects.push_back(std::move(object));
    }

    return result;
}

std::vector<DetectionBox> YoloPostprocessor::decode(const TensorPlane& tensor) {
    std::vector<DetectionBox> detections;

    size_t count = 0;
    size_t attrs = 0;
    bool transposed = false;
    if (!ResolveYoloShape(tensor.shape, count, attrs, transposed) || attrs < 5) {
        LOG_MAIN_WARN_AT("Unsupported YOLO output shape");
        return detections;
    }

    const auto* data = tensor.Data<float>();
    const auto has_objectness = HasObjectness(attrs);
    const auto class_begin = has_objectness ? size_t{5} : size_t{4};
    if (class_begin >= attrs) {
        return detections;
    }

    detections.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto cx = YoloValue(data, i, 0, count, attrs, transposed);
        const auto cy = YoloValue(data, i, 1, count, attrs, transposed);
        const auto w = std::abs(YoloValue(data, i, 2, count, attrs, transposed));
        const auto h = std::abs(YoloValue(data, i, 3, count, attrs, transposed));
        const auto objectness = has_objectness ? YoloValue(data, i, 4, count, attrs, transposed) : 1.0f;

        int best_class = -1;
        float best_score = 0.0f;
        for (size_t c = class_begin; c < attrs; ++c) {
            const auto class_score = YoloValue(data, i, c, count, attrs, transposed);
            if (class_score > best_score) {
                best_score = class_score;
                best_class = static_cast<int>(c - class_begin);
            }
        }

        const auto score = objectness * best_score;
        if (best_class < 0 || score < conf_threshold_) {
            continue;
        }

        DetectionBox box;
        box.class_id = best_class;
        box.confidence = score;
        box.x1 = std::clamp(cx - w * 0.5f, 0.0f, static_cast<float>(tensor_meta_.input_width));
        box.y1 = std::clamp(cy - h * 0.5f, 0.0f, static_cast<float>(tensor_meta_.input_height));
        box.x2 = std::clamp(cx + w * 0.5f, 0.0f, static_cast<float>(tensor_meta_.input_width));
        box.y2 = std::clamp(cy + h * 0.5f, 0.0f, static_cast<float>(tensor_meta_.input_height));
        if (Area(box) > 0.0f) {
            detections.push_back(box);
        }
    }

    return detections;
}

std::vector<DetectionBox> YoloPostprocessor::nms(std::vector<DetectionBox>& detections) {
    std::sort(detections.begin(),
              detections.end(),
              [](const DetectionBox& lhs, const DetectionBox& rhs) {
                  return lhs.confidence > rhs.confidence;
              });

    std::vector<DetectionBox> kept;
    std::vector<bool> removed(detections.size(), false);
    for (size_t i = 0; i < detections.size(); ++i) {
        if (removed[i]) {
            continue;
        }

        kept.push_back(detections[i]);
        for (size_t j = i + 1; j < detections.size(); ++j) {
            if (removed[j] || detections[i].class_id != detections[j].class_id) {
                continue;
            }
            if (IoU(detections[i], detections[j]) > nms_threshold_) {
                removed[j] = true;
            }
        }
    }

    return kept;
}

Rectangle YoloPostprocessor::restoreBox(const Rectangle& box) {
    const auto sx = tensor_meta_.letterbox.scale_x > kEpsilon ? tensor_meta_.letterbox.scale_x : 1.0f;
    const auto sy = tensor_meta_.letterbox.scale_y > kEpsilon ? tensor_meta_.letterbox.scale_y : 1.0f;

    const auto x1 = std::clamp((box.x - tensor_meta_.letterbox.pad_x) / sx,
                               0.0f,
                               static_cast<float>(tensor_meta_.src_width));
    const auto y1 = std::clamp((box.y - tensor_meta_.letterbox.pad_y) / sy,
                               0.0f,
                               static_cast<float>(tensor_meta_.src_height));
    const auto x2 = std::clamp((box.x + box.width - tensor_meta_.letterbox.pad_x) / sx,
                               0.0f,
                               static_cast<float>(tensor_meta_.src_width));
    const auto y2 = std::clamp((box.y + box.height - tensor_meta_.letterbox.pad_y) / sy,
                               0.0f,
                               static_cast<float>(tensor_meta_.src_height));

    Rectangle restored;
    restored.x = x1;
    restored.y = y1;
    restored.width = std::max(0.0f, x2 - x1);
    restored.height = std::max(0.0f, y2 - y1);
    return restored;
}
