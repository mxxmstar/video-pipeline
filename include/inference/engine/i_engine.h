#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "model/i_model.h"
#include "tensordata/tensor_frame.h"

enum class EngineCapability : uint32_t {
    NONE = 0,
    ASYNC = 1u << 0, ///< 鏄惁鏀寔寮傛鎺ㄧ悊
    DYNAMIC = 1u << 1, ///< 鏄惁鏀寔鍔ㄦ€佸舰鐘?
    BATCH = 1u << 2, ///< 鏄惁鏀寔鎵归噺鎺ㄧ悊
    CPU = 1u << 3, ///< 鏄惁鏀寔 CPU 鎺ㄧ悊
    GPU = 1u << 4, ///< 鏄惁鏀寔 GPU 鎺ㄧ悊
};

inline EngineCapability operator|(EngineCapability lhs, EngineCapability rhs) {
    return static_cast<EngineCapability>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

inline EngineCapability& operator|=(EngineCapability& lhs, EngineCapability rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline bool HasEngineCapability(EngineCapability value, EngineCapability capability) {
    return (static_cast<uint32_t>(value) & static_cast<uint32_t>(capability)) != 0;
}

/// @brief 鎺ㄧ悊寮曟搸閰嶇疆鍜屼俊鎭?
struct EngineConfig {
    /// @brief 妯″瀷鏂囦欢璺緞
    std::string model_path;
    /// @brief 鍚庣绫诲瀷
    std::string backend{"OPENVINO"};
    /// @brief 璁惧绫诲瀷
    std::string device{"CPU"};
    /// @brief 鎵归噺澶у皬
    int batch_size{1};
    /// @brief 鎺ㄧ悊璇锋眰姹犲ぇ灏?
    uint32_t request_count{1};
    /// @brief 鏄惁鏀寔寮傛鎺ㄧ悊
    bool support_async{false};
    /// @brief 鏄惁鏀寔鍔ㄦ€佸舰鐘?
    bool support_dynamic_shape{false};
    /// @brief 鏈€澶ф壒閲忓ぇ灏?
    uint32_t max_batch_size{1};
};

/// @brief OpenVINO 棰勫鐞嗛厤缃?
struct OpenVinoPreprocessConfig {
    /// @brief 鏄惁鍚敤 OpenVINO 鍐呴儴棰勫鐞?
    bool enabled{false};
    /// @brief OpenVINO 棰勫鐞嗚緭鍏ュ儚绱犳牸寮?
    PixelFormat input_pixel_format{PixelFormat::kNV12};
    /// @brief 妯″瀷鏈熸湜鐨勫儚绱犳牸寮?
    PixelFormat model_pixel_format{PixelFormat::kRGB24};
    /// @brief 妯″瀷杈撳叆甯冨眬锛岀┖瀛楃涓茶〃绀轰粠妯″瀷杈撳叆褰㈢姸鎺ㄦ柇
    std::string model_input_layout;
    /// @brief OpenVINO 缂╂斁鍥犲瓙锛? 琛ㄧず涓嶈繘琛岀缉鏀?
    float scale{255.0f};
};

/// @brief 鎺ㄧ悊寮曟搸鍔犺浇閰嶇疆
struct EngineLoadConfig {
    EngineConfig engine;
    std::optional<OpenVinoPreprocessConfig> preprocess; // openvino 鑷甫棰勫鐞?
};

/// @brief 鎺ㄧ悊涓婁笅鏂?
struct InferContext {
    /// @brief 甯?ID
    uint64_t frame_id{0};
    /// @brief 鏃堕棿鎴?
    uint64_t pts{0};
};

/// @brief 杈撳叆/杈撳嚭寮犻噺鎻忚堪
struct TensorDesc {
    /// @brief 寮犻噺鍚嶇О
    std::string name;
    /// @brief 寮犻噺鍏冪礌绫诲瀷
    TensorType type{TensorType::UNKNOWN};
    /// @brief 寮犻噺褰㈢姸
    TensorShape shape;
    /// @brief 鏄惁鍔ㄦ€佸舰鐘?
    bool dynamic_shape{false};
    /// @brief 鏄惁杈撳叆寮犻噺
    bool is_input{true};
    /// @brief 寮犻噺鍏冪礌鏁伴噺
    size_t element_count{0};
    /// @brief 寮犻噺瀛楄妭鏁?
    size_t bytes{0};
};

/// @brief 妯″瀷寮犻噺鎻忚堪
struct TensorModelDesc {
    std::vector<TensorDesc> inputs;
    std::vector<TensorDesc> outputs;
};

/// @brief 寮傛鎺ㄧ悊杈撳嚭
struct InferOutput {
    bool success{false};
    TensorFrame output;
};

using InferCallback = std::function<void(const InferContext&, InferOutput&&)>;

/// @brief 鎺ㄧ悊寮曟搸鍩虹被
/// 瀹冧粎鍔犺浇妯″瀷銆佽繍琛屾帹鐞嗗苟杩斿洖杈撳嚭寮犻噺
class IInferenceEngine {
public:
    virtual ~IInferenceEngine() = default;

    /// @brief 鍔犺浇妯″瀷
    virtual bool LoadModel(const EngineLoadConfig& config) = 0;
    /// @brief 杩愯鍚屾鎺ㄧ悊
    virtual bool Infer(const TensorFrame& input, TensorFrame& output) = 0;
    /// @brief 杩愯寮傛鎺ㄧ悊
    virtual bool InferAsync(const InferContext& ctx, const TensorFrame& input, InferCallback cb) = 0;
    /// @brief 閲婃斁鎺ㄧ悊寮曟搸璧勬簮
    virtual void Release() = 0;
    /// @brief 鑾峰彇寮曟搸鑳藉姏
    virtual EngineCapability Supports() const = 0;
    /// @brief 鑾峰彇妯″瀷杈撳叆/杈撳嚭寮犻噺鎻忚堪
    virtual TensorModelDesc GetModelDesc() const = 0;
    /// @brief 鑾峰彇鎺ㄧ悊寮曟搸閰嶇疆鍜屼俊鎭?
    virtual EngineConfig GetEngineConfig() const = 0;
};
