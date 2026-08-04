#include "media/protocol/rtsp/rtsp_session.h"

#include "common/log/logger.h"

#include <array>
#include <cctype>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <string_view>

#include <boost/uuid/detail/md5.hpp>

namespace {

bool AsciiEqualsIgnoreCase(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto lower = [](unsigned char ch) {
            return ch >= 'A' && ch <= 'Z'
                ? static_cast<unsigned char>(ch + ('a' - 'A'))
                : ch;
        };
        if (lower(static_cast<unsigned char>(lhs[i])) !=
            lower(static_cast<unsigned char>(rhs[i]))) {
            return false;
        }
    }
    return true;
}

std::string Md5Hex(std::string_view value) {
    boost::uuids::detail::md5 hash;
    hash.process_bytes(value.data(), value.size());
    boost::uuids::detail::md5::digest_type digest{};
    hash.get_digest(digest);

    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

bool ConstantTimeEquals(std::string_view lhs, std::string_view rhs) {
    const auto size = std::max(lhs.size(), rhs.size());
    unsigned char difference = static_cast<unsigned char>(lhs.size() ^ rhs.size());
    for (std::size_t i = 0; i < size; ++i) {
        const auto left = i < lhs.size()
            ? static_cast<unsigned char>(lhs[i])
            : static_cast<unsigned char>(0);
        const auto right = i < rhs.size()
            ? static_cast<unsigned char>(rhs[i])
            : static_cast<unsigned char>(0);
        difference = static_cast<unsigned char>(difference | (left ^ right));
    }
    return difference == 0;
}

std::optional<std::string> DecodeBase64(std::string_view value) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    if (value.empty() || value.size() % 4 != 0) {
        return std::nullopt;
    }

    std::string decoded;
    decoded.reserve(value.size() / 4 * 3);
    for (std::size_t offset = 0; offset < value.size(); offset += 4) {
        std::array<int, 4> digits{};
        int padding = 0;
        for (std::size_t index = 0; index < 4; ++index) {
            const auto ch = value[offset + index];
            if (ch == '=') {
                ++padding;
                digits[index] = 0;
                if (index < 2 || (padding == 1 && index != 3)) {
                    return std::nullopt;
                }
            } else {
                if (padding != 0) {
                    return std::nullopt;
                }
                const auto position = alphabet.find(ch);
                if (position == std::string_view::npos) {
                    return std::nullopt;
                }
                digits[index] = static_cast<int>(position);
            }
        }
        const auto combined = (digits[0] << 18) |
                              (digits[1] << 12) |
                              (digits[2] << 6) |
                              digits[3];
        decoded.push_back(static_cast<char>((combined >> 16) & 0xFF));
        if (padding < 2) {
            decoded.push_back(static_cast<char>((combined >> 8) & 0xFF));
        }
        if (padding == 0) {
            decoded.push_back(static_cast<char>(combined & 0xFF));
        }
        if (padding > 0 && offset + 4 != value.size()) {
            return std::nullopt;
        }
    }
    return decoded;
}

std::optional<std::map<std::string, std::string>> ParseDigestParameters(
    std::string_view value) {
    std::map<std::string, std::string> parameters;
    std::size_t offset = 0;
    while (offset < value.size()) {
        while (offset < value.size() &&
               (value[offset] == ' ' || value[offset] == '\t' || value[offset] == ',')) {
            ++offset;
        }
        if (offset == value.size()) {
            break;
        }

        const auto key_begin = offset;
        while (offset < value.size() && value[offset] != '=' &&
               value[offset] != ',' && value[offset] != ' ' && value[offset] != '\t') {
            ++offset;
        }
        if (key_begin == offset) {
            return std::nullopt;
        }
        const auto key = value.substr(key_begin, offset - key_begin);
        while (offset < value.size() && (value[offset] == ' ' || value[offset] == '\t')) {
            ++offset;
        }
        if (offset == value.size() || value[offset] != '=') {
            return std::nullopt;
        }
        ++offset;
        while (offset < value.size() && (value[offset] == ' ' || value[offset] == '\t')) {
            ++offset;
        }

        std::string parsed_value;
        if (offset < value.size() && value[offset] == '"') {
            ++offset;
            while (offset < value.size() && value[offset] != '"') {
                if (value[offset] == '\\') {
                    ++offset;
                    if (offset == value.size()) {
                        return std::nullopt;
                    }
                }
                parsed_value.push_back(value[offset++]);
            }
            if (offset == value.size() || value[offset] != '"') {
                return std::nullopt;
            }
            ++offset;
        } else {
            const auto value_begin = offset;
            while (offset < value.size() && value[offset] != ',') {
                ++offset;
            }
            auto value_end = offset;
            while (value_end > value_begin &&
                   (value[value_end - 1] == ' ' || value[value_end - 1] == '\t')) {
                --value_end;
            }
            if (value_begin == value_end) {
                return std::nullopt;
            }
            parsed_value.assign(value.substr(value_begin, value_end - value_begin));
        }

        const auto lower_key = [&]() {
            std::string result(key);
            for (auto& ch : result) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return result;
        }();
        if (!parameters.emplace(lower_key, std::move(parsed_value)).second) {
            return std::nullopt;
        }
        while (offset < value.size() && (value[offset] == ' ' || value[offset] == '\t')) {
            ++offset;
        }
        if (offset < value.size() && value[offset] != ',') {
            return std::nullopt;
        }
    }
    return parameters;
}

