#include "mediaflow/mediaflow.h"

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

    std::string Name() const override {
        return "frame-sink";
    }

protected:
    void Process(MediaFrameMessage message) override {
        assert(message.Valid());
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

} // namespace

int main() {
    TestSingleVideoChainAndReconnectGeneration();
    return 0;
}
