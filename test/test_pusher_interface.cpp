#include "media/pusher/ffmpeg_protocol_adapter.h"
#include "media/pusher/ffmpeg_pusher.h"

#include "common/log/logger.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

class VectorMediaBuffer : public IMediaBuffer {
public:
    explicit VectorMediaBuffer(std::vector<std::uint8_t> data)
        : data_(std::move(data)) {}

    std::uint8_t* Data() override { return data_.data(); }
    const std::uint8_t* Data() const override { return data_.data(); }
    size_t Size() const override { return data_.size(); }

private:
    std::vector<std::uint8_t> data_;
};

class FakeProtocolAdapter : public IProtocolAdapter {
public:
    bool Connect(const PusherConfig& config) override {
        ++connect_calls;
        last_config = config;
        return connect_result;
    }

    bool Send(const MediaPacket& pkt) override {
        ++send_calls;
        last_packet = pkt;
        return send_result;
    }

    bool Close() override {
        ++close_calls;
        return close_result;
    }

    bool connect_result{true};
    bool send_result{true};
    bool close_result{true};
    int connect_calls{0};
    int send_calls{0};
    int close_calls{0};
    PusherConfig last_config;
    MediaPacket last_packet;
};

static PusherConfig MakeConfig() {
    PusherConfig config;
    config.url = "rtsp://127.0.0.1/live/test";
    config.codec_type = CodecType::H264;
    config.media_type = MediaType::VIDEO;
    config.width = 640;
    config.height = 360;
    return config;
}

static MediaPacket MakePacket() {
    MediaPacket pkt;
    pkt.type = MediaType::VIDEO;
    pkt.codec = CodecType::H264;
    pkt.pts = 10;
    pkt.dts = 9;
    pkt.keyframe = true;
    pkt.buffer = std::make_shared<VectorMediaBuffer>(
        std::vector<std::uint8_t>{0x00, 0x00, 0x01, 0x65});
    return pkt;
}

static void TestConfigValidation() {
    auto config = MakeConfig();
    assert(config.IsValid());

    config.url.clear();
    assert(!config.IsValid());

    config = MakeConfig();
    config.width = 0;
    assert(!config.IsValid());
}

static void TestPusherDelegatesLifecycleAndSend() {
    auto adapter = std::make_unique<FakeProtocolAdapter>();
    auto* fake = adapter.get();

    FFmpegPusher pusher(MakeConfig(), std::move(adapter));
    assert(!pusher.Send(MakePacket()));
    assert(fake->send_calls == 0);

    assert(pusher.Connect());
    assert(fake->connect_calls == 1);
    assert(fake->last_config.url == "rtsp://127.0.0.1/live/test");

    assert(pusher.Send(MakePacket()));
    assert(fake->send_calls == 1);
    assert(fake->last_packet.codec == CodecType::H264);
    assert(fake->last_packet.buffer);

    assert(pusher.Close());
    assert(fake->close_calls == 1);
}

static void TestCodecMapping() {
    assert(FFmpegProtocolAdapter::MapCodecType(CodecType::H264) == AV_CODEC_ID_H264);
    assert(FFmpegProtocolAdapter::MapCodecType(CodecType::H265) == AV_CODEC_ID_HEVC);
    assert(FFmpegProtocolAdapter::MapCodecType(CodecType::AAC) == AV_CODEC_ID_AAC);
    assert(FFmpegProtocolAdapter::MapCodecType(CodecType::UNKNOWN) == AV_CODEC_ID_NONE);
}

int main() {
    ai_camera::log::Init("video_pipeline_pusher_test");

    TestConfigValidation();
    TestPusherDelegatesLifecycleAndSend();
    TestCodecMapping();

    ai_camera::log::Shutdown();
    return 0;
}