bool IsHexValue(std::string_view value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') ||
               (ch >= 'a' && ch <= 'f') ||
               (ch >= 'A' && ch <= 'F');
    });
}

std::optional<std::uint32_t> ParseHex32(std::string_view value) {
    if (value.empty() || value.size() > 8 || !IsHexValue(value)) {
        return std::nullopt;
    }
    std::uint32_t result = 0;
    for (const auto ch : value) {
        result <<= 4;
        if (ch >= '0' && ch <= '9') {
            result += static_cast<std::uint32_t>(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            result += static_cast<std::uint32_t>(ch - 'a' + 10);
        } else {
            result += static_cast<std::uint32_t>(ch - 'A' + 10);
        }
    }
    return result;
}

std::string MakeNonce(std::uint64_t session_id) {
    std::random_device random;
    std::ostringstream output;
    output << std::hex << std::setfill('0')
           << static_cast<std::uint64_t>(random()) << session_id
           << static_cast<std::uint64_t>(
                  std::chrono::steady_clock::now().time_since_epoch().count());
    return Md5Hex(output.str());
}

} // namespace

    // connection 只捕获 session 的 weak_ptr；manager 是 session 的强所有者，
    // 因而 socket 的晚到回调不会延长已移除 session 的生命周期。
    std::shared_ptr<RtspClientSession> RtspClientSession::Create(
        boost::asio::ip::tcp::socket socket,
        std::shared_ptr<const RtspSessionContext> context,
        std::uint64_t id,
        ClosedHandler on_closed) {
        auto session = std::shared_ptr<RtspClientSession>(
            new RtspClientSession(std::move(context), id, std::move(on_closed)));
        const auto weak = std::weak_ptr<RtspClientSession>(session);
        session->connection_ = std::make_shared<RtspConnection>(
            std::move(socket),
            [weak](RtspRequest request) {
                if (const auto self = weak.lock()) {
                    self->HandleRequest(request);
                }
            },
            [weak](std::uint8_t channel,
                   std::vector<std::uint8_t> payload) {
                if (const auto self = weak.lock()) {
                    self->HandleInterleavedFrame(channel, std::move(payload));
                }
            },
            [weak](const RtspRequestParseResult& parse_result) {
                if (const auto self = weak.lock()) {
                    self->HandleConnectionParseError(parse_result);
                }
            },
            [weak](const boost::system::error_code& ec) {
                if (const auto self = weak.lock()) {
                    self->OnConnectionClosed(ec);
                }
            });
        return session;
    }



    // Start 由 accept executor 调用，connection 会在同一 executor 上启动 read loop。
    void RtspClientSession::Start() {
        if (connection_) {
            // 计时器从 session 建立时开始；后续 request、RTCP 和媒体发送
            // 都会重新设置同一个 timer，避免空连接长期占用 session registry。
            TouchActivity();
            connection_->Start();
        }
    }



    // Stop 与远端断开共用幂等 Close 路径，关闭 media transport 后再通知 manager 移除 registry。
    void RtspClientSession::Stop() {
        Close();
    }



    bool RtspClientSession::IsPlaying() const {
        return playing_.load();
    }



    bool RtspClientSession::IsMulticastTransport() const {
        return std::any_of(track_states_.begin(),
                           track_states_.end(),
                           [](const auto& item) {
                               return item.second.ready &&
                                      item.second.multicast_subscription.has_value();
                           });
    }



    bool RtspClientSession::IsPlayingMulticast() const {
        return playing_.load() && IsMulticastTransport();
    }



    bool RtspClientSession::IsPlayingMulticastTrack(int track_id) const {
        const auto* state = FindTrackState(track_id);
        return playing_.load() &&
               state &&
               state->ready &&
               state->multicast_subscription.has_value();
    }



    std::uint64_t RtspClientSession::Id() const {
        return id_;
    }



    void RtspClientSession::SendRtpPayload(int track_id,
                                           const RtpPayload& payload,
                                           std::uint8_t payload_type) {
        if (!playing_.load() || payload.payload.empty()) {
            return;
        }

        TouchActivity();

        auto* state = FindTrackState(track_id);
        if (!state || !state->ready) {
            return;
        }

        if (state->sender && state->media_transport) {
            // sender 只负责把 payload 变成完整 RTP bytes；socket/framing 仍
            // 完全由 transport 决定，TCP 和 UDP 因而共享同一套 sequence/SSRC。
            auto packet = state->sender->BuildRtpPacket(payload, payload_type);
            if (packet.empty()) {
                return;
            }
            state->media_transport->SendRtp(std::move(packet));
            MaybeSendRtcpSenderReport(*state);
        }
    }

