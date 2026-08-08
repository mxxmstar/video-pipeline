#include "mediaflow/mediaflow.h"

#include "media/decoder/ffmpeg_decoder.h"
#include "media/puller/ffmpeg_puller.h"
#include "media/simple_buffer.h"

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef VIDEO_PIPELINE_TEST_AV_PATH
#define VIDEO_PIPELINE_TEST_AV_PATH "test_av.ts"
#endif

namespace {

using namespace mediaflow;

struct DecoderState {
    std::mutex mutex;
    int open_count{0};
    int decode_count{0};
    int close_count{0};
};

MediaStreamInfo MakeVideoInfo() {
    MediaStreamInfo info;
    info.media_type = MediaType::VIDEO;
    info.codec_type = CodecType::H264;
    info.stream_index = 7;
    info.time_base = Rational{1, 1000};
    info.detail = VideoStreamInfo{640, 360, 25.0F, PixelFormat::kUnknown};
    return info;
}

MediaStreamInfo MakeAudioInfo() {
    MediaStreamInfo info;
    info.media_type = MediaType::AUDIO;
    info.codec_type = CodecType::AAC;
    info.stream_index = 8;
    info.time_base = Rational{1, 48000};
    info.detail = AudioStreamInfo{48000, 2, 0, SampleFormat::S16};
    return info;
}

std::shared_ptr<MediaPacket> MakePacket(MediaType type,
                                        int stream_index,
                                        int64_t pts,
                                        CodecType codec) {
    auto packet = std::make_shared<MediaPacket>();
    packet->type = type;
    packet->codec = codec;
    packet->stream_index = stream_index;
    packet->pts = pts;
    packet->dts = pts;
    packet->duration = 40;
    packet->time_base = Rational{1, 1000};
    packet->keyframe = type == MediaType::VIDEO;
    packet->buffer = std::make_shared<SimpleBuffer>(std::vector<std::uint8_t>{0x01});
    return packet;
}

void TestMediaQueueItemCostTraits() {
    auto packet = MakePacket(MediaType::VIDEO, 7, 40, CodecType::H264);
    packet->buffer = std::make_shared<SimpleBuffer>(
        std::vector<std::uint8_t>{0, 1, 2, 3, 4, 5, 6});
    packet->dts = 20;
    packet->duration = 2;

    MediaPacketMessage packet_message;
    packet_message.packet = packet;
    packet_message.generation = 3;
    const auto packet_cost = QueueItemTraits<MediaPacketMessage>::Cost(packet_message);
    assert(packet_cost.bytes == 7);
    assert(packet_cost.timestamp_us == 20000);
    assert(packet_cost.duration_us == 2000);
    assert(packet_cost.generation == 3);
    assert(packet_cost.track == QueueTrack::Video);
    assert(QueueItemTraits<MediaPacketMessage>::IsVideo(packet_message));
    assert(QueueItemTraits<MediaPacketMessage>::IsKeyframe(packet_message));

    packet->dts = kNoTimestamp;
    packet->pts = kNoTimestamp;
    assert(QueueItemTraits<MediaPacketMessage>::Cost(packet_message).timestamp_us ==
           kNoQueueTimestamp);

    MediaPacketMessage eos_packet;
    eos_packet.generation = 3;
    eos_packet.eos = true;
    const auto eos_packet_cost =
        QueueItemTraits<MediaPacketMessage>::Cost(eos_packet);
    assert(eos_packet_cost.bytes == 0);
    assert(eos_packet_cost.timestamp_us == kNoQueueTimestamp);
    assert(QueueItemTraits<MediaPacketMessage>::IsControl(eos_packet));

    auto frame = std::make_shared<MediaFrame>();
    frame->type = MediaType::AUDIO;
    frame->time.pts_us = 50000;
    frame->time.duration_us = 20000;
    frame->buffer = std::make_shared<SimpleBuffer>(
        std::vector<std::uint8_t>(12, 0));
    MediaFrameMessage frame_message{frame, 4};
    const auto frame_cost = QueueItemTraits<MediaFrameMessage>::Cost(frame_message);
    assert(frame_cost.bytes == 12);
    assert(frame_cost.timestamp_us == 50000);
    assert(frame_cost.duration_us == 20000);
    assert(frame_cost.generation == 4);
    assert(frame_cost.track == QueueTrack::Audio);
    assert(QueueItemTraits<MediaFrameMessage>::IsAudio(frame_message));

    MediaFrameMessage eos_frame{nullptr, 4, true};
    assert(QueueItemTraits<MediaFrameMessage>::Cost(eos_frame).bytes == 0);
    assert(QueueItemTraits<MediaFrameMessage>::IsControl(eos_frame));
}

void TestMediaFlowPipelineConfig() {
    MediaFlowPipelineConfig config;

    const auto audio_packet =
        config.MakePacketEdgeOptions(QueueTrack::Audio);
    assert(audio_packet.capacity == 512);
    assert(audio_packet.budget.max_items == 512);
    assert(audio_packet.budget.max_span_us == 1'000'000);
    assert(audio_packet.budget.max_bytes == 96'000);
    assert(audio_packet.backpressure == BackpressurePolicy::DropOldest);
    assert(audio_packet.max_batch_size == 128);

    // 覆盖设备级突发窗口后，字节上限必须随媒体时长重新计算，避免 items/span
    // 已经放宽而 bytes 仍沿用旧配置，形成隐蔽的第二重丢包边界。
    config.audio_packet.max_span_us = 2'000'000;
    const auto expanded_audio_packet =
        config.MakePacketEdgeOptions(QueueTrack::Audio);
    assert(expanded_audio_packet.budget.max_span_us == 2'000'000);
    assert(expanded_audio_packet.budget.max_bytes == 192'000);

    const auto video_frame =
        config.MakeFrameEdgeOptions(QueueTrack::Video);
    assert(video_frame.capacity == 8);
    assert(video_frame.budget.max_span_us == 250'000);
    assert(video_frame.budget.max_bytes == 0);
    assert(video_frame.backpressure == BackpressurePolicy::DropOldest);
}

void TestPlaybackProfileBuildsPipelineConfig() {
    const auto low_latency = PlaybackProfile::LowLatencyPreview();
    assert(low_latency.IsValid());
    assert(low_latency.EffectivePlaybackBufferMs() == 220);
    assert(low_latency.ResolveVideoQueueFrames() == 7);
    assert(low_latency.ResolveAudioQueueFrames() == 50);
    assert(low_latency.ResolveAudioPcmQueueChunks() == 19);

    const auto low_latency_pipeline = low_latency.MakePipelineConfig();
    assert(low_latency_pipeline.video_packet.max_span_us == 250'000);
    assert(low_latency_pipeline.audio_packet.max_span_us == 250'000);
    assert(low_latency_pipeline.video_frame.max_span_us == 250'000);
    assert(low_latency_pipeline.video_frame.max_items == 8);
    assert(low_latency_pipeline.audio_frame.max_span_us == 500'000);
    assert(low_latency_pipeline.audio_frame.max_items == 64);

    const auto stable = PlaybackProfile::StablePlayback();
    assert(stable.IsValid());
    assert(stable.EffectivePlaybackBufferMs() == 600);
    assert(stable.ResolveVideoQueueFrames() == 16);
    assert(stable.ResolveAudioQueueFrames() == 50);
    assert(stable.ResolveAudioPcmQueueChunks() == 38);

    const auto stable_pipeline = stable.MakePipelineConfig();
    assert(stable_pipeline.video_packet.max_span_us == 1'000'000);
    assert(stable_pipeline.audio_packet.max_span_us == 1'000'000);
    assert(stable_pipeline.video_frame.max_span_us == 600'000);
    assert(stable_pipeline.video_frame.max_items == 16);
    assert(stable_pipeline.audio_frame.max_span_us == 600'000);
    assert(stable_pipeline.audio_frame.max_items == 64);

    auto custom = PlaybackProfile::Custom();
    custom.playback_buffer_ms = 300;
    custom.network_jitter_buffer_ms = 600;
    custom.audio_device_buffer_ms = 150;
    custom.expected_video_frame_rate = 30;
    custom.max_video_queue_frames = 24;
    custom.max_audio_queue_frames = 72;
    assert(custom.IsValid());
    const auto custom_pipeline = custom.MakePipelineConfig();
    assert(custom_pipeline.video_packet.max_span_us == 600'000);
    assert(custom_pipeline.audio_packet.max_span_us == 600'000);
    assert(custom_pipeline.video_frame.max_span_us == 450'000);
    assert(custom_pipeline.video_frame.max_items == 24);
    assert(custom_pipeline.audio_frame.max_span_us == 500'000);
    assert(custom_pipeline.audio_frame.max_items == 72);

    custom.audio_device_buffer_ms = 0;
    assert(!custom.IsValid());
}

class ScriptedPuller final : public IPuller {
public:
    bool Open(const std::string&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ++open_count_;
        read_index_ = 0;
        closed_ = false;
        return true;
    }

    void Close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }

    bool ReadPacket(std::shared_ptr<MediaPacket>& packet) override {
        const auto result = ReadPacketResult();
        packet = result.packet;
        return result.status == PullReadStatus::Packet;
    }

    PullReadResult ReadPacketResult() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return {PullReadStatus::Stopped, nullptr, 0, "closed"};
        }

        if (open_count_ == 1) {
            if (read_index_++ == 0) {
                // 音频包必须被 TrackRouterNode 丢弃，不能误送进视频 decoder。
                return PullReadResult::PacketResult(
                    MakePacket(MediaType::AUDIO, 8, 1, CodecType::AAC));
            }
            if (read_index_ == 2) {
                return PullReadResult::PacketResult(
                    MakePacket(MediaType::VIDEO, 7, 40, CodecType::H264));
            }
            return {PullReadStatus::RetryableError, nullptr, -11, "temporary"};
        }

        if (open_count_ == 2) {
            if (read_index_++ == 0) {
                return PullReadResult::PacketResult(
                    MakePacket(MediaType::VIDEO, 7, 80, CodecType::H264));
            }
            return {PullReadStatus::EOS, nullptr, 0, "eos"};
        }

        return {PullReadStatus::EOS, nullptr, 0, "eos"};
    }

    MultiStreamInfo GetStreamInfo() const override {
        MultiStreamInfo info;
        info.stream_infos.push_back(MakeVideoInfo());
        info.video_stream_idx_ = 0;
        return info;
    }

    void SetEventCallback(EventCallback cb) override {
        event_callback_ = std::move(cb);
    }

private:
    mutable std::mutex mutex_;
    int open_count_{0};
    int read_index_{0};
    bool closed_{true};
    EventCallback event_callback_;
};

/// 先制造音频突发，再发送视频关键帧，用于验证 Source 入口的轨道隔离。
class BurstTrackPuller final : public IPuller {
public:
    bool Open(const std::string&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        index_ = 0;
        closed_ = false;
        stop_requested_ = false;
        return true;
    }

    void RequestStop() override {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
    }

    void Close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        stop_requested_ = true;
    }

    bool ReadPacket(std::shared_ptr<MediaPacket>& packet) override {
        const auto result = ReadPacketResult();
        packet = result.packet;
        return result.status == PullReadStatus::Packet;
    }

    PullReadResult ReadPacketResult() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || stop_requested_) {
            return {PullReadStatus::Stopped, nullptr, 0, "stop requested"};
        }

        if (index_ < 8) {
            return PullReadResult::PacketResult(
                MakePacket(MediaType::AUDIO, 8, index_++ * 960,
                           CodecType::AAC));
        }
        if (index_++ == 8) {
            auto packet = MakePacket(MediaType::VIDEO, 7, 40, CodecType::H264);
            packet->keyframe = true;
            return PullReadResult::PacketResult(std::move(packet));
        }
        return {PullReadStatus::EOS, nullptr, 0, "eos"};
    }

    MultiStreamInfo GetStreamInfo() const override {
        MultiStreamInfo info;
        info.stream_infos.push_back(MakeVideoInfo());
        info.stream_infos.push_back(MakeAudioInfo());
        info.video_stream_idx_ = 0;
        info.audio_stream_idx_ = 1;
        return info;
    }

    void SetEventCallback(EventCallback cb) override {
        event_callback_ = std::move(cb);
    }

private:
    mutable std::mutex mutex_;
    std::size_t index_{0};
    bool closed_{true};
    bool stop_requested_{false};
    EventCallback event_callback_;
};

class BlockingStopPuller final : public IPuller {
public:
    bool Open(const std::string&) override {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = false;
        read_started_ = false;
        read_returned_ = false;
        close_called_ = false;
        return true;
    }

    void RequestStop() override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
            ++request_stop_count_;
        }
        condition_.notify_all();
    }

    void Close() override {
        std::unique_lock<std::mutex> lock(mutex_);
        close_called_ = true;
        // 模拟 Puller 不能与 ReadPacketResult 并发关闭底层上下文：只有读线程
        // 退出后 Close 才允许返回。若 Source 先 Close 再发停止请求，这里会
        // 永久等待，正好覆盖本阶段修复的生命周期顺序问题。
        condition_.wait(lock, [this]() {
            return read_returned_;
        });
    }

    bool ReadPacket(std::shared_ptr<MediaPacket>& packet) override {
        const auto result = ReadPacketResult();
        packet = result.packet;
        return result.status == PullReadStatus::Packet;
    }

    PullReadResult ReadPacketResult() override {
        std::unique_lock<std::mutex> lock(mutex_);
        read_started_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this]() {
            return stop_requested_;
        });
        read_returned_ = true;
        condition_.notify_all();
        return {PullReadStatus::Stopped, nullptr, 0, "stop requested"};
    }

    MultiStreamInfo GetStreamInfo() const override {
        return {};
    }

    void SetEventCallback(EventCallback cb) override {
        event_callback_ = std::move(cb);
    }

    bool WaitUntilReadStarted() {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [this]() {
            return read_started_;
        });
    }

    bool WaitUntilReadReturned() {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [this]() {
            return read_returned_;
        });
    }

    int RequestStopCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return request_stop_count_;
    }

    bool CloseCalled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return close_called_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool stop_requested_{false};
    bool read_started_{false};
    bool read_returned_{false};
    bool close_called_{false};
    int request_stop_count_{0};
    EventCallback event_callback_;
};

class RecordingDecoder final : public IDecoder {
public:
    explicit RecordingDecoder(std::shared_ptr<DecoderState> state)
        : state_(std::move(state)) {}

