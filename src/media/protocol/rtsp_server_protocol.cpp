#include "media/protocol/rtsp_server_protocol.h"

#include "common/log/logger.h"
#include "media/protocol/rtsp_transport_spec.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <deque>
#include <iomanip>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <utility>

#include <boost/asio/ip/multicast.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>



namespace {

constexpr std::size_t kRtpHeaderSize = 12;
constexpr std::size_t kTcpInterleavedHeaderSize = 4;
constexpr std::size_t kRtcpSenderReportSize = 28;
constexpr std::uint8_t kRtcpSenderReportPacketType = 200;
constexpr std::uint8_t kRtcpReceiverReportPacketType = 201;
constexpr std::uint8_t kRtcpSourceDescriptionPacketType = 202;
constexpr std::uint8_t kRtcpByePacketType = 203;
constexpr std::uint8_t kRtcpAppPacketType = 204;
constexpr std::chrono::seconds kRtcpSenderReportInterval{5};
constexpr std::uint64_t kUnixToNtpSeconds = 2208988800ULL;

struct RtcpReportBlock {
    std::uint32_t source_ssrc{0};
    std::uint8_t fraction_lost{0};
    std::int32_t cumulative_lost{0};
    std::uint32_t extended_highest_sequence{0};
    std::uint32_t jitter{0};
    std::uint32_t last_sender_report{0};
    std::uint32_t delay_since_last_sender_report{0};
};

std::string Trim(std::string value) {
    const auto not_space = [](unsigned char ch) {
        return !std::isspace(ch);
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(),
                value.end());
    return value;
}

std::string ToUpper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

bool IsIpv4Multicast(const boost::asio::ip::address& address) {
    if (!address.is_v4()) {
        return false;
    }

    const auto bytes = address.to_v4().to_bytes();
    return bytes[0] >= 224 && bytes[0] <= 239;
}

std::uint32_t RandomU32() {
    static thread_local std::mt19937 generator{std::random_device{}()};
    return std::uniform_int_distribution<std::uint32_t>{}(generator);
}

std::uint16_t RandomU16() {
    static thread_local std::mt19937 generator{std::random_device{}()};
    return std::uniform_int_distribution<std::uint16_t>{}(generator);
}

void WriteU16(std::uint8_t* data, std::uint16_t value) {
    data[0] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[1] = static_cast<std::uint8_t>(value & 0xFF);
}

void WriteU32(std::uint8_t* data, std::uint32_t value) {
    data[0] = static_cast<std::uint8_t>((value >> 24) & 0xFF);
    data[1] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
    data[2] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
    data[3] = static_cast<std::uint8_t>(value & 0xFF);
}

std::uint16_t ReadU16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(data[0]) << 8) |
                                      static_cast<std::uint16_t>(data[1]));
}

std::uint32_t ReadU32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) |
           static_cast<std::uint32_t>(data[3]);
}

std::int32_t ReadS24(const std::uint8_t* data) {
    auto value = (static_cast<std::uint32_t>(data[0]) << 16) |
                 (static_cast<std::uint32_t>(data[1]) << 8) |
                 static_cast<std::uint32_t>(data[2]);
    if ((value & 0x00800000U) != 0) {
        value |= 0xFF000000U;
    }
    return static_cast<std::int32_t>(value);
}

const char* RtcpPacketTypeName(std::uint8_t packet_type) {
    switch (packet_type) {
    case kRtcpSenderReportPacketType:
        return "SR";
    case kRtcpReceiverReportPacketType:
        return "RR";
    case kRtcpSourceDescriptionPacketType:
        return "SDES";
    case kRtcpByePacketType:
        return "BYE";
    case kRtcpAppPacketType:
        return "APP";
    default:
        return "UNKNOWN";
    }
}

bool ReadRtcpReportBlock(const std::uint8_t* data,
                         std::size_t size,
                         RtcpReportBlock& block) {
    if (size < 24) {
        return false;
    }

    block.source_ssrc = ReadU32(data);
    block.fraction_lost = data[4];
    block.cumulative_lost = ReadS24(data + 5);
    block.extended_highest_sequence = ReadU32(data + 8);
    block.jitter = ReadU32(data + 12);
    block.last_sender_report = ReadU32(data + 16);
    block.delay_since_last_sender_report = ReadU32(data + 20);
    return true;
}

std::vector<std::uint8_t> MakeRtpPacket(const RtpPayload& payload,
                                        std::uint8_t payload_type,
                                        std::uint32_t ssrc,
                                        std::uint16_t& sequence,
                                        std::uint32_t& last_rtp_timestamp) {
    last_rtp_timestamp = payload.timestamp;

    const auto rtp_size = kRtpHeaderSize + payload.payload.size();
    std::vector<std::uint8_t> packet(rtp_size);

    auto* rtp = packet.data();
    rtp[0] = 0x80;
    rtp[1] = static_cast<std::uint8_t>((payload.marker ? 0x80 : 0) |
                                       (payload_type & 0x7F));
    WriteU16(rtp + 2, sequence++);
    WriteU32(rtp + 4, payload.timestamp);
    WriteU32(rtp + 8, ssrc);

    std::copy(payload.payload.begin(),
              payload.payload.end(),
              packet.begin() + static_cast<std::ptrdiff_t>(kRtpHeaderSize));
    return packet;
}

std::vector<std::uint8_t> BuildRtcpSenderReport(std::uint32_t ssrc,
                                                std::uint32_t rtp_timestamp,
                                                std::uint32_t packet_count,
                                                std::uint32_t octet_count) {
    using namespace std::chrono;

    const auto now = system_clock::now().time_since_epoch();
    const auto seconds_part = duration_cast<seconds>(now);
    const auto nanoseconds_part =
        duration_cast<nanoseconds>(now - seconds_part).count();

    const auto ntp_seconds = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(seconds_part.count()) + kUnixToNtpSeconds);
    const auto ntp_fraction = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(nanoseconds_part) << 32) / 1000000000ULL);

    std::vector<std::uint8_t> report(kRtcpSenderReportSize);
    report[0] = 0x80; // V=2, P=0, RC=0
    report[1] = kRtcpSenderReportPacketType;
    WriteU16(report.data() + 2, 6); // 28 字节 RTCP SR => length 为 32bit word 数减 1。
    WriteU32(report.data() + 4, ssrc);
    WriteU32(report.data() + 8, ntp_seconds);
    WriteU32(report.data() + 12, ntp_fraction);
    WriteU32(report.data() + 16, rtp_timestamp);
    WriteU32(report.data() + 20, packet_count);
    WriteU32(report.data() + 24, octet_count);
    return report;
}

bool ShouldSendRtcpSenderReport(
    std::chrono::steady_clock::time_point& next_report_time) {
    const auto now = std::chrono::steady_clock::now();
    if (next_report_time != std::chrono::steady_clock::time_point{} &&
        now < next_report_time) {
        return false;
    }

    // 首个 RTP 包之后立即发送一次 SR，后续按固定间隔补发，方便播放器快速同步时钟。
    next_report_time = now + kRtcpSenderReportInterval;
    return true;
}

std::string MakeSessionId(std::uint64_t id) {
    std::ostringstream oss;
    oss << std::hex << std::setw(8) << std::setfill('0') << id;
    return oss.str();
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

std::map<std::string, std::string> ParseHeaders(const std::string& request) {
    std::map<std::string, std::string> headers;
    std::istringstream iss(request);
    std::string line;
    std::getline(iss, line);

    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            break;
        }

        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }

        auto key = Trim(line.substr(0, colon));
        auto value = Trim(line.substr(colon + 1));
        headers[std::move(key)] = std::move(value);
    }

    return headers;
}

std::string HeaderValue(const std::map<std::string, std::string>& headers,
                        const std::string& key) {
    auto it = headers.find(key);
    return it == headers.end() ? std::string{} : it->second;
}

