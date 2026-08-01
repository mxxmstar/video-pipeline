#include "media/protocol/h264_bitstream.h"
#include "media/protocol/h264_rtp_packetizer.h"
#include "media/protocol/ffmpeg_protocol_adapter.h"
#include "media/protocol/rtsp_transport_spec.h"
#include "media/simple_buffer.h"
#include "media/publisher/i_publisher.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class RecordingProtocol : public IProtocol {
public:
    PublisherResult Start(const PublisherConfig& config,
                          const std::vector<MediaTrackConfig>& tracks) override {
        // 这个替身不创建真实网络连接，只记录 protocol 收到的配置和轨道，
        // 用于把 adapter 的 track 路由测试与 FFmpeg/RTSP 环境解耦。
        config_ = config;
        tracks_ = tracks;
        started_ = true;
        return PublisherResult::Success();
    }

    PublisherResult Write(const EncodedAccessUnit& access_unit) override {
        if (!started_) {
            return PublisherResult::Failure(
                PublisherErrorCode::InvalidState,
                "recording protocol is not started");
        }
        // 保存最后一个 access unit，测试可以据此检查 adapter 是否正确补齐了
        // track_id、媒体类型和 codec 类型。
        last_access_unit_ = access_unit;
        ++write_count;
        return PublisherResult::Success();
    }

    void Stop() override {
        started_ = false;
    }

    std::string GetOutputUrl() const override {
        return config_.url;
    }

    PublisherStats GetStats() const override {
        return {};
    }

    PublisherConfig config_;
    std::vector<MediaTrackConfig> tracks_;
    EncodedAccessUnit last_access_unit_;
    int write_count{0};

private:
    bool started_{false};
};

static PublisherConfig MakeRtspConfig() {
    PublisherConfig config;
    config.mode = PublishMode::PullServer;
    config.protocol = PublishProtocol::RtspServer;
    config.listen_host = "127.0.0.1";
    config.listen_port = 8554;
    config.stream_path = "/live/main";

    MediaTrackConfig track;
    track.track_id = 0;
    track.media_type = MediaType::VIDEO;
    track.codec_type = CodecType::H264;
    track.width = 1280;
    track.height = 720;
    track.fps = 25.0f;
    track.extra_data = {
        0x01, 0x64, 0x00, 0x1f, 0xff,
        0xe1, 0x00, 0x04, 0x67, 0x64, 0x00, 0x1f,
        0x01, 0x00, 0x04, 0x68, 0xee, 0x3c, 0x80,
    };
    config.tracks.push_back(track);
    return config;
}

static void TestPublisherConfigValidation() {
    auto config = MakeRtspConfig();
    assert(config.IsValid());
    assert(config.Validate().code == PublisherErrorCode::None);

    config.tracks.clear();
    assert(!config.IsValid());
    assert(config.Validate().code == PublisherErrorCode::InvalidConfiguration);

    config = MakeRtspConfig();
    config.listen_port = 0;
    assert(!config.IsValid());

    config = MakeRtspConfig();
    config.listen_host = "not-an-ip-address";
    assert(config.Validate().code == PublisherErrorCode::InvalidConfiguration);

    config = MakeRtspConfig();
    config.mode = PublishMode::PushClient;
    config.url = "rtsp://127.0.0.1/live/main";
    assert(config.Validate().code == PublisherErrorCode::InvalidConfiguration);

    config = MakeRtspConfig();
    config.protocol = PublishProtocol::RtpUdp;
    assert(config.Validate().code == PublisherErrorCode::UnsupportedProtocol);

    config.protocol = PublishProtocol::WebRtc;
    assert(config.Validate().code == PublisherErrorCode::UnsupportedProtocol);

    config = MakeRtspConfig();
    auto second_track = config.tracks.front();
    second_track.rtp_payload_type = 97;
    config.tracks.push_back(second_track);
    assert(config.Validate().code == PublisherErrorCode::InvalidConfiguration);

    config = MakeRtspConfig();
    second_track = config.tracks.front();
    second_track.track_id = 1;
    config.tracks.push_back(second_track);
    assert(config.Validate().code == PublisherErrorCode::InvalidConfiguration);

    config = MakeRtspConfig();
    config.rtsp.enable_multicast = true;
    assert(config.Validate().code == PublisherErrorCode::InvalidConfiguration);

    config.rtsp.enable_udp = true;
    config.rtsp.multicast_rtcp_port = config.rtsp.multicast_rtp_port + 2;
    assert(config.Validate().code == PublisherErrorCode::InvalidConfiguration);

    config.rtsp.multicast_rtp_port = 5005;
    config.rtsp.multicast_rtcp_port = 5006;
    assert(config.Validate().code == PublisherErrorCode::InvalidConfiguration);

    config.rtsp.multicast_rtp_port = 5004;
    config.rtsp.multicast_rtcp_port = 5005;
    config.rtsp.multicast_address = "127.0.0.1";
    assert(config.Validate().code == PublisherErrorCode::InvalidConfiguration);

    config = MakeRtspConfig();
    config.tracks.front().codec_type = CodecType::H265;
    assert(config.Validate().code == PublisherErrorCode::UnsupportedCodec);

    config = MakeRtspConfig();
    config.mode = PublishMode::PushClient;
    config.protocol = PublishProtocol::FfmpegMux;
    config.url = "rtsp://127.0.0.1:8554/live/main";
    assert(config.Validate().code == PublisherErrorCode::None);

    config.ffmpeg.io_timeout_ms = -1;
    assert(config.Validate().code == PublisherErrorCode::InvalidConfiguration);

    config.ffmpeg.io_timeout_ms = 5000;
    config.ffmpeg.bitstream_filters.emplace(99, "h264_mp4toannexb");
    assert(config.Validate().code == PublisherErrorCode::InvalidConfiguration);
}

