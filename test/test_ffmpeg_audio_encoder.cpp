#include "media/encoder/ffmpeg_encoder.h"
#include "media/simple_buffer.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

constexpr int kSampleRate = 16000;
constexpr int kChannels = 1;
constexpr int kSamplesPerFrame = 1024;
constexpr double kToneHz = 440.0;

FramePtr MakeS16AudioFrame(int frame_index) {
    std::vector<std::int16_t> pcm(kSamplesPerFrame * kChannels);
    for (int i = 0; i < kSamplesPerFrame; ++i) {
        const double t =
            static_cast<double>(frame_index * kSamplesPerFrame + i) /
            static_cast<double>(kSampleRate);
        const double sample = std::sin(2.0 * 3.14159265358979323846 * kToneHz * t);
        pcm[i] = static_cast<std::int16_t>(sample * 12000.0);
    }

    auto frame = std::make_shared<MediaFrame>();
    frame->type = MediaType::AUDIO;
    frame->time.pts_us =
        static_cast<std::int64_t>(frame_index) * kSamplesPerFrame * 1000000LL /
        kSampleRate;
    frame->time.dts_us = frame->time.pts_us;
    frame->time.duration_us = kSamplesPerFrame * 1000000LL / kSampleRate;

    AudioFrameMeta meta;
    meta.sample_format = SampleFormat::S16;
    meta.sample_rate = kSampleRate;
    meta.channels = kChannels;
    meta.nb_samples = kSamplesPerFrame;
    meta.bytes_per_sample = static_cast<int>(sizeof(std::int16_t));
    meta.planar = false;
    meta.plane_count = 1;
    meta.planes[0].offset = 0;
    meta.planes[0].stride =
        static_cast<int32_t>(pcm.size() * sizeof(std::int16_t));
    meta.planes[0].size = meta.planes[0].stride;
    frame->meta = meta;

    frame->buffer = std::make_shared<SimpleBuffer>(
        pcm.data(), pcm.size() * sizeof(std::int16_t));
    return frame;
}

} // namespace

int main() {
    EncoderConfig config;
    config.media_type = MediaType::AUDIO;
    config.codec_type = CodecType::AAC;
    config.audio.sample_rate = kSampleRate;
    config.audio.channels = kChannels;
    config.audio.sample_format = SampleFormat::S16;
    config.bitrate = 64000;
    config.time_base_num = 1;
    config.time_base_den = 1000000;
    config.global_header = true;

    FFmpegEncoder encoder;
    assert(encoder.Open(config));

    std::vector<PacketPtr> packets;
    for (int i = 0; i < 8; ++i) {
        assert(encoder.Encode(MakeS16AudioFrame(i), packets));
    }
    assert(encoder.Encode(nullptr, packets));

    std::size_t audio_packets = 0;
    for (const auto& packet : packets) {
        if (!packet) {
            continue;
        }
        assert(packet->type == MediaType::AUDIO);
        assert(packet->codec == CodecType::AAC);
        assert(packet->buffer);
        assert(packet->buffer->Size() > 0);
        ++audio_packets;
    }

    assert(audio_packets > 0);
    assert(!encoder.GetExtraData().empty());
    encoder.Close();

    std::cout << "AAC audio encoder conversion test passed, packets="
              << audio_packets << "\n";
    return 0;
}
