#include "media/stream/stream_session.h"

#include <asio/post.hpp>
#include "common/log/logger.h"

// ── 辅助 ───────────────────────────────────────────────────────────

inline static const char* StateNameImpl(MediaStreamSession::State s) {
    switch (s) {
        case MediaStreamSession::State::KIDLE:         return "KIDLE";
        case MediaStreamSession::State::KCONNECTING:   return "KCONNECTING";
        case MediaStreamSession::State::KCONNECTED:    return "KCONNECTED";
        case MediaStreamSession::State::KRECONNECTING: return "KRECONNECTING";
        case MediaStreamSession::State::KSTOPPED:      return "KSTOPPED";
        case MediaStreamSession::State::KERROR:        return "KERROR";
        default:                                 return "UNKNOWN";
    }
}

// ── ctor / dtor ────────────────────────────────────────────────────

MediaStreamSession::MediaStreamSession(asio::io_context& io)
    : io_(io)
    , reconnect_timer_(io)
    , watchdog_timer_(io)
    , decoder_timer_(io)
    , jitter_buffer_(std::make_unique<AdaptiveJitterBuffer>()) {
}

MediaStreamSession::~MediaStreamSession() {
    Stop();
}

// ── 配置 ────────────────────────────────────────────────────────────

void MediaStreamSession::SetPuller(std::unique_ptr<IPuller> puller) {
    puller_ = std::move(puller);
}

void MediaStreamSession::SetUrl(const std::string& url) {
    url_ = url;
}

void MediaStreamSession::SetReconnectIntervalMs(int ms) {
    reconnect_interval_ms_ = ms;
}

void MediaStreamSession::SetMaxReconnectCount(int count) {
    max_reconnect_count_ = count;
}

void MediaStreamSession::SetJitterBufferIntervalMs(int ms) {
    jitter_buffer_interval_ms_ = ms > 0 ? ms : 0;
}

void MediaStreamSession::SetJitterBufferConfig(const AdaptiveJitterBuffer::Config& config) {
    jitter_buffer_ = std::make_unique<AdaptiveJitterBuffer>(config);
}

void MediaStreamSession::ApplyPullerConfig(const MediaStreamSourceConfig& config) {
    if (!puller_) {
        return;
    }
    puller_->SetConnectTimeoutMs(config.session.connect_timeout_ms);
    puller_->SetReadTimeoutMs(config.session.read_timeout_ms);
    puller_->SetLowLatency(config.puller.low_latency);
    puller_->SetCredentials(config.puller.username, config.puller.password);
    puller_->SetRtspTransport(config.puller.rtsp_transport);
    puller_->SetRtspAutoSwitchToTcp(config.puller.rtsp_auto_switch_tcp);
    puller_->SetRtspAutoSwitchTimeoutMs(config.puller.rtsp_auto_switch_timeout_ms);
}

void MediaStreamSession::SetWatchdogIntervalMs(int ms) {
    watchdog_interval_ms_ = ms;
}

// ── 回调 setter ─────────────────────────────────────────────────────

void MediaStreamSession::SetPacketCallback(PacketCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    packet_cb_ = std::move(cb);
}

void MediaStreamSession::SetStreamInfoCallback(StreamInfoCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    streaminfo_cb_ = std::move(cb);
}

void MediaStreamSession::SetStateCallback(StateCallback cb) {
    std::lock_guard<std::mutex> lock(cb_mutex_);
    state_cb_ = std::move(cb);
}

// ── 生命周期 ────────────────────────────────────────────────────────

