#pragma once
/// @file ffmpeg_decoder.hpp
/// 基于 FFmpeg libavcodec 的软件解码器实现。

#include "media/decoder/i_decoder.h"

#include <mutex>

extern "C" {
#include <libavcodec/avcodec.h>
}

/// @brief FFmpeg 软件解码器
///
/// 实现 IDecoder 接口，使用 FFmpeg libavcodec 完成 H.264/H.265/AAC/OPUS 等格式解码。
///
/// 数据流：
///   MediaPacket (AVPacket backend)
///     -> avcodec_send_packet()
///     -> loop avcodec_receive_frame()
///     -> FFmpegFrameBuffer + MediaFrame
///     -> FrameCallback
///
/// 注意：
///   - 暂不支持硬件加速解码
///   - 解码器状态由 avcodec 内部管理，无需额外包排序缓冲
class FFmpegDecoder : public IDecoder {
public:
    FFmpegDecoder() = default;
    ~FFmpegDecoder() override;

    // 禁止拷贝
    FFmpegDecoder(const FFmpegDecoder&) = delete;
    FFmpegDecoder& operator=(const FFmpegDecoder&) = delete;

    // ==================== IDecoder ====================

    bool Open(const MediaStreamInfo& info) override;
    void Close() override;
    bool Decode(std::shared_ptr<MediaPacket> packet) override;
    void SetFrameCallback(FrameCallback cb) override;

    // ==================== 工具 ====================

    /// @brief AVCodecID -> CodecType 映射
    static CodecType MapAVCodecID(AVCodecID id);

    /// @brief AVPixelFormat -> PixelFormat 映射
    static PixelFormat MapAVPixelFormat(AVPixelFormat fmt);

    /// @brief AVSampleFormat -> SampleFormat 映射
    static SampleFormat MapAVSampleFormat(AVSampleFormat fmt);

private:
    /// @brief 接收所有已解码帧并回调
    /// @return true 成功（至少零帧已回调），false 解码器错误
    bool ReceiveFrames();

    AVCodecContext* codec_ctx_{nullptr};  ///< FFmpeg 解码器上下文
    MediaStreamInfo      stream_info_;          ///< 解码器打开的流信息
    FrameCallback   frame_cb_;             ///< 解码帧回调
    std::mutex      cb_mutex_;             ///< 保护 frame_cb_ setter
};