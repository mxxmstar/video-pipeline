#include "common/log/logger.h"
#include "media/decoder/ffmpeg_decoder.h"
#include "media/puller/avtp_puller.h"
#include "render/render_session.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr const char* kDefaultAvtpUrl =
    "avtp://\\Device\\NPF_{00087179-AD1C-4C01-904C-11127E5E94C2}"
    "?format=auto"
    "&probe_timeout=5000"
    "&read_timeout=100"
    "&probe_packets=2000";
constexpr int kDefaultFps = 25;
constexpr int kDefaultMaxFrames = INT_MAX;
constexpr int kDefaultDurationSeconds = 30 * 6000;
constexpr int kDefaultDrainMs = 3000;
constexpr int kSkipTest = 77;

struct RunOptions {
    std::string avtp_url{kDefaultAvtpUrl};
    int fps{kDefaultFps};
    int max_frames{kDefaultMaxFrames};
    int duration_seconds{kDefaultDurationSeconds};
    int drain_ms{kDefaultDrainMs};
    bool visible{true};
};

struct RenderProbeState {
    std::unique_ptr<render::RenderSession> render_session;
    bool initialized{false};
    bool failed{false};
    bool closed{false};
    bool enable_audio{false};
    std::string error;

    int fps{kDefaultFps};
    int64_t frame_duration_us{1'000'000 / kDefaultFps};
    int decoded_video_frames{0};
    int decoded_audio_frames{0};
    int submitted_video_frames{0};
    int submitted_audio_frames{0};
    int video_decoder_errors{0};
    int audio_decoder_errors{0};
    int read_video_packets{0};
    int read_audio_packets{0};
    int64_t next_audio_pts_us{0};
};

void PrintUsage(const char* exe) {
    std::cout
        << "Usage:\n"
        << "  " << exe << " [avtp-url] [--fps N] [--max-frames N]\n"
        << "       [--duration-sec N] [--drain-ms N] [--hidden]\n";
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

RunOptions ParseArgs(int argc, char** argv) {
    RunOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            PrintUsage(argv[0]);
            std::exit(0);
        }
        if (arg == "--hidden") {
            options.visible = false;
            continue;
        }
        if (arg == "--fps") {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for --fps");
            }
            options.fps = std::max(1, ParseInt(argv[++i], "--fps"));
            continue;
        }
        if (arg == "--max-frames") {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for --max-frames");
            }
            options.max_frames = std::max(1, ParseInt(argv[++i], "--max-frames"));
            continue;
        }
        if (arg == "--duration-sec") {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for --duration-sec");
            }
            options.duration_seconds = std::max(1, ParseInt(argv[++i], "--duration-sec"));
            continue;
        }
        if (arg == "--drain-ms") {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for --drain-ms");
            }
            options.drain_ms = std::max(0, ParseInt(argv[++i], "--drain-ms"));
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

void NormalizeVideoTime(RenderProbeState& state, MediaFrame& frame) {
    const auto frame_index =
        static_cast<int64_t>(std::max(0, state.decoded_video_frames - 1));
    const auto pts_us = frame_index * state.frame_duration_us;
    frame.time.pts_us = pts_us;
    frame.time.dts_us = pts_us;
    frame.time.duration_us = state.frame_duration_us;
}

void NormalizeAudioTime(RenderProbeState& state, MediaFrame& frame) {
    int64_t duration_us = frame.time.duration_us;
    if (duration_us <= 0) {
        const auto* audio = frame.AudioMeta();
        if (audio && audio->sample_rate > 0 && audio->nb_samples > 0) {
            duration_us = static_cast<int64_t>(
                static_cast<int64_t>(audio->nb_samples) * 1000000LL /
                static_cast<int64_t>(audio->sample_rate));
        }
    }
    if (duration_us <= 0) {
        duration_us = 20'000;
    }

    frame.time.pts_us = state.next_audio_pts_us;
    frame.time.dts_us = state.next_audio_pts_us;
    frame.time.duration_us = duration_us;
    state.next_audio_pts_us += duration_us;
}

