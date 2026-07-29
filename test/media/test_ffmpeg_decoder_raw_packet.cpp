#include "media/decoder/ffmpeg_decoder.h"
#include "media/puller/ffmpeg_puller.h"
#include "media/simple_buffer.h"

#include <iostream>
#include <memory>

#ifndef VIDEO_PIPELINE_TEST_MP4_PATH
#define VIDEO_PIPELINE_TEST_MP4_PATH "test.mp4"
#endif

namespace {

/// @brief 将 FFmpegPuller 输出的零拷贝 MediaPacket 复制成普通内存包。
///
/// FFmpegPuller 默认会把 AVPacket 放在 BackendHandle::FFMPEG 中，decoder 可以零拷贝使用；
/// AVTP/RTP 这类协议模块则通常只有一段连续编码字节，没有 AVPacket 后端对象。
/// 这里特意复制出 SimpleBuffer 并清空 backend，用来覆盖 FFmpegDecoder 的 raw packet 路径。
std::shared_ptr<MediaPacket> MakeRawPacketCopy(const std::shared_ptr<MediaPacket>& source) {
    if (!source || !source->buffer || !source->buffer->Data() || source->buffer->Size() == 0) {
        return nullptr;
    }

    auto raw = std::make_shared<MediaPacket>(*source);
    raw->buffer = std::make_shared<SimpleBuffer>(source->buffer->Data(), source->buffer->Size());
    raw->backend = {};
    return raw;
}

} // namespace

int main() {
    constexpr int kMaxPacketsToRead = 500;
    constexpr int kMinDecodedFrames = 1;

    FFmpegPuller puller;
    puller.SetLowLatency(false);
    puller.SetConnectTimeoutMs(3000);
    puller.SetReadTimeoutMs(3000);

    if (!puller.Open(VIDEO_PIPELINE_TEST_MP4_PATH)) {
        std::cerr << "open test media failed: " << VIDEO_PIPELINE_TEST_MP4_PATH << std::endl;
        return 1;
    }

    const MultiStreamInfo streams = puller.GetStreamInfo();
    if (!streams.HasVideoStream()) {
        std::cerr << "test media has no video stream" << std::endl;
        return 1;
    }

    FFmpegDecoder decoder;
    int decoded_frames = 0;
    decoder.SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
        if (frame && frame->type == MediaType::VIDEO) {
            ++decoded_frames;
        }
    });

    const MediaStreamInfo& video_info = streams.stream_infos[streams.video_stream_idx_];
    if (!decoder.Open(video_info)) {
        std::cerr << "open decoder failed" << std::endl;
        return 1;
    }

    int raw_packets_sent = 0;
    for (int i = 0; i < kMaxPacketsToRead && decoded_frames < kMinDecodedFrames; ++i) {
        std::shared_ptr<MediaPacket> source_packet;
        if (!puller.ReadPacket(source_packet)) {
            break;
        }
        if (!source_packet || source_packet->type != MediaType::VIDEO) {
            continue;
        }

        auto raw_packet = MakeRawPacketCopy(source_packet);
        if (!raw_packet) {
            std::cerr << "make raw packet copy failed" << std::endl;
            return 1;
        }

        if (!decoder.Decode(raw_packet)) {
            std::cerr << "decode raw packet failed" << std::endl;
            return 1;
        }
        ++raw_packets_sent;
    }

    // 部分编码流有 B 帧或内部重排序，关闭时冲刷一次，避免“已经吃到包但帧还没吐出”的假失败。
    decoder.Close();
    puller.Close();

    if (decoded_frames < kMinDecodedFrames) {
        std::cerr << "raw packet path produced no frame, raw_packets_sent="
                  << raw_packets_sent << std::endl;
        return 1;
    }

    std::cout << "raw packet decode ok, packets=" << raw_packets_sent
              << ", frames=" << decoded_frames << std::endl;
    return 0;
}
