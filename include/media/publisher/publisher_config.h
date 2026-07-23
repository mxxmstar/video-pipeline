#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "media/media_packet.h"



enum class PublishMode {
    PushClient, ///< 推送模式 client(发送到ZLM).
    PullServer, ///< 拉取模式 server(直接监听本机端口，等待客户端拉流).
};

enum class PublishProtocol {
    Auto,
    FfmpegMux,  ///< FFmpeg 复用器推送.
    RtspServer, ///< RTSP 服务器推送.
    RtpUdp,     ///< RTP UDP 推送.
    WebRtc, ///< WebRTC 推送.
};

struct MediaTrackConfig {
    int track_id{0};
    MediaType media_type{MediaType::VIDEO};
    CodecType codec_type{CodecType::H264};

    int width{0};
    int height{0};
    float fps{25.0f};
    int sample_rate{0};
    int channels{0};

    int time_base_num{1};
    int time_base_den{1000000};
    std::vector<std::uint8_t> extra_data;

    std::uint8_t rtp_payload_type{96};
    std::uint32_t rtp_clock_rate{90000};

    bool IsValid() const {
        if (codec_type == CodecType::UNKNOWN || time_base_num <= 0 ||
            time_base_den <= 0) {
            return false;
        }

        if (media_type == MediaType::VIDEO) {
            return width > 0 && height > 0;
        }

        if (media_type == MediaType::AUDIO) {
            return sample_rate > 0 && channels > 0;
        }

        return false;
    }
};

struct FfmpegPublishOptions {
    std::string format_name;
    std::string rtsp_transport{"tcp"};
};

struct RtspServerOptions {
    bool enable_tcp_interleaved{true};  ///< 是否开启 TCP 交错模式.
    bool enable_udp{false};             ///< 是否开启 UDP 模式.
    bool enable_multicast{false};       ///< 是否在 SDP 中宣告并接受 UDP 组播.
    // 当 SETUP 请求 RTP/AVP;multicast 但没有指定 destination、port 或 ttl 时，
    // 使用这里的默认组播目标。
    std::string multicast_address{"239.255.0.1"};
    std::uint16_t multicast_rtp_port{5004};
    std::uint16_t multicast_rtcp_port{5005};
    std::uint8_t multicast_ttl{16};
    int max_payload_size{1420};        ///< 最大 payload 大小.
};

struct PublisherConfig {
    PublishMode mode{PublishMode::PushClient};
    PublishProtocol protocol{PublishProtocol::Auto};

    // PushClient uses url directly. PullServer may also use an rtsp:// URL
    // as shorthand for listen host/port/path.
    std::string url;
    std::string listen_host{"0.0.0.0"};
    std::uint16_t listen_port{8554};
    std::string stream_path{"/live/main"};

    std::vector<MediaTrackConfig> tracks;

    FfmpegPublishOptions ffmpeg;
    RtspServerOptions rtsp;

    bool IsValid() const {
        if (protocol == PublishProtocol::RtpUdp) {
            return false;
        }

        if (mode == PublishMode::PushClient && url.empty()) {
            return false;
        }

        if (mode == PublishMode::PullServer &&
            (listen_port == 0 || stream_path.empty())) {
            return false;
        }

        if (tracks.empty()) {
            return false;
        }

        for (const auto& track : tracks) {
            if (!track.IsValid()) {
                return false;
            }
        }

        return true;
    }
};
