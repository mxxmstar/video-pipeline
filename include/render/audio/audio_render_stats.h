#pragma once

#include <cstddef>
#include <cstdint>

namespace render::audio {

/// @brief 音频 renderer 内部播放状态快照。
///
/// RenderSession 只统计提交到 renderer 的 MediaFrame 数量；真正进入 WASAPI
/// 之前还会经过重采样、PCM 队列和声卡事件线程。这个结构体用于把内部队列
/// 深度、underrun 和播放进度暴露给上层诊断。
struct AudioRenderStats {
    std::int64_t submitted_pcm_frames{0};
    std::int64_t queued_pcm_frames{0};
    std::int64_t played_pcm_frames{0};
    std::int64_t dropped_pcm_frames{0};
    std::int64_t dropped_pcm_chunks{0};
    std::int64_t underruns{0};
    std::size_t queued_pcm_chunks{0};
};

} // namespace render::audio
