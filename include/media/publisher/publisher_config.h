#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "media/media_packet.h"
#include "media/publisher/publisher_result.h"



enum class PublishMode {
    PushClient, ///< 推送模式 client(发送到ZLM).
    PullServer, ///< 拉取模式 server(直接监听本机端口，等待客户端拉流).
};

enum class PublishProtocol {
    Auto,
    FfmpegMux,  ///< FFmpeg 复用器推送.
    RtspServer, ///< RTSP 服务器推送.
    RtpUdp,     ///< RTP UDP 推送.
    WebRtc,     ///< WebRTC 推送.
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

    /// @brief 只校验与协议无关的轨道结构和媒体参数。
    PublisherResult ValidateStructure() const;

    bool IsValid() const {
        return ValidateStructure().IsSuccess();
    }
};

struct FfmpegPublishOptions {
    // 显式指定 FFmpeg 输出 muxer 的名称。例如 RTSP 推流通常使用 "rtsp"。
    // 为空时由 FfmpegMuxProtocol 根据 URL 推断；RTSP URL 会默认选择 rtsp
    // muxer，其它 URL 则交给 libavformat 按协议/扩展名选择。
    std::string format_name;

    // RTSP 输出的底层传输方式，例如 "tcp" 或 "udp"。该选项只会在
    // format_name/URL 解析为 RTSP 输出时传给 FFmpeg。
    std::string rtsp_transport{"tcp"};

    // 单次 FFmpeg I/O 操作的超时时间，单位为毫秒。它覆盖连接、写 header、
    // 写 packet、写 trailer 和关闭输出等操作；设置为 0 表示不主动设置超时。
    int io_timeout_ms{5000};

    // packet 写入发生 RuntimeDisconnected 后最多重新建立多少次输出会话。
    // 0 表示不自动重连，调用方会直接收到写入失败结果。
    int reconnect_attempts{3};

    // 两次重连尝试之间的固定等待时间，单位为毫秒。当前实现是固定退避，
    // 还没有指数退避或主备 URL 切换能力。
    int reconnect_backoff_ms{200};

    // 按 Publisher track_id 指定 FFmpeg bitstream filter。例如：
    // { {0, "h264_mp4toannexb"} }。
    // filter 默认不启用，只有确认输入 packet 格式与目标 muxer 不匹配时才配置，
    // 避免对已经满足目标格式的 packet 进行重复转换。
    std::unordered_map<int, std::string> bitstream_filters;
};

struct RtspServerOptions {
    bool enable_tcp_interleaved{true};  ///< 是否开启 TCP 交错模式.
    bool enable_udp{false};             ///< 是否开启 UDP 模式.
    bool enable_multicast{false};       ///< 是否在 SDP 中宣告并接受 UDP 组播.
    // 控制面闲置超时时间，单位为毫秒。0 表示关闭超时；播放中的 session
    // 会在收到 RTSP/interleaved/媒体活动时刷新计时器，避免正常推流被误关。
    int session_idle_timeout_ms{0};
    // 允许建立 RTSP session 的客户端 IP 精确列表。空列表表示允许所有地址；
    // 当前不解析 CIDR，避免把配置中的网段语义误判成单个客户端地址。
    std::vector<std::string> allowed_client_addresses;
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

    // PushClient 使用 url；PullServer 使用 listen_host/listen_port/stream_path。
    std::string url;
    std::string listen_host{"0.0.0.0"};
    std::uint16_t listen_port{8554};
    std::string stream_path{"/live/main"};

    std::vector<MediaTrackConfig> tracks;

    FfmpegPublishOptions ffmpeg;
    RtspServerOptions rtsp;

    /// @brief 校验与具体发布协议无关的配置结构。
    PublisherResult ValidateStructure() const;

    /// @brief 校验解析后的协议能力、角色和协议专用配置。
    PublisherResult ValidateProtocol() const;

    /// @brief 依次执行结构校验和协议能力校验。
    PublisherResult Validate() const;

    bool IsValid() const {
        return Validate().IsSuccess();
    }
};

/// @brief 将 Auto 解析成当前发布角色对应的实际协议。
PublishProtocol ResolvePublishProtocol(const PublisherConfig& config);