std::string BuildResponse(int status_code,
                          const std::string& status_reason,
                          const std::string& cseq,
                          const std::map<std::string, std::string>& headers = {},
                          const std::string& body = {},
                          const std::string& content_type = "application/sdp") {
    std::ostringstream oss;
    oss << "RTSP/1.0 " << status_code << " " << status_reason << "\r\n";
    if (!cseq.empty()) {
        oss << "CSeq: " << cseq << "\r\n";
    }
    oss << "Server: video-pipeline/1.0\r\n";
    for (const auto& [key, value] : headers) {
        oss << key << ": " << value << "\r\n";
    }
    if (!body.empty()) {
        oss << "Content-Type: " << content_type << "\r\n";
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

std::size_t FindHeaderEnd(const std::vector<std::uint8_t>& buffer) {
    if (buffer.size() < 4) {
        return std::string::npos;
    }

    for (std::size_t i = 0; i + 3 < buffer.size(); ++i) {
        if (buffer[i] == '\r' && buffer[i + 1] == '\n' &&
            buffer[i + 2] == '\r' && buffer[i + 3] == '\n') {
            return i + 4;
        }
    }
    return std::string::npos;
}

} // namespace

/// @brief RTSP 会话
/// @note 管理单个客户端连接的核心类，负责处理一个客户端从连接到断开的完整生命周期。
class RtspServerProtocol::ClientSession
    : public std::enable_shared_from_this<RtspServerProtocol::ClientSession> {
public:
    ClientSession(boost::asio::ip::tcp::socket socket,
                  RtspServerProtocol& owner,
                  std::uint64_t id)
        : socket_(std::move(socket)),
          owner_(owner),
          id_(id),
          session_id_(MakeSessionId(id)),
          ssrc_(RandomU32()),
          sequence_(RandomU16()) {
    }

    void Start() {
        DoRead();
    }

    void Stop() {
        playing_ = false;
        ready_ = false;

        boost::system::error_code ignored;
        CloseUdpSockets();
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
    }

    bool IsPlaying() const {
        return playing_.load();
    }

    bool IsMulticastTransport() const {
        return transport_spec_.mode == RtspTransportMode::UdpMulticast;
    }

    bool IsPlayingMulticast() const {
        return playing_.load() && IsMulticastTransport();
    }

    std::uint64_t Id() const {
        return id_;
    }

    void SendRtpPayload(const RtpPayload& payload, std::uint8_t payload_type) {
        if (!playing_.load() || payload.payload.empty()) {
            return;
        }

        if (transport_spec_.IsTcpInterleaved()) {
            SendTcpInterleavedRtpPayload(payload, payload_type);
            return;
        }

        if (transport_spec_.mode == RtspTransportMode::UdpUnicast) {
            SendUdpRtpPayload(payload, payload_type);
        }
    }

private:
    void DoRead() {
        auto self = shared_from_this();
        socket_.async_read_some(
            boost::asio::buffer(read_chunk_),
            [self](boost::system::error_code ec, std::size_t bytes) {
                if (ec) {
                    self->Close();
                    return;
                }
                self->read_buffer_.insert(self->read_buffer_.end(),
                                          self->read_chunk_.data(),
                                          self->read_chunk_.data() + bytes);
                self->ProcessReadBuffer();
                self->DoRead();
            });
    }

    void ProcessReadBuffer() {
        while (!read_buffer_.empty()) {
            if (read_buffer_[0] == '$') {
                if (read_buffer_.size() < 4) {
                    return;
                }
                const auto channel = read_buffer_[1];
                const auto length = ReadU16(read_buffer_.data() + 2);
                const auto frame_size = kTcpInterleavedHeaderSize + length;
                if (read_buffer_.size() < frame_size) {
                    return;
                }

                if (ready_ && channel == static_cast<std::uint8_t>(rtcp_channel_) &&
                    length > 0) {
                    HandleRtcpPacket(read_buffer_.data() + kTcpInterleavedHeaderSize,
                                     length,
                                     "tcp-interleaved");
                }
                read_buffer_.erase(read_buffer_.begin(),
                                   read_buffer_.begin() +
                                       static_cast<std::ptrdiff_t>(frame_size));
                continue;
            }

            const auto header_end = FindHeaderEnd(read_buffer_);
            if (header_end == std::string::npos) {
                return;
            }

            std::string request(reinterpret_cast<const char*>(read_buffer_.data()),
                                header_end);
            read_buffer_.erase(read_buffer_.begin(),
                               read_buffer_.begin() +
                                   static_cast<std::ptrdiff_t>(header_end));
            HandleRequest(request);
        }
    }

    void HandleRequest(const std::string& request) {
        std::istringstream iss(request);
        std::string request_line;
        std::getline(iss, request_line);
        if (!request_line.empty() && request_line.back() == '\r') {
            request_line.pop_back();
        }

        std::istringstream line_stream(request_line);
        std::string method;
        std::string url;
        std::string version;
        line_stream >> method >> url >> version;

        const auto headers = ParseHeaders(request);
        const auto cseq = HeaderValue(headers, "CSeq");
        const auto upper_method = ToUpper(method);

        if (version != "RTSP/1.0") {
            SendRtsp(BuildResponse(400, "Bad Request", cseq));
            return;
        }

        if (upper_method == "OPTIONS") {
            SendRtsp(BuildResponse(
                200,
                "OK",
                cseq,
                {{"Public", "OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER"}}));
            return;
        }

        if (upper_method == "DESCRIBE") {
            requested_url_ = url;
            const auto sdp = owner_.BuildSdp(LocalAddress());
            SendRtsp(BuildResponse(
                200,
                "OK",
                cseq,
                {{"Content-Base", requested_url_ + "/"}, {"Session", session_id_}},
                sdp));
            return;
        }

        if (upper_method == "SETUP") {
            requested_url_ = url;
            const auto transport = HeaderValue(headers, "Transport");
            std::string transport_error;
            RtspTransportSpec transport_spec;
            if (!RtspTransportSpec::Parse(transport, transport_spec, &transport_error)) {
                LOG_WARN("RTSP SETUP rejected transport '{}': {}",
                         transport,
                         transport_error);
                SendRtsp(BuildResponse(461, "Unsupported Transport", cseq));
                return;
            }

            if (transport_spec.IsTcpInterleaved() &&
                !owner_.config_.rtsp.enable_tcp_interleaved) {
                LOG_WARN("RTSP SETUP rejected TCP transport because it is disabled: {}",
                         transport);
                SendRtsp(BuildResponse(461, "Unsupported Transport", cseq));
                return;
            }

            if (transport_spec.IsUdp()) {
                if (!owner_.config_.rtsp.enable_udp) {
                    LOG_WARN("RTSP SETUP rejected UDP transport because it is disabled: {}",
                             transport);
                    SendRtsp(BuildResponse(461, "Unsupported Transport", cseq));
                    return;
                }
            }

            if (transport_spec.mode == RtspTransportMode::UdpUnicast) {
                if (!ConfigureUdpTransport(transport_spec)) {
                    SendRtsp(BuildResponse(461, "Unsupported Transport", cseq));
                    return;
                }
            }

            if (transport_spec.mode == RtspTransportMode::UdpMulticast &&
                !owner_.ConfigureSharedMulticastTransport(transport_spec, LocalAddress())) {
                SendRtsp(BuildResponse(461, "Unsupported Transport", cseq));
                return;
            }

            transport_spec_ = transport_spec;
            rtp_channel_ = static_cast<int>(transport_spec_.rtp_channel);
            rtcp_channel_ = static_cast<int>(transport_spec_.rtcp_channel);
            ready_ = true;
            playing_ = false;

            SendRtsp(BuildResponse(
                200,
                "OK",
                cseq,
                {{"Session", session_id_},
                 {"Transport", transport_spec_.ToSetupResponseHeader()}}
            ));
            return;
        }

        if (upper_method == "PLAY") {
            if (!ready_) {
                SendRtsp(BuildResponse(455, "Method Not Valid In This State", cseq));
                return;
            }

            playing_ = true;
            const auto play_url = url.empty() ? requested_url_ : url;
            const auto rtp_sequence = IsMulticastTransport()
                ? owner_.GetMulticastSequence()
                : sequence_;
            const auto rtp_timestamp = IsMulticastTransport()
                ? owner_.GetMulticastLastRtpTimestamp()
                : last_rtp_timestamp_;
            SendRtsp(BuildResponse(
                200,
                "OK",
                cseq,
                {{"Session", session_id_},
                 {"RTP-Info",
                  "url=" + play_url + "/track0;seq=" +
                      std::to_string(rtp_sequence) + ";rtptime=" +
                      std::to_string(rtp_timestamp)}}));
            return;
        }

        if (upper_method == "PAUSE") {
            playing_ = false;
            SendRtsp(BuildResponse(200, "OK", cseq, {{"Session", session_id_}}));
            return;
        }

        if (upper_method == "GET_PARAMETER") {
            SendRtsp(BuildResponse(200, "OK", cseq, {{"Session", session_id_}}));
            return;
        }

        if (upper_method == "TEARDOWN") {
            playing_ = false;
            SendRtsp(BuildResponse(200, "OK", cseq, {{"Session", session_id_}}));
            Close();
            return;
        }

        SendRtsp(BuildResponse(405, "Method Not Allowed", cseq));
    }

    std::vector<std::uint8_t> BuildInterleavedFrame(
        const std::vector<std::uint8_t>& packet,
        int channel) const {
        std::vector<std::uint8_t> frame(kTcpInterleavedHeaderSize + packet.size());

        frame[0] = '$';
        frame[1] = static_cast<std::uint8_t>(channel);
        WriteU16(frame.data() + 2, static_cast<std::uint16_t>(packet.size()));

        std::copy(packet.begin(),
                  packet.end(),
                  frame.begin() +
                      static_cast<std::ptrdiff_t>(kTcpInterleavedHeaderSize));
        return frame;
    }

    std::vector<std::uint8_t> BuildInterleavedRtpFrame(
        const RtpPayload& payload,
        std::uint8_t payload_type) {
        return BuildInterleavedFrame(BuildRtpPacket(payload, payload_type),
                                     rtp_channel_);
    }

    std::vector<std::uint8_t> BuildRtpPacket(
        const RtpPayload& payload,
        std::uint8_t payload_type) {
        auto packet = MakeRtpPacket(payload,
                                    payload_type,
                                    ssrc_,
                                    sequence_,
                                    last_rtp_timestamp_);
        RecordRtcpSenderStats(payload);
        return packet;
    }

    void RecordRtcpSenderStats(const RtpPayload& payload) {
        // RTCP SR 的 sender octet count 只统计 RTP payload 字节数，不包含 RTP header。
        ++rtcp_packet_count_;
        rtcp_octet_count_ += static_cast<std::uint32_t>(payload.payload.size());
    }

    void SendTcpInterleavedRtpPayload(const RtpPayload& payload,
                                      std::uint8_t payload_type) {
        auto self = shared_from_this();
        auto frame = BuildInterleavedRtpFrame(payload, payload_type);
        boost::asio::post(socket_.get_executor(),
                          [self, frame = std::move(frame)]() mutable {
                              self->EnqueueWrite(std::move(frame));
                          });
        MaybeSendRtcpSenderReport();
    }

    void SendUdpRtpPayload(const RtpPayload& payload,
                           std::uint8_t payload_type) {
        auto self = shared_from_this();
        // UDP unicast 发送裸 RTP 包，不能带 RTSP over TCP 的 '$' 头。
        auto packet = std::make_shared<std::vector<std::uint8_t>>(
            BuildRtpPacket(payload, payload_type));

        boost::asio::post(
            socket_.get_executor(),
            [self, packet]() {
                if (!self->udp_rtp_socket_ || !self->udp_rtp_socket_->is_open()) {
                    return;
                }

                self->udp_rtp_socket_->async_send_to(
                    boost::asio::buffer(*packet),
                    self->udp_rtp_endpoint_,
                    [self, packet](boost::system::error_code ec, std::size_t) {
                        if (ec && !self->closed_) {
                            LOG_WARN("RTSP UDP RTP send failed: {}", ec.message());
                        }
                    });
            });
        MaybeSendRtcpSenderReport();
    }

    void MaybeSendRtcpSenderReport() {
        if (rtcp_packet_count_ == 0 ||
            !ShouldSendRtcpSenderReport(next_rtcp_sr_time_)) {
            return;
        }

        auto report = BuildRtcpSenderReport(ssrc_,
                                            last_rtp_timestamp_,
                                            rtcp_packet_count_,
                                            rtcp_octet_count_);
        if (transport_spec_.IsTcpInterleaved()) {
            auto self = shared_from_this();
            auto frame = BuildInterleavedFrame(report, rtcp_channel_);
            boost::asio::post(socket_.get_executor(),
                              [self, frame = std::move(frame)]() mutable {
                                  self->EnqueueWrite(std::move(frame));
                              });
            return;
        }

        if (transport_spec_.mode == RtspTransportMode::UdpUnicast) {
            SendUdpRtcpSenderReport(std::move(report));
        }
    }

    void SendUdpRtcpSenderReport(std::vector<std::uint8_t> report) {
        auto self = shared_from_this();
        auto packet =
            std::make_shared<std::vector<std::uint8_t>>(std::move(report));

        boost::asio::post(
            socket_.get_executor(),
            [self, packet]() {
                if (!self->udp_rtcp_socket_ || !self->udp_rtcp_socket_->is_open()) {
                    return;
                }

                self->udp_rtcp_socket_->async_send_to(
                    boost::asio::buffer(*packet),
                    self->udp_rtcp_endpoint_,
                    [self, packet](boost::system::error_code ec, std::size_t) {
                        if (ec && !self->closed_) {
                            LOG_WARN("RTSP UDP RTCP sender report send failed: {}",
                                     ec.message());
                        }
                    });
            });
    }

    void HandleRtcpPacket(const std::uint8_t* data,
                          std::size_t size,
                          const char* transport_name) {
        std::size_t offset = 0;
        while (offset + 4 <= size) {
            const auto* packet = data + offset;
            const auto version = static_cast<std::uint8_t>(packet[0] >> 6);
            const auto report_count = static_cast<std::uint8_t>(packet[0] & 0x1F);
            const auto packet_type = packet[1];
            const auto packet_size =
                (static_cast<std::size_t>(ReadU16(packet + 2)) + 1U) * 4U;

            if (version != 2 || packet_size < 4 || offset + packet_size > size) {
                LOG_DEBUG("RTSP RTCP packet ignored: session={}, transport={}, "
                          "version={}, size={}, remaining={}",
                          id_,
                          transport_name,
                          static_cast<int>(version),
                          packet_size,
                          size - offset);
                return;
            }

            if (packet_type == kRtcpReceiverReportPacketType) {
                ParseRtcpReceiverReport(packet, packet_size, report_count, transport_name);
            } else if (packet_type == kRtcpSenderReportPacketType) {
                ParseRtcpSenderReport(packet, packet_size, report_count, transport_name);
            } else {
                LOG_DEBUG("RTSP RTCP {} received: session={}, transport={}, size={}",
                          RtcpPacketTypeName(packet_type),
                          id_,
                          transport_name,
                          packet_size);
            }

            offset += packet_size;
        }

        if (offset != size) {
            LOG_DEBUG("RTSP RTCP trailing bytes ignored: session={}, transport={}, "
                      "bytes={}",
                      id_,
                      transport_name,
                      size - offset);
        }
    }

    void ParseRtcpReceiverReport(const std::uint8_t* packet,
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

        const auto reporter_ssrc = ReadU32(packet + 4);
        ParseRtcpReportBlocks(packet + 8,
                              report_count,
                              reporter_ssrc,
                              transport_name,
                              "RR");
    }

    void ParseRtcpSenderReport(const std::uint8_t* packet,
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

        const auto reporter_ssrc = ReadU32(packet + 4);
        ParseRtcpReportBlocks(packet + 28,
                              report_count,
                              reporter_ssrc,
                              transport_name,
                              "SR");
    }

    void ParseRtcpReportBlocks(const std::uint8_t* blocks,
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
            if (!ReadRtcpReportBlock(blocks + static_cast<std::size_t>(index) * 24U,
                                     24,
                                     block)) {
                return;
            }

            // Receiver Report 里的 source_ssrc 应该指向本 server 发送 RTP 使用的 SSRC。
            const bool matches_sender = block.source_ssrc == ssrc_;
            if (matches_sender || !has_receiver_report_) {
                last_receiver_report_ = block;
                last_receiver_reporter_ssrc_ = reporter_ssrc;
                has_receiver_report_ = true;
                ++receiver_reports_received_;
                // session 级 RR 最终汇总到 PublisherStats，便于外部观察客户端反馈是否到达。
                owner_.AddRtcpReceiverReportsReceived(1);
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

    void StartUdpRtcpReceive() {
        if (!udp_rtcp_socket_ || !udp_rtcp_socket_->is_open()) {
            return;
        }

        auto self = shared_from_this();
        udp_rtcp_socket_->async_receive_from(
            boost::asio::buffer(udp_rtcp_read_buffer_),
            udp_rtcp_sender_endpoint_,
            [self](boost::system::error_code ec, std::size_t bytes) {
                if (ec) {
                    if (ec != boost::asio::error::operation_aborted &&
                        !self->closed_) {
                        LOG_DEBUG("RTSP UDP RTCP receive failed: session={}, error={}",
                                  self->id_,
                                  ec.message());
                    }
                    return;
                }

                const bool expected_sender =
                    self->udp_rtcp_sender_endpoint_.address() ==
                        self->udp_rtcp_endpoint_.address() &&
                    self->udp_rtcp_sender_endpoint_.port() ==
                        self->udp_rtcp_endpoint_.port();
                if (expected_sender) {
                    self->HandleRtcpPacket(self->udp_rtcp_read_buffer_.data(),
                                           bytes,
                                           "udp-unicast");
                } else {
                    LOG_DEBUG("RTSP UDP RTCP ignored from unexpected endpoint: "
                              "session={}, endpoint={}:{}",
                              self->id_,
                              self->udp_rtcp_sender_endpoint_.address().to_string(),
                              self->udp_rtcp_sender_endpoint_.port());
                }

                if (!self->closed_) {
                    self->StartUdpRtcpReceive();
                }
            });
    }

    bool ConfigureUdpTransport(RtspTransportSpec& transport_spec) {
        CloseUdpSockets();

        boost::system::error_code ec;
        const auto remote_tcp_endpoint = socket_.remote_endpoint(ec);
        if (ec) {
            LOG_WARN("RTSP SETUP UDP failed to resolve client endpoint: {}",
                     ec.message());
            return false;
        }

        auto destination = remote_tcp_endpoint.address();
        if (!transport_spec.destination.empty()) {
            boost::system::error_code address_ec;
            auto configured_destination =
                boost::asio::ip::make_address(transport_spec.destination, address_ec);
            if (address_ec ||
                configured_destination.is_v6() != destination.is_v6()) {
                LOG_WARN("RTSP SETUP UDP rejected destination '{}': {}",
                         transport_spec.destination,
                         address_ec ? address_ec.message() : "address family mismatch");
                return false;
            }
            destination = configured_destination;
        }

        const auto protocol = destination.is_v6()
            ? boost::asio::ip::udp::v6()
            : boost::asio::ip::udp::v4();

        auto bind_address = destination.is_v6()
            ? boost::asio::ip::address{boost::asio::ip::address_v6::any()}
            : boost::asio::ip::address{boost::asio::ip::address_v4::any()};

        const auto local_tcp_endpoint = socket_.local_endpoint(ec);
        if (!ec && local_tcp_endpoint.address().is_v6() == destination.is_v6()) {
            bind_address = local_tcp_endpoint.address();
        }

        udp_rtp_socket_ =
            std::make_unique<boost::asio::ip::udp::socket>(socket_.get_executor());
        udp_rtcp_socket_ =
            std::make_unique<boost::asio::ip::udp::socket>(socket_.get_executor());

        udp_rtp_socket_->open(protocol, ec);
        if (ec) {
            LOG_WARN("RTSP SETUP UDP RTP socket open failed: {}", ec.message());
            CloseUdpSockets();
            return false;
        }

        udp_rtcp_socket_->open(protocol, ec);
        if (ec) {
            LOG_WARN("RTSP SETUP UDP RTCP socket open failed: {}", ec.message());
            CloseUdpSockets();
            return false;
        }

        udp_rtp_socket_->bind(boost::asio::ip::udp::endpoint(bind_address, 0), ec);
        if (ec) {
            LOG_WARN("RTSP SETUP UDP RTP socket bind failed: {}", ec.message());
            CloseUdpSockets();
            return false;
        }

        udp_rtcp_socket_->bind(boost::asio::ip::udp::endpoint(bind_address, 0), ec);
        if (ec) {
            LOG_WARN("RTSP SETUP UDP RTCP socket bind failed: {}", ec.message());
            CloseUdpSockets();
            return false;
        }

        const auto rtp_local_endpoint = udp_rtp_socket_->local_endpoint(ec);
        if (ec) {
            LOG_WARN("RTSP SETUP UDP RTP local endpoint failed: {}", ec.message());
            CloseUdpSockets();
            return false;
        }
        const auto rtcp_local_endpoint = udp_rtcp_socket_->local_endpoint(ec);
        if (ec) {
            LOG_WARN("RTSP SETUP UDP RTCP local endpoint failed: {}", ec.message());
            CloseUdpSockets();
            return false;
        }

        transport_spec.server_rtp_port = rtp_local_endpoint.port();
        transport_spec.server_rtcp_port = rtcp_local_endpoint.port();
        if (!rtp_local_endpoint.address().is_unspecified()) {
            transport_spec.source = rtp_local_endpoint.address().to_string();
        }
        udp_rtp_endpoint_ =
            boost::asio::ip::udp::endpoint(destination, transport_spec.client_rtp_port);
        udp_rtcp_endpoint_ =
            boost::asio::ip::udp::endpoint(destination, transport_spec.client_rtcp_port);

        LOG_INFO("RTSP SETUP UDP transport: client_rtp={}:{} server_rtp={}:{}",
                 udp_rtp_endpoint_.address().to_string(),
                 udp_rtp_endpoint_.port(),
                 rtp_local_endpoint.address().to_string(),
                 rtp_local_endpoint.port());
        StartUdpRtcpReceive();
        return true;
    }

    void SendRtsp(std::string response) {
        std::vector<std::uint8_t> bytes(response.begin(), response.end());
        EnqueueWrite(std::move(bytes));
    }

    void EnqueueWrite(std::vector<std::uint8_t> data) {
        const bool writing = !write_queue_.empty();
        write_queue_.push_back(std::move(data));
        if (!writing) {
            DoWrite();
        }
    }

    void DoWrite() {
        if (write_queue_.empty()) {
            return;
        }

        auto self = shared_from_this();
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(write_queue_.front()),
            [self](boost::system::error_code ec, std::size_t) {
                if (ec) {
                    self->Close();
                    return;
                }
                self->write_queue_.pop_front();
                self->DoWrite();
            });
    }

    std::string LocalAddress() const {
        boost::system::error_code ec;
        const auto endpoint = socket_.local_endpoint(ec);
        return ec ? "127.0.0.1" : endpoint.address().to_string();
    }

    void CloseUdpSockets() {
        boost::system::error_code ignored;
        if (udp_rtp_socket_) {
            udp_rtp_socket_->cancel(ignored);
            udp_rtp_socket_->close(ignored);
            udp_rtp_socket_.reset();
        }
        if (udp_rtcp_socket_) {
            udp_rtcp_socket_->cancel(ignored);
            udp_rtcp_socket_->close(ignored);
            udp_rtcp_socket_.reset();
        }
    }

    void Close() {
        if (closed_) {
            return;
        }
        closed_ = true;
        playing_ = false;
        ready_ = false;

        boost::system::error_code ignored;
        CloseUdpSockets();
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
        owner_.RemoveSession(id_);
    }

    boost::asio::ip::tcp::socket socket_;
    RtspServerProtocol& owner_;
    std::uint64_t id_{0};
    std::string session_id_;
    std::string requested_url_;
    std::array<std::uint8_t, 8192> read_chunk_{};
    std::vector<std::uint8_t> read_buffer_;
    std::deque<std::vector<std::uint8_t>> write_queue_;
    std::uint32_t ssrc_{0};
    std::uint16_t sequence_{0};
    std::uint32_t last_rtp_timestamp_{0};
    RtspTransportSpec transport_spec_;
    std::unique_ptr<boost::asio::ip::udp::socket> udp_rtp_socket_;
    std::unique_ptr<boost::asio::ip::udp::socket> udp_rtcp_socket_;
    boost::asio::ip::udp::endpoint udp_rtp_endpoint_;
    boost::asio::ip::udp::endpoint udp_rtcp_endpoint_;
    boost::asio::ip::udp::endpoint udp_rtcp_sender_endpoint_;
    std::array<std::uint8_t, 2048> udp_rtcp_read_buffer_{};
    std::uint32_t rtcp_packet_count_{0};
    std::uint32_t rtcp_octet_count_{0};
    std::chrono::steady_clock::time_point next_rtcp_sr_time_{};
    RtcpReportBlock last_receiver_report_{};
    std::uint32_t last_receiver_reporter_ssrc_{0};
    std::uint64_t receiver_reports_received_{0};
    bool has_receiver_report_{false};
    int rtp_channel_{0};
    int rtcp_channel_{1};
    bool ready_{false};
    std::atomic_bool playing_{false};
    bool closed_{false};
};

RtspServerProtocol::RtspServerProtocol()
    : h264_packetizer_(1420) {
}

RtspServerProtocol::~RtspServerProtocol() {
    Stop();
}

bool RtspServerProtocol::ConfigureSharedMulticastTransport(
    RtspTransportSpec& transport_spec,
    const std::string& source_address) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (multicast_ready_) {
        // 组播是共享传输，后续客户端复用同一个组播地址、端口和 RTP 序列空间。
        transport_spec = multicast_transport_spec_;
        return true;
    }

    auto destination_text = transport_spec.destination.empty()
        ? config_.rtsp.multicast_address
        : transport_spec.destination;
    if (destination_text.empty()) {
        LOG_WARN("RTSP SETUP multicast rejected: empty destination");
        return false;
    }

    boost::system::error_code ec;
    const auto destination = boost::asio::ip::make_address(destination_text, ec);
    if (ec || !IsIpv4Multicast(destination)) {
        LOG_WARN("RTSP SETUP multicast rejected destination '{}': {}",
                 destination_text,
                 ec ? ec.message() : "not an IPv4 multicast address");
        return false;
    }

    if (transport_spec.server_rtp_port == 0) {
        transport_spec.server_rtp_port = config_.rtsp.multicast_rtp_port;
    }
    if (transport_spec.server_rtcp_port == 0) {
        transport_spec.server_rtcp_port = config_.rtsp.multicast_rtcp_port;
    }
    if (transport_spec.server_rtp_port == 0 ||
        transport_spec.server_rtcp_port == 0) {
        LOG_WARN("RTSP SETUP multicast rejected: invalid RTP/RTCP port");
        return false;
    }
    if (transport_spec.ttl == 0) {
        transport_spec.ttl = config_.rtsp.multicast_ttl == 0
            ? std::uint8_t{16}
            : config_.rtsp.multicast_ttl;
    }

    auto bind_address =
        boost::asio::ip::address{boost::asio::ip::address_v4::any()};
    boost::system::error_code source_ec;
    const auto source = boost::asio::ip::make_address(source_address, source_ec);
    if (!source_ec && source.is_v4()) {
        bind_address = source;
        transport_spec.source = source.to_string();
    }

    auto rtp_socket = std::make_shared<boost::asio::ip::udp::socket>(io_);
    auto rtcp_socket = std::make_shared<boost::asio::ip::udp::socket>(io_);
    auto rtcp_receiver_socket =
        std::make_shared<boost::asio::ip::udp::socket>(io_);

    rtp_socket->open(boost::asio::ip::udp::v4(), ec);
    if (ec) {
        LOG_WARN("RTSP SETUP multicast RTP socket open failed: {}", ec.message());
        return false;
    }

    rtcp_socket->open(boost::asio::ip::udp::v4(), ec);
    if (ec) {
        LOG_WARN("RTSP SETUP multicast RTCP socket open failed: {}", ec.message());
        return false;
    }

    rtcp_receiver_socket->open(boost::asio::ip::udp::v4(), ec);
    if (ec) {
        LOG_WARN("RTSP SETUP multicast RTCP receiver socket open failed: {}",
                 ec.message());
        return false;
    }

    // RTP 和 RTCP SR 是发送 socket：绑定本地源地址的临时端口，然后发到组播组。
    rtp_socket->bind(boost::asio::ip::udp::endpoint(bind_address, 0), ec);
    if (ec) {
        LOG_WARN("RTSP SETUP multicast RTP socket bind failed: {}", ec.message());
        return false;
    }

    rtcp_socket->bind(boost::asio::ip::udp::endpoint(bind_address, 0), ec);
    if (ec) {
        LOG_WARN("RTSP SETUP multicast RTCP socket bind failed: {}", ec.message());
        return false;
    }

    // RTCP RR 是接收 socket：必须绑定标准组播 RTCP 端口并 join group。
    rtcp_receiver_socket->set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (ec) {
        LOG_WARN("RTSP SETUP multicast RTCP receiver set reuse failed: {}",
                 ec.message());
    }
    rtcp_receiver_socket->bind(
        boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(),
                                       transport_spec.server_rtcp_port),
        ec);
    if (ec) {
        LOG_WARN("RTSP SETUP multicast RTCP receiver bind failed: {}", ec.message());
        return false;
    }

    if (bind_address.is_v4() &&
        bind_address.to_v4() != boost::asio::ip::address_v4::any()) {
        rtcp_receiver_socket->set_option(
            boost::asio::ip::multicast::join_group(destination.to_v4(),
                                                   bind_address.to_v4()),
            ec);
        if (ec) {
            rtcp_receiver_socket->set_option(
                boost::asio::ip::multicast::join_group(destination.to_v4()),
                ec);
        }
    } else {
        rtcp_receiver_socket->set_option(
            boost::asio::ip::multicast::join_group(destination.to_v4()),
            ec);
    }
    if (ec) {
        LOG_WARN("RTSP SETUP multicast RTCP receiver join group failed: {}",
                 ec.message());
        return false;
    }

    for (auto* socket : {rtp_socket.get(), rtcp_socket.get()}) {
        socket->set_option(boost::asio::ip::multicast::hops(transport_spec.ttl), ec);
        if (ec) {
            LOG_WARN("RTSP SETUP multicast set TTL failed: {}", ec.message());
        }
        socket->set_option(boost::asio::ip::multicast::enable_loopback(true), ec);
        if (ec) {
            LOG_WARN("RTSP SETUP multicast enable loopback failed: {}", ec.message());
        }
        if (bind_address.is_v4() &&
            bind_address.to_v4() != boost::asio::ip::address_v4::any()) {
            socket->set_option(
                boost::asio::ip::multicast::outbound_interface(bind_address.to_v4()),
                ec);
            if (ec) {
                LOG_WARN("RTSP SETUP multicast outbound interface failed: {}",
                         ec.message());
            }
        }
    }

    transport_spec.destination = destination.to_string();
    multicast_rtp_endpoint_ =
        boost::asio::ip::udp::endpoint(destination, transport_spec.server_rtp_port);
    multicast_rtcp_endpoint_ =
        boost::asio::ip::udp::endpoint(destination, transport_spec.server_rtcp_port);
    multicast_transport_spec_ = transport_spec;
    multicast_rtp_socket_ = std::move(rtp_socket);
    multicast_rtcp_socket_ = std::move(rtcp_socket);
    multicast_rtcp_receiver_socket_ = std::move(rtcp_receiver_socket);
    multicast_ssrc_ = RandomU32();
    multicast_sequence_ = RandomU16();
    multicast_last_rtp_timestamp_ = 0;
    multicast_rtcp_packet_count_ = 0;
    multicast_rtcp_octet_count_ = 0;
    multicast_next_rtcp_sr_time_ = {};
    multicast_rtcp_feedback_by_reporter_.clear();
    multicast_ready_ = true;

    const auto log_multicast_address = multicast_rtp_endpoint_.address().to_string();
    const auto log_multicast_port = multicast_rtp_endpoint_.port();
    const auto log_multicast_ttl = transport_spec.ttl;
    lock.unlock();
    StartMulticastRtcpReceive();

    LOG_INFO("RTSP SETUP shared multicast transport: destination={}:{} ttl={}",
             log_multicast_address,
             log_multicast_port,
             static_cast<int>(log_multicast_ttl));
    return true;
}

