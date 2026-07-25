#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "media/media_frame.h"
#include "info/result.h"
#include "tensordata/tensor_frame.h"

/// @brief 妯″瀷閰嶇疆缁撴瀯浣擄紝浠呮弿杩版ā鍨嬭涔夊眰閫氱敤閰嶇疆
struct ModelConfig {
    /// @brief 妯″瀷鍚嶇О
    std::string name{""};
    /// @brief 鏄惁鏀寔鍔ㄦ€佸舰鐘?
    bool dynamic_shape{false};
    /// @brief 绫诲埆鏁伴噺
    int class_count{80};
    /// @brief 涓氬姟鍙傛暟
    std::map<std::string, std::string> options;
};

/// @brief 浠诲姟绫诲瀷鏋氫妇
enum class TaskType {
    DETECT,
    SEGMENT,
    POSE,
    OCR,
    CLASSIFY
};

/// @brief 妯″瀷鍏冧俊鎭?
struct ModelMeta {
    /// @brief 妯″瀷鍚嶇О
    std::string name;
    /// @brief 浠诲姟绫诲瀷
    TaskType task{TaskType::DETECT};
    /// @brief 杈撳叆瀹藉害
    int input_width{0};
    /// @brief 杈撳叆楂樺害
    int input_height{0};
    /// @brief 杈撳叆鍍忕礌鏍煎紡
    PixelFormat input_format{PixelFormat::kUnknown};
    /// @brief 鏄惁鍔ㄦ€佽緭鍏?
    bool dynamic_shape{false};
    /// @brief 绫诲埆鏁伴噺
    int class_count{80};
};

/// @brief 妯″瀷璇箟灞傛帴鍙?
class IModel {
public:
    virtual ~IModel() = default;

    /// @brief 鍒濆鍖栨ā鍨嬭祫婧?
    virtual bool Initialize(const ModelConfig& config) = 0;
    /// @brief 鑾峰彇妯″瀷鍏冧俊鎭?
    virtual const ModelMeta& GetModelMeta() const = 0;
    /// @brief 鏇存柊妯″瀷鍏冧俊鎭紝鐢ㄤ簬鎺ㄧ悊寮曟搸瑙ｆ瀽鍚庡洖鍐欒緭鍏ュ舰鐘?
    virtual bool UpdateModelMeta(const ModelMeta& meta) {
        model_meta_ = meta;
        return true;
    }
    /// @brief 棰勫鐞嗗獟浣撳抚
    virtual TensorFrame Preprocess(const MediaFrame& frame) = 0;
    /// @brief 鍚庡鐞嗘ā鍨嬭緭鍑哄紶閲?
    virtual FrameResult Postprocess(const TensorFrame& output) = 0;

protected:
    ModelMeta model_meta_;  ///< 妯″瀷鍙傛暟
};
