#pragma once
#include "tensordata/tensor_frame.h"
#include "media/media_frame.h"
class IPreprocessor {
public:
    /// @brief 将MediaFrame转换为TensorFrame
    virtual TensorFrame Process(const MediaFrame&) = 0;
};