bool InitializeRender(RenderProbeState& state,
                      const RunOptions& options,
                      const MediaFrame& first_frame) {
    const int width = first_frame.Width();
    const int height = first_frame.Height();
    if (width <= 0 || height <= 0) {
        state.error = "invalid decoded video size";
        return false;
    }

    render::RenderSessionConfig config;
    config.video.window_width = width;
    config.video.window_height = height;
    config.video.title = "video-pipeline AVTP A/V render probe";
    config.video.visible = options.visible;
    config.video.vsync = false;
    config.video.close_on_escape = true;
    config.audio.buffer_duration_ms = 200;
    config.audio.queue_capacity_chunks = 64;
    config.audio.fail_if_device_unavailable = false;
    config.enable_video = true;
    config.enable_audio = state.enable_audio;
    // AVTP probe 在 codec probe 阶段会先缓存一批包，进入 render 后可能短时间突发解码。
    // 这里把视频队列放宽到本次验证帧数，避免测试工具自身的队列满丢帧掩盖真实渲染情况。
    config.max_video_queue_frames =
        static_cast<std::size_t>(std::clamp(options.max_frames, 6, 120));
    config.max_audio_queue_frames = 64;
    // 这个 probe 的目的只是确认 AVTP -> decoder -> render 的通路是否正确，不做
    // 播放同步验证；同步策略留给 camera/RTSP 场景单独回归，避免 probe 本身把短时
    // 突发提交误判成“晚帧”而大量丢弃。
    config.av_sync.enabled = false;

    state.render_session = std::make_unique<render::RenderSession>();
    if (!state.render_session->Init(config) ||
        !state.render_session->Start()) {
        state.error = "failed to start RenderSession: " +
                      state.render_session->LastError();
        return false;
    }

    state.initialized = true;
    std::cout << "AVTP render initialized: " << width << "x" << height
              << ", audio=" << (state.enable_audio ? "on" : "off")
              << ", visible=" << (options.visible ? "true" : "false") << "\n";
    return true;
}

void HandleVideoFrame(RenderProbeState& state,
                      const RunOptions& options,
                      std::shared_ptr<MediaFrame> frame) {
    if (state.failed || !frame || frame->type != MediaType::VIDEO) {
        return;
    }

    ++state.decoded_video_frames;
    NormalizeVideoTime(state, *frame);

    if (!state.initialized &&
        !InitializeRender(state, options, *frame)) {
        state.failed = true;
        return;
    }

    if (!state.render_session || !state.render_session->SubmitFrame(frame)) {
        state.failed = true;
        state.error = state.render_session
            ? state.render_session->LastError()
            : "render session is null";
        return;
    }
    ++state.submitted_video_frames;
}

void HandleAudioFrame(RenderProbeState& state, std::shared_ptr<MediaFrame> frame) {
    if (state.failed || !frame || frame->type != MediaType::AUDIO) {
        return;
    }
    ++state.decoded_audio_frames;

    // RenderSession 要等首个视频帧确定窗口尺寸后才能启动。启动前解码出的音频帧
    // 当前不会提交给 renderer，因此也不能提前推进本测试工具维护的音频时间轴。
    // 否则第一帧真正提交的音频可能已经带着几百毫秒 PTS，音频 renderer 一旦上报
    // audio master，后续 PTS 从 0 开始的视频帧会被 AV sync 误判为严重晚到并丢弃。
    if (!state.initialized || !state.render_session) {
        return;
    }

    NormalizeAudioTime(state, *frame);
    if (!state.render_session->SubmitFrame(frame)) {
        state.failed = true;
        state.error = state.render_session->LastError();
        return;
    }
    ++state.submitted_audio_frames;
}

void PrintRenderStats(const char* prefix,
                      const RenderProbeState& state,
                      const render::RenderStats& stats) {
    std::cout << prefix
              << "read_video=" << state.read_video_packets
              << ", read_audio=" << state.read_audio_packets
              << ", decoded_video=" << state.decoded_video_frames
              << ", decoded_audio=" << state.decoded_audio_frames
              << ", submitted_video=" << state.submitted_video_frames
              << ", submitted_audio=" << state.submitted_audio_frames
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
              << "\n" << std::flush;
}

