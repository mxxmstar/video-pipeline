#include "media/protocol/rtsp_server_protocol.h"

#include "common/log/logger.h"
#include "media/protocol/audio_rtp_packetizer.h"
#include "media/protocol/rtsp/rtsp_multicast_publisher.h"
#include "media/protocol/rtsp/rtsp_builders.h"
#include "media/protocol/rtsp/rtsp_session.h"

#include <algorithm>
#include <array>
#include <future>
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

    auto normalized_tracks = tracks;
    NormalizeRtspTrackDefaults(normalized_tracks);
    auto normalized_config = config;
    normalized_config.tracks = normalized_tracks;

    const auto max_payload_size = normalized_config.rtsp.max_payload_size > 0
        ? static_cast<std::size_t>(normalized_config.rtsp.max_payload_size)
        : std::size_t{1420};
    H264RtpPacketizer h264_packetizer(max_payload_size);
    auto h264_track = std::find_if(
        normalized_tracks.begin(),
        normalized_tracks.end(),
        [](const auto& track) {
            return track.media_type == MediaType::VIDEO &&
                   track.codec_type == CodecType::H264;
        });
    const auto h264_parameter_sets = h264_track == normalized_tracks.end()
        ? H264ParameterSets{}
        : H264Bitstream::ExtractParameterSets(h264_track->extra_data);

    boost::system::error_code ec;
    const auto address = boost::asio::ip::make_address(
        normalized_config.listen_host,
        ec);
    if (ec) {
        LOG_ERROR("RtspServerProtocol: invalid listen host {}: {}",
                  normalized_config.listen_host,
                  ec.message());
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidConfiguration,
            "invalid RTSP listen host: " + ec.message(),
            ec.value());
    }

    const auto multicast_payload_type = h264_track == normalized_tracks.end()
        ? std::uint8_t{96}
        : h264_track->rtp_payload_type;
    const auto multicast_clock_rate = h264_track == normalized_tracks.end()
        ? std::uint32_t{90000}
        : h264_track->rtp_clock_rate;
    // publisher 和 context 先在局部变量中建立。回调捕获这个 shared_ptr 快照，
    // 不再在 session 的异步路径中裸读 façade 的 multicast_publisher_ 成员。
    auto multicast_publisher = std::make_shared<RtspMulticastPublisher>(
        io_.get_executor(),
        normalized_config.rtsp,
        multicast_payload_type,
        multicast_clock_rate,
        RtspMulticastPublisher::ReceiverReportHandler{});

    auto session_context = std::make_shared<RtspSessionContext>();
    session_context->config = normalized_config;
    session_context->tracks = normalized_tracks;
    session_context->io_executor = io_.get_executor();
    session_context->build_sdp = [this](const std::string& host) {
        return BuildSdp(host);
    };
    session_context->configure_multicast =
        [multicast_publisher](RtspTransportSpec& transport_spec,
                              const std::string& source_address) {
            return multicast_publisher->Configure(transport_spec,
                                                   source_address);
        };
    session_context->get_multicast_sequence = [multicast_publisher]() {
        return multicast_publisher->GetSequence();
    };
    session_context->get_multicast_last_rtp_timestamp =
        [multicast_publisher]() {
            return multicast_publisher->GetLastRtpTimestamp();
        };
    session_context->add_receiver_reports = [this](std::uint64_t count) {
        AddRtcpReceiverReportsReceived(count);
    };
    auto session_manager = std::make_shared<RtspSessionManager>(
        session_context);
    const auto weak_session_manager = std::weak_ptr<RtspSessionManager>(
        session_manager);
    session_context->record_auth_failure =
        [weak_session_manager](const std::string& client_address) {
            if (const auto manager = weak_session_manager.lock()) {
                return manager->RecordAuthFailure(client_address);
            }
            return true;
        };
    session_context->record_slow_client = [weak_session_manager]() {
        if (const auto manager = weak_session_manager.lock()) {
            manager->RecordSlowClientClosed();
        }
    };

    // acceptor 也先在局部完成 open/bind/listen。只有监听资源完整可用时，
    // 才把 config/context/manager/publisher 一起提交给 façade。
    auto acceptor = std::make_unique<boost::asio::ip::tcp::acceptor>(io_);
    boost::asio::ip::tcp::endpoint endpoint(address, normalized_config.listen_port);
    acceptor->open(endpoint.protocol(), ec);
    if (ec) {
        LOG_ERROR("RtspServerProtocol: acceptor open failed: {}", ec.message());
        return PublisherResult::Failure(
            PublisherErrorCode::ResourceOpenFailed,
            "failed to open RTSP acceptor: " + ec.message(),
            ec.value());
    }

    acceptor->set_option(boost::asio::socket_base::reuse_address(true), ec);
    if (ec) {
        LOG_WARN("RtspServerProtocol: set reuse_address failed: {}", ec.message());
    }

    acceptor->bind(endpoint, ec);
    if (ec) {
        LOG_ERROR("RtspServerProtocol: bind {}:{} failed: {}",
                  normalized_config.listen_host,
                  normalized_config.listen_port,
                  ec.message());
        return PublisherResult::Failure(
            PublisherErrorCode::BindFailed,
            "failed to bind RTSP listen endpoint: " + ec.message(),
            ec.value());
    }

    acceptor->listen(boost::asio::socket_base::max_listen_connections, ec);
    if (ec) {
        LOG_ERROR("RtspServerProtocol: listen failed: {}", ec.message());
        return PublisherResult::Failure(
            PublisherErrorCode::ResourceOpenFailed,
            "failed to listen on RTSP endpoint: " + ec.message(),
            ec.value());
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = std::move(normalized_config);
        tracks_ = std::move(normalized_tracks);
        h264_parameter_sets_ = h264_parameter_sets;
        h264_packetizer_ = h264_packetizer;
        session_context_ = std::move(session_context);
        session_manager_ = std::move(session_manager);
        multicast_publisher_ = std::move(multicast_publisher);
        acceptor_ = std::move(acceptor);
        stats_ = {};
        ++lifecycle_generation_;
        started_ = true;
    }

    io_.restart();
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
    MediaTrackConfig track;
    H264RtpPacketizer h264_packetizer;
    std::shared_ptr<RtspSessionManager> session_manager;
    std::shared_ptr<RtspMulticastPublisher> multicast_publisher;
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return PublisherResult::Failure(
                PublisherErrorCode::InvalidState,
                "RTSP server protocol must be started before writing");
        }

        const auto* configured_track = FindTrackById(tracks_, access_unit.track_id);
        if (!configured_track || access_unit.codec_type != configured_track->codec_type) {
            return PublisherResult::Failure(
                PublisherErrorCode::InvalidMediaPacket,
                "access unit does not match a configured RTSP track");
        }
        track = *configured_track;
        h264_packetizer = h264_packetizer_;
        session_manager = session_manager_;
        multicast_publisher = multicast_publisher_;
        generation = lifecycle_generation_;
    }

    std::vector<RtpPayload> packets;
    if (track.codec_type == CodecType::H264) {
        if (access_unit.nals.empty()) {
            return PublisherResult::Failure(
                PublisherErrorCode::InvalidMediaPacket,
                "H264 access unit does not contain NAL units");
        }
        UpdateH264ParameterSets(access_unit);
        packets = h264_packetizer.Packetize(access_unit);
    } else if (track.codec_type == CodecType::AAC ||
               track.codec_type == CodecType::G711A ||
               track.codec_type == CodecType::G711U) {
        packets = AudioRtpPacketizer::Packetize(access_unit);
    }
    if (packets.empty()) {
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidMediaPacket,
            "access unit could not be packetized for RTP");
    }

    const auto sessions = session_manager
        ? session_manager->Snapshot()
        : std::vector<std::shared_ptr<RtspClientSession>>{};
    const auto payload_type = track.rtp_payload_type;
    const auto track_id = track.track_id;
    const auto is_h264 = track.codec_type == CodecType::H264;

    // pipeline 线程只复制 session/publisher 快照和 immutable RTP payload，
    // 实际读取 session 状态、修改 per-track sender 以及调用 transport 全部
    // 在 io_context 上执行。这样 Stop() 可以通过 started_ 闸门阻止晚到媒体任务。
    boost::asio::post(
        io_,
        [this,
         sessions,
         multicast_publisher,
         packets = std::move(packets),
         track_id,
         payload_type,
         is_h264,
         generation]() mutable {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!started_ || lifecycle_generation_ != generation) {
                    return;
                }
            }

            const auto has_multicast_receiver =
                is_h264 &&
                std::any_of(
                    sessions.begin(),
                    sessions.end(),
                    [track_id](const auto& session) {
                        return session &&
                               session->IsPlayingMulticastTrack(track_id);
                    });

            for (const auto& packet : packets) {
                for (const auto& session : sessions) {
                    if (session && session->IsPlaying() &&
                        !session->IsPlayingMulticastTrack(track_id)) {
                        session->SendRtpPayload(track_id, packet, payload_type);
                    }
                }
                if (has_multicast_receiver && multicast_publisher) {
                    // 共享 publisher 只接受一次 packet；客户端数量不会改变
                    // multicast sender 的 sequence、SSRC 或 packet count。
                    multicast_publisher->Publish(packet, payload_type);
                }
            }
        });

    const auto data_size = access_unit.encoded_data
        ? access_unit.encoded_data->Size()
        : [&access_unit]() {
              std::size_t total = 0;
              for (const auto& nal : access_unit.nals) {
                  total += nal.data.size();
              }
              return total;
          }();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.packets_published;
        stats_.bytes_published += data_size;
        stats_.clients_connected = sessions.size();
    }
    return PublisherResult::Success();
}

