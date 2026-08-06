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

bool StreamSourceNode::RegisterPorts(PortRegistry& registry) {
    // 保留 SourceNode 的混合 out 端口，兼容尚未迁移的旧图；新的媒体图应
    // 连接下面两个按轨道拆分的端口，避免音频突发和视频包共享同一条入口边。
    return SourceNode<MediaPacketMessage>::RegisterPorts(registry) &&
           registry.Output("video", video_output_) &&
           registry.Output("audio", audio_output_);
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

void StreamSourceNode::StopProduction() {
    // 这里只发出非阻塞停止请求，不能在排空屏障建立前等待网络 I/O 或 join
    // 读取线程；否则 GracefulStop 的 timeout 根本无法覆盖 Source 停止阶段。
    RequestProductionStop();
}

bool StreamSourceNode::IsProductionStopped() const {
    return finished_.load();
}

void StreamSourceNode::Stop() {
    // 先让读循环失效，并通过 Puller 的非阻塞请求唤醒底层 I/O。
    RequestProductionStop();
    if (read_thread_.joinable() &&
        read_thread_.get_id() != std::this_thread::get_id()) {
        // 必须先等待读线程退出，再调用 Close 释放 Puller 内部资源；这样
        // Puller 不需要在 Close 中与 av_read_frame 并发操作同一上下文。
        read_thread_.join();
    }
    if (puller_) {
        puller_->Close();
    }
    finished_.store(true);
}

void StreamSourceNode::RequestProductionStop() {
    stop_requested_.store(true);
    generation_.fetch_add(1);
    if (puller_) {
        puller_->RequestStop();
    }
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

OutputPort<MediaPacketMessage>& StreamSourceNode::VideoOutput() {
    return video_output_;
}

OutputPort<MediaPacketMessage>& StreamSourceNode::AudioOutput() {
    return audio_output_;
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

                // Puller 可能在首批压缩包到达后才补齐视频宽高（例如 RTSP
                // 探测阶段返回 0x0，parser 从 SPS/VPS 恢复尺寸）。每个包
                // 到达时刷新一次描述，确保动态恢复的信息能进入当前消息。
                UpdateStreamInfo(puller_->GetStreamInfo());
                auto message = MediaPacketMessage{};
                message.packet = result.packet;
                message.stream_info = FindStreamInfo(result.packet);
                message.generation = current_generation;
                // 先向兼容的混合 out 发送，再向对应轨道端口发送。未连接的
                // 端口不会创建队列；迁移后的正式图只连接 video/audio，因此
                // 音频和视频在 Source 边界使用完全独立的容量和背压策略。
                EmitTrackPacket(message);
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
                // 正常 EOF 必须沿媒体链路传播，后续 Decoder/Encoder 才能显式
                // Flush，并把编码器中尚未输出的尾部 packet 交给 Publisher。
                EmitTrackPacket(MediaPacketMessage{
                    nullptr,
                    nullptr,
                    current_generation,
                    true});
                finished_.store(true);
                return;
            case IPuller::PullReadStatus::FatalError:
            case IPuller::PullReadStatus::Stopped:
                finished_.store(true);
                return;
        }
    }

    finished_.store(true);
}

