#pragma once
/// @file media_frame.hpp
/// 视频帧（Frame）定义，包含解码后的图像数据及其元信息。

#include <cstdint>
#include <cstddef>
#include <variant>
#include <memory>
#include "i_media_buffer.h"
#include "media_packet.h"

/// 像素格式
enum class PixelFormat {
    kUnknown = 0,
    kNV12,     ///< YUV420sp (Y + UV 交错)
    kNV21,     ///< YUV420sp (UV 顺序相反)
    kI420,     ///< YUV420p (三个平面)
    kBGR24,    ///< BGR 24 位
    kRGB24,    ///< RGB 24 位
    kGRAY8,    ///< 8 位灰度
};

/// @brief 音频采样格式
enum class SampleFormat {
    Unknown = 0,
    U8,
    S16,
    S32,
    FLT,
    DBL,
    U8P,
    S16P,
    S32P,
    FLTP,
    DBLP,
};

/// @brief 时间戳
struct MediaTime {
    int64_t pts_us{0};   ///< 显示时间戳（微秒）
    int64_t dts_us{0};   ///< 解码时间戳（微秒）
    int64_t duration_us{0}; ///< 持续时间（微秒）
};

/// @brief 平面信息
struct PlaneInfo {
    int32_t offset{0}; ///< 平面数据偏移（字节）
    int32_t stride{0}; ///< 平面行跨度（字节）  
    int32_t size{0};     ///< 平面数据大小（字节）
};

/// @brief struct VideoFrameMeta 视频帧元数据
struct VideoFrameMeta {
    PixelFormat pixel_format{PixelFormat::kUnknown}; ///< 像素格式
    int32_t     width{0};                           ///< 图像宽度（像素）
    int32_t     height{0};                          ///< 图像高度（像素）
    int32_t     plane_count{0};                        ///< 平面数量
    PlaneInfo   plane_info[8] {0}; ///< 平面信息
};

struct AudioFrameMeta {
    SampleFormat sample_format{SampleFormat::Unknown};  ///< 采样格式
    int sample_rate{0};                              ///< 采样率（Hz）
    int channels{0};                                 ///< 通道数
    uint64_t channel_layout{0};                      ///< 通道布局
    int nb_samples{0};                               ///< 样本数
    int bytes_per_sample{0};                        ///< 每个样本字节数
    bool planar{false};                             ///< 是否为平面格式
    int32_t plane_count{0};                         ///< 平面数量
    PlaneInfo planes[8]{};                          ///< 平面信息
};

using FrameMeta = std::variant<VideoFrameMeta, AudioFrameMeta>;

/// 媒体帧：解码后的图像数据及其描述信息
class MediaFrame {
public:
    MediaType    type{MediaType::VIDEO};              ///< 媒体类型（VIDEO/AUDIO）
    MediaTime   time;                               ///< 时间戳信息
    FrameMeta meta;                                 ///< 媒体帧元数据            

    std::shared_ptr<IMediaBuffer> buffer;           ///< packed 图像数据
    BackendHandle backend;                          ///< 后端引擎句柄

    /// @brief 获取视频元数据（若 type 为 VIDEO）
    const VideoFrameMeta* VideoMeta() const {
        if (type == MediaType::VIDEO) {
            return &std::get<VideoFrameMeta>(meta);
        }
        return nullptr;
    }

    /// @brief 获取音频元数据（若 type 为 AUDIO）
    const AudioFrameMeta* AudioMeta() const {
        if (type == MediaType::AUDIO) {
            return &std::get<AudioFrameMeta>(meta);
        }
        return nullptr;
    }

    /// @brief 获取视频宽度
    int32_t Width() const {
        if (auto* v = VideoMeta()) return v->width;
        return 0;
    }

    /// @brief 获取视频高度
    int32_t Height() const {
        if (auto* v = VideoMeta()) return v->height;
        return 0;
    }

    /// @brief 获取像素格式
    PixelFormat GetPixelFormat() const {
        if (auto* v = VideoMeta()) return v->pixel_format;
        return PixelFormat::kUnknown;
    }

    /// @brief 获取平面数量
    int32_t PlaneCount() const {
        if (auto* v = VideoMeta()) return v->plane_count;
        if (auto* a = AudioMeta()) return a->plane_count;
        return 0;
    }

    /// @brief 获取指定平面的 stride
    int32_t Stride(int index) const {
        if (auto* v = VideoMeta()) return v->plane_info[index].stride;
        if (auto* a = AudioMeta()) return a->planes[index].stride;
        return 0;
    }

    /// @brief 获取指定平面的 offset
    int32_t PlaneOffset(int index) const {
        if (auto* v = VideoMeta()) return v->plane_info[index].offset;
        if (auto* a = AudioMeta()) return a->planes[index].offset;
        return 0;
    }

    /// @brief 获取样本数（音频）
    int NbSamples() const {
        if (auto* a = AudioMeta()) return a->nb_samples;
        return 0;
    }

    /// @brief 获取采样率（音频）
    int SampleRate() const {
        if (auto* a = AudioMeta()) return a->sample_rate;
        return 0;
    }

    /// @brief 获取通道数（音频）
    int Channels() const {
        if (auto* a = AudioMeta()) return a->channels;
        return 0;
    }

    /// @brief 获取采样格式（音频）
    SampleFormat SampleFmt() const {
        if (auto* a = AudioMeta()) return a->sample_format;
        return SampleFormat::Unknown;
    }
};