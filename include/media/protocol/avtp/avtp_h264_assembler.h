#pragma once

#include "media/protocol/avtp/avtp_packet_parser.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace media::avtp {

/// @brief 已经组好的一个 H.264 access unit。
///
/// data 保持 Annex-B 形式，可直接交给 FFmpeg H.264 decoder。
/// timestamp 当前使用抓包时间；AVTP timestamp 保留，后续做 gPTP
/// 时间轴映射时可以接入。
struct H264AccessUnit {
    std::vector<std::uint8_t> data;
    std::int64_t capture_timestamp_us{0};
    std::uint32_t avtp_timestamp{0};
    bool keyframe{false};
};

/// @brief 将 AVTP/CVF H.264 分片组装成完整 access unit。
///
/// 设备当前以 marker 表示一个 access unit 结束，因此 assembler 的核心策略是：
/// - 同一 stream/source 的 payload 按 sequence_num 顺序拼接；
/// - marker=false 时继续缓存；
/// - marker=true 时输出一个完整 access unit；
/// - 发现 sequence gap 时丢弃当前 access unit，并等到 marker 后恢复。
class AvtpH264Assembler {
public:
    /// @brief Push() 的结果。
    enum class Result {
        NeedMore,        ///< 当前包已接收，但 access unit 尚未结束。
        AccessUnitReady, ///< marker 到达，output 中已有完整 access unit。
        Dropped,         ///< 当前包触发丢弃或处于丢弃恢复阶段。
    };

    /// @brief 组帧统计，用于现场诊断丢包、坏包和 access unit 输出情况。
    struct Stats {
        std::uint64_t packets{0};
        std::uint64_t access_units{0};
        std::uint64_t lost_packets{0};
        std::uint64_t malformed_packets{0};
        std::uint64_t payload_bytes{0};
        std::uint64_t dropped_access_units{0};
    };

    explicit AvtpH264Assembler(std::size_t max_access_unit_size =
                                   8 * 1024 * 1024);

    /// @brief 清空统计、流状态和当前缓存。
    void Reset();

    /// @brief 推入一个 CVF/H.264 包。
    ///
    /// 成功输出时返回 AccessUnitReady，并把 output 填成完整 Annex-B AU。
    Result Push(const ParsedCvfPacket& packet,
                std::int64_t capture_timestamp_us,
                H264AccessUnit& output);
    const Stats& GetStats() const { return stats_; }

private:
    /// @brief 判断包是否仍属于当前正在组帧的 stream/source。
    bool SameStream(const ParsedCvfPacket& packet) const;

    /// @brief 切换到新的 stream/source，丢弃旧缓存并重置 sequence 状态。
    void SwitchStream(const ParsedCvfPacket& packet);

    /// @brief 只清空当前 access unit 缓存，不清统计和 stream 身份。
    void ResetAccessUnit();

    /// @brief 追加 payload，同时保护最大 access unit 大小。
    bool Append(const std::uint8_t* data, std::size_t size);

    /// @brief 丢弃当前 access unit；必要时进入 dropping_until_marker 恢复模式。
    Result DropCurrentAccessUnit(bool wait_until_marker);

    /// @brief marker 到达后生成 H264AccessUnit。
    Result FinishAccessUnit(H264AccessUnit& output);

    std::size_t max_access_unit_size_;
    Stats stats_;
    std::vector<std::uint8_t> current_;
    bool has_stream_{false};
    std::uint64_t stream_id_{0};
    MacAddress source_mac_{};
    std::uint8_t expected_sequence_{0};
    bool have_expected_sequence_{false};
    bool dropping_until_marker_{false};
    std::int64_t first_capture_timestamp_us_{0};
    std::uint32_t first_avtp_timestamp_{0};
};

/// @brief 在 Annex-B access unit 中查找指定 H.264 NAL type。
bool ContainsH264NalType(const std::vector<std::uint8_t>& data,
                         std::uint8_t nal_type);

} // namespace media::avtp
