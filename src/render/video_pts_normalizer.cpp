#include "render/video_pts_normalizer.h"

#include <algorithm>
#include <cstdlib>

namespace render {

namespace {

std::int64_t MillisecondsToUs(int value_ms) {
    // 配置允许传入 0 或负数。这里统一夹到非负，避免后续阈值比较出现反直觉结果。
    return static_cast<std::int64_t>(std::max(0, value_ms)) * 1000;
}

} // namespace

VideoPtsNormalizer::VideoPtsNormalizer(VideoPtsNormalizerConfig config)
    : config_(config) {}

void VideoPtsNormalizer::Reset() {
    last_input_pts_us_ = 0;
    last_output_pts_us_ = 0;
    has_last_ = false;
}

void VideoPtsNormalizer::SetConfig(VideoPtsNormalizerConfig config) {
    config_ = config;
    // 配置变化后，旧的 last_input/last_output 已经不再一定匹配新策略。
    // 直接 Reset 可以避免新旧阈值或 fallback fps 混用。
    Reset();
}

const VideoPtsNormalizerConfig& VideoPtsNormalizer::Config() const {
    return config_;
}

VideoPtsNormalizeResult VideoPtsNormalizer::Normalize(
    std::int64_t input_pts_us,
    std::int64_t duration_us) {
    if (!config_.enabled) {
        // 关闭归一化时保持透明：调用方仍通过同一个接口拿结果，但不改变任何时间戳。
        return {input_pts_us, false, VideoPtsNormalizeReason::None};
    }

    if (!has_last_) {
        // 第一帧没有“上一帧”可以参考，因此只做最小修正：
        // 正常的 0 起始 PTS 会被保留；负数 PTS 则夹到 0，避免播放时间轴从负数开始。
        const auto first_pts = std::max<std::int64_t>(0, input_pts_us);
        last_input_pts_us_ = input_pts_us;
        last_output_pts_us_ = first_pts;
        has_last_ = true;
        return {first_pts,
                first_pts != input_pts_us,
                first_pts == input_pts_us
                    ? VideoPtsNormalizeReason::None
                    : VideoPtsNormalizeReason::MissingOrRepeated};
    }

    const auto expected_pts_us =
        last_output_pts_us_ + FrameDurationUs(duration_us);

    // expected_pts_us 表示“如果这一帧需要兜底，应该落在连续时间轴上的位置”。
    // 下面先只判断原始 PTS 是否可信；真正选择 input 还是 expected 放在最后统一处理。
    VideoPtsNormalizeReason reason = VideoPtsNormalizeReason::None;
    if (input_pts_us <= 0 || input_pts_us == last_input_pts_us_) {
        // 除第一帧外，<=0 或与上一帧完全相同通常意味着 PTS 缺失/重复。
        // 对实时预览来说，重复 PTS 会让多帧视频挤在同一个播放时刻，必须展开。
        reason = VideoPtsNormalizeReason::MissingOrRepeated;
    } else if (input_pts_us < last_input_pts_us_) {
        // 输入 PTS 倒退会让 AV sync 误以为当前帧严重落后，从而被错误丢弃。
        reason = VideoPtsNormalizeReason::Backward;
    } else if (IsLargeJump(input_pts_us, expected_pts_us)) {
        // 大跳变可能来自流重连、摄像头时间戳重置或上游 time_base 处理异常。
        // 第一版选择保持连续输出，优先保证本地预览稳定。
        reason = VideoPtsNormalizeReason::LargeJump;
    }

    const bool normalized = reason != VideoPtsNormalizeReason::None;
    const auto output_pts_us = normalized ? expected_pts_us : input_pts_us;
    // last_input 记录原始输入，用于继续识别“输入端是否重复/倒退”；
    // last_output 记录归一化后的连续时间轴，用于下一帧 fallback。
    last_input_pts_us_ = input_pts_us;
    last_output_pts_us_ = output_pts_us;
    return {output_pts_us, normalized, reason};
}

std::int64_t VideoPtsNormalizer::FrameDurationUs(std::int64_t duration_us) const {
    if (duration_us > 0) {
        // 解码器能给出可信 duration 时优先使用它，避免把可变帧率强行量化为固定 fps。
        return duration_us;
    }

    // fallback_fps 至少为 1，避免除零；配置错误时宁可得到 1fps 的保守时间轴。
    const auto fps = std::max(1, config_.fallback_fps);
    return 1'000'000LL / fps;
}

bool VideoPtsNormalizer::IsLargeJump(
    std::int64_t input_pts_us,
    std::int64_t expected_pts_us) const {
    const auto max_jump_us = MillisecondsToUs(config_.max_jump_ms);
    if (max_jump_us <= 0) {
        // max_jump_ms <= 0 表示关闭“大跳变”检测，但重复/倒退仍会被处理。
        return false;
    }

    // 使用绝对偏差：既能发现 PTS 突然跳到未来，也能发现 PTS 相对预期严重滞后。
    return std::llabs(input_pts_us - expected_pts_us) > max_jump_us;
}

} // namespace render