bool StreamSourceNode::EmitTrackPacket(const MediaPacketMessage& message) {
    // 混合 out 是旧图的兼容出口。MediaPacketMessage 只持有 shared_ptr，按
    // 三个出口复制消息对象不会复制实际媒体载荷；每个出口内部仍各自拥有
    // 独立的 Transport 队列。
    bool accepted = Emit(message);
    if (message.eos) {
        accepted = audio_output_.Send(message) && accepted;
        accepted = video_output_.Send(message) && accepted;
        return accepted;
    }

    if (!message.packet) {
        return accepted;
    }
    if (message.packet->type == MediaType::VIDEO) {
        accepted = video_output_.Send(message) && accepted;
    } else if (message.packet->type == MediaType::AUDIO) {
        accepted = audio_output_.Send(message) && accepted;
    }
    return accepted;
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

TrackRouterNode::TrackRouterNode(TrackSelection video_selection,
                                 TrackSelection audio_selection)
    : video_selection_(video_selection),
      audio_selection_(audio_selection) {
    input_.SetHandler([this](MediaPacketMessage message) {
        Process(std::move(message), InputTrack::Mixed);
    });
    video_input_.SetHandler([this](MediaPacketMessage message) {
        Process(std::move(message), InputTrack::Video);
    });
    audio_input_.SetHandler([this](MediaPacketMessage message) {
        Process(std::move(message), InputTrack::Audio);
    });
}

bool TrackRouterNode::RegisterPorts(PortRegistry& registry) {
    return registry.Input("in", input_) &&
           registry.Input("video-in", video_input_) &&
           registry.Input("audio-in", audio_input_) &&
           registry.Output("video", video_output_) &&
           registry.Output("audio", audio_output_);
}

bool TrackRouterNode::Start() {
    accepting_.store(true);
    selected_video_stream_index_ = video_selection_.stream_index;
    selected_audio_stream_index_ = audio_selection_.stream_index;
    selected_video_generation_ = 0;
    selected_audio_generation_ = 0;
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

InputPort<MediaPacketMessage>& TrackRouterNode::VideoInput() {
    return video_input_;
}

InputPort<MediaPacketMessage>& TrackRouterNode::AudioInput() {
    return audio_input_;
}

OutputPort<MediaPacketMessage>& TrackRouterNode::VideoOutput() {
    return video_output_;
}

OutputPort<MediaPacketMessage>& TrackRouterNode::AudioOutput() {
    return audio_output_;
}

void TrackRouterNode::Process(MediaPacketMessage message,
                              InputTrack input_track) {
    if (!accepting_.load() || !message.Valid()) {
        return;
    }

    if (message.eos) {
        // 混合兼容输入无法判断 EOS 属于哪条轨道，因此广播一次；拆分输入
        // 则只传播到对应输出，避免每个 Decoder 收到两次结束消息。
        if (input_track == InputTrack::Video) {
            video_output_.Send(std::move(message));
        } else if (input_track == InputTrack::Audio) {
            audio_output_.Send(std::move(message));
        } else {
            video_output_.Send(message);
            audio_output_.Send(std::move(message));
        }
        return;
    }

    if (!message.packet ||
        (message.packet->type != MediaType::VIDEO &&
         message.packet->type != MediaType::AUDIO)) {
        return;
    }

    // 独立输入边已经完成了媒体类型隔离；这里再次校验是为了保护兼容
    // 调用方，防止错误地把音频包接到 video-in 后进入错误 Decoder。
    if ((input_track == InputTrack::Video &&
         message.packet->type != MediaType::VIDEO) ||
        (input_track == InputTrack::Audio &&
         message.packet->type != MediaType::AUDIO)) {
        return;
    }

    if (message.packet->type == MediaType::VIDEO) {
        if (message.generation != selected_video_generation_) {
            selected_video_generation_ = message.generation;
            selected_video_stream_index_ = video_selection_.stream_index;
        }
        if (video_selection_.codec != CodecType::UNKNOWN &&
            message.packet->codec != video_selection_.codec) {
            return;
        }
        if (selected_video_stream_index_ < 0) {
            selected_video_stream_index_ = message.packet->stream_index;
        }
        if (message.packet->stream_index == selected_video_stream_index_) {
            video_output_.Send(std::move(message));
        }
        return;
    }

    if (message.generation != selected_audio_generation_) {
        selected_audio_generation_ = message.generation;
        selected_audio_stream_index_ = audio_selection_.stream_index;
    }
    if (audio_selection_.codec != CodecType::UNKNOWN &&
        message.packet->codec != audio_selection_.codec) {
        return;
    }
    if (selected_audio_stream_index_ < 0) {
        selected_audio_stream_index_ = message.packet->stream_index;
    }
    if (message.packet->stream_index == selected_audio_stream_index_) {
        audio_output_.Send(std::move(message));
    }
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
    flushed_ = false;
    return true;
}

void DecoderNode::Stop() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    // Stop 只关闭新的输入。GracefulStop 会先等待 Executor 和 Edge 排空，再
    // 调用 Flush；这样 Flush 不会与尚未结束的 Decode 并发访问解码器。
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
    flushed_ = false;
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
        if (!decoder_ || !decoder_open_ || flushed_) {
            return true;
        }
    }

    // 不能持有 state_mutex_ 调用 decoder_->Flush：Flush 可能同步触发
    // FrameCallback，而 OnDecodedFrame 需要重新取得同一把锁。
    if (!decoder_->Flush()) {
        SetError("decoder flush failed");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        flushed_ = true;
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
        if (!message.Valid() ||
            (!message.eos && !message.packet) ||
            (!message.eos && message.packet &&
             message.packet->type != MediaType::VIDEO &&
             message.packet->type != MediaType::AUDIO)) {
            invalid_message = true;
        }
        if (active_generation_ != 0 && message.generation < active_generation_) {
            // 旧连接的包可能已经在某条边的队列中。它们必须在这里被丢弃，
            // 不能重新送入新连接已经 Open 的 decoder。
            return;
        }
    }

    if (invalid_message) {
        SetError("invalid media packet message");
        return;
    }

    if (message.eos) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            active_generation_ = message.generation;
            callback_generation_ = message.generation;
        }
        if (!Flush()) {
            return;
        }
        output_.Send(MediaFrameMessage{nullptr, message.generation, true});
        return;
    }

    if (!OpenForMessage(message)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        callback_generation_ = message.generation;
        flushed_ = false;
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
        SetError("stream info is incomplete for decoder open");
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
    // 不完整元数据可能只对应探测阶段的临时包。后续收到完整描述并成功
    // 打开 Decoder 后清除临时错误，避免恢复后的测试仍被旧错误标记污染。
    last_error_.clear();
    active_generation_ = message.generation;
    return true;
}

