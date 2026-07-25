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
    ASYNC = 1u << 0, ///< 是否支持异步推理
    DYNAMIC = 1u << 1, ///< 是否支持动态形状
    BATCH = 1u << 2, ///< 是否支持批量推理
    CPU = 1u << 3, ///< 是否支持 CPU 推理
    GPU = 1u << 4, ///< 是否支持 GPU 推理
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

/// @brief 推理引擎配置和信息
struct EngineConfig {
    /// @brief 模型文件路径
    std::string model_path;
    /// @brief 后端类型
    std::string backend{"OPENVINO"};
    /// @brief 设备类型
    std::string device{"CPU"};
    /// @brief 批量大小
    int batch_size{1};
    /// @brief 推理请求池大小
    uint32_t request_count{1};
    /// @brief 是否支持异步推理
    bool support_async{false};
    /// @brief 是否支持动态形状
    bool support_dynamic_shape{false};
    /// @brief 最大批量大小
    uint32_t max_batch_size{1};
};

/// @brief OpenVINO 预处理配置
struct OpenVinoPreprocessConfig {
    /// @brief 是否启用 OpenVINO 内部预处理
    bool enabled{false};
    /// @brief OpenVINO 预处理输入像素格式
    PixelFormat input_pixel_format{PixelFormat::kNV12};
    /// @brief 模型期望的像素格式
    PixelFormat model_pixel_format{PixelFormat::kRGB24};
    /// @brief 模型输入布局，空字符串表示从模型输入形状推断
    std::string model_input_layout;
    /// @brief OpenVINO 缩放因子，0 表示不进行缩放
    float scale{255.0f};
};

/// @brief 推理引擎加载配置
struct EngineLoadConfig {
    EngineConfig engine;
    std::optional<OpenVinoPreprocessConfig> preprocess; // openvino 自带预处理
};

/// @brief 推理上下文
struct InferContext {
    /// @brief 帧 ID
    uint64_t frame_id{0};
    /// @brief 时间戳
    uint64_t pts{0};
};

/// @brief 输入/输出张量描述
struct TensorDesc {
    /// @brief 张量名称
    std::string name;
    /// @brief 张量元素类型
    TensorType type{TensorType::UNKNOWN};
    /// @brief 张量形状
    TensorShape shape;
    /// @brief 是否动态形状
    bool dynamic_shape{false};
    /// @brief 是否输入张量
    bool is_input{true};
    /// @brief 张量元素数量
    size_t element_count{0};
    /// @brief 张量字节数
    size_t bytes{0};
};

/// @brief 模型张量描述
struct TensorModelDesc {
    std::vector<TensorDesc> inputs;
    std::vector<TensorDesc> outputs;
};

/// @brief 异步推理输出
struct InferOutput {
    bool success{false};
    TensorFrame output;
};

using InferCallback = std::function<void(const InferContext&, InferOutput&&)>;

/// @brief 推理引擎基类
/// 它仅加载模型、运行推理并返回输出张量
class IInferenceEngine {
public:
    virtual ~IInferenceEngine() = default;

    /// @brief 加载模型
    virtual bool LoadModel(const EngineLoadConfig& config) = 0;
    /// @brief 运行同步推理
    virtual bool Infer(const TensorFrame& input, TensorFrame& output) = 0;
    /// @brief 运行异步推理
    virtual bool InferAsync(const InferContext& ctx, const TensorFrame& input, InferCallback cb) = 0;
    /// @brief 释放推理引擎资源
    virtual void Release() = 0;
    /// @brief 获取引擎能力
    virtual EngineCapability Supports() const = 0;
    /// @brief 获取模型输入/输出张量描述
    virtual TensorModelDesc GetModelDesc() const = 0;
    /// @brief 获取推理引擎配置和信息
    virtual EngineConfig GetEngineConfig() const = 0;
};
