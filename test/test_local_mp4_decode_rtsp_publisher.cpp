#include "media/decoder/ffmpeg_decoder.h"
#include "media/encoder/ffmpeg_encoder.h"
#include "media/puller/ffmpeg_puller.h"
#include "media/publisher/i_publisher.h"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef VIDEO_PIPELINE_TEST_MP4_PATH
#define VIDEO_PIPELINE_TEST_MP4_PATH "test.mp4"
#endif

namespace {

constexpr int kFallbackFps = 25;
constexpr int kDefaultMaxVideoPacketsToRead = 300;
constexpr int kDefaultMaxDecodedFrames = 120;
constexpr int kTargetBitrate = 1'500'000;
constexpr std::uint16_t kDefaultManualPort = 7852;
constexpr const char* kStreamPath = "/local/mp4";

struct RunOptions {
    std::uint16_t port{kDefaultManualPort};
    bool loop{true};
    bool self_test{false};
    bool real_time{true};
    int max_video_packets{0};
    int max_decoded_frames{0};
};

void PrintUsage(const char* exe) {
    std::cout
        << "Usage:\n"
        << "  " << exe << " [--loop|--once] [--port N] [--self-test]\n\n"
        << "Defaults are VLC-friendly: --loop --port 7852 --no-self-test --realtime\n"
        << "Open VLC with TCP transport: rtsp://127.0.0.1:7852/local/mp4\n";
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

RunOptions ParseArgs(int argc, char** argv) {
    RunOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            std::exit(0);
        }
        if (arg == "--loop") {
            options.loop = true;
            options.real_time = true;
            continue;
        }
        if (arg == "--once") {
            options.loop = false;
            options.self_test = true;
            options.real_time = false;
            continue;
        }
        if (arg == "--self-test") {
            options.self_test = true;
            continue;
        }
        if (arg == "--no-self-test") {
            options.self_test = false;
            continue;
        }
        if (arg == "--realtime") {
            options.real_time = true;
            continue;
        }
        if (arg == "--no-realtime") {
            options.real_time = false;
            continue;
        }
        if (arg == "--port") {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for --port");
            }
            options.port = ParsePort(argv[++i]);
            continue;
        }
        if (arg == "--max-video-packets") {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for --max-video-packets");
            }
            options.max_video_packets = ParseInt(argv[++i], "--max-video-packets");
            continue;
        }
        if (arg == "--max-decoded-frames") {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for --max-decoded-frames");
            }
            options.max_decoded_frames = ParseInt(argv[++i], "--max-decoded-frames");
            continue;
        }

        throw std::runtime_error("unknown argument: " + arg);
    }

    if (!options.loop) {
        if (options.max_video_packets <= 0) {
            options.max_video_packets = kDefaultMaxVideoPacketsToRead;
        }
        if (options.max_decoded_frames <= 0) {
            options.max_decoded_frames = kDefaultMaxDecodedFrames;
        }
    }

    return options;
}

int ResolveFps(const VideoStreamInfo& video_info) {
    if (std::isfinite(video_info.fps) && video_info.fps >= 1.0f) {
        return std::clamp(static_cast<int>(std::lround(video_info.fps)), 1, 60);
    }
    return kFallbackFps;
}

std::uint16_t FindFreeTcpPort() {
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor(
        io,
        {boost::asio::ip::make_address("127.0.0.1"), 0});
    return acceptor.local_endpoint().port();
}

class RtspTcpClient {
public:
    ~RtspTcpClient() {
        Close();
    }

    bool ConnectAndPlay(std::uint16_t port, const std::string& stream_path) {
        url_ = "rtsp://127.0.0.1:" + std::to_string(port) + stream_path;

        boost::system::error_code ec;
        socket_.connect({boost::asio::ip::make_address("127.0.0.1"), port}, ec);
        if (ec) {
            std::cerr << "RTSP client connect failed: " << ec.message() << "\n";
            return false;
        }
        socket_.non_blocking(true, ec);
        if (ec) {
            std::cerr << "RTSP client non_blocking failed: " << ec.message() << "\n";
            return false;
        }

        if (!SendAndExpectOk("DESCRIBE " + url_ + " RTSP/1.0\r\n"
                             "CSeq: 1\r\n"
                             "Accept: application/sdp\r\n\r\n")) {
            return false;
        }

        if (!SendAndExpectOk("SETUP " + url_ + "/track0 RTSP/1.0\r\n"
                             "CSeq: 2\r\n"
                             "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n")) {
            return false;
        }

        return SendAndExpectOk("PLAY " + url_ + " RTSP/1.0\r\n"
                               "CSeq: 3\r\n"
                               "Session: 00000001\r\n\r\n");
    }

