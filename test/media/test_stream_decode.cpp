/// @file test_stream_decode.cpp
/// @brief 使用 MediaFlow 运行 RTSP 音视频解码链路的交互式验证程序。

#include "mediaflow/mediaflow.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#endif

#include "media/decoder/ffmpeg_decoder.h"
#include "media/ffmpeg_frame_buffer.h"
#include "media/puller/ffmpeg_puller.h"

namespace {

using namespace mediaflow;

/**
 * @brief 统计一路解码帧的终端节点。
 *
 * 这个节点只做验证和日志输出，不把帧复制到其他缓存。帧的线程归属由
 * MediaFlow Graph 管理，因此测试主线程只读取原子计数，不直接碰 Decoder。
 */
class FrameCounterSink final : public SinkNode<MediaFrameMessage> {
public:
    FrameCounterSink(MediaType media_type, std::string name)
        : media_type_(media_type), name_(std::move(name)) {}

    std::string Name() const override {
        return name_;
    }

    int Count() const {
        return frame_count_.load();
    }

protected:
    void Process(MediaFrameMessage message) override {
        // EOS 是控制消息，不属于实际解码帧；正常停止时它仍会经过该节点，
        // 但不能把它计入媒体帧数量。
        if (!message.Valid() || message.eos || !message.frame ||
            message.frame->type != media_type_) {
            return;
        }

        const int count = frame_count_.fetch_add(1) + 1;
        if (count % LogInterval() != 1) {
            return;
        }

        if (media_type_ == MediaType::VIDEO) {
            const auto* meta = message.frame->VideoMeta();
            if (meta) {
                LOG_INFO("[MediaFlow][Video] frame #{}: {}x{}, format={}, pts_us={}",
                         count,
                         meta->width,
                         meta->height,
                         static_cast<int>(meta->pixel_format),
                         message.frame->time.pts_us);
            }
            return;
        }

        const auto* meta = message.frame->AudioMeta();
        if (meta) {
            LOG_INFO("[MediaFlow][Audio] frame #{}: sample_rate={}, channels={}, "
                     "samples={}, pts_us={}",
                     count,
                     meta->sample_rate,
                     meta->channels,
                     meta->nb_samples,
                     message.frame->time.pts_us);
        }
    }

private:
    int LogInterval() const {
        return media_type_ == MediaType::VIDEO ? 30 : 100;
    }

    MediaType media_type_;
    std::string name_;
    std::atomic<int> frame_count_{0};
};

void PrintGraphError(const Graph& graph, const std::string& operation) {
    const auto error = graph.LastError();
    std::cerr << operation << " failed: " << error.message << "\n";
}

void PrintEdgeDiagnostics(const char* name, const EdgeMetricsSnapshot& metrics) {
    const auto& budget = metrics.budget;
    std::cout << "  " << name
              << ": accepted=" << metrics.accepted
              << ", dropped_newest=" << metrics.dropped_newest
              << ", dropped_oldest=" << metrics.dropped_oldest
              << ", rejected=" << metrics.rejected
              << ", queue=(items=" << budget.items
              << ", bytes=" << budget.bytes
              << ", span_us=" << budget.span_us
              << ", high=" << budget.items_high_watermark << "/"
              << budget.bytes_high_watermark << "/"
              << budget.span_high_watermark_us
              << ", watermarks=" << budget.high_watermark_enters << "/"
              << budget.high_watermark_leaves
              << ", limits=" << budget.limit_items << "/"
              << budget.limit_bytes << "/" << budget.limit_span
              << ", oversized=" << budget.oversized
              << ", keyframes=" << budget.dropped_keyframes
              << ", timestamps=" << budget.timestamp_invalid << "/"
              << budget.timestamp_discontinuity << ")\n";
}

std::uint64_t CurrentProcessRssBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters,
                             sizeof(counters)) != 0) {
        return static_cast<std::uint64_t>(counters.WorkingSetSize);
    }
#endif
    return 0;
}

struct ProcessCpuSnapshot {
    std::uint64_t total_100ns{0};
    bool valid{false};
};

ProcessCpuSnapshot CurrentProcessCpuTime() {
#if defined(_WIN32)
    FILETIME creation_time{};
    FILETIME exit_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time,
                         &kernel_time, &user_time)) {
        return {};
    }

    ULARGE_INTEGER kernel_ticks{};
    ULARGE_INTEGER user_ticks{};
    kernel_ticks.LowPart = kernel_time.dwLowDateTime;
    kernel_ticks.HighPart = kernel_time.dwHighDateTime;
    user_ticks.LowPart = user_time.dwLowDateTime;
    user_ticks.HighPart = user_time.dwHighDateTime;
    return {kernel_ticks.QuadPart + user_ticks.QuadPart, true};
