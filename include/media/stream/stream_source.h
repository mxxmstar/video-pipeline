#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "media/media_packet.h"
#include "media/stream/stream_info.h"
#include "media/stream/stream_session.h"
#include "media/stream/source_config.h"

/// @brief 媒体源
///
/// 一个 MediaStreamSource 对应一路流。
///
/// 职责：
///   - 持有 StreamSession，管理拉流生命周期
///   - 缓存 MediaStreamInfo（元数据）
///   - 向多个订阅者分发 MediaPacket
///
/// 后续扩展（设计预留）：
///   - decoder 管理
///   - pipeline 管理
///   - sink 管理
class MediaStreamSource : public std::enable_shared_from_this<MediaStreamSource> {
public:
    /// @brief 构造
    /// @param stream_id 全局唯一流标识
    explicit MediaStreamSource(const std::string& stream_id);

    ~MediaStreamSource();

    // ==================== Session ====================

    /// @brief 注入会话实例
    /// @param session 外部创建的 MediaStreamSession（应已完成 SetPuller/SetUrl 等配置）
    void SetSession(std::shared_ptr<MediaStreamSession> session);

    // ==================== 生命周期 ====================

    /// @brief 启动拉流（通过 session 打开连接、拉起读线程）
    /// @return true 启动成功
    bool Start();

    /// @brief 停止拉流
    void Stop();
    

    /// @brief 获取流信息（连接成功后有效）
    MultiStreamInfo GetStreamInfo() const;    

    /// @brief 设置全局配置（自动拆分下发至 session 与 puller）
    void SetMediaStreamSourceConfig(const MediaStreamSourceConfig& config);

    // ==================== 订阅 ====================

    /// @brief 包订阅回调
    using PacketCallback = std::function<void(std::shared_ptr<MediaPacket>)>;

    /// @brief 添加一个包订阅者
    /// @param cb 每次读取到包时被调用
    void AddPacketSubscriber(PacketCallback cb);

    /// @brief 启动定时打印流统计（秒间隔）
    void StartStatsPrint(int interval_seconds = 5);

    /// @brief 停止定时打印
    void StopStatsPrint();

private:
    // ==================== Session 回调桥接 ====================

    void OnStreamInfo(const MultiStreamInfo& info);
    void OnPacket(std::shared_ptr<MediaPacket> packet);
    void OnSessionState(MediaStreamSession::State state);

    // ==================== 内部 ====================

    /// @brief 将 config_ 中的 session/puller 配置应用到对应组件
    void ApplyConfig();

    std::string stream_id_;                         ///< 流标识
    std::shared_ptr<MediaStreamSession> session_;        ///< 拉流会话
    MultiStreamInfo stream_info_;                        ///< 缓存的流元信息
    MediaStreamSourceConfig config_;                     ///< 全局配置

    // ── 订阅者 ──
    std::mutex subscriber_mutex_;                           ///< 保护 subscribers
    std::vector<PacketCallback> packet_subscribers_;        ///< 包订阅者列表

    // ── 保护回调 setter（session_setter 互斥） ──
    std::mutex cb_mutex_;

    // ── 流统计 ──
    std::atomic<uint64_t> video_packet_count_{0};  ///< 视频包计数
    std::atomic<uint64_t> audio_packet_count_{0};  ///< 音频包计数
    std::atomic<uint64_t> video_bytes_{0};         ///< 视频字节数
    std::atomic<uint64_t> audio_bytes_{0};         ///< 音频字节数

    // ── 定时打印 ──
    std::atomic<bool> stats_print_running_{false}; ///< 打印线程运行标志
    std::thread stats_print_thread_;               ///< 打印线程
    void printStatsLoop(int interval_seconds);
};