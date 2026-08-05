#pragma once

/**
 * @file media_nodes.h
 * @brief MediaFlow 与现有 media 模块之间的第一批适配节点。
 *
 * 本文件只放媒体链路的边界对象和节点声明，调度仍由 MediaFlow Graph
 * 负责。节点之间不直接共享裸回调，而是传递带 generation 的消息封装：
 * 当拉流器重连或节点重新启动时，解码节点可以据此拒绝旧连接晚到的包。
 */

#include "mediaflow/core/node.h"

#include "media/decoder/i_decoder.h"
#include "media/puller/i_puller.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mediaflow {

/// Source 到媒体节点之间传递的压缩包消息。
struct MediaPacketMessage {
    std::shared_ptr<MediaPacket> packet;                   ///< 压缩媒体包。
    std::shared_ptr<const MediaStreamInfo> stream_info;    ///< 当前包所属轨道描述。
    std::uint64_t generation{0};                           ///< 拉流连接代次。

    bool Valid() const {
        return packet != nullptr && generation != 0;
    }
};

/// Decoder 到下游节点之间传递的解码帧消息。
struct MediaFrameMessage {
    std::shared_ptr<MediaFrame> frame; ///< 解码后的媒体帧。
    std::uint64_t generation{0};       ///< 产生该帧的拉流连接代次。

    bool Valid() const {
        return frame != nullptr && generation != 0;
    }
};

/// Source 节点的重连策略。
struct StreamSourceNodeOptions {
    int reconnect_interval_ms{100}; ///< 可恢复读错误后的等待时间。
    int max_reconnect_count{-1};    ///< 连续重连上限，-1 表示不限制。
};

/// 轨道选择条件。
struct TrackSelection {
    int stream_index{-1};             ///< FFmpeg stream index；-1 表示首个匹配视频轨。
    CodecType codec{CodecType::UNKNOWN}; ///< 可选编码格式约束。
};

/**
 * @brief 从 IPuller 读取压缩包的 MediaFlow 源节点。
 *
 * 节点在自己的读取线程中调用 IPuller，避免阻塞 Graph 的业务 Executor。
 * SourceNode 的启动优先级高于普通节点，因此 Graph 会先完成路由和解码
 * 节点的 Init/Start，再打开并读取第一条媒体包。
 *
 * A1 使用 IPuller 的结构化 ReadPacketResult()，只在 RetryableError 时重连；
 * EOS、FatalError 和主动停止均不会被误当成无限重连。每次成功重连都会
 * 递增 generation，并将新代次附在后续消息上。
 */
class StreamSourceNode final : public SourceNode<MediaPacketMessage> {
public:
    StreamSourceNode(std::string stream_id,
                     std::unique_ptr<IPuller> puller,
                     std::string url,
                     StreamSourceNodeOptions options = {});
    ~StreamSourceNode() override;

    StreamSourceNode(const StreamSourceNode&) = delete;
    StreamSourceNode& operator=(const StreamSourceNode&) = delete;

    bool Init() override;
    bool Start() override;
    void Stop() override;
    void Deinit() override;
    std::string Name() const override;

    /// 获取最近一次成功打开连接得到的流描述。
    MultiStreamInfo StreamInfo() const;

    /// 获取当前连接代次，便于监控和测试。
    std::uint64_t Generation() const;

    /// 判断读取线程是否已经因 EOS/致命错误结束。
    bool Finished() const;

private:
    void ReadLoop(std::uint64_t generation);
    bool OpenGeneration(std::uint64_t generation);
    bool WaitBeforeReconnect();
    std::shared_ptr<const MediaStreamInfo> FindStreamInfo(
        const std::shared_ptr<MediaPacket>& packet) const;
    void UpdateStreamInfo(const MultiStreamInfo& info);

    std::string stream_id_;
    std::unique_ptr<IPuller> puller_;
    std::string url_;
    StreamSourceNodeOptions options_;

    mutable std::mutex info_mutex_;
    MultiStreamInfo stream_info_;

    std::atomic<bool> stop_requested_{true};
    std::atomic<bool> finished_{false};
    std::atomic<std::uint64_t> generation_{0};
    std::thread read_thread_;
};

/**
 * @brief 从所有压缩包中选择一路视频轨。
 *
 * Source 可以同时输出音频和多个视频轨。TrackRouterNode 将轨道选择集中
 * 在一个可观察节点中，避免 Decoder 通过 codec 或包顺序猜测输入轨道。
 */
class TrackRouterNode final : public INode {
public:
    explicit TrackRouterNode(TrackSelection selection = {});

    bool RegisterPorts(PortRegistry& registry) override;
    bool Start() override;
    void Stop() override;
    std::string Name() const override;

    /// 输入端口，接收 Source 的全部压缩包。
    InputPort<MediaPacketMessage>& Input();

    /// 视频输出端口，连接到 DecoderNode。
    OutputPort<MediaPacketMessage>& VideoOutput();

private:
    void Process(MediaPacketMessage message);

    InputPort<MediaPacketMessage> input_;
    OutputPort<MediaPacketMessage> video_output_;
    TrackSelection selection_;
    int selected_stream_index_{-1};
    std::uint64_t selected_generation_{0};
    std::atomic<bool> accepting_{false};
};

/**
 * @brief 将一路压缩视频包适配到 IDecoder 的节点。
 *
 * Decoder 在 Init 阶段安装回调；如果构造时提供了 stream_info，则同时完成
 * 解码器 Open。若流信息只能在 Source 连接后获得，则第一次收到包时按消息
 * 中的 stream_info 延迟 Open，但此时 Graph 的输入边和 Decoder 节点已经就绪，
 * 不会出现“先读包、后注册 subscriber”的首包丢失窗口。
 */
class DecoderNode final : public INode {
public:
    explicit DecoderNode(std::unique_ptr<IDecoder> decoder,
                         MediaStreamInfo stream_info = {});
    ~DecoderNode() override;

    DecoderNode(const DecoderNode&) = delete;
    DecoderNode& operator=(const DecoderNode&) = delete;

    bool RegisterPorts(PortRegistry& registry) override;
    bool Init() override;
    bool Start() override;
    void Stop() override;
    void Deinit() override;
    std::string Name() const override;

    /// 输入压缩包。
    InputPort<MediaPacketMessage>& Input();

    /// 输出解码帧。
    OutputPort<MediaFrameMessage>& Output();

    /// Graph 具备 graceful barrier 后由上层显式调用，输出 decoder 残留帧。
    bool Flush();

    /// 最近一次解码失败的简要原因，便于节点级监控。
    std::string LastError() const;

private:
    void Process(MediaPacketMessage message);
    void OnDecodedFrame(std::shared_ptr<MediaFrame> frame);
    bool OpenForMessage(const MediaPacketMessage& message);
    bool IsUsableStreamInfo(const MediaStreamInfo& info) const;
    bool SameStream(const MediaStreamInfo& left,
                    const MediaStreamInfo& right) const;
    void SetError(std::string message);
    void SetErrorLocked(std::string message);

    std::unique_ptr<IDecoder> decoder_;
    MediaStreamInfo configured_stream_info_;
    MediaStreamInfo active_stream_info_;
    InputPort<MediaPacketMessage> input_;
    OutputPort<MediaFrameMessage> output_;

    mutable std::mutex state_mutex_;
    std::string last_error_;
    std::uint64_t active_generation_{0};
    std::uint64_t callback_generation_{0};
    bool decoder_open_{false};
    bool accepting_{false};
};

} // namespace mediaflow