    void Close() {
        boost::system::error_code ignored;
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
        interleaved_buffer_.clear();
    }

    bool ReadInterleavedFrame(std::chrono::milliseconds timeout) {
        std::array<std::uint8_t, 4096> chunk{};
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (std::chrono::steady_clock::now() < deadline) {
            boost::system::error_code ec;
            const auto n = socket_.read_some(boost::asio::buffer(chunk), ec);
            if (!ec && n > 0) {
                interleaved_buffer_.insert(interleaved_buffer_.end(),
                                           chunk.data(),
                                           chunk.data() + n);
                if (ConsumeInterleavedFrame()) {
                    return true;
                }
            } else if (ec == boost::asio::error::would_block ||
                       ec == boost::asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else if (ec) {
                return false;
            }
        }

        return false;
    }

private:
    bool SendAndExpectOk(const std::string& request) {
        boost::asio::write(socket_, boost::asio::buffer(request));
        const auto response = ReadUntilHeader(std::chrono::seconds(2));
        if (response.find("RTSP/1.0 200 OK") == std::string::npos) {
            std::cerr << "Unexpected RTSP response:\n" << response << "\n";
            return false;
        }
        return true;
    }

    std::string ReadUntilHeader(std::chrono::milliseconds timeout) {
        std::string data;
        std::array<char, 2048> chunk{};
        const auto deadline = std::chrono::steady_clock::now() + timeout;

        while (std::chrono::steady_clock::now() < deadline) {
            boost::system::error_code ec;
            const auto n = socket_.read_some(boost::asio::buffer(chunk), ec);
            if (!ec && n > 0) {
                data.append(chunk.data(), n);
                if (data.find("\r\n\r\n") != std::string::npos) {
                    return data;
                }
            } else if (ec == boost::asio::error::would_block ||
                       ec == boost::asio::error::try_again) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            } else if (ec) {
                return data;
            }
        }

        return data;
    }

    bool ConsumeInterleavedFrame() {
        auto dollar = std::find(
            interleaved_buffer_.begin(),
            interleaved_buffer_.end(),
            static_cast<std::uint8_t>('$'));
        if (dollar == interleaved_buffer_.end()) {
            interleaved_buffer_.clear();
            return false;
        }

        const auto offset = static_cast<std::size_t>(
            std::distance(interleaved_buffer_.begin(), dollar));
        if (interleaved_buffer_.size() < offset + 4) {
            return false;
        }

        const auto length = static_cast<std::uint16_t>(
            (interleaved_buffer_[offset + 2] << 8) |
            interleaved_buffer_[offset + 3]);
        if (interleaved_buffer_.size() < offset + 4 + length) {
            return false;
        }

        const auto frame_begin = offset;
        const auto rtp_begin = frame_begin + 4;
        if (length < 12 ||
            interleaved_buffer_[frame_begin] != '$' ||
            interleaved_buffer_[frame_begin + 1] != 0 ||
            (interleaved_buffer_[rtp_begin + 1] & 0x7F) != 96) {
            return false;
        }

        interleaved_buffer_.erase(
            interleaved_buffer_.begin(),
            interleaved_buffer_.begin() +
                static_cast<std::ptrdiff_t>(offset + 4 + length));
        return true;
    }

    boost::asio::io_context io_;
    boost::asio::ip::tcp::socket socket_{io_};
    std::string url_;
    std::vector<std::uint8_t> interleaved_buffer_;
};

struct PipelineState {
    std::unique_ptr<FFmpegEncoder> encoder;
    std::unique_ptr<IPublisher> publisher;
    RtspTcpClient rtsp_client;
    std::uint16_t rtsp_port{0};

    int fps{kFallbackFps};
    int64_t frame_duration_us{1'000'000 / kFallbackFps};
    int decoded_frames{0};
    int encoded_packets{0};
    int published_packets{0};
    int video_packets_read{0};

    bool initialized{false};
    bool received_rtp{false};
    bool failed{false};
    bool have_next_frame_time{false};
    std::chrono::steady_clock::time_point next_frame_time;
    std::string error;
};

void NormalizeFrameTime(PipelineState& state, MediaFrame& frame) {
    const auto frame_index = static_cast<int64_t>(std::max(0, state.decoded_frames - 1));
    const auto pts_us = frame_index * state.frame_duration_us;
    frame.time.pts_us = pts_us;
    frame.time.dts_us = pts_us;
    frame.time.duration_us = state.frame_duration_us;
}

void PaceIfNeeded(PipelineState& state, const RunOptions& options) {
    if (!options.real_time || state.failed) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!state.have_next_frame_time) {
        state.next_frame_time = now;
        state.have_next_frame_time = true;
    }

    state.next_frame_time += std::chrono::microseconds(state.frame_duration_us);
    if (state.next_frame_time > now) {
        std::this_thread::sleep_until(state.next_frame_time);
    }
}

