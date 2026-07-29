#pragma once

#include <cstdint>

namespace render {

/// @brief 视频 PTS 归一化配置。
struct VideoPtsNormalizerConfig {
    /// 是否启用归一化。关闭后原样返回输入 PTS。
    bool enabled{true};

    /// 当 frame.duration_us 不可用时使用的 fallback FPS。
    ///
    /// 例如摄像头没有提供准确 duration 时，fallback_fps=25 会生成 40ms 的连续帧间隔。
    /// 该值只影响异常 PTS 的兜底生成，不会改写正常单调递增的输入 PTS。
    int fallback_fps{30};

    /// 输入 PTS 与预期 PTS 偏差超过该阈值时，认为发生明显跳变。
    ///
    /// 阈值不能太小，否则普通网络 jitter 会被误判；第一版使用 1000ms，只处理足以
    /// 破坏 AV sync 决策的大幅跳变。
    int max_jump_ms{1000};
};

/// @brief PTS 归一化原因。
enum class VideoPtsNormalizeReason {
    /// 输入 PTS 正常，未做修正。
    None,
    /// PTS 缺失、重复，或除首帧外仍为 0/负数。
    MissingOrRepeated,
    /// 输入 PTS 小于上一帧输入 PTS。
    Backward,
    /// 输入 PTS 与连续时间轴预期值偏差过大。
    LargeJump,
};

/// @brief 一次视频 PTS 归一化结果。
struct VideoPtsNormalizeResult {
    /// 供 AV sync 使用的最终 PTS。
    std::int64_t pts_us{0};
    /// 是否发生过归一化修正。
    bool normalized{false};
    /// 发生修正时的原因；未修正时为 None。
    VideoPtsNormalizeReason reason{VideoPtsNormalizeReason::None};
};

/// @brief 面向实时预览的轻量视频 PTS 归一化器。
///
/// FFmpeg/RTSP 摄像头流偶尔会出现 PTS 缺失、重复、倒退或大幅跳变。AV sync 如果直接
/// 使用这些异常值，会把正常视频帧误判为“极早”或“极晚”。该类只修正明显异常：
/// - 第一帧保留输入 PTS；如果第一帧没有有效 PTS，则从 0 开始。
/// - 后续帧 PTS 重复/缺失/倒退时，按上一帧输出 PTS + 帧间隔生成。
/// - 后续帧相对预期值发生大跳变时，也按连续时间轴生成。
///
/// 它不改变帧内容，也不做复杂 jitter buffer；目标是给阶段 4 AV sync 一个稳定的
/// 第一版时间轴输入。
class VideoPtsNormalizer {
public:
    explicit VideoPtsNormalizer(VideoPtsNormalizerConfig config = {});

    void Reset();
    void SetConfig(VideoPtsNormalizerConfig config);
    const VideoPtsNormalizerConfig& Config() const;

    /// @brief 归一化一帧视频 PTS。
    /// @param input_pts_us 解码帧原始 PTS，单位微秒。
    /// @param duration_us 解码帧持续时间，单位微秒；<=0 时使用 fallback_fps。
    /// @return 可直接用于 AV sync 的稳定 PTS。
    VideoPtsNormalizeResult Normalize(std::int64_t input_pts_us,
                                      std::int64_t duration_us);

private:
    /// @brief 返回本帧用于生成 fallback PTS 的帧间隔。
    std::int64_t FrameDurationUs(std::int64_t duration_us) const;

    /// @brief 判断输入 PTS 是否相对连续时间轴发生明显跳变。
    bool IsLargeJump(std::int64_t input_pts_us,
                     std::int64_t expected_pts_us) const;

    VideoPtsNormalizerConfig config_;
    /// 上一帧原始输入 PTS，用于判断重复和倒退。
    std::int64_t last_input_pts_us_{0};
    /// 上一帧输出 PTS，用于生成连续 fallback 时间轴。
    std::int64_t last_output_pts_us_{0};
    /// 是否已经处理过至少一帧。
    bool has_last_{false};
};

} // namespace render