void RtspServerProtocol::Stop() {
    std::shared_ptr<RtspSessionManager> session_manager;
    std::shared_ptr<RtspMulticastPublisher> multicast_publisher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 先关掉媒体入口，再处理 socket 和 session。已经排队的媒体任务会
        // 在 executor 上再次检查这个闸门，避免 Stop() 之后产生新的发送。
        started_ = false;
        ++lifecycle_generation_;
        session_manager = session_manager_;
        multicast_publisher = multicast_publisher_;
    }

    boost::system::error_code ignored;
    if (acceptor_) {
        // acceptor 的成员所有权暂不 reset，必须等 io thread join 后才能销毁，
        // 避免最后一个 async_accept callback 与 unique_ptr 释放并发访问。
        acceptor_->cancel(ignored);
        acceptor_->close(ignored);
    }
    if (multicast_publisher) {
        // publisher 的 Close() 自身线程安全；先关闭 socket 可以立即阻止
        // 已经从 pipeline 投递但尚未执行的 multicast send。
        multicast_publisher->Close();
    }

    const auto sessions = session_manager
        ? session_manager->Snapshot()
        : std::vector<std::shared_ptr<RtspClientSession>>{};
    const auto on_io_thread =
        io_thread_.joinable() &&
        io_thread_.get_id() == std::this_thread::get_id();

    if (on_io_thread || !io_thread_.joinable()) {
        // 没有可等待的 io thread 时只能同步执行 session close；正常 Stop
        // 路径由下面的 barrier 在 Asio executor 上执行这一段。
        for (const auto& session : sessions) {
            if (session) {
                session->Stop();
            }
        }
    } else {
        // 等待 session close handler 在停止 io_context 前执行完，确保
        // connection、transport 和 manager 的关闭顺序真正发生在 executor 上。
        auto closed = std::make_shared<std::promise<void>>();
        auto closed_future = closed->get_future();
        boost::asio::post(
            io_,
            [sessions, closed]() {
                for (const auto& session : sessions) {
                    if (session) {
                        session->Stop();
                    }
                }
                closed->set_value();
            });
        closed_future.wait();
    }

    work_guard_.reset();
    io_.stop();
    if (io_thread_.joinable() && !on_io_thread) {
        io_thread_.join();
    }

    if (!on_io_thread) {
        if (session_manager) {
            const auto manager_stats = session_manager->GetStats();
            session_manager->Clear();
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.clients_connected = manager_stats.active_connections;
            stats_.connections_accepted = manager_stats.connections_accepted;
            stats_.connections_closed = manager_stats.connections_closed;
            stats_.connections_rejected = manager_stats.connections_rejected;
            stats_.connections_rejected_by_capacity =
                manager_stats.connections_rejected_by_capacity;
            stats_.connections_rejected_by_address =
                manager_stats.connections_rejected_by_address;
            stats_.connections_rejected_by_rate_limit =
                manager_stats.connections_rejected_by_rate_limit;
            stats_.auth_failures = manager_stats.auth_failures;
            stats_.auth_failures_rejected = manager_stats.auth_failures_rejected;
            stats_.slow_clients_closed = manager_stats.slow_clients_closed;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (session_manager_ == session_manager) {
                session_manager_.reset();
            }
            if (multicast_publisher_ == multicast_publisher) {
                multicast_publisher_.reset();
            }
            session_context_.reset();
            acceptor_.reset();
        }
        io_.restart();
    }
}