bool MediaStreamSession::Start() {
    if (!puller_) {
        LOG_ERROR("Start() rejected: puller_ is null");
        return false;
    }
    if (url_.empty()) {
        LOG_ERROR("Start() rejected: url is empty");
        return false;
    }

    setState(State::KCONNECTING);

    // 打开拉流器（同步）
    if (!puller_->Open(url_)) {
        setState(State::KERROR);
        return false;
    }

    // 获取并分发 StreamInfo
    MultiStreamInfo info = puller_->GetStreamInfo();
    if (streaminfo_cb_) {
        streaminfo_cb_(info);
    }        

    // 标记运行状态
    running_ = true;
    last_read_time_ = std::chrono::steady_clock::now();
    reconnect_count_ = 0;
    stats_ = {};
    async_bytes_received_ = 0;
    async_packets_received_ = 0;
    clearJitterBuffer();

    setState(State::KCONNECTED);

    // 通过 post 调度读循环到 io_context（由外部线程池驱动）
    asio::post(io_, [self = shared_from_this()]() { self->readLoop(); });

    // 启动 Watchdog
    if (watchdog_interval_ms_ > 0) {
        startWatchdog();
    }

    if (jitter_buffer_interval_ms_ > 0) {
        startDecoderDriveTimer();
    }

    return true;
}

void MediaStreamSession::Stop() {
    State expected = state_.load();
    // 只有 CONNECTED / RECONNECTING / ERROR 需要真正停止
    if (expected != State::KCONNECTED && expected != State::KRECONNECTING && expected != State::KERROR) {        
        return;
    }

    // 设置停止标志
    running_ = false;

    // 取消定时器
    asio::error_code ec;
    reconnect_timer_.cancel();
    watchdog_timer_.cancel();
    decoder_timer_.cancel();
    clearJitterBuffer();

    // 关闭拉流器（这会同时中断阻塞中的 av_read_frame，使读循环退出）
    if (puller_) {
        puller_->Close();
    }

    setState(State::KSTOPPED);
}

// ── 内部：连接（同步） ──────────────────────────────────────────────

void MediaStreamSession::connect() {
    if (!puller_->Open(url_)) {
        LOG_ERROR("Reconnect Open failed");
        doReconnect();
        return;
    }

    // 分发 StreamInfo（重连后可能变化）
    MultiStreamInfo info = puller_->GetStreamInfo();
    if (streaminfo_cb_) {
        streaminfo_cb_(info);
    }

    reconnect_count_ = 0;
    last_read_time_ = std::chrono::steady_clock::now();
    clearJitterBuffer();
    setState(State::KCONNECTED);

    // 重新 post 读循环（io_context 线程池仍在运行）
    running_ = true;
    asio::post(io_, [self = shared_from_this()]() { self->readLoop(); });

    // 重启 Watchdog
    if (watchdog_interval_ms_ > 0) {
        asio::error_code ec;
        watchdog_timer_.cancel();
        startWatchdog();
    }
    if (jitter_buffer_interval_ms_ > 0) {
        startDecoderDriveTimer();
    }        
}

// ── 内部：读循环（通过 io_context::post 调度，单次执行） ───────

void MediaStreamSession::readLoop() {
    // 每次 handler 入口先检查是否应继续
    if (!running_) {
        return;
    }

    // 读一个包（可能阻塞，故在 io_context 多线程环境下需保证其它线程可处理定时器）
    std::shared_ptr<MediaPacket> packet;
    bool ok = puller_->ReadPacket(packet);

    if (!running_) {
        return;
    }

    if (ok) {
        // 空 packet = 非目标流跳过，继续下一轮
        if (!packet) {
            asio::post(io_, [self = shared_from_this()]() { self->readLoop(); });
            return;
        }

        // 更新统计
        async_bytes_received_  += packet->buffer ? packet->buffer->Size() : 0;
        async_packets_received_++;
        last_read_time_ = std::chrono::steady_clock::now();

        if (jitter_buffer_interval_ms_ > 0)
            enqueuePacket(std::move(packet));
        else
            dispatchPacket(std::move(packet));

        // 继续下一轮
        asio::post(io_, [self = shared_from_this()]() { self->readLoop(); });

    } else {
        // 读取失败：EOF 或错误
        LOG_WARN("ReadLoop: ReadPacket failed, reconnecting...");
        doReconnect();
    }
}

