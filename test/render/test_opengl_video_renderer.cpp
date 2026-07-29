#include "render/render_session.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "media/decoder/ffmpeg_decoder.h"
#include "media/encoder/ffmpeg_encoder.h"
#include "media/puller/ffmpeg_puller.h"
#include "media/simple_buffer.h"

namespace {

constexpr int kSkipTest = 77;

// 摄像头端到端手动渲染测试配置，与 test_rtsp_server_publisher.cpp 中的
// TestCameraAudioVideoDecodeEncodePublish 使用同一路 RTSP 源。
constexpr const char* kManualCameraSourceUrl = "rtsp://192.168.66.83/live/mainstream";
constexpr int kManualConnectTimeoutMs = 5000;
constexpr int kManualReadTimeoutMs = 5000;
constexpr int kManualFallbackFps = 30;
constexpr int kManualTargetBitrate = 2'000'000;

struct CameraAvRenderState {
    std::unique_ptr<FFmpegEncoder> video_encoder;
    std::unique_ptr<FFmpegEncoder> audio_encoder;
    std::unique_ptr<render::RenderSession> render_session;

    int fps{kManualFallbackFps};
    int64_t frame_duration_us{1'000'000 / kManualFallbackFps};
    int64_t next_audio_pts_us{0};
    int decoded_video_frames{0};
    int decoded_audio_frames{0};
    int encoded_video_packets{0};
    int encoded_audio_packets{0};
    int read_video_packets{0};
    int read_audio_packets{0};
    int reported_video_frames{0};
    bool initialized{false};
    bool failed{false};
    bool closed{false};
    std::string error;
};

int ResolveManualFps(const VideoStreamInfo& video_info) {
    if (std::isfinite(video_info.fps) && video_info.fps >= 1.0f) {
        return std::max(1, static_cast<int>(std::lround(video_info.fps)));
    }
    return kManualFallbackFps;
}

void NormalizeCameraVideoFrameTime(CameraAvRenderState& state, MediaFrame& frame) {
    const auto frame_index =
        static_cast<int64_t>(std::max(0, state.decoded_video_frames - 1));
    const auto pts_us = frame_index * state.frame_duration_us;
    frame.time.pts_us = pts_us;
    frame.time.dts_us = pts_us;
    frame.time.duration_us = state.frame_duration_us;
}

bool InitializeCameraAvRender(CameraAvRenderState& state,
                              const MediaFrame& first_frame,
                              const VideoStreamInfo& video_info,
                              const MediaStreamInfo& audio_stream_info) {
    if (state.initialized) {
        return true;
    }

    const auto pixel_format = first_frame.GetPixelFormat();
    if (pixel_format == PixelFormat::kUnknown) {
        state.error = "decoded video frame pixel format is unknown";
        return false;
    }

    const int width = first_frame.Width() > 0 ? first_frame.Width() : video_info.width;
    const int height = first_frame.Height() > 0 ? first_frame.Height() : video_info.height;
    if (width <= 0 || height <= 0) {
        state.error = "invalid decoded video size";
        return false;
    }

    // 保留原 DecodeEncodePublish 测试里的编码链路：视频帧仍会编码成 H264，
    // 只是编码结果不再发布到 RTSP，而是把 decoded frame 直接交给 renderer 显示。
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
    encoder_config.bitrate = kManualTargetBitrate;
    encoder_config.time_base_num = 1;
    encoder_config.time_base_den = 1'000'000;
    encoder_config.encoder_name = "libx264";
    encoder_config.preset = "ultrafast";
    encoder_config.tune = "zerolatency";
    encoder_config.global_header = true;

    state.video_encoder = std::make_unique<FFmpegEncoder>();
    if (!state.video_encoder->Open(encoder_config)) {
        state.error = "failed to open H264 encoder";
        return false;
    }

    const auto& audio_info = audio_stream_info.get_detail<AudioStreamInfo>();
    EncoderConfig audio_encoder_config;
    audio_encoder_config.media_type = MediaType::AUDIO;
    audio_encoder_config.codec_type = CodecType::AAC;
    audio_encoder_config.audio.sample_rate = audio_info.sample_rate;
    audio_encoder_config.audio.channels = audio_info.channels;
    audio_encoder_config.audio.channel_layout = audio_info.channel_layout;
    audio_encoder_config.audio.sample_format = SampleFormat::S16;
    audio_encoder_config.bitrate = 64'000;
    audio_encoder_config.time_base_num = 1;
    audio_encoder_config.time_base_den = 1'000'000;
    audio_encoder_config.encoder_name = "aac";
    audio_encoder_config.global_header = true;

    state.audio_encoder = std::make_unique<FFmpegEncoder>();
    if (!state.audio_encoder->Open(audio_encoder_config)) {
        state.error = "failed to open AAC audio encoder";
        return false;
    }

    render::RenderSessionConfig render_config;
    render_config.video.window_width = width;
    render_config.video.window_height = height;
    render_config.video.title = "video-pipeline camera render test";
    render_config.video.visible = true;
    render_config.video.vsync = false;
    render_config.video.close_on_escape = true;
    // 真实 RTSP 摄像头的音频包不是严格按 20ms 匀速到达，当前手动测试还保留
    // H264/AAC 编码覆盖，会额外制造 CPU 抖动。这里给 WASAPI shared buffer 和
    // renderer 内部 PCM 队列多留一些余量，避免短时突发导致“刚满就丢、刚空就补静音”。
    render_config.audio.buffer_duration_ms = 200;
    render_config.audio.queue_capacity_chunks = 64;
    render_config.audio.fail_if_device_unavailable = true;
    render_config.enable_video = true;
    render_config.enable_audio = true;
    render_config.max_video_queue_frames = 6;
    render_config.max_audio_queue_frames = 50;

    // OpenGL context 与 WASAPI/COM 都由 RenderSession 的工作线程创建和释放。
    // decoder callback 从这里开始只负责投递 shared_ptr，不再直接碰窗口和声卡。
    state.render_session = std::make_unique<render::RenderSession>();
    if (!state.render_session->Init(render_config) ||
        !state.render_session->Start()) {
        state.error = "failed to start render session: " +
                      state.render_session->LastError();
        return false;
    }

    state.initialized = true;
    std::cout << "Camera decode/encode/render initialized: video="
              << width << "x" << height << " @" << state.fps
              << "fps, audio=" << audio_info.sample_rate
              << "Hz/" << audio_info.channels << "ch\n";
    std::cout << "Rendering camera stream. Press ESC or close the window to stop.\n"
              << std::flush;
    return true;
}

MediaFrame MakeRgbFrame() {
    // 构造一个很小的 RGB24 视频帧，作为无相机时的 OpenGL 手动 smoke 场景。
    std::vector<std::uint8_t> data{
        255, 0, 0,      0, 255, 0,
        0, 0, 255,      255, 255, 255,
    };

    VideoFrameMeta video_meta;
    video_meta.pixel_format = PixelFormat::kRGB24;
    video_meta.width = 2;
    video_meta.height = 2;
    video_meta.plane_count = 1;
    video_meta.plane_info[0].offset = 0;
    video_meta.plane_info[0].stride = 2 * 3;
    video_meta.plane_info[0].size = static_cast<int32_t>(data.size());

    MediaFrame frame;
    frame.type = MediaType::VIDEO;
    frame.meta = video_meta;
    frame.buffer = std::make_shared<SimpleBuffer>(std::move(data));
    return frame;
}

int TestStaticRgbRenderLoop() {
    render::RenderSessionConfig config;
    config.video.window_width = 640;
    config.video.window_height = 480;
    config.video.title = "video-pipeline OpenGL render test";
    config.video.visible = true;
    config.video.vsync = false;
    config.video.close_on_escape = true;
    config.enable_video = true;
    config.enable_audio = false;

    render::RenderSession session;
    if (!session.Init(config) || !session.Start()) {
        std::cerr << "RenderSession init unavailable; skipping render loop: "
                  << session.LastError() << '\n';
        return kSkipTest;
    }

    auto frame = std::make_shared<MediaFrame>(MakeRgbFrame());
    if (!session.SubmitFrame(frame)) {
        std::cerr << "RenderSession failed to accept a valid RGB frame\n";
        session.Stop();
        return 1;
    }

    std::cout << "Rendering static RGB frame. Press ESC to close.\n";
    while (session.IsRunning() && !session.ShouldClose()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    session.Stop();
    return 0;
}

int TestStaticRgbRenderSmoke() {
    render::RenderSessionConfig config;
    config.video.window_width = 64;
    config.video.window_height = 64;
    config.video.title = "video-pipeline hidden OpenGL smoke test";
    config.video.visible = false;
    config.video.vsync = false;
    config.video.close_on_escape = false;
    config.enable_video = true;
    config.enable_audio = false;

    render::RenderSession session;
    if (!session.Init(config) || !session.Start()) {
        std::cerr << "RenderSession init unavailable; skipping render smoke test: "
                  << session.LastError() << '\n';
        return kSkipTest;
    }

    if (!session.SubmitFrame(std::make_shared<MediaFrame>(MakeRgbFrame()))) {
        std::cerr << "RenderSession failed to accept a valid RGB frame\n";
        session.Stop();
        return 1;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (session.GetStats().rendered_video_frames == 0 &&
           session.IsRunning() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const auto stats = session.GetStats();
    session.Stop();
    if (stats.rendered_video_frames != 1) {
        std::cerr << "RenderSession did not render the submitted RGB frame: "
                  << session.LastError() << '\n';
        return 1;
    }
    return 0;
}

int TestCameraAudioVideoDecodeEncodeRender(int max_decoded_video_frames = 0) {
    FFmpegPuller puller;
    puller.SetConnectTimeoutMs(kManualConnectTimeoutMs);
    puller.SetReadTimeoutMs(kManualReadTimeoutMs);
    puller.SetRtspTransport("tcp");
    puller.SetRtspAutoSwitchToTcp(false);
    puller.SetLowLatency(true);

    std::cout << "Opening source: " << kManualCameraSourceUrl << "\n";
    if (!puller.Open(kManualCameraSourceUrl)) {
        std::cerr << "Failed to open source RTSP.\n";
        return 1;
    }

    const auto multi_info = puller.GetStreamInfo();
    if (!multi_info.HasVideoStream() || !multi_info.HasAudioStream()) {
        std::cerr << "Source must contain both video and audio streams.\n";
        puller.Close();
        return 1;
    }

    const auto video_stream_info =
        multi_info.stream_infos[multi_info.video_stream_idx_];
    const auto audio_stream_info =
        multi_info.stream_infos[multi_info.audio_stream_idx_];
    const auto video_detail = video_stream_info.get_detail<VideoStreamInfo>();
    const auto audio_detail = audio_stream_info.get_detail<AudioStreamInfo>();

    if (video_stream_info.codec_type != CodecType::H264) {
        std::cerr << "Expected H264 source video, got codec="
                  << static_cast<int>(video_stream_info.codec_type) << "\n";
        puller.Close();
        return 1;
    }
    if (audio_stream_info.codec_type != CodecType::G711A &&
        audio_stream_info.codec_type != CodecType::G711U) {
        std::cerr << "Expected G711 audio source, got codec="
                  << static_cast<int>(audio_stream_info.codec_type) << "\n";
        puller.Close();
        return 1;
    }

    std::cout << "Source video: " << video_detail.width << "x"
              << video_detail.height << " @" << video_detail.fps << "fps\n";
    std::cout << "Source audio: codec="
              << static_cast<int>(audio_stream_info.codec_type)
              << ", sample_rate=" << audio_detail.sample_rate
              << ", channels=" << audio_detail.channels << "\n" << std::flush;

    CameraAvRenderState state;
    state.fps = ResolveManualFps(video_detail);
    state.frame_duration_us = 1'000'000 / state.fps;

    FFmpegDecoder video_decoder;
    FFmpegDecoder audio_decoder;
    video_decoder.SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
        if (state.failed || state.closed || !frame || frame->type != MediaType::VIDEO) {
            return;
        }

        ++state.decoded_video_frames;
        NormalizeCameraVideoFrameTime(state, *frame);

        if (!state.initialized &&
            !InitializeCameraAvRender(state, *frame, video_detail, audio_stream_info)) {
            state.failed = true;
            return;
        }

        if (state.render_session && state.render_session->ShouldClose()) {
            state.closed = true;
            return;
        }

        if (!state.render_session || !state.render_session->SubmitFrame(frame)) {
            state.failed = true;
            state.error = state.render_session
                ? "failed to submit decoded video frame: " +
                      state.render_session->LastError()
                : "render session is not initialized";
            return;
        }

        // 编码只用于沿用原函数的 DecodeEncode 覆盖面，编码后的 packet 不再发布。
        std::vector<PacketPtr> encoded;
        if (!state.video_encoder->Encode(std::move(frame), encoded)) {
            state.failed = true;
            state.error = "failed to encode H264 video frame";
            return;
        }
        state.encoded_video_packets += static_cast<int>(encoded.size());
    });

    audio_decoder.SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
        if (state.failed || state.closed || !frame || frame->type != MediaType::AUDIO) {
            return;
        }

        ++state.decoded_audio_frames;
        if (!state.initialized || !state.audio_encoder) {
            return;
        }

        const int sample_rate = frame->SampleRate() > 0
            ? frame->SampleRate()
            : audio_detail.sample_rate;
        const int nb_samples = frame->NbSamples() > 0
            ? frame->NbSamples()
            : sample_rate / 50;
        const int64_t duration_us = sample_rate > 0
            ? static_cast<int64_t>(nb_samples) * 1'000'000LL / sample_rate
            : 20'000;
        frame->time.pts_us = state.next_audio_pts_us;
        frame->time.dts_us = frame->time.pts_us;
        frame->time.duration_us = duration_us;
        state.next_audio_pts_us += duration_us;

        if (!state.render_session || !state.render_session->SubmitFrame(frame)) {
            state.failed = true;
            state.error = state.render_session
                ? "failed to submit decoded audio frame: " +
                      state.render_session->LastError()
                : "render session is not initialized";
            return;
        }

        std::vector<PacketPtr> encoded;
        if (!state.audio_encoder->Encode(std::move(frame), encoded)) {
            state.failed = true;
            state.error = "failed to encode AAC audio frame";
            return;
        }
        state.encoded_audio_packets += static_cast<int>(encoded.size());
    });

