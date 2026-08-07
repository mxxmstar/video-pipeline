#pragma once

/**
 * @file media_timing.h
 * @brief MediaFlow 的统一时钟、时间基换算和 PTS/DTS 调度基础设施。
 *
 * 该文件只定义媒体时间的通用语义，不依赖 render 模块。这样拉流、解码、
 * 编码和发布链路可以共享同一套时间规则，而不会为了使用同步逻辑反向依赖
 * OpenGL 或音频设备。
 */

#include "media/media_packet.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace mediaflow {

/// @brief 当前统一时钟实际采用的参考源。
enum class ClockSource {
    System,
    Audio,
};

/// @brief 一次线程安全的统一时钟读数。
struct ClockSnapshot {
    std::int64_t position_us{0};
    ClockSource source{ClockSource::System};
    std::uint64_t generation{0};
    std::uint64_t discontinuity{0};
    bool valid{false};
};

/**
 * @brief 以音频播放位置为主、steady_clock 为后备的统一媒体时钟。
 *
 * 时钟只保存“媒体时间位置”，不直接访问声卡、队列或渲染器。音频输出层
 * 通过 UpdateAudioPosition() 上报已经播放到的 PTS；在音频位置暂时不可用时，
 * 时钟从最近一次校准位置继续用 steady_clock 向前走。generation 变化或流发生
 * 时间戳跳变时，调用 Reset/NotifyDiscontinuity 清除旧轨道锚点，防止新旧连接
 * 被错误地比较。
 */
class UnifiedClock {
public:
    /// @brief 从指定媒体时间启动系统时钟。
    void Start(std::uint64_t generation = 0,
               std::int64_t start_pts_us = 0);

    /// @brief 暂停系统后备时钟，音频 master 位置仍保留。
    void Pause();

    /// @brief 恢复系统后备时钟。
    void Resume();

    /// @brief 重建指定代次的时间锚点，并清除旧音频 master。
    void Reset(std::uint64_t generation = 0,
               std::int64_t position_us = 0);

    /// @brief 标记一次时间轴不连续，并以新位置重建时钟。
    void NotifyDiscontinuity(std::uint64_t generation,
                             std::int64_t position_us = 0);

    /// @brief 直接校准系统后备位置，不改变代次和音频 master 状态。
    void SetSystemPositionUs(std::int64_t position_us);

    /// @brief 更新音频 master 的已播放 PTS。
    ///
    /// audio_pts_us=0 是合法起点，只有 kNoTimestamp 才表示“本次没有位置”。
    /// 当上报代次与当前代次不一致时，函数会先切换时间轴并清理旧 master。
    void UpdateAudioPosition(std::uint64_t generation,
                             std::int64_t audio_pts_us);

    /// @brief 获取当前时钟快照；prefer_audio 为真时优先使用音频 master。
    ClockSnapshot Snapshot(bool prefer_audio = true) const;

    bool HasAudioPosition() const;
    bool IsRunning() const;

private:
    using SteadyClock = std::chrono::steady_clock;

    std::int64_t SystemPositionUsLocked(SteadyClock::time_point now) const;

    mutable std::mutex mutex_;
    SteadyClock::time_point reference_time_{SteadyClock::now()};
    std::int64_t reference_position_us_{0};
    std::int64_t audio_position_us_{0};
    std::uint64_t generation_{0};
    std::uint64_t discontinuity_{0};
    bool running_{false};
    bool paused_{false};
    bool has_audio_position_{false};
};

/// @brief 一个媒体对象的时间戳统一换算结果，单位为微秒。
struct MediaTiming {
    std::int64_t pts_us{kNoTimestamp};
    std::int64_t dts_us{kNoTimestamp};
    std::int64_t duration_us{kNoTimestamp};
    bool pts_valid{false};
    bool dts_valid{false};
    bool duration_valid{false};
};

/// @brief 将单个 time_base tick 安全换算到微秒。
///
/// 不使用 0 表示缺失；非法 time_base、kNoTimestamp 或超出 int64 范围时，
/// 返回 kNoTimestamp。该函数只做数值重标定，不会把 PTS 当成 DTS 使用。
std::int64_t TimestampToMicroseconds(std::int64_t timestamp,
                                     const Rational& time_base);

