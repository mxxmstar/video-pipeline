#include "media/stream/stream_session.h"

#include <boost/asio/post.hpp>
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

MediaStreamSession::MediaStreamSession(boost::asio::io_context& io)
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

    const State current_state = state_.load();
    if (current_state == State::KCONNECTING ||
        current_state == State::KCONNECTED ||
        current_state == State::KRECONNECTING) {
        LOG_WARN("Start() rejected: session is already active ({})",
                 StateNameImpl(current_state));
        return false;
    }

    // 每次 Start 都产生新的代次。旧代次的异步 handler 即使已经排队，
    // 也只能在入口处发现代次不匹配并退出。
    const std::uint64_t generation = generation_.fetch_add(1) + 1;
    running_ = true;
    setState(State::KCONNECTING);

    // 打开拉流器（同步）
    if (!puller_->Open(url_)) {
        if (isGenerationActive(generation)) {
            running_ = false;
            setState(State::KERROR);
        }
        return false;
    }

    if (!isGenerationActive(generation)) {
        // Stop 可能在 Open 阻塞期间被调用，不能把已打开的连接泄漏给旧代次。
        puller_->Close();
        return false;
    }

    // 获取并分发 StreamInfo
    MultiStreamInfo info = puller_->GetStreamInfo();
    StreamInfoCallback streaminfo_cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        streaminfo_cb = streaminfo_cb_;
    }
    if (streaminfo_cb) {
        streaminfo_cb(info);
    }

    // 标记运行状态
    running_ = true;
    last_read_time_ = std::chrono::steady_clock::now();
    reconnect_count_ = 0;
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_ = {};
        stats_window_start_ = std::chrono::steady_clock::now();
        stats_window_bytes_ = 0;
    }
    clearJitterBuffer();

    setState(State::KCONNECTED);

    // 通过 post 调度读循环到 io_context（由外部线程池驱动）
    boost::asio::post(io_, [self = shared_from_this(), generation]() {
        self->readLoop(generation);
    });

    // 启动 Watchdog
    if (watchdog_interval_ms_ > 0) {
        startWatchdog(generation);
    }

    if (jitter_buffer_interval_ms_ > 0) {
        startDecoderDriveTimer(generation);
    }

    return true;
}