    bool Open(const MediaStreamInfo& info) override {
        assert(info.media_type == MediaType::VIDEO);
        assert(info.codec_type == CodecType::H264);
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->open_count;
        return true;
    }

    bool Flush() override {
        return true;
    }

    void Close() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->close_count;
    }

    bool Decode(std::shared_ptr<MediaPacket> packet) override {
        assert(packet);
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            ++state_->decode_count;
        }
        auto frame = std::make_shared<MediaFrame>();
        frame->type = MediaType::VIDEO;
        frame->time.pts_us = packet->pts * 1000;
        frame->meta = VideoFrameMeta{};
        frame_callback_(std::move(frame));
        return true;
    }

    void SetFrameCallback(FrameCallback cb) override {
        frame_callback_ = std::move(cb);
    }

private:
    std::shared_ptr<DecoderState> state_;
    FrameCallback frame_callback_;
};

class FrameSink final : public SinkNode<MediaFrameMessage> {
public:
    bool WaitFor(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [this, count]() {
            return generations_.size() >= count;
        });
    }

    std::vector<std::uint64_t> Generations() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generations_;
    }

    std::size_t Count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return generations_.size();
    }

    std::string Name() const override {
        return "frame-sink";
    }

    protected:
    void Process(MediaFrameMessage message) override {
        assert(message.Valid());
        if (message.eos) {
            // EOS 是控制消息，不应计入解码帧 generation 断言。
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            generations_.push_back(message.generation);
        }
        condition_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<std::uint64_t> generations_;
};