RtspClientSession::RtspClientSession(
    std::shared_ptr<const RtspSessionContext> context,
    std::uint64_t id,
    ClosedHandler on_closed)
    : context_(std::move(context)),
      idle_timer_(context_->io_executor),
      on_closed_(std::move(on_closed)),
      id_(id),
      session_id_(rtsp_session_detail::MakeSessionId(id)) {
}



    void RtspClientSession::HandleInterleavedFrame(
        std::uint8_t channel,
        std::vector<std::uint8_t> payload) {
        TouchActivity();
        auto* state = FindTrackStateByRtcpChannel(channel);
        if (state && !payload.empty()) {
            HandleRtcpPacket(*state,
                             payload.data(),
                             payload.size(),
                             "tcp-interleaved");
        }
    }



    void RtspClientSession::HandleConnectionParseError(
        const RtspRequestParseResult& parse_result) {
        LOG_WARN("RTSP request parse failed at byte {}: {}",
                 parse_result.error_offset,
                 parse_result.error);
        // framing 错误后无法可靠定位下一条 request。停止 RTP 发送，
        // 先把 400 写完再关闭 TCP，避免错误数据继续堆入发送队列。
        playing_ = false;
        if (!connection_ || closed_) {
            return;
        }
        connection_->SendRtsp(RtspResponseBuilder::Build(400, "Bad Request", {}));
        connection_->CloseAfterFlush();
    }



    void RtspClientSession::OnConnectionClosed(const boost::system::error_code&) {
        // connection 已经完成 TCP cancel/close；session 只清理 media transport
        // 状态和 registry，不再次直接触碰 TCP socket。
        Close();
    }



    void RtspClientSession::HandleRequest(const RtspRequest& request) {
        TouchActivity();
        // 解析器负责语法，RtspClientSession 负责 RTSP 语义状态机。这里先处理
        // 所有请求都需要的 CSeq/version，再进入具体方法的状态校验。
        std::string cseq;
        if (!rtsp_session_detail::ExtractCSeq(request, cseq)) {
            SendRtsp(RtspResponseBuilder::Build(400, "Bad Request", {}));
            return;
        }

        if (request.version_major != 1 || request.version_minor != 0) {
            SendRtsp(RtspResponseBuilder::Build(505, "RTSP Version Not Supported", cseq));
            return;
        }

        if (request.uri == "*" && request.method != "OPTIONS") {
            SendRtsp(RtspResponseBuilder::Build(400, "Bad Request", cseq));
            return;
        }

        if (!AuthenticateRequest(request, cseq)) {
            return;
        }

        if (request.method == "OPTIONS") {
            SendRtsp(RtspResponseBuilder::Build(
                200,
                "OK",
                cseq,
                {{"Public", "OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER"}}));
            return;
        }

        if (request.method == "DESCRIBE") {
            requested_url_ = request.uri;
            const auto sdp = context_->build_sdp(LocalAddress());
            SendRtsp(RtspResponseBuilder::Build(
                200,
                "OK",
                cseq,
                {{"Content-Base", requested_url_ + "/"}, {"Session", session_id_}},
                sdp));
            return;
        }

        if (request.method == "SETUP") {
            requested_url_ = request.uri;
            const auto* track = FindTrackForSetupUrl(request.uri);
            if (!track) {
                LOG_WARN("RTSP SETUP rejected unknown track url '{}'", request.uri);
                SendRtsp(RtspResponseBuilder::Build(404, "Not Found", cseq));
                return;
            }

            const auto transport = request.HeaderValue("Transport");
            std::string transport_error;
            RtspTransportSpec transport_spec;
            if (!RtspTransportSpec::Parse(transport, transport_spec, &transport_error)) {
                LOG_WARN("RTSP SETUP rejected transport '{}': {}",
                         transport,
                         transport_error);
                SendRtsp(RtspResponseBuilder::Build(461, "Unsupported Transport", cseq));
                return;
            }

            if (transport_spec.IsTcpInterleaved() &&
                !context_->config.rtsp.enable_tcp_interleaved) {
                LOG_WARN("RTSP SETUP rejected TCP transport because it is disabled: {}",
                         transport);
                SendRtsp(RtspResponseBuilder::Build(461, "Unsupported Transport", cseq));
                return;
            }

            if (transport_spec.IsUdp()) {
                if (!context_->config.rtsp.enable_udp) {
                    LOG_WARN("RTSP SETUP rejected UDP transport because it is disabled: {}",
                             transport);
                    SendRtsp(RtspResponseBuilder::Build(461, "Unsupported Transport", cseq));
                    return;
                }
            }

            if (transport_spec.mode == RtspTransportMode::UdpUnicast ||
                transport_spec.mode == RtspTransportMode::TcpInterleaved) {
                auto& state = PrepareTrackState(*track);
                if (!CreateMediaTransport(state, transport_spec)) {
                    SendRtsp(RtspResponseBuilder::Build(461, "Unsupported Transport", cseq));
                    return;
                }
                FinishTrackSetup(state, transport_spec);
            } else if (transport_spec.mode == RtspTransportMode::UdpMulticast) {
                if (!context_->config.rtsp.enable_multicast) {
                    LOG_WARN("RTSP SETUP multicast rejected because multicast is disabled");
                    SendRtsp(RtspResponseBuilder::Build(461, "Unsupported Transport", cseq));
                    return;
                }
                if (track->media_type != MediaType::VIDEO ||
                    track->codec_type != CodecType::H264) {
                    // 当前共享 multicast sender 仍按单视频流建模，音频 multicast 后续再扩展独立端口组。
                    LOG_WARN("RTSP SETUP multicast rejected for non-H264 video track {}",
                             track->track_id);
                    SendRtsp(RtspResponseBuilder::Build(461, "Unsupported Transport", cseq));
                    return;
                }
                auto& state = PrepareTrackState(*track);
                if (!context_->configure_multicast(transport_spec,
                                                              LocalAddress())) {
                    SendRtsp(RtspResponseBuilder::Build(461, "Unsupported Transport", cseq));
                    return;
                }
                // multicast socket 属于 server 级共享 publisher；session 只
                // 保存 response snapshot，两个客户端不会各自创建发送循环。
                state.multicast_subscription =
                    RtspMulticastSubscription{transport_spec};
                FinishTrackSetup(state, transport_spec);
            } else {
                LOG_WARN("RTSP SETUP rejected unsupported transport mode");
                SendRtsp(RtspResponseBuilder::Build(461, "Unsupported Transport", cseq));
                return;
            }

            playing_ = false;

            SendRtsp(RtspResponseBuilder::Build(
                200,
                "OK",
                cseq,
                {{"Session", session_id_},
                 {"Transport", transport_spec.ToSetupResponseHeader()}}
            ));
            return;
        }

        if (request.method == "PLAY") {
            if (!HasReadyTrack()) {
                SendRtsp(RtspResponseBuilder::Build(455, "Method Not Valid In This State", cseq));
                return;
            }

            playing_ = true;
            const auto play_url = request.uri.empty() ? requested_url_ : request.uri;
            SendRtsp(RtspResponseBuilder::Build(
                200,
                "OK",
                cseq,
                {{"Session", session_id_},
                 {"RTP-Info", BuildRtpInfo(play_url)}}));
            return;
        }

        if (request.method == "PAUSE") {
            playing_ = false;
            SendRtsp(RtspResponseBuilder::Build(200, "OK", cseq, {{"Session", session_id_}}));
            return;
        }

        if (request.method == "GET_PARAMETER") {
            SendRtsp(RtspResponseBuilder::Build(200, "OK", cseq, {{"Session", session_id_}}));
            return;
        }

        if (request.method == "TEARDOWN") {
            playing_ = false;
            SendRtsp(RtspResponseBuilder::Build(200, "OK", cseq, {{"Session", session_id_}}));
            Close();
            return;
        }

        SendRtsp(RtspResponseBuilder::Build(405, "Method Not Allowed", cseq));
    }



    RtspClientSession::TrackSessionState* RtspClientSession::FindTrackState(int track_id) {
        auto it = track_states_.find(track_id);
        return it == track_states_.end() ? nullptr : &it->second;
    }



    const RtspClientSession::TrackSessionState* RtspClientSession::FindTrackState(int track_id) const {
        auto it = track_states_.find(track_id);
        return it == track_states_.end() ? nullptr : &it->second;
    }



    RtspClientSession::TrackSessionState* RtspClientSession::FindTrackStateByRtcpChannel(std::uint8_t channel) {
        auto it = std::find_if(track_states_.begin(),
                               track_states_.end(),
                               [channel](auto& item) {
                                   return item.second.ready &&
                                          item.second.transport_spec.IsTcpInterleaved() &&
                                          item.second.rtcp_channel ==
                                              static_cast<int>(channel);
                               });
        return it == track_states_.end() ? nullptr : &it->second;
    }



    const MediaTrackConfig* RtspClientSession::FindTrackForSetupUrl(const std::string& url) const {
        int track_id = 0;
        if (rtsp_session_detail::ParseTrackIdFromUrl(url, track_id)) {
            return rtsp_session_detail::FindTrackById(context_->tracks, track_id);
        }
        return context_->tracks.size() == 1 ? &context_->tracks.front() : nullptr;
    }



    RtspClientSession::TrackSessionState& RtspClientSession::PrepareTrackState(const MediaTrackConfig& track) {
        auto& state = track_states_[track.track_id];
        CloseTransport(state);
        state = {};
        state.track_id = track.track_id;
        state.track = &track;
        state.sender = std::make_unique<RtpSender>(
            RtpSender::CreateDefault(track.rtp_payload_type,
                                     track.rtp_clock_rate));
        return state;
    }



    void RtspClientSession::FinishTrackSetup(RtspClientSession::TrackSessionState& state,
                          const RtspTransportSpec& transport_spec) {
        state.transport_spec = transport_spec;
        state.rtp_channel = static_cast<int>(transport_spec.rtp_channel);
        state.rtcp_channel = static_cast<int>(transport_spec.rtcp_channel);
        state.ready = true;
        ready_ = true;
    }



    bool RtspClientSession::HasReadyTrack() const {
        return std::any_of(track_states_.begin(),
                           track_states_.end(),
                           [](const auto& item) {
                               return item.second.ready;
                           });
    }



    std::string RtspClientSession::BuildTrackUrl(const std::string& play_url, int track_id) const {
        if (play_url.find("track" + std::to_string(track_id)) != std::string::npos) {
            return play_url;
        }
        return play_url + (play_url.empty() || play_url.back() == '/' ? "" : "/") +
               "track" + std::to_string(track_id);
    }



    std::string RtspClientSession::BuildRtpInfo(const std::string& play_url) const {
        std::ostringstream oss;
        bool first = true;
        for (const auto& [track_id, state] : track_states_) {
            if (!state.ready) {
                continue;
            }

            std::uint16_t rtp_sequence = 0;
            std::uint32_t rtp_timestamp = 0;
            if (state.transport_spec.mode == RtspTransportMode::UdpMulticast) {
                rtp_sequence = context_->get_multicast_sequence();
                rtp_timestamp = context_->get_multicast_last_rtp_timestamp();
            } else if (state.sender) {
                const auto snapshot = state.sender->Snapshot();
                rtp_sequence = snapshot.next_sequence;
                rtp_timestamp = snapshot.last_rtp_timestamp;
            }
            if (!first) {
                oss << ",";
            }
            first = false;
            oss << "url=" << BuildTrackUrl(play_url, track_id)
                << ";seq=" << rtp_sequence
                << ";rtptime=" << rtp_timestamp;
        }
        return oss.str();
    }



    void RtspClientSession::MaybeSendRtcpSenderReport(RtspClientSession::TrackSessionState& state) {
        if (!state.sender || !state.sender->ShouldSendSenderReport()) {
            return;
        }

        auto report = state.sender->BuildSenderReport();
        if (report.empty()) {
            return;
        }
        if (state.media_transport) {
            state.media_transport->SendRtcp(std::move(report));
        }
    }



    void RtspClientSession::HandleRtcpPacket(RtspClientSession::TrackSessionState& state,
                          const std::uint8_t* data,
                          std::size_t size,
                          const char* transport_name) {
        const auto parsed = RtcpPacketCodec::ParseCompound(data, size);
        if (!parsed.valid) {
            LOG_DEBUG("RTSP RTCP packet ignored: session={}, transport={}, "
                      "version={}, size={}, remaining={}",
                      id_,
                      transport_name,
                      static_cast<int>(parsed.invalid_version),
                      parsed.invalid_packet_size,
                      size - parsed.error_offset);
            return;
        }

        for (const auto& parsed_packet : parsed.packets) {
            const auto* packet = parsed_packet.bytes.data();
            const auto packet_size = parsed_packet.bytes.size();
            const auto packet_type = parsed_packet.packet_type;
            const auto report_count = parsed_packet.report_count;
            if (packet_type == RtcpPacketCodec::kReceiverReportPacketType) {
                ParseRtcpReceiverReport(state,
                                        packet,
                                        packet_size,
                                        report_count,
                                        transport_name);
            } else if (packet_type == RtcpPacketCodec::kSenderReportPacketType) {
                ParseRtcpSenderReport(state,
                                      packet,
                                      packet_size,
                                      report_count,
                                      transport_name);
            } else {
                LOG_DEBUG("RTSP RTCP {} received: session={}, transport={}, size={}",
                          RtcpPacketCodec::PacketTypeName(packet_type),
                          id_,
                          transport_name,
                          packet_size);
            }
        }

        if (parsed.trailing_bytes != 0) {
            LOG_DEBUG("RTSP RTCP trailing bytes ignored: session={}, transport={}, "
                      "bytes={}",
                      id_,
                      transport_name,
                      parsed.trailing_bytes);
        }
    }



    void RtspClientSession::ParseRtcpReceiverReport(RtspClientSession::TrackSessionState& state,
                                 const std::uint8_t* packet,
                                 std::size_t packet_size,
                                 std::uint8_t report_count,
                                 const char* transport_name) {
        if (packet_size < 8U + static_cast<std::size_t>(report_count) * 24U) {
            LOG_DEBUG("RTSP RTCP RR ignored: session={}, transport={}, size={}, rc={}",
                      id_,
                      transport_name,
                      packet_size,
                      static_cast<int>(report_count));
            return;
        }

        const auto reporter_ssrc = rtsp_session_detail::ReadU32(packet + 4);
        ParseRtcpReportBlocks(state,
                              packet + 8,
                              report_count,
                              reporter_ssrc,
                              transport_name,
                              "RR");
    }



    void RtspClientSession::ParseRtcpSenderReport(RtspClientSession::TrackSessionState& state,
                               const std::uint8_t* packet,
                               std::size_t packet_size,
                               std::uint8_t report_count,
                               const char* transport_name) {
        if (packet_size < 28U + static_cast<std::size_t>(report_count) * 24U) {
            LOG_DEBUG("RTSP RTCP SR ignored: session={}, transport={}, size={}, rc={}",
                      id_,
                      transport_name,
                      packet_size,
                      static_cast<int>(report_count));
            return;
        }

        const auto reporter_ssrc = rtsp_session_detail::ReadU32(packet + 4);
        ParseRtcpReportBlocks(state,
                              packet + 28,
                              report_count,
                              reporter_ssrc,
                              transport_name,
                              "SR");
    }



    void RtspClientSession::ParseRtcpReportBlocks(RtspClientSession::TrackSessionState& state,
                               const std::uint8_t* blocks,
                               std::uint8_t report_count,
                               std::uint32_t reporter_ssrc,
                               const char* transport_name,
                               const char* packet_type_name) {
        if (report_count == 0) {
            LOG_DEBUG("RTSP RTCP {} received without report blocks: session={}, "
                      "transport={}, reporter_ssrc={}",
                      packet_type_name,
                      id_,
                      transport_name,
                      reporter_ssrc);
            return;
        }

        for (std::uint8_t index = 0; index < report_count; ++index) {
            RtcpReportBlock block;
            if (!RtcpPacketCodec::ReadReportBlock(
                    blocks + static_cast<std::size_t>(index) * 24U,
                    24,
                    block)) {
                return;
            }

            // Receiver Report 里的 source_ssrc 应该指向本 server 发送 RTP 使用的 SSRC。
            const auto sender_ssrc = state.sender
                ? state.sender->Snapshot().ssrc
                : 0;
            const bool matches_sender = state.sender &&
                                        block.source_ssrc == sender_ssrc;
            if (matches_sender || !state.has_receiver_report) {
                state.last_receiver_report = block;
                state.last_receiver_reporter_ssrc = reporter_ssrc;
                state.has_receiver_report = true;
                ++state.receiver_reports_received;
                // session 级 RR 最终汇总到 PublisherStats，便于外部观察客户端反馈是否到达。
                context_->add_receiver_reports(1);
            }

            LOG_DEBUG("RTSP RTCP {} block: session={}, transport={}, reporter_ssrc={}, "
                      "media_ssrc={}, match={}, fraction_lost={}, cumulative_lost={}, "
                      "highest_seq={}, jitter={}, lsr={}, dlsr={}",
                      packet_type_name,
                      id_,
                      transport_name,
                      reporter_ssrc,
                      block.source_ssrc,
                      matches_sender,
                      static_cast<int>(block.fraction_lost),
                      block.cumulative_lost,
                      block.extended_highest_sequence,
                      block.jitter,
                      block.last_sender_report,
                      block.delay_since_last_sender_report);
        }
    }



    bool RtspClientSession::CreateMediaTransport(
        RtspClientSession::TrackSessionState& state,
        RtspTransportSpec& transport_spec) {
        boost::system::error_code ec;
        RtspMediaTransportContext transport_context;
        transport_context.io_executor = context_->io_executor;
        transport_context.connection = connection_;
        if (connection_) {
            transport_context.local_endpoint = connection_->LocalEndpoint(ec);
            if (ec) {
                LOG_WARN("RTSP SETUP failed to resolve local endpoint: {}", ec.message());
                return false;
            }
            transport_context.remote_endpoint = connection_->RemoteEndpoint(ec);
            if (ec) {
                LOG_WARN("RTSP SETUP failed to resolve client endpoint: {}", ec.message());
                return false;
            }
        }

        const auto track_id = state.track_id;
        const auto weak = weak_from_this();
        transport_context.on_rtcp =
            [weak, track_id](std::vector<std::uint8_t> packet,
                             std::string transport_name) {
                if (const auto self = weak.lock()) {
                    auto* state = self->FindTrackState(track_id);
                    if (state && !packet.empty()) {
                        self->HandleRtcpPacket(*state,
                                               packet.data(),
                                               packet.size(),
                                               transport_name.c_str());
                    }
                }
            };

        const auto result = RtspMediaTransportFactory::Create(
            transport_spec,
            transport_context);
        if (!result) {
            LOG_WARN("RTSP SETUP transport creation failed: {}", result.error);
            return false;
        }
        state.media_transport = result.transport;
        transport_spec = result.response_spec;
        LOG_INFO("RTSP SETUP {} media transport ready",
                 transport_spec.IsTcpInterleaved() ? "TCP interleaved" : "UDP unicast");
        return true;
    }



    void RtspClientSession::SendRtsp(std::string response) {
        if (connection_) {
            connection_->SendRtsp(std::move(response));
        }
    }



    std::string RtspClientSession::LocalAddress() const {
        if (!connection_) {
            return "127.0.0.1";
        }
        boost::system::error_code ec;
        const auto endpoint = connection_->LocalEndpoint(ec);
        return ec ? "127.0.0.1" : endpoint.address().to_string();
    }



    void RtspClientSession::CloseTransport(TrackSessionState& state) {
        if (state.media_transport) {
            state.media_transport->Close();
            state.media_transport.reset();
        }
    }



    void RtspClientSession::CloseAllTransports() {
        for (auto& [_, state] : track_states_) {
            CloseTransport(state);
        }
    }



    void RtspClientSession::Close() {
        if (closed_) {
            return;
        }
        closed_ = true;
        // 当前项目使用的 Boost 版本只提供无参数 cancel()；timer 位于
        // session 自己的 executor，取消失败不会改变 session 的关闭语义。
        idle_timer_.cancel();
        playing_ = false;
        ready_ = false;

        CloseAllTransports();
        if (connection_) {
            connection_->Close();
        }
        if (on_closed_) {
            on_closed_(id_);
        }
}

