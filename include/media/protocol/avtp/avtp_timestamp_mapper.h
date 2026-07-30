#pragma once

#include <cstdint>
#include <unordered_map>

namespace media::avtp {

/// @brief 将 IEEE 1722 的 32 位 AVTP presentation timestamp 映射到微秒时间轴。
///
/// AVTP timestamp 是 gPTP 纳秒时间的低 32 位，大约每 4.295 秒回绕一次；它不能
/// 直接塞进 MediaPacket::pts。映射器以首个有效 AVTP timestamp 和对应抓包时间为
/// 锚点，后续利用抓包时间选择距离预期值最近的 32 位展开周期。这样既能处理回绕，
/// 也能让音频和视频共享同一条媒体时间线。
class AvtpTimestampMapper {
public:
    struct Config {
        /// AVTP 时间与抓包时间的相对走势偏差超过该值时，认为时钟发生跳变并重建锚点。
        /// 该阈值不限制固定 presentation offset，只检测连接期间突然出现的时间跳变。
        std::int64_t discontinuity_threshold_us{500000};
    };

    struct Stats {
        std::uint64_t mapped_timestamps{0};
        std::uint64_t invalid_fallbacks{0};
        std::uint64_t uncertain_fallbacks{0};
        std::uint64_t media_clock_restarts{0};
        std::uint64_t discontinuity_resets{0};
        std::uint64_t forward_wraps{0};
    };

    AvtpTimestampMapper();
    explicit AvtpTimestampMapper(Config config);

    /// @brief 清除锚点和统计；AvtpPuller 每次重新 Open 时调用。
    void Reset();

    /// @brief 返回与 capture_timestamp_us 同属微秒域的媒体时间戳。
    ///
    /// timestamp 无效或被发送端标记为 uncertain 时安全回退到抓包时间；MCR
    /// (media_clock_restart) 会立即重建映射锚点，避免把重启前后的时钟强行拼接。
    std::int64_t Map(std::uint64_t stream_id,
                     std::uint32_t avtp_timestamp,
                     std::int64_t capture_timestamp_us,
                     bool timestamp_valid,
                     bool timestamp_uncertain,
                     bool media_clock_restart);

    bool IsInitialized() const { return initialized_; }
    const Stats& GetStats() const { return stats_; }

private:
    void SetAnchor(std::uint32_t avtp_timestamp,
                   std::int64_t capture_timestamp_us);
    std::int64_t ExpandNearest(std::uint32_t avtp_timestamp,
                               std::int64_t predicted_ns) const;

    Config config_;
    Stats stats_;
    bool initialized_{false};
    std::int64_t anchor_avtp_ns_{0};
    std::int64_t anchor_capture_us_{0};
    std::int64_t last_extended_avtp_ns_{0};
    // IEEE 1722 的 MR 是“发生重启时翻转”的 toggle，不是只维持一个包的脉冲。
    // 每条 stream 独立记忆上次取值，避免音视频交错时互相误触发重置。
    std::unordered_map<std::uint64_t, bool> media_clock_restart_bits_;
};

} // namespace media::avtp
