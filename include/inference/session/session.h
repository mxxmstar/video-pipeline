#pragma once

#include <memory>

#include "media/media_frame.h"
#include "info/result.h"
#include "engine/i_engine.h"
#include "model/i_model.h"

/// @brief 杩炴帴 model 鍜?engine
class InferenceSession {
public:
    /// @brief 鍒濆鍖栨帹鐞嗕細璇?
    bool Initialize(std::shared_ptr<IModel> model, std::shared_ptr<IInferenceEngine> engine);
    /// @brief 鎺ㄧ悊涓€甯ц棰?
    /// 棰勫鐞?+ 鎺ㄧ悊 + 鍚庡鐞?
    FrameResult Infer(const MediaFrame& frame);

private:
    /// @brief 鎺ㄧ悊妯″瀷
    std::shared_ptr<IModel> model_;
    /// @brief 鎺ㄧ悊寮曟搸
    std::shared_ptr<IInferenceEngine> engine_;
};
