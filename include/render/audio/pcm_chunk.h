#pragma once

#include <cstdint>
#include <vector>

namespace render::audio {

/// @brief 音频渲染层内部使用的连续 PCM 数据块。
///
/// PcmChunk 是 Render(frame) 重采样后的队列单位，不绑定 WASAPI。后续如果增加
/// SDL/miniaudio 后端，也可以复用这个数据结构作为“已经适配到设备格式”的 PCM 包。
struct PcmChunk {
    std::vector<std::uint8_t> data;
    std::uint32_t nb_samples{0};
    std::int64_t pts_us{0};
    std::int64_t duration_us{0};
};

} // namespace render::audio
