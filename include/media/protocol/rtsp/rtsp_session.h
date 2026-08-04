#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>

#include "media/publisher/publisher_config.h"
#include "media/protocol/h264_rtp_packetizer.h"
#include "media/protocol/rtcp_packet_codec.h"
#include "media/protocol/rtp_sender.h"
#include "media/protocol/rtsp/rtsp_builders.h"
#include "media/protocol/rtsp/rtsp_connection.h"
#include "media/protocol/rtsp/rtsp_media_transport.h"
#include "media/protocol/rtsp/rtsp_request_parser.h"
#include "media/protocol/rtsp/rtsp_transport_spec.h"

namespace rtsp_session_detail {

inline std::uint32_t ReadU32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

inline std::string MakeSessionId(std::uint64_t id) {
    std::ostringstream oss;
    oss << std::hex << std::setw(8) << std::setfill('0') << id;
    return oss.str();
}

inline bool ExtractCSeq(const RtspRequest& request, std::string& cseq) {
    const auto values = request.HeaderValues("CSeq");
    if (values.size() != 1 || values.front().empty()) {
        return false;
    }
    if (!std::all_of(values.front().begin(), values.front().end(),
                     [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) {
        return false;
    }
    cseq.assign(values.front());
    return true;
}

inline bool ParseTrackIdFromUrl(const std::string& url, int& track_id) {
    const auto marker = url.rfind("track");
    if (marker == std::string::npos) {
        return false;
    }
    const auto digits_begin = marker + std::string{"track"}.size();
    if (digits_begin >= url.size() ||
        !std::isdigit(static_cast<unsigned char>(url[digits_begin]))) {
        return false;
    }
    int value = 0;
    for (std::size_t i = digits_begin; i < url.size(); ++i) {
        const auto ch = static_cast<unsigned char>(url[i]);
        if (!std::isdigit(ch)) {
            break;
        }
        value = value * 10 + static_cast<int>(ch - '0');
    }
    track_id = value;
    return true;
}

inline const MediaTrackConfig* FindTrackById(
    const std::vector<MediaTrackConfig>& tracks,
    int track_id) {
    const auto it = std::find_if(
        tracks.begin(),
        tracks.end(),
        [track_id](const auto& track) { return track.track_id == track_id; });
    return it == tracks.end() ? nullptr : &*it;
}

/// @brief 按精确 IP 地址判断新连接是否允许进入 session registry。
///
/// allowlist 为空时保持旧行为，允许所有客户端；配置非空时只比较
/// address().to_string() 的规范文本，不把 CIDR 或主机名偷偷解释成另一种语义。
inline bool IsClientAddressAllowed(
    const RtspServerOptions& options,
    const boost::asio::ip::tcp::endpoint& endpoint) {
    if (options.allowed_client_addresses.empty()) {
        return true;
    }
    const auto address = endpoint.address().to_string();
    return std::find(options.allowed_client_addresses.begin(),
                     options.allowed_client_addresses.end(),
                     address) != options.allowed_client_addresses.end();
}

} // namespace rtsp_session_detail

/// @brief 供单个 RTSP 客户端使用的窄化上下文。
///
/// 上下文只保存规范化后的只读配置、Asio executor 和 server 提供的功能回调；
/// session 不持有 RtspServerProtocol 引用，因此状态机可以独立测试和迁移。
struct RtspSessionContext {
    PublisherConfig config;
    std::vector<MediaTrackConfig> tracks;
    boost::asio::any_io_executor io_executor;
    std::function<std::string(const std::string&)> build_sdp;
    std::function<bool(RtspTransportSpec&, const std::string&)> configure_multicast;
    std::function<std::uint16_t()> get_multicast_sequence;
    std::function<std::uint32_t()> get_multicast_last_rtp_timestamp;
    std::function<void(std::uint64_t)> add_receiver_reports;
    // manager 提供的窄化回调只记录鉴权失败和是否触发地址限流；session 不
    // 直接持有 manager，避免形成 owner/session/manager 循环引用。
    std::function<bool(const std::string&)> record_auth_failure;
};

/// @brief `RtspSessionManager` 的只读连接统计快照。
///
/// 计数均为当前 protocol 生命周期内的累计值；active_connections 是当前
/// registry 中的活动 session 数，调用方拿到副本后无需继续持有 manager 锁。
struct RtspSessionManagerStats {
    std::uint64_t active_connections{0};
    std::uint64_t connections_accepted{0};
    std::uint64_t connections_closed{0};
    std::uint64_t connections_rejected{0};
    std::uint64_t connections_rejected_by_capacity{0};
    std::uint64_t connections_rejected_by_address{0};
    std::uint64_t connections_rejected_by_rate_limit{0};
    std::uint64_t auth_failures{0};
    std::uint64_t auth_failures_rejected{0};
};

/// @brief RTSP 会话
/// @note 管理单个客户端连接的核心类，负责处理一个客户端从连接到断开的完整生命周期。
class RtspClientSession
    : public std::enable_shared_from_this<RtspClientSession> {
public:
    using ClosedHandler = std::function<void(std::uint64_t)>;

    static std::shared_ptr<RtspClientSession> Create(
        boost::asio::ip::tcp::socket socket,
        std::shared_ptr<const RtspSessionContext> context,
        std::uint64_t id,
        std::string client_address,
        ClosedHandler on_closed);


    void Start();


    void Stop();


    bool IsPlaying() const;


    bool IsMulticastTransport() const;


    bool IsPlayingMulticast() const;


    bool IsPlayingMulticastTrack(int track_id) const;


    std::uint64_t Id() const;


    void SendRtpPayload(int track_id,
                        const RtpPayload& payload,
                        std::uint8_t payload_type);


private:
    RtspClientSession(std::shared_ptr<const RtspSessionContext> context,
                      std::uint64_t id,
                      std::string client_address,
                      ClosedHandler on_closed);


    // 每个 SDP track 都有独立的 sender/transport 状态。音频和视频不能共用
    // SSRC、sequence 或 timestamp，否则播放器会把不同媒体流混到同一条 RTP 时间线里。
    struct TrackSessionState {
        int track_id{0};
        const MediaTrackConfig* track{nullptr};
        RtspTransportSpec transport_spec;
        // unicast/TCP 的 socket 和异步 receive 归 transport 所有；multicast
        // 这里只记录共享 publisher 返回的订阅描述，不创建客户端 socket。
        std::shared_ptr<IRtspMediaTransport> media_transport;
        std::optional<RtspMulticastSubscription> multicast_subscription;
        std::unique_ptr<RtpSender> sender;
        RtcpReportBlock last_receiver_report{};
        std::uint32_t last_receiver_reporter_ssrc{0};
        std::uint64_t receiver_reports_received{0};
        int rtp_channel{0};
        int rtcp_channel{1};
        bool has_receiver_report{false};
        bool ready{false};
    };

    void HandleInterleavedFrame(
        std::uint8_t channel,
        std::vector<std::uint8_t> payload);


    void HandleConnectionParseError(
        const RtspRequestParseResult& parse_result);


    void OnConnectionClosed(const boost::system::error_code&);


    void HandleRequest(const RtspRequest& request);


    TrackSessionState* FindTrackState(int track_id);


    const TrackSessionState* FindTrackState(int track_id) const;


    TrackSessionState* FindTrackStateByRtcpChannel(std::uint8_t channel);


    const MediaTrackConfig* FindTrackForSetupUrl(const std::string& url) const;


    TrackSessionState& PrepareTrackState(const MediaTrackConfig& track);


    void FinishTrackSetup(TrackSessionState& state,
                          const RtspTransportSpec& transport_spec);


    bool HasReadyTrack() const;


    std::string BuildTrackUrl(const std::string& play_url, int track_id) const;


    std::string BuildRtpInfo(const std::string& play_url) const;


    void MaybeSendRtcpSenderReport(TrackSessionState& state);


    void HandleRtcpPacket(TrackSessionState& state,
                          const std::uint8_t* data,
                          std::size_t size,
                          const char* transport_name);


    void ParseRtcpReceiverReport(TrackSessionState& state,
                                 const std::uint8_t* packet,
                                 std::size_t packet_size,
                                 std::uint8_t report_count,
                                 const char* transport_name);


    void ParseRtcpSenderReport(TrackSessionState& state,
                               const std::uint8_t* packet,
                               std::size_t packet_size,
                               std::uint8_t report_count,
                               const char* transport_name);


    void ParseRtcpReportBlocks(TrackSessionState& state,
                               const std::uint8_t* blocks,
                               std::uint8_t report_count,
                               std::uint32_t reporter_ssrc,
                               const char* transport_name,
                               const char* packet_type_name);


    bool CreateMediaTransport(TrackSessionState& state,
                              RtspTransportSpec& transport_spec);


    void SendRtsp(std::string response);


    std::string LocalAddress() const;


    void CloseTransport(TrackSessionState& state);


    void CloseAllTransports();


    // 所有 session 控制状态都在 connection 所属 executor 上修改；timer 只
    // 负责唤醒同一个 executor，不需要额外给 track_states_ 加锁。
    void TouchActivity();


    void OnIdleTimeout();


    bool AuthenticateRequest(const RtspRequest& request,
                             const std::string& cseq);


    bool ValidateDigestAuthorization(const RtspRequest& request,
                                     std::string_view authorization,
                                     bool& stale_nonce);


    void SendUnauthorized(const std::string& cseq, bool stale_nonce);


    std::string EnsureDigestNonce();


    void Close();


    std::shared_ptr<const RtspSessionContext> context_;
    boost::asio::steady_timer idle_timer_;
    ClosedHandler on_closed_;
    std::shared_ptr<RtspConnection> connection_;
    std::uint64_t id_{0};
    std::string client_address_;
    std::string session_id_;
    std::string requested_url_;
    // nonce 只在当前 session 内复用，且由创建时间控制有效期；这样过期后
    // 不接受旧 Digest response，也不会把 nonce 状态放到 façade 全局共享。
    std::string auth_nonce_;
    std::chrono::steady_clock::time_point auth_nonce_created_at_{};
    std::uint32_t digest_nonce_count_{0};
    // key 对应 MediaTrackConfig::track_id，也就是 SDP/SETUP 中的 trackN。
    std::map<int, TrackSessionState> track_states_;
    bool ready_{false};
    std::atomic_bool playing_{false};
    bool closed_{false};
};



class RtspSessionManager
    : public std::enable_shared_from_this<RtspSessionManager> {
public:
    using Session = std::shared_ptr<RtspClientSession>;

    explicit RtspSessionManager(
        std::shared_ptr<const RtspSessionContext> context);

    RtspSessionManager(const RtspSessionManager&) = delete;
    RtspSessionManager& operator=(const RtspSessionManager&) = delete;

    /// 在 manager 锁内分配唯一内部 id；session 的连接回调只通过
    /// manager 的窄化 Remove() 回收 registry 项。
    Session Create(boost::asio::ip::tcp::socket socket);

    void Remove(std::uint64_t session_id);

    std::vector<Session> Snapshot() const;

    std::size_t Size() const;

    /// 返回连接生命周期和限流拒绝原因的只读快照。
    RtspSessionManagerStats GetStats() const;

    /// 记录一次鉴权失败；返回 false 表示该地址达到失败阈值，session 应
    /// 在写完当前 401 challenge 后关闭，避免继续占用连接资源。
    bool RecordAuthFailure(const std::string& client_address);

    void Clear();

private:
    struct AddressState {
        std::chrono::steady_clock::time_point window_started{};
        std::size_t connection_attempts{0};
        std::size_t auth_failures{0};
        std::size_t active_connections{0};
    };

    void ResetAddressWindow(AddressState& state,
                            std::chrono::steady_clock::time_point now) const;

    std::shared_ptr<const RtspSessionContext> context_;
    mutable std::mutex mutex_;
    std::vector<Session> sessions_;
    std::unordered_map<std::string, AddressState> address_states_;
    std::unordered_map<std::uint64_t, std::string> session_addresses_;
    std::size_t pending_connections_{0};
    std::uint64_t next_session_id_{1};
    RtspSessionManagerStats stats_;
};
