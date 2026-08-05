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
#include "media/encoder/i_encoder.h"
#include "media/puller/i_puller.h"
#include "media/publisher/i_publisher.h"

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

/// Encoder 到 PublisherSink 之间传递的编码包消息。
struct EncodedPacketMessage {
    std::shared_ptr<MediaPacket> packet; ///< 编码后的压缩包。
    EncodedTrackInfo track_info;         ///< Encoder 实际输出轨道描述。
    int track_id{0};                     ///< MediaFlow 内部输出轨道 ID。
    std::uint64_t generation{0};         ///< 产生该包的输入连接代次。

    bool Valid() const {
        return packet != nullptr && generation != 0 &&
               track_info.media_type != MediaType::UNKNOWN &&
               track_info.codec_type != CodecType::UNKNOWN;
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

/// Encoder 节点的输出配置。
struct EncoderNodeOptions {
    int track_id{0}; ///< PublisherConfig 中对应的 track_id。
};

/// PublisherSink 的启动和断线恢复策略。
struct PublisherSinkNodeOptions {
    bool wait_for_keyframe_on_start{true}; ///< 首次启动前丢弃非关键视频包。
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

/**
 * @brief 将解码帧适配为编码包的节点。
 *
 * EncoderNode 在 Init 阶段打开 IEncoder，并在每个 FrameMessage 到达时把
 * Encode 返回的全部 packet 按原顺序发送到下游。每个输出包都会补齐本节点
 * 负责的 track_id，但不会覆盖 Encoder 已生成的 time_base、PTS、DTS 和 duration。
 * 重连代次变化时直接关闭旧 encoder 并重开，不把旧代次的 flush 包混入新链路。
 */
class EncoderNode final : public INode {
public:
    EncoderNode(std::unique_ptr<IEncoder> encoder,
                EncoderConfig config,
                EncoderNodeOptions options = {});
    ~EncoderNode() override;

    EncoderNode(const EncoderNode&) = delete;
    EncoderNode& operator=(const EncoderNode&) = delete;

    bool RegisterPorts(PortRegistry& registry) override;
    bool Init() override;
    bool Start() override;
    void Stop() override;
    void Deinit() override;
    std::string Name() const override;

    InputPort<MediaFrameMessage>& Input();
    OutputPort<EncodedPacketMessage>& Output();

    /// 在 Graph graceful barrier 内显式输出编码器残留包。
    bool Flush();

    EncodedTrackInfo OutputInfo() const;
    std::string LastError() const;

private:
    void Process(MediaFrameMessage message);
    bool OpenForGeneration(std::uint64_t generation);
    bool EmitPackets(const std::vector<PacketPtr>& packets,
                     std::uint64_t generation);
    void SetError(std::string message);
    void SetErrorLocked(std::string message);

    std::unique_ptr<IEncoder> encoder_;
    EncoderConfig config_;
    EncoderNodeOptions options_;
    InputPort<MediaFrameMessage> input_;
    OutputPort<EncodedPacketMessage> output_;

    mutable std::mutex state_mutex_;
    std::string last_error_;
    EncodedTrackInfo output_info_;
    std::uint64_t active_generation_{0};
    bool encoder_open_{false};
    bool accepting_{false};
};

/**
 * @brief 将编码包发布到现有 IPublisher 的终端节点。
 *
 * Publisher 不在 Graph Start 阶段盲目启动，因为此时 Encoder 还没有提供
 * 实际 time_base、尺寸和 extradata。节点收到第一条完整 EncodedPacketMessage
 * 后合并轨道描述、启动 Publisher，并在同一 Serialized 节点上下文中执行后续
 * Publish。Publisher 断线或输入 generation 变化后，非关键视频包会被丢弃，
 * 直到下一关键帧重新建立可播放入口。
 */
class PublisherSinkNode final : public SinkNode<EncodedPacketMessage> {
public:
    explicit PublisherSinkNode(
        PublisherConfig config,
        std::unique_ptr<IPublisher> publisher = nullptr,
        PublisherSinkNodeOptions options = {});
    ~PublisherSinkNode() override;

    PublisherSinkNode(const PublisherSinkNode&) = delete;
    PublisherSinkNode& operator=(const PublisherSinkNode&) = delete;

    bool Init() override;
    bool Start() override;
    void Stop() override;
    void Deinit() override;
    int StartPriority() const override;
    std::string Name() const override;

    PublisherStats Stats() const;
    PublisherResult LastResult() const;
    std::string LastError() const;
    PublisherConfig Config() const;

protected:
    void Process(EncodedPacketMessage message) override;

private:
    bool PrepareTrack(const EncodedPacketMessage& message);
    bool StartPublisher(const EncodedPacketMessage& message);
    bool IsVideoKeyframe(const EncodedPacketMessage& message) const;
    void SetError(std::string message);

    PublisherConfig config_;
    std::unique_ptr<IPublisher> publisher_;
    PublisherSinkNodeOptions options_;

    mutable std::mutex state_mutex_;
    PublisherResult last_result_;
    std::string last_error_;
    std::uint64_t active_generation_{0};
    bool accepting_{false};
    bool publisher_started_{false};
    bool awaiting_keyframe_{false};
};

} // namespace mediaflow
