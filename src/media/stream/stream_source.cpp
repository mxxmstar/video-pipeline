#include "media/stream/stream_source.h"

#include <chrono>
#include <cstddef>
#include <thread>

#include "common/log/logger.h"

// ── ctor / dtor ────────────────────────────────────────────────────

MediaStreamSource::MediaStreamSource(const std::string& stream_id)
    : stream_id_(stream_id) {
}

MediaStreamSource::~MediaStreamSource() {
    Stop();
}

// ── Session ─────────────────────────────────────────────────────────

void MediaStreamSource::SetSession(std::shared_ptr<MediaStreamSession> session) {
    session_ = std::move(session);
}

// ── 生命周期 ────────────────────────────────────────────────────────

bool MediaStreamSource::Start() {
    if (!session_) {
        LOG_ERROR("[{}] Start() rejected: session_ is null", stream_id_);
        return false;
    }

    // 应用配置到 session 和 puller
    ApplyConfig();

    // 桥接 session 回调 -> source 回调
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        session_->SetStreamInfoCallback(
            [self = shared_from_this()](const MultiStreamInfo& info) {
                self->OnStreamInfo(info);
            });
        session_->SetPacketCallback(
            [self = shared_from_this()](std::shared_ptr<MediaPacket> pkt) {
                self->OnPacket(std::move(pkt));
            });
        session_->SetStateCallback(
            [self = shared_from_this()](MediaStreamSession::State state) {
                self->OnSessionState(state);
            });
    }

    // 启动会话
    return session_->Start();
}

void MediaStreamSource::Stop() {
    if (session_)
        session_->Stop();
}

// ── 元数据 ─────────────────────────────────────────────────────────

MultiStreamInfo MediaStreamSource::GetStreamInfo() const {
    return stream_info_;
}

// ── 配置 ────────────────────────────────────────────────────────────

void MediaStreamSource::SetMediaStreamSourceConfig(const MediaStreamSourceConfig& config) {
    config_ = config;
}

// ── 订阅 ────────────────────────────────────────────────────────────

void MediaStreamSource::AddPacketSubscriber(PacketCallback cb) {
    if (!cb) return;
    std::lock_guard<std::mutex> lock(subscriber_mutex_);
    packet_subscribers_.push_back(std::move(cb));
}

// ── Session 回调桥接 ───────────────────────────────────────────────

void MediaStreamSource::OnStreamInfo(const MultiStreamInfo& info) {
    stream_info_ = info;
    LOG_INFO("[{}] StreamInfo received: video={}, audio={}", 
             stream_id_, info.HasVideoStream(), info.HasAudioStream());
}

void MediaStreamSource::OnPacket(std::shared_ptr<MediaPacket> packet) {
    if (!packet)
        return;

    // packet->Dump();
    // 更新统计
    if (packet->type == MediaType::VIDEO) {
        video_packet_count_.fetch_add(1, std::memory_order_relaxed);
        video_bytes_.fetch_add(packet->buffer->Size(), std::memory_order_relaxed);
    } else if (packet->type == MediaType::AUDIO) {
        audio_packet_count_.fetch_add(1, std::memory_order_relaxed);
        audio_bytes_.fetch_add(packet->buffer->Size(), std::memory_order_relaxed);
    }
    // 广播给所有订阅者
    std::lock_guard<std::mutex> lock(subscriber_mutex_);
    for (auto& cb : packet_subscribers_) {
        if (cb)
            cb(packet);  // 共享 ptr，所有订阅者共享同一对象
    }
}

void MediaStreamSource::OnSessionState(MediaStreamSession::State state) {
    LOG_INFO("[{}] Session state: {}", stream_id_, MediaStreamSession::StateName(state));
    // 预留：后续可在此处处理 decoder/pipeline 生命周期
}

// ── 内部 ────────────────────────────────────────────────────────────

void MediaStreamSource::ApplyConfig() {
    if (!session_)
        return;

    // ── session 层配置 ──
    session_->SetReconnectIntervalMs(config_.session.reconnect_interval_ms);
    session_->SetMaxReconnectCount(config_.session.max_reconnect_count);
    session_->SetWatchdogIntervalMs(config_.session.watchdog_interval_ms);
    session_->SetJitterBufferIntervalMs(config_.session.jitter_buffer_interval_ms);

    AdaptiveJitterBuffer::Config jitter_config;
    jitter_config.capacity_packets = config_.session.jitter_buffer_capacity_packets > 0
            ? static_cast<std::size_t>(config_.session.jitter_buffer_capacity_packets) : 1;
    jitter_config.min_delay_ms = config_.session.jitter_buffer_min_delay_ms;
    jitter_config.max_delay_ms = config_.session.jitter_buffer_max_delay_ms;
    jitter_config.safety_margin_ms = config_.session.jitter_buffer_safety_margin_ms;
    jitter_config.alpha = config_.session.jitter_buffer_alpha;
    session_->SetJitterBufferConfig(jitter_config);
    session_->ApplyPullerConfig(config_);

    // ── puller 层配置（需向下转型） ──
    // FFmpegPuller 提供了 SetXxx 扩展接口，可直接在这里调用
    // 示例（用户根据需要启用）：
    // if (auto* fp = dynamic_cast<FFmpegPuller*>(/*puller指针*/)) {
    //     fp->SetConnectTimeoutMs(config_.puller.io_timeout_ms);
    //     fp->SetLowLatency(config_.puller.low_latency);
    // }
    // 注意：puller 在 session_ 内部，这里无法直接访问。
    // 建议在创建 puller 时直接配置好再注入 session。
}

// ── 流统计打印 ─────────────────────────────────────────────────────

void MediaStreamSource::StartStatsPrint(int interval_seconds) {
    if (stats_print_running_.exchange(true)) {
        LOG_WARN("[{}] Stats print already running", stream_id_);
        return;
    }
    stats_print_thread_ = std::thread(&MediaStreamSource::printStatsLoop, this, interval_seconds);
}

void MediaStreamSource::StopStatsPrint() {
    if (stats_print_running_.exchange(false)) {
        if (stats_print_thread_.joinable()) {
            stats_print_thread_.join();
        }
    }
}

void MediaStreamSource::printStatsLoop(int interval_seconds) {
    while (stats_print_running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(interval_seconds));
        if (!stats_print_running_.load()) break;

        uint64_t v_packets = video_packet_count_.load();
        uint64_t a_packets = audio_packet_count_.load();
        uint64_t v_bytes = video_bytes_.load();
        uint64_t a_bytes = audio_bytes_.load();

        LOG_INFO("[{}] Stats: video={} pkts ({:.2f} MB), audio={} pkts ({:.2f} MB)",
                 stream_id_,
                 v_packets, v_bytes / (1024.0 * 1024.0),
                 a_packets, a_bytes / (1024.0 * 1024.0));
    }
}