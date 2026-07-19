#include "common/log/logger.h"
#include "common/process/process.h"
#include "media/decoder/ffmpeg_decoder.h"
#include "media/encoder/ffmpeg_encoder.h"
#include "media/puller/ffmpeg_puller.h"
#include "media/pusher/ffmpeg_pusher.h"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kSourceRtspUrl = "rtsp://192.168.24.169/live/mainstream";
constexpr const char* kPushRtmpUrl = "rtmp://127.0.0.1/live/video_pipeline_test";
constexpr const char* kVerifyRtspUrl = "rtsp://127.0.0.1/live/video_pipeline_test";
constexpr const char* kMediaServerPath =
    R"(E:\share\project\video-pipeline\third_apps\win32\zlmediakit\MediaServer.exe)";

constexpr unsigned short kZlmHttpPort = 8888;
constexpr unsigned short kZlmRtmpPort = 1935;
constexpr unsigned short kRtcSignalingPort = 13000;
constexpr unsigned short kRtcSignalingSslPort = 13001;

constexpr int kConnectTimeoutMs = 5000;
constexpr int kReadTimeoutMs = 5000;
constexpr int kZlmStartupTimeoutMs = 10000;
constexpr int kPipelineRunSeconds = 2000;
constexpr int kMaxEncodedPackets = 5000;
constexpr int kVerifyAfterPackets = 30;
constexpr int kTargetBitrate = 2'000'000;
constexpr int kFallbackFps = 25;

std::string TrimLeft(std::string value) {
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), [](unsigned char ch) {
                    return ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n';
                }));
    return value;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

bool CanConnectTcp(const std::string& host, unsigned short port) {
    boost::asio::io_context io;
    boost::asio::ip::tcp::socket socket(io);
    boost::system::error_code ec;

    socket.connect(
        boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address(host), port),
        ec);
    return !ec;
}

std::filesystem::path MakeRuntimeConfig(const std::filesystem::path& media_server) {
    const auto source = media_server.parent_path() / "config.ini";
    if (!std::filesystem::exists(source)) {
        throw std::runtime_error("ZLMediaKit config.ini not found: " + source.string());
    }

    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto output = std::filesystem::temp_directory_path() /
                  ("video_pipeline_e2e_zlm_" + std::to_string(stamp) + ".ini");

    std::ifstream in(source, std::ios::binary);
    if (!in) {
        throw std::runtime_error("failed to open ZLMediaKit config template: " + source.string());
    }

    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to create ZLMediaKit runtime config: " + output.string());
    }

    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        const auto trimmed = TrimLeft(line);
        if (!trimmed.empty() && trimmed.front() == '[') {
            const auto end = trimmed.find(']');
            section = end == std::string::npos ? std::string{} : trimmed.substr(0, end + 1);
        }

        if (section == "[rtc]" && StartsWith(trimmed, "signalingPort=")) {
            out << "signalingPort=" << kRtcSignalingPort << "\n";
            continue;
        }

        if (section == "[rtc]" && StartsWith(trimmed, "signalingSslPort=")) {
            out << "signalingSslPort=" << kRtcSignalingSslPort << "\n";
            continue;
        }

        out << line << "\n";
    }

    return output;
}

class ZlmServerGuard {
public:
    ZlmServerGuard()
        : process_(io_.get_executor()) {}

    ~ZlmServerGuard() {
        Stop();
    }