void RtspServerProtocol::CloseSharedMulticastTransport() {
    std::lock_guard<std::mutex> lock(mutex_);
    boost::system::error_code ignored;
    if (multicast_rtp_socket_) {
        multicast_rtp_socket_->cancel(ignored);
        multicast_rtp_socket_->close(ignored);
        multicast_rtp_socket_.reset();
    }
    if (multicast_rtcp_socket_) {
        multicast_rtcp_socket_->cancel(ignored);
        multicast_rtcp_socket_->close(ignored);
        multicast_rtcp_socket_.reset();
    }
    if (multicast_rtcp_receiver_socket_) {
        multicast_rtcp_receiver_socket_->cancel(ignored);
        multicast_rtcp_receiver_socket_->close(ignored);
        multicast_rtcp_receiver_socket_.reset();
    }
    multicast_transport_spec_ = {};
    multicast_rtcp_packet_count_ = 0;
    multicast_rtcp_octet_count_ = 0;
    multicast_next_rtcp_sr_time_ = {};
    multicast_rtcp_feedback_by_reporter_.clear();
    multicast_ready_ = false;
}

void RtspServerProtocol::SendMulticastRtpPayload(
    const RtpPayload& payload,
    std::uint8_t payload_type) {
    std::shared_ptr<boost::asio::ip::udp::socket> socket;
    std::shared_ptr<std::vector<std::uint8_t>> packet;
    boost::asio::ip::udp::endpoint endpoint;
    bool send_sender_report = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!multicast_ready_ || !multicast_rtp_socket_ ||
            !multicast_rtp_socket_->is_open()) {
            return;
        }

        socket = multicast_rtp_socket_;
        endpoint = multicast_rtp_endpoint_;
        // multicast 必须使用共享 SSRC/sequence，不能按每个 RTSP client 单独递增。
        packet = std::make_shared<std::vector<std::uint8_t>>(
            MakeRtpPacket(payload,
                          payload_type,
                          multicast_ssrc_,
                          multicast_sequence_,
                          multicast_last_rtp_timestamp_));
        // multicast 的 RTCP SR 也必须按共享 sender 统计，不能按 RTSP 会话重复累计。
        ++multicast_rtcp_packet_count_;
        multicast_rtcp_octet_count_ +=
            static_cast<std::uint32_t>(payload.payload.size());
        send_sender_report =
            ShouldSendRtcpSenderReport(multicast_next_rtcp_sr_time_);
    }

    boost::asio::post(
        io_,
        [socket, endpoint, packet]() {
            if (!socket || !socket->is_open()) {
                return;
            }

            socket->async_send_to(
                boost::asio::buffer(*packet),
                endpoint,
                [packet](boost::system::error_code ec, std::size_t) {
                    if (ec && ec != boost::asio::error::operation_aborted) {
                        LOG_WARN("RTSP multicast RTP send failed: {}", ec.message());
                    }
                });
        });

    if (send_sender_report) {
        SendMulticastRtcpSenderReport();
    }
}