    if (!video_decoder.Open(video_stream_info)) {
        std::cerr << "Failed to open video decoder.\n";
        puller.Close();
        return 1;
    }
    if (!audio_decoder.Open(audio_stream_info)) {
        std::cerr << "Failed to open audio decoder.\n";
        video_decoder.Close();
        puller.Close();
        return 1;
    }

    while (!state.failed && !state.closed) {
        if (state.render_session) {
            if (state.render_session->ShouldClose()) {
                state.closed = true;
                break;
            }
            if (!state.render_session->IsRunning()) {
                state.failed = true;
                state.error = "render session stopped unexpectedly: " +
                              state.render_session->LastError();
                break;
            }
        }

        std::shared_ptr<MediaPacket> packet;
        if (!puller.ReadPacket(packet)) {
            state.failed = true;
            state.error = "source ReadPacket failed";
            break;
        }
        if (!packet) {
            continue;
        }

        if (packet->type == MediaType::VIDEO) {
            ++state.read_video_packets;
            if (!video_decoder.Decode(packet)) {
                state.failed = true;
                state.error = "video Decode failed";
                break;
            }
        } else if (packet->type == MediaType::AUDIO) {
            ++state.read_audio_packets;
            if (!audio_decoder.Decode(packet)) {
                state.failed = true;
                state.error = "audio Decode failed";
                break;
            }
        }

        if (state.render_session &&
            state.decoded_video_frames >= state.reported_video_frames + 120) {
            state.reported_video_frames = state.decoded_video_frames;
            const auto stats = state.render_session->GetStats();
            // 这组日志用于手动判断阶段 4 同步策略是否真的在工作：
            // - dropped_video 是总视频丢帧，可能来自队列满，也可能来自 AV sync。
            // - avsync_dropped_video 只统计 AV sync 主动丢弃的晚帧。
            // - avsync_waits/avsync_wait_ms 反映早到视频帧被音频 master 节奏约束的程度。
            // - clock 显示当前 master 是 audio 还是 system fallback。
            // - normalized_video_pts 用于观察上游摄像头 PTS 是否稳定。
            std::cout << "camera render pipeline: read_video="
                      << state.read_video_packets
                      << ", read_audio=" << state.read_audio_packets
                      << ", decoded_video=" << state.decoded_video_frames
                      << ", decoded_audio=" << state.decoded_audio_frames
                      << ", rendered_video=" << stats.rendered_video_frames
                      << ", rendered_audio=" << stats.rendered_audio_frames
                      << ", dropped_video=" << stats.dropped_video_frames
                      << ", avsync_dropped_video=" << stats.av_sync_dropped_video_frames
                      << ", avsync_waits=" << stats.av_sync_video_waits
                      << ", avsync_wait_ms=" << stats.av_sync_video_wait_us / 1000
                      << ", clock=" << (stats.playback_clock_source == 1 ? "audio" : "system")
                      << ", normalized_video_pts=" << stats.normalized_video_pts_frames
                      << ", dropped_audio=" << stats.dropped_audio_frames
                      << ", audio_underruns=" << stats.audio_underruns
                      << ", audio_pcm_dropped=" << stats.audio_dropped_pcm_frames
                      << ", video_queue=" << stats.video_queue_size
                      << ", audio_queue=" << stats.audio_queue_size
                      << ", audio_renderer_queue=" << stats.audio_renderer_queue_size
                      << ", audio_renderer_frames=" << stats.audio_renderer_queued_frames
                      << ", encoded_video_packets=" << state.encoded_video_packets
                      << ", encoded_audio_packets=" << state.encoded_audio_packets
                      << "\n" << std::flush;
        }

        if (max_decoded_video_frames > 0 &&
            state.decoded_video_frames >= max_decoded_video_frames) {
            // --camera-frames 用于自动化验证：跑够指定解码视频帧后主动收尾，
            // 避免依赖人工关闭 GLFW 窗口，也避免命令超时导致 stdout 没有 flush。
            state.closed = true;
            break;
        }
    }