    bool Start() {
        if (CanConnectTcp("127.0.0.1", kZlmHttpPort) &&
            CanConnectTcp("127.0.0.1", kZlmRtmpPort)) {
            std::cout << "Reuse existing ZLMediaKit on HTTP " << kZlmHttpPort
                      << " and RTMP " << kZlmRtmpPort << ".\n";
            return true;
        }

        const std::filesystem::path executable{kMediaServerPath};
        if (!std::filesystem::exists(executable)) {
            std::cerr << "MediaServer.exe not found: " << executable.string() << "\n";
            return false;
        }

        try {
            runtime_config_ = MakeRuntimeConfig(executable);
        } catch (const std::exception& e) {
            std::cerr << e.what() << "\n";
            return false;
        }

        common::process::ProcessOptions options;
        options.executable = executable;
        options.working_directory = executable.parent_path();
        options.arguments = {"-c", runtime_config_.string()};
        options.console_mode = common::process::ConsoleMode::NewConsole;
        options.window_mode = common::process::WindowMode::Hidden;

        boost::system::error_code ec;
        if (!process_.Start(options, ec)) {
            std::cerr << "Failed to start MediaServer: " << ec.message()
                      << " (" << process_.LastError() << ")\n";
            return false;
        }

        owns_process_ = true;
        std::cout << "Started MediaServer, pid=" << process_.Pid()
                  << ", config=" << runtime_config_.string() << "\n";

        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kZlmStartupTimeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            boost::system::error_code running_ec;
            if (!process_.IsRunning(running_ec)) {
                std::cerr << "MediaServer exited during startup, exit_code="
                          << process_.ExitCode() << "\n";
                return false;
            }

            if (CanConnectTcp("127.0.0.1", kZlmHttpPort) &&
                CanConnectTcp("127.0.0.1", kZlmRtmpPort)) {
                std::cout << "ZLMediaKit is ready.\n";
                return true;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }

        std::cerr << "Timed out waiting for ZLMediaKit ports.\n";
        return false;
    }

    void Stop() {
        boost::system::error_code ec;
        if (owns_process_ && process_.IsRunning(ec)) {
            process_.RequestExit(ec);
            for (int i = 0; i < 20; ++i) {
                boost::system::error_code running_ec;
                if (!process_.IsRunning(running_ec)) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            boost::system::error_code running_ec;
            if (process_.IsRunning(running_ec)) {
                process_.Terminate(ec);
                process_.Wait(ec);
            }
        }

        owns_process_ = false;
        if (!runtime_config_.empty()) {
            std::filesystem::remove(runtime_config_, ec);
            runtime_config_.clear();
        }
    }

private:
    boost::asio::io_context io_;
    common::process::Process process_;
    std::filesystem::path runtime_config_;
    bool owns_process_{false};
};

int ResolveFps(const VideoStreamInfo& video_info) {
    if (std::isfinite(video_info.fps) && video_info.fps >= 1.0f) {
        return std::clamp(static_cast<int>(std::lround(video_info.fps)), 1, 60);
    }
    return kFallbackFps;
}

bool VerifyOutputRtspOnce() {
    FFmpegPuller verifier;
    verifier.SetConnectTimeoutMs(3000);
    verifier.SetReadTimeoutMs(3000);
    verifier.SetRtspTransport("tcp");
    verifier.SetRtspAutoSwitchToTcp(false);
    verifier.SetLowLatency(true);

    if (!verifier.Open(kVerifyRtspUrl)) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        std::shared_ptr<MediaPacket> packet;
        if (!verifier.ReadPacket(packet)) {
            return false;
        }
        if (packet && packet->type == MediaType::VIDEO) {
            std::cout << "Verified output RTSP can be pulled: "
                      << kVerifyRtspUrl << "\n";
            return true;
        }
    }

    return false;
}

struct PipelineState {
    std::unique_ptr<FFmpegEncoder> encoder;
    std::unique_ptr<IPusher> pusher;
    int64_t frame_duration_us{1'000'000 / kFallbackFps};
    int fps{kFallbackFps};
    int decoded_frames{0};
    int encoded_packets{0};
    int pushed_packets{0};
    int keyframes{0};
    bool initialized{false};
    bool failed{false};
    bool output_verified{false};
    std::string error;
};