void RtspServerProtocol::SendMulticastRtcpSenderReport() {
    std::shared_ptr<boost::asio::ip::udp::socket> socket;
    std::shared_ptr<std::vector<std::uint8_t>> report;
    boost::asio::ip::udp::endpoint endpoint;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!multicast_ready_ || !multicast_rtcp_socket_ ||
            !multicast_rtcp_socket_->is_open() ||
            multicast_rtcp_packet_count_ == 0) {
            return;
        }

        socket = multicast_rtcp_socket_;
        endpoint = multicast_rtcp_endpoint_;
        report = std::make_shared<std::vector<std::uint8_t>>(
            BuildRtcpSenderReport(multicast_ssrc_,
                                  multicast_last_rtp_timestamp_,
                                  multicast_rtcp_packet_count_,
                                  multicast_rtcp_octet_count_));
    }

    boost::asio::post(
        io_,
        [socket, endpoint, report]() {
            if (!socket || !socket->is_open()) {
                return;
            }

            socket->async_send_to(
                boost::asio::buffer(*report),
                endpoint,
                [report](boost::system::error_code ec, std::size_t) {
                    if (ec && ec != boost::asio::error::operation_aborted) {
                        LOG_WARN("RTSP multicast RTCP sender report send failed: {}",
                                 ec.message());
                    }
                });
        });
}

