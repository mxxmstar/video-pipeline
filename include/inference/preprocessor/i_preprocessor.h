#pragma once
#include "tensordata/tensor_frame.h"
#include "media/media_frame.h"
class IPreprocessor {
public:
    /// @brief 灏哅ediaFrame杞崲涓篢ensorFrame
    virtual TensorFrame Process(const MediaFrame&) = 0;
};
