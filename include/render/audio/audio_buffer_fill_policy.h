#pragma once

#include <cstdint>

namespace render::audio {

/// @brief 一次设备 buffer 填充请求的决策结果。
struct AudioBufferFillRequest {
    std::uint32_t frames_to_request{0};
    bool silence_only{false};
};

/// @brief 供音频 buffer 填充策略读取的运行时上下文。
///
/// 这里刻意把策略输入收拢成一个结构体，而不是继续扩展 Plan() 的参数列表。
/// 后续如果要加入“目标缓冲延迟、音视频时钟偏差、是否允许追帧”等策略输入，只需要给
/// context 增加字段，不必反复修改调用点和策略接口。
struct AudioBufferFillContext {
    std::uint32_t available_frames{0};
    std::uint32_t active_remaining_frames{0};
    std::int64_t queued_frames{0};
    int sample_rate{0};
};

/// @brief 音频设备 buffer 填充策略。
///
/// 该策略不依赖 WASAPI，只根据“设备当前可写帧数、活跃 chunk 剩余帧数、
/// 队列中待写帧数、采样率”决定本轮最多向设备申请多少帧。
class AudioBufferFillPolicy {
public:
    /// @brief 计算本轮应向设备申请的帧数。
    ///
    /// PCM 足够时只申请实际可写出的 PCM 数量；完全没有 PCM 时只申请约 10ms
    /// 的短静音，避免把短时缺口放大成整段设备 buffer 静音。
    AudioBufferFillRequest Plan(const AudioBufferFillContext& context) const;
};

} // namespace render::audio
