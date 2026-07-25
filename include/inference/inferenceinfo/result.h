#pragma once
#include <string>
#include <vector>
#include <memory>
#include <optional>
#if 0
/// @brief  推理结果类型枚举
enum class ResultType {
    UNKNOWN,     ///< 未知结果类型
    DETECTION,   ///< 检测结果类型    
    CLASSIFICATION,  ///< 分类结果类型
    SEGMENTATION,  ///< 分割结果类型
    POSE,          ///< 关键点检测结果类型
    OCR,           ///< 文本识别结果类型
    TRACK,         ///< 跟踪结果类型
    EMBEDDING,     ///< 嵌入结果类型
    LLM            ///< 大语言模型结果类型
};

/// @brief 检测框结构体
struct DetectionBox {
    int class_id;   ///< 检测框的类别ID
    float score;   ///< 检测框的置信度

    float x1;       ///< 检测框左上角x坐标
    float y1;       ///< 检测框左上角y坐标
    float x2;       ///< 检测框右下角x坐标
    float y2;       ///< 检测框右下角y坐标
};

/// @brief 检测结果结构体
struct DetectionResult {
    std::vector<DetectionBox> boxes; ///< 检测框列表
};

/// @brief 分类结果结构体
struct ClassificationResult {
    int class_id;   ///< 分类结果的类别ID
    float score;   ///< 分类结果的置信度
};

/// @brief 分割掩码结构体
struct SegmentationMask {
    int width;   ///< 分割掩码的宽度
    int height;   ///< 分割掩码的高度

    std::vector<uint8_t> mask; ///< 分割掩码数据
};

/// @brief 分割结果结构体
struct SegmentationResult {
    std::vector<SegmentationMask> masks; ///< 分割掩码列表
};


/// @brief 关键点结构体
struct KeyPoint {
    float x;       ///< 关键点x坐标
    float y;       ///< 关键点y坐标
    float score;    ///< 关键点的置信度
};

/// @brief 姿态检测结果结构体
struct PoseResult {
    std::vector<KeyPoint> keypoints; ///< 关键点列表
};
#endif
///////////////////////////////////////////////////////////////////
struct Rectangle {
    float x;       ///< 矩形左上角x坐标
    float y;       ///< 矩形左上角y坐标
    float width;   ///< 矩形的宽度
    float height;  ///< 矩形的高度
};

/// @brief 关键点结构体
struct KeyPoint {
    float x;       ///< 关键点x坐标
    float y;       ///< 关键点y坐标
    float score;    ///< 关键点的置信度
};

/// @brief 对象元数据结构体（第一阶段：目标检测）
struct ObjectMeta {
    int id{-1}; ///< 对象ID
    int class_id{-1}; ///< 对象的类别ID
    float score{0.0f}; ///< 对象的置信度
    Rectangle rect; ///< 对象的矩形区域
};

/// @brief 姿态结果结构体
struct PoseMeta {    
    std::vector<KeyPoint> keypoints; ///< 关键点名称列表
};

/// @brief 分割掩码元数据结构体
struct MaskMeta {
    int width;   ///< 分割掩码的宽度
    int height;   ///< 分割掩码的高度

    std::vector<uint8_t> mask; ///< 分割掩码数据
};

/// @brief 分类元数据结构体（第二阶段：分类）
struct ClassificationMeta {
    int class_id;   ///< 分类结果的类别ID
    float score;   ///< 分类结果的置信度
};

/// @brief 对象结果结构体（第二阶段：分类）
struct ObjectResult {
    ObjectMeta object;  ///< 对象元数据
    std::optional<PoseMeta> pose; ///< 姿态结果
    std::optional<MaskMeta> mask;   ///< 分割掩码结果
    std::optional<ClassificationMeta> cls; ///< 分类结果
};

/// @brief 帧结果结构体（汇总）
struct FrameResult {
    uint64_t frame_id; ///< 帧ID
    uint64_t pts; ///< 时间戳
    std::vector<ObjectResult> objects;  ///< 对象结果列表
};

/// @brief 检测结果结构体
struct DetectionBox {
    int class_id; ///< 检测框的类别ID
    float confidence; ///< 检测框的置信度
    float x1; ///< 检测框左上角x坐标
    float y1; ///< 检测框左上角y坐标
    float x2; ///< 检测框右下角x坐标
    float y2; ///< 检测框右下角y坐标
};