bool RtspClientSession::AuthenticateRequest(const RtspRequest& request,
                                            const std::string& cseq) {
    const auto mode = context_->config.rtsp.auth_mode;
    if (mode == RtspAuthMode::None) {
        return true;
    }

    const auto authorization = request.HeaderValues("Authorization");
    bool authorized = false;
    bool stale_nonce = false;
    if (authorization.size() == 1) {
        if (mode == RtspAuthMode::Basic) {
            const auto header = authorization.front();
            const auto separator = header.find(' ');
            if (separator != std::string_view::npos &&
                AsciiEqualsIgnoreCase(header.substr(0, separator), "Basic")) {
                const auto decoded = DecodeBase64(header.substr(separator + 1));
                if (decoded) {
                    const auto colon = decoded->find(':');
                    authorized = colon != std::string::npos &&
                                 ConstantTimeEquals(
                                     decoded->substr(0, colon),
                                     context_->config.rtsp.auth_username) &&
                                 ConstantTimeEquals(
                                     decoded->substr(colon + 1),
                                     context_->config.rtsp.auth_password);
                }
            }
        } else if (mode == RtspAuthMode::Digest) {
            authorized = ValidateDigestAuthorization(
                request,
                authorization.front(),
                stale_nonce);
        }
    }

    if (!authorized) {
        SendUnauthorized(cseq, stale_nonce);
        return false;
    }
    return true;
}