class BlockingPacketSink final : public SinkNode<MediaPacketMessage> {
public:
    explicit BlockingPacketSink(MediaType expected_type, bool block_first)
        : expected_type_(expected_type), block_first_(block_first) {}

    std::string Name() const override {
        return expected_type_ == MediaType::AUDIO ? "audio-packet-sink"
                                                   : "video-packet-sink";
    }

    bool WaitUntilEntered() {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [this]() {
            return entered_;
        });
    }

    bool WaitForPackets(std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [this, count]() {
            return packet_count_ >= count;
        });
    }

    bool WaitForEos() {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(lock, std::chrono::seconds(2), [this]() {
            return eos_count_ >= 1;
        });
    }

    void Release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

    int EosCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return eos_count_;
    }

protected:
    void Process(MediaPacketMessage message) override {
        if (message.eos) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++eos_count_;
            condition_.notify_all();
            return;
        }
        assert(message.packet);
        assert(message.packet->type == expected_type_);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ++packet_count_;
            if (block_first_ && !entered_) {
                entered_ = true;
                condition_.notify_all();
                condition_.wait(lock, [this]() {
                    return released_;
                });
            }
        }
        condition_.notify_all();
    }

private:
    const MediaType expected_type_;
    const bool block_first_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t packet_count_{0};
    int eos_count_{0};
    bool entered_{false};
    bool released_{false};
};

struct EncoderState {
    std::mutex mutex;
    int open_count{0};
    int encode_count{0};
    int flush_count{0};
    int close_count{0};
};

class RecordingEncoder final : public IEncoder {
public:
    explicit RecordingEncoder(std::shared_ptr<EncoderState> state)
        : state_(std::move(state)) {}

    bool Open(const EncoderConfig& config) override {
        config_ = config;
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->open_count;
        return true;
    }

    bool Encode(FramePtr frame, std::vector<PacketPtr>& packets) override {
        assert(frame);
        int encode_index = 0;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            encode_index = ++state_->encode_count;
        }

        auto first = MakePacket(MediaType::VIDEO, 0, encode_index * 40,
                                CodecType::H264);
        first->keyframe = false;
        packets.push_back(std::move(first));

