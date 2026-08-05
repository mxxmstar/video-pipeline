#include "mediaflow/media_nodes.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace mediaflow {

namespace {

/// 将包上的最小元数据补成一个可供 Decoder 使用的 StreamInfo。
MediaStreamInfo MakeFallbackStreamInfo(const MediaPacket& packet) {
    MediaStreamInfo info;
    info.media_type = packet.type;
    info.codec_type = packet.codec;
    info.stream_index = packet.stream_index;
    info.time_base = packet.time_base;
    if (packet.type == MediaType::VIDEO) {
        info.detail = VideoStreamInfo{};
    } else {
        info.detail = AudioStreamInfo{};
    }
    return info;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// StreamSourceNode
// ─────────────────────────────────────────────────────────────────────────────

StreamSourceNode::StreamSourceNode(std::string stream_id,
                                   std::unique_ptr<IPuller> puller,
                                   std::string url,
                                   StreamSourceNodeOptions options)
    : stream_id_(std::move(stream_id)),
      puller_(std::move(puller)),
      url_(std::move(url)),
      options_(options) {}

StreamSourceNode::~StreamSourceNode() {
    Stop();
}

bool StreamSourceNode::Init() {
    // Init 只做静态校验，不打开网络。真正的 Open 放在 Source 最后启动时，
    // 此时 Graph 已经完成所有下游节点的 Init/Start。
    if (!puller_ || url_.empty()) {
        return false;
    }
    return true;
}

bool StreamSourceNode::Start() {
    if (!puller_ || url_.empty() || read_thread_.joinable()) {
        return false;
    }

    stop_requested_.store(false);
    finished_.store(false);
    const std::uint64_t generation = generation_.fetch_add(1) + 1;
    if (!OpenGeneration(generation)) {
        stop_requested_.store(true);
        finished_.store(true);
        return false;
    }

    // 读操作可能阻塞在网络 I/O，因此不能占用 Graph 的业务 Executor。
    read_thread_ = std::thread([this, generation]() {
        ReadLoop(generation);
    });
    return true;
}

void StreamSourceNode::Stop() {
    // 先让读循环的代次检查失效，再关闭 puller。FFmpegPuller 的 Close 会
    // 唤醒正在 av_read_frame 中等待的线程，其他 Puller 也应遵守相同契约。
    stop_requested_.store(true);
    generation_.fetch_add(1);
    if (puller_) {
        puller_->Close();
    }

    if (read_thread_.joinable() &&
        read_thread_.get_id() != std::this_thread::get_id()) {
        read_thread_.join();
    }
    finished_.store(true);
}

void StreamSourceNode::Deinit() {
    Stop();
}

std::string StreamSourceNode::Name() const {
    return "stream-source:" + stream_id_;
}

MultiStreamInfo StreamSourceNode::StreamInfo() const {
    std::lock_guard<std::mutex> lock(info_mutex_);
    return stream_info_;
}

std::uint64_t StreamSourceNode::Generation() const {
    return generation_.load();
}

bool StreamSourceNode::Finished() const {
    return finished_.load();
}

void StreamSourceNode::ReadLoop(std::uint64_t generation) {
    int reconnect_count = 0;
    std::uint64_t current_generation = generation;

    while (!stop_requested_.load() && generation_.load() == current_generation) {
        const IPuller::PullReadResult result = puller_->ReadPacketResult();
        if (stop_requested_.load() || generation_.load() != current_generation) {
            break;
        }

        switch (result.status) {
            case IPuller::PullReadStatus::Packet: {
                if (!result.packet) {
                    // Puller 契约要求 Packet 必须带非空 packet。这里把违规
                    // 返回当作一次可恢复读错误，避免向下游发送无效消息。
                    continue;
                }

                auto message = MediaPacketMessage{};
                message.packet = result.packet;
                message.stream_info = FindStreamInfo(result.packet);
                message.generation = current_generation;
                Emit(std::move(message));
                reconnect_count = 0;
                break;
            }

            case IPuller::PullReadStatus::NoData:
                // NoData 不是 EOF，也不是连接故障。短暂让出 CPU 后继续读。
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                break;

            case IPuller::PullReadStatus::RetryableError: {
                if (options_.max_reconnect_count >= 0 &&
                    reconnect_count >= options_.max_reconnect_count) {
                    finished_.store(true);
                    return;
                }

                puller_->Close();
                ++reconnect_count;
                if (!WaitBeforeReconnect()) {
                    return;
                }

                current_generation = generation_.fetch_add(1) + 1;
                if (!OpenGeneration(current_generation)) {
                    // Open 失败仍属于可重试阶段；下一轮继续等待并尝试，
                    // 直到达到配置上限或收到 Stop。
                    if (options_.max_reconnect_count >= 0 &&
                        reconnect_count >= options_.max_reconnect_count) {
                        finished_.store(true);
                        return;
                    }
                    if (!WaitBeforeReconnect()) {
                        return;
                    }
                    continue;
                }
                break;
            }

            case IPuller::PullReadStatus::EOS:
            case IPuller::PullReadStatus::FatalError:
            case IPuller::PullReadStatus::Stopped:
                finished_.store(true);
                return;
        }
    }

    finished_.store(true);
}

bool StreamSourceNode::OpenGeneration(std::uint64_t generation) {
    if (stop_requested_.load() || generation_.load() != generation) {
        return false;
    }
    if (!puller_->Open(url_)) {
        return false;
    }

    UpdateStreamInfo(puller_->GetStreamInfo());
    if (stop_requested_.load() || generation_.load() != generation) {
        // Stop 可能正好发生在 Open 返回之后；不能把这个已打开的连接
        // 留给失效代次。
        puller_->Close();
        return false;
    }
    return true;
}

bool StreamSourceNode::WaitBeforeReconnect() {
    const int interval_ms = std::max(0, options_.reconnect_interval_ms);
    for (int elapsed = 0; elapsed < interval_ms; ++elapsed) {
        if (stop_requested_.load()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return !stop_requested_.load();
}

std::shared_ptr<const MediaStreamInfo> StreamSourceNode::FindStreamInfo(
    const std::shared_ptr<MediaPacket>& packet) const {
    if (!packet) {
        return nullptr;
    }

    std::lock_guard<std::mutex> lock(info_mutex_);
    for (const auto& info : stream_info_.stream_infos) {
        if (info.stream_index == packet->stream_index) {
            return std::make_shared<MediaStreamInfo>(info);
        }
    }

    // 某些轻量 Puller 只返回包级元数据，没有构造完整的 MultiStreamInfo。
    // 保留这个兼容路径，但真实 FFmpeg/AVTP Puller 应优先提供完整描述。
    return std::make_shared<MediaStreamInfo>(MakeFallbackStreamInfo(*packet));
}

void StreamSourceNode::UpdateStreamInfo(const MultiStreamInfo& info) {
    std::lock_guard<std::mutex> lock(info_mutex_);
    stream_info_ = info;
}

// ─────────────────────────────────────────────────────────────────────────────
// TrackRouterNode
// ─────────────────────────────────────────────────────────────────────────────

TrackRouterNode::TrackRouterNode(TrackSelection selection)
    : selection_(selection) {
    input_.SetHandler([this](MediaPacketMessage message) {
        Process(std::move(message));
    });
}

bool TrackRouterNode::RegisterPorts(PortRegistry& registry) {
    return registry.Input("in", input_) &&
           registry.Output("video", video_output_);
}

bool TrackRouterNode::Start() {
    accepting_.store(true);
    selected_stream_index_ = selection_.stream_index;
    selected_generation_ = 0;
    return true;
}

void TrackRouterNode::Stop() {
    accepting_.store(false);
}

std::string TrackRouterNode::Name() const {
    return "track-router";
}

InputPort<MediaPacketMessage>& TrackRouterNode::Input() {
    return input_;
}

OutputPort<MediaPacketMessage>& TrackRouterNode::VideoOutput() {
    return video_output_;
}

void TrackRouterNode::Process(MediaPacketMessage message) {
    if (!accepting_.load() || !message.Valid() ||
        message.packet->type != MediaType::VIDEO) {
        return;
    }

    // 自动选轨时，每个连接代次重新选择首个视频轨，避免重连后的 stream
    // index 变化导致新连接被永久丢弃。
    if (message.generation != selected_generation_) {
        selected_generation_ = message.generation;
        selected_stream_index_ = selection_.stream_index;
    }

    if (selection_.codec != CodecType::UNKNOWN &&
        message.packet->codec != selection_.codec) {
        return;
    }

    if (selected_stream_index_ < 0) {
        selected_stream_index_ = message.packet->stream_index;
    }
    if (message.packet->stream_index != selected_stream_index_) {
        return;
    }

    video_output_.Send(std::move(message));
}

// ─────────────────────────────────────────────────────────────────────────────
// DecoderNode
// ─────────────────────────────────────────────────────────────────────────────

DecoderNode::DecoderNode(std::unique_ptr<IDecoder> decoder,
                         MediaStreamInfo stream_info)
    : decoder_(std::move(decoder)),
      configured_stream_info_(std::move(stream_info)) {
    input_.SetHandler([this](MediaPacketMessage message) {
        Process(std::move(message));
    });
}

DecoderNode::~DecoderNode() {
    Deinit();
}

bool DecoderNode::RegisterPorts(PortRegistry& registry) {
    return registry.Input("in", input_) &&
           registry.Output("out", output_);
}

bool DecoderNode::Init() {
    if (!decoder_) {
        SetError("decoder is null");
        return false;
    }

    decoder_->SetFrameCallback([this](std::shared_ptr<MediaFrame> frame) {
        OnDecodedFrame(std::move(frame));
    });

    // 已知流信息时在 Init 完成 Open；未知时保留延迟 Open 路径，等待 Source
    // 在首个消息中提供真实编码参数和时间基。
    if (IsUsableStreamInfo(configured_stream_info_)) {
        if (!decoder_->Open(configured_stream_info_)) {
            SetError("decoder open failed during init");
            return false;
        }
        active_stream_info_ = configured_stream_info_;
        decoder_open_ = true;
    }
    return true;
}

bool DecoderNode::Start() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    accepting_ = true;
    active_generation_ = 0;
    callback_generation_ = 0;
    return true;
}

void DecoderNode::Stop() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    // Graph 当前是 Immediate Stop，没有 pending-task barrier。停止阶段不在
    // 这里 Flush，避免与 Executor 中尚未结束的 Decode 并发访问解码器；待 A3
    // 的 Graceful Stop 屏障接入后，再由上层显式调用 Flush()。
    accepting_ = false;
}

void DecoderNode::Deinit() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    accepting_ = false;
    if (decoder_) {
        decoder_->SetFrameCallback({});
        decoder_->Close();
    }
    decoder_open_ = false;
    active_generation_ = 0;
    callback_generation_ = 0;
}

std::string DecoderNode::Name() const {
    return "decoder";
}

InputPort<MediaPacketMessage>& DecoderNode::Input() {
    return input_;
}

OutputPort<MediaFrameMessage>& DecoderNode::Output() {
    return output_;
}

bool DecoderNode::Flush() {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!decoder_ || !decoder_open_) {
            return true;
        }
    }

    // 不能持有 state_mutex_ 调用 decoder_->Flush：Flush 可能同步触发
    // FrameCallback，而 OnDecodedFrame 需要重新取得同一把锁。
    if (!decoder_->Flush()) {
        SetError("decoder flush failed");
        return false;
    }
    return true;
}

