#include "media/protocol/rtsp_server_protocol.h"

#include "common/log/logger.h"
#include "media/protocol/audio_rtp_packetizer.h"
#include "media/protocol/rtsp/rtsp_multicast_publisher.h"
#include "media/protocol/rtsp/rtsp_builders.h"
#include "media/protocol/rtsp/rtsp_session.h"

#include <algorithm>
#include <array>
#include <memory>
#include <utility>

#include <boost/asio/post.hpp>



namespace {

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
            return multicast_publisher_ &&
                   multicast_publisher_->Configure(transport_spec,
                                                    source_address);
        };
    session_context_->get_multicast_sequence = [this]() {
        return multicast_publisher_ ? multicast_publisher_->GetSequence() : 0;
    };
    session_context_->get_multicast_last_rtp_timestamp = [this]() {
        return multicast_publisher_
            ? multicast_publisher_->GetLastRtpTimestamp()
            : 0;
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

    const auto multicast_payload_type = h264_track == tracks_.end()
        ? std::uint8_t{96}
        : h264_track->rtp_payload_type;
    const auto multicast_clock_rate = h264_track == tracks_.end()
        ? std::uint32_t{90000}
        : h264_track->rtp_clock_rate;
    // S6 将组播 socket、共享 RtpSender 和 RR 聚合移入独立 publisher；
    // callback 只把匹配到的 RR 数量汇总回 façade 统计。
    multicast_publisher_ = std::make_shared<RtspMulticastPublisher>(
        io_.get_executor(),
        config_.rtsp,
        multicast_payload_type,
        multicast_clock_rate,
        [this](std::uint64_t count) {
            AddRtcpReceiverReportsReceived(count);
        });

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
        if (has_multicast_receiver && track->codec_type == CodecType::H264 &&
            multicast_publisher_) {
            // publisher 内部只有一份共享 sender/socket；多个 session 只影响
            // 是否需要发布，不会让同一 access unit 重复产生 multicast packet。
            multicast_publisher_->Publish(packet, payload_type);
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
    if (multicast_publisher_) {
        multicast_publisher_->Close();
    }

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
    multicast_publisher_.reset();
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
