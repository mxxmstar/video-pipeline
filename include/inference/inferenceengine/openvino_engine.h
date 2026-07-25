#pragma once

#include <atomic>
#include <memory>

#include <openvino/openvino.hpp>

#include "inferenceengine/i_engine.h"
#include "inferenceengine/openvino_request_pool.h"

/// @brief OpenVINO CPU 推理引擎
class OpenVinoCpuEngine : public IInferenceEngine {
public:
    OpenVinoCpuEngine();
    ~OpenVinoCpuEngine() override;

    /// @brief 加载模型
    bool LoadModel(const EngineLoadConfig& config) override;
    /// @brief 运行同步推理
    bool Infer(const TensorFrame& input, TensorFrame& output) override;
    /// @brief 运行异步推理
    bool InferAsync(const InferContext& ctx, const TensorFrame& input, InferCallback cb) override;
    /// @brief 释放推理引擎资源
    void Release() override;
    /// @brief 获取引擎能力
    EngineCapability Supports() const override;
    /// @brief 获取模型输入/输出张量描述
    TensorModelDesc GetModelDesc() const override;
    /// @brief 获取推理引擎配置和信息
    EngineConfig GetEngineConfig() const override;

private:
    /// @brief OpenVINO Core
    ov::Core core_;
    /// @brief 编译模型
    ov::CompiledModel compiled_model_;
    /// @brief 推理请求池
    std::shared_ptr<OpenVinoInferRequestPool> request_pool_;
    /// @brief 引擎配置
    EngineConfig config_;
    /// @brief 模型张量描述
    TensorModelDesc tensor_model_desc_;
    /// @brief OpenVINO 预处理配置
    OpenVinoPreprocessConfig preprocess_config_;
    /// @brief 初始化状态
    bool initialized_{false};
    /// @brief 是否接受新的异步推理请求
    std::atomic<bool> accepting_async_{false};
};
