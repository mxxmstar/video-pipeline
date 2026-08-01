#include "media/protocol/ffmpeg_protocol_adapter.h"

#include "media/protocol/ffmpeg_mux_protocol.h"

#include <algorithm>
#include <iterator>
#include <utility>



FfmpegProtocolAdapter::FfmpegProtocolAdapter(std::unique_ptr<IProtocol> protocol)
    : protocol_(std::move(protocol)) {
}

FfmpegProtocolAdapter::~FfmpegProtocolAdapter() {
    Close();
}

PublisherResult FfmpegProtocolAdapter::Open(const PublisherConfig& config) {
    // Open 可以重复调用。先关闭旧会话，确保旧的 FFmpeg context、socket 和
    // 统计状态不会与新配置混在一起。
    Close();

    config_ = config;
    if (!protocol_) {
        // 生产路径不需要显式注入 protocol，由 adapter 负责创建默认 mux 实现；
        // 注入能力主要用于测试 track 路由和 adapter 行为。
        protocol_ = std::make_unique<FfmpegMuxProtocol>();
    }

    // protocol 只接收已经整理好的 track 列表，真正的 URL 打开和 header 写入
    // 在 FfmpegMuxProtocol::Start 中完成。
    auto result = protocol_->Start(config_, config_.tracks);
    opened_ = result.IsSuccess();
    return result;
}

PublisherResult FfmpegProtocolAdapter::Send(const MediaPacket& packet) {
    if (!opened_ || !protocol_) {
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidState,
            "FFmpeg protocol adapter is not open");
    }

    // MediaPacket 的 stream_index 来自上游拉流/解码链路，不应直接假定它永远
    // 等于 Publisher track_id。先完成一次有歧义检测，再交给 protocol 写出。
    const auto* track = FindTrackForPacket(packet);
    if (!track) {
        return PublisherResult::Failure(
            PublisherErrorCode::InvalidMediaPacket,
            "media packet cannot be mapped to a unique FFmpeg track");
    }

    return protocol_->Write(ToAccessUnit(packet, *track));
}

void FfmpegProtocolAdapter::Close() {
    if (protocol_) {
        // IProtocol::Stop 需要具备幂等性，因此即使 adapter 已经关闭也可以安全调用。
        protocol_->Stop();
    }
    opened_ = false;
}

std::string FfmpegProtocolAdapter::GetOutputUrl() const {
    return protocol_ ? protocol_->GetOutputUrl() : std::string{};
}

PublisherStats FfmpegProtocolAdapter::GetStats() const {
    return protocol_ ? protocol_->GetStats() : PublisherStats{};
}

const MediaTrackConfig* FfmpegProtocolAdapter::FindTrackForPacket(
    const MediaPacket& packet) const {
    if (config_.tracks.empty()) {
        return nullptr;
    }

    // UNKNOWN 表示 packet 没有提供该维度的可靠信息，因此只约束已知字段。
    // 这样可以兼容只带 stream_index、只带媒体类型/codec，或三者都带齐的上游。
    const auto matches_packet = [&packet](const MediaTrackConfig& track) {
        return (packet.type == MediaType::UNKNOWN ||
                packet.type == track.media_type) &&
               (packet.codec == CodecType::UNKNOWN ||
                packet.codec == track.codec_type);
    };

    if (packet.stream_index >= 0) {
        // stream_index 是最强的路由线索，但仍要校验媒体类型和 codec，防止上游
        // 编号与 publisher 配置发生错位时把音频包写进视频 stream。
        const auto by_id = std::find_if(
            config_.tracks.begin(),
            config_.tracks.end(),
            [&packet, &matches_packet](const MediaTrackConfig& track) {
                return track.track_id == packet.stream_index &&
                       matches_packet(track);
            });
        if (by_id != config_.tracks.end()) {
            return &*by_id;
        }
    }

    // 没有通过 stream_index 命中时，允许使用媒体类型和 codec 做回退匹配。
    // 该回退只在候选唯一时成立；多轨同 codec 时必须由上游提供 stream_index。
    const auto by_media_codec = std::find_if(
        config_.tracks.begin(),
        config_.tracks.end(),
        [&matches_packet](const MediaTrackConfig& track) {
            return matches_packet(track);
        });
    if (by_media_codec == config_.tracks.end()) {
        return nullptr;
    }

    // 查找第二个候选，用于识别“看起来匹配，但实际上无法确定”的多轨场景。
    const auto second_match = std::find_if(
        std::next(by_media_codec),
        config_.tracks.end(),
        [&matches_packet](const MediaTrackConfig& track) {
            return matches_packet(track);
        });
    return second_match == config_.tracks.end() ? &*by_media_codec : nullptr;
}

EncodedAccessUnit FfmpegProtocolAdapter::ToAccessUnit(
    const MediaPacket& packet,
    const MediaTrackConfig& track) const {
    EncodedAccessUnit access_unit;
    // track 元数据来自已完成歧义检查的配置，而不是完全信任 packet 中的字段；
    // 这样 protocol 收到的 track_id、媒体类型和 codec 始终相互一致。
    access_unit.track_id = track.track_id;
    access_unit.media_type = track.media_type;
    access_unit.codec_type = track.codec_type;
    access_unit.pts = packet.pts;
    access_unit.dts = packet.dts;
    access_unit.duration = packet.duration;
    access_unit.time_base = packet.time_base;
    access_unit.keyframe = packet.keyframe;

    // encoded_data 使用共享 buffer 传递，FFmpeg protocol 会在需要时复制到 AVPacket；
    // FFMPEG backend 则允许 protocol 直接引用上游 AVPacket，避免不必要的复制。
    access_unit.encoded_data = packet.buffer;
    access_unit.backend = packet.backend;
    return access_unit;
}
