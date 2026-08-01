#include "common/log/logger.h"
#include "media/decoder/ffmpeg_decoder.h"
#include "media/encoder/ffmpeg_encoder.h"
#include "media/puller/avtp_puller.h"
#include "media/publisher/i_publisher.h"

#include <boost/asio/ip/tcp.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// 这条默认 URL 对应现场抓包中看到的 AVTP 源，方便开发机直接运行。
// 如果换网卡、换设备或换 stream_id，可以在命令行传入新的 avtp:// URL 覆盖它。
    //"?src=aa:80:b9:86:cf:7e"
constexpr const char* kDefaultAvtpUrl =
    "avtp://\\Device\\NPF_{00087179-AD1C-4C01-904C-11127E5E94C2}"
    "?src=02:aa:bb:cc:dd:ee"
    "&format=auto"
    "&probe_timeout=5000"
    "&read_timeout=100"
    "&probe_packets=2000";

constexpr const char* kDefaultStreamPath = "/avtp/main";
// 默认使用 0 让测试启动时自动挑选空闲端口；固定端口可通过 --port 指定。
constexpr std::uint16_t kDefaultRtspPort = 0;
constexpr int kDefaultFps = 25;
constexpr int kDefaultBitrate = 2'000'000;
constexpr int kDefaultMaxFrames = 300 * 6000;
constexpr int kDefaultDurationSeconds = 30;
constexpr int kDefaultMaxDecoderErrorsBeforeInit = 100;

struct RunOptions {
    std::string avtp_url{kDefaultAvtpUrl};
    std::string listen_host{"127.0.0.1"};
    std::uint16_t rtsp_port{kDefaultRtspPort};
    std::string stream_path{kDefaultStreamPath};
    int fps{kDefaultFps};
    int bitrate{kDefaultBitrate};
    int max_frames{kDefaultMaxFrames};
    int duration_seconds{kDefaultDurationSeconds};
};

struct PipelineState {
    std::unique_ptr<FFmpegEncoder> encoder;
    std::unique_ptr<IPublisher> publisher;
    bool initialized{false};
    bool failed{false};
    std::string error;

    bool has_audio_stream{false};
    CodecType audio_codec{CodecType::UNKNOWN};
    int audio_sample_rate{0};
    int audio_channels{0};

    int decoded_frames{0};
    int decoded_audio_frames{0};
    int decoder_errors{0};
    int audio_decoder_errors{0};
    int encoded_packets{0};
    int published_packets{0};
    int read_audio_packets{0};
    int published_audio_packets{0};
    int keyframes{0};
    int64_t frame_duration_us{1'000'000 / kDefaultFps};
};

void PrintUsage(const char* exe) {
    std::cout
        << "Usage:\n"
        << "  " << exe << " [avtp-url] [--port N|0] [--path /avtp/main]\n"
        << "       [--host 0.0.0.0] [--fps N] [--bitrate N]\n"
        << "       [--max-frames N] [--duration-sec N]\n\n"
        << "Example:\n"
        << "  " << exe << " \"avtp://\\\\Device\\\\NPF_{...}?src=aa:80:b9:86:cf:7e&format=auto\"\n\n"
        << "--port 0 means auto-pick a free local TCP port.\n";
}

int ParseInt(const char* value, const char* option_name) {
    if (!value) {
        throw std::runtime_error(std::string{"missing value for "} + option_name);
    }

    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (*value == '\0' || (end && *end != '\0')) {
        throw std::runtime_error(std::string{"invalid integer for "} + option_name);
    }
    return static_cast<int>(parsed);
}

std::uint16_t ParsePort(const char* value) {
    const int parsed = ParseInt(value, "--port");
    if (parsed < 0 || parsed > 65535) {
        throw std::runtime_error("--port must be in range 0..65535");
    }
    return static_cast<std::uint16_t>(parsed);
}

std::uint16_t FindFreeTcpPort() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(
        io,
        {boost::asio::ip::make_address("127.0.0.1"), 0});
    return acceptor.local_endpoint().port();
}

