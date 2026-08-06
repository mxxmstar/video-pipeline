#pragma once

#include <functional>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "media/media_packet.h"
#include "media/stream/stream_info.h"

/// @brief 拉流器接口（协议适配层）
///
/// 仅负责三件事：
///   1. Open()       — 建立底层传输连接
///   2. ReadPacket() — 从网络读取一个媒体包
///   3. Close()      — 断开底层连接
///
/// 不负责：
///   - 重连 / watchdog / 状态机
///   - 统计 / 超时检测
///   - decoder / pipeline / source 管理
///
/// 上述功能统一由 StreamSession 和 MediaStreamSource 上层管理。
class IPuller {
public:
    virtual ~IPuller() = default;

    /// @brief 一次读取的结构化结果。
    ///
    /// 旧接口只有 bool，无法区分“暂时没有数据”“本地文件结束”和“网络错误”。
    /// MediaFlow SourceNode 必须依靠这些状态决定继续读取、发送 EOS 还是重连。
    enum class PullReadStatus {
        Packet,          ///< 成功取得 packet，result.packet 非空
        NoData,          ///< 本次没有可交付 packet，可稍后继续读取
        EOS,             ///< 输入正常结束，不应按网络故障重连
        RetryableError,  ///< 暂时性 I/O 错误或超时，可以重连
        FatalError,      ///< 不可恢复的读取错误
        Stopped,         ///< Puller 已被关闭或读取被主动中断
    };

    struct PullReadResult {
        PullReadStatus status{PullReadStatus::FatalError};
        std::shared_ptr<MediaPacket> packet;
        int native_error{0};
        std::string message;

        static PullReadResult PacketResult(std::shared_ptr<MediaPacket> value) {
            return {PullReadStatus::Packet, std::move(value), 0, {}};
        }
    };

    // ==================== 生命周期 ====================

    /// @brief 打开流
    /// @param url 流地址（如 rtsp://…、http://…）
    /// @return true 连接成功
    virtual bool Open(const std::string& url) = 0;

    /// @brief 关闭流
    virtual void Close() = 0;

    /// @brief 请求中断当前阻塞读，但不等待资源释放。
    ///
    /// GracefulStop 使用这个阶段先停止 Source 继续生产，再排空已经进入
    /// MediaFlow 的消息。实现必须尽快返回；完整的 Close() 仍由 Stop 阶段执行。
    /// 不会阻塞的 Puller 可以保持默认实现。
    virtual void RequestStop() {}

    /// @brief 读取一个媒体包
    /// @param[out] packet 输出包（成功时为有效对象，失败时为 nullptr）
    /// @return true 成功读取；false 读取失败或流结束
    ///
    /// 当返回 false 时，调用方可通过 GetStreamInfo() 判断区别：
    /// 若 info 有效则为 EOF，否则为错误。
    virtual bool ReadPacket(std::shared_ptr<MediaPacket>& packet) = 0;

    /// @brief 读取一个媒体包并返回可用于状态机决策的结果。
    ///
    /// 默认实现把旧 bool 接口包装成兼容结果，具体 Puller 应覆盖此方法以提供
    /// EOS、超时和主动停止的准确语义。
    virtual PullReadResult ReadPacketResult() {
        std::shared_ptr<MediaPacket> packet;
        if (!ReadPacket(packet)) {
            return {PullReadStatus::FatalError, nullptr, 0,
                    "legacy puller read failed"};
        }
        if (!packet) {
            return {PullReadStatus::NoData, nullptr, 0,
                    "legacy puller returned no packet"};
        }
        return PullReadResult::PacketResult(std::move(packet));
    }

    // ==================== 元数据 ====================

    /// @brief 获取流信息
    virtual MultiStreamInfo GetStreamInfo() const = 0;

    // ==================== 回调 ====================

    /// @brief 拉流器层事件回调（协议异常等）
    using EventCallback = std::function<void(const std::string&)>;

    /// @brief 设置事件回调
    virtual void SetEventCallback(EventCallback cb) = 0;

    // ==================== 可选配置 ====================

    virtual void SetConnectTimeoutMs(int) {}
    virtual void SetReadTimeoutMs(int) {}
    virtual void SetLowLatency(bool) {}
    virtual void SetCredentials(const std::string&, const std::string&) {}
    virtual void SetRtspTransport(const std::string&) {}
    virtual void SetRtspAutoSwitchToTcp(bool) {}
    virtual void SetRtspAutoSwitchTimeoutMs(int) {}
};
