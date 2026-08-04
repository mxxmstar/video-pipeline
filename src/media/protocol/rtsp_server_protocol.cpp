#include "media/protocol/rtsp_server_protocol.h"

#include "common/log/logger.h"
#include "media/protocol/audio_rtp_packetizer.h"
#include "media/protocol/rtcp_packet_codec.h"
#include "media/protocol/rtsp/rtsp_builders.h"
#include "media/protocol/rtsp/rtsp_session.h"
#include "media/protocol/rtsp/rtsp_transport_spec.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <random>
#include <utility>

#include <boost/asio/ip/multicast.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/post.hpp>



namespace {

constexpr std::size_t kRtpHeaderSize = 12;
constexpr std::chrono::seconds kRtcpSenderReportInterval{5};

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

} // namespace

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
            RtcpPacketCodec::BuildSenderReport(multicast_ssrc_,
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
    const auto parsed = RtcpPacketCodec::ParseCompound(data, size);
    if (!parsed.valid) {
        LOG_DEBUG("RTSP multicast RTCP packet ignored: endpoint={}:{} "
                  "version={}, size={}, remaining={}",
                  sender_endpoint.address().to_string(),
                  sender_endpoint.port(),
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
            ParseMulticastRtcpReceiverReport(packet,
                                             packet_size,
                                             report_count,
                                             sender_endpoint);
        } else if (packet_type == RtcpPacketCodec::kSenderReportPacketType) {
            ParseMulticastRtcpSenderReport(packet,
                                           packet_size,
                                           report_count,
                                           sender_endpoint);
        } else {
            // SDES/BYE/APP 暂时只识别并忽略，保留日志用于排查播放器行为。
            LOG_DEBUG("RTSP multicast RTCP {} received: endpoint={}:{} size={}",
                      RtcpPacketCodec::PacketTypeName(packet_type),
                      sender_endpoint.address().to_string(),
                      sender_endpoint.port(),
                      packet_size);
        }
    }

    if (parsed.trailing_bytes != 0) {
        LOG_DEBUG("RTSP multicast RTCP trailing bytes ignored: endpoint={}:{} "
                  "bytes={}",
                  sender_endpoint.address().to_string(),
                  sender_endpoint.port(),
                  parsed.trailing_bytes);
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
        if (!RtcpPacketCodec::ReadReportBlock(
                blocks + static_cast<std::size_t>(index) * 24U,
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

    // S3 将 session 所需依赖冻结为窄化 context。context 保存配置快照，
    // 需要读取 server 动态状态的能力通过回调提供，session 不再反向持有 façade。
    session_context_ = std::make_shared<RtspSessionContext>();
    session_context_->config = config_;
    session_context_->tracks = tracks_;
    session_context_->io_executor = io_.get_executor();
    session_context_->build_sdp = [this](const std::string& host) {
        return BuildSdp(host);
    };
    session_context_->configure_multicast =
        [this](RtspTransportSpec& transport_spec,
               const std::string& source_address) {
            return ConfigureSharedMulticastTransport(transport_spec,
                                                     source_address);
        };
    session_context_->get_multicast_sequence = [this]() {
        return GetMulticastSequence();
    };
    session_context_->get_multicast_last_rtp_timestamp = [this]() {
        return GetMulticastLastRtpTimestamp();
    };
    session_context_->add_receiver_reports = [this](std::uint64_t count) {
        AddRtcpReceiverReportsReceived(count);
    };
    session_manager_ =
        std::make_shared<RtspSessionManager>(session_context_);

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
        packets = AudioRtpPacketizer::Packetize(access_unit);
    } else if (track->codec_type == CodecType::G711A ||
               track->codec_type == CodecType::G711U) {
        packets = AudioRtpPacketizer::Packetize(access_unit);
    }
    if (packets.empty()) {
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidMediaPacket,
            "access unit could not be packetized for RTP");
    }

    const auto sessions = session_manager_
        ? session_manager_->Snapshot()
        : std::vector<std::shared_ptr<RtspClientSession>>{};
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
    const auto sessions = session_manager_
        ? session_manager_->Snapshot()
        : std::vector<std::shared_ptr<RtspClientSession>>{};
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
    }

    work_guard_.reset();
    io_.stop();
    if (io_thread_.joinable()) {
        io_thread_.join();
    }
    if (session_manager_) {
        session_manager_->Clear();
        session_manager_.reset();
    }
    session_context_.reset();
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
    const auto clients_connected = session_manager_
        ? session_manager_->Size()
        : std::size_t{0};
    std::lock_guard<std::mutex> lock(mutex_);
    auto stats = stats_;
    stats.clients_connected = clients_connected;
    return stats;
}

void RtspServerProtocol::AddRtcpReceiverReportsReceived(std::uint64_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.rtcp_receiver_reports_received += count;
}

std::string RtspServerProtocol::BuildSdp(const std::string& host_for_sdp) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return RtspSdpBuilder::Build(
        config_, tracks_, h264_parameter_sets_, host_for_sdp);
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
        if (session_manager_) {
            auto session = session_manager_->Create(std::move(socket));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.clients_connected = session_manager_->Size();
            }
            session->Start();
        }
    } else if (ec != boost::asio::error::operation_aborted) {
        LOG_WARN("RtspServerProtocol: accept failed: {}", ec.message());
    }

    AcceptNext();
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