void RtspServerProtocol::StartMulticastRtcpReceive() {
    std::shared_ptr<boost::asio::ip::udp::socket> socket;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!multicast_ready_ || !multicast_rtcp_receiver_socket_ ||
            !multicast_rtcp_receiver_socket_->is_open()) {
            return;
        }
        socket = multicast_rtcp_receiver_socket_;
    }

    // multicast RR 接收是 protocol 级共享状态；一次只挂一个 receive，完成后再续订。
    socket->async_receive_from(
        boost::asio::buffer(multicast_rtcp_read_buffer_),
        multicast_rtcp_sender_endpoint_,
        [this, socket](boost::system::error_code ec, std::size_t bytes) {
            if (ec) {
                if (ec != boost::asio::error::operation_aborted) {
                    LOG_DEBUG("RTSP multicast RTCP receive failed: {}",
                              ec.message());
                }
                return;
            }

            const auto sender_endpoint = multicast_rtcp_sender_endpoint_;
            HandleMulticastRtcpPacket(multicast_rtcp_read_buffer_.data(),
                                      bytes,
                                      sender_endpoint);

            bool should_continue = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                should_continue = started_ && multicast_ready_ &&
                                  multicast_rtcp_receiver_socket_ == socket;
            }
            if (should_continue) {
                StartMulticastRtcpReceive();
            }
        });
}

