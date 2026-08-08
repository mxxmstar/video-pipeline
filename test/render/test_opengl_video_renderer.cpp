#include "render/render_session.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
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
#include "render/playback_profile.h"

namespace {

constexpr int kSkipTest = 77;

// 摄像头端到端手动渲染测试配置，与 test_rtsp_server_publisher.cpp 中的
// TestCameraAudioVideoDecodeEncodePublish 使用同一路 RTSP 源。
constexpr const char* kDefaultRtspSourceUrl =
    "rtsp://192.168.66.83/live/mainstream";
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
    bool encode_frames{true};
    /// 仅供本地验收 A/B 对照：关闭时视频不按 audio master 做 PTS 等待/丢帧。
    bool av_sync_enabled{true};
    /// 真实流的音频播放缓冲可能领先视频显示；该值用于验收不同视频缓冲窗口。
    std::size_t max_video_queue_frames{6};
    /// 音频启动等待期间保留的解码帧数量，用于覆盖 RTSP 的首批音频突发。
    std::size_t max_audio_queue_frames{50};
    mediaflow::PlaybackMode playback_mode{mediaflow::PlaybackMode::Custom};
    mediaflow::PlaybackProfile playback_profile{};
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

void EnsureVideoFrameTime(CameraAvRenderState& state, MediaFrame& frame) {
    // FFmpegDecoder 已经把容器时间基换算为微秒。有效的源 PTS/DTS 必须保留，
    // 否则后续记录到的“音频 master 对视频显示误差”只是人工帧率时间轴的误差，
    // 不能反映真实 RTSP 传输和解码链路。只有源缺失时间戳时才生成兜底值。
    if (frame.time.pts_us != kNoTimestamp) {
        if (frame.time.dts_us == kNoTimestamp) {
            frame.time.dts_us = frame.time.pts_us;
        }
        if (frame.time.duration_us <= 0) {
            frame.time.duration_us = state.frame_duration_us;
        }
        return;
    }

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

    const auto& audio_info = audio_stream_info.get_detail<AudioStreamInfo>();
    if (state.encode_frames) {
    // 保留原 DecodeEncodePublish 测试里的编码链路：视频帧仍会编码成 H264，
    // 真实播放同步验收可以关闭这段额外负载，避免把编码性能误判为渲染同步问题。
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
    }

    render::RenderSessionConfig render_config;
    render_config.video.window_width = width;
    render_config.video.window_height = height;
    render_config.video.title = "video-pipeline camera render test";
    render_config.video.visible = true;
    render_config.video.vsync = false;
    render_config.video.close_on_escape = true;
    if (state.playback_mode != mediaflow::PlaybackMode::Custom) {
        // 长测使用公开 PlaybackProfile 生成的完整渲染配置，确保两组结果不只是
        // 手工修改 RenderSession 的视频队列，而是同时覆盖设备 buffer、PCM 队列、
        // A/V 队列和晚帧策略。
        render_config = render::MakeRenderSessionConfig(state.playback_profile);
        render_config.video.window_width = width;
        render_config.video.window_height = height;
        render_config.video.title = "video-pipeline camera render test";
        render_config.video.visible = true;
        render_config.video.vsync = false;
        render_config.video.close_on_escape = true;
    } else {
        // 兼容原有手动摄像头测试：保留其较大的设备 buffer 和 PCM 队列。
        render_config.audio.buffer_duration_ms = 200;
        render_config.audio.queue_capacity_chunks = 64;
    }
    render_config.audio.fail_if_device_unavailable = true;
    render_config.enable_video = true;
    render_config.enable_audio = true;
    if (state.playback_mode == mediaflow::PlaybackMode::Custom) {
        render_config.max_video_queue_frames = state.max_video_queue_frames;
        render_config.max_audio_queue_frames = state.max_audio_queue_frames;
    }
    render_config.av_sync.enabled = state.av_sync_enabled;

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
              << "Hz/" << audio_info.channels << "ch, queues=video/"
              << render_config.max_video_queue_frames << ", audio/"
              << render_config.max_audio_queue_frames
              << ", profile="
              << (state.playback_mode == mediaflow::PlaybackMode::LowLatencyPreview
                      ? "low-latency"
                      : state.playback_mode == mediaflow::PlaybackMode::StablePlayback
                            ? "stable"
                            : "manual")
              << ", device_buffer_ms="
              << render_config.audio.buffer_duration_ms
              << ", playback_buffer_ms=" << render_config.playback_buffer_ms
              << "\n";
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

int TestCameraAudioVideoDecodeEncodeRender(
    const std::string& source_url = kDefaultRtspSourceUrl,
    int max_decoded_video_frames = 0,
    bool encode_frames = true,
    bool av_sync_enabled = true,
    std::size_t max_video_queue_frames = 6,
    std::size_t max_audio_queue_frames = 50,
    int drain_after_input_ms = 1500,
    mediaflow::PlaybackMode playback_mode = mediaflow::PlaybackMode::Custom) {
    FFmpegPuller puller;
    puller.SetConnectTimeoutMs(kManualConnectTimeoutMs);
    puller.SetReadTimeoutMs(kManualReadTimeoutMs);
    puller.SetRtspTransport("tcp");
    puller.SetRtspAutoSwitchToTcp(false);
    puller.SetLowLatency(true);

    std::cout << "Opening source: " << source_url << "\n";
    if (!puller.Open(source_url)) {
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
        audio_stream_info.codec_type != CodecType::G711U &&
        audio_stream_info.codec_type != CodecType::AAC) {
        std::cerr << "Expected G711 or AAC audio source, got codec="
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
    state.encode_frames = encode_frames;
    state.av_sync_enabled = av_sync_enabled;
    state.max_video_queue_frames = max_video_queue_frames;
    state.max_audio_queue_frames = max_audio_queue_frames;
    state.playback_mode = playback_mode;
    if (playback_mode == mediaflow::PlaybackMode::LowLatencyPreview) {
        state.playback_profile = mediaflow::PlaybackProfile::LowLatencyPreview();
    } else if (playback_mode == mediaflow::PlaybackMode::StablePlayback) {
        state.playback_profile = mediaflow::PlaybackProfile::StablePlayback();
    }
    state.fps = ResolveManualFps(video_detail);
    state.frame_duration_us = 1'000'000 / state.fps;
    if (state.playback_mode != mediaflow::PlaybackMode::Custom) {
        state.playback_profile.expected_video_frame_rate = state.fps;
    }

    FFmpegDecoder video_decoder;
    FFmpegDecoder audio_decoder;
    video_decoder.SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
        if (state.failed || state.closed || !frame || frame->type != MediaType::VIDEO) {
            return;
        }

        ++state.decoded_video_frames;
        EnsureVideoFrameTime(state, *frame);

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
            if (state.playback_mode == mediaflow::PlaybackMode::StablePlayback &&
                state.render_session && state.render_session->IsRunning()) {
                // StablePlayback 在视频队列满时保留旧帧、拒绝新帧；这属于有界
                // 队列的背压结果，不是设备或 RenderSession 生命周期故障。
                return;
            }
            state.failed = true;
            state.error = state.render_session
                ? "failed to submit decoded video frame: " +
                      state.render_session->LastError()
                : "render session is not initialized";
            return;
        }

        // 编码模式保留原 DecodeEncodeRender 覆盖面；render-only 模式只验证
        // 解码后的真实 PTS、RenderSession 和音频设备，不引入编码 CPU 压力。
        if (state.video_encoder) {
            std::vector<PacketPtr> encoded;
            if (!state.video_encoder->Encode(std::move(frame), encoded)) {
                state.failed = true;
                state.error = "failed to encode H264 video frame";
                return;
            }
            state.encoded_video_packets += static_cast<int>(encoded.size());
        }
    });