bool RtspClientSession::ValidateDigestAuthorization(
    const RtspRequest& request,
    std::string_view authorization,
    bool& stale_nonce) {
    const auto separator = authorization.find(' ');
    if (separator == std::string_view::npos ||
        !AsciiEqualsIgnoreCase(authorization.substr(0, separator), "Digest")) {
        return false;
    }

    const auto parsed = ParseDigestParameters(authorization.substr(separator + 1));
    if (!parsed) {
        return false;
    }
    const auto& parameters = *parsed;
    const auto find = [&parameters](std::string_view name) -> const std::string* {
        const auto it = parameters.find(std::string{name});
        return it == parameters.end() ? nullptr : &it->second;
    };
    const auto* username = find("username");
    const auto* realm = find("realm");
    const auto* nonce = find("nonce");
    const auto* uri = find("uri");
    const auto* response = find("response");
    const auto* qop = find("qop");
    const auto* nc = find("nc");
    const auto* cnonce = find("cnonce");
    const auto* algorithm = find("algorithm");
    if (!username || !realm || !nonce || !uri || !response ||
        !qop || !nc || !cnonce ||
        !AsciiEqualsIgnoreCase(algorithm ? *algorithm : "MD5", "MD5") ||
        *username != context_->config.rtsp.auth_username ||
        *realm != context_->config.rtsp.auth_realm ||
        *uri != request.uri ||
        !AsciiEqualsIgnoreCase(*qop, "auth") ||
        nc->size() != 8 || !IsHexValue(*nc) ||
        cnonce->empty() || response->size() != 32 || !IsHexValue(*response)) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto nonce_age = now - auth_nonce_created_at_;
    const auto nonce_ttl = std::chrono::milliseconds(
        context_->config.rtsp.auth_nonce_ttl_ms);
    const auto nonce_count = ParseHex32(*nc);
    if (auth_nonce_.empty() || *nonce != auth_nonce_ || nonce_age >= nonce_ttl) {
        stale_nonce = !auth_nonce_.empty() && *nonce == auth_nonce_;
        return false;
    }
    if (!nonce_count || *nonce_count <= digest_nonce_count_) {
        return false;
    }

    const auto& options = context_->config.rtsp;
    const auto ha1 = Md5Hex(options.auth_username + ":" +
                            options.auth_realm + ":" +
                            options.auth_password);
    const auto ha2 = Md5Hex(request.method + ":" + request.uri);
    const auto expected = Md5Hex(ha1 + ":" + *nonce + ":" + *nc + ":" +
                                 *cnonce + ":" + *qop + ":" + ha2);
    if (!ConstantTimeEquals(expected, *response)) {
        return false;
    }
    digest_nonce_count_ = *nonce_count;
    return true;
}