std::string RtspServerProtocol::GetOutputUrl() const {
    std::lock_guard<std::mutex> lock(mutex_);
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
    std::shared_ptr<RtspSessionManager> session_manager;
    std::shared_ptr<RtspMulticastPublisher> multicast_publisher;
    PublisherStats stats;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats = stats_;
        session_manager = session_manager_;
        multicast_publisher = multicast_publisher_;
    }
    // manager 自己保护 registry；protocol mutex 不跨越 Size()，避免统计读取
    // 与 session closed callback 形成锁顺序反转。
    if (session_manager) {
        const auto manager_stats = session_manager->GetStats();
        stats.clients_connected = manager_stats.active_connections;
        stats.connections_accepted = manager_stats.connections_accepted;
        stats.connections_closed = manager_stats.connections_closed;
        stats.connections_rejected = manager_stats.connections_rejected;
        stats.connections_rejected_by_capacity =
            manager_stats.connections_rejected_by_capacity;
        stats.connections_rejected_by_address =
            manager_stats.connections_rejected_by_address;
        stats.connections_rejected_by_rate_limit =
            manager_stats.connections_rejected_by_rate_limit;
        stats.auth_failures = manager_stats.auth_failures;
        stats.auth_failures_rejected = manager_stats.auth_failures_rejected;
        stats.slow_clients_closed = manager_stats.slow_clients_closed;
    } else {
        stats.clients_connected = 0;
    }
    // multicast RR 由 publisher 自己累计并输出 snapshot；session RR 仍由
    // session context 的窄回调累计到 stats_，两条来源在这里统一合并。
    if (multicast_publisher) {
        stats.rtcp_receiver_reports_received +=
            multicast_publisher->GetStats().receiver_reports_received;
    }
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || !acceptor_) {
            return;
        }
    }
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
    std::shared_ptr<RtspSessionManager> session_manager;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return;
        }
        session_manager = session_manager_;
    }

    if (!ec) {
        if (session_manager) {
            auto session = session_manager->Create(std::move(socket));
            if (session) {
                const auto client_count = session_manager->Size();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    stats_.clients_connected = client_count;
                }
                session->Start();
            }
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