void RtspServerProtocol::HandleMulticastRtcpPacket(
    const std::uint8_t* data,
    std::size_t size,
    const boost::asio::ip::udp::endpoint& sender_endpoint) {
    std::size_t offset = 0;
    while (offset + 4 <= size) {
        const auto* packet = data + offset;
        const auto version = static_cast<std::uint8_t>(packet[0] >> 6);
        const auto report_count = static_cast<std::uint8_t>(packet[0] & 0x1F);
        const auto packet_type = packet[1];
        const auto packet_size =
            (static_cast<std::size_t>(ReadU16(packet + 2)) + 1U) * 4U;

        if (version != 2 || packet_size < 4 || offset + packet_size > size) {
            LOG_DEBUG("RTSP multicast RTCP packet ignored: endpoint={}:{} "
                      "version={}, size={}, remaining={}",
                      sender_endpoint.address().to_string(),
                      sender_endpoint.port(),
                      static_cast<int>(version),
                      packet_size,
                      size - offset);
            return;
        }

        if (packet_type == kRtcpReceiverReportPacketType) {
            ParseMulticastRtcpReceiverReport(packet,
                                             packet_size,
                                             report_count,
                                             sender_endpoint);
        } else if (packet_type == kRtcpSenderReportPacketType) {
            ParseMulticastRtcpSenderReport(packet,
                                           packet_size,
                                           report_count,
                                           sender_endpoint);
        } else {
            // SDES/BYE/APP 暂时只识别并忽略，保留日志用于排查播放器行为。
            LOG_DEBUG("RTSP multicast RTCP {} received: endpoint={}:{} size={}",
                      RtcpPacketTypeName(packet_type),
                      sender_endpoint.address().to_string(),
                      sender_endpoint.port(),
                      packet_size);
        }

        offset += packet_size;
    }

    if (offset != size) {
        LOG_DEBUG("RTSP multicast RTCP trailing bytes ignored: endpoint={}:{} "
                  "bytes={}",
                  sender_endpoint.address().to_string(),
                  sender_endpoint.port(),
                  size - offset);
    }
}