    audio_decoder.SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
        if (state.failed || state.closed || !frame || frame->type != MediaType::AUDIO) {
            return;
        }

        ++state.decoded_audio_frames;
        if (!state.initialized) {
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
        // 音频同样优先保留 Decoder 的实际 PTS。缺失时才沿用上一个音频帧
        // 的结束位置，保证没有源 PTS 的旧摄像头仍可完成手动渲染验证。
        if (frame->time.pts_us == kNoTimestamp) {
            frame->time.pts_us = state.next_audio_pts_us;
        }
        if (frame->time.dts_us == kNoTimestamp) {
            frame->time.dts_us = frame->time.pts_us;
        }
        frame->time.duration_us = duration_us;
        state.next_audio_pts_us = frame->time.pts_us + duration_us;

        if (!state.render_session || !state.render_session->SubmitFrame(frame)) {
            state.failed = true;
            state.error = state.render_session
                ? "failed to submit decoded audio frame: " +
                      state.render_session->LastError()
                : "render session is not initialized";
            return;
        }

        if (state.audio_encoder) {
            std::vector<PacketPtr> encoded;
            if (!state.audio_encoder->Encode(std::move(frame), encoded)) {
                state.failed = true;
                state.error = "failed to encode AAC audio frame";
                return;
            }
            state.encoded_audio_packets += static_cast<int>(encoded.size());
        }
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

    bool reached_frame_limit = false;
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
                      << ", catch_up_dropped_video="
                      << stats.video_catch_up_dropped_frames
                      << ", catch_up_events=" << stats.video_catch_up_events
                      << ", avsync_waits=" << stats.av_sync_video_waits
                      << ", avsync_wait_ms=" << stats.av_sync_video_wait_us / 1000
                      << ", clock=" << (stats.playback_clock_source == 1 ? "audio" : "system")
                      << ", avsync_samples="
                      << stats.video_audio_master_sync_samples
                      << ", avsync_last_error_us="
                      << stats.last_video_audio_master_error_us
                      << ", avsync_error_range_us="
                      << stats.min_video_audio_master_error_us << "/"
                      << stats.max_video_audio_master_error_us
                      << ", avsync_abs_avg_us="
                      << (stats.video_audio_master_sync_samples > 0
                              ? stats.total_abs_video_audio_master_error_us /
                                    stats.video_audio_master_sync_samples
                              : 0)
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
            // 自动化验收停止读取新包后不能立即 Stop：视频 PTS 调度和 WASAPI
            // 仍可能持有少量已解码帧。记录帧数上限后由下方受控排空，避免把
            // 正常在途帧误计为播放端丢失。
            reached_frame_limit = true;
            break;
        }
    }

    if (reached_frame_limit && state.render_session &&
        drain_after_input_ms > 0) {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(drain_after_input_ms);
        while (std::chrono::steady_clock::now() < deadline &&
               !state.render_session->ShouldClose() &&
               state.render_session->IsRunning()) {
            const auto stats = state.render_session->GetStats();
            if (stats.video_queue_size == 0 && stats.audio_queue_size == 0 &&
                stats.audio_renderer_queue_size == 0) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
              << ", catch_up_dropped_video="
              << render_stats.video_catch_up_dropped_frames
              << ", catch_up_events=" << render_stats.video_catch_up_events
              << ", avsync_waits=" << render_stats.av_sync_video_waits
              << ", avsync_wait_ms=" << render_stats.av_sync_video_wait_us / 1000
              << ", clock=" << (render_stats.playback_clock_source == 1 ? "audio" : "system")
              << ", avsync_samples="
              << render_stats.video_audio_master_sync_samples
              << ", avsync_last_error_us="
              << render_stats.last_video_audio_master_error_us
              << ", avsync_error_range_us="
              << render_stats.min_video_audio_master_error_us << "/"
              << render_stats.max_video_audio_master_error_us
              << ", avsync_abs_avg_us="
              << (render_stats.video_audio_master_sync_samples > 0
                      ? render_stats.total_abs_video_audio_master_error_us /
                            render_stats.video_audio_master_sync_samples
                      : 0)
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
        return TestCameraAudioVideoDecodeEncodeRender(
            kDefaultRtspSourceUrl, std::stoi(argv[2]));
    }

    if (mode == "--rtsp-url") {
        if (argc < 3) {
            std::cerr << "Usage: test_opengl_video_renderer --rtsp-url <url> "
                         "[--frames <count>] [--render-only] "
                         "[--playback-profile low-latency|stable]\n";
            return 1;
        }
        const std::string source_url = argv[2];
        if (source_url.empty()) {
            std::cerr << "RTSP URL must not be empty\n";
            return 1;
        }
        int max_frames = 0;
        bool render_only = false;
        bool av_sync_enabled = true;
        std::size_t max_video_queue_frames = 6;
        std::size_t max_audio_queue_frames = 50;
        int drain_after_input_ms = 1500;
        mediaflow::PlaybackMode playback_mode =
            mediaflow::PlaybackMode::Custom;
        for (int index = 3; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--frames" && index + 1 < argc) {
                try {
                    max_frames = std::max(0, std::stoi(argv[++index]));
                } catch (...) {
                    std::cerr << "Invalid --frames value\n";
                    return 1;
                }
            } else if (option == "--render-only") {
                render_only = true;
            } else if (option == "--no-av-sync") {
                // 保留默认同步策略；该开关只用于将真实播放瓶颈归因到
                // PTS 调度或输入/渲染吞吐，而不是生产配置。
                av_sync_enabled = false;
            } else if (option == "--video-queue-frames" && index + 1 < argc) {
                try {
                    const int value = std::stoi(argv[++index]);
                    if (value <= 0) {
                        throw std::out_of_range("video queue frames");
                    }
                    max_video_queue_frames = static_cast<std::size_t>(value);
                } catch (...) {
                    std::cerr << "Invalid --video-queue-frames value\n";
                    return 1;
                }
            } else if (option == "--audio-queue-frames" && index + 1 < argc) {
                try {
                    const int value = std::stoi(argv[++index]);
                    if (value <= 0) {
                        throw std::out_of_range("audio queue frames");
                    }
                    max_audio_queue_frames = static_cast<std::size_t>(value);
                } catch (...) {
                    std::cerr << "Invalid --audio-queue-frames value\n";
                    return 1;
                }
            } else if (option == "--drain-ms" && index + 1 < argc) {
                try {
                    drain_after_input_ms = std::max(0, std::stoi(argv[++index]));
                } catch (...) {
                    std::cerr << "Invalid --drain-ms value\n";
                    return 1;
                }
            } else if (option == "--playback-profile" && index + 1 < argc) {
                const std::string profile = argv[++index];
                if (profile == "low-latency") {
                    playback_mode = mediaflow::PlaybackMode::LowLatencyPreview;
                } else if (profile == "stable") {
                    playback_mode = mediaflow::PlaybackMode::StablePlayback;
                } else {
                    std::cerr << "Invalid --playback-profile value, expected "
                                 "low-latency or stable\n";
                    return 1;
                }
            } else {
                std::cerr << "Invalid --rtsp-url option\n";
                return 1;
            }
        }
        return TestCameraAudioVideoDecodeEncodeRender(
            source_url, max_frames, !render_only, av_sync_enabled,
            max_video_queue_frames, max_audio_queue_frames, drain_after_input_ms,
            playback_mode);
    }

    if (mode == "--smoke") {
        return TestStaticRgbRenderSmoke();
    }

    if (mode == "--loop") {
        return TestStaticRgbRenderLoop();
    }

    std::cerr << "Usage: test_opengl_video_renderer "
                 "[--camera|--camera-frames <count>|--rtsp-url <url> "
                 "[--frames <count>] [--render-only] [--no-av-sync] "
                 "[--video-queue-frames <count>] [--audio-queue-frames <count>] "
                 "[--drain-ms <count>] "
                 "[--playback-profile low-latency|stable]|--smoke|--loop]\n";
    return 1;
}
