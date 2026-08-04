#include "media/protocol/rtsp_server_protocol.h"

#include "common/log/logger.h"
#include "media/protocol/rtsp/rtsp_request_parser.h"
#include "media/protocol/rtsp/rtsp_transport_spec.h"

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
constexpr std::size_t kAacAuHeaderSize = 4;

struct RtcpReportBlock {
    std::uint32_t source_ssrc{0};
    std::uint8_t fraction_lost{0};
    std::int32_t cumulative_lost{0};
    std::uint32_t extended_highest_sequence{0};
    std::uint32_t jitter{0};
    std::uint32_t last_sender_report{0};
    std::uint32_t delay_since_last_sender_report{0};
};

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

bool ExtractCSeq(const RtspRequest& request, std::string& cseq) {
    // CSeq 是 RTSP 每条请求都必须携带的关联号。解析器保留重复字段，
    // 这里显式要求唯一且只含数字，防止响应被错误关联到客户端事务。
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

bool ParseTrackIdFromUrl(const std::string& url, int& track_id) {
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

const MediaTrackConfig* FindTrackById(const std::vector<MediaTrackConfig>& tracks,
                                      int track_id) {
    auto it = std::find_if(tracks.begin(), tracks.end(), [track_id](const auto& track) {
        return track.track_id == track_id;
    });
    return it == tracks.end() ? nullptr : &*it;
}

bool IsRtspServerSupportedTrack(const MediaTrackConfig& track) {
    if (track.media_type == MediaType::VIDEO) {
        return track.codec_type == CodecType::H264;
    }
    if (track.media_type == MediaType::AUDIO) {
        return track.codec_type == CodecType::AAC ||
               track.codec_type == CodecType::G711A ||
               track.codec_type == CodecType::G711U;
    }
    return false;
}

void NormalizeRtspTrackDefaults(std::vector<MediaTrackConfig>& tracks) {
    std::array<bool, 128> used_payload_types{};
    std::uint8_t next_dynamic_payload_type = 96;

    const auto allocate_dynamic_payload_type = [&]() {
        while (next_dynamic_payload_type < 128 &&
               used_payload_types[next_dynamic_payload_type]) {
            ++next_dynamic_payload_type;
        }
        return next_dynamic_payload_type < 128 ? next_dynamic_payload_type++ : 96;
    };

    for (auto& track : tracks) {
        if (track.media_type == MediaType::AUDIO) {
            // 音频 RTP timestamp 使用采样率时钟；旧配置默认 90000 是视频时钟，不能直接沿用。
            track.rtp_clock_rate = track.sample_rate > 0
                ? static_cast<std::uint32_t>(track.sample_rate)
                : track.rtp_clock_rate;
        } else if (track.rtp_clock_rate == 0) {
            track.rtp_clock_rate = 90000;
        }

        auto desired_payload_type = track.rtp_payload_type;
        if (track.media_type == MediaType::AUDIO &&
            track.codec_type == CodecType::G711A &&
            desired_payload_type == 96 &&
            track.sample_rate == 8000) {
            // RFC 3551 的静态 PT 8 只表示 8kHz PCMA；16kHz 等宽带语音必须使用动态 PT。
            desired_payload_type = 8;
        } else if (track.media_type == MediaType::AUDIO &&
                   track.codec_type == CodecType::G711U &&
                   desired_payload_type == 96 &&
                   track.sample_rate == 8000) {
            // RFC 3551 的静态 PT 0 只表示 8kHz PCMU；其它采样率走动态 payload type。
            desired_payload_type = 0;
        }

        if (desired_payload_type >= used_payload_types.size() ||
            used_payload_types[desired_payload_type]) {
            desired_payload_type = allocate_dynamic_payload_type();
        }
        track.rtp_payload_type = desired_payload_type;
        if (track.rtp_payload_type < used_payload_types.size()) {
            used_payload_types[track.rtp_payload_type] = true;
        }
    }
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

    // AAC-LC AudioSpecificConfig: audioObjectType=2, samplingFrequencyIndex, channelConfiguration。
    const auto config = static_cast<std::uint16_t>(
        (2U << 11U) |
        (static_cast<std::uint16_t>(sampling_index) << 7U) |
        (static_cast<std::uint16_t>(track.channels) << 3U));
    std::vector<std::uint8_t> bytes{
        static_cast<std::uint8_t>((config >> 8) & 0xFF),
        static_cast<std::uint8_t>(config & 0xFF),
    };
    return HexLower(bytes);
}

std::size_t AdtsHeaderSize(const std::uint8_t* data, std::size_t size) {
    if (size < 7 || data[0] != 0xFF || (data[1] & 0xF0) != 0xF0) {
        return 0;
    }

    const bool protection_absent = (data[1] & 0x01) != 0;
    const auto header_size = protection_absent ? std::size_t{7} : std::size_t{9};
    return size >= header_size ? header_size : 0;
}

std::vector<RtpPayload> PacketizeAacAccessUnit(
    const EncodedAccessUnit& access_unit) {
    if (!access_unit.encoded_data || !access_unit.encoded_data->Data() ||
        access_unit.encoded_data->Size() == 0) {
        return {};
    }

    const auto* data = access_unit.encoded_data->Data();
    auto size = access_unit.encoded_data->Size();
    // RTSP/RTP 发送的是 AAC access unit；如果上游给的是 ADTS 帧，需要先剥掉 ADTS 头。
    const auto adts_header_size = AdtsHeaderSize(data, size);
    if (adts_header_size > 0) {
        data += adts_header_size;
        size -= adts_header_size;
    }
    if (size == 0) {
        return {};
    }

    RtpPayload payload;
    payload.timestamp = static_cast<std::uint32_t>(access_unit.pts);
    payload.marker = true;
    payload.payload.resize(kAacAuHeaderSize + size);

    // RFC 3640 AAC-hbr：AU-headers-length=16bit，单个 AU-header 使用 13bit size + 3bit index。
    WriteU16(payload.payload.data(), 16);
    const auto au_header = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(size) & 0x1FFFU) << 3U);
    WriteU16(payload.payload.data() + 2, au_header);
    std::copy(data, data + size, payload.payload.begin() + kAacAuHeaderSize);
    return {std::move(payload)};
}

