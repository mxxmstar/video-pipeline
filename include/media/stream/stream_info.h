#pragma once

#include <string>
#include <vector>
#include <cstdio>
#include <variant>

#include "media/media_packet.h"
#include "media/media_frame.h"
#include "common/log/logger.h"

/// @brief 视频流信息
struct VideoStreamInfo {
    int width{0};   ///< 视频宽度
    int height{0};  ///< 视频高度
    float fps{0};   ///< 视频帧率
    PixelFormat pixel_format{PixelFormat::kUnknown}; ///< 视频像素格式
    void Dump() const {
        LOG_INFO("VideoStreamInfo: {}x{}, fps: {}, pixel_format: {}",
                 width, height, fps, static_cast<int>(pixel_format));
    }
};

/// @brief 音频流信息
struct AudioStreamInfo {
    int sample_rate{0}; ///< 音频采样率
    int channels{0};   ///< 音频通道数
    uint64_t channel_layout{0}; ///< 音频通道布局
    SampleFormat sample_format{SampleFormat::Unknown}; ///< 音频采样格式
    void Dump() const {
        LOG_INFO("AudioStreamInfo: sample_rate: {}, channels: {}, channel_layout: {}, sample_format: {}",
                 sample_rate, channels, channel_layout, static_cast<int>(sample_format));
    }
};
/// @brief 媒体流信息
///
/// 描述一路媒体流的编码参数与元数据。
/// 由 IPuller 在连接成功后构造，传递 StreamSession -> MediaStreamSource。
struct MediaStreamInfo {
    MediaType media_type   = MediaType::UNKNOWN; ///< 媒体类型（视频/音频）
    CodecType codec_type   = CodecType::UNKNOWN; ///< 编码格式（H264/H265/AAC/…）
    int       stream_index = -1;                  ///< 流索引
    Rational time_base{1, 1000000}; ///< 时间戳使用的时间基；时间戳为该基准下的 tick
    std::vector<uint8_t> extra_data; ///< 额外数据（如 H.264 SPS/PPS）
    std::variant<VideoStreamInfo, AudioStreamInfo> detail;

    /// @brief 获取指定类型的详细信息
    template<typename T>
    const T& get_detail() const { return std::get<T>(detail); }

    /// @brief 打印流信息到日志
    void Dump(bool dump_extra_data = true) const {
        LOG_INFO("StreamInfo: {}", media_type == MediaType::VIDEO ? "VIDEO" : "AUDIO");
        LOG_INFO("stream_index: {}, codec_type: {}, time_base: {}",
                 stream_index, static_cast<int>(codec_type), time_base.toString());

        if (media_type == MediaType::VIDEO) {
            auto video_info = std::get<VideoStreamInfo>(detail);
            video_info.Dump();
        } else {
            auto audio_info = std::get<AudioStreamInfo>(detail);
            audio_info.Dump();
        }

        if (dump_extra_data) {
            if (!extra_data.empty()) {
                std::string hex;
                for (size_t i = 0; i < extra_data.size() && i < 32; ++i) {
                    char buf[4];
                    std::snprintf(buf, sizeof(buf), "%02x ", extra_data[i]);
                    hex += buf;
                }
                LOG_INFO("extra_data ({} bytes): {}", extra_data.size(), hex);
            } else {
                LOG_INFO("extra_data: empty");
            }
        }
    }
};

struct MultiStreamInfo {
    std::vector<MediaStreamInfo> stream_infos;
    int video_stream_idx_{-1}; ///< index into stream_infos, not FFmpeg stream_index
    int audio_stream_idx_{-1}; ///< index into stream_infos, not FFmpeg stream_index

    bool HasVideoStream() const {
        return video_stream_idx_ >= 0 &&
               video_stream_idx_ < static_cast<int>(stream_infos.size());
    }
    bool HasAudioStream() const {
        return audio_stream_idx_ >= 0 &&
               audio_stream_idx_ < static_cast<int>(stream_infos.size());
    }

    void DumpStream() {
        if (HasVideoStream()) {
            stream_infos[video_stream_idx_].Dump();
        }
        if (HasAudioStream()) {
            stream_infos[audio_stream_idx_].Dump();
        }
    }
};
