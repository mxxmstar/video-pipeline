#pragma once

#include "media/puller/i_puller.h"

#include <map>
#include <memory>
#include <mutex>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
}

/// @brief 基于 FFmpeg 的拉流器实现
///
/// 实现 IPuller 纯虚接口，仅负责：
///   - Open()       — 创建 AVFormatContext、查找视频流、缓存 StreamInfo
///   - ReadPacket() — av_read_frame -> MediaPacket（零拷贝 FFmpegPacketBuffer）
///   - Close()      — avformat_close_input
///
/// 不负责重连 / watchdog / 状态机 / 统计（均由 StreamSession 管理）。
class FFmpegPuller : public IPuller {
public:
    FFmpegPuller();
    ~FFmpegPuller() override;

    // ==================== IPuller ====================

    bool Open(const std::string& url) override;
    void Close() override;
    bool ReadPacket(std::shared_ptr<MediaPacket>& packet) override;
    MultiStreamInfo GetStreamInfo() const override;
    void SetEventCallback(EventCallback cb) override;

    // ==================== 工具 ====================

    /// @brief AVCodecID -> CodecType 映射
    static CodecType MapCodecID(AVCodecID id);

    // ==================== 扩展配置 ====================

    void SetConnectTimeoutMs(int ms) override;
    void SetReadTimeoutMs(int ms) override;
    void SetLowLatency(bool enable) override;
    void SetCredentials(const std::string& username, const std::string& password) override;
    void SetRtspTransport(const std::string& transport) override;
    void SetRtspAutoSwitchToTcp(bool enable) override;
    void SetRtspAutoSwitchTimeoutMs(int ms) override;

private:
    /// @brief FFmpeg 中断回调上下文
    struct InterruptContext {
        std::atomic<bool> interrupted{false};
        std::atomic<bool> timed_out{false};
        std::chrono::steady_clock::time_point start_time;
        int timeout_ms{5000};
    };

    std::string BuildRtspTransportOption() const;

    // ── FFmpeg 资源 ──
    AVFormatContext*   fmt_ctx_{nullptr};     ///< 格式上下文
    std::map<int, AVCodecParameters*> codecpars_; ///< 编码参数对象池
    // AVCodecParameters* video_codecpar_{nullptr};     ///< 选中视频流的编码参数
    // AVCodecParameters* audio_codecpar_{nullptr};     ///< 选中音频流的编码参数

    InterruptContext   interrupt_ctx_;         ///< 中断回调上下文
    MultiStreamInfo    cached_info_;           ///< 缓存的流信息
    std::mutex         io_mutex_;              ///< 避免 Close 与 av_read_frame 并发关闭句柄

    // ── 配置 ──
    int    connect_timeout_ms_{5000};          ///< 连接超时（毫秒）
    int    read_timeout_ms_{10000};            ///< 读超时（毫秒）
    bool   low_latency_{true};                 ///< 低延迟模式
    std::string rtsp_transport_{"udp"};         ///< RTSP transport: udp / tcp / ...
    bool   rtsp_auto_switch_tcp_{false};       ///< UDP 超时后是否允许 FFmpeg 尝试 TCP
    int    rtsp_auto_switch_timeout_ms_{10000};///< UDP->TCP 切换等待时间（毫秒）
    std::string username_;                     ///< 鉴权用户名
    std::string password_;                     ///< 鉴权密码

    // ── 对象池 ──
    std::vector<AVPacket*> packet_pool_;       ///< AVPacket 对象池
    std::mutex             pool_mutex_;        ///< 保护对象池的互斥锁

    // ── 回调 ──
    EventCallback event_cb_;                   ///< 事件回调
};