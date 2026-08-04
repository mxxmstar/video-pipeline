#pragma once

#include <vector>

#include "media/protocol/h264_rtp_packetizer.h"

/// @brief AAC/G711 音频 access unit 的 RTP payload 生成器。
///
/// 该组件只生成 payload，不处理 RTP header、SSRC、sequence 或 socket。
/// H264 仍由 H264RtpPacketizer 处理，避免 codec-specific 规则互相耦合。
class AudioRtpPacketizer {
public:
    /// 使用 access unit 的 PTS 作为 RTP timestamp，并为一个 access unit
    /// 生成一个 marker=true 的 payload；RTP header 由上层 sender 添加。
    /// 空数据、未知 codec 或不支持的媒体类型返回空 vector。
    static std::vector<RtpPayload> Packetize(const EncodedAccessUnit& access_unit);
};