RunOptions ParseArgs(int argc, char** argv) {
    RunOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            std::exit(0);
        }
        if (arg == "--port") {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for --port");
            }
            options.rtsp_port = ParsePort(argv[++i]);
            continue;
        }
        if (arg == "--path") {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for --path");
            }
            options.stream_path = argv[++i];
            if (options.stream_path.empty() || options.stream_path.front() != '/') {
                throw std::runtime_error("--path must start with '/'");
            }
            continue;
        }
        if (arg == "--host") {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for --host");
            }
            options.listen_host = argv[++i];
            continue;
        }
        if (arg == "--fps") {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for --fps");
            }
            options.fps = ParseInt(argv[++i], "--fps");
            if (options.fps <= 0) {
                throw std::runtime_error("--fps must be positive");
            }
            continue;
        }
        if (arg == "--bitrate") {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for --bitrate");
            }
            options.bitrate = ParseInt(argv[++i], "--bitrate");
            if (options.bitrate <= 0) {
                throw std::runtime_error("--bitrate must be positive");
            }
            continue;
        }
        if (arg == "--max-frames") {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for --max-frames");
            }
            options.max_frames = ParseInt(argv[++i], "--max-frames");
            if (options.max_frames <= 0) {
                throw std::runtime_error("--max-frames must be positive");
            }
            continue;
        }
        if (arg == "--duration-sec") {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for --duration-sec");
            }
            options.duration_seconds = ParseInt(argv[++i], "--duration-sec");
            if (options.duration_seconds <= 0) {
                throw std::runtime_error("--duration-sec must be positive");
            }
            continue;
        }
        if (!arg.empty() && arg[0] != '-') {
            options.avtp_url = arg;
            continue;
        }

        throw std::runtime_error("unknown argument: " + arg);
    }

    return options;
}

void NormalizeFrameTime(PipelineState& state, MediaFrame& frame) {
    const auto frame_index = static_cast<int64_t>(std::max(0, state.decoded_frames - 1));
    const auto pts_us = frame_index * state.frame_duration_us;
    frame.time.pts_us = pts_us;
    frame.time.dts_us = pts_us;
    frame.time.duration_us = state.frame_duration_us;
}

bool InitializeOutputPipeline(PipelineState& state,
                              const RunOptions& options,
                              const MediaFrame& first_frame) {
    const PixelFormat pixel_format = first_frame.GetPixelFormat();
    if (pixel_format == PixelFormat::kUnknown) {
        state.error = "decoded frame pixel format is unknown";
        return false;
    }

    const int width = first_frame.Width();
    const int height = first_frame.Height();
    if (width <= 0 || height <= 0) {
        state.error = "decoded frame size is invalid";
        return false;
    }

    state.frame_duration_us = 1'000'000 / std::max(1, options.fps);

    EncoderConfig encoder_config;
    encoder_config.media_type = MediaType::VIDEO;
    encoder_config.codec_type = CodecType::H264;
    encoder_config.video.width = width;
    encoder_config.video.height = height;
    encoder_config.video.fps_num = options.fps;
    encoder_config.video.fps_den = 1;
    encoder_config.video.pixel_format = pixel_format;
    encoder_config.video.gop_size = options.fps;
    encoder_config.video.max_b_frames = 0;
    encoder_config.bitrate = options.bitrate;
    encoder_config.time_base_num = 1;
    encoder_config.time_base_den = 1'000'000;
    encoder_config.encoder_name = "libx264";
    encoder_config.preset = "ultrafast";
    encoder_config.tune = "zerolatency";
    encoder_config.global_header = true;

    state.encoder = std::make_unique<FFmpegEncoder>();
    if (!state.encoder->Open(encoder_config)) {
        state.error = "failed to open FFmpeg H264 encoder";
        return false;
    }

    PublisherConfig publisher_config;
    publisher_config.mode = PublishMode::PullServer;
    publisher_config.protocol = PublishProtocol::RtspServer;
    publisher_config.listen_host = options.listen_host;
    publisher_config.listen_port = options.rtsp_port;
    publisher_config.stream_path = options.stream_path;
    publisher_config.rtsp.enable_tcp_interleaved = true;
    publisher_config.rtsp.enable_udp = true;
    // 与现有本地 MP4 -> RTSP publisher 测试保持一致：同时允许 TCP、UDP 单播和组播。
    // 这样 VLC/ffplay 可以按自己的 Transport 偏好拉流，也便于对比两个测试的行为。
    publisher_config.rtsp.enable_multicast = true;

    MediaTrackConfig track;
    track.track_id = 0;
    track.media_type = MediaType::VIDEO;
    track.codec_type = CodecType::H264;
    track.width = width;
    track.height = height;
    track.fps = static_cast<float>(options.fps);
    track.time_base_num = encoder_config.time_base_num;
    track.time_base_den = encoder_config.time_base_den;
    track.extra_data = state.encoder->GetExtraData();
    publisher_config.tracks.push_back(std::move(track));

    if (state.has_audio_stream && state.audio_codec != CodecType::UNKNOWN &&
        state.audio_sample_rate > 0 && state.audio_channels > 0) {
        MediaTrackConfig audio_track;
        audio_track.track_id = 1;
        audio_track.media_type = MediaType::AUDIO;
        audio_track.codec_type = state.audio_codec;
        audio_track.sample_rate = state.audio_sample_rate;
        audio_track.channels = state.audio_channels;
        audio_track.time_base_num = 1;
        audio_track.time_base_den = 1'000'000;
        audio_track.rtp_clock_rate =
            static_cast<std::uint32_t>(state.audio_sample_rate);
        publisher_config.tracks.push_back(std::move(audio_track));
    }

    state.publisher = IPublisher::Create(publisher_config);
    if (!state.publisher || !state.publisher->Start()) {
        state.error = "failed to start RTSP publisher";
        return false;
    }

    state.initialized = true;
    std::cout << "AVTP decode/publish pipeline initialized: "
              << width << "x" << height << " @" << options.fps
              << "fps";
    if (state.has_audio_stream) {
        std::cout << ", audio=" << state.audio_sample_rate
                  << "Hz/" << state.audio_channels << "ch codec="
                  << static_cast<int>(state.audio_codec);
    }
    std::cout << "\n";
    std::cout << "Play URL: rtsp://127.0.0.1:" << options.rtsp_port
              << options.stream_path << "\n";
    return true;
}

