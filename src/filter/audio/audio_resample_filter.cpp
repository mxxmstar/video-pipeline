#include "filter/audio/audio_resample_filter.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

namespace filter::audio {
namespace {

std::string AvErrorString(int error_code) {
    char buffer[AV_ERROR_MAX_STRING_SIZE]{};
    av_make_error_string(buffer, sizeof(buffer), error_code);
    return buffer;
}

AVSampleFormat ToAVSampleFormat(SampleFormat format) {
    switch (format) {
        case SampleFormat::U8:   return AV_SAMPLE_FMT_U8;
        case SampleFormat::S16:  return AV_SAMPLE_FMT_S16;
        case SampleFormat::S32:  return AV_SAMPLE_FMT_S32;
        case SampleFormat::FLT:  return AV_SAMPLE_FMT_FLT;
        case SampleFormat::DBL:  return AV_SAMPLE_FMT_DBL;
        case SampleFormat::U8P:  return AV_SAMPLE_FMT_U8P;
        case SampleFormat::S16P: return AV_SAMPLE_FMT_S16P;
        case SampleFormat::S32P: return AV_SAMPLE_FMT_S32P;
        case SampleFormat::FLTP: return AV_SAMPLE_FMT_FLTP;
        case SampleFormat::DBLP: return AV_SAMPLE_FMT_DBLP;
        default:                 return AV_SAMPLE_FMT_NONE;
    }
}

SampleFormat ToPackedSampleFormat(SampleFormat format) {
    switch (format) {
        case SampleFormat::U8P:  return SampleFormat::U8;
        case SampleFormat::S16P: return SampleFormat::S16;
        case SampleFormat::S32P: return SampleFormat::S32;
        case SampleFormat::FLTP: return SampleFormat::FLT;
        case SampleFormat::DBLP: return SampleFormat::DBL;
        default:                 return format;
    }
}

std::uint64_t DefaultChannelLayoutMask(int channels) {
    AVChannelLayout layout{};
    av_channel_layout_default(&layout, channels);
    const auto mask = layout.order == AV_CHANNEL_ORDER_NATIVE ? layout.u.mask : 0;
    av_channel_layout_uninit(&layout);
    return mask;
}

void FillChannelLayout(AVChannelLayout& layout,
                       int channels,
                       std::uint64_t channel_layout) {
    av_channel_layout_uninit(&layout);
    if (channel_layout != 0) {
        av_channel_layout_from_mask(&layout, channel_layout);
        return;
    }
    av_channel_layout_default(&layout, channels);
}

bool FillAudioFrameFromMediaFrame(const MediaFrame& input,
                                  AVFrame* frame,
                                  AVSampleFormat sample_format,
                                  int channels,
                                  int sample_rate,
                                  int nb_samples,
                                  std::uint64_t channel_layout,
                                  std::string& error) {
    const auto* base = input.buffer ? input.buffer->Data() : nullptr;
    const auto buffer_size = input.buffer ? input.buffer->Size() : 0;
    if (!base || buffer_size == 0) {
        error = "audio frame buffer is empty";
        return false;
    }
    if (!frame || sample_format == AV_SAMPLE_FMT_NONE ||
        channels <= 0 || sample_rate <= 0 || nb_samples <= 0) {
        error = "invalid source audio parameters";
        return false;
    }

    const int expected_size = av_samples_get_buffer_size(
        nullptr,
        channels,
        nb_samples,
        sample_format,
        1);
    if (expected_size < 0) {
        error = "failed to calculate source audio buffer size: " +
                AvErrorString(expected_size);
        return false;
    }
    if (static_cast<std::size_t>(expected_size) > buffer_size) {
        error = "audio frame buffer is smaller than metadata requires";
        return false;
    }

    frame->format = sample_format;
    frame->sample_rate = sample_rate;
    frame->nb_samples = nb_samples;
    FillChannelLayout(frame->ch_layout, channels, channel_layout);
    frame->pts = input.time.pts_us;

    // 当前 decoder 的 FFmpegFrameBuffer 会把 packed/planar 音频都压成一段连续内存；
    // avcodec_fill_audio_frame 能根据 sample_format 自动恢复 AVFrame 的 data 指针。
    const int ret = avcodec_fill_audio_frame(
        frame,
        channels,
        sample_format,
        const_cast<std::uint8_t*>(base),
        static_cast<int>(buffer_size),
        1);
    if (ret < 0) {
        error = "failed to fill source audio frame: " + AvErrorString(ret);
        return false;
    }
    return true;
}

} // namespace

AudioResampleFilter::~AudioResampleFilter() {
    Close();
}

bool AudioResampleFilter::Open(const AudioResampleConfig& config) {
    Close();
    config_ = config;
    config_.output_sample_format = ToPackedSampleFormat(config_.output_sample_format);
    opened_ = true;
    last_error_.clear();
    return true;
}

bool AudioResampleFilter::Process(const MediaFrame& input, PcmFrame& output) {
    output = {};
    if (!opened_) {
        SetError("AudioResampleFilter is not opened");
        return false;
    }
    if (input.type != MediaType::AUDIO) {
        SetError("input frame is not audio");
        return false;
    }

    ResampleParams next_params;
    if (!BuildParams(input, next_params)) {
        return false;
    }
    if (!EnsureContext(input, next_params)) {
        return false;
    }

    const auto src_fmt = ToAVSampleFormat(next_params.input_format);
    const auto dst_fmt = ToAVSampleFormat(next_params.output_format);
    const auto src_samples = input.NbSamples();
    AVFrame* source_frame = av_frame_alloc();
    if (!source_frame) {
        SetError("av_frame_alloc failed");
        return false;
    }

    std::string frame_error;
    if (!FillAudioFrameFromMediaFrame(input,
                                      source_frame,
                                      src_fmt,
                                      next_params.input_channels,
                                      next_params.input_sample_rate,
                                      src_samples,
                                      next_params.input_channel_layout,
                                      frame_error)) {
        av_frame_free(&source_frame);
        SetError(std::move(frame_error));
        return false;
    }

    const auto delay = swr_get_delay(swr_, next_params.input_sample_rate);
    const int output_capacity = static_cast<int>(av_rescale_rnd(
        delay + src_samples,
        next_params.output_sample_rate,
        next_params.input_sample_rate,
        AV_ROUND_UP));
    if (output_capacity <= 0) {
        av_frame_free(&source_frame);
        SetError("invalid output sample capacity");
        return false;
    }

    std::uint8_t** converted_data = nullptr;
    int converted_linesize = 0;
    int ret = av_samples_alloc_array_and_samples(
        &converted_data,
        &converted_linesize,
        next_params.output_channels,
        output_capacity,
        dst_fmt,
        1);
    if (ret < 0) {
        av_frame_free(&source_frame);
        SetError("failed to allocate resample output buffer: " + AvErrorString(ret));
        return false;
    }

    std::vector<const std::uint8_t*> source_planes(
        static_cast<std::size_t>(next_params.input_channels));
    for (int i = 0; i < next_params.input_channels; ++i) {
        source_planes[static_cast<std::size_t>(i)] = source_frame->extended_data[i];
    }

    ret = swr_convert(
        swr_,
        converted_data,
        output_capacity,
        source_planes.data(),
        src_samples);
    if (ret < 0) {
        av_freep(&converted_data[0]);
        av_freep(&converted_data);
        av_frame_free(&source_frame);
        SetError("swr_convert failed: " + AvErrorString(ret));
        return false;
    }

    const int output_size = av_samples_get_buffer_size(
        nullptr,
        next_params.output_channels,
        ret,
        dst_fmt,
        1);
    if (output_size < 0) {
        av_freep(&converted_data[0]);
        av_freep(&converted_data);
        av_frame_free(&source_frame);
        SetError("failed to calculate resample output size: " +
                 AvErrorString(output_size));
        return false;
    }

    output.sample_rate = next_params.output_sample_rate;
    output.channels = next_params.output_channels;
    output.channel_layout = next_params.output_channel_layout;
    output.sample_format = next_params.output_format;
    output.bytes_per_sample = av_get_bytes_per_sample(dst_fmt);
    output.nb_samples = ret;
    output.pts_us = input.time.pts_us;
    output.duration_us = next_params.output_sample_rate > 0
        ? static_cast<int64_t>(ret) * 1'000'000LL / next_params.output_sample_rate
        : 0;
    output.data.assign(converted_data[0], converted_data[0] + output_size);

    av_freep(&converted_data[0]);
    av_freep(&converted_data);
    av_frame_free(&source_frame);
    last_error_.clear();
    return true;
}

void AudioResampleFilter::Close() {
    if (swr_) {
        swr_free(&swr_);
    }
    params_ = {};
    opened_ = false;
}

bool AudioResampleFilter::EnsureContext(const MediaFrame&, ResampleParams& params) {
    if (swr_ && params == params_) {
        return true;
    }

    if (swr_) {
        swr_free(&swr_);
    }

    AVChannelLayout src_layout{};
    AVChannelLayout dst_layout{};
    FillChannelLayout(src_layout, params.input_channels, params.input_channel_layout);
    FillChannelLayout(dst_layout, params.output_channels, params.output_channel_layout);

    const auto src_fmt = ToAVSampleFormat(params.input_format);
    const auto dst_fmt = ToAVSampleFormat(params.output_format);
    int ret = swr_alloc_set_opts2(&swr_,
                                  &dst_layout,
                                  dst_fmt,
                                  params.output_sample_rate,
                                  &src_layout,
                                  src_fmt,
                                  params.input_sample_rate,
                                  0,
                                  nullptr);
    av_channel_layout_uninit(&src_layout);
    av_channel_layout_uninit(&dst_layout);
    if (ret < 0 || !swr_) {
        SetError("swr_alloc_set_opts2 failed: " + AvErrorString(ret));
        return false;
    }

    ret = swr_init(swr_);
    if (ret < 0) {
        swr_free(&swr_);
        SetError("swr_init failed: " + AvErrorString(ret));
        return false;
    }

    params_ = params;
    return true;
}

bool AudioResampleFilter::BuildParams(const MediaFrame& input, ResampleParams& params) {
    const auto* audio_meta = input.AudioMeta();
    if (!audio_meta) {
        SetError("input frame has no audio metadata");
        return false;
    }

    params.input_format = input.SampleFmt();
    params.input_sample_rate = input.SampleRate();
    params.input_channels = input.Channels();
    params.input_channel_layout = audio_meta->channel_layout;
    params.output_format = ToPackedSampleFormat(config_.output_sample_format);
    params.output_sample_rate = config_.output_sample_rate > 0
        ? config_.output_sample_rate
        : params.input_sample_rate;
    params.output_channels = config_.output_channels > 0
        ? config_.output_channels
        : params.input_channels;
    params.output_channel_layout = config_.output_channel_layout != 0
        ? config_.output_channel_layout
        : DefaultChannelLayoutMask(params.output_channels);

    if (ToAVSampleFormat(params.input_format) == AV_SAMPLE_FMT_NONE ||
        ToAVSampleFormat(params.output_format) == AV_SAMPLE_FMT_NONE ||
        params.input_sample_rate <= 0 ||
        params.output_sample_rate <= 0 ||
        params.input_channels <= 0 ||
        params.output_channels <= 0 ||
        input.NbSamples() <= 0) {
        SetError("invalid audio resample parameters");
        return false;
    }
    return true;
}

void AudioResampleFilter::SetError(std::string message) {
    last_error_ = std::move(message);
}

} // namespace filter::audio
