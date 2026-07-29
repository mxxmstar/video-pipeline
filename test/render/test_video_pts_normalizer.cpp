#include "render/video_pts_normalizer.h"

#include <iostream>

namespace {

bool TestKeepsMonotonicPts() {
    // 正常单调递增的 PTS 必须原样保留。normalizer 只修明显异常，不能把
    // 上游已经正确的时间戳重新量化。
    render::VideoPtsNormalizer normalizer;
    const auto first = normalizer.Normalize(0, 40'000);
    const auto second = normalizer.Normalize(40'000, 40'000);
    const auto third = normalizer.Normalize(80'000, 40'000);

    return !first.normalized &&
           !second.normalized &&
           !third.normalized &&
           first.pts_us == 0 &&
           second.pts_us == 40'000 &&
           third.pts_us == 80'000;
}

bool TestRepeatingPtsUsesExpectedDuration() {
    // 很多实时流在 PTS 缺失时会连续给 0 或重复上一帧 PTS。这里验证第二帧
    // 重复 0 时会按 duration 生成 40ms。
    render::VideoPtsNormalizer normalizer;
    normalizer.Normalize(0, 40'000);

    const auto repeated = normalizer.Normalize(0, 40'000);
    return repeated.normalized &&
           repeated.reason == render::VideoPtsNormalizeReason::MissingOrRepeated &&
           repeated.pts_us == 40'000;
}

bool TestBackwardPtsUsesExpectedDuration() {
    // PTS 倒退会让 AV sync 误判为严重晚帧。normalizer 应继续沿上一帧输出
    // 时间轴推进，而不是信任倒退的输入值。
    render::VideoPtsNormalizer normalizer;
    normalizer.Normalize(100'000, 40'000);

    const auto backward = normalizer.Normalize(80'000, 40'000);
    return backward.normalized &&
           backward.reason == render::VideoPtsNormalizeReason::Backward &&
           backward.pts_us == 140'000;
}

bool TestLargeJumpUsesExpectedDuration() {
    // 大跳变常见于摄像头重连或时间戳基准变化。第一版策略选择本地连续播放，
    // 因此 0 -> 1s 的突跳会被修正为下一帧 40ms。
    render::VideoPtsNormalizerConfig config;
    config.max_jump_ms = 100;
    render::VideoPtsNormalizer normalizer(config);
    normalizer.Normalize(0, 40'000);

    const auto jumped = normalizer.Normalize(1'000'000, 40'000);
    return jumped.normalized &&
           jumped.reason == render::VideoPtsNormalizeReason::LargeJump &&
           jumped.pts_us == 40'000;
}

bool TestFallbackFpsWhenDurationMissing() {
    // 如果解码帧没有 duration，normalizer 用 fallback_fps 生成连续间隔。
    // 25fps 对应 40ms。
    render::VideoPtsNormalizerConfig config;
    config.fallback_fps = 25;
    render::VideoPtsNormalizer normalizer(config);
    normalizer.Normalize(0, 0);

    const auto missing = normalizer.Normalize(0, 0);
    return missing.normalized && missing.pts_us == 40'000;
}

} // namespace

int main() {
    if (!TestKeepsMonotonicPts()) {
        std::cerr << "TestKeepsMonotonicPts failed\n";
        return 1;
    }
    if (!TestRepeatingPtsUsesExpectedDuration()) {
        std::cerr << "TestRepeatingPtsUsesExpectedDuration failed\n";
        return 1;
    }
    if (!TestBackwardPtsUsesExpectedDuration()) {
        std::cerr << "TestBackwardPtsUsesExpectedDuration failed\n";
        return 1;
    }
    if (!TestLargeJumpUsesExpectedDuration()) {
        std::cerr << "TestLargeJumpUsesExpectedDuration failed\n";
        return 1;
    }
    if (!TestFallbackFpsWhenDurationMissing()) {
        std::cerr << "TestFallbackFpsWhenDurationMissing failed\n";
        return 1;
    }

    std::cout << "Video PTS normalizer tests passed\n";
    return 0;
}