        // 一帧输出两个包，验证 EncoderNode 不会只转发 vector 的第一个元素。
        auto second = MakePacket(MediaType::VIDEO, 0, encode_index * 40 + 20,
                                 CodecType::H264);
        second->keyframe = encode_index == 1;
        packets.push_back(std::move(second));
        return true;
    }

    bool Flush(std::vector<PacketPtr>& packets) override {
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            ++state_->flush_count;
        }
        auto tail = MakePacket(MediaType::VIDEO, 0, 100, CodecType::H264);
        tail->keyframe = true;
        packets.push_back(std::move(tail));
        return true;
    }

    EncodedTrackInfo GetOutputInfo() const override {
        EncodedTrackInfo info;
        info.media_type = MediaType::VIDEO;
        info.codec_type = CodecType::H264;
        info.time_base = Rational{1, 1000};
        info.width = config_.video.width;
        info.height = config_.video.height;
        info.fps = 25.0F;
        info.extra_data = {0x01, 0x64, 0x00, 0x1f};
        return info;
    }

    void Close() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->close_count;
    }

private:
    std::shared_ptr<EncoderState> state_;
    EncoderConfig config_;
};

struct PublisherState {
    std::mutex mutex;
    std::condition_variable condition;
    int start_count{0};
    int stop_count{0};
    int publish_attempts{0};
    bool await_next_publish{false};
    std::vector<MediaPacket> packets;
    PublisherConfig config;
};

class RecordingPublisher final : public IPublisher {
public:
    explicit RecordingPublisher(std::shared_ptr<PublisherState> state)
        : state_(std::move(state)) {}

    PublisherResult Start() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->start_count;
        started_ = true;
        state_->config = config_;
        return PublisherResult::Success();
    }

    PublisherResult Publish(const MediaPacket& packet) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!started_) {
            return PublisherResult::Failure(
                PublisherErrorCode::InvalidState, "publisher is stopped");
        }
        ++state_->publish_attempts;
        state_->condition.notify_all();
        if (state_->await_next_publish) {
            state_->await_next_publish = false;
            return PublisherResult::Failure(
                PublisherErrorCode::AwaitingKeyframe,
                "waiting for keyframe");
        }
        state_->packets.push_back(packet);
        state_->condition.notify_all();
        return PublisherResult::Success();
    }

    void Stop() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->stop_count;
        started_ = false;
        state_->condition.notify_all();
    }

    std::string GetPlayUrl() const override {
        return "recording://publisher";
    }

    PublisherStats GetStats() const override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        PublisherStats stats;
        stats.packets_published = state_->packets.size();
        return stats;
    }

    PublisherResult GetLastResult() const override {
        return PublisherResult::Success();
    }

    void SetConfig(PublisherConfig config) {
        config_ = std::move(config);
    }

private:
    std::shared_ptr<PublisherState> state_;
    PublisherConfig config_;
    bool started_{false};
};

class VideoFrameSource final : public SourceNode<MediaFrameMessage> {
public:
    std::string Name() const override {
        return "video-frame-source";
    }

    bool Start() override {
        auto frame = std::make_shared<MediaFrame>();
        frame->type = MediaType::VIDEO;
        frame->time.pts_us = 0;
        frame->meta = VideoFrameMeta{PixelFormat::kI420, 640, 360, 0, {}};
        Emit(MediaFrameMessage{std::move(frame), 1});
        return true;
    }
};

class EncodedPacketSource final : public SourceNode<EncodedPacketMessage> {
public:
    EncodedPacketSource(int track_id, EncodedTrackInfo info)
        : track_id_(track_id), info_(std::move(info)) {}

    std::string Name() const override {
        return "encoded-source";
    }

    bool Start() override {
        auto packet = MakePacket(
            info_.media_type,
            track_id_,
            0,
            info_.codec_type);
        packet->time_base = info_.time_base;
        packet->keyframe = info_.media_type == MediaType::VIDEO;
        Emit(EncodedPacketMessage{
            packet, info_, track_id_, 1, false});
        // EOS 单独携带轨道描述，PublisherSink 才能判断多轨是否全部结束。
        Emit(EncodedPacketMessage{
            nullptr, info_, track_id_, 1, true});
        return true;
    }

private:
    int track_id_;
    EncodedTrackInfo info_;
};

/// 双轨压缩包源：故意让 PTS 显示顺序与 DTS 解码顺序交叉，验证 Publisher
/// 节点实际使用 MediaFlow 的 DTS 交织器，而不是按两个输入 Edge 的到达顺序写出。
class DtsOrderPacketSource final : public SourceNode<EncodedPacketMessage> {
public:
    DtsOrderPacketSource(int track_id,
                         EncodedTrackInfo info,
                         std::vector<std::pair<std::int64_t, std::int64_t>> times)
        : track_id_(track_id), info_(std::move(info)), times_(std::move(times)) {}

    std::string Name() const override {
        return "dts-order-source";
    }

    bool Start() override {
        for (const auto& [pts, dts] : times_) {
            auto packet = MakePacket(info_.media_type, track_id_, pts,
                                     info_.codec_type);
            packet->dts = dts;
            packet->pts = pts;
            packet->time_base = info_.time_base;
            packet->keyframe = info_.media_type == MediaType::VIDEO && pts == 0;
            Emit(EncodedPacketMessage{
                packet, info_, track_id_, 1, false});
        }
        Emit(EncodedPacketMessage{
            nullptr, info_, track_id_, 1, true});
        return true;
    }

private:
    int track_id_;
    EncodedTrackInfo info_;
    std::vector<std::pair<std::int64_t, std::int64_t>> times_;
};

bool WaitForPublished(const std::shared_ptr<PublisherState>& state,
                      std::size_t count) {
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->condition.wait_for(lock, std::chrono::seconds(2),
                                     [state, count]() {
        return state->packets.size() >= count;
    });
}

bool WaitForPublishAttempts(const std::shared_ptr<PublisherState>& state,
                            int count) {
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->condition.wait_for(lock, std::chrono::seconds(2),
                                     [state, count]() {
        return state->publish_attempts >= count;
    });
}

bool WaitForPublisherStop(const std::shared_ptr<PublisherState>& state) {
    std::unique_lock<std::mutex> lock(state->mutex);
    return state->condition.wait_for(lock, std::chrono::seconds(2),
                                     [state]() {
                                         return state->stop_count >= 1;
                                     });
}