bool PublishEncodedPackets(PipelineState& state,
                           const RunOptions& options,
                           std::vector<PacketPtr>& packets) {
    for (auto& packet : packets) {
        if (!packet) {
            continue;
        }

        packet->stream_index = 0;
        packet->time_base = Rational{1, 1'000'000};
        if (packet->duration <= 0) {
            packet->duration = state.frame_duration_us;
        }

        if (!state.publisher->Publish(*packet)) {
            state.error = "failed to publish encoded packet";
            return false;
        }

        ++state.encoded_packets;
        ++state.published_packets;

        if (options.self_test &&
            !state.received_rtp &&
            state.rtsp_client.ReadInterleavedFrame(std::chrono::milliseconds(300))) {
            state.received_rtp = true;
            state.rtsp_client.Close();
        }
    }

    return true;
}

bool InitializePipeline(PipelineState& state,
                        const RunOptions& options,
                        const MediaFrame& first_frame,
                        const VideoStreamInfo& video_info) {
    const auto pixel_format = first_frame.GetPixelFormat();
    if (pixel_format == PixelFormat::kUnknown) {
        state.error = "decoded frame pixel format is not supported by this test";
        return false;
    }

    const int width = first_frame.Width() > 0 ? first_frame.Width() : video_info.width;
    const int height = first_frame.Height() > 0 ? first_frame.Height() : video_info.height;
    if (width <= 0 || height <= 0) {
        state.error = "invalid decoded video size";
        return false;
    }

    EncoderConfig encoder_config;
    encoder_config.media_type = MediaType::VIDEO;
    encoder_config.codec_type = CodecType::H264;
    encoder_config.video.width = width;
    encoder_config.video.height = height;
    encoder_config.video.fps_num = state.fps;
    encoder_config.video.fps_den = 1;
    encoder_config.video.pixel_format = pixel_format;
    encoder_config.video.gop_size = state.fps;
    encoder_config.video.max_b_frames = 0;
    encoder_config.bitrate = kTargetBitrate;
    encoder_config.time_base_num = 1;
    encoder_config.time_base_den = 1'000'000;
    encoder_config.encoder_name = "libx264";
    encoder_config.preset = "ultrafast";
    encoder_config.tune = "zerolatency";
    encoder_config.global_header = true;

    state.encoder = std::make_unique<FFmpegEncoder>();
    if (!state.encoder->Open(encoder_config)) {
        state.error = "failed to open H264 encoder";
        return false;
    }

    PublisherConfig publisher_config;
    publisher_config.mode = PublishMode::PullServer;
    publisher_config.protocol = PublishProtocol::RtspServer;
    publisher_config.listen_host = "127.0.0.1";
    publisher_config.listen_port = state.rtsp_port;
    publisher_config.stream_path = kStreamPath;

    MediaTrackConfig track;
    track.track_id = 0;
    track.media_type = MediaType::VIDEO;
    track.codec_type = CodecType::H264;
    track.width = width;
    track.height = height;
    track.fps = static_cast<float>(state.fps);
    track.time_base_num = encoder_config.time_base_num;
    track.time_base_den = encoder_config.time_base_den;
    track.extra_data = state.encoder->GetExtraData();
    publisher_config.tracks.push_back(std::move(track));

    state.publisher = IPublisher::Create(publisher_config);
    if (!state.publisher || !state.publisher->Start(publisher_config)) {
        state.error = "failed to start local RTSP publisher";
        return false;
    }

    if (options.self_test &&
        !state.rtsp_client.ConnectAndPlay(state.rtsp_port, kStreamPath)) {
        state.error = "failed to connect RTSP verification client";
        return false;
    }

    state.initialized = true;
    std::cout << "Local MP4 pipeline initialized: "
              << width << "x" << height << " @" << state.fps
              << "fps, play=" << state.publisher->GetPlayUrl() << "\n";
    std::cout << "VLC must use RTSP over TCP for the current RTSP server MVP.\n";
    return true;
}

bool OpenInput(const std::filesystem::path& input_path,
               FFmpegPuller& puller,
               MediaStreamInfo& video_stream_info,
               VideoStreamInfo& video_detail) {
    puller.Close();

    if (!puller.Open(input_path.string())) {
        std::cerr << "Failed to open local MP4: " << input_path.string() << "\n";
        return false;
    }

    const auto multi_info = puller.GetStreamInfo();
    if (!multi_info.HasVideoStream()) {
        std::cerr << "Local MP4 has no video stream: " << input_path.string() << "\n";
        return false;
    }

    video_stream_info = multi_info.stream_infos[multi_info.video_stream_idx_];
    video_detail = video_stream_info.get_detail<VideoStreamInfo>();
    return true;
}