void DrainRenderQueues(RenderProbeState& state, const RunOptions& options) {
    if (!state.initialized || !state.render_session || options.drain_ms <= 0) {
        return;
    }

    // RenderSession 的视频线程会按 PTS/AV sync 节奏显示，不应该在主循环刚达到
    // --max-frames 后立刻 Stop()。这里给 renderer 一个短暂收尾窗口，让已提交的
    // 音视频帧被消费完；否则手动验证会出现“submitted_video 很多但 rendered_video
    // 只有 1”的假象。
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(options.drain_ms);
    while (!state.render_session->ShouldClose() &&
           state.render_session->IsRunning() &&
           std::chrono::steady_clock::now() < deadline) {
        const auto stats = state.render_session->GetStats();
        const auto finished_video =
            stats.rendered_video_frames + stats.dropped_video_frames;
        const auto finished_audio =
            stats.rendered_audio_frames + stats.dropped_audio_frames;
        if (finished_video >= state.submitted_video_frames &&
            finished_audio >= state.submitted_audio_frames) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

} // namespace

int main(int argc, char** argv) {
    logger::Init("avtp_render_probe");

    RunOptions options;
    try {
        options = ParseArgs(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << "\n";
        PrintUsage(argv[0]);
        logger::Shutdown();
        return 1;
    }

    AvtpPuller puller;
    std::cout << "Opening AVTP source:\n  " << options.avtp_url << "\n";
    if (!puller.Open(options.avtp_url)) {
        std::cerr << "failed to open AVTP source\n";
        logger::Shutdown();
        return 1;
    }

    const MultiStreamInfo streams = puller.GetStreamInfo();
    if (!streams.HasVideoStream()) {
        std::cerr << "AVTP source has no video stream info\n";
        puller.Close();
        logger::Shutdown();
        return 1;
    }

    RenderProbeState state;
    state.fps = options.fps;
    state.frame_duration_us = 1'000'000 / std::max(1, options.fps);
    state.enable_audio = streams.HasAudioStream();

    FFmpegDecoder video_decoder;
    FFmpegDecoder audio_decoder;
    video_decoder.SetFrameCallback(
        [&](std::shared_ptr<MediaFrame> frame) {
            HandleVideoFrame(state, options, std::move(frame));
        });
    if (!video_decoder.Open(streams.stream_infos[streams.video_stream_idx_])) {
        std::cerr << "failed to open video decoder\n";
        puller.Close();
        logger::Shutdown();
        return 1;
    }

    if (streams.HasAudioStream()) {
        audio_decoder.SetFrameCallback(
            [&](std::shared_ptr<MediaFrame> frame) {
                HandleAudioFrame(state, std::move(frame));
            });
        if (!audio_decoder.Open(streams.stream_infos[streams.audio_stream_idx_])) {
            std::cerr << "failed to open audio decoder\n";
            video_decoder.Close();
            puller.Close();
            logger::Shutdown();
            return 1;
        }
    }

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(options.duration_seconds);
    while (!state.failed &&
           !state.closed &&
           state.decoded_video_frames < options.max_frames &&
           std::chrono::steady_clock::now() < deadline) {
        if (state.render_session) {
            if (state.render_session->ShouldClose()) {
                state.closed = true;
                break;
            }
            if (!state.render_session->IsRunning()) {
                state.failed = true;
                state.error = "render session stopped: " +
                              state.render_session->LastError();
                break;
            }
        }

        std::shared_ptr<MediaPacket> packet;
        if (!puller.ReadPacket(packet)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (!packet) {
            continue;
        }

        if (packet->type == MediaType::AUDIO) {
            ++state.read_audio_packets;
            if (streams.HasAudioStream() && !audio_decoder.Decode(packet)) {
                ++state.audio_decoder_errors;
            }
        } else if (packet->type == MediaType::VIDEO) {
            ++state.read_video_packets;
            if (!video_decoder.Decode(packet)) {
                ++state.video_decoder_errors;
                if (state.initialized || state.video_decoder_errors > 100) {
                    state.failed = true;
                    state.error = "video decoder rejected too many packets";
                    break;
                }
            }
        }

        if (state.initialized && state.decoded_video_frames % 120 == 0) {
            const auto stats = state.render_session->GetStats();
            PrintRenderStats("avtp render: ", state, stats);
        }
    }

    render::RenderStats stats;
    if (state.render_session) {
        DrainRenderQueues(state, options);
        state.render_session->Stop();
        stats = state.render_session->GetStats();
    }
    audio_decoder.Close();
    video_decoder.Close();
    const auto timestamp_stats = puller.GetStats().timestamp_mapper;
    puller.Close();

    PrintRenderStats("Summary: ", state, stats);
    std::cout << "Decoder summary: video_decoder_errors="
              << state.video_decoder_errors
              << ", audio_decoder_errors=" << state.audio_decoder_errors
              << ", timestamp_mapped=" << timestamp_stats.mapped_timestamps
              << ", timestamp_invalid=" << timestamp_stats.invalid_fallbacks
              << ", timestamp_uncertain=" << timestamp_stats.uncertain_fallbacks
              << ", timestamp_mcr=" << timestamp_stats.media_clock_restarts
              << ", timestamp_resets=" << timestamp_stats.discontinuity_resets
              << ", timestamp_wraps=" << timestamp_stats.forward_wraps
              << "\n";

    if (state.failed) {
        std::cerr << "AVTP render failed: " << state.error << "\n";
        logger::Shutdown();
        return 1;
    }
    if (!state.initialized) {
        std::cerr << "AVTP render did not initialize\n";
        logger::Shutdown();
        return kSkipTest;
    }

    logger::Shutdown();
    return 0;
}
