#pragma once

#include <memory>

#include "media/media_frame.h"
#include "inferenceinfo/result.h"
#include "inferenceengine/i_engine.h"
#include "model/i_model.h"

/// @brief 连接 model 和 engine
class InferenceSession {
public:
    /// @brief 初始化推理会话
    bool Initialize(std::shared_ptr<IModel> model, std::shared_ptr<IInferenceEngine> engine);
    /// @brief 推理一帧视频
    /// 预处理 + 推理 + 后处理
    FrameResult Infer(const MediaFrame& frame);

private:
    /// @brief 推理模型
    std::shared_ptr<IModel> model_;
    /// @brief 推理引擎
    std::shared_ptr<IInferenceEngine> engine_;
};