void RtspClientSession::SendUnauthorized(const std::string& cseq,
                                          bool stale_nonce) {
    const auto& options = context_->config.rtsp;
    std::string challenge;
    if (options.auth_mode == RtspAuthMode::Basic) {
        challenge = "Basic realm=\"" + options.auth_realm + "\"";
    } else {
        challenge = "Digest realm=\"" + options.auth_realm +
                    "\", nonce=\"" + EnsureDigestNonce() +
                    "\", algorithm=MD5, qop=\"auth\"";
        if (stale_nonce) {
            challenge += ", stale=true";
        }
    }

    SendRtsp(RtspResponseBuilder::Build(
        401,
        "Unauthorized",
        cseq,
        {{"WWW-Authenticate", challenge}}));
}

std::string RtspClientSession::EnsureDigestNonce() {
    const auto now = std::chrono::steady_clock::now();
    const auto ttl = std::chrono::milliseconds(
        context_->config.rtsp.auth_nonce_ttl_ms);
    if (auth_nonce_.empty() || now - auth_nonce_created_at_ >= ttl) {
        auth_nonce_ = MakeNonce(id_);
        auth_nonce_created_at_ = now;
        digest_nonce_count_ = 0;
    }
    return auth_nonce_;
}

void RtspClientSession::TouchActivity() {
    if (closed_ || !context_ ||
        context_->config.rtsp.session_idle_timeout_ms <= 0) {
        return;
    }

    idle_timer_.expires_after(std::chrono::milliseconds(
        context_->config.rtsp.session_idle_timeout_ms));
    const auto weak = weak_from_this();
    idle_timer_.async_wait([weak](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        if (const auto self = weak.lock()) {
            self->OnIdleTimeout();
        }
    });
}

