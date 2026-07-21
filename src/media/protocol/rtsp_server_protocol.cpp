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
                const auto length = static_cast<std::uint16_t>(
                    (read_buffer_[2] << 8) | read_buffer_[3]);
                const auto frame_size = kTcpInterleavedHeaderSize + length;
                if (read_buffer_.size() < frame_size) {
                    return;
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

    std::vector<std::uint8_t> BuildInterleavedRtpFrame(
        const RtpPayload& payload,
        std::uint8_t payload_type) {
        auto packet = BuildRtpPacket(payload, payload_type);
        std::vector<std::uint8_t> frame(kTcpInterleavedHeaderSize + packet.size());

        frame[0] = '$';
        frame[1] = static_cast<std::uint8_t>(rtp_channel_);
        WriteU16(frame.data() + 2, static_cast<std::uint16_t>(packet.size()));

        std::copy(packet.begin(),
                  packet.end(),
                  frame.begin() +
                      static_cast<std::ptrdiff_t>(kTcpInterleavedHeaderSize));
        return frame;
    }

    std::vector<std::uint8_t> BuildRtpPacket(
        const RtpPayload& payload,
        std::uint8_t payload_type) {
        return MakeRtpPacket(payload,
                             payload_type,
                             ssrc_,
                             sequence_,
                             last_rtp_timestamp_);
    }

    void SendTcpInterleavedRtpPayload(const RtpPayload& payload,
                                      std::uint8_t payload_type) {
        auto self = shared_from_this();
        auto frame = BuildInterleavedRtpFrame(payload, payload_type);
        boost::asio::post(socket_.get_executor(),
                          [self, frame = std::move(frame)]() mutable {
                              self->EnqueueWrite(std::move(frame));
                          });
    }

    void SendUdpRtpPayload(const RtpPayload& payload,
                           std::uint8_t payload_type) {
        auto self = shared_from_this();
        // UDP unicast 和 multicast 都发送裸 RTP 包，不能带 RTSP over TCP 的 '$' 头。
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
    std::lock_guard<std::mutex> lock(mutex_);
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

    // 发送端不需要 join group；这里只绑定本地源地址并设置 multicast 选项。
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
    multicast_ssrc_ = RandomU32();
    multicast_sequence_ = RandomU16();
    multicast_last_rtp_timestamp_ = 0;
    multicast_ready_ = true;

    LOG_INFO("RTSP SETUP shared multicast transport: destination={}:{} ttl={}",
             multicast_rtp_endpoint_.address().to_string(),
             multicast_rtp_endpoint_.port(),
             static_cast<int>(transport_spec.ttl));
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
    multicast_transport_spec_ = {};
    multicast_ready_ = false;
}

void RtspServerProtocol::SendMulticastRtpPayload(
    const RtpPayload& payload,
    std::uint8_t payload_type) {
    std::shared_ptr<boost::asio::ip::udp::socket> socket;
    std::shared_ptr<std::vector<std::uint8_t>> packet;
    boost::asio::ip::udp::endpoint endpoint;
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
