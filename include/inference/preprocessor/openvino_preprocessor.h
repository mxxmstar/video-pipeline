#pragma once

#include "media/media_frame.h"
#include "preprocessor/i_preprocessor.h"
#include "tensordata/tensor_data.h"

/// @brief OpenVINO YOLO 棰勫鐞嗗櫒閰嶇疆
struct OpenVinoYoloPreprocessorConfig {
    /// @brief 妯″瀷杈撳叆楂樺害
    uint32_t input_height{640};
    /// @brief 妯″瀷杈撳叆瀹藉害
    uint32_t input_width{640};
    /// @brief 妯″瀷杈撳叆鍍忕礌鏍煎紡锛宬Unknown 琛ㄧず浠?MediaFrame 涓哄噯
    PixelFormat pixel_format{PixelFormat::kUnknown};
};

/// @brief OpenVINO YOLO 棰勫鐞嗗櫒
/// 鐩存帴灏?YUV plane 灏佽鎴?TensorFrame锛岃 OpenVINO 鍐呴儴棰勫鐞嗗鐞嗛鑹茶浆鎹㈠拰 resize
class OpenVinoYoloPreprocessor : public IPreprocessor {
public:
    /// @brief 鍒濆鍖栭澶勭悊鍣?
    bool Initialize(const OpenVinoYoloPreprocessorConfig& config);

    TensorFrame Process(const MediaFrame& frame) override;

private:
    uint32_t input_height_ = 640;    ///< 妯″瀷杈撳叆楂樺害
    uint32_t input_width_ = 640;     ///< 妯″瀷杈撳叆瀹藉害
    PixelFormat pixel_format_{PixelFormat::kUnknown}; ///< 妯″瀷杈撳叆鍍忕礌鏍煎紡
};