void RtspServerProtocol::ParseMulticastRtcpReceiverReport(
    const std::uint8_t* packet,
    std::size_t packet_size,
    std::uint8_t report_count,
    const boost::asio::ip::udp::endpoint& sender_endpoint) {
    if (packet_size < 8U + static_cast<std::size_t>(report_count) * 24U) {
        LOG_DEBUG("RTSP multicast RTCP RR ignored: endpoint={}:{} size={}, rc={}",
                  sender_endpoint.address().to_string(),
                  sender_endpoint.port(),
                  packet_size,
                  static_cast<int>(report_count));
        return;
    }

    const auto reporter_ssrc = ReadU32(packet + 4);
    ParseMulticastRtcpReportBlocks(packet + 8,
                                   report_count,
                                   reporter_ssrc,
                                   "RR",
                                   sender_endpoint);
}

void RtspServerProtocol::ParseMulticastRtcpSenderReport(
    const std::uint8_t* packet,
    std::size_t packet_size,
    std::uint8_t report_count,
    const boost::asio::ip::udp::endpoint& sender_endpoint) {
    if (packet_size < 28U + static_cast<std::size_t>(report_count) * 24U) {
        LOG_DEBUG("RTSP multicast RTCP SR ignored: endpoint={}:{} size={}, rc={}",
                  sender_endpoint.address().to_string(),
                  sender_endpoint.port(),
                  packet_size,
                  static_cast<int>(report_count));
        return;
    }

    const auto reporter_ssrc = ReadU32(packet + 4);
    ParseMulticastRtcpReportBlocks(packet + 28,
                                   report_count,
                                   reporter_ssrc,
                                   "SR",
                                   sender_endpoint);
}

void RtspServerProtocol::ParseMulticastRtcpReportBlocks(
    const std::uint8_t* blocks,
    std::uint8_t report_count,
    std::uint32_t reporter_ssrc,
    const char* packet_type_name,
    const boost::asio::ip::udp::endpoint& sender_endpoint) {
    if (report_count == 0) {
        LOG_DEBUG("RTSP multicast RTCP {} received without report blocks: "
                  "endpoint={}:{} reporter_ssrc={}",
                  packet_type_name,
                  sender_endpoint.address().to_string(),
                  sender_endpoint.port(),
                  reporter_ssrc);
        return;
    }

    for (std::uint8_t index = 0; index < report_count; ++index) {
        RtcpReportBlock block;
        if (!ReadRtcpReportBlock(blocks + static_cast<std::size_t>(index) * 24U,
                                 24,
                                 block)) {
            return;
        }

        bool matches_sender = false;
        std::uint64_t reports_received = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            matches_sender = multicast_ready_ &&
                             block.source_ssrc == multicast_ssrc_;
            if (matches_sender) {
                // 每个 multicast receiver 用自己的 reporter SSRC 聚合最新反馈。
                auto& feedback = multicast_rtcp_feedback_by_reporter_[reporter_ssrc];
                feedback.media_ssrc = block.source_ssrc;
                feedback.fraction_lost = block.fraction_lost;
                feedback.cumulative_lost = block.cumulative_lost;
                feedback.extended_highest_sequence =
                    block.extended_highest_sequence;
                feedback.jitter = block.jitter;
                feedback.last_sender_report = block.last_sender_report;
                feedback.delay_since_last_sender_report =
                    block.delay_since_last_sender_report;
                reports_received = ++feedback.reports_received;
                ++stats_.rtcp_receiver_reports_received;
            }
        }

        LOG_DEBUG("RTSP multicast RTCP {} block: endpoint={}:{} reporter_ssrc={}, "
                  "media_ssrc={}, match={}, receiver_reports={}, fraction_lost={}, "
                  "cumulative_lost={}, highest_seq={}, jitter={}, lsr={}, dlsr={}",
                  packet_type_name,
                  sender_endpoint.address().to_string(),
                  sender_endpoint.port(),
                  reporter_ssrc,
                  block.source_ssrc,
                  matches_sender,
                  reports_received,
                  static_cast<int>(block.fraction_lost),
                  block.cumulative_lost,
                  block.extended_highest_sequence,
                  block.jitter,
                  block.last_sender_report,
                  block.delay_since_last_sender_report);
    }
}

std::uint16_t RtspServerProtocol::GetMulticastSequence() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return multicast_sequence_;
}

std::uint32_t RtspServerProtocol::GetMulticastLastRtpTimestamp() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return multicast_last_rtp_timestamp_;
}