bool ShouldStop(const PipelineState& state, const RunOptions& options) {
    if (state.failed) {
        return true;
    }
    if (!options.loop && options.self_test && state.received_rtp) {
        return true;
    }
    if (options.max_video_packets > 0 &&
        state.video_packets_read >= options.max_video_packets) {
        return true;
    }
    if (options.max_decoded_frames > 0 &&
        state.decoded_frames >= options.max_decoded_frames) {
        return true;
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    RunOptions options;
    try {
        options = ParseArgs(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        PrintUsage(argv[0]);
        return 1;
    }

    const std::filesystem::path input_path{VIDEO_PIPELINE_TEST_MP4_PATH};
    if (!std::filesystem::exists(input_path)) {
        std::cerr << "Local MP4 sample not found: "
                  << input_path.string() << "\n";
        return 1;
    }

    FFmpegPuller puller;
    puller.SetConnectTimeoutMs(3000);
    puller.SetReadTimeoutMs(3000);
    puller.SetLowLatency(false);

    MediaStreamInfo video_stream_info;
    VideoStreamInfo video_detail;
    if (!OpenInput(input_path, puller, video_stream_info, video_detail)) {
        puller.Close();
        return 1;
    }

    PipelineState state;
    state.fps = ResolveFps(video_detail);
    state.frame_duration_us = 1'000'000 / state.fps;
    state.rtsp_port = options.port == 0 ? FindFreeTcpPort() : options.port;

    FFmpegDecoder decoder;
    decoder.SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
        if (state.failed || !frame || frame->type != MediaType::VIDEO) {
            return;
        }

        ++state.decoded_frames;
        NormalizeFrameTime(state, *frame);

        if (!state.initialized &&
            !InitializePipeline(state, options, *frame, video_detail)) {
            state.failed = true;
            return;
        }

        std::vector<PacketPtr> encoded;
        if (!state.encoder->Encode(std::move(frame), encoded)) {
            state.failed = true;
            state.error = "failed to encode decoded frame";
            return;
        }

        if (!PublishEncodedPackets(state, options, encoded)) {
            state.failed = true;
            return;
        }

        PaceIfNeeded(state, options);
    });

    if (!decoder.Open(video_stream_info)) {
        std::cerr << "Failed to open decoder for local MP4 video stream.\n";
        puller.Close();
        return 1;
    }

    while (!ShouldStop(state, options)) {
        std::shared_ptr<MediaPacket> packet;
        if (!puller.ReadPacket(packet)) {
            decoder.Close();
            puller.Close();

            if (!options.loop) {
                break;
            }

            if (!OpenInput(input_path, puller, video_stream_info, video_detail)) {
                state.failed = true;
                state.error = "failed to reopen local MP4";
                break;
            }

            if (!decoder.Open(video_stream_info)) {
                state.failed = true;
                state.error = "failed to reopen decoder for local MP4";
                break;
            }
            continue;
        }

        if (!packet || packet->type != MediaType::VIDEO) {
            continue;
        }

        ++state.video_packets_read;
        if (!decoder.Decode(packet)) {
            state.failed = true;
            state.error = "failed to decode local MP4 packet";
            break;
        }
    }

    decoder.Close();

    if (!state.failed && state.initialized && !options.loop) {
        std::vector<PacketPtr> flushed;
        if (!state.encoder->Encode(nullptr, flushed)) {
            state.failed = true;
            state.error = "failed to flush encoder";
        } else if (!PublishEncodedPackets(state, options, flushed)) {
            state.failed = true;
        }
    }

    if (state.publisher) {
        state.publisher->Stop();
    }
    if (state.encoder) {
        state.encoder->Close();
    }
    puller.Close();

    if (state.failed) {
        std::cerr << "Local MP4 decode/publish test failed: "
                  << state.error << "\n";
        return 1;
    }

    if (!state.initialized || state.published_packets == 0) {
        std::cerr << "Local MP4 decode/publish test published no packets.\n";
        return 1;
    }

    if (options.self_test && !state.received_rtp) {
        std::cerr << "Local MP4 decode/publish test did not receive RTP. "
                  << "decoded_frames=" << state.decoded_frames
                  << ", encoded_packets=" << state.encoded_packets
                  << ", published_packets=" << state.published_packets << "\n";
        return 1;
    }

    std::cout << "Local MP4 decode/publish test passed. decoded_frames="
              << state.decoded_frames
              << ", encoded_packets=" << state.encoded_packets
              << ", published_packets=" << state.published_packets << "\n";
    return 0;
}