std::vector<RtpPayload> PacketizeRawAudioAccessUnit(
    const EncodedAccessUnit& access_unit) {
    if (!access_unit.encoded_data || !access_unit.encoded_data->Data() ||
        access_unit.encoded_data->Size() == 0) {
        return {};
    }

    RtpPayload payload;
    payload.timestamp = static_cast<std::uint32_t>(access_unit.pts);
    payload.marker = true;
    const auto* data = access_unit.encoded_data->Data();
    const auto size = access_unit.encoded_data->Size();
    payload.payload.assign(data, data + size);
    return {std::move(payload)};
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
          session_id_(MakeSessionId(id)) {
    }

    void Start() {
        DoRead();
    }

    void Stop() {
        playing_ = false;
        ready_ = false;

        boost::system::error_code ignored;
        CloseAllUdpSockets();
        socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
    }

    bool IsPlaying() const {
        return playing_.load();
    }

    bool IsMulticastTransport() const {
        return std::any_of(track_states_.begin(),
                           track_states_.end(),
                           [](const auto& item) {
                               return item.second.ready &&
                                      item.second.transport_spec.mode ==
                                          RtspTransportMode::UdpMulticast;
                           });
    }

    bool IsPlayingMulticast() const {
        return playing_.load() && IsMulticastTransport();
    }

    bool IsPlayingMulticastTrack(int track_id) const {
        const auto* state = FindTrackState(track_id);
        return playing_.load() &&
               state &&
               state->ready &&
               state->transport_spec.mode == RtspTransportMode::UdpMulticast;
    }

    std::uint64_t Id() const {
        return id_;
    }

    void SendRtpPayload(int track_id,
                        const RtpPayload& payload,
                        std::uint8_t payload_type) {
        if (!playing_.load() || payload.payload.empty()) {
            return;
        }

        auto* state = FindTrackState(track_id);
        if (!state || !state->ready) {
            return;
        }

        if (state->transport_spec.IsTcpInterleaved()) {
            SendTcpInterleavedRtpPayload(*state, payload, payload_type);
            return;
        }

        if (state->transport_spec.mode == RtspTransportMode::UdpUnicast) {
            SendUdpRtpPayload(*state, payload, payload_type);
        }
    }

private:
    // 每个 SDP track 都有独立的 RTP/RTCP 状态。音频和视频不能共用 SSRC、
    // sequence 或 timestamp，否则播放器会把不同媒体流混到同一条 RTP 时间线里。
    struct TrackSessionState {
        int track_id{0};
        const MediaTrackConfig* track{nullptr};
        RtspTransportSpec transport_spec;
        std::unique_ptr<boost::asio::ip::udp::socket> udp_rtp_socket;
        std::unique_ptr<boost::asio::ip::udp::socket> udp_rtcp_socket;
        boost::asio::ip::udp::endpoint udp_rtp_endpoint;
        boost::asio::ip::udp::endpoint udp_rtcp_endpoint;
        boost::asio::ip::udp::endpoint udp_rtcp_sender_endpoint;
        std::array<std::uint8_t, 2048> udp_rtcp_read_buffer{};
        std::uint32_t ssrc{0};
        std::uint16_t sequence{0};
        std::uint32_t last_rtp_timestamp{0};
        std::uint32_t rtcp_packet_count{0};
        std::uint32_t rtcp_octet_count{0};
        std::chrono::steady_clock::time_point next_rtcp_sr_time{};
        RtcpReportBlock last_receiver_report{};
        std::uint32_t last_receiver_reporter_ssrc{0};
        std::uint64_t receiver_reports_received{0};
        int rtp_channel{0};
        int rtcp_channel{1};
        bool has_receiver_report{false};
        bool ready{false};
    };

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
                if (!self->closed_ && !self->close_after_write_) {
                    self->DoRead();
                }
            });
    }

    void ProcessReadBuffer() {
        while (!read_buffer_.empty()) {
            if (read_buffer_[0] == '$') {
                // RTSP over TCP 的 interleaved RTP/RTCP frame 与文本 request
                // 共用一条连接。只有首字节是 '$' 才进入二进制 framing；否则
                // 把 buffer 交给 request parser，避免把 '$' 误当作 request-line。
                if (read_buffer_.size() < 4) {
                    return;
                }
                const auto channel = read_buffer_[1];
                const auto length = ReadU16(read_buffer_.data() + 2);
                const auto frame_size = kTcpInterleavedHeaderSize + length;
                if (read_buffer_.size() < frame_size) {
                    return;
                }

                auto* state = FindTrackStateByRtcpChannel(channel);
                if (state && length > 0) {
                    HandleRtcpPacket(*state,
                                     read_buffer_.data() + kTcpInterleavedHeaderSize,
                                     length,
                                     "tcp-interleaved");
                }
                read_buffer_.erase(read_buffer_.begin(),
                                   read_buffer_.begin() +
                                       static_cast<std::ptrdiff_t>(frame_size));
                continue;
            }

            RtspRequest request;
            // parser 只消费一条完整 request，剩余字节继续留在 read_buffer_，
            // 从而同时支持 TCP 分片、pipelining 和 request 后紧跟 interleaved
            // frame 的情况。
            const auto parse_result = request_parser_.Parse(
                std::span<const std::uint8_t>{read_buffer_}, request);
            if (parse_result.status == RtspRequestParseStatus::NeedMoreData) {
                return;
            }
            if (parse_result.status == RtspRequestParseStatus::Error) {
                LOG_WARN("RTSP request parse failed at byte {}: {}",
                         parse_result.error_offset,
                         parse_result.error);
                read_buffer_.clear();
                // framing 错误后无法可靠定位下一条 request。停止 RTP 发送，
                // 先把 400 写完再关闭 TCP，避免错误数据继续堆入发送队列。
                playing_ = false;
                close_after_write_ = true;
                SendRtsp(BuildResponse(400, "Bad Request", {}));
                return;
            }

            read_buffer_.erase(read_buffer_.begin(),
                               read_buffer_.begin() +
                                   static_cast<std::ptrdiff_t>(parse_result.consumed));
            HandleRequest(request);
        }
    }

    void HandleRequest(const RtspRequest& request) {
        // 解析器负责语法，ClientSession 负责 RTSP 语义状态机。这里先处理
        // 所有请求都需要的 CSeq/version，再进入具体方法的状态校验。
        std::string cseq;
        if (!ExtractCSeq(request, cseq)) {
            SendRtsp(BuildResponse(400, "Bad Request", {}));
            return;
        }

        if (request.version_major != 1 || request.version_minor != 0) {
            SendRtsp(BuildResponse(505, "RTSP Version Not Supported", cseq));
            return;
        }

        if (request.uri == "*" && request.method != "OPTIONS") {
            SendRtsp(BuildResponse(400, "Bad Request", cseq));
            return;
        }

        if (request.method == "OPTIONS") {
            SendRtsp(BuildResponse(
                200,
                "OK",
                cseq,
                {{"Public", "OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN, GET_PARAMETER"}}));
            return;
        }

        if (request.method == "DESCRIBE") {
            requested_url_ = request.uri;
            const auto sdp = owner_.BuildSdp(LocalAddress());
            SendRtsp(BuildResponse(
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
                SendRtsp(BuildResponse(404, "Not Found", cseq));
                return;
            }

            const auto transport = request.HeaderValue("Transport");
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
                auto& state = PrepareTrackState(*track);
                if (!ConfigureUdpTransport(state, transport_spec)) {
                    SendRtsp(BuildResponse(461, "Unsupported Transport", cseq));
                    return;
                }
                FinishTrackSetup(state, transport_spec);
            } else if (transport_spec.mode == RtspTransportMode::UdpMulticast) {
                if (!owner_.config_.rtsp.enable_multicast) {
                    LOG_WARN("RTSP SETUP multicast rejected because multicast is disabled");
                    SendRtsp(BuildResponse(461, "Unsupported Transport", cseq));
                    return;
                }
                if (track->media_type != MediaType::VIDEO ||
                    track->codec_type != CodecType::H264) {
                    // 当前共享 multicast sender 仍按单视频流建模，音频 multicast 后续再扩展独立端口组。
                    LOG_WARN("RTSP SETUP multicast rejected for non-H264 video track {}",
                             track->track_id);
                    SendRtsp(BuildResponse(461, "Unsupported Transport", cseq));
                    return;
                }
                auto& state = PrepareTrackState(*track);
                if (!owner_.ConfigureSharedMulticastTransport(transport_spec,
                                                              LocalAddress())) {
                    SendRtsp(BuildResponse(461, "Unsupported Transport", cseq));
                    return;
                }
                FinishTrackSetup(state, transport_spec);
            } else {
                auto& state = PrepareTrackState(*track);
                FinishTrackSetup(state, transport_spec);
            }

            playing_ = false;

            SendRtsp(BuildResponse(
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
                SendRtsp(BuildResponse(455, "Method Not Valid In This State", cseq));
                return;
            }

            playing_ = true;
            const auto play_url = request.uri.empty() ? requested_url_ : request.uri;
            SendRtsp(BuildResponse(
                200,
                "OK",
                cseq,
                {{"Session", session_id_},
                 {"RTP-Info", BuildRtpInfo(play_url)}}));
            return;
        }

        if (request.method == "PAUSE") {
            playing_ = false;
            SendRtsp(BuildResponse(200, "OK", cseq, {{"Session", session_id_}}));
            return;
        }

        if (request.method == "GET_PARAMETER") {
            SendRtsp(BuildResponse(200, "OK", cseq, {{"Session", session_id_}}));
            return;
        }

        if (request.method == "TEARDOWN") {
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

    TrackSessionState* FindTrackState(int track_id) {
        auto it = track_states_.find(track_id);
        return it == track_states_.end() ? nullptr : &it->second;
    }

    const TrackSessionState* FindTrackState(int track_id) const {
        auto it = track_states_.find(track_id);
        return it == track_states_.end() ? nullptr : &it->second;
    }

    TrackSessionState* FindTrackStateByRtcpChannel(std::uint8_t channel) {
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

    const MediaTrackConfig* FindTrackForSetupUrl(const std::string& url) const {
        int track_id = 0;
        if (ParseTrackIdFromUrl(url, track_id)) {
            return FindTrackById(owner_.tracks_, track_id);
        }
        return owner_.tracks_.size() == 1 ? &owner_.tracks_.front() : nullptr;
    }

    TrackSessionState& PrepareTrackState(const MediaTrackConfig& track) {
        auto& state = track_states_[track.track_id];
        CloseUdpSockets(state);
        state = {};
        state.track_id = track.track_id;
        state.track = &track;
        state.ssrc = RandomU32();
        state.sequence = RandomU16();
        return state;
    }

    void FinishTrackSetup(TrackSessionState& state,
                          const RtspTransportSpec& transport_spec) {
        state.transport_spec = transport_spec;
        state.rtp_channel = static_cast<int>(transport_spec.rtp_channel);
        state.rtcp_channel = static_cast<int>(transport_spec.rtcp_channel);
        state.ready = true;
        ready_ = true;
    }

    bool HasReadyTrack() const {
        return std::any_of(track_states_.begin(),
                           track_states_.end(),
                           [](const auto& item) {
                               return item.second.ready;
                           });
    }

    std::string BuildTrackUrl(const std::string& play_url, int track_id) const {
        if (play_url.find("track" + std::to_string(track_id)) != std::string::npos) {
            return play_url;
        }
        return play_url + (play_url.empty() || play_url.back() == '/' ? "" : "/") +
               "track" + std::to_string(track_id);
    }

    std::string BuildRtpInfo(const std::string& play_url) const {
        std::ostringstream oss;
        bool first = true;
        for (const auto& [track_id, state] : track_states_) {
            if (!state.ready) {
                continue;
            }

            const auto rtp_sequence =
                state.transport_spec.mode == RtspTransportMode::UdpMulticast
                    ? owner_.GetMulticastSequence()
                    : state.sequence;
            const auto rtp_timestamp =
                state.transport_spec.mode == RtspTransportMode::UdpMulticast
                    ? owner_.GetMulticastLastRtpTimestamp()
                    : state.last_rtp_timestamp;
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

    std::vector<std::uint8_t> BuildInterleavedRtpFrame(
        TrackSessionState& state,
        const RtpPayload& payload,
        std::uint8_t payload_type) {
        return BuildInterleavedFrame(BuildRtpPacket(state, payload, payload_type),
                                     state.rtp_channel);
    }

    std::vector<std::uint8_t> BuildRtpPacket(
        TrackSessionState& state,
        const RtpPayload& payload,
        std::uint8_t payload_type) {
        auto packet = MakeRtpPacket(payload,
                                    payload_type,
                                    state.ssrc,
                                    state.sequence,
                                    state.last_rtp_timestamp);
        RecordRtcpSenderStats(state, payload);
        return packet;
    }

    void RecordRtcpSenderStats(TrackSessionState& state,
                               const RtpPayload& payload) {
        // RTCP SR 的 sender octet count 只统计 RTP payload 字节数，不包含 RTP header。
        ++state.rtcp_packet_count;
        state.rtcp_octet_count += static_cast<std::uint32_t>(payload.payload.size());
    }

    void SendTcpInterleavedRtpPayload(TrackSessionState& state,
                                      const RtpPayload& payload,
                                      std::uint8_t payload_type) {
        auto self = shared_from_this();
        auto frame = BuildInterleavedRtpFrame(state, payload, payload_type);
        boost::asio::post(socket_.get_executor(),
                          [self, frame = std::move(frame)]() mutable {
                              self->EnqueueWrite(std::move(frame));
                          });
        MaybeSendRtcpSenderReport(state);
    }

    void SendUdpRtpPayload(TrackSessionState& state,
                           const RtpPayload& payload,
                           std::uint8_t payload_type) {
        auto self = shared_from_this();
        const auto track_id = state.track_id;
        // UDP unicast 发送裸 RTP 包，不能带 RTSP over TCP 的 '$' 头。
        auto packet = std::make_shared<std::vector<std::uint8_t>>(
            BuildRtpPacket(state, payload, payload_type));

        boost::asio::post(
            socket_.get_executor(),
            [self, track_id, packet]() {
                auto* state = self->FindTrackState(track_id);
                if (!state || !state->udp_rtp_socket ||
                    !state->udp_rtp_socket->is_open()) {
                    return;
                }

                state->udp_rtp_socket->async_send_to(
                    boost::asio::buffer(*packet),
                    state->udp_rtp_endpoint,
                    [self, packet](boost::system::error_code ec, std::size_t) {
                        if (ec && !self->closed_) {
                            LOG_WARN("RTSP UDP RTP send failed: {}", ec.message());
                        }
                    });
            });
        MaybeSendRtcpSenderReport(state);
    }

    void MaybeSendRtcpSenderReport(TrackSessionState& state) {
        if (state.rtcp_packet_count == 0 ||
            !ShouldSendRtcpSenderReport(state.next_rtcp_sr_time)) {
            return;
        }

        auto report = BuildRtcpSenderReport(state.ssrc,
                                            state.last_rtp_timestamp,
                                            state.rtcp_packet_count,
                                            state.rtcp_octet_count);
        if (state.transport_spec.IsTcpInterleaved()) {
            auto self = shared_from_this();
            auto frame = BuildInterleavedFrame(report, state.rtcp_channel);
            boost::asio::post(socket_.get_executor(),
                              [self, frame = std::move(frame)]() mutable {
                                  self->EnqueueWrite(std::move(frame));
                              });
            return;
        }

        if (state.transport_spec.mode == RtspTransportMode::UdpUnicast) {
            SendUdpRtcpSenderReport(state.track_id, std::move(report));
        }
    }

    void SendUdpRtcpSenderReport(int track_id,
                                 std::vector<std::uint8_t> report) {
        auto self = shared_from_this();
        auto packet =
            std::make_shared<std::vector<std::uint8_t>>(std::move(report));

        boost::asio::post(
            socket_.get_executor(),
            [self, track_id, packet]() {
                auto* state = self->FindTrackState(track_id);
                if (!state || !state->udp_rtcp_socket ||
                    !state->udp_rtcp_socket->is_open()) {
                    return;
                }

                state->udp_rtcp_socket->async_send_to(
                    boost::asio::buffer(*packet),
                    state->udp_rtcp_endpoint,
                    [self, packet](boost::system::error_code ec, std::size_t) {
                        if (ec && !self->closed_) {
                            LOG_WARN("RTSP UDP RTCP sender report send failed: {}",
                                     ec.message());
                        }
                    });
            });
    }

    void HandleRtcpPacket(TrackSessionState& state,
                          const std::uint8_t* data,
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
                ParseRtcpReceiverReport(state,
                                        packet,
                                        packet_size,
                                        report_count,
                                        transport_name);
            } else if (packet_type == kRtcpSenderReportPacketType) {
                ParseRtcpSenderReport(state,
                                      packet,
                                      packet_size,
                                      report_count,
                                      transport_name);
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

    void ParseRtcpReceiverReport(TrackSessionState& state,
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

        const auto reporter_ssrc = ReadU32(packet + 4);
        ParseRtcpReportBlocks(state,
                              packet + 8,
                              report_count,
                              reporter_ssrc,
                              transport_name,
                              "RR");
    }

    void ParseRtcpSenderReport(TrackSessionState& state,
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

        const auto reporter_ssrc = ReadU32(packet + 4);
        ParseRtcpReportBlocks(state,
                              packet + 28,
                              report_count,
                              reporter_ssrc,
                              transport_name,
                              "SR");
    }

    void ParseRtcpReportBlocks(TrackSessionState& state,
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
            if (!ReadRtcpReportBlock(blocks + static_cast<std::size_t>(index) * 24U,
                                     24,
                                     block)) {
                return;
            }

            // Receiver Report 里的 source_ssrc 应该指向本 server 发送 RTP 使用的 SSRC。
            const bool matches_sender = block.source_ssrc == state.ssrc;
            if (matches_sender || !state.has_receiver_report) {
                state.last_receiver_report = block;
                state.last_receiver_reporter_ssrc = reporter_ssrc;
                state.has_receiver_report = true;
                ++state.receiver_reports_received;
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

    void StartUdpRtcpReceive(TrackSessionState& state) {
        if (!state.udp_rtcp_socket || !state.udp_rtcp_socket->is_open()) {
            return;
        }

        auto self = shared_from_this();
        const auto track_id = state.track_id;
        state.udp_rtcp_socket->async_receive_from(
            boost::asio::buffer(state.udp_rtcp_read_buffer),
            state.udp_rtcp_sender_endpoint,
            [self, track_id](boost::system::error_code ec, std::size_t bytes) {
                auto* state = self->FindTrackState(track_id);
                if (!state) {
                    return;
                }
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
                    state->udp_rtcp_sender_endpoint.address() ==
                        state->udp_rtcp_endpoint.address() &&
                    state->udp_rtcp_sender_endpoint.port() ==
                        state->udp_rtcp_endpoint.port();
                if (expected_sender) {
                    self->HandleRtcpPacket(*state,
                                           state->udp_rtcp_read_buffer.data(),
                                           bytes,
                                           "udp-unicast");
                } else {
                    LOG_DEBUG("RTSP UDP RTCP ignored from unexpected endpoint: "
                              "session={}, endpoint={}:{}",
                              self->id_,
                              state->udp_rtcp_sender_endpoint.address().to_string(),
                              state->udp_rtcp_sender_endpoint.port());
                }

                if (!self->closed_) {
                    self->StartUdpRtcpReceive(*state);
                }
            });
    }

    bool ConfigureUdpTransport(TrackSessionState& state,
                               RtspTransportSpec& transport_spec) {
        CloseUdpSockets(state);

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

        state.udp_rtp_socket =
            std::make_unique<boost::asio::ip::udp::socket>(socket_.get_executor());
        state.udp_rtcp_socket =
            std::make_unique<boost::asio::ip::udp::socket>(socket_.get_executor());

        state.udp_rtp_socket->open(protocol, ec);
        if (ec) {
            LOG_WARN("RTSP SETUP UDP RTP socket open failed: {}", ec.message());
            CloseUdpSockets(state);
            return false;
        }

        state.udp_rtcp_socket->open(protocol, ec);
        if (ec) {
            LOG_WARN("RTSP SETUP UDP RTCP socket open failed: {}", ec.message());
            CloseUdpSockets(state);
            return false;
        }

        state.udp_rtp_socket->bind(boost::asio::ip::udp::endpoint(bind_address, 0), ec);
        if (ec) {
            LOG_WARN("RTSP SETUP UDP RTP socket bind failed: {}", ec.message());
            CloseUdpSockets(state);
            return false;
        }

        state.udp_rtcp_socket->bind(boost::asio::ip::udp::endpoint(bind_address, 0), ec);
        if (ec) {
            LOG_WARN("RTSP SETUP UDP RTCP socket bind failed: {}", ec.message());
            CloseUdpSockets(state);
            return false;
        }

        const auto rtp_local_endpoint = state.udp_rtp_socket->local_endpoint(ec);
        if (ec) {
            LOG_WARN("RTSP SETUP UDP RTP local endpoint failed: {}", ec.message());
            CloseUdpSockets(state);
            return false;
        }
        const auto rtcp_local_endpoint = state.udp_rtcp_socket->local_endpoint(ec);
        if (ec) {
            LOG_WARN("RTSP SETUP UDP RTCP local endpoint failed: {}", ec.message());
            CloseUdpSockets(state);
            return false;
        }

        transport_spec.server_rtp_port = rtp_local_endpoint.port();
        transport_spec.server_rtcp_port = rtcp_local_endpoint.port();
        if (!rtp_local_endpoint.address().is_unspecified()) {
            transport_spec.source = rtp_local_endpoint.address().to_string();
        }
        state.udp_rtp_endpoint =
            boost::asio::ip::udp::endpoint(destination, transport_spec.client_rtp_port);
        state.udp_rtcp_endpoint =
            boost::asio::ip::udp::endpoint(destination, transport_spec.client_rtcp_port);

        LOG_INFO("RTSP SETUP UDP transport: client_rtp={}:{} server_rtp={}:{}",
                 state.udp_rtp_endpoint.address().to_string(),
                 state.udp_rtp_endpoint.port(),
                 rtp_local_endpoint.address().to_string(),
                 rtp_local_endpoint.port());
        StartUdpRtcpReceive(state);
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
                if (self->write_queue_.empty() && self->close_after_write_) {
                    self->Close();
                    return;
                }
                self->DoWrite();
            });
    }

    std::string LocalAddress() const {
        boost::system::error_code ec;
        const auto endpoint = socket_.local_endpoint(ec);
        return ec ? "127.0.0.1" : endpoint.address().to_string();
    }

    void CloseUdpSockets(TrackSessionState& state) {
        boost::system::error_code ignored;
        if (state.udp_rtp_socket) {
            state.udp_rtp_socket->cancel(ignored);
            state.udp_rtp_socket->close(ignored);
            state.udp_rtp_socket.reset();
        }
        if (state.udp_rtcp_socket) {
            state.udp_rtcp_socket->cancel(ignored);
            state.udp_rtcp_socket->close(ignored);
            state.udp_rtcp_socket.reset();
        }
    }

    void CloseAllUdpSockets() {
        for (auto& [_, state] : track_states_) {
            CloseUdpSockets(state);
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
        CloseAllUdpSockets();
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
    RtspRequestParser request_parser_;
    std::deque<std::vector<std::uint8_t>> write_queue_;
    // key 对应 MediaTrackConfig::track_id，也就是 SDP/SETUP 中的 trackN。
    std::map<int, TrackSessionState> track_states_;
    bool ready_{false};
    std::atomic_bool playing_{false};
    bool closed_{false};
    bool close_after_write_{false};
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

PublisherResult RtspServerProtocol::Start(
    const PublisherConfig& config,
    const std::vector<MediaTrackConfig>& tracks) {
    Stop();

    if (tracks.empty() ||
        !std::all_of(tracks.begin(), tracks.end(), IsRtspServerSupportedTrack) ||
        (!config.rtsp.enable_tcp_interleaved && !config.rtsp.enable_udp)) {
        LOG_ERROR("RtspServerProtocol: supports H264 video and AAC/G711 audio with RTSP TCP interleaved or UDP");
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidConfiguration,
            "RTSP server requires supported tracks and at least one transport");
    }

    config_ = config;
    tracks_ = tracks;
    NormalizeRtspTrackDefaults(tracks_);
    config_.tracks = tracks_;
    stats_ = {};
    h264_packetizer_ = H264RtpPacketizer(
        config_.rtsp.max_payload_size > 0
            ? static_cast<std::size_t>(config_.rtsp.max_payload_size)
            : 1420);
    auto h264_track = std::find_if(tracks_.begin(), tracks_.end(), [](const auto& track) {
        return track.media_type == MediaType::VIDEO &&
               track.codec_type == CodecType::H264;
    });
    h264_parameter_sets_ = h264_track == tracks_.end()
        ? H264ParameterSets{}
        : H264Bitstream::ExtractParameterSets(h264_track->extra_data);

    boost::system::error_code ec;
    auto address = boost::asio::ip::make_address(config_.listen_host, ec);
    if (ec) {
        LOG_ERROR("RtspServerProtocol: invalid listen host {}: {}",
                  config_.listen_host,
                  ec.message());
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidConfiguration,
            "invalid RTSP listen host: " + ec.message(),
            ec.value());
    }

    boost::asio::ip::tcp::endpoint endpoint(address, config_.listen_port);
    acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(io_);
    acceptor_->open(endpoint.protocol(), ec);
    if (ec) {
        LOG_ERROR("RtspServerProtocol: acceptor open failed: {}", ec.message());
        Stop();
        return PublisherResult::Failure(
            PublisherErrorCode::ResourceOpenFailed,
            "failed to open RTSP acceptor: " + ec.message(),
            ec.value());
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
        return PublisherResult::Failure(
            PublisherErrorCode::BindFailed,
            "failed to bind RTSP listen endpoint: " + ec.message(),
            ec.value());
    }

    acceptor_->listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        LOG_ERROR("RtspServerProtocol: listen failed: {}", ec.message());
        Stop();
        return PublisherResult::Failure(
            PublisherErrorCode::ResourceOpenFailed,
            "failed to listen on RTSP endpoint: " + ec.message(),
            ec.value());
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
    return PublisherResult::Success();
}

PublisherResult RtspServerProtocol::Write(
    const EncodedAccessUnit& access_unit) {
    const auto* track = FindTrackById(tracks_, access_unit.track_id);
    if (!track || access_unit.codec_type != track->codec_type) {
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidMediaPacket,
            "access unit does not match a configured RTSP track");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return PublisherResult::Failure(
                PublisherErrorCode::InvalidState,
                "RTSP server protocol must be started before writing");
        }
    }

    std::vector<RtpPayload> packets;
    if (track->codec_type == CodecType::H264) {
        if (access_unit.nals.empty()) {
            return PublisherResult::Failure(
                PublisherErrorCode::InvalidMediaPacket,
                "H264 access unit does not contain NAL units");
        }
        UpdateH264ParameterSets(access_unit);
        packets = h264_packetizer_.Packetize(access_unit);
    } else if (track->codec_type == CodecType::AAC) {
        packets = PacketizeAacAccessUnit(access_unit);
    } else if (track->codec_type == CodecType::G711A ||
               track->codec_type == CodecType::G711U) {
        packets = PacketizeRawAudioAccessUnit(access_unit);
    }
    if (packets.empty()) {
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidMediaPacket,
            "access unit could not be packetized for RTP");
    }

    std::vector<std::shared_ptr<ClientSession>> sessions;
    SnapshotSessions(sessions);
    const auto payload_type = track->rtp_payload_type;
    const auto has_multicast_receiver =
        std::any_of(sessions.begin(), sessions.end(), [track](const auto& session) {
            return session && session->IsPlayingMulticastTrack(track->track_id);
        });

    for (const auto& packet : packets) {
        for (const auto& session : sessions) {
            if (session && session->IsPlaying() &&
                !session->IsPlayingMulticastTrack(track->track_id)) {
                session->SendRtpPayload(track->track_id, packet, payload_type);
            }
        }
        if (has_multicast_receiver && track->codec_type == CodecType::H264) {
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
    return PublisherResult::Success();
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

    boost::system::error_code address_ec;
    const auto multicast_address =
        boost::asio::ip::make_address(config_.rtsp.multicast_address, address_ec);
    const bool use_multicast_sdp =
        config_.rtsp.enable_udp &&
        config_.rtsp.enable_multicast &&
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

    for (const auto& track : tracks_) {
        const auto media_name =
            track.media_type == MediaType::AUDIO ? "audio" : "video";
        const auto media_port =
            use_multicast_sdp && track.media_type == MediaType::VIDEO
                ? config_.rtsp.multicast_rtp_port
                : 0;

        oss << "m=" << media_name << " " << media_port
            << " RTP/AVP " << static_cast<int>(track.rtp_payload_type) << "\r\n";

        if (track.codec_type == CodecType::H264) {
            oss << "a=rtpmap:" << static_cast<int>(track.rtp_payload_type)
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
