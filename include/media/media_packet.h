#pragma once
/// @file media_packet.hpp
/// 媒体包（Packet）定义，包含编码数据及其元信息。

#include <cstdint>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>

#include "media/i_media_buffer.h"
#include "common/log/logger.h"
/// 媒体流类型
enum class MediaType : int {
    // 数值是公共数据契约的一部分，不能依赖枚举成员声明顺序。
    // UNKNOWN 必须与任何真实媒体类型不同，否则“未知类型”会被误判为视频。
    UNKNOWN = 0,      ///< 未知
    VIDEO = 1,        ///< 视频
    AUDIO = 2,        ///< 音频
    PCAP = 3,         ///< pcap 数据包
};

/// 编码格式（值参考 FFmpeg 的 AVCodecID）
enum class CodecType : int {
    UNKNOWN = 0,
    H264    = 27,   // AV_CODEC_ID_H264
    H265    = 173,  // AV_CODEC_ID_HEVC
    AAC     = 86018,// AV_CODEC_ID_AAC
    OPUS    = 86097,// AV_CODEC_ID_OPUS
    G711A   = 65543,// AV_CODEC_ID_PCM_ALAW
    G711U   = 65542,// AV_CODEC_ID_PCM_MULAW
    G726    = 69643,// AV_CODEC_ID_ADPCM_G726
    JPEG    = 7,    // AV_CODEC_ID_MJPEG
};

/// 后端引擎句柄，用于传递特定引擎的内部对象指针
struct BackendHandle {
    enum Type {
        NONE = 0,     ///< 无后端
        FFMPEG,       ///< FFmpeg AVPacket / AVFrame
        OPENH264,     ///< OpenH264 编码器
        WEBRTC,       ///< WebRTC 内部缓冲区
    };
    Type type{NONE};  ///< 后端类型
    void* ptr{nullptr};///< 后端内部对象指针
};

/// @brief 有理分数，用于表示时间基等
struct Rational {
    int num{1};
    int den{1};
    std::string toString() const {
        return std::to_string(num) + "/" + std::to_string(den);
    }
};

/// @brief 统一表示“没有时间戳”的值。
///
/// 0 是合法的媒体时间点（例如第一帧从 0 开始），因此不能再用 0 表示
/// 时间戳缺失。该值与 FFmpeg 的 AV_NOPTS_VALUE 数值一致，但公共头文件
/// 不依赖 FFmpeg 头文件。
inline constexpr std::int64_t kNoTimestamp =
    (std::numeric_limits<std::int64_t>::min)();

/// @brief 判断时间戳是否有效。
inline constexpr bool IsValidTimestamp(std::int64_t timestamp) {
    return timestamp != kNoTimestamp;
}

/// @brief 判断时间基是否可以用于整数时间戳换算。
inline constexpr bool IsValidTimeBase(const Rational& value) {
    return value.num > 0 && value.den > 0;
}

/// 媒体包：一个编码帧或编码帧分片的数据及其描述信息
class MediaPacket {
public:
    MediaType  type{MediaType::UNKNOWN};         ///< 媒体流类型
    CodecType  codec{CodecType::UNKNOWN};        ///< 编码格式
    int stream_index{-1 };                       ///< 流索引

    // pts/dts/duration 都是 time_base 下的整数 tick，不是固定微秒。
    // 需要微秒时必须按 time_base 显式重标定，不能直接比较不同来源的值。
    int64_t    pts{kNoTimestamp};                 ///< 显示时间戳（time_base tick）
    int64_t    dts{kNoTimestamp};                 ///< 解码时间戳（time_base tick）
    int64_t duration{kNoTimestamp};               ///< 持续时间（time_base tick）
    Rational time_base{1, 1000000};                ///< 时间戳使用的时间基

    bool       keyframe{false};                   ///< 是否为关键帧
    std::shared_ptr<IMediaBuffer> buffer;          ///< 编码数据载荷
    BackendHandle backend;                         ///< 后端引擎句柄

    void Dump() const {
        LOG_INFO("MediaPacket: type={}, codec={}, stream_index={}, pts={}, dts={}, duration={}, time_base={}, keyframe={}, buffer_size={}",
                 (int)type, (int)codec, stream_index, pts, dts, duration,
                 time_base.toString(), keyframe, buffer ? buffer->Size() : 0);
    }
};