void TestSingleVideoChainAndReconnectGeneration() {
    auto decoder_state = std::make_shared<DecoderState>();
    auto executor = std::make_shared<AsioExecutor>("media-node-test", 2);

    Graph graph;
    assert(graph.AddNode<StreamSourceNode>(
        "source", executor, NodeOptions{}, "camera",
        std::make_unique<ScriptedPuller>(), "scripted://camera",
        StreamSourceNodeOptions{1, 2}));
    assert(graph.AddNode<TrackRouterNode>(
        "router", executor, NodeOptions{}, TrackSelection{7, CodecType::H264}));
    assert(graph.AddNode<DecoderNode>(
        "decoder", executor, NodeOptions{},
        std::make_unique<RecordingDecoder>(decoder_state)));
    assert(graph.AddNode<FrameSink>("sink", executor, NodeOptions{}));

    assert(graph.Connect<MediaPacketMessage>("source", "router"));
    assert(graph.Connect<MediaPacketMessage>(
        "router", "video", "decoder", "in"));
    assert(graph.Connect<MediaFrameMessage>("decoder", "sink"));
    assert(graph.Start());

    auto sink = graph.GetNode<FrameSink>("sink");
    auto source = graph.GetNode<StreamSourceNode>("source");
    auto decoder = graph.GetNode<DecoderNode>("decoder");
    assert(sink && source && decoder);
    assert(sink->WaitFor(2));

    const auto generations = sink->Generations();
    assert((generations == std::vector<std::uint64_t>{1, 2}));
    assert(source->Finished());
    assert(source->Generation() >= 2);
    assert(decoder->LastError().empty());

    {
        std::lock_guard<std::mutex> lock(decoder_state->mutex);
        // 首次连接和重连各打开一次，音频旁路且两个视频包都被解码。
        assert(decoder_state->open_count == 2);
        assert(decoder_state->decode_count == 2);
    }

    // 模拟边队列中晚到的旧代次消息，Decoder 必须丢弃它而不是重新打开
    // 新连接的上下文或向 sink 追加第三帧。
    MediaPacketMessage stale;
    stale.packet = MakePacket(MediaType::VIDEO, 7, 120, CodecType::H264);
    stale.stream_info = std::make_shared<MediaStreamInfo>(MakeVideoInfo());
    stale.generation = 1;
    decoder->Input().Receive(std::move(stale));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(sink->Generations().size() == 2);

    graph.Stop();
}

void TestSourceTrackQueuesAreIndependent() {
    auto source_executor = std::make_shared<AsioExecutor>("track-source", 1);
    auto audio_executor = std::make_shared<AsioExecutor>("track-audio", 1);
    auto video_executor = std::make_shared<AsioExecutor>("track-video", 1);

    Graph graph;
    assert(graph.AddNode<StreamSourceNode>(
        "source", source_executor, NodeOptions{}, "burst",
        std::make_unique<BurstTrackPuller>(), "scripted://burst"));
    assert(graph.AddNode<BlockingPacketSink>(
        "audio-sink", audio_executor, NodeOptions{}, MediaType::AUDIO, true));
    assert(graph.AddNode<BlockingPacketSink>(
        "video-sink", video_executor, NodeOptions{}, MediaType::VIDEO, false));

    EdgeOptions audio_edge;
    audio_edge.capacity = 2;
    audio_edge.backpressure = BackpressurePolicy::DropNewest;
    audio_edge.max_batch_size = 1;
    EdgeOptions video_edge;
    video_edge.capacity = 2;
    video_edge.backpressure = BackpressurePolicy::PreferVideoKeyframes;
    video_edge.max_batch_size = 1;

    // Source 的两个轨道端口必须对应两条不同的 Edge。音频消费者被阻塞时，
    // 音频包只能填满 audio 边；视频关键帧仍应进入独立的 video 边。
    assert(graph.Connect<MediaPacketMessage>(
        "source", "audio", "audio-sink", "in", audio_edge));
    assert(graph.Connect<MediaPacketMessage>(
        "source", "video", "video-sink", "in", video_edge));
    assert(graph.Start());

    auto source = graph.GetNode<StreamSourceNode>("source");
    auto audio_sink = graph.GetNode<BlockingPacketSink>("audio-sink");
    auto video_sink = graph.GetNode<BlockingPacketSink>("video-sink");
    assert(source && audio_sink && video_sink);
    assert(audio_sink->WaitUntilEntered());
    assert(video_sink->WaitForPackets(1));

    const auto source_finished_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!source->Finished() &&
           std::chrono::steady_clock::now() < source_finished_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(source->Finished());

    EdgeMetricsSnapshot audio_metrics;
    EdgeMetricsSnapshot video_metrics;
    assert(graph.GetEdgeMetrics("source:audio->audio-sink:in", audio_metrics));
    assert(graph.GetEdgeMetrics("source:video->video-sink:in", video_metrics));
    assert(audio_metrics.dropped_newest > 0);
    assert(video_metrics.accepted >= 1);
    assert(video_metrics.dropped_newest == 0);

    // EOS 是控制消息，即使音频边已满也必须替换普通音频包进入队列；释放
    // 阻塞消费者后，两条独立轨道各收到一次 EOS，验证 Flush 边界没有丢失。
    audio_sink->Release();
    assert(audio_sink->WaitForEos());
    assert(video_sink->WaitForEos());
    assert(audio_sink->EosCount() == 1);
    assert(video_sink->EosCount() == 1);
    graph.Stop();
}

