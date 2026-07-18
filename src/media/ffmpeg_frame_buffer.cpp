// @file ffmpeg_frame_buffer.cpp
// FFmpeg AVFrame wrapper implementation: AVFrame data to packed memory.
#include "media/ffmpeg_frame_buffer.h"

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
}

FFmpegFrameBuffer::~FFmpegFrameBuffer() {
    std::free(packed_data_);
    if (frame_) av_frame_free(&frame_);
}

uint8_t* FFmpegFrameBuffer::Data() { return packed_data_; }
const uint8_t* FFmpegFrameBuffer::Data() const { return packed_data_; }
size_t FFmpegFrameBuffer::Size() const { return packed_size_; }