void MediaStreamSession::Stop() {
    State expected = state_.load();
    // CONNECTING 也必须可停止，否则 Open 阶段无法取消，调用方会永久卡在
    // “连接中”。KIDLE/KSTOPPED 已经没有需要取消的异步工作。
    if (expected == State::KIDLE || expected == State::KSTOPPED) {
        return;
    }

    // 先使所有旧 handler 失效，再取消 timer 和关闭底层 I/O。
    generation_.fetch_add(1);
    running_ = false;

    // 取消定时器
    boost::system::error_code ec;
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

void MediaStreamSession::connect(std::uint64_t generation) {
    if (!isGenerationActive(generation)) {
        return;
    }

    if (!puller_->Open(url_)) {
        LOG_ERROR("Reconnect Open failed");
        doReconnect(generation);
        return;
    }

    if (!isGenerationActive(generation)) {
        puller_->Close();
        return;
    }

    // 分发 StreamInfo（重连后可能变化）
    MultiStreamInfo info = puller_->GetStreamInfo();
    StreamInfoCallback streaminfo_cb;
    {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        streaminfo_cb = streaminfo_cb_;
    }
    if (streaminfo_cb) {
        streaminfo_cb(info);
    }

    reconnect_count_ = 0;
    last_read_time_ = std::chrono::steady_clock::now();
    clearJitterBuffer();
    setState(State::KCONNECTED);

    // 重新 post 读循环（io_context 线程池仍在运行）。running_ 在重连期间
    // 一直保持为 true，但代次检查仍然保留，防止旧 timer 重新启动读循环。
    boost::asio::post(io_, [self = shared_from_this(), generation]() {
        self->readLoop(generation);
    });

    // 重启 Watchdog
    if (watchdog_interval_ms_ > 0) {
        boost::system::error_code ec;
        watchdog_timer_.cancel();
        startWatchdog(generation);
    }
    if (jitter_buffer_interval_ms_ > 0) {
        startDecoderDriveTimer(generation);
    }
}

// ── 内部：读循环（通过 io_context::post 调度，单次执行） ───────

void MediaStreamSession::readLoop(std::uint64_t generation) {
    // 每次 handler 入口先检查是否应继续
    if (!isGenerationActive(generation)) {
        return;
    }

    // 读一个包（可能阻塞，故在 io_context 多线程环境下需保证其它线程可处理定时器）
    std::shared_ptr<MediaPacket> packet;
    const IPuller::PullReadResult result = puller_->ReadPacketResult();

    if (!isGenerationActive(generation)) {
        return;
    }

    if (result.status == IPuller::PullReadStatus::Packet) {
        packet = result.packet;
        // 空 packet = 非目标流跳过，继续下一轮
        if (!packet) {
            boost::asio::post(io_, [self = shared_from_this(), generation]() {
                self->readLoop(generation);
            });
            return;
        }

        // 更新累计统计和码率窗口。统计只在这里写入，查询通过 mutex 获取
        // 快照，避免外部监控线程与读循环并发访问普通字段。
        const std::uint64_t bytes = packet->buffer ? packet->buffer->Size() : 0;
        {
            std::lock_guard<std::mutex> lock(stats_mutex_);
            ++stats_.packets_received;
            stats_.bytes_received += bytes;
            stats_window_bytes_ += bytes;
            const auto now = std::chrono::steady_clock::now();
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - stats_window_start_).count();
            if (elapsed_ms >= 1000) {
                stats_.bitrate = static_cast<double>(stats_window_bytes_) * 8.0 /
                                 static_cast<double>(elapsed_ms);
                stats_window_start_ = now;
                stats_window_bytes_ = 0;
            }
        }
        last_read_time_ = std::chrono::steady_clock::now();

        if (jitter_buffer_interval_ms_ > 0)
            enqueuePacket(std::move(packet), generation);
        else
            dispatchPacket(std::move(packet), generation);

        // 继续下一轮
        boost::asio::post(io_, [self = shared_from_this(), generation]() {
            self->readLoop(generation);
        });
        return;
    }

    if (result.status == IPuller::PullReadStatus::NoData) {
        // NoData 不是故障，不增加重连次数；下一轮继续读取即可。
        boost::asio::post(io_, [self = shared_from_this(), generation]() {
            self->readLoop(generation);
        });
        return;
    }

    switch (result.status) {
        case IPuller::PullReadStatus::RetryableError:
            LOG_WARN("ReadLoop: retryable read error: {}",
                     result.message.empty() ? "unknown" : result.message);
            doReconnect(generation);
            return;
        case IPuller::PullReadStatus::EOS:
            // 本地文件或其他有限输入正常结束，不应被当作网络故障重连。
            running_ = false;
            puller_->Close();
            clearJitterBuffer();
            setState(State::KSTOPPED);
            return;
        case IPuller::PullReadStatus::Stopped:
            running_ = false;
            setState(State::KSTOPPED);
            return;
        case IPuller::PullReadStatus::FatalError:
            LOG_ERROR("ReadLoop: fatal read error: {}",
                      result.message.empty() ? "unknown" : result.message);
            running_ = false;
            puller_->Close();
            setState(State::KERROR);
            return;
        case IPuller::PullReadStatus::Packet:
        case IPuller::PullReadStatus::NoData:
            // 上方已处理，保留分支使编译器覆盖所有枚举值。
            return;
    }
}

void MediaStreamSession::startDecoderDriveTimer(std::uint64_t generation) {
    if (!isGenerationActive(generation) || jitter_buffer_interval_ms_ <= 0)
        return;

    decoder_timer_.expires_after(std::chrono::milliseconds(jitter_buffer_interval_ms_));
    decoder_timer_.async_wait(
        [self = shared_from_this(), generation](boost::system::error_code ec) {
            self->onDecoderDriveTimer(ec, generation);
        });
}

