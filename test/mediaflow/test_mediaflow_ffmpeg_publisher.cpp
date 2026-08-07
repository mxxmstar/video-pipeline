#include "mediaflow/media_nodes.h"
#include "mediaflow/media_timing.h"

#include "media/puller/ffmpeg_puller.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifndef VIDEO_PIPELINE_TEST_AV_PATH
#define VIDEO_PIPELINE_TEST_AV_PATH "test_av.ts"
#endif

#ifndef VIDEO_PIPELINE_TEST_OUTPUT_PATH
#define VIDEO_PIPELINE_TEST_OUTPUT_PATH "mediaflow_ffmpeg_publisher_acceptance.mkv"
#endif

namespace {

using namespace mediaflow;

constexpr int kGeneration = 1;
constexpr std::size_t kTargetVideoPackets = 180; // 使用样本中的主要双轨包序列。
constexpr std::size_t kMaxReadIterations = 10'000;
constexpr std::int64_t kTimestampToleranceUs = 2'000;

struct TrackSamples {
    std::vector<std::int64_t> pts_us;
};

std::int64_t AbsDifference(std::int64_t left, std::int64_t right) {
    return left >= right ? left - right : right - left;
}

int TrackKey(MediaType media_type) {
    return static_cast<int>(media_type);
}

const MediaStreamInfo* FindStreamInfo(const MultiStreamInfo& streams,
                                      int stream_index) {
    const auto it = std::find_if(
        streams.stream_infos.begin(), streams.stream_infos.end(),
        [stream_index](const MediaStreamInfo& info) {
            return info.stream_index == stream_index;
        });
    return it == streams.stream_infos.end() ? nullptr : &*it;
}

EncodedTrackInfo ToEncodedTrackInfo(const MediaStreamInfo& info) {
    EncodedTrackInfo result;
    result.media_type = info.media_type;
    result.codec_type = info.codec_type;
    result.time_base = info.time_base;
    result.extra_data = info.extra_data;

    if (info.media_type == MediaType::VIDEO &&
        std::holds_alternative<VideoStreamInfo>(info.detail)) {
        const auto& video = std::get<VideoStreamInfo>(info.detail);
        result.width = video.width;
        result.height = video.height;
        result.fps = video.fps;
    } else if (info.media_type == MediaType::AUDIO &&
               std::holds_alternative<AudioStreamInfo>(info.detail)) {
        const auto& audio = std::get<AudioStreamInfo>(info.detail);
        result.sample_rate = audio.sample_rate;
        result.channels = audio.channels;
    }
    return result;
}

MediaTrackConfig ToTrackConfig(const MediaStreamInfo& info) {
    const auto encoded = ToEncodedTrackInfo(info);
    MediaTrackConfig result;
    // 使用输入 stream_index 作为 Publisher track_id，保证 FFmpeg adapter
    // 可以沿用原始 MediaPacket 的 stream_index 做唯一轨道路由。
    result.track_id = info.stream_index;
    result.media_type = encoded.media_type;
    result.codec_type = encoded.codec_type;
    result.width = encoded.width;
    result.height = encoded.height;
    result.fps = encoded.fps;
    result.sample_rate = encoded.sample_rate;
    result.channels = encoded.channels;
    result.time_base_num = encoded.time_base.num;
    result.time_base_den = encoded.time_base.den;
    result.extra_data = encoded.extra_data;
    return result;
}

void AssertTimestampRetention(const std::map<int, TrackSamples>& expected,
                              const std::map<int, TrackSamples>& actual) {
    assert(expected.size() == actual.size());
    for (const auto& [track, expected_samples] : expected) {
        const auto actual_it = actual.find(track);
        assert(actual_it != actual.end());
        const auto& actual_samples = actual_it->second;
        assert(!expected_samples.pts_us.empty());
        assert(expected_samples.pts_us.size() == actual_samples.pts_us.size());

        // 容器可能重建 time base，但相对 PTS 不能改变。使用相对首包比较，
        // 允许 muxer 在文件级时间轴上统一增加一个固定起点偏移。
        const auto expected_origin = expected_samples.pts_us.front();
        const auto actual_origin = actual_samples.pts_us.front();
        for (std::size_t i = 0; i < expected_samples.pts_us.size(); ++i) {
            const auto expected_relative = expected_samples.pts_us[i] - expected_origin;
            const auto actual_relative = actual_samples.pts_us[i] - actual_origin;
            assert(AbsDifference(expected_relative, actual_relative) <=
                   kTimestampToleranceUs);
        }
    }
}

void TestRealFfmpegDualTrackPublisher() {
    const std::filesystem::path input_path = VIDEO_PIPELINE_TEST_AV_PATH;
    const std::filesystem::path output_path = VIDEO_PIPELINE_TEST_OUTPUT_PATH;
    std::error_code file_error;
    std::filesystem::remove(output_path, file_error);

    FFmpegPuller puller;
    puller.SetLowLatency(false);
    puller.SetConnectTimeoutMs(3'000);
    puller.SetReadTimeoutMs(3'000);
    assert(puller.Open(input_path.string()));

    const auto input_streams = puller.GetStreamInfo();
    assert(input_streams.HasVideoStream());
    assert(input_streams.HasAudioStream());

    PublisherConfig config;
    config.url = output_path.string();
    config.protocol = PublishProtocol::FfmpegMux;
    config.ffmpeg.format_name = "matroska";
    for (const auto& info : input_streams.stream_infos) {
        if (info.media_type == MediaType::VIDEO ||
            info.media_type == MediaType::AUDIO) {
            config.tracks.push_back(ToTrackConfig(info));
        }
    }
    assert(config.tracks.size() == 2);

    PublisherSinkNode sink(config, IPublisher::Create(config));
    assert(sink.Init());
    assert(sink.Start());

    std::map<int, TrackSamples> expected_samples;
    bool video_anchor_found = false;
    std::size_t video_packets = 0;
    std::size_t read_iterations = 0;
    std::size_t input_packets = 0;

    while (video_packets < kTargetVideoPackets &&
           read_iterations++ < kMaxReadIterations) {
        const auto result = puller.ReadPacketResult();
        if (result.status == IPuller::PullReadStatus::NoData) {
            continue;
        }
        if (result.status == IPuller::PullReadStatus::EOS) {
            break;
        }
        assert(result.status == IPuller::PullReadStatus::Packet);
        assert(result.packet);

        const auto& packet = result.packet;
        const auto* stream_info =
            FindStreamInfo(input_streams, packet->stream_index);
        assert(stream_info);
        if (!video_anchor_found) {
            // 只从第一个视频关键帧开始验收，避免把无法独立解码的文件前缀
            // 误判成 Publisher 的输出缺陷；音频仍然与视频一起参与后续发布。
            if (packet->type != MediaType::VIDEO || !packet->keyframe) {
                continue;
            }
            video_anchor_found = true;
        }

        const auto timing = GetMediaTiming(*packet);
        assert(timing.pts_valid);
        assert(timing.dts_valid);
        expected_samples[TrackKey(packet->type)].pts_us.push_back(
            timing.pts_us);

        EncodedPacketMessage message;
        message.packet = packet;
        message.track_info = ToEncodedTrackInfo(*stream_info);
        message.track_id = packet->stream_index;
        message.generation = kGeneration;
        sink.Input().Receive(std::move(message));
        ++input_packets;
        if (packet->type == MediaType::VIDEO) {
            ++video_packets;
        }
    }

    assert(video_anchor_found);
    assert(video_packets == kTargetVideoPackets);
    assert(input_packets > video_packets);

    // 每条轨道分别发送 EOS，验证多轨 Publisher 只在两条轨道都结束后写 trailer。
    for (const auto& info : input_streams.stream_infos) {
        if (info.media_type != MediaType::VIDEO &&
            info.media_type != MediaType::AUDIO) {
            continue;
        }
        EncodedPacketMessage eos;
        eos.track_info = ToEncodedTrackInfo(info);
        eos.track_id = info.stream_index;
        eos.generation = kGeneration;
        eos.eos = true;
        sink.Input().Receive(std::move(eos));
    }
    puller.Close();

    assert(std::filesystem::exists(output_path));
    assert(std::filesystem::file_size(output_path) > 0);

    // 重新打开真实 mux 输出，确认 trailer、双轨 stream 描述和 packet 读取均有效。
    FFmpegPuller output_puller;
    output_puller.SetLowLatency(false);
    output_puller.SetConnectTimeoutMs(3'000);
    output_puller.SetReadTimeoutMs(3'000);
    assert(output_puller.Open(output_path.string()));
    const auto output_streams = output_puller.GetStreamInfo();
    assert(output_streams.HasVideoStream());
    assert(output_streams.HasAudioStream());

    std::map<int, TrackSamples> actual_samples;
    std::int64_t previous_dts_us = kNoTimestamp;
    std::size_t output_packets = 0;
    while (true) {
        const auto result = output_puller.ReadPacketResult();
        if (result.status == IPuller::PullReadStatus::EOS) {
            break;
        }
        if (result.status == IPuller::PullReadStatus::NoData) {
            continue;
        }
        assert(result.status == IPuller::PullReadStatus::Packet);
        assert(result.packet);

        const auto timing = GetMediaTiming(*result.packet);
        assert(timing.pts_valid);
        assert(timing.dts_valid);
        if (IsValidTimestamp(previous_dts_us)) {
            // FFmpeg mux 输出应按已交织的 DTS 顺序写出，不能出现后写包的
            // 解码时间戳早于前一包的情况。
            assert(timing.dts_us >= previous_dts_us);
        }
        previous_dts_us = timing.dts_us;
        actual_samples[TrackKey(result.packet->type)].pts_us.push_back(
            timing.pts_us);
        ++output_packets;
    }
    output_puller.Close();

    assert(output_packets == input_packets);
    assert(actual_samples[TrackKey(MediaType::VIDEO)].pts_us.size() ==
           expected_samples[TrackKey(MediaType::VIDEO)].pts_us.size());
    assert(actual_samples[TrackKey(MediaType::AUDIO)].pts_us.size() ==
           expected_samples[TrackKey(MediaType::AUDIO)].pts_us.size());
    AssertTimestampRetention(expected_samples, actual_samples);

    std::cout << "real FFmpeg dual-track publisher acceptance passed: "
              << "input_packets=" << input_packets
              << ", output_packets=" << output_packets
              << ", video_packets=" << video_packets
              << ", output=" << output_path.string() << "\n";

    sink.Deinit();
}

} // namespace

int main() {
    TestRealFfmpegDualTrackPublisher();
    return 0;
}
