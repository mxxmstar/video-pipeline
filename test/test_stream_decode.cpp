/// @file test_stream_decode.cpp
/// 测试 RTSP 拉流 + 音视频解码完整流程

#include <iostream>
#include <atomic>
#include <thread>

#include "media/stream/stream_session.h"
#include "media/stream/stream_source.h"
#include "media/puller/ffmpeg_puller.h"
#include "media/decoder/ffmpeg_decoder.h"
#include <asio.hpp>

struct IOTestContext {
    asio::io_context io;
    asio::executor_work_guard<asio::io_context::executor_type> work;
    std::thread thread;

    IOTestContext()
        : work(asio::make_work_guard(io))
        , thread([this]() { io.run(); }) {}

    ~IOTestContext() {
        work.reset();
        if (thread.joinable())
            thread.join();
    }
};

int main()
{
    std::cout << "=== Test Stream Decode ===" << std::endl;
    std::string url = "rtsp://192.168.24.169/live/mainstream";

    // 1. 创建拉流器
    auto puller = std::make_unique<FFmpegPuller>();
    puller->SetConnectTimeoutMs(3000);
    puller->SetReadTimeoutMs(3000);
    puller->SetLowLatency(true);

    // 2. 创建会话和源
    IOTestContext io_ctx;
    auto session = std::make_shared<MediaStreamSession>(io_ctx.io);
    session->SetPuller(std::move(puller));
    session->SetUrl(url);

    auto source = std::make_shared<MediaStreamSource>("test_stream");
    source->SetSession(session);

    // 3. 创建解码器（支持音视频多路）
    std::shared_ptr<FFmpegDecoder> video_decoder;
    std::shared_ptr<FFmpegDecoder> audio_decoder;

    std::atomic<int> video_frame_count{0};
    std::atomic<int> audio_frame_count{0};

    // 4. 启动拉流
    bool ret = source->Start();
    if (!ret) {
        std::cerr << "Stream start failed" << std::endl;
        return -1;
    }
    std::cout << "Stream started" << std::endl;

    // 5. 等待 StreamInfo 回调，创建解码器
    MultiStreamInfo multi_info = source->GetStreamInfo();
    int wait_count = 0;
    while (!multi_info.HasVideoStream() && !multi_info.HasAudioStream() && wait_count < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        multi_info = source->GetStreamInfo();
        wait_count++;
    }

    if (!multi_info.HasVideoStream() && !multi_info.HasAudioStream()) {
        std::cerr << "No video or audio stream found" << std::endl;
        return -1;
    }

    // 6. 打开视频解码器
    if (multi_info.HasVideoStream()) {
        video_decoder = std::make_shared<FFmpegDecoder>();
        video_decoder->SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
            if (frame) {
                video_frame_count++;
                const auto& meta = std::get<VideoFrameMeta>(frame->meta);
                if (video_frame_count % 30 == 1) {
                    LOG_INFO("[Video] frame #{}: {}x{}, format={}, pts={}",
                             video_frame_count.load(),
                             meta.width, meta.height,
                             static_cast<int>(meta.pixel_format),
                             frame->time.pts_us);
                }
            }
        });
        video_decoder->Open(multi_info.stream_infos[multi_info.video_stream_idx_]);
        LOG_INFO("Video decoder opened: codec={}",
                 static_cast<int>(multi_info.stream_infos[multi_info.video_stream_idx_].codec_type));
    }

    // 7. 打开音频解码器
    if (multi_info.HasAudioStream()) {
        audio_decoder = std::make_shared<FFmpegDecoder>();
        audio_decoder->SetFrameCallback([&](std::shared_ptr<MediaFrame> frame) {
            if (frame) {
                audio_frame_count++;
                const auto& meta = std::get<AudioFrameMeta>(frame->meta);
                if (audio_frame_count % 100 == 1) {
                    LOG_INFO("[Audio] frame #{}: sample_rate={}, channels={}, samples={}",
                             audio_frame_count.load(),
                             meta.sample_rate, meta.channels,
                             meta.nb_samples);
                }
            }
        });
        audio_decoder->Open(multi_info.stream_infos[multi_info.audio_stream_idx_]);
        LOG_INFO("Audio decoder opened: codec={}",
                 static_cast<int>(multi_info.stream_infos[multi_info.audio_stream_idx_].codec_type));
    }

    // 8. 订阅包 -> 送入对应解码器
    source->AddPacketSubscriber(
        [&](std::shared_ptr<MediaPacket> pkt) {
            if (!pkt || !pkt->buffer) return;

            if (pkt->type == MediaType::VIDEO && video_decoder) {
                video_decoder->Decode(pkt);
            } else if (pkt->type == MediaType::AUDIO && audio_decoder) {
                audio_decoder->Decode(pkt);
            }
    });

    // 9. 启动统计打印
    source->StartStatsPrint(5);

    // 10. 阻塞主线程，等待用户输入退出
    std::cout << "Press Enter to stop..." << std::endl;
    std::cin.get();

    source->StopStatsPrint();
    source->Stop();

    LOG_INFO("Total decoded: video={} frames, audio={} frames",
             video_frame_count.load(), audio_frame_count.load());

    return 0;
}