void TestDecoderRejectsIncompleteStreamInfo() {
    auto decoder_state = std::make_shared<DecoderState>();
    DecoderNode decoder(std::make_unique<RecordingDecoder>(decoder_state));

    PortRegistry registry;
    assert(decoder.RegisterPorts(registry));
    assert(decoder.Init());
    assert(decoder.Start());

    // 模拟 FFmpeg 探测阶段返回的 H.264 轨道：codec 和 time_base 已知，
    // 但视频尺寸仍为 0。DecoderNode 必须拒绝这条描述，不能把无效参数
    // 交给具体 Decoder，也不能让测试误报为“已打开视频解码”。
    MediaStreamInfo incomplete = MakeVideoInfo();
    incomplete.detail = VideoStreamInfo{0, 0, 25.0F, PixelFormat::kUnknown};

    MediaPacketMessage message;
    message.packet = MakePacket(MediaType::VIDEO, 7, 0, CodecType::H264);
    message.stream_info = std::make_shared<MediaStreamInfo>(incomplete);
    message.generation = 1;
    decoder.Input().Receive(std::move(message));

    assert(decoder.LastError() == "stream info is incomplete for decoder open");
    {
        std::lock_guard<std::mutex> lock(decoder_state->mutex);
        assert(decoder_state->open_count == 0);
        assert(decoder_state->decode_count == 0);
    }

    // 探测阶段的临时 0x0 描述不应永久污染 Decoder。收到完整描述后，
    // 节点必须恢复 Open/Decode，并清除临时错误状态。
    message.stream_info = std::make_shared<MediaStreamInfo>(MakeVideoInfo());
    message.packet = MakePacket(MediaType::VIDEO, 7, 40, CodecType::H264);
    decoder.Input().Receive(std::move(message));
    {
        std::lock_guard<std::mutex> lock(decoder_state->mutex);
        assert(decoder_state->open_count == 1);
        assert(decoder_state->decode_count == 1);
    }
    assert(decoder.LastError().empty());

    decoder.Stop();
    decoder.Deinit();
}

void TestFfmpegDecoderRecoversAfterVideoPacketGap() {
    // 该测试使用真实 FFmpegPuller/FFmpegDecoder，但不连接摄像头。跳过一个
    // 非关键包来模拟 Queue/Dispatch 丢包，MediaFlow 序号负责把这个事实传给
    // Decoder；因此测试验证的是实际 H.264 解码器重建，而不是模拟 Decoder
    // 只返回一帧的理想路径。
    FFmpegPuller puller;
    puller.SetLowLatency(false);
    puller.SetConnectTimeoutMs(3000);
    puller.SetReadTimeoutMs(3000);
    assert(puller.Open(VIDEO_PIPELINE_TEST_AV_PATH));

    const auto streams = puller.GetStreamInfo();
    assert(streams.HasVideoStream());
    assert(streams.HasAudioStream());
    const auto video_info =
        streams.stream_infos[streams.video_stream_idx_];
    const auto audio_info =
        streams.stream_infos[streams.audio_stream_idx_];
    auto video_stream_info =
        std::make_shared<const MediaStreamInfo>(video_info);
    auto audio_stream_info =
        std::make_shared<const MediaStreamInfo>(audio_info);

    auto executor = std::make_shared<AsioExecutor>("ffmpeg-recovery-test", 2);
    Graph graph;
    assert(graph.AddNode<DecoderNode>(
        "decoder", executor, NodeOptions{},
        std::make_unique<FFmpegDecoder>()));
    assert(graph.AddNode<DecoderNode>(
        "audio-decoder", executor, NodeOptions{},
        std::make_unique<FFmpegDecoder>()));
    assert(graph.AddNode<FrameSink>("sink", executor, NodeOptions{}));
    assert(graph.AddNode<FrameSink>("audio-sink", executor, NodeOptions{}));
    assert(graph.Connect<MediaFrameMessage>("decoder", "sink"));
    assert(graph.Connect<MediaFrameMessage>("audio-decoder", "audio-sink"));
    assert(graph.Start());

    auto decoder = graph.GetNode<DecoderNode>("decoder");
    auto audio_decoder = graph.GetNode<DecoderNode>("audio-decoder");
    auto sink = graph.GetNode<FrameSink>("sink");
    auto audio_sink = graph.GetNode<FrameSink>("audio-sink");
    assert(decoder && audio_decoder && sink && audio_sink);

    std::uint64_t video_sequence = 0;
    std::uint64_t audio_sequence = 0;
    bool gap_injected = false;
    bool recovery_keyframe_sent = false;
    std::size_t frames_before_recovery = 0;
    int video_packets_seen = 0;
    constexpr int kMaxVideoPackets = 360;

    while (video_packets_seen < kMaxVideoPackets &&
           (!recovery_keyframe_sent || sink->Count() <= frames_before_recovery)) {
        std::shared_ptr<MediaPacket> packet;
        if (!puller.ReadPacket(packet)) {
            break;
        }
        if (!packet || (packet->type != MediaType::VIDEO &&
                        packet->type != MediaType::AUDIO)) {
            continue;
        }

        if (packet->type == MediaType::AUDIO) {
            MediaPacketMessage audio_message;
            audio_message.packet = std::move(packet);
            audio_message.stream_info = audio_stream_info;
            audio_message.generation = 1;
            audio_message.sequence = ++audio_sequence;
            audio_decoder->Input().Receive(std::move(audio_message));
            continue;
        }

        ++video_packets_seen;
        const auto packet_sequence = ++video_sequence;

        // 选择首个已送出若干包后的非关键帧作为丢失包，避免把测试退化为
        // “首包就等待关键帧”。下一条仍是依赖帧时，Decoder 应继续丢弃。
        if (!gap_injected && video_packets_seen > 10 && !packet->keyframe) {
            assert(sink->WaitFor(1));
            frames_before_recovery = sink->Count();
            gap_injected = true;
            continue;
        }

        MediaPacketMessage message;
        message.packet = std::move(packet);
        message.stream_info = video_stream_info;
        message.generation = 1;
        message.sequence = packet_sequence;
        const bool is_keyframe = message.packet->keyframe;
        decoder->Input().Receive(std::move(message));

        if (gap_injected && is_keyframe) {
            recovery_keyframe_sent = true;
        }
    }

    assert(gap_injected);
    assert(recovery_keyframe_sent);
    // 必须看到恢复关键帧之后新增的解码帧，不能只凭关键帧已经送入
    // Decoder 就宣称恢复成功。
    assert(sink->WaitFor(frames_before_recovery + 1));
    assert(audio_sink->WaitFor(1));

    const auto recovery = decoder->RecoveryStats();
    assert(recovery.sequence_gaps >= 1);
    assert(recovery.dropped_until_keyframe >= 1);
    assert(recovery.recovered_keyframes >= 1);
    assert(!recovery.awaiting_keyframe);

    graph.Stop();
    puller.Close();
}

