#pragma once

#include <vector>

#include "inferenceinfo/result.h"
#include "postprocessor/i_postprocessor.h"
#include "tensordata/tensor_frame.h"

/// @brief OpenVINO YOLO postprocessor config.
struct OpenVinoYoloPostprocessorConfig {
    float conf_threshold{0.25f};
    float nms_threshold{0.45f};
    int input_width{640};
    int input_height{640};
};

class YoloPostprocessor : public IPostprocessor {
public:
    bool Initialize(const OpenVinoYoloPostprocessorConfig& config);

    FrameResult Process(const TensorFrame& output) override;

private:
    /// @brief Decode TensorPlane to DetectionBox according to YOLO output layout.
    std::vector<DetectionBox> decode(const TensorPlane& tensor);

    std::vector<DetectionBox> nms(std::vector<DetectionBox>& detections);

    Rectangle restoreBox(const Rectangle& box);

private:
    float conf_threshold_{0.25f};

    float nms_threshold_{0.45f};

    TensorMeta tensor_meta_;
};