/// @brief 将 MediaPacket 的 PTS、DTS 和 duration 分别换算到微秒。
MediaTiming GetMediaTiming(const MediaPacket& packet);

/// @brief 描述视频帧应该采取的播放调度动作。
enum class VideoScheduleAction {
    Render,
    Drop,
    Wait,
};

/// @brief 视频 PTS 调度参数。
struct VideoScheduleConfig {
    bool enabled{true};
    std::int64_t late_threshold_us{80'000};
    std::int64_t early_threshold_us{20'000};
    std::int64_t max_wait_us{20'000};
    bool drop_late_video_frames{true};
};

/// @brief 一次视频 PTS 调度结果。
struct VideoScheduleDecision {
    VideoScheduleAction action{VideoScheduleAction::Render};
    std::int64_t delta_us{0};
    std::int64_t wait_us{0};
    bool keyframe{false};
    bool clock_reset{false};
};

/**
 * @brief 根据视频 PTS 和统一时钟生成 Render/Drop/Wait 决策。
 *
 * 视频“应该何时显示”只能比较 PTS。DTS 是解码顺序时间戳，不能拿来决定
 * 屏幕显示时刻。关键帧默认受保护：即使它已经晚到，也返回 Render，交由上层
 * 通过丢弃后续非关键帧或重新建立解码窗口处理，避免同步策略破坏恢复锚点。
 */
class VideoPtsScheduler {
public:
    explicit VideoPtsScheduler(VideoScheduleConfig config = {});

    void SetConfig(VideoScheduleConfig config);
    const VideoScheduleConfig& Config() const;

    /// @param video_pts_us 视频显示时间戳，必须是已经换算到微秒的 PTS。
    /// @param keyframe 当前帧是否为关键帧。
    /// @param generation 当前视频帧所属连接代次。
    VideoScheduleDecision Decide(std::int64_t video_pts_us,
                                 bool keyframe,
                                 std::uint64_t generation,
                                 const ClockSnapshot& clock) const;

private:
    VideoScheduleConfig config_;
};

/// @brief 进入发布/编码交织器的压缩包及其微秒时间。
struct DtsPacket {
    std::shared_ptr<MediaPacket> packet;
    MediaTiming timing;
    std::uint64_t generation{0};

    bool Valid() const {
        return packet != nullptr && timing.dts_valid;
    }
};

/// @brief DTS 交织器的配置。
struct DtsInterleaverConfig {
    /// 缓存达到该数量后强制吐出最早 DTS，防止重排序无限增长。
    std::size_t max_pending_packets{32};
    /// 缓存中最早与最晚 DTS 的间隔达到该值后吐出最早包；0 表示关闭。
    std::int64_t max_pending_span_us{200'000};
};

/**
 * @brief 按 DTS 对多轨编码包进行有界交织。
 *
 * PTS 可以因 B 帧重排而晚于 DTS，发布链路不能按 PTS 发送，否则接收端可能
 * 先看到依赖帧再看到参考帧。该类只负责“何时可以按解码顺序吐出”，不负责
 * 修改包内原始 PTS/DTS，也不把不同 generation 的包混在同一时间轴中。
 */
class DtsInterleaver {
public:
    explicit DtsInterleaver(DtsInterleaverConfig config = {});

    void SetConfig(DtsInterleaverConfig config);
    const DtsInterleaverConfig& Config() const;

    /// @brief 加入一个包，返回因容量/时间跨度达到边界而可以发送的包。
    std::vector<DtsPacket> Push(DtsPacket item);

    /// @brief 刷出当前代次剩余包；缺失 DTS 的包按到达顺序最后输出。
    std::vector<DtsPacket> Flush();

    /// @brief 清空旧代次缓存并开始新的代次。
    void Reset(std::uint64_t generation = 0);

    std::size_t PendingSize() const;

private:
    void EmitReady(std::vector<DtsPacket>& output, bool force);

    DtsInterleaverConfig config_;
    std::vector<DtsPacket> pending_;
    std::uint64_t generation_{0};
};

} // namespace mediaflow
