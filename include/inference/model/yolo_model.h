#pragma once

#include <memory>

#include "model/i_model.h"
#include "postprocessor/i_postprocessor.h"
#include "preprocessor/i_preprocessor.h"

class YoloModel : public IModel {
public:
    /// @brief 鍒濆鍖栨ā鍨嬭祫婧?
    /// @param config 妯″瀷閰嶇疆
    /// @return 鏄惁鍒濆鍖栨垚鍔?
    bool Initialize(const ModelConfig& config) override;

    /// @brief 鑾峰彇妯″瀷鍏冧俊鎭?
    const ModelMeta& GetModelMeta() const override;

    /// @brief 鏇存柊妯″瀷鍏冧俊鎭?
    bool UpdateModelMeta(const ModelMeta& meta) override;

    /// @brief 棰勫鐞嗗獟浣撳抚
    /// 灏嗗師濮?YUV plane 灏佽鎴?TensorFrame锛岃 OpenVINO 鍐呴儴棰勫鐞嗗鐞嗛鑹茶浆鎹㈠拰 resize
    /// @param frame 濯掍綋甯ф暟鎹?
    /// @return 澶勭悊鍚庣殑寮犻噺甯?
    TensorFrame Preprocess(const MediaFrame& frame) override;

    /// @brief 鍚庡鐞嗘ā鍨嬭緭鍑哄紶閲?
    /// Decode + NMS + Result
    /// @param output 妯″瀷杈撳嚭寮犻噺
    /// @return 瑙嗛甯х粨鏋?
    FrameResult Postprocess(const TensorFrame& output) override;

private:
    /// @brief 閲嶇疆棰勫鐞嗗櫒
    bool resetPreprocessor();

    /// @brief 閲嶇疆鍚庡鐞嗗櫒
    bool resetPostprocessor();

    std::unique_ptr<IPreprocessor> preprocessor_;   ///< 棰勫鐞嗗櫒
    std::unique_ptr<IPostprocessor> postprocessor_; ///< 鍚庡鐞嗗櫒
    bool initialized_{false}; ///< 鍒濆鍖栫姸鎬?
};