bool DecoderNode::IsUsableStreamInfo(const MediaStreamInfo& info) const {
    if ((info.media_type != MediaType::VIDEO &&
         info.media_type != MediaType::AUDIO) ||
        info.codec_type == CodecType::UNKNOWN ||
        !IsValidTimeBase(info.time_base) ||
        info.stream_index < 0) {
        return false;
    }

    // FFmpeg 在网络探测尚未拿到 SPS/PPS 或音频参数时，可能返回 codec 已知但
    // 尺寸/采样参数仍为 0 的 StreamInfo。这样的描述不能用于打开 Decoder：
    // 继续 Open 会把一次探测不完整伪装成正常媒体流，随后只剩音频或静默无帧。
    if (info.media_type == MediaType::VIDEO) {
        if (!std::holds_alternative<VideoStreamInfo>(info.detail)) {
            return false;
        }
        const auto& video = info.get_detail<VideoStreamInfo>();
        return video.width > 0 && video.height > 0;
    }

    if (!std::holds_alternative<AudioStreamInfo>(info.detail)) {
        return false;
    }
    const auto& audio = info.get_detail<AudioStreamInfo>();
    return audio.sample_rate > 0 && audio.channels > 0;
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
    if (left.media_type == MediaType::AUDIO) {
        const auto& left_audio = left.get_detail<AudioStreamInfo>();
        const auto& right_audio = right.get_detail<AudioStreamInfo>();
        return left_audio.sample_rate == right_audio.sample_rate &&
               left_audio.channels == right_audio.channels &&
               left_audio.channel_layout == right_audio.channel_layout;
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

// ─────────────────────────────────────────────────────────────────────────────
// EncoderNode
// ─────────────────────────────────────────────────────────────────────────────

EncoderNode::EncoderNode(std::unique_ptr<IEncoder> encoder,
                         EncoderConfig config,
                         EncoderNodeOptions options)
    : encoder_(std::move(encoder)),
      config_(std::move(config)),
      options_(options) {
    input_.SetHandler([this](MediaFrameMessage message) {
        Process(std::move(message));
    });
}

EncoderNode::~EncoderNode() {
    Deinit();
}

bool EncoderNode::RegisterPorts(PortRegistry& registry) {
    return registry.Input("in", input_) &&
           registry.Output("out", output_);
}

bool EncoderNode::Init() {
    if (!encoder_) {
        SetError("encoder is null");
        return false;
    }
    if (!encoder_->Open(config_)) {
        SetError("encoder open failed during init");
        return false;
    }

    const auto info = encoder_->GetOutputInfo();
    if (info.media_type == MediaType::UNKNOWN ||
        info.codec_type == CodecType::UNKNOWN ||
        !IsValidTimeBase(info.time_base)) {
        encoder_->Close();
        SetError("encoder returned incomplete output track info");
        return false;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    output_info_ = info;
    encoder_open_ = true;
    flushed_ = false;
    return true;
}

bool EncoderNode::Start() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    accepting_ = true;
    active_generation_ = 0;
    flushed_ = false;
    return true;
}

void EncoderNode::Stop() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    // Stop 只关闭新的输入。GracefulStop 会在所有 Encode 任务完成后调用
    // Flush，避免 Stop 线程与节点 Executor 并发操作有状态 encoder。
    accepting_ = false;
}

void EncoderNode::Deinit() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    accepting_ = false;
    if (encoder_) {
        encoder_->Close();
    }
    encoder_open_ = false;
    active_generation_ = 0;
    output_info_ = {};
    flushed_ = false;
}