void MediaStreamSession::onDecoderDriveTimer(
    const boost::system::error_code& ec,
    std::uint64_t generation) {
    if (ec || !isGenerationActive(generation) || jitter_buffer_interval_ms_ <= 0)
        return;

    if (state_.load() != State::KCONNECTED)
        return;

    std::shared_ptr<MediaPacket> packet;
    if (jitter_buffer_)
        packet = jitter_buffer_->PopReady();

    if (packet)
        dispatchPacket(std::move(packet), generation);

    startDecoderDriveTimer(generation);
}

void MediaStreamSession::enqueuePacket(std::shared_ptr<MediaPacket> packet,
                                       std::uint64_t generation) {
    if (!packet || !isGenerationActive(generation))
        return;

    if (jitter_buffer_ && !jitter_buffer_->Push(std::move(packet))) {
        LOG_DEBUG("Jitter buffer full, dropping incoming packet");
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.jitter_dropped_packets;
    }
}

void MediaStreamSession::dispatchPacket(std::shared_ptr<MediaPacket> packet,
                                        std::uint64_t generation) {
    if (!packet || !isGenerationActive(generation)) {
        return;
    }

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

void MediaStreamSession::doReconnect(std::uint64_t generation) {
    if (!isGenerationActive(generation))
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
        running_ = false;
        setState(State::KERROR);
        return;
    }

    reconnect_count_++;
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        ++stats_.reconnect_count;
    }

    LOG_INFO("Reconnect attempt {} in {} ms", reconnect_count_, reconnect_interval_ms_);

    // 异步等待后重试
    reconnect_timer_.expires_after(std::chrono::milliseconds(reconnect_interval_ms_));
    reconnect_timer_.async_wait(
        [self = shared_from_this(), generation](boost::system::error_code ec) {
            if (ec || !self->isGenerationActive(generation))
                return;
            self->connect(generation);
        });
}

// ── 内部：Watchdog ──────────────────────────────────────────────────

void MediaStreamSession::startWatchdog(std::uint64_t generation) {
    if (!isGenerationActive(generation) || watchdog_interval_ms_ <= 0) {
        return;
    }
    watchdog_timer_.expires_after(std::chrono::milliseconds(watchdog_interval_ms_));
    watchdog_timer_.async_wait(
        [self = shared_from_this(), generation](boost::system::error_code ec) {
            self->onWatchdog(ec, generation);
        });
}

void MediaStreamSession::onWatchdog(const boost::system::error_code& ec,
                                    std::uint64_t generation) {
    if (ec || !isGenerationActive(generation))
        return;

    auto now     = std::chrono::steady_clock::now();
    auto idle_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_read_time_).count();

    if (idle_ms > watchdog_interval_ms_) {
        LOG_WARN("Watchdog timeout: idle={}ms > interval={}ms", idle_ms, watchdog_interval_ms_);
        // Watchdog 超时触发重连
        doReconnect(generation);
        return;
    }

    // 继续下一轮检测
    startWatchdog(generation);
}

// ── 统计 ────────────────────────────────────────────────────────────

MediaStreamSession::Stats MediaStreamSession::GetStats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    Stats snapshot = stats_;
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - stats_window_start_).count();
    if (elapsed_ms > 0 && stats_window_bytes_ > 0) {
        // 1 bit/ms 等于 1 kbps，因此无需再乘除额外的 1000。
        snapshot.bitrate = static_cast<double>(stats_window_bytes_) * 8.0 /
                           static_cast<double>(elapsed_ms);
    }
    return snapshot;
}

// ── 状态 ────────────────────────────────────────────────────────────

MediaStreamSession::State MediaStreamSession::GetState() const {
    return state_.load();
}

bool MediaStreamSession::isGenerationActive(std::uint64_t generation) const {
    return running_.load() && generation_.load() == generation;
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
