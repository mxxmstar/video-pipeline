#pragma once
/// @file ffmpeg_frame_buffer.hpp
/// FFmpeg AVFrame 包装器，将多平面 AVFrame pack 为连续内存后适配 IMediaBuffer 接口。

#include "media/i_media_buffer.h"

struct AVFrame;

/// @brief FFmpeg 解码帧的分层存活内存快照。
///
/// 该统计只描述当前仍被 FFmpegFrameBuffer 持有的对象，不代表进程的 RSS。
/// 其中 packed_bytes 是 MediaFrame 对外暴露的连续缓冲区，av_buffer_bytes 是
/// AVFrame 仍然引用的 FFmpeg 解码缓冲区。两者同时存在时，能够直接观察当前
/// 实现的双份数据占用；peak_* 用于识别突发解码阶段的最高存活量。
struct FFmpegFrameMemoryStats {
    size_t live_wrappers{0};
    size_t live_packed_bytes{0};
    size_t live_av_buffer_bytes{0};
    size_t peak_wrappers{0};
    size_t peak_packed_bytes{0};
    size_t peak_av_buffer_bytes{0};
};


/// @brief 包装 FFmpeg AVFrame 的 IMediaBuffer 实现
/// @note 构造函数会将 AVFrame 的多平面数据打包为一块连续内存
class FFmpegFrameBuffer : public IMediaBuffer {
public:
    /// 接管 frame 所有权，将各平面数据复制到 packed_data_ 连续内存
    explicit FFmpegFrameBuffer(AVFrame* frame, size_t total_size);
    ~FFmpegFrameBuffer() override;
    uint8_t* Data() override;
    const uint8_t* Data() const override;
    size_t Size() const override;

    /// @brief 获取所有 FFmpegFrameBuffer 实例的线程安全内存快照。
    static FFmpegFrameMemoryStats GetMemoryStats();

    /// 获取内部的原始 AVFrame 指针
    AVFrame* GetFrame() const { return frame_; }

private:
    AVFrame* frame_{nullptr};         ///< 被包装的 AVFrame（析构时释放）
    uint8_t* packed_data_{nullptr};   ///< 连续 packed 后的数据
    size_t   packed_size_{0};         ///< packed 数据总大小
    size_t   av_buffer_size_{0};      ///< frame 引用的 FFmpeg AVBuffer payload 大小
    bool     stats_registered_{false}; ///< 是否已经计入全局存活统计
};