std::string EncoderNode::Name() const {
    return "encoder";
}

InputPort<MediaFrameMessage>& EncoderNode::Input() {
    return input_;
}

OutputPort<EncodedPacketMessage>& EncoderNode::Output() {
    return output_;
}

bool EncoderNode::Flush() {
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!encoder_ || !encoder_open_ || flushed_) {
            return true;
        }
        generation = active_generation_;
        if (generation == 0) {
            return true;
        }
    }

    std::vector<PacketPtr> packets;
    if (!encoder_->Flush(packets)) {
        SetError("encoder flush failed");
        return false;
    }
    const bool emitted = EmitPackets(packets, generation);
    if (emitted) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        flushed_ = true;
    }
    return emitted;
}

EncodedTrackInfo EncoderNode::OutputInfo() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return output_info_;
}

std::string EncoderNode::LastError() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_error_;
}

void EncoderNode::Process(MediaFrameMessage message) {
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!accepting_) {
            return;
        }
        if (!message.Valid() || (!message.eos && !message.frame) ||
            (!message.eos && message.frame &&
             message.frame->type != config_.media_type)) {
            SetErrorLocked("invalid frame message for encoder");
            return;
        }
        if (active_generation_ != 0 &&
            message.generation < active_generation_) {
            // Source 重连后，旧 frame 可能仍在上游边队列中。不能让它重新
            // 进入已经为新代次打开的 encoder。
            return;
        }
    }

    if (message.eos) {
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            active_generation_ = message.generation;
        }
        if (!Flush()) {
            return;
        }
        EncodedTrackInfo info;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            info = output_info_;
        }
        output_.Send(EncodedPacketMessage{
            nullptr, info, options_.track_id, message.generation, true});
        return;
    }

    if (!OpenForGeneration(message.generation)) {
        return;
    }

    std::vector<PacketPtr> packets;
    if (!encoder_->Encode(std::move(message.frame), packets)) {
        SetError("encoder encode failed");
        return;
    }
    if (!EmitPackets(packets, message.generation)) {
        SetError("encoder output packet was rejected");
    }
}

bool EncoderNode::OpenForGeneration(std::uint64_t generation) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!accepting_) {
        return false;
    }
    if (active_generation_ != 0 && generation < active_generation_) {
        return false;
    }

    if (active_generation_ != 0 && active_generation_ != generation) {
        // 旧代次不 Flush，避免旧连接尾包穿越到新连接。
        encoder_->Close();
        encoder_open_ = false;
    }

    if (!encoder_open_) {
        if (!encoder_->Open(config_)) {
            SetErrorLocked("encoder open failed for new generation");
            return false;
        }
        output_info_ = encoder_->GetOutputInfo();
        if (output_info_.media_type == MediaType::UNKNOWN ||
            output_info_.codec_type == CodecType::UNKNOWN ||
            !IsValidTimeBase(output_info_.time_base)) {
            encoder_->Close();
            encoder_open_ = false;
            SetErrorLocked("encoder returned incomplete output track info");
            return false;
        }
        encoder_open_ = true;
        flushed_ = false;
    }
    active_generation_ = generation;
    return true;
}

