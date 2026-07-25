#pragma once

#include <memory>

#include "model/i_model.h"
#include "postprocessor/i_postprocessor.h"
#include "preprocessor/i_preprocessor.h"

class YoloModel : public IModel {
public:
    /// @brief 初始化模型资源
    /// @param config 模型配置
    /// @return 是否初始化成功
    bool Initialize(const ModelConfig& config) override;

    /// @brief 获取模型元信息
    const ModelMeta& GetModelMeta() const override;

    /// @brief 更新模型元信息
    bool UpdateModelMeta(const ModelMeta& meta) override;

    /// @brief 预处理媒体帧
    /// 将原始 YUV plane 封装成 TensorFrame，让 OpenVINO 内部预处理处理颜色转换和 resize
    /// @param frame 媒体帧数据
    /// @return 处理后的张量帧
    TensorFrame Preprocess(const MediaFrame& frame) override;

    /// @brief 后处理模型输出张量
    /// Decode + NMS + Result
    /// @param output 模型输出张量
    /// @return 视频帧结果
    FrameResult Postprocess(const TensorFrame& output) override;

private:
    /// @brief 重置预处理器
    bool resetPreprocessor();

    /// @brief 重置后处理器
    bool resetPostprocessor();

    std::unique_ptr<IPreprocessor> preprocessor_;   ///< 预处理器
    std::unique_ptr<IPostprocessor> postprocessor_; ///< 后处理器
    bool initialized_{false}; ///< 初始化状态
};
