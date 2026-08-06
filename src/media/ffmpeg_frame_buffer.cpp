// @file ffmpeg_frame_buffer.cpp
// FFmpeg AVFrame wrapper implementation: AVFrame data to packed memory.
#include "media/ffmpeg_frame_buffer.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
}

namespace {

std::atomic<size_t> g_live_wrappers{0};
std::atomic<size_t> g_live_packed_bytes{0};
std::atomic<size_t> g_live_av_buffer_bytes{0};
std::atomic<size_t> g_peak_wrappers{0};
std::atomic<size_t> g_peak_packed_bytes{0};
std::atomic<size_t> g_peak_av_buffer_bytes{0};

void UpdatePeak(std::atomic<size_t>& peak, size_t value) {
    size_t current = peak.load(std::memory_order_relaxed);
    while (current < value &&
           !peak.compare_exchange_weak(current, value,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
    }
}

/// @brief 计算 AVFrame 当前引用的 FFmpeg 数据缓冲区大小。
///
/// AVFrame 既可能使用固定大小的 buf 数组，也可能使用 extended_buf 保存
/// 更多平面。FFmpeg 约定 extended_buf 只保存 buf 数组之外的额外引用，因而
/// 这里直接累加两组引用，避免每个解码帧都为诊断统计分配临时容器。
size_t FrameBufferBytes(const AVFrame* frame) {
    if (!frame) {
        return 0;
    }

    size_t total = 0;
    const auto add = [&](const AVBufferRef* buffer) {
        if (buffer && buffer->size > 0) {
            total += static_cast<size_t>(buffer->size);
        }
    };

    for (const AVBufferRef* buffer : frame->buf) {
        add(buffer);
    }
    for (int i = 0; i < frame->nb_extended_buf; ++i) {
        add(frame->extended_buf[i]);
    }
    return total;
}

bool IsAudioFrame(const AVFrame* frame) {
    return frame && frame->nb_samples > 0 && frame->ch_layout.nb_channels > 0;
}

bool IsVideoFrame(const AVFrame* frame) {
    return frame && frame->width > 0 && frame->height > 0;
}

void CopyBounded(uint8_t*& dst, size_t& remaining, const uint8_t* src, size_t size) {
    if (!src || !dst || remaining == 0 || size == 0) {
        return;
    }

    const size_t copy_size = size < remaining ? size : remaining;
    std::memcpy(dst, src, copy_size);
    dst += copy_size;
    remaining -= copy_size;
}

const uint8_t* AudioPlaneData(const AVFrame* frame, int plane) {
    if (!frame || plane < 0) {
        return nullptr;
    }
    if (frame->extended_data) {
        return frame->extended_data[plane];
    }
    return plane < AV_NUM_DATA_POINTERS ? frame->data[plane] : nullptr;
}

void CopyAudioFrameToPacked(const AVFrame* frame, uint8_t* dst, size_t dst_size) {
    const auto sample_fmt = static_cast<AVSampleFormat>(frame->format);
    const int channels = frame->ch_layout.nb_channels;
    const int bytes_per_sample = av_get_bytes_per_sample(sample_fmt);

    if (channels <= 0 || frame->nb_samples <= 0 || bytes_per_sample <= 0) {
        return;
    }

    uint8_t* out = dst;
    size_t remaining = dst_size;
    const bool planar = av_sample_fmt_is_planar(sample_fmt) != 0;

    if (planar) {
        const size_t plane_size =
            static_cast<size_t>(frame->nb_samples) * static_cast<size_t>(bytes_per_sample);
        for (int ch = 0; ch < channels; ++ch) {
            CopyBounded(out, remaining, AudioPlaneData(frame, ch), plane_size);
        }
        return;
    }

    const size_t packed_size =
        static_cast<size_t>(frame->nb_samples) *
        static_cast<size_t>(channels) *
        static_cast<size_t>(bytes_per_sample);
    CopyBounded(out, remaining, AudioPlaneData(frame, 0), packed_size);
}

void CopyVideoFrameToPacked(const AVFrame* frame, uint8_t* dst, size_t dst_size) {
    if (dst_size > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return;
    }

    const uint8_t* src_data[4] = {
        frame->data[0],
        frame->data[1],
        frame->data[2],
        frame->data[3],
    };

    (void)av_image_copy_to_buffer(
        dst,
        static_cast<int>(dst_size),
        src_data,
        frame->linesize,
        static_cast<AVPixelFormat>(frame->format),
        frame->width,
        frame->height,
        1);
}

} // namespace

FFmpegFrameBuffer::FFmpegFrameBuffer(AVFrame* frame, size_t total_size)
    : frame_(frame), packed_size_(total_size) {
    if (frame_ && total_size > 0) {
        packed_data_ = static_cast<uint8_t*>(std::malloc(total_size));
        if (!packed_data_) {
            av_frame_free(&frame_);
            throw std::bad_alloc();
        }
        std::memset(packed_data_, 0, total_size);

        if (IsAudioFrame(frame_)) {
            CopyAudioFrameToPacked(frame_, packed_data_, total_size);
        } else if (IsVideoFrame(frame_)) {
            CopyVideoFrameToPacked(frame_, packed_data_, total_size);
        }
    }

    // 只有在构造成功后登记，确保 malloc 失败路径不会留下虚假的存活计数。
    if (frame_) {
        av_buffer_size_ = FrameBufferBytes(frame_);
        const size_t live_wrappers =
            g_live_wrappers.fetch_add(1, std::memory_order_relaxed) + 1;
        const size_t live_packed =
            g_live_packed_bytes.fetch_add(packed_size_, std::memory_order_relaxed) +
            packed_size_;
        const size_t live_av_buffers =
            g_live_av_buffer_bytes.fetch_add(av_buffer_size_, std::memory_order_relaxed) +
            av_buffer_size_;
        UpdatePeak(g_peak_wrappers, live_wrappers);
        UpdatePeak(g_peak_packed_bytes, live_packed);
        UpdatePeak(g_peak_av_buffer_bytes, live_av_buffers);
        stats_registered_ = true;
    }
}

FFmpegFrameBuffer::~FFmpegFrameBuffer() {
    if (stats_registered_) {
        g_live_wrappers.fetch_sub(1, std::memory_order_relaxed);
        g_live_packed_bytes.fetch_sub(packed_size_, std::memory_order_relaxed);
        g_live_av_buffer_bytes.fetch_sub(av_buffer_size_, std::memory_order_relaxed);
    }
    std::free(packed_data_);
    if (frame_) av_frame_free(&frame_);
}

uint8_t* FFmpegFrameBuffer::Data() { return packed_data_; }
const uint8_t* FFmpegFrameBuffer::Data() const { return packed_data_; }
size_t FFmpegFrameBuffer::Size() const { return packed_size_; }

FFmpegFrameMemoryStats FFmpegFrameBuffer::GetMemoryStats() {
    FFmpegFrameMemoryStats stats;
    stats.live_wrappers = g_live_wrappers.load(std::memory_order_relaxed);
    stats.live_packed_bytes = g_live_packed_bytes.load(std::memory_order_relaxed);
    stats.live_av_buffer_bytes =
        g_live_av_buffer_bytes.load(std::memory_order_relaxed);
    stats.peak_wrappers = g_peak_wrappers.load(std::memory_order_relaxed);
    stats.peak_packed_bytes = g_peak_packed_bytes.load(std::memory_order_relaxed);
    stats.peak_av_buffer_bytes =
        g_peak_av_buffer_bytes.load(std::memory_order_relaxed);
    return stats;
}