bool EncoderNode::EmitPackets(const std::vector<PacketPtr>& packets,
                              std::uint64_t generation) {
    EncodedTrackInfo info;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!accepting_ || active_generation_ != generation) {
            return false;
        }
        info = output_info_;
    }

    for (const auto& packet : packets) {
        if (!packet) {
            return false;
        }

        // FFmpegEncoder 当前以 0 作为内部占位 stream index。真正的 Publisher
        // track_id 由 MediaFlow EncoderNode 统一赋值，避免每个业务测试手工改包。
        packet->stream_index = options_.track_id;
        if (!output_.Send(EncodedPacketMessage{
                packet, info, options_.track_id, generation})) {
            return false;
        }
    }
    return true;
}

void EncoderNode::SetError(std::string message) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    SetErrorLocked(std::move(message));
}

void EncoderNode::SetErrorLocked(std::string message) {
    last_error_ = std::move(message);
}

// ─────────────────────────────────────────────────────────────────────────────
// PublisherSinkNode
// ─────────────────────────────────────────────────────────────────────────────

PublisherSinkNode::PublisherSinkNode(
    PublisherConfig config,
    std::unique_ptr<IPublisher> publisher,
    PublisherSinkNodeOptions options)
    : config_(std::move(config)),
      publisher_(std::move(publisher)),
      options_(options) {}

PublisherSinkNode::~PublisherSinkNode() {
    Deinit();
}

bool PublisherSinkNode::Init() {
    // Publisher 的最终轨道参数可能要等 Encoder 输出后才能得到，因此 Init
    // 不提前调用 Publisher::Start，也不要求部分填写的 track 在此时通过完整
    // ValidateStructure。空字段会由 PrepareTrack 合并 Encoder 信息，最终配置
    // 仍由 Start 阶段的 Publisher 做完整校验。
    return true;
}

bool PublisherSinkNode::Start() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    accepting_ = true;
    active_generation_ = 0;
    publisher_started_ = false;
    awaiting_keyframe_ = options_.wait_for_keyframe_on_start;
    pending_packets_.clear();
    seen_track_ids_.clear();
    eos_track_ids_.clear();
    return true;
}

void PublisherSinkNode::Stop() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    // Publisher 的 Stop 放在 Deinit。Graph 会在停止所有 Executor 后再调用
    // Deinit，避免这里与正在执行的 Publish 并发访问协议对象。
    accepting_ = false;
}

void PublisherSinkNode::Deinit() {
    std::lock_guard<std::mutex> lock(state_mutex_);
    accepting_ = false;
    if (publisher_) {
        publisher_->Stop();
    }
    publisher_started_ = false;
    awaiting_keyframe_ = false;
    pending_packets_.clear();
    seen_track_ids_.clear();
    eos_track_ids_.clear();
}

int PublisherSinkNode::StartPriority() const {
    // Publisher 先于 Encoder 启动，停止时则先停止 Publisher，防止 Encoder
    // 在 Publisher 已经关闭后继续把编码包投入终端节点。
    return 10;
}

std::string PublisherSinkNode::Name() const {
    return "publisher-sink";
}

PublisherStats PublisherSinkNode::Stats() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return publisher_ ? publisher_->GetStats() : PublisherStats{};
}

PublisherResult PublisherSinkNode::LastResult() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_result_;
}

std::string PublisherSinkNode::LastError() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_error_;
}

PublisherConfig PublisherSinkNode::Config() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return config_;
}