void RtspClientSession::OnIdleTimeout() {
    if (closed_) {
        return;
    }

    // PLAY 状态代表客户端仍在消费媒体；如果 pipeline 暂时没有新帧，仍
    // 保留 session，避免把“无帧”误判成控制面闲置。下一次媒体或 RTSP
    // 活动会重新设置 timer，PAUSE/未 PLAY 状态则按配置主动关闭。
    if (playing_.load()) {
        TouchActivity();
        return;
    }

    LOG_INFO("RTSP session {} closed after {} ms of control inactivity",
             id_,
             context_->config.rtsp.session_idle_timeout_ms);
    Close();
}

RtspSessionManager::RtspSessionManager(
    std::shared_ptr<const RtspSessionContext> context)
    : context_(std::move(context)) {
}

RtspSessionManager::Session RtspSessionManager::Create(
    boost::asio::ip::tcp::socket socket) {
    boost::system::error_code endpoint_error;
    const auto remote_endpoint = socket.remote_endpoint(endpoint_error);
    const auto& allowed_addresses =
        context_->config.rtsp.allowed_client_addresses;
    const bool rejected_by_allowlist =
        (!allowed_addresses.empty() && endpoint_error) ||
        (!endpoint_error &&
         !rtsp_session_detail::IsClientAddressAllowed(
             context_->config.rtsp,
             remote_endpoint));
    if (rejected_by_allowlist) {
        // 在进入 registry 前拒绝连接，避免未授权客户端先占用 session
        // ID、媒体 transport 或统计槽位；socket 仍由这里负责关闭。
        LOG_WARN("RTSP client rejected by address allowlist: {}",
                 endpoint_error ? endpoint_error.message()
                                : remote_endpoint.address().to_string());
        boost::system::error_code close_error;
        socket.close(close_error);
        return {};
    }

    // ID 分配和 registry 插入分别受同一把锁保护；不在锁内启动 socket 或执行回调。
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        id = next_session_id_++;
    }

    const auto weak_manager = weak_from_this();
    auto session = RtspClientSession::Create(
        std::move(socket),
        context_,
        id,
        [weak_manager](std::uint64_t closed_id) {
            if (const auto manager = weak_manager.lock()) {
                manager->Remove(closed_id);
            }
        });
    {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.push_back(session);
    }
    return session;
}

void RtspSessionManager::Remove(std::uint64_t session_id) {
    // closed callback 只做容器删除，实际 socket close 已由 session/connection 完成。
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(
        std::remove_if(
            sessions_.begin(),
            sessions_.end(),
            [session_id](const Session& session) {
                return !session || session->Id() == session_id;
            }),
        sessions_.end());
}

std::vector<RtspSessionManager::Session> RtspSessionManager::Snapshot() const {
    // 返回 shared_ptr 副本后立即释放锁，调用方可安全执行媒体发送和状态查询。
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_;
}

std::size_t RtspSessionManager::Size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sessions_.size();
}

void RtspSessionManager::Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.clear();
}