static void TestPublisherStructuredResults() {
    auto invalid_config = MakeRtspConfig();
    invalid_config.tracks.clear();
    auto invalid_publisher = IPublisher::Create(invalid_config);
    assert(invalid_publisher);

    const auto start_result = invalid_publisher->Start();
    assert(start_result.code == PublisherErrorCode::InvalidConfiguration);
    assert(!start_result.message.empty());
    assert(invalid_publisher->GetLastResult().code == start_result.code);

    auto publisher = IPublisher::Create(MakeRtspConfig());
    assert(publisher);
    MediaPacket packet;
    const auto publish_result = publisher->Publish(packet);
    assert(publish_result.code == PublisherErrorCode::InvalidState);
    assert(!publish_result.message.empty());
}

static void TestFfmpegTrackMapping() {
    // 故意让配置中的 video/audio track_id 使用 10/20，而 packet.stream_index
    // 使用 0。这样可以验证 stream_index 无法命中时，adapter 能按媒体类型和
    // codec 唯一回退到正确的 audio track，而不是把 packet 错写到 video。
    PublisherConfig config;
    config.mode = PublishMode::PushClient;
    config.protocol = PublishProtocol::FfmpegMux;
    config.url = "rtsp://127.0.0.1:8554/live/test";

    MediaTrackConfig video_track;
    video_track.track_id = 10;
    video_track.media_type = MediaType::VIDEO;
    video_track.codec_type = CodecType::H264;
    video_track.width = 640;
    video_track.height = 360;

    MediaTrackConfig audio_track;
    audio_track.track_id = 20;
    audio_track.media_type = MediaType::AUDIO;
    audio_track.codec_type = CodecType::AAC;
    audio_track.sample_rate = 48000;
    audio_track.channels = 2;
    config.tracks = {video_track, audio_track};

    auto protocol = std::make_unique<RecordingProtocol>();
    auto* protocol_view = protocol.get();
    FfmpegProtocolAdapter adapter(std::move(protocol));
    assert(adapter.Open(config));

    MediaPacket packet;
    packet.type = MediaType::AUDIO;
    packet.codec = CodecType::AAC;
    packet.stream_index = 0;
    packet.buffer = std::make_shared<SimpleBuffer>(
        std::vector<std::uint8_t>{0x01, 0x02, 0x03});

    // 由于只有一个 AAC audio 候选，这个 packet 应该被唯一映射到 track 20。
    assert(adapter.Send(packet));
    assert(protocol_view->write_count == 1);
    assert(protocol_view->last_access_unit_.track_id == 20);
    assert(protocol_view->last_access_unit_.media_type == MediaType::AUDIO);
    assert(protocol_view->last_access_unit_.codec_type == CodecType::AAC);
}

static void TestAnnexBSplit() {
    const std::vector<std::uint8_t> data{
        0x00, 0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1f,
        0x00, 0x00, 0x01, 0x68, 0xee, 0x3c, 0x80,
        0x00, 0x00, 0x00, 0x01, 0x65, 0x88,
    };

    const auto nals = H264Bitstream::SplitPacket(data.data(), data.size());
    assert(nals.size() == 3);
    assert(nals[0].H264Type() == 7);
    assert(nals[1].H264Type() == 8);
    assert(nals[2].H264Type() == 5);
}

static void TestAvccExtradata() {
    const auto config = MakeRtspConfig();
    const auto sets = H264Bitstream::ExtractParameterSets(config.tracks[0].extra_data);
    assert(sets.sps.size() == 4);
    assert(sets.pps.size() == 4);
    assert(H264Bitstream::ParseAvccLengthSize(config.tracks[0].extra_data) == 4);
    assert(H264Bitstream::Base64Encode(sets.sps) == "Z2QAHw==");
}

static void TestAvccSplit() {
    const std::vector<std::uint8_t> data{
        0x00, 0x00, 0x00, 0x02, 0x65, 0x88,
        0x00, 0x00, 0x00, 0x02, 0x41, 0x9a,
    };

    const auto nals = H264Bitstream::SplitPacket(data.data(), data.size(), 4);
    assert(nals.size() == 2);
    assert(nals[0].H264Type() == 5);
    assert(nals[1].H264Type() == 1);
}