void PublisherSinkNode::Process(EncodedPacketMessage message) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!accepting_ || !message.Valid()) {
        return;
    }

    if (message.eos) {
        HandleEndOfStream(message);
        return;
    }
    if (!message.packet) {
        return;
    }

    if (active_generation_ != 0 && message.generation < active_generation_) {
        return;
    }
    if (active_generation_ != 0 && message.generation != active_generation_) {
        // 新输入代次必须从独立关键帧重新建立发布入口；旧 Publisher 会话不
        // 能把上一代的 DTS/网络状态带进新流。
        if (publisher_) {
            publisher_->Stop();
        }
        publisher_started_ = false;
        awaiting_keyframe_ = true;
        pending_packets_.clear();
        eos_track_ids_.clear();
    }
    active_generation_ = message.generation;

    // 多轨配置必须先收齐每条轨道的真实编码描述，才能一次性建立包含
    // 音频和视频的 FFmpeg 输出上下文。等待期间使用有界队列，防止某条轨道
    // 永远不出包时无限占用内存。
    if (!publisher_started_) {
        if (!PrepareTrack(message)) {
            last_result_ = PublisherResult::Failure(
                PublisherErrorCode::InvalidConfiguration, last_error_);
            return;
        }
        if (options_.wait_for_all_tracks && config_.tracks.size() > 1 &&
            !AllConfiguredTracksSeen()) {
            if (pending_packets_.size() >= options_.max_pending_packets) {
                last_result_ = PublisherResult::Failure(
                    PublisherErrorCode::InvalidState,
                    "publisher multi-track startup queue is full");
                return;
            }
            pending_packets_.push_back(std::move(message));
            return;
        }
    }

    const bool is_keyframe = IsVideoKeyframe(message);
    const bool is_video = message.track_info.media_type == MediaType::VIDEO;
    if (options_.wait_for_keyframe_on_start && awaiting_keyframe_ &&
        HasConfiguredVideoTrack() && (!is_video || !is_keyframe) &&
        (publisher_started_ || is_video || !AllConfiguredTracksSeen())) {
        if (!publisher_started_) {
            if (pending_packets_.size() >= options_.max_pending_packets) {
                last_result_ = PublisherResult::Failure(
                    PublisherErrorCode::InvalidState,
                    "publisher keyframe queue is full");
                return;
            }
            pending_packets_.push_back(std::move(message));
            return;
        }
        last_result_ = PublisherResult::Failure(
            PublisherErrorCode::AwaitingKeyframe,
            "publisher is waiting for the next video keyframe");
        return;
    }

    if (!publisher_started_ && !StartPublisher(message)) {
        return;
    }

    if (!pending_packets_.empty()) {
        auto pending = std::move(pending_packets_);
        pending_packets_.clear();
        for (const auto& queued : pending) {
            PublishPacket(queued);
        }
    }

    PublishPacket(message);
    return;
}

bool PublisherSinkNode::AllConfiguredTracksSeen() const {
    if (config_.tracks.empty()) {
        return true;
    }
    for (const auto& track : config_.tracks) {
        if (!seen_track_ids_.contains(track.track_id)) {
            return false;
        }
    }
    return true;
}

bool PublisherSinkNode::HasConfiguredVideoTrack() const {
    return std::any_of(
        config_.tracks.begin(), config_.tracks.end(),
        [](const MediaTrackConfig& track) {
            return track.media_type == MediaType::VIDEO;
        });
}

bool PublisherSinkNode::PublishPacket(const EncodedPacketMessage& message) {
    if (!publisher_started_ || !message.packet || !message.Valid()) {
        return false;
    }

    const bool is_video = message.track_info.media_type == MediaType::VIDEO;
    const bool is_keyframe = IsVideoKeyframe(message);
    if (options_.wait_for_keyframe_on_start && awaiting_keyframe_ &&
        HasConfiguredVideoTrack() && (!is_video || !is_keyframe)) {
        last_result_ = PublisherResult::Failure(
            PublisherErrorCode::AwaitingKeyframe,
            "publisher is waiting for the next video keyframe");
        return true;
    }

    last_result_ = publisher_->Publish(*message.packet);
    if (last_result_.IsSuccess()) {
        publisher_started_ = true;
        if (is_video && is_keyframe) {
            awaiting_keyframe_ = false;
        } else if (!HasConfiguredVideoTrack()) {
            awaiting_keyframe_ = false;
        }
        return true;
    }

    if (last_result_.code == PublisherErrorCode::AwaitingKeyframe) {
        publisher_started_ = true;
        awaiting_keyframe_ = true;
        return true;
    }

    publisher_started_ = false;
    awaiting_keyframe_ = true;
    last_error_ = last_result_.message;
    return false;
}

void PublisherSinkNode::HandleEndOfStream(
    const EncodedPacketMessage& message) {
    eos_track_ids_.insert(message.track_id);

    // 显式配置多轨时，只有所有轨道都收到 EOS 才能关闭 muxer；否则音频或
    // 视频其中一条先结束会截断另一条轨道的尾部数据。
    const bool all_tracks_finished = !config_.tracks.empty()
        ? std::all_of(config_.tracks.begin(), config_.tracks.end(),
                      [this](const MediaTrackConfig& track) {
                          return eos_track_ids_.contains(track.track_id);
                      })
        : true;
    if (all_tracks_finished && publisher_started_ && publisher_) {
        publisher_->Stop();
        publisher_started_ = false;
        awaiting_keyframe_ = false;
    }
}

