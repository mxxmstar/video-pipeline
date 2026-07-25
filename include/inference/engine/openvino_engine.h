#pragma once

#include <atomic>
#include <memory>

#include <openvino/openvino.hpp>

#include "engine/i_engine.h"
#include "engine/openvino_request_pool.h"

/// @brief OpenVINO CPU 鎺ㄧ悊寮曟搸
class OpenVinoCpuEngine : public IInferenceEngine {
public:
    OpenVinoCpuEngine();
    ~OpenVinoCpuEngine() override;

    /// @brief 鍔犺浇妯″瀷
    bool LoadModel(const EngineLoadConfig& config) override;
    /// @brief 杩愯鍚屾鎺ㄧ悊
    bool Infer(const TensorFrame& input, TensorFrame& output) override;
    /// @brief 杩愯寮傛鎺ㄧ悊
    bool InferAsync(const InferContext& ctx, const TensorFrame& input, InferCallback cb) override;
    /// @brief 閲婃斁鎺ㄧ悊寮曟搸璧勬簮
    void Release() override;
    /// @brief 鑾峰彇寮曟搸鑳藉姏
    EngineCapability Supports() const override;
    /// @brief 鑾峰彇妯″瀷杈撳叆/杈撳嚭寮犻噺鎻忚堪
    TensorModelDesc GetModelDesc() const override;
    /// @brief 鑾峰彇鎺ㄧ悊寮曟搸閰嶇疆鍜屼俊鎭?
    EngineConfig GetEngineConfig() const override;

private:
    /// @brief OpenVINO Core
    ov::Core core_;
    /// @brief 缂栬瘧妯″瀷
    ov::CompiledModel compiled_model_;
    /// @brief 鎺ㄧ悊璇锋眰姹?
    std::shared_ptr<OpenVinoInferRequestPool> request_pool_;
    /// @brief 寮曟搸閰嶇疆
    EngineConfig config_;
    /// @brief 妯″瀷寮犻噺鎻忚堪
    TensorModelDesc tensor_model_desc_;
    /// @brief OpenVINO 棰勫鐞嗛厤缃?
    OpenVinoPreprocessConfig preprocess_config_;
    /// @brief 鍒濆鍖栫姸鎬?
    bool initialized_{false};
    /// @brief 鏄惁鎺ュ彈鏂扮殑寮傛鎺ㄧ悊璇锋眰
    std::atomic<bool> accepting_async_{false};
};
