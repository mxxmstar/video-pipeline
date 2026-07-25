#pragma once

#include "media/media_frame.h"
#include "preprocessor/i_preprocessor.h"
#include "tensordata/tensor_data.h"

/// @brief OpenVINO YOLO 预处理器配置
struct OpenVinoYoloPreprocessorConfig {
    /// @brief 模型输入高度
    uint32_t input_height{640};
    /// @brief 模型输入宽度
    uint32_t input_width{640};
    /// @brief 模型输入像素格式，kUnknown 表示以 MediaFrame 为准
    PixelFormat pixel_format{PixelFormat::kUnknown};
};

/// @brief OpenVINO YOLO 预处理器
/// 直接将 YUV plane 封装成 TensorFrame，让 OpenVINO 内部预处理处理颜色转换和 resize
class OpenVinoYoloPreprocessor : public IPreprocessor {
public:
    /// @brief 初始化预处理器
    bool Initialize(const OpenVinoYoloPreprocessorConfig& config);

    TensorFrame Process(const MediaFrame& frame) override;

private:
    uint32_t input_height_ = 640;    ///< 模型输入高度
    uint32_t input_width_ = 640;     ///< 模型输入宽度
    PixelFormat pixel_format_{PixelFormat::kUnknown}; ///< 模型输入像素格式
};