#else
    return {};
#endif
}

double ProcessCpuPercent(const ProcessCpuSnapshot& begin,
                         const ProcessCpuSnapshot& end,
                         std::uint64_t wall_ms) {
    if (!begin.valid || !end.valid || end.total_100ns < begin.total_100ns ||
        wall_ms == 0) {
        return -1.0;
    }

    // FILETIME 的单位为 100 ns；结果表示“单核等效 CPU 百分比”，多线程进程
    // 可以超过 100%，便于在相同机器和相同构建配置下比较 CPU 增量。
    const auto cpu_100ns = end.total_100ns - begin.total_100ns;
    const double wall_100ns = static_cast<double>(wall_ms) * 10000.0;
    return static_cast<double>(cpu_100ns) / wall_100ns * 100.0;
}

} // namespace

int main(int argc, char* argv[]) {
    std::cout << "=== MediaFlow RTSP Stream Decode ===\n";

    constexpr const char* kDefaultUrl =
        "rtsp://192.168.66.83/live/mainstream";
    int duration_ms = 0;
    std::string stream_url = kDefaultUrl;
    std::string rtsp_transport = "tcp";
    // 默认地址保留为已长期验证的摄像头；--url 只改变本次测试输入，避免为
    // 每个设备手工修改源码后忘记还原。未知参数直接失败，防止拼写错误导致
    // 在错误配置下采集到不可比较的性能数据。
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        if (option == "--duration-ms" && index + 1 < argc) {
            try {
                duration_ms = std::max(0, std::stoi(argv[++index]));
            } catch (...) {
                std::cerr << "Invalid --duration-ms value\n";
                return 1;
            }
        } else if (option == "--url" && index + 1 < argc) {
            stream_url = argv[++index];
            if (stream_url.empty()) {
                std::cerr << "Invalid --url value\n";
                return 1;
            }
        } else if (option == "--rtsp-transport" && index + 1 < argc) {
            rtsp_transport = argv[++index];
            if (rtsp_transport != "tcp" && rtsp_transport != "udp") {
                std::cerr << "Invalid --rtsp-transport value, expected tcp or udp\n";
                return 1;
            }
        } else {
            std::cerr << "Usage: test_stream_decode [--duration-ms N] [--url URL] "
                         "[--rtsp-transport tcp|udp]\n";
            return 1;
        }
    }

    // 连接参数仍由测试程序集中设置，SourceNode 只负责把 Puller 的读取结果
    // 转换成 MediaPacketMessage，不在业务节点中重新实现 FFmpeg I/O。
    auto puller = std::make_unique<FFmpegPuller>();
    // 默认使用已有摄像头发布和渲染测试的 TCP 配置；UDP 验收通过命令行显式
    // 选择，并关闭自动 TCP 回退，避免把 UDP 失败误判为 UDP/TCP 兼容通过。
    puller->SetConnectTimeoutMs(5000);
    puller->SetReadTimeoutMs(5000);
    puller->SetRtspTransport(rtsp_transport);
    puller->SetRtspAutoSwitchToTcp(false);
    puller->SetLowLatency(true);

    auto executor = std::make_shared<AsioExecutor>("stream-decode", 2);

    // 真实 RTSP 拉流器可能一次性读出网络接收缓冲区中的一段历史数据，
    // 因此 Source 在启动初期会短暂快于 Decoder。压缩包预算由目标积压时间、
    // 预估码率和 2 倍突发系数计算，max_items 只作为码率未知和异常时间戳时的
    // 兜底；不能再把 2048/8192 当作所有设备和所有场景的固定默认值。
    NodeOptions decode_node_options{
        NodeExecutionMode::Serialized,
        4096};
    decode_node_options.prefer_video_keyframes = true;
    // 队列预算属于 Pipeline 配置。设备适配层可在这里复制默认配置并覆盖轨道
    // 参数，Graph 连接阶段只接收已经生成好的边配置。
    const MediaFlowPipelineConfig pipeline_config;
    const auto video_packet_edge_options =
        pipeline_config.MakePacketEdgeOptions(QueueTrack::Video);
    const auto audio_packet_edge_options =
        pipeline_config.MakePacketEdgeOptions(QueueTrack::Audio);
    // 解码后的原始帧不适合根据压缩码率估算字节数。这里使用帧数和媒体时间
    // 窗口限流，待 PipelineBuilder 能获得像素格式和分辨率后再补充原始帧字节预算。
    const auto video_frame_edge_options =
        pipeline_config.MakeFrameEdgeOptions(QueueTrack::Video);
    const auto audio_frame_edge_options =
        pipeline_config.MakeFrameEdgeOptions(QueueTrack::Audio);
    Graph graph;

    // Graph 会先启动 Router、Decoder 和统计 Sink，最后才启动 Source 并打开
    // RTSP。这样第一条音频或视频包到来时，下游端口已经全部注册完成。
    if (!graph.AddNode<StreamSourceNode>(
            "source", executor, decode_node_options, "test-stream",
            std::move(puller), stream_url, StreamSourceNodeOptions{100, -1})) {
        PrintGraphError(graph, "add source");
        return 1;
    }
    if (!graph.AddNode<TrackRouterNode>(
            "router", executor, decode_node_options, TrackSelection{},
            TrackSelection{})) {
        PrintGraphError(graph, "add router");
        return 1;
    }
    if (!graph.AddNode<DecoderNode>(
            "video-decoder", executor, decode_node_options,
            std::make_unique<FFmpegDecoder>())) {
        PrintGraphError(graph, "add video decoder");
        return 1;
    }
    if (!graph.AddNode<DecoderNode>(
            "audio-decoder", executor, decode_node_options,
            std::make_unique<FFmpegDecoder>())) {
        PrintGraphError(graph, "add audio decoder");
        return 1;
    }
    if (!graph.AddNode<FrameCounterSink>(
            "video-counter", executor, decode_node_options, MediaType::VIDEO,
            "video-frame-counter")) {
        PrintGraphError(graph, "add video counter");
        return 1;
    }
    if (!graph.AddNode<FrameCounterSink>(
            "audio-counter", executor, decode_node_options, MediaType::AUDIO,
            "audio-frame-counter")) {
        PrintGraphError(graph, "add audio counter");
        return 1;
    }

    // Source 的 video/audio 输出和 Router 的 video-in/audio-in 分别形成两条
    // 独立入口边。Router 的两个输出端口继续对应两类媒体，每一路 Decoder
    // 只接收自身类型的压缩包，避免音频突发重新回到共享入口队列。
    if (!graph.Connect<MediaPacketMessage>(
            "source", "video", "router", "video-in",
            video_packet_edge_options) ||
        !graph.Connect<MediaPacketMessage>(
            "source", "audio", "router", "audio-in",
            audio_packet_edge_options) ||
        !graph.Connect<MediaPacketMessage>(
            "router", "video", "video-decoder", "in",
            video_packet_edge_options) ||
        !graph.Connect<MediaPacketMessage>(
            "router", "audio", "audio-decoder", "in",
            audio_packet_edge_options) ||
        !graph.Connect<MediaFrameMessage>(
            "video-decoder", "video-counter", video_frame_edge_options) ||
        !graph.Connect<MediaFrameMessage>(
            "audio-decoder", "audio-counter", audio_frame_edge_options)) {
        PrintGraphError(graph, "connect mediaflow graph");
        return 1;
    }

    if (!graph.Start()) {
        PrintGraphError(graph, "start mediaflow graph");
        return 1;
    }

    std::cout << "Stream started through MediaFlow: " << stream_url << "\n"
              << "Press Enter to stop...\n";

    // 停止阶段保留一条独立观测线程。它不参与 Graph 调度，只读取公开的
    // 原子状态和指标，便于区分 Source 读线程、Edge 排空还是节点任务回收卡住。
    std::atomic<bool> stop_observer_done{false};
    std::atomic<std::uint64_t> observer_sample_count{0};
    auto source_for_observer = graph.GetNode<StreamSourceNode>("source");
    auto video_counter_for_observer =
        graph.GetNode<FrameCounterSink>("video-counter");
    auto audio_counter_for_observer =
        graph.GetNode<FrameCounterSink>("audio-counter");
    const auto observer_start = std::chrono::steady_clock::now();
    std::thread stop_observer([&]() {
        auto next_sample = std::chrono::steady_clock::now() +
                           std::chrono::seconds(5);
        while (!stop_observer_done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (stop_observer_done.load()) break;
            if (std::chrono::steady_clock::now() < next_sample) {
                continue;
            }
            next_sample += std::chrono::seconds(5);

            NodeMetricsSnapshot observer_video_metrics;
            NodeMetricsSnapshot observer_audio_metrics;
            NodeMetricsSnapshot observer_router_metrics;
            NodeMetricsSnapshot observer_video_counter_metrics;
            NodeMetricsSnapshot observer_audio_counter_metrics;
            EdgeMetricsSnapshot observer_source_video_edge;
            EdgeMetricsSnapshot observer_source_audio_edge;
            EdgeMetricsSnapshot observer_video_edge;
            EdgeMetricsSnapshot observer_audio_edge;
            graph.GetMetrics("video-decoder", observer_video_metrics);
            graph.GetMetrics("audio-decoder", observer_audio_metrics);
            graph.GetMetrics("router", observer_router_metrics);
            graph.GetMetrics("video-counter", observer_video_counter_metrics);
            graph.GetMetrics("audio-counter", observer_audio_counter_metrics);
            graph.GetEdgeMetrics("source:video->router:video-in",
                                 observer_source_video_edge);
            graph.GetEdgeMetrics("source:audio->router:audio-in",
                                 observer_source_audio_edge);
            graph.GetEdgeMetrics("router:video->video-decoder:in",
                                 observer_video_edge);
            graph.GetEdgeMetrics("router:audio->audio-decoder:in",
                                 observer_audio_edge);
            const auto rss_bytes = CurrentProcessRssBytes();
            // FFmpegFrameBuffer 的存活统计和 RSS 分开输出：前者只反映仍被
            // MediaFrame 持有的解码帧，后者还会受到 Windows 堆和 FFmpeg 内部
            // 缓存的工作集保留影响。二者走势不同可缩小 RSS 跃升的排查范围。
            const auto frame_memory = FFmpegFrameBuffer::GetMemoryStats();
            const auto elapsed_seconds = std::chrono::duration_cast<
                std::chrono::seconds>(std::chrono::steady_clock::now() -
                                      observer_start).count();
            const auto dropped = observer_source_video_edge.dropped_newest +
                                 observer_source_video_edge.dropped_oldest +
                                 observer_source_audio_edge.dropped_newest +
                                 observer_source_audio_edge.dropped_oldest +
                                 observer_video_edge.dropped_newest +
                                 observer_video_edge.dropped_oldest +
                                 observer_audio_edge.dropped_newest +
                                 observer_audio_edge.dropped_oldest;
            const auto rejected = observer_source_video_edge.rejected +
                                  observer_source_audio_edge.rejected +
                                  observer_video_edge.rejected +
                                  observer_audio_edge.rejected;
            const auto timestamp_diagnostics =
                observer_source_video_edge.budget.timestamp_invalid +
                observer_source_video_edge.budget.timestamp_discontinuity +
                observer_source_audio_edge.budget.timestamp_invalid +
                observer_source_audio_edge.budget.timestamp_discontinuity +
                observer_video_edge.budget.timestamp_invalid +
                observer_video_edge.budget.timestamp_discontinuity +
                observer_audio_edge.budget.timestamp_invalid +
                observer_audio_edge.budget.timestamp_discontinuity;
            observer_sample_count.fetch_add(1, std::memory_order_relaxed);
            std::cerr << "[MediaFlow][Sample] elapsed_s=" << elapsed_seconds
                      << ", frames=(video="
                      << (video_counter_for_observer
                              ? video_counter_for_observer->Count()
                              : 0)
                      << ", audio="
                      << (audio_counter_for_observer
                              ? audio_counter_for_observer->Count()
                              : 0)
                      << "), rss_bytes=" << rss_bytes
                      << ", frame_memory=(wrappers="
                      << frame_memory.live_wrappers
                      << ", packed_bytes=" << frame_memory.live_packed_bytes
                      << ", av_buffer_bytes="
                      << frame_memory.live_av_buffer_bytes
                      << ", peak_wrappers=" << frame_memory.peak_wrappers
                      << ", peak_packed_bytes="
                      << frame_memory.peak_packed_bytes
                      << ", peak_av_buffer_bytes="
                      << frame_memory.peak_av_buffer_bytes << ")"
                      << ", source_finished="
                      << (source_for_observer && source_for_observer->Finished())
                      << ", source_generation="
                      << (source_for_observer ? source_for_observer->Generation() : 0)
                      << ", decoder_pending=("
                      << observer_video_metrics.pending_tasks << ","
                      << observer_audio_metrics.pending_tasks << ")"
                      << ", other_pending=(router="
                      << observer_router_metrics.pending_tasks
                      << ", video_counter="
                      << observer_video_counter_metrics.pending_tasks
                      << ", audio_counter="
                      << observer_audio_counter_metrics.pending_tasks << ")"
                      << ", edge_queue=(source_video="
                      << observer_source_video_edge.queue_size
                      << ", source_audio="
                      << observer_source_audio_edge.queue_size
                      << ", video_decoder=" << observer_video_edge.queue_size
                      << ", audio_decoder=" << observer_audio_edge.queue_size
                      << "), budget=(source_video="
                      << observer_source_video_edge.budget.items << "/"
                      << observer_source_video_edge.budget.bytes << "/"
                      << observer_source_video_edge.budget.span_us
                      << ", source_audio="
                      << observer_source_audio_edge.budget.items << "/"
                      << observer_source_audio_edge.budget.bytes << "/"
                      << observer_source_audio_edge.budget.span_us
                      << ", video_decoder="
                      << observer_video_edge.budget.items << "/"
                      << observer_video_edge.budget.bytes << "/"
                      << observer_video_edge.budget.span_us
                      << ", audio_decoder="
                      << observer_audio_edge.budget.items << "/"
                      << observer_audio_edge.budget.bytes << "/"
                      << observer_audio_edge.budget.span_us
                      << "), diagnostics=(dropped=" << dropped
                      << ", rejected=" << rejected
                      << ", timestamp=" << timestamp_diagnostics << ")\n";
        }
    });

    const auto run_start = observer_start;
    const auto cpu_start = CurrentProcessCpuTime();

    // 采样线程已经在实际拉流期间运行；持续时长结束后再进入 GracefulStop，
    // 这样日志同时覆盖稳定运行期和停止排空期。
    if (duration_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    } else {
        std::cin.get();
    }

    // 在进入停止屏障前记录稳定运行期的边界。吞吐只使用此刻已经处理完成的帧，
    // 避免把 GracefulStop 的 Flush 尾帧混入持续运行性能数据。
    const auto run_end = std::chrono::steady_clock::now();
    const auto cpu_end = CurrentProcessCpuTime();
    const auto pre_stop_video_frames = video_counter_for_observer
        ? static_cast<std::uint64_t>(video_counter_for_observer->Count())
        : 0;
    const auto pre_stop_audio_frames = audio_counter_for_observer
        ? static_cast<std::uint64_t>(audio_counter_for_observer->Count())
        : 0;

    // 交互式停止使用 GracefulStop：先停止 Puller 生产，再排空已经进入 Graph
    // 的包，并让两个 Decoder 输出内部残留帧。即使网络流没有发送 EOS，也能
    // 通过图级 Flush 结束一次完整的解码生命周期。
    const bool stopped = graph.GracefulStop(std::chrono::seconds(5));
    stop_observer_done.store(true);
    stop_observer.join();
    if (!stopped) {
        std::cerr << "MediaFlow graceful stop timed out or flush failed\n";
    }

    // GracefulStop 返回后，所有 Edge 和 Node pending task 都应已释放其消息引用。
    // 这里专门检查 FFmpegFrameBuffer 的存活统计，避免仅凭队列 items=0 就误判
    // 解码帧已经释放；RSS 是否下降则另由 Windows 工作集采样和专用工具判断。
    const auto frame_memory_after_stop = FFmpegFrameBuffer::GetMemoryStats();
    const bool frame_buffers_released =
        frame_memory_after_stop.live_wrappers == 0 &&
        frame_memory_after_stop.live_packed_bytes == 0 &&
        frame_memory_after_stop.live_av_buffer_bytes == 0;
    std::cout << "Frame buffer memory after stop: wrappers="
              << frame_memory_after_stop.live_wrappers
              << ", packed_bytes=" << frame_memory_after_stop.live_packed_bytes
              << ", av_buffer_bytes="
              << frame_memory_after_stop.live_av_buffer_bytes << "\n";

    auto video_counter = graph.GetNode<FrameCounterSink>("video-counter");
    auto audio_counter = graph.GetNode<FrameCounterSink>("audio-counter");
    auto video_decoder = graph.GetNode<DecoderNode>("video-decoder");
    auto audio_decoder = graph.GetNode<DecoderNode>("audio-decoder");
    auto source = graph.GetNode<StreamSourceNode>("source");
    NodeMetricsSnapshot video_metrics;
    NodeMetricsSnapshot audio_metrics;
    NodeMetricsSnapshot source_metrics;
    NodeMetricsSnapshot router_metrics;
    graph.GetMetrics("video-decoder", video_metrics);
    graph.GetMetrics("audio-decoder", audio_metrics);
    graph.GetMetrics("source", source_metrics);
    graph.GetMetrics("router", router_metrics);
    EdgeMetricsSnapshot source_video_edge_metrics;
    EdgeMetricsSnapshot source_audio_edge_metrics;
    EdgeMetricsSnapshot video_edge_metrics;
    EdgeMetricsSnapshot audio_edge_metrics;
    graph.GetEdgeMetrics("source:video->router:video-in",
                         source_video_edge_metrics);
    graph.GetEdgeMetrics("source:audio->router:audio-in",
                         source_audio_edge_metrics);
    graph.GetEdgeMetrics("router:video->video-decoder:in", video_edge_metrics);
    graph.GetEdgeMetrics("router:audio->audio-decoder:in", audio_edge_metrics);
    // 现场设备验证不仅要看最终帧数，还要保留 Decoder 的输入处理量和错误，
    // 这样可以区分“没有收到视频包”和“收到包但 FFmpeg 没有产出帧”。
    std::cout << "Decoder metrics: video(enqueued=" << video_metrics.enqueued
              << ", processed=" << video_metrics.processed
              << ", rejected=" << video_metrics.rejected << ")"
              << ", audio(enqueued=" << audio_metrics.enqueued
              << ", processed=" << audio_metrics.processed
              << ", rejected=" << audio_metrics.rejected << ")\n";
    std::cout << "Graph metrics: source(processed=" << source_metrics.processed
              << ", rejected=" << source_metrics.rejected
              << "), router(processed=" << router_metrics.processed
              << ", rejected=" << router_metrics.rejected << ")\n";
    std::cout << "Edge diagnostics:\n";
    PrintEdgeDiagnostics("source-video", source_video_edge_metrics);
    PrintEdgeDiagnostics("source-audio", source_audio_edge_metrics);
    PrintEdgeDiagnostics("router-video", video_edge_metrics);
    PrintEdgeDiagnostics("router-audio", audio_edge_metrics);
    if (source) {
        std::cout << "Source state: generation=" << source->Generation()
                  << ", finished=" << source->Finished() << "\n";
    }
    if (video_decoder && !video_decoder->LastError().empty()) {
        std::cerr << "Video decoder error: " << video_decoder->LastError() << "\n";
    }
    if (audio_decoder && !audio_decoder->LastError().empty()) {
        std::cerr << "Audio decoder error: " << audio_decoder->LastError() << "\n";
    }
    std::cout << "Total decoded through MediaFlow: video="
              << (video_counter ? video_counter->Count() : 0)
              << " frames, audio="
              << (audio_counter ? audio_counter->Count() : 0)
              << " frames\n";

    const auto run_wall_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<
        std::chrono::milliseconds>(run_end - run_start).count());
    const double cpu_percent = ProcessCpuPercent(cpu_start, cpu_end, run_wall_ms);
    const double run_seconds = static_cast<double>(run_wall_ms) / 1000.0;
    std::cout << std::fixed << std::setprecision(2)
              << "Performance metrics: run_wall_ms=" << run_wall_ms
              << ", process_cpu_100ns="
              << (cpu_end.valid && cpu_start.valid &&
                          cpu_end.total_100ns >= cpu_start.total_100ns
                      ? cpu_end.total_100ns - cpu_start.total_100ns
                      : 0)
              << ", cpu_percent_one_core=" << cpu_percent
              << ", observer_samples="
              << observer_sample_count.load(std::memory_order_relaxed)
              << ", throughput_fps=(video="
              << (run_seconds > 0.0
                      ? static_cast<double>(pre_stop_video_frames) / run_seconds
                      : 0.0)
              << ", audio="
              << (run_seconds > 0.0
                      ? static_cast<double>(pre_stop_audio_frames) / run_seconds
                      : 0.0)
              << ")\n";

    // 该程序验证的是音视频双轨解码，不应只因为停止屏障成功就把“设备没有
    // 发送媒体包”报告为通过。这样现场设备故障会被 CI/人工测试明确识别。
    const bool decoded_both_tracks =
        video_counter && video_counter->Count() > 0 &&
        audio_counter && audio_counter->Count() > 0;
    if (!decoded_both_tracks) {
        std::cerr << "MediaFlow stream decode did not receive both audio and video "
                     "frames\n";
    }
    return stopped && decoded_both_tracks && frame_buffers_released ? 0 : 1;
}