static void TestFuAPacketization() {
    EncodedAccessUnit access_unit;
    access_unit.codec_type = CodecType::H264;
    access_unit.pts = 9000;
    NalUnit nal;
    nal.data.resize(11);
    nal.data[0] = 0x65;
    for (std::size_t i = 1; i < nal.data.size(); ++i) {
        nal.data[i] = static_cast<std::uint8_t>(i);
    }
    access_unit.nals.push_back(std::move(nal));

    H264RtpPacketizer packetizer(6);
    const auto packets = packetizer.Packetize(access_unit);
    assert(packets.size() == 3);
    assert(packets.front().payload[0] == 0x7c);
    assert((packets.front().payload[1] & 0x80) != 0);
    assert((packets.back().payload[1] & 0x40) != 0);
    assert(packets.back().marker);
}

static void TestRtspTransportSpecTcp() {
    RtspTransportSpec spec;
    std::string error;
    assert(RtspTransportSpec::Parse(
        "RTP/AVP/TCP;unicast;interleaved=2-3",
        spec,
        &error));
    assert(spec.mode == RtspTransportMode::TcpInterleaved);
    assert(spec.rtp_channel == 2);
    assert(spec.rtcp_channel == 3);
    assert(spec.ToSetupResponseHeader() ==
           "RTP/AVP/TCP;unicast;interleaved=2-3");

    assert(RtspTransportSpec::Parse("rtp/avp/tcp;Interleaved=0-1", spec));
    assert(spec.rtp_channel == 0);
    assert(spec.rtcp_channel == 1);

    assert(RtspTransportSpec::Parse("RTP/AVP/TCP;unicast", spec));
    assert(spec.rtp_channel == 0);
    assert(spec.rtcp_channel == 1);
}

static void TestRtspTransportSpecUdp() {
    RtspTransportSpec spec;
    assert(RtspTransportSpec::Parse(
        "RTP/AVP;unicast;client_port=5004-5005",
        spec));
    assert(spec.mode == RtspTransportMode::UdpUnicast);
    assert(spec.client_rtp_port == 5004);
    assert(spec.client_rtcp_port == 5005);
    assert(spec.ToSetupResponseHeader() ==
           "RTP/AVP;unicast;client_port=5004-5005");

    spec.server_rtp_port = 6000;
    spec.server_rtcp_port = 6001;
    assert(spec.ToSetupResponseHeader() ==
           "RTP/AVP;unicast;client_port=5004-5005;server_port=6000-6001");
}

static void TestRtspTransportSpecMulticast() {
    RtspTransportSpec spec;
    assert(RtspTransportSpec::Parse(
        "RTP/AVP;multicast;destination=239.255.0.1;port=5004-5005;ttl=16",
        spec));
    assert(spec.mode == RtspTransportMode::UdpMulticast);
    assert(spec.destination == "239.255.0.1");
    assert(spec.server_rtp_port == 5004);
    assert(spec.server_rtcp_port == 5005);
    assert(spec.ttl == 16);
    assert(spec.ToSetupResponseHeader() ==
           "RTP/AVP;multicast;destination=239.255.0.1;port=5004-5005;ttl=16");

    assert(RtspTransportSpec::Parse("RTP/AVP;multicast", spec));
    assert(spec.mode == RtspTransportMode::UdpMulticast);
    assert(spec.destination.empty());
    assert(spec.server_rtp_port == 0);
    assert(spec.ttl == 0);
}

static void TestRtspTransportSpecAlternatives() {
    RtspTransportSpec spec;
    assert(RtspTransportSpec::Parse(
        "unsupported;foo=bar, RTP/AVP/TCP;unicast;interleaved=4-5",
        spec));
    assert(spec.mode == RtspTransportMode::TcpInterleaved);
    assert(spec.rtp_channel == 4);
    assert(spec.rtcp_channel == 5);
}

static void TestRtspTransportSpecInvalid() {
    RtspTransportSpec spec;
    std::string error;
    assert(!RtspTransportSpec::Parse("RTP/AVP;unicast", spec, &error));
    assert(!error.empty());

    assert(!RtspTransportSpec::Parse(
        "RTP/AVP/TCP;unicast;interleaved=abc-1",
        spec,
        &error));
    assert(!error.empty());
}

int main() {
    TestPublisherConfigValidation();
    TestPublisherStructuredResults();
    TestFfmpegTrackMapping();
    TestAnnexBSplit();
    TestAvccExtradata();
    TestAvccSplit();
    TestFuAPacketization();
    TestRtspTransportSpecTcp();
    TestRtspTransportSpecUdp();
    TestRtspTransportSpecMulticast();
    TestRtspTransportSpecAlternatives();
    TestRtspTransportSpecInvalid();

    auto publisher = IPublisher::Create(MakeRtspConfig());
    assert(publisher);
    return 0;
}
