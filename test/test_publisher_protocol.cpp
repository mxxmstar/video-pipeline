#include "media/protocol/h264_bitstream.h"
#include "media/protocol/h264_rtp_packetizer.h"
#include "media/publisher/i_publisher.h"

#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

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

    config.tracks.clear();
    assert(!config.IsValid());

    config = MakeRtspConfig();
    config.listen_port = 0;
    assert(!config.IsValid());
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

int main() {
    TestPublisherConfigValidation();
    TestAnnexBSplit();
    TestAvccExtradata();
    TestAvccSplit();
    TestFuAPacketization();

    auto publisher = IPublisher::Create(MakeRtspConfig());
    assert(publisher);
    return 0;
}
