#include "render/opengl_video_renderer.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "media/decoder/ffmpeg_decoder.h"
#include "media/encoder/ffmpeg_encoder.h"
#include "media/puller/ffmpeg_puller.h"
#include "media/simple_buffer.h"
#include "render/audio/wasapi_audio_renderer.h"

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
    std::unique_ptr<render::OpenGLVideoRenderer> renderer;
    std::unique_ptr<render::audio::WasapiAudioRenderer> audio_renderer;

    int fps{kManualFallbackFps};
    int64_t frame_duration_us{1'000'000 / kManualFallbackFps};
    int64_t next_audio_pts_us{0};
    int decoded_video_frames{0};
    int decoded_audio_frames{0};
    int encoded_video_packets{0};
    int encoded_audio_packets{0};
    int rendered_video_frames{0};
    int rendered_audio_frames{0};
    int read_video_packets{0};
    int read_audio_packets{0};
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

    render::RenderConfig render_config;
    render_config.window_width = width;
    render_config.window_height = height;
    render_config.title = "video-pipeline camera render test";
    render_config.visible = true;
    render_config.vsync = false;
    render_config.close_on_escape = true;

    state.renderer = std::make_unique<render::OpenGLVideoRenderer>();
    if (!state.renderer->Init(render_config)) {
        state.error = "failed to initialize OpenGL renderer";
        return false;
    }

    render::audio::AudioRenderConfig audio_render_config;
    audio_render_config.buffer_duration_ms = 100;
    audio_render_config.fail_if_device_unavailable = true;

    state.audio_renderer = std::make_unique<render::audio::WasapiAudioRenderer>();
    if (!state.audio_renderer->Init(audio_render_config)) {
        state.error = "failed to initialize WASAPI audio renderer: " +
                      state.audio_renderer->LastError();
        return false;
    }

    state.initialized = true;
    std::cout << "Camera decode/encode/render initialized: video="
              << width << "x" << height << " @" << state.fps
              << "fps, audio=" << audio_info.sample_rate
              << "Hz/" << audio_info.channels << "ch\n";
    std::cout << "Rendering camera stream. Press ESC or close the window to stop.\n";
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
    render::RenderConfig config;
    config.window_width = 640;
    config.window_height = 480;
    config.title = "video-pipeline OpenGL render test";
    config.visible = true;
    config.vsync = false;
    config.close_on_escape = true;

    render::OpenGLVideoRenderer renderer;
    if (!renderer.Init(config)) {
        std::cerr << "OpenGL renderer init unavailable; skipping render smoke test\n";
        return kSkipTest;
    }

    auto frame = MakeRgbFrame();
    if (!renderer.Render(frame)) {
        std::cerr << "OpenGL renderer failed to render a valid RGB frame\n";
        renderer.Shutdown();
        return 1;
    }

    std::cout << "Rendering static RGB frame. Press ESC to close.\n";
    while (!renderer.ShouldClose()) {
        renderer.Render(frame);
        renderer.PollEvents();
    }

    renderer.Shutdown();
    return 0;
}

int TestStaticRgbRenderSmoke() {
    render::RenderConfig config;
    config.window_width = 64;
    config.window_height = 64;
    config.title = "video-pipeline hidden OpenGL smoke test";
    config.visible = false;
    config.vsync = false;
    config.close_on_escape = false;

    render::OpenGLVideoRenderer renderer;
    if (!renderer.Init(config)) {
        std::cerr << "OpenGL renderer init unavailable; skipping render smoke test\n";
        return kSkipTest;
    }

    auto frame = MakeRgbFrame();
    if (!renderer.Render(frame)) {
        std::cerr << "OpenGL renderer failed to render a valid RGB frame\n";
        renderer.Shutdown();
        return 1;
    }

    renderer.PollEvents();
    renderer.Shutdown();
    return 0;
}

int TestCameraAudioVideoDecodeEncodeRender() {
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
              << ", channels=" << audio_detail.channels << "\n";

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

        if (state.renderer && state.renderer->ShouldClose()) {
            state.closed = true;
            return;
        }

        if (!state.renderer || !state.renderer->Render(*frame)) {
            state.failed = true;
            state.error = "failed to render decoded video frame";
            return;
        }
        ++state.rendered_video_frames;
        state.renderer->PollEvents();

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

        if (!state.audio_renderer || !state.audio_renderer->Render(*frame)) {
            state.failed = true;
            state.error = state.audio_renderer
                ? "failed to render decoded audio frame: " +
                      state.audio_renderer->LastError()
                : "audio renderer is not initialized";
            return;
        }
        ++state.rendered_audio_frames;

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
        if (state.renderer) {
            state.renderer->PollEvents();
            if (state.renderer->ShouldClose()) {
                state.closed = true;
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

        if (state.rendered_video_frames > 0 &&
            state.rendered_video_frames % 120 == 0) {
            std::cout << "camera render pipeline: read_video="
                      << state.read_video_packets
                      << ", read_audio=" << state.read_audio_packets
                      << ", decoded_video=" << state.decoded_video_frames
                      << ", decoded_audio=" << state.decoded_audio_frames
                      << ", rendered_video=" << state.rendered_video_frames
                      << ", rendered_audio=" << state.rendered_audio_frames
                      << ", encoded_video_packets=" << state.encoded_video_packets
                      << ", encoded_audio_packets=" << state.encoded_audio_packets
                      << "\n";
        }
    }

    if (state.renderer) {
        state.renderer->Shutdown();
    }
    if (state.audio_renderer) {
        state.audio_renderer->Shutdown();
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
              << state.rendered_video_frames
              << ", rendered_audio=" << state.rendered_audio_frames
              << ", encoded_video_packets=" << state.encoded_video_packets
              << ", encoded_audio_packets=" << state.encoded_audio_packets << "\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    const std::string mode = argc > 1 ? argv[1] : "--camera";
    if (mode == "--camera") {
        return TestCameraAudioVideoDecodeEncodeRender();
    }

    if (mode == "--smoke") {
        return TestStaticRgbRenderSmoke();
    }

    if (mode == "--loop") {
        return TestStaticRgbRenderLoop();
    }

    std::cerr << "Usage: test_opengl_video_renderer [--camera|--smoke|--loop]\n";
    return 1;
}