bool InitializeEncodeAndPush(PipelineState& state,
                             const MediaFrame& first_frame,
                             const VideoStreamInfo& video_info) {
    const auto pixel_format = first_frame.GetPixelFormat();
    if (pixel_format == PixelFormat::kUnknown) {
        state.error = "decoded frame pixel format is unknown";
        return false;
    }

    EncoderConfig encoder_config;
    encoder_config.media_type = MediaType::VIDEO;
    encoder_config.codec_type = CodecType::H264;
    encoder_config.video.width = video_info.width;
    encoder_config.video.height = video_info.height;
    encoder_config.video.fps_num = state.fps;
    encoder_config.video.fps_den = 1;
    encoder_config.video.pixel_format = pixel_format;
    encoder_config.video.gop_size = state.fps * 2;
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
        state.error = "failed to open FFmpegEncoder";
        return false;
    }

    PusherConfig pusher_config;
    pusher_config.url = kPushRtmpUrl;
    pusher_config.format_name = "flv";
    pusher_config.media_type = MediaType::VIDEO;
    pusher_config.codec_type = CodecType::H264;
    pusher_config.width = video_info.width;
    pusher_config.height = video_info.height;
    pusher_config.time_base_num = encoder_config.time_base_num;
    pusher_config.time_base_den = encoder_config.time_base_den;
    pusher_config.extra_data = state.encoder->GetExtraData();

    if (pusher_config.extra_data.empty()) {
        std::cout << "Warning: encoder extradata is empty; continuing anyway.\n";
    }

    state.pusher = IPusher::Create(std::move(pusher_config));
    if (!state.pusher || !state.pusher->Connect()) {
        state.error = "failed to connect FFmpegPusher to ZLMediaKit";
        return false;
    }

    state.initialized = true;
    std::cout << "Pipeline initialized: " << video_info.width << "x"
              << video_info.height << " @" << state.fps
              << "fps, push=" << kPushRtmpUrl << "\n";
    std::cout << "Try playing: " << kVerifyRtspUrl << "\n";
    return true;
}

void NormalizeFrameTime(PipelineState& state, MediaFrame& frame) {
    const auto frame_index = static_cast<int64_t>(std::max(0, state.decoded_frames - 1));
    const auto pts_us = frame_index * state.frame_duration_us;
    frame.time.pts_us = pts_us;
    frame.time.dts_us = pts_us;
    frame.time.duration_us = state.frame_duration_us;
}

bool PushEncodedPackets(PipelineState& state, std::vector<PacketPtr>& packets) {
    for (auto& packet : packets) {
        if (!packet) {
            continue;
        }

        packet->time_base = Rational{1, 1'000'000};
        if (packet->duration <= 0) {
            packet->duration = state.frame_duration_us;
        }

        ++state.encoded_packets;
        if (packet->keyframe) {
            ++state.keyframes;
        }

        if (!state.pusher->Send(*packet)) {
            state.error = "failed to send encoded packet to ZLMediaKit";
            return false;
        }
        ++state.pushed_packets;

        if (state.pushed_packets % 50 == 0) {
            std::cout << "pushed packets=" << state.pushed_packets
                      << ", decoded frames=" << state.decoded_frames
                      << ", keyframes=" << state.keyframes << "\n";
        }
    }

    return true;
}

} // namespace