std::string DecoderNode::LastError() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_error_;
}

void DecoderNode::Process(MediaPacketMessage message) {
    bool invalid_message = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!accepting_) {
            return;
        }
        if (!message.Valid() || !message.packet ||
            message.packet->type != MediaType::VIDEO) {
            invalid_message = true;
        }
        if (active_generation_ != 0 && message.generation < active_generation_) {
            // 旧连接的包可能已经在某条边的队列中。它们必须在这里被丢弃，
            // 不能重新送入新连接已经 Open 的 decoder。
            return;
        }
    }

    if (invalid_message) {
        SetError("invalid video packet message");
        return;
    }

    if (!OpenForMessage(message)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        callback_generation_ = message.generation;
    }
    if (!decoder_->Decode(std::move(message.packet))) {
        SetError("decoder decode failed");
    }
}

void DecoderNode::OnDecodedFrame(std::shared_ptr<MediaFrame> frame) {
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!accepting_ || !frame || callback_generation_ == 0) {
            return;
        }
        generation = callback_generation_;
    }
    output_.Send(MediaFrameMessage{std::move(frame), generation});
}

bool DecoderNode::OpenForMessage(const MediaPacketMessage& message) {
    MediaStreamInfo info = configured_stream_info_;
    if (message.stream_info && IsUsableStreamInfo(*message.stream_info)) {
        info = *message.stream_info;
    }
    if (!IsUsableStreamInfo(info)) {
        SetError("stream info is unavailable for decoder open");
        return false;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!accepting_) {
        return false;
    }

    const bool generation_changed = active_generation_ != 0 &&
                                    active_generation_ != message.generation;
    const bool stream_changed = decoder_open_ &&
                                !SameStream(active_stream_info_, info);
    if (generation_changed || stream_changed) {
        // 重连时不 Flush 旧代次：Flush 会生成旧连接的尾帧，违反“旧代次
        // 不进入新 decoder”的边界。旧上下文直接 Close，再打开新上下文。
        decoder_->Close();
        decoder_open_ = false;
    }

    if (!decoder_open_) {
        if (!decoder_->Open(info)) {
            SetErrorLocked("decoder open failed for packet stream");
            return false;
        }
        active_stream_info_ = info;
        decoder_open_ = true;
    }
    active_generation_ = message.generation;
    return true;
}

bool DecoderNode::IsUsableStreamInfo(const MediaStreamInfo& info) const {
    return info.media_type == MediaType::VIDEO &&
           info.codec_type != CodecType::UNKNOWN &&
           IsValidTimeBase(info.time_base);
}

bool DecoderNode::SameStream(const MediaStreamInfo& left,
                             const MediaStreamInfo& right) const {
    if (left.media_type != right.media_type ||
        left.codec_type != right.codec_type ||
        left.stream_index != right.stream_index ||
        left.time_base.num != right.time_base.num ||
        left.time_base.den != right.time_base.den ||
        left.extra_data != right.extra_data) {
        return false;
    }

    if (left.media_type == MediaType::VIDEO) {
        const auto& left_video = left.get_detail<VideoStreamInfo>();
        const auto& right_video = right.get_detail<VideoStreamInfo>();
        return left_video.width == right_video.width &&
               left_video.height == right_video.height &&
               left_video.pixel_format == right_video.pixel_format;
    }
    return true;
}

void DecoderNode::SetError(std::string message) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    SetErrorLocked(std::move(message));
}

void DecoderNode::SetErrorLocked(std::string message) {
    last_error_ = std::move(message);
}

} // namespace mediaflow