    render::RenderStats render_stats;
    if (state.render_session) {
        state.render_session->Stop();
        render_stats = state.render_session->GetStats();
    }
    if (state.video_encoder) {
        state.video_encoder->Close();
    }
    if (state.audio_encoder) {
        state.audio_encoder->Close();
    }
    audio_decoder.Close();
    video_decoder.Close();
    puller.Close();

    if (state.failed) {
        std::cerr << "Camera decode/encode/render failed: " << state.error << "\n";
        return 1;
    }

    std::cout << "Camera decode/encode/render stopped: rendered_video="
              << render_stats.rendered_video_frames
              << ", rendered_audio=" << render_stats.rendered_audio_frames
              << ", dropped_video=" << render_stats.dropped_video_frames
              << ", avsync_dropped_video=" << render_stats.av_sync_dropped_video_frames
              << ", avsync_waits=" << render_stats.av_sync_video_waits
              << ", avsync_wait_ms=" << render_stats.av_sync_video_wait_us / 1000
              << ", clock=" << (render_stats.playback_clock_source == 1 ? "audio" : "system")
              << ", normalized_video_pts=" << render_stats.normalized_video_pts_frames
              << ", dropped_audio=" << render_stats.dropped_audio_frames
              << ", audio_underruns=" << render_stats.audio_underruns
              << ", audio_pcm_dropped=" << render_stats.audio_dropped_pcm_frames
              << ", encoded_video_packets=" << state.encoded_video_packets
              << ", encoded_audio_packets=" << state.encoded_audio_packets
              << "\n" << std::flush;
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    const std::string mode = argc > 1 ? argv[1] : "--camera";
    if (mode == "--camera") {
        return TestCameraAudioVideoDecodeEncodeRender();
    }

    if (mode == "--camera-frames") {
        if (argc < 3) {
            std::cerr << "Usage: test_opengl_video_renderer --camera-frames <count>\n";
            return 1;
        }
        // 该模式仍然走真实 camera decode/encode/render 链路，只是增加自动退出条件。
        return TestCameraAudioVideoDecodeEncodeRender(std::stoi(argv[2]));
    }

    if (mode == "--smoke") {
        return TestStaticRgbRenderSmoke();
    }

    if (mode == "--loop") {
        return TestStaticRgbRenderLoop();
    }

    std::cerr << "Usage: test_opengl_video_renderer [--camera|--camera-frames <count>|--smoke|--loop]\n";
    return 1;
}
