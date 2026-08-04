#include "media/protocol/rtsp/rtsp_builders.h"

#include <array>
#include <iomanip>
#include <sstream>

#include <boost/asio/ip/address.hpp>

std::string RtspResponseBuilder::Build(
    int status_code,
    const std::string& status_reason,
    const std::string& cseq,
    const std::map<std::string, std::string>& headers,
    const std::string& body,
    const std::string& content_type) {
    // RTSP/1.0 使用 CRLF 结束 status line 和每个 header；不能使用
    // ostream 的平台换行符，否则 Windows 下会产生不兼容的响应。
    std::ostringstream oss;
    oss << "RTSP/1.0 " << status_code << " " << status_reason << "\r\n";
    if (!cseq.empty()) {
        oss << "CSeq: " << cseq << "\r\n";
    }
    oss << "Server: video-pipeline/1.0\r\n";
    // headers 使用 std::map，保持与旧实现相同的确定性输出顺序，便于
    // characterization test 逐字节比较，同时不改变调用方传入的内容。
    for (const auto& [key, value] : headers) {
        oss << key << ": " << value << "\r\n";
    }
    if (!body.empty()) {
        oss << "Content-Type: " << content_type << "\r\n";
        // body.size() 是字符串字节数；RTSP Content-Length 不计算字符数。
        oss << "Content-Length: " << body.size() << "\r\n";
    } else {
        oss << "Content-Length: 0\r\n";
    }
    oss << "\r\n";
    if (!body.empty()) {
        oss << body;
    }
    return oss.str();
}

namespace {

bool IsIpv4Multicast(const boost::asio::ip::address& address) {
    if (!address.is_v4()) {
        return false;
    }

    const auto bytes = address.to_v4().to_bytes();
    return bytes[0] >= 224 && bytes[0] <= 239;
}

std::string ProfileLevelId(const std::vector<std::uint8_t>& sps) {
    if (sps.size() >= 4) {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0') << std::nouppercase
            << std::setw(2) << static_cast<int>(sps[1])
            << std::setw(2) << static_cast<int>(sps[2])
            << std::setw(2) << static_cast<int>(sps[3]);
        return oss.str();
    }
    return "42e01f";
}

int AacSamplingFrequencyIndex(int sample_rate) {
    constexpr std::array<int, 13> kRates{
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350,
    };
    for (std::size_t i = 0; i < kRates.size(); ++i) {
        if (kRates[i] == sample_rate) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string HexLower(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::nouppercase;
    for (const auto byte : bytes) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

std::string AacAudioSpecificConfigHex(const MediaTrackConfig& track) {
    if (!track.extra_data.empty()) {
        return HexLower(track.extra_data);
    }

    const auto sampling_index = AacSamplingFrequencyIndex(track.sample_rate);
    if (sampling_index < 0 || track.channels <= 0 || track.channels > 7) {
        return {};
    }

    // AAC-LC AudioSpecificConfig: audioObjectType=2, samplingFrequencyIndex,
    // channelConfiguration。这里仍保持旧实现的两字节构造规则。
    const auto config = static_cast<std::uint16_t>(
        (2U << 11U) |
        (static_cast<std::uint16_t>(sampling_index) << 7U) |
        (static_cast<std::uint16_t>(track.channels) << 3U));
    return HexLower({
        static_cast<std::uint8_t>((config >> 8) & 0xFF),
        static_cast<std::uint8_t>(config & 0xFF),
    });
}

} // namespace

std::string RtspSdpBuilder::Build(
    const PublisherConfig& config,
    const std::vector<MediaTrackConfig>& tracks,
    const H264ParameterSets& h264_parameter_sets,
    const std::string& host_for_sdp) {
    boost::system::error_code address_ec;
    const auto multicast_address =
        boost::asio::ip::make_address(config.rtsp.multicast_address, address_ec);
    const bool use_multicast_sdp =
        config.rtsp.enable_udp &&
        config.rtsp.enable_multicast &&
        !address_ec &&
        IsIpv4Multicast(multicast_address) &&
        config.rtsp.multicast_rtp_port != 0;
    const auto multicast_ttl = config.rtsp.multicast_ttl == 0
        ? std::uint8_t{16}
        : config.rtsp.multicast_ttl;

    // SDP builder 只读取快照；SETUP 是否真的创建 multicast socket 仍由
    // session/server 校验，避免把“描述可用”误当成“资源已创建”。
    std::ostringstream oss;
    oss << "v=0\r\n"
        << "o=- 0 1 IN IP4 " << host_for_sdp << "\r\n"
        << "s=video-pipeline\r\n"
        << "t=0 0\r\n"
        << "a=control:*\r\n";

    if (use_multicast_sdp) {
        oss << "c=IN IP4 " << multicast_address.to_string() << "/"
            << static_cast<int>(multicast_ttl) << "\r\n"
            << "a=type:broadcast\r\n";
    } else {
        oss << "c=IN IP4 0.0.0.0\r\n";
    }

    // 每条 m= 块都使用 track 自身的 payload type、clock rate 和 control URL，
    // 这样多轨会话不会因为遍历顺序而共享 codec 参数。
    for (const auto& track : tracks) {
        const auto media_name =
            track.media_type == MediaType::AUDIO ? "audio" : "video";
        const auto media_port =
            use_multicast_sdp && track.media_type == MediaType::VIDEO
                ? config.rtsp.multicast_rtp_port
                : 0;

        oss << "m=" << media_name << " " << media_port
            << " RTP/AVP " << static_cast<int>(track.rtp_payload_type) << "\r\n";

        if (track.codec_type == CodecType::H264) {
            oss << "a=rtpmap:" << static_cast<int>(track.rtp_payload_type)
                << " H264/" << track.rtp_clock_rate << "\r\n";
            oss << "a=fmtp:" << static_cast<int>(track.rtp_payload_type)
                << " packetization-mode=1;profile-level-id="
                << ProfileLevelId(h264_parameter_sets.sps);

            if (h264_parameter_sets.HasBoth()) {
                oss << ";sprop-parameter-sets="
                    << H264Bitstream::Base64Encode(h264_parameter_sets.sps)
                    << ","
                    << H264Bitstream::Base64Encode(h264_parameter_sets.pps);
            }
            oss << "\r\n";
        } else if (track.codec_type == CodecType::AAC) {
            // AAC 走 RFC 3640 MPEG4-GENERIC；config 是 AudioSpecificConfig 的十六进制。
            oss << "a=rtpmap:" << static_cast<int>(track.rtp_payload_type)
                << " MPEG4-GENERIC/" << track.rtp_clock_rate << "/"
                << track.channels << "\r\n";
            oss << "a=fmtp:" << static_cast<int>(track.rtp_payload_type)
                << " streamtype=5;profile-level-id=1;mode=AAC-hbr"
                << ";sizelength=13;indexlength=3;indexdeltalength=3";
            const auto config_hex = AacAudioSpecificConfigHex(track);
            if (!config_hex.empty()) {
                oss << ";config=" << config_hex;
            }
            oss << "\r\n";
        } else if (track.codec_type == CodecType::G711A) {
            oss << "a=rtpmap:" << static_cast<int>(track.rtp_payload_type)
                << " PCMA/" << track.rtp_clock_rate << "/"
                << track.channels << "\r\n";
        } else if (track.codec_type == CodecType::G711U) {
            oss << "a=rtpmap:" << static_cast<int>(track.rtp_payload_type)
                << " PCMU/" << track.rtp_clock_rate << "/"
                << track.channels << "\r\n";
        }

        oss << "a=control:track" << track.track_id << "\r\n";
    }
    return oss.str();
}