void MediaStreamSession::startDecoderDriveTimer() {
    if (!running_ || jitter_buffer_interval_ms_ <= 0)
        return;

    decoder_timer_.expires_after(std::chrono::milliseconds(jitter_buffer_interval_ms_));
    decoder_timer_.async_wait(
        [self = shared_from_this()](asio::error_code ec) {
            self->onDecoderDriveTimer(ec);
        });
}

void MediaStreamSession::onDecoderDriveTimer(const asio::error_code& ec) {
    if (ec || !running_ || jitter_buffer_interval_ms_ <= 0)
        return;

    if (state_.load() != State::KCONNECTED)
        return;

    std::shared_ptr<MediaPacket> packet;
    if (jitter_buffer_)
        packet = jitter_buffer_->PopReady();

    if (packet)
        dispatchPacket(std::move(packet));

    startDecoderDriveTimer();
}

void MediaStreamSession::enqueuePacket(std::shared_ptr<MediaPacket> packet) {
    if (!packet)
        return;

    if (jitter_buffer_ && !jitter_buffer_->Push(std::move(packet))) {
        LOG_DEBUG("Jitter buffer full, dropping incoming packet");
    }        
}

void MediaStreamSession::dispatchPacket(std::shared_ptr<MediaPacket> packet) {
    PacketCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        cb = packet_cb_;
    }
    if (cb) {
        cb(std::move(packet));
    }
}

void MediaStreamSession::clearJitterBuffer() {
    if (jitter_buffer_)
        jitter_buffer_->Reset();
}

// ── 内部：异步重连 ────────────────────────────────────────────────

void MediaStreamSession::doReconnect() {
    if (!running_)
        return;

    setState(State::KRECONNECTING);
    decoder_timer_.cancel();
    clearJitterBuffer();

    // 关闭旧连接
    if (puller_) {
        puller_->Close();
    }

    // 检查重连上限
    if (max_reconnect_count_ >= 0 && reconnect_count_ >= max_reconnect_count_) {
        LOG_ERROR("Max reconnect reached ({})", reconnect_count_);
        setState(State::KERROR);
        return;
    }

    reconnect_count_++;

    LOG_INFO("Reconnect attempt {} in {} ms", reconnect_count_, reconnect_interval_ms_);

    // 异步等待后重试
    reconnect_timer_.expires_after(std::chrono::milliseconds(reconnect_interval_ms_));
    reconnect_timer_.async_wait(
        [self = shared_from_this()](asio::error_code ec) {
            if (ec || !self->running_)
                return;
            self->connect();
        });
}

// ── 内部：Watchdog ──────────────────────────────────────────────────

void MediaStreamSession::startWatchdog() {
    watchdog_timer_.expires_after(std::chrono::milliseconds(watchdog_interval_ms_));
    watchdog_timer_.async_wait(
        [self = shared_from_this()](asio::error_code ec) {
            self->onWatchdog(ec);
        });
}

void MediaStreamSession::onWatchdog(const asio::error_code& ec) {
    if (ec || !running_)
        return;

    auto now     = std::chrono::steady_clock::now();
    auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_read_time_).count();

    if (idle_ms > watchdog_interval_ms_) {
        LOG_WARN("Watchdog timeout: idle={}ms > interval={}ms", idle_ms, watchdog_interval_ms_);
        // Watchdog 超时触发重连
        doReconnect();
        return;
    }

    // 继续下一轮检测
    startWatchdog();
}

// ── 统计 ────────────────────────────────────────────────────────────

MediaStreamSession::Stats MediaStreamSession::GetStats() const {
    return stats_;
}

// ── 状态 ────────────────────────────────────────────────────────────

MediaStreamSession::State MediaStreamSession::GetState() const {
    return state_.load();
}

void MediaStreamSession::setState(State s) {
    State old = state_.exchange(s);
    if (old == s)
        return;

    LOG_INFO("State: {} -> {}", StateNameImpl(old), StateNameImpl(s));

    // 通知状态回调
    StateCallback cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        cb = state_cb_;
    }
    if (cb)
        cb(s);
}

const char* MediaStreamSession::StateName(State s) {
    return StateNameImpl(s);
}