void TestSourceStopRequestIsNonBlocking() {
    auto puller = std::make_unique<BlockingStopPuller>();
    auto* puller_state = puller.get();
    StreamSourceNode source(
        "blocking-stop", std::move(puller), "test://blocking-stop");

    assert(source.Init());
    assert(source.Start());
    assert(puller_state->WaitUntilReadStarted());

    const auto begin = std::chrono::steady_clock::now();
    source.StopProduction();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - begin);

    // StopProduction 只请求中断，不应等待 Close 或读线程 join。
    assert(elapsed < std::chrono::milliseconds(100));
    assert(puller_state->RequestStopCount() == 1);
    assert(puller_state->WaitUntilReadReturned());
    const auto finished_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!source.IsProductionStopped() &&
           std::chrono::steady_clock::now() < finished_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(source.IsProductionStopped());

    source.Stop();
    assert(puller_state->CloseCalled());
}

void TestEncoderAndPublisherChain() {
    auto encoder_state = std::make_shared<EncoderState>();
    auto publisher_state = std::make_shared<PublisherState>();
    auto executor = std::make_shared<AsioExecutor>("encoder-publisher-test", 2);

    EncoderConfig encoder_config;
    encoder_config.media_type = MediaType::VIDEO;
    encoder_config.codec_type = CodecType::H264;
    encoder_config.video.width = 640;
    encoder_config.video.height = 360;
    encoder_config.video.pixel_format = PixelFormat::kI420;

    PublisherConfig publisher_config;
    publisher_config.url = "recording://publisher";
    publisher_config.protocol = PublishProtocol::FfmpegMux;

    Graph graph;
    assert(graph.AddNode<VideoFrameSource>("frames", executor, NodeOptions{}));
    assert(graph.AddNode<EncoderNode>(
        "encoder", executor, NodeOptions{},
        std::make_unique<RecordingEncoder>(encoder_state), encoder_config,
        EncoderNodeOptions{12}));
    assert(graph.AddNode<PublisherSinkNode>(
        "publisher", executor, NodeOptions{}, publisher_config,
        std::make_unique<RecordingPublisher>(publisher_state),
        PublisherSinkNodeOptions{true}));
    assert(graph.Connect<MediaFrameMessage>("frames", "encoder"));
    assert(graph.Connect<EncodedPacketMessage>("encoder", "publisher"));
    assert(graph.Start());

    // 第一帧的第一个包是非关键帧，第二个包为关键帧；首个非关键帧应被
    // PublisherSink 丢弃，关键帧启动 Publisher，后续包继续发送。
    assert(WaitForPublished(publisher_state, 1));
    auto encoder = graph.GetNode<EncoderNode>("encoder");
    auto publisher = graph.GetNode<PublisherSinkNode>("publisher");
    assert(encoder && publisher);
    assert(encoder->OutputInfo().fps == 25.0F);
    assert(encoder->Flush());
    assert(WaitForPublished(publisher_state, 2));

    {
        std::lock_guard<std::mutex> lock(publisher_state->mutex);
        assert(publisher_state->start_count == 1);
        assert(publisher_state->packets.size() == 2);
        assert(publisher_state->packets[0].keyframe);
        assert(publisher_state->packets[0].stream_index == 12);
        assert(publisher_state->packets[1].keyframe);
    }

    const auto configured = publisher->Config();
    assert(configured.tracks.size() == 1);
    assert(configured.tracks[0].track_id == 12);
    assert(configured.tracks[0].width == 640);
    assert(configured.tracks[0].height == 360);
    assert(configured.tracks[0].time_base_num == 1);
    assert(configured.tracks[0].time_base_den == 1000);
    assert(!configured.tracks[0].extra_data.empty());

    {
        std::lock_guard<std::mutex> lock(encoder_state->mutex);
        assert(encoder_state->open_count == 1);
        assert(encoder_state->encode_count == 1);
        assert(encoder_state->flush_count == 1);
    }

    graph.Stop();
}

void TestPublisherAwaitingKeyframeState() {
    auto encoder_state = std::make_shared<EncoderState>();
    auto publisher_state = std::make_shared<PublisherState>();
    publisher_state->await_next_publish = true;
    auto executor = std::make_shared<AsioExecutor>("publisher-keyframe-test", 2);

    EncoderConfig encoder_config;
    encoder_config.media_type = MediaType::VIDEO;
    encoder_config.codec_type = CodecType::H264;
    encoder_config.video.width = 640;
    encoder_config.video.height = 360;
    encoder_config.video.pixel_format = PixelFormat::kI420;

    PublisherConfig publisher_config;
    publisher_config.url = "recording://publisher";
    publisher_config.protocol = PublishProtocol::FfmpegMux;

    Graph graph;
    assert(graph.AddNode<VideoFrameSource>("frames", executor, NodeOptions{}));
    assert(graph.AddNode<EncoderNode>(
        "encoder", executor, NodeOptions{},
        std::make_unique<RecordingEncoder>(encoder_state), encoder_config,
        EncoderNodeOptions{12}));
    assert(graph.AddNode<PublisherSinkNode>(
        "publisher", executor, NodeOptions{}, publisher_config,
        std::make_unique<RecordingPublisher>(publisher_state),
        PublisherSinkNodeOptions{true}));
    assert(graph.Connect<MediaFrameMessage>("frames", "encoder"));
    assert(graph.Connect<EncodedPacketMessage>("encoder", "publisher"));
    assert(graph.Start());

    auto encoder = graph.GetNode<EncoderNode>("encoder");
    auto publisher = graph.GetNode<PublisherSinkNode>("publisher");
    assert(encoder && publisher);
    assert(WaitForPublishAttempts(publisher_state, 1));
    assert(encoder->Flush());
    assert(WaitForPublished(publisher_state, 1));

    {
        std::lock_guard<std::mutex> lock(publisher_state->mutex);
        // AwaitingKeyframe 不能让 PublisherSink 把会话标记为关闭；Flush
        // 产生的下一关键帧应沿用第一次 Start 的 publisher。
        assert(publisher_state->start_count == 1);
        assert(publisher_state->publish_attempts == 2);
        assert(publisher_state->packets.size() == 1);
        assert(publisher_state->packets.front().keyframe);
    }
    assert(publisher->LastResult().IsSuccess());
    graph.Stop();
}

