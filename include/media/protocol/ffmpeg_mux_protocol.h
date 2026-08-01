#pragma once

#include "media/protocol/i_protocol.h"

#include <chrono>
#include <unordered_map>

struct AVFormatContext;
struct AVBSFContext;
struct AVPacket;

/// @brief 基于 FFmpeg libavformat 的编码包发布协议。
///
/// 该类只负责把已经编码好的 EncodedAccessUnit 写入 FFmpeg 输出 muxer，
/// 不负责解码、编码，也不负责从 MediaPacket 推断 track。MediaPacket 到
/// EncodedAccessUnit 的转换由 FfmpegProtocolAdapter 完成。
class FfmpegMuxProtocol : public IProtocol {
public:
    FfmpegMuxProtocol() = default;
    ~FfmpegMuxProtocol() override;

    FfmpegMuxProtocol(const FfmpegMuxProtocol&) = delete;
    FfmpegMuxProtocol& operator=(const FfmpegMuxProtocol&) = delete;

    PublisherResult Start(
        const PublisherConfig& config,
        const std::vector<MediaTrackConfig>& tracks) override;
    PublisherResult Write(const EncodedAccessUnit& access_unit) override;
    void Stop() override;

    std::string GetOutputUrl() const override;
    PublisherStats GetStats() const override;

private:
    // 按 Publisher track_id 创建 AVStream，并保存 track_id 到 stream index 的映射。
    PublisherResult BuildStreams(const std::vector<MediaTrackConfig>& tracks);

    // 为配置中显式指定的 track 创建 AVBSFContext。filter 的输入参数来自对应
    // AVStream 的 codecpar 和 time_base，输出 packet 仍写回同一条 stream。
    PublisherResult BuildBitstreamFilters();

    // 将上游编码包转换为 FFmpeg AVPacket，并完成 stream 选择、时间基转换和
    // keyframe 标记。该函数不执行实际 I/O。
    PublisherResult BuildPacket(const EncodedAccessUnit& access_unit,
                                AVPacket* out) const;

    // 构造 AVPacket 后，按需经过 bitstream filter，再交给 WriteAvPacket 写出。
    PublisherResult WriteEncodedPacket(const EncodedAccessUnit& access_unit);

    // 对已经准备好的 AVPacket 执行一次同步 av_write_frame，并更新统计信息。
    PublisherResult WriteAvPacket(AVPacket* packet);

    // 释放当前 FFmpeg 输出会话并按配置重新创建 context、stream、filter 和 header。
    PublisherResult Reconnect();

    // 释放 FFmpeg context 及其关联资源。正常停止时写 trailer，重连或启动失败
    // 时不写 trailer，避免对已经断开的远端连接再次执行无意义的收尾 I/O。
    void FreeContext(bool write_trailer);

    // 标记一次 FFmpeg I/O 操作的截止时间，供 AVIOInterruptCB 查询。
    void BeginIoOperation();

    // 清除当前 I/O 操作的超时状态，防止后续非 I/O 代码误触发中断回调。
    void EndIoOperation();

    // FFmpeg 在阻塞 I/O 期间调用的中断回调；返回非零表示达到本次操作的截止时间。
    static int InterruptCallback(void* opaque);

    // 将项目内部 CodecType 转换为 FFmpeg AVCodecID。
    static int MapCodecType(CodecType type);

    // Start 使用的完整配置和轨道快照。重连时依赖这两份快照重建输出会话。
    PublisherConfig config_;
    std::vector<MediaTrackConfig> tracks_;

    // FFmpeg 的 stream index 不一定等于上游 track_id，因此必须显式维护映射。
    std::unordered_map<int, int> track_to_stream_index_;

    // 按输出 stream index 保存 bitstream filter。一个 track 最多配置一个 filter，
    // 由 FFmpeg filter 内部处理 packet 的格式转换。
    std::unordered_map<int, AVBSFContext*> bitstream_filters_;

    AVFormatContext* fmt_ctx_{nullptr};

    // 只有 header 成功写出后才允许调用 av_write_frame/av_write_trailer。
    bool header_written_{false};

    // 重连建立新会话后，视频必须等到关键帧才能恢复；音频或非关键帧在此期间会被拒绝。
    bool waiting_for_keyframe_{false};

    // 表示 interrupt callback 当前是否服务于一项有明确截止时间的 I/O 操作。
    bool io_operation_active_{false};
    std::chrono::steady_clock::time_point io_deadline_{};

    // 统计当前发布任务已经成功写出的 packet 和字节数。重连不会清零累计值。
    PublisherStats stats_;
};