bool PublisherSinkNode::PrepareTrack(const EncodedPacketMessage& message) {
    if (!message.Valid()) {
        last_error_ = "encoded packet message is incomplete";
        return false;
    }

    MediaTrackConfig* track = nullptr;
    if (config_.tracks.empty()) {
        config_.tracks.emplace_back();
        track = &config_.tracks.back();
        track->track_id = message.track_id;
        // MediaTrackConfig 有面向视频的默认值，但动态轨道必须先用 UNKNOWN
        // 表示“等待 Encoder 实际描述”，否则 H265 等合法输出会被默认 H264
        // 误判为配置冲突。
        track->media_type = MediaType::UNKNOWN;
        track->codec_type = CodecType::UNKNOWN;
    } else {
        const auto it = std::find_if(
            config_.tracks.begin(), config_.tracks.end(),
            [&message](const MediaTrackConfig& candidate) {
                return candidate.track_id == message.track_id;
            });
        if (it == config_.tracks.end()) {
            last_error_ = "encoded packet track is not configured in publisher";
            return false;
        }
        track = &*it;
    }

    const auto& info = message.track_info;
    if (track->media_type != MediaType::UNKNOWN &&
        track->media_type != info.media_type) {
        last_error_ = "publisher track media type does not match encoder output";
        return false;
    }
    if (track->codec_type != CodecType::UNKNOWN &&
        track->codec_type != info.codec_type) {
        last_error_ = "publisher track codec does not match encoder output";
        return false;
    }

    track->media_type = info.media_type;
    track->codec_type = info.codec_type;
    if (info.media_type == MediaType::VIDEO) {
        if (track->width > 0 && info.width > 0 && track->width != info.width) {
            last_error_ = "publisher video width does not match encoder output";
            return false;
        }
        if (track->height > 0 && info.height > 0 && track->height != info.height) {
            last_error_ = "publisher video height does not match encoder output";
            return false;
        }
        if (info.width > 0) track->width = info.width;
        if (info.height > 0) track->height = info.height;
        if (info.fps > 0.0f) track->fps = info.fps;
    } else if (info.media_type == MediaType::AUDIO) {
        if (info.sample_rate > 0) track->sample_rate = info.sample_rate;
        if (info.channels > 0) track->channels = info.channels;
    }

    if (IsValidTimeBase(info.time_base)) {
        track->time_base_num = info.time_base.num;
        track->time_base_den = info.time_base.den;
    }
    if (!info.extra_data.empty()) {
        track->extra_data = info.extra_data;
    }
    seen_track_ids_.insert(message.track_id);
    return true;
}

bool PublisherSinkNode::StartPublisher(const EncodedPacketMessage& message) {
    if (!PrepareTrack(message)) {
        last_result_ = PublisherResult::Failure(
            PublisherErrorCode::InvalidConfiguration, last_error_);
        return false;
    }
    if (!publisher_) {
        publisher_ = IPublisher::Create(config_);
    }
    if (!publisher_) {
        last_result_ = PublisherResult::Failure(
            PublisherErrorCode::InternalError,
            "failed to create publisher");
        last_error_ = last_result_.message;
        return false;
    }

    const auto structure_result = config_.ValidateStructure();
    if (!structure_result) {
        last_result_ = structure_result;
        last_error_ = last_result_.message;
        return false;
    }

    last_result_ = publisher_->Start();
    publisher_started_ = last_result_.IsSuccess();
    if (!publisher_started_) {
        last_error_ = last_result_.message;
    }
    return publisher_started_;
}

bool PublisherSinkNode::IsVideoKeyframe(
    const EncodedPacketMessage& message) const {
    return message.packet &&
           message.track_info.media_type == MediaType::VIDEO &&
           message.packet->keyframe;
}

void PublisherSinkNode::SetError(std::string message) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    last_error_ = std::move(message);
}

} // namespace mediaflow