int main() {
    ai_camera::log::Init("video_pipeline_e2e");

    ZlmServerGuard zlm;
    if (!zlm.Start()) {
        ai_camera::log::Shutdown();
        return 1;
    }

    FFmpegPuller puller;
    puller.SetConnectTimeoutMs(kConnectTimeoutMs);
    puller.SetReadTimeoutMs(kReadTimeoutMs);
    puller.SetRtspTransport("tcp");
    puller.SetRtspAutoSwitchToTcp(false);
    puller.SetLowLatency(true);

    std::cout << "Opening source: " << kSourceRtspUrl << "\n";
    if (!puller.Open(kSourceRtspUrl)) {
        std::cerr << "Failed to open source RTSP.\n";
        ai_camera::log::Shutdown();
        return 1;
    }

    const auto multi_info = puller.GetStreamInfo();
    if (!multi_info.HasVideoStream()) {
        std::cerr << "Source has no video stream.\n";
        ai_camera::log::Shutdown();
        return 1;
    }

    const auto video_stream_info = multi_info.stream_infos[multi_info.video_stream_idx_];
    const auto video_detail = video_stream_info.get_detail<VideoStreamInfo>();

    FFmpegDecoder decoder;
    PipelineState state;
    state.fps = ResolveFps(video_detail);
    state.frame_duration_us = 1'000'000 / state.fps;

    decoder.SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
        if (state.failed || !frame || frame->type != MediaType::VIDEO) {
            return;
        }

        ++state.decoded_frames;
        NormalizeFrameTime(state, *frame);

        if (!state.initialized &&
            !InitializeEncodeAndPush(state, *frame, video_detail)) {
            state.failed = true;
            return;
        }

        std::vector<PacketPtr> encoded;
        if (!state.encoder->Encode(std::move(frame), encoded)) {
            state.failed = true;
            state.error = "failed to encode video frame";
            return;
        }

        if (!PushEncodedPackets(state, encoded)) {
            state.failed = true;
        }
    });

    if (!decoder.Open(video_stream_info)) {
        std::cerr << "Failed to open video decoder.\n";
        ai_camera::log::Shutdown();
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(kPipelineRunSeconds);
    auto next_verify_time = std::chrono::steady_clock::now();
    std::future<bool> verify_future;

    auto poll_verify = [&]() {
        if (!verify_future.valid()) {
            return;
        }

        if (verify_future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            return;
        }

        if (verify_future.get()) {
            state.output_verified = true;
        } else {
            next_verify_time = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
    };

    while (!state.failed &&
           state.encoded_packets < kMaxEncodedPackets &&
           std::chrono::steady_clock::now() < deadline) {
        poll_verify();

        std::shared_ptr<MediaPacket> packet;
        if (!puller.ReadPacket(packet)) {
            state.failed = true;
            state.error = "source ReadPacket failed";
            break;
        }

        if (!packet || packet->type != MediaType::VIDEO) {
            continue;
        }

        if (!decoder.Decode(packet)) {
            state.failed = true;
            state.error = "video Decode failed";
            break;
        }

        if (!state.output_verified &&
            !verify_future.valid() &&
            state.pushed_packets >= kVerifyAfterPackets &&
            std::chrono::steady_clock::now() >= next_verify_time) {
            verify_future = std::async(std::launch::async, VerifyOutputRtspOnce);
        }
    }

    poll_verify();
    if (!state.output_verified && verify_future.valid()) {
        state.output_verified = verify_future.get();
    }

    if (!state.failed && state.encoder && state.pusher) {
        std::vector<PacketPtr> flushed;
        if (state.encoder->Encode(nullptr, flushed)) {
            (void)PushEncodedPackets(state, flushed);
        }
    }

    if (state.pusher) {
        state.pusher->Close();
    }
    if (state.encoder) {
        state.encoder->Close();
    }
    decoder.Close();
    puller.Close();

    if (state.failed) {
        std::cerr << "Pipeline failed: " << state.error << "\n";
        ai_camera::log::Shutdown();
        return 1;
    }

    if (!state.output_verified) {
        std::cerr << "Pipeline pushed packets, but output RTSP verification failed: "
                  << kVerifyRtspUrl << "\n";
        ai_camera::log::Shutdown();
        return 1;
    }

    std::cout << "Pipeline test passed. decoded_frames=" << state.decoded_frames
              << ", encoded_packets=" << state.encoded_packets
              << ", pushed_packets=" << state.pushed_packets
              << ", keyframes=" << state.keyframes << "\n";
    std::cout << "RTMP publish URL: " << kPushRtmpUrl << "\n";
    std::cout << "RTSP play URL:    " << kVerifyRtspUrl << "\n";

    ai_camera::log::Shutdown();
    return 0;
}