bool RtspServerProtocol::Start(const PublisherConfig& config,
                               const std::vector<MediaTrackConfig>& tracks) {
    Stop();

    if (tracks.empty() || tracks.front().codec_type != CodecType::H264 ||
        (!config.rtsp.enable_tcp_interleaved && !config.rtsp.enable_udp)) {
        LOG_ERROR("RtspServerProtocol: supports H264 with RTSP TCP interleaved or UDP");
        return false;
    }

    config_ = config;
    tracks_ = tracks;
    stats_ = {};
    h264_packetizer_ = H264RtpPacketizer(
        config_.rtsp.max_payload_size > 0
            ? static_cast<std::size_t>(config_.rtsp.max_payload_size)
            : 1420);
    h264_parameter_sets_ = H264Bitstream::ExtractParameterSets(tracks_.front().extra_data);

    boost::system::error_code ec;
    auto address = boost::asio::ip::make_address(config_.listen_host, ec);
    if (ec) {
        LOG_ERROR("RtspServerProtocol: invalid listen host {}: {}",
                  config_.listen_host,
                  ec.message());
        return false;
    }

    boost::asio::ip::tcp::endpoint endpoint(address, config_.listen_port);
    acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(io_);
    acceptor_->open(endpoint.protocol(), ec);
    if (ec) {
        LOG_ERROR("RtspServerProtocol: acceptor open failed: {}", ec.message());
        Stop();
        return false;
    }

    acceptor_->set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (ec) {
        LOG_WARN("RtspServerProtocol: set reuse_address failed: {}", ec.message());
    }

    acceptor_->bind(endpoint, ec);
    if (ec) {
        LOG_ERROR("RtspServerProtocol: bind {}:{} failed: {}",
                  config_.listen_host,
                  config_.listen_port,
                  ec.message());
        Stop();
        return false;
    }

    acceptor_->listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        LOG_ERROR("RtspServerProtocol: listen failed: {}", ec.message());
        Stop();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = true;
    }

    work_guard_ = std::make_unique<WorkGuard>(io_.get_executor());
    AcceptNext();
    io_thread_ = std::thread([this]() {
        io_.run();
    });

    LOG_INFO("RtspServerProtocol: started {}", GetOutputUrl());
    return true;
}

bool RtspServerProtocol::Write(const EncodedAccessUnit& access_unit) {
    if (access_unit.codec_type != CodecType::H264 || access_unit.nals.empty()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return false;
        }
    }

    UpdateH264ParameterSets(access_unit);

    const auto packets = h264_packetizer_.Packetize(access_unit);
    if (packets.empty()) {
        return false;
    }

    std::vector<std::shared_ptr<ClientSession>> sessions;
    SnapshotSessions(sessions);
    const auto payload_type = tracks_.empty() ? std::uint8_t{96}
                                             : tracks_.front().rtp_payload_type;
    const auto has_multicast_receiver =
        std::any_of(sessions.begin(), sessions.end(), [](const auto& session) {
            return session && session->IsPlayingMulticast();
        });

    for (const auto& packet : packets) {
        for (const auto& session : sessions) {
            if (session && session->IsPlaying() &&
                !session->IsMulticastTransport()) {
                session->SendRtpPayload(packet, payload_type);
            }
        }
        if (has_multicast_receiver) {
            SendMulticastRtpPayload(packet, payload_type);
        }
    }

    const auto data_size = access_unit.encoded_data
        ? access_unit.encoded_data->Size()
        : [&access_unit]() {
              std::size_t total = 0;
              for (const auto& nal : access_unit.nals) {
                  total += nal.data.size();
              }
              return total;
          }();

    std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.packets_published;
    stats_.bytes_published += data_size;
    stats_.clients_connected = sessions.size();
    return true;
}

void RtspServerProtocol::Stop() {
    std::vector<std::shared_ptr<ClientSession>> sessions;
    SnapshotSessions(sessions);
    for (auto& session : sessions) {
        if (session) {
            boost::asio::post(io_, [session]() {
                session->Stop();
            });
        }
    }

    boost::system::error_code ignored;
    if (acceptor_) {
        acceptor_->cancel(ignored);
        acceptor_->close(ignored);
        acceptor_.reset();
    }
    CloseSharedMulticastTransport();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = false;
        sessions_.clear();
    }

    work_guard_.reset();
    io_.stop();
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    io_.restart();
}

std::string RtspServerProtocol::GetOutputUrl() const {
    if (!config_.url.empty()) {
        return config_.url;
    }

    const auto host = (config_.listen_host.empty() || config_.listen_host == "0.0.0.0")
        ? std::string{"127.0.0.1"}
        : config_.listen_host;

    return "rtsp://" + host + ":" + std::to_string(config_.listen_port) +
           config_.stream_path;
}

PublisherStats RtspServerProtocol::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto stats = stats_;
    std::vector<std::shared_ptr<ClientSession>> sessions;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (auto session = it->lock()) {
            sessions.push_back(std::move(session));
            ++it;
        } else {
            it = sessions_.erase(it);
        }
    }
    stats.clients_connected = sessions.size();
    return stats;
}

void RtspServerProtocol::AddRtcpReceiverReportsReceived(std::uint64_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.rtcp_receiver_reports_received += count;
}

std::string RtspServerProtocol::BuildSdp(const std::string& host_for_sdp) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto& track = tracks_.front();

    boost::system::error_code address_ec;
    const auto multicast_address =
        boost::asio::ip::make_address(config_.rtsp.multicast_address, address_ec);
    const bool use_multicast_sdp =
        config_.rtsp.enable_udp &&
        !address_ec &&
        IsIpv4Multicast(multicast_address) &&
        config_.rtsp.multicast_rtp_port != 0;
    const auto multicast_ttl = config_.rtsp.multicast_ttl == 0
        ? std::uint8_t{16}
        : config_.rtsp.multicast_ttl;

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

    oss << "m=video "
        << (use_multicast_sdp ? config_.rtsp.multicast_rtp_port : 0)
        << " RTP/AVP " << static_cast<int>(track.rtp_payload_type) << "\r\n"
        << "a=rtpmap:" << static_cast<int>(track.rtp_payload_type)
        << " H264/" << track.rtp_clock_rate << "\r\n";

    oss << "a=fmtp:" << static_cast<int>(track.rtp_payload_type)
        << " packetization-mode=1;profile-level-id="
        << ProfileLevelId(h264_parameter_sets_.sps);

    if (h264_parameter_sets_.HasBoth()) {
        oss << ";sprop-parameter-sets="
            << H264Bitstream::Base64Encode(h264_parameter_sets_.sps)
            << ","
            << H264Bitstream::Base64Encode(h264_parameter_sets_.pps);
    }
    oss << "\r\n";
    oss << "a=control:track0\r\n";
    return oss.str();
}

void RtspServerProtocol::RemoveSession(std::uint64_t session_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    sessions_.erase(
        std::remove_if(sessions_.begin(),
                       sessions_.end(),
                       [session_id](const std::weak_ptr<ClientSession>& weak) {
                           auto session = weak.lock();
                           return !session || session->Id() == session_id;
                       }),
        sessions_.end());
    stats_.clients_connected = sessions_.size();
}

void RtspServerProtocol::AcceptNext() {
    if (!acceptor_) {
        return;
    }

    acceptor_->async_accept(
        [this](boost::system::error_code ec, boost::asio::ip::tcp::socket socket) {
            OnAccepted(ec, std::move(socket));
        });
}

void RtspServerProtocol::OnAccepted(boost::system::error_code ec,
                                    boost::asio::ip::tcp::socket socket) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return;
        }
    }

    if (!ec) {
        std::uint64_t session_id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            session_id = next_session_id_++;
        }

        auto session = std::make_shared<ClientSession>(std::move(socket), *this, session_id);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions_.push_back(session);
            stats_.clients_connected = sessions_.size();
        }
        session->Start();
    } else if (ec != boost::asio::error::operation_aborted) {
        LOG_WARN("RtspServerProtocol: accept failed: {}", ec.message());
    }

    AcceptNext();
}

void RtspServerProtocol::SnapshotSessions(
    std::vector<std::shared_ptr<ClientSession>>& sessions) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (auto session = it->lock()) {
            sessions.push_back(std::move(session));
            ++it;
        } else {
            it = sessions_.erase(it);
        }
    }
}

void RtspServerProtocol::UpdateH264ParameterSets(
    const EncodedAccessUnit& access_unit) {
    const auto sets = H264Bitstream::ExtractParameterSets(access_unit.nals);
    if (!sets.sps.empty() || !sets.pps.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sets.sps.empty()) {
            h264_parameter_sets_.sps = sets.sps;
        }
        if (!sets.pps.empty()) {
            h264_parameter_sets_.pps = sets.pps;
        }
    }
}
