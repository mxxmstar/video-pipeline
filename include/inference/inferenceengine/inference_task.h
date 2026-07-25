#pragma once
#include <memory>
#include <functional>
#include "media/media_frame.h"
#include "inferenceinfo/result.h"

/// @brief 推理任务
struct InferTask {
    uint64_t frame_id;
    uint32_t stream_id;
    MediaFrame frame;
    std::function<void(FrameResult)> callback;
};
