#pragma once

#include "media/protocol/avtp/avtp_packet_parser.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace media::avtp {

/// @brief H.265/JPEG 等非 H.264 payload 的通用 access unit。
///
/// 当前设备同样依靠 AVTP marker 表示 access unit 结束，因此这里不理解
/// 具体编码语法，只负责按 sequence/marker 拼接 payload。
struct AvtpAccessUnit {
    std::vector<std::uint8_t> data;
    std::int64_t capture_timestamp_us{0};
    std::uint32_t avtp_timestamp{0};
};

/// @brief 通用 payload 组帧器。
///
/// 与 H.264 专用 assembler 的主要区别是：不做 NAL 缓存/关键帧判断，
/// 只输出完整字节序列。H.265 keyframe、JPEG keyframe 由 puller 层在
/// access unit 输出后判断。
class AvtpPayloadAssembler {
public:
    enum class Result {
        NeedMore,        ///< 当前包已接收，但 access unit 尚未结束。
        AccessUnitReady, ///< marker 到达，output 中已有完整 access unit。
        Dropped,         ///< 当前包触发丢弃或处于丢弃恢复阶段。
    };

    /// @brief 组帧统计，用于现场诊断。
    struct Stats {
        std::uint64_t packets{0};
        std::uint64_t access_units{0};
        std::uint64_t lost_packets{0};
        std::uint64_t malformed_packets{0};
        std::uint64_t payload_bytes{0};
        std::uint64_t dropped_access_units{0};
    };

    explicit AvtpPayloadAssembler(std::size_t max_access_unit_size =
                                      16 * 1024 * 1024);

    /// @brief 清空统计、流状态和当前缓存。
    void Reset();

    /// @brief 推入一个非 H.264 payload 分片。
    Result Push(const ParsedCvfPacket& packet,
                const std::uint8_t* payload,
                std::size_t payload_size,
                std::int64_t capture_timestamp_us,
                AvtpAccessUnit& output);
    const Stats& GetStats() const { return stats_; }

private:
    /// @brief 判断包是否仍属于当前正在组帧的 stream/source。
    bool SameStream(const ParsedCvfPacket& packet) const;

    /// @brief 切换到新的 stream/source。
    void SwitchStream(const ParsedCvfPacket& packet);

    /// @brief 只清空当前 access unit 缓存。
    void ResetAccessUnit();

    /// @brief 追加 payload，同时保护最大 access unit 大小。
    bool Append(const std::uint8_t* data, std::size_t size);

    /// @brief 丢弃当前 access unit；必要时等 marker 后恢复。
    Result DropCurrentAccessUnit(bool wait_until_marker);

    /// @brief marker 到达后生成 AvtpAccessUnit。
    Result FinishAccessUnit(AvtpAccessUnit& output);

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

} // namespace media::avtp