void TestMultiTrackPublisherAndGracefulEos() {
    auto publisher_state = std::make_shared<PublisherState>();
    auto executor = std::make_shared<AsioExecutor>("publisher-multi-track-test", 2);

    EncodedTrackInfo video_info;
    video_info.media_type = MediaType::VIDEO;
    video_info.codec_type = CodecType::H264;
    video_info.time_base = Rational{1, 1000};
    video_info.width = 640;
    video_info.height = 360;
    video_info.fps = 25.0F;
    video_info.extra_data = {0x01, 0x64, 0x00, 0x1f};

    EncodedTrackInfo audio_info;
    audio_info.media_type = MediaType::AUDIO;
    audio_info.codec_type = CodecType::AAC;
    audio_info.time_base = Rational{1, 48000};
    audio_info.sample_rate = 48000;
    audio_info.channels = 2;

    PublisherConfig publisher_config;
    publisher_config.url = "recording://multi-track-publisher";
    publisher_config.protocol = PublishProtocol::FfmpegMux;
    publisher_config.tracks = {
        MediaTrackConfig{1, MediaType::VIDEO, CodecType::H264,
                         640, 360, 25.0F, 0, 0, 1, 1000,
                         video_info.extra_data},
        MediaTrackConfig{2, MediaType::AUDIO, CodecType::AAC,
                         0, 0, 0.0F, 48000, 2, 1, 48000, {}}
    };

    Graph graph;
    assert(graph.AddNode<EncodedPacketSource>(
        "video-source", executor, NodeOptions{}, 1, video_info));
    assert(graph.AddNode<EncodedPacketSource>(
        "audio-source", executor, NodeOptions{}, 2, audio_info));
    assert(graph.AddNode<PublisherSinkNode>(
        "publisher", executor, NodeOptions{}, publisher_config,
        std::make_unique<RecordingPublisher>(publisher_state),
        PublisherSinkNodeOptions{true, true, 16}));
    assert(graph.Connect<EncodedPacketMessage>("video-source", "publisher"));
    assert(graph.Connect<EncodedPacketMessage>("audio-source", "publisher"));
    assert(graph.Start());

    assert(WaitForPublished(publisher_state, 2));
    assert(WaitForPublisherStop(publisher_state));

    auto publisher = graph.GetNode<PublisherSinkNode>("publisher");
    assert(publisher);
    const auto configured = publisher->Config();
    assert(configured.tracks.size() == 2);
    assert(configured.tracks[0].track_id == 1);
    assert(configured.tracks[1].track_id == 2);

    assert(graph.GracefulStop(std::chrono::seconds(2)));
    assert(graph.GetState() == Graph::State::Stopped);
}

void TestPublisherInterleavesMultipleTracksByDts() {
    auto publisher_state = std::make_shared<PublisherState>();
    auto executor = std::make_shared<AsioExecutor>("publisher-dts-order-test", 2);

    EncodedTrackInfo video_info;
    video_info.media_type = MediaType::VIDEO;
    video_info.codec_type = CodecType::H264;
    video_info.time_base = Rational{1, 1000};
    video_info.width = 640;
    video_info.height = 360;
    video_info.fps = 25.0F;
    video_info.extra_data = {0x01, 0x64, 0x00, 0x1f};

    EncodedTrackInfo audio_info;
    audio_info.media_type = MediaType::AUDIO;
    audio_info.codec_type = CodecType::AAC;
    audio_info.time_base = Rational{1, 1000};
    audio_info.sample_rate = 48000;
    audio_info.channels = 2;

    PublisherConfig publisher_config;
    publisher_config.url = "recording://dts-order-publisher";
    publisher_config.protocol = PublishProtocol::FfmpegMux;
    publisher_config.tracks = {
        MediaTrackConfig{1, MediaType::VIDEO, CodecType::H264,
                         640, 360, 25.0F, 0, 0, 1, 1000,
                         video_info.extra_data},
        MediaTrackConfig{2, MediaType::AUDIO, CodecType::AAC,
                         0, 0, 0.0F, 48000, 2, 1, 1000, {}}
    };

    PublisherSinkNodeOptions options{true, true, 32};
    options.enable_dts_interleaving = true;
    options.dts_interleaver.max_pending_packets = 2;
    options.dts_interleaver.max_pending_span_us = 0;

    Graph graph;
    assert(graph.AddNode<DtsOrderPacketSource>(
        "video-source", executor, NodeOptions{}, 1, video_info,
        std::vector<std::pair<std::int64_t, std::int64_t>>{
            {0, 0}, {40, 20}}));
    assert(graph.AddNode<DtsOrderPacketSource>(
        "audio-source", executor, NodeOptions{}, 2, audio_info,
        std::vector<std::pair<std::int64_t, std::int64_t>>{
            {10, 10}, {30, 30}}));
    assert(graph.AddNode<PublisherSinkNode>(
        "publisher", executor, NodeOptions{}, publisher_config,
        std::make_unique<RecordingPublisher>(publisher_state), options));
    assert(graph.Connect<EncodedPacketMessage>("video-source", "publisher"));
    assert(graph.Connect<EncodedPacketMessage>("audio-source", "publisher"));
    assert(graph.Start());

    assert(WaitForPublished(publisher_state, 4));
    assert(WaitForPublisherStop(publisher_state));

    std::vector<std::int64_t> dts;
    {
        std::lock_guard<std::mutex> lock(publisher_state->mutex);
        for (const auto& packet : publisher_state->packets) {
            dts.push_back(packet.dts * 1000);
        }
    }
    // 视频关键帧首先作为发布锚点发送；其余包由交织器按微秒 DTS 输出。
    assert((dts == std::vector<std::int64_t>{0, 10'000, 20'000, 30'000}));
    graph.Stop();
}

} // namespace

int main() {
    TestMediaQueueItemCostTraits();
    TestMediaFlowPipelineConfig();
    TestPlaybackProfileBuildsPipelineConfig();
    TestSingleVideoChainAndReconnectGeneration();
    TestSourceTrackQueuesAreIndependent();
    TestDecoderRejectsIncompleteStreamInfo();
    TestFfmpegDecoderRecoversAfterVideoPacketGap();
    TestSourceStopRequestIsNonBlocking();
    TestEncoderAndPublisherChain();
    TestPublisherAwaitingKeyframeState();
    TestMultiTrackPublisherAndGracefulEos();
    TestPublisherInterleavesMultipleTracksByDts();
    return 0;
}
