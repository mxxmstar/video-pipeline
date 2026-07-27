#include "filter/audio/audio_resample_filter.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "media/simple_buffer.h"

namespace {

void AppendS16(std::vector<std::uint8_t>& bytes, std::int16_t value) {
    const auto* ptr = reinterpret_cast<const std::uint8_t*>(&value);
    bytes.push_back(ptr[0]);
    bytes.push_back(ptr[1]);
}

std::int16_t ReadS16(const std::vector<std::uint8_t>& bytes, std::size_t sample_index) {
    std::int16_t value = 0;
    std::memcpy(&value, bytes.data() + sample_index * sizeof(value), sizeof(value));
    return value;
}

MediaFrame MakeAudioFrame(int sample_rate,
                          int channels,
                          SampleFormat sample_format,
                          int nb_samples,
                          bool planar,
                          std::vector<std::uint8_t> data) {
    AudioFrameMeta audio_meta;
    audio_meta.sample_format = sample_format;
    audio_meta.sample_rate = sample_rate;
    audio_meta.channels = channels;
    audio_meta.channel_layout = channels == 1 ? 4 : 3; // mono/stereo native masks
    audio_meta.nb_samples = nb_samples;
    audio_meta.bytes_per_sample = 2;
    audio_meta.planar = planar;
    audio_meta.plane_count = planar ? channels : 1;

    const auto plane_size = static_cast<int32_t>(nb_samples * audio_meta.bytes_per_sample);
    for (int i = 0; i < audio_meta.plane_count; ++i) {
        audio_meta.planes[i].offset = planar ? i * plane_size : 0;
        audio_meta.planes[i].stride = planar ? plane_size : static_cast<int32_t>(data.size());
        audio_meta.planes[i].size = planar ? plane_size : static_cast<int32_t>(data.size());
    }

    MediaFrame frame;
    frame.type = MediaType::AUDIO;
    frame.time.pts_us = 12345;
    frame.meta = audio_meta;
    frame.buffer = std::make_shared<SimpleBuffer>(std::move(data));
    return frame;
}

void TestPackedS16Passthrough() {
    std::vector<std::uint8_t> data;
    AppendS16(data, 100);
    AppendS16(data, -100);
    AppendS16(data, 200);
    AppendS16(data, -200);

    auto frame = MakeAudioFrame(48000, 2, SampleFormat::S16, 2, false, data);

    filter::audio::AudioResampleConfig config;
    config.output_sample_rate = 48000;
    config.output_channels = 2;
    config.output_sample_format = SampleFormat::S16;

    filter::audio::AudioResampleFilter filter;
    assert(filter.Open(config));

    filter::audio::PcmFrame out;
    assert(filter.Process(frame, out));
    assert(out.sample_rate == 48000);
    assert(out.channels == 2);
    assert(out.sample_format == SampleFormat::S16);
    assert(out.nb_samples == 2);
    assert(out.data.size() == data.size());
    assert(ReadS16(out.data, 0) == 100);
    assert(ReadS16(out.data, 1) == -100);
    assert(ReadS16(out.data, 2) == 200);
    assert(ReadS16(out.data, 3) == -200);
}

void TestPlanarS16ToInterleaved() {
    std::vector<std::uint8_t> data;
    AppendS16(data, 1);
    AppendS16(data, 2);
    AppendS16(data, 10);
    AppendS16(data, 20);

    auto frame = MakeAudioFrame(48000, 2, SampleFormat::S16P, 2, true, data);

    filter::audio::AudioResampleConfig config;
    config.output_sample_rate = 48000;
    config.output_channels = 2;
    config.output_sample_format = SampleFormat::S16;

    filter::audio::AudioResampleFilter filter;
    assert(filter.Open(config));

    filter::audio::PcmFrame out;
    assert(filter.Process(frame, out));
    assert(out.nb_samples == 2);
    assert(out.data.size() == 4 * sizeof(std::int16_t));
    assert(ReadS16(out.data, 0) == 1);
    assert(ReadS16(out.data, 1) == 10);
    assert(ReadS16(out.data, 2) == 2);
    assert(ReadS16(out.data, 3) == 20);
}

void TestResampleRate() {
    std::vector<std::uint8_t> data;
    for (int i = 0; i < 480; ++i) {
        AppendS16(data, static_cast<std::int16_t>(i));
    }

    auto frame = MakeAudioFrame(48000, 1, SampleFormat::S16, 480, false, data);

    filter::audio::AudioResampleConfig config;
    config.output_sample_rate = 24000;
    config.output_channels = 1;
    config.output_sample_format = SampleFormat::S16;

    filter::audio::AudioResampleFilter filter;
    assert(filter.Open(config));

    filter::audio::PcmFrame out;
    assert(filter.Process(frame, out));
    assert(out.sample_rate == 24000);
    assert(out.channels == 1);
    assert(out.sample_format == SampleFormat::S16);
    assert(out.nb_samples > 0);
    assert(out.nb_samples <= 260);
    assert(out.data.size() ==
           static_cast<std::size_t>(out.nb_samples * out.BytesPerFrame()));
}

void TestInvalidInputFails() {
    filter::audio::AudioResampleFilter filter;
    filter::audio::PcmFrame out;
    auto frame = MakeAudioFrame(48000, 2, SampleFormat::S16, 2, false, {});

    filter::audio::AudioResampleConfig config;
    assert(filter.Open(config));
    assert(!filter.Process(frame, out));
}

} // namespace

int main() {
    TestPackedS16Passthrough();
    TestPlanarS16ToInterleaved();
    TestResampleRate();
    TestInvalidInputFails();
    return 0;
}
