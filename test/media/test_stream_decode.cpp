/// @file test_stream_decode.cpp
/// @brief 使用 MediaFlow 运行 RTSP 音视频解码链路的交互式验证程序。

#include "mediaflow/mediaflow.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "media/decoder/ffmpeg_decoder.h"
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

} // namespace

int main(int argc, char* argv[]) {
    std::cout << "=== MediaFlow RTSP Stream Decode ===\n";

    int duration_ms = 0;
    if (argc == 3 && std::string(argv[1]) == "--duration-ms") {
        try {
            duration_ms = std::max(0, std::stoi(argv[2]));
        } catch (...) {
            std::cerr << "Invalid --duration-ms value\n";
            return 1;
        }
    }

    // 连接参数仍由测试程序集中设置，SourceNode 只负责把 Puller 的读取结果
    // 转换成 MediaPacketMessage，不在业务节点中重新实现 FFmpeg I/O。
    constexpr const char* kUrl = "rtsp://192.168.66.83/live/mainstream";
    auto puller = std::make_unique<FFmpegPuller>();
    // 当前 URL 按已有摄像头发布和渲染测试的成功配置运行：RTSP 控制与媒体
    // 使用 TCP，开启低延迟模式，避免 MediaFlow 测试与已验证链路使用不同
    // 的 FFmpeg 会话参数。
    puller->SetConnectTimeoutMs(5000);
    puller->SetReadTimeoutMs(5000);
    puller->SetRtspTransport("tcp");
    puller->SetRtspAutoSwitchToTcp(false);
    puller->SetLowLatency(true);

    auto executor = std::make_shared<AsioExecutor>("stream-decode", 2);

    // 真实 RTSP 拉流器可能一次性读出网络接收缓冲区中的一段历史数据，
    // 因此 Source 在启动初期会短暂快于 Decoder。默认的 64 条队列会让
    // 音频包先占满队列，随后视频包被 DropNewest 丢弃，无法反映旧手工
    // 测试的实际解码能力。这里为验证图保留更大的有界队列，同时仍使用
    // DropNewest 保持包的时间顺序，避免无界缓存掩盖下游处理能力问题。
    NodeOptions decode_node_options{
        NodeExecutionMode::Serialized,
        4096};
    decode_node_options.prefer_video_keyframes = true;
    const EdgeOptions packet_edge_options{
        TransportKind::Queue,
        4096,
        BackpressurePolicy::PreferVideoKeyframes,
        64,
        0};
    const EdgeOptions frame_edge_options{
        TransportKind::Queue,
        1024,
        BackpressurePolicy::DropNewest,
        64,
        0};
    Graph graph;

    // Graph 会先启动 Router、Decoder 和统计 Sink，最后才启动 Source 并打开
    // RTSP。这样第一条音频或视频包到来时，下游端口已经全部注册完成。
    if (!graph.AddNode<StreamSourceNode>(
            "source", executor, decode_node_options, "test-stream",
            std::move(puller), kUrl, StreamSourceNodeOptions{100, -1})) {
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

    // Router 的两个输出端口分别对应两类媒体。每一路 Decoder 只接收自身
    // 类型的压缩包，避免旧测试中按 packet type 在 subscriber 回调里分流。
    if (!graph.Connect<MediaPacketMessage>(
            "source", "router", packet_edge_options) ||
        !graph.Connect<MediaPacketMessage>(
            "router", "video", "video-decoder", "in", packet_edge_options) ||
        !graph.Connect<MediaPacketMessage>(
            "router", "audio", "audio-decoder", "in", packet_edge_options) ||
        !graph.Connect<MediaFrameMessage>(
            "video-decoder", "video-counter", frame_edge_options) ||
        !graph.Connect<MediaFrameMessage>(
            "audio-decoder", "audio-counter", frame_edge_options)) {
        PrintGraphError(graph, "connect mediaflow graph");
        return 1;
    }

    if (!graph.Start()) {
        PrintGraphError(graph, "start mediaflow graph");
        return 1;
    }

    std::cout << "Stream started through MediaFlow: " << kUrl << "\n"
              << "Press Enter to stop...\n";
    if (duration_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));
    } else {
        std::cin.get();
    }

    // 停止阶段保留一条独立观测线程。它不参与 Graph 调度，只读取公开的
    // 原子状态和指标，便于区分 Source 读线程、Edge 排空还是节点任务回收卡住。
    std::atomic<bool> stop_observer_done{false};
    std::atomic<bool> stop_observer_active{true};
    auto source_for_observer = graph.GetNode<StreamSourceNode>("source");
    std::thread stop_observer([&]() {
        while (!stop_observer_done.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!stop_observer_active.load()) {
                continue;
            }

            NodeMetricsSnapshot observer_video_metrics;
            NodeMetricsSnapshot observer_audio_metrics;
            NodeMetricsSnapshot observer_router_metrics;
            NodeMetricsSnapshot observer_video_counter_metrics;
            NodeMetricsSnapshot observer_audio_counter_metrics;
            EdgeMetricsSnapshot observer_source_edge;
            EdgeMetricsSnapshot observer_video_edge;
            EdgeMetricsSnapshot observer_audio_edge;
            graph.GetMetrics("video-decoder", observer_video_metrics);
            graph.GetMetrics("audio-decoder", observer_audio_metrics);
            graph.GetMetrics("router", observer_router_metrics);
            graph.GetMetrics("video-counter", observer_video_counter_metrics);
            graph.GetMetrics("audio-counter", observer_audio_counter_metrics);
            graph.GetEdgeMetrics("source:out->router:in", observer_source_edge);
            graph.GetEdgeMetrics("router:video->video-decoder:in",
                                 observer_video_edge);
            graph.GetEdgeMetrics("router:audio->audio-decoder:in",
                                 observer_audio_edge);
            std::cerr << "[MediaFlow][StopObserver] source_finished="
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
                      << ", edge_queue=("
                      << observer_source_edge.queue_size << ","
                      << observer_video_edge.queue_size << ","
                      << observer_audio_edge.queue_size << ")\n";
        }
    });

    // 交互式停止使用 GracefulStop：先停止 Puller 生产，再排空已经进入 Graph
    // 的包，并让两个 Decoder 输出内部残留帧。即使网络流没有发送 EOS，也能
    // 通过图级 Flush 结束一次完整的解码生命周期。
    const bool stopped = graph.GracefulStop(std::chrono::seconds(5));
    stop_observer_active.store(false);
    stop_observer_done.store(true);
    stop_observer.join();
    if (!stopped) {
        std::cerr << "MediaFlow graceful stop timed out or flush failed\n";
    }

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
    EdgeMetricsSnapshot source_edge_metrics;
    EdgeMetricsSnapshot video_edge_metrics;
    EdgeMetricsSnapshot audio_edge_metrics;
    graph.GetEdgeMetrics("source:out->router:in", source_edge_metrics);
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
              << ", rejected=" << router_metrics.rejected
              << "), edges(source=" << source_edge_metrics.accepted
              << "/" << source_edge_metrics.dropped_newest
              << ", video=" << video_edge_metrics.accepted
              << "/" << video_edge_metrics.dropped_newest
              << ", audio=" << audio_edge_metrics.accepted
              << "/" << audio_edge_metrics.dropped_newest << ")\n";
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

    // 该程序验证的是音视频双轨解码，不应只因为停止屏障成功就把“设备没有
    // 发送媒体包”报告为通过。这样现场设备故障会被 CI/人工测试明确识别。
    const bool decoded_both_tracks =
        video_counter && video_counter->Count() > 0 &&
        audio_counter && audio_counter->Count() > 0;
    if (!decoded_both_tracks) {
        std::cerr << "MediaFlow stream decode did not receive both audio and video "
                     "frames\n";
    }
    return stopped && decoded_both_tracks ? 0 : 1;
}
