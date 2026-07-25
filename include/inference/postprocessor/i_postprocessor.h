#pragma once
#include "tensordata/tensor_frame.h"
#include "media/media_frame.h"
#include "info/result.h"
class IPostprocessor {
public:
    virtual FrameResult Process(const TensorFrame& output) = 0;
};
