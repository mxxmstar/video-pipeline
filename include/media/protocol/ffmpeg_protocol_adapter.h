#pragma once

#include <memory>

#include "media/protocol/i_protocol.h"
#include "media/protocol/i_protocol_adapter.h"

/// @brief 将 pipeline 的 MediaPacket 适配为 FFmpeg mux protocol 的输入模型。
///
/// Adapter 负责两件事：根据 packet 元数据选择唯一的 Publisher track，以及
/// 把 packet 的时间戳、编码数据和后端句柄转换为 EncodedAccessUnit。FFmpeg
/// context、stream、muxer 和网络 I/O 均由内部 IProtocol 实现负责。
class FfmpegProtocolAdapter : public IProtocolAdapter {
public:
    explicit FfmpegProtocolAdapter(std::unique_ptr<IProtocol> protocol = nullptr);
    ~FfmpegProtocolAdapter() override;

    PublisherResult Open(const PublisherConfig& config) override;
    PublisherResult Send(const MediaPacket& packet) override;
    void Close() override;

    std::string GetOutputUrl() const override;
    PublisherStats GetStats() const override;

private:
    // 优先按 packet.stream_index 查找 track；查找不到时再按媒体类型和 codec
    // 做唯一匹配。无法唯一确定目标轨道时返回 nullptr，避免静默写错轨道。
    const MediaTrackConfig* FindTrackForPacket(const MediaPacket& packet) const;

    // 复制 packet 的时序、编码数据和 backend 信息，并使用配置中的 track 元数据
    // 补齐确定的 track_id、媒体类型和 codec 类型。
    EncodedAccessUnit ToAccessUnit(const MediaPacket& packet,
                                   const MediaTrackConfig& track) const;

    // 保存 Open 时使用的轨道配置，Send 的每个 packet 都基于它进行路由。
    PublisherConfig config_;

    // 默认由工厂创建 FfmpegMuxProtocol；测试时也可以注入替身 IProtocol。
    std::unique_ptr<IProtocol> protocol_;

    // 只有 protocol_->Start 成功后才允许 Send；Close 或启动失败都会清零该状态。
    bool opened_{false};
};