bool PublishAudioPacket(PipelineState& state, const std::shared_ptr<MediaPacket>& packet) {
    if (!state.initialized || !state.publisher || !packet) {
        return true;
    }

    packet->stream_index = 1;
    packet->time_base = Rational{1, 1'000'000};
    if (!state.publisher->Publish(*packet)) {
        state.error = "failed to publish AVTP audio packet";
        return false;
    }

    ++state.published_audio_packets;
    ++state.published_packets;
    return true;
}

bool PublishEncodedPackets(PipelineState& state, std::vector<PacketPtr>& packets) {
    for (auto& packet : packets) {
        if (!packet) {
            continue;
        }

        packet->stream_index = 0;
        packet->time_base = Rational{1, 1'000'000};
        if (packet->duration <= 0) {
            packet->duration = state.frame_duration_us;
        }

        if (packet->keyframe) {
            ++state.keyframes;
        }

        if (!state.publisher->Publish(*packet)) {
            state.error = "failed to publish encoded packet";
            return false;
        }

        ++state.encoded_packets;
        ++state.published_packets;
    }

    return true;
}

void HandleDecodedFrame(PipelineState& state,
                        const RunOptions& options,
                        const std::shared_ptr<MediaFrame>& frame) {
    if (state.failed || !frame || frame->type != MediaType::VIDEO) {
        return;
    }

    ++state.decoded_frames;

    if (!state.initialized &&
        !InitializeOutputPipeline(state, options, *frame)) {
        state.failed = true;
        return;
    }

    NormalizeFrameTime(state, *frame);

    std::vector<PacketPtr> encoded_packets;
    if (!state.encoder->Encode(frame, encoded_packets) ||
        !PublishEncodedPackets(state, encoded_packets)) {
        state.failed = true;
        return;
    }

    if (state.decoded_frames % 30 == 0) {
        std::cout << "decoded=" << state.decoded_frames
                  << ", decoded_audio=" << state.decoded_audio_frames
                  << ", encoded=" << state.encoded_packets
                  << ", published=" << state.published_packets
                  << ", published_audio=" << state.published_audio_packets
                  << ", keyframes=" << state.keyframes << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    logger::Init("avtp_decode_rtsp_publisher");

    RunOptions options;
    try {
        options = ParseArgs(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        PrintUsage(argv[0]);
        logger::Shutdown();
        return 1;
    }
    if (options.rtsp_port == 0) {
        options.rtsp_port = FindFreeTcpPort();
    }

    std::cout << "Opening AVTP source:\n  " << options.avtp_url << "\n";

    AvtpPuller puller;
    if (!puller.Open(options.avtp_url)) {
        std::cerr << "failed to open AVTP source\n";
        logger::Shutdown();
        return 1;
    }

    MultiStreamInfo streams = puller.GetStreamInfo();
    if (!streams.HasVideoStream()) {
        std::cerr << "AVTP source has no video stream info\n";
        puller.Close();
        logger::Shutdown();
        return 1;
    }

    FFmpegDecoder decoder;
    FFmpegDecoder audio_decoder;
    PipelineState state;
    state.frame_duration_us = 1'000'000 / std::max(1, options.fps);
    if (streams.HasAudioStream()) {
        const MediaStreamInfo& audio_info = streams.stream_infos[streams.audio_stream_idx_];
        const auto& audio_detail = audio_info.get_detail<AudioStreamInfo>();
        state.has_audio_stream = true;
        state.audio_codec = audio_info.codec_type;
        state.audio_sample_rate = audio_detail.sample_rate;
        state.audio_channels = audio_detail.channels;
        audio_decoder.SetFrameCallback(
            [&](std::shared_ptr<MediaFrame> frame) {
                if (frame && frame->type == MediaType::AUDIO) {
                    ++state.decoded_audio_frames;
                }
            });
        if (!audio_decoder.Open(audio_info)) {
            std::cerr << "failed to open audio decoder\n";
            puller.Close();
            logger::Shutdown();
            return 1;
        }
    }
    decoder.SetFrameCallback(
        [&](std::shared_ptr<MediaFrame> frame) {
            HandleDecodedFrame(state, options, frame);
        });

    const MediaStreamInfo& video_info = streams.stream_infos[streams.video_stream_idx_];
    if (!decoder.Open(video_info)) {
        std::cerr << "failed to open decoder\n";
        puller.Close();
        logger::Shutdown();
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(options.duration_seconds);
    int avtp_packets = 0;
    int empty_reads = 0;

    while (!state.failed &&
           state.decoded_frames < options.max_frames &&
           std::chrono::steady_clock::now() < deadline) {
        std::shared_ptr<MediaPacket> packet;
        if (!puller.ReadPacket(packet)) {
            ++empty_reads;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (!packet) {
            continue;
        }

        ++avtp_packets;
        if (packet->type == MediaType::AUDIO) {
            ++state.read_audio_packets;
            if (state.has_audio_stream && !audio_decoder.Decode(packet)) {
                ++state.audio_decoder_errors;
            }
            if (!PublishAudioPacket(state, packet)) {
                state.failed = true;
                break;
            }
            continue;
        }

        if (!decoder.Decode(packet)) {
            ++state.decoder_errors;
            // 直播接入时可能先收到 P/B 帧，尚未等到 SPS/PPS 或 IDR。
            // 这种情况下 libavcodec 会拒绝当前 packet，但后续关键帧到来后通常可以恢复。
            if (state.initialized ||
                state.decoder_errors > kDefaultMaxDecoderErrorsBeforeInit) {
                state.failed = true;
                state.error = "decoder rejected too many AVTP packets";
                break;
            }
            continue;
        }
    }

    if (state.encoder) {
        std::vector<PacketPtr> flushed_packets;
        if (state.encoder->Encode(nullptr, flushed_packets)) {
            (void)PublishEncodedPackets(state, flushed_packets);
        }
    }

    decoder.Close();
    audio_decoder.Close();
    const auto timestamp_stats = puller.GetStats().timestamp_mapper;
    puller.Close();
    if (state.publisher) {
        state.publisher->Stop();
    }

    std::cout << "Summary: avtp_packets=" << avtp_packets
              << ", decoded_frames=" << state.decoded_frames
              << ", decoded_audio_frames=" << state.decoded_audio_frames
              << ", decoder_errors=" << state.decoder_errors
              << ", audio_decoder_errors=" << state.audio_decoder_errors
              << ", encoded_packets=" << state.encoded_packets
              << ", published_packets=" << state.published_packets
              << ", read_audio_packets=" << state.read_audio_packets
              << ", published_audio_packets=" << state.published_audio_packets
              << ", empty_reads=" << empty_reads
              << ", timestamp_mapped=" << timestamp_stats.mapped_timestamps
              << ", timestamp_invalid=" << timestamp_stats.invalid_fallbacks
              << ", timestamp_uncertain=" << timestamp_stats.uncertain_fallbacks
              << ", timestamp_mcr=" << timestamp_stats.media_clock_restarts
              << ", timestamp_resets=" << timestamp_stats.discontinuity_resets
              << ", timestamp_wraps=" << timestamp_stats.forward_wraps << "\n";

    if (state.failed) {
        std::cerr << "pipeline failed: " << state.error << "\n";
        logger::Shutdown();
        return 1;
    }

    if (state.published_packets == 0) {
        std::cerr << "pipeline produced no published packet\n";
        logger::Shutdown();
        return 1;
    }

    logger::Shutdown();
    return 0;
}
