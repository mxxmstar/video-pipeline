#include "media/puller/avtp_puller.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>

int main(int argc, char** argv) {
    // 手动 live probe 工具，不注册到 ctest。它用于现场确认：
    // - Npcap 是否能打开指定网卡；
    // - AvtpPuller::Open(format=auto) 是否能 probe 出 codec；
    // - ReadPacket() 是否能吐出完整媒体包。
    if (argc < 2) {
        std::cerr << "usage: avtp_live_probe <avtp-url>\n";
        return 2;
    }

    AvtpPuller puller;
    puller.SetEventCallback([](const std::string& message) {
        std::cerr << "event: " << message << "\n";
    });

    const std::string url = argv[1];
    if (!puller.Open(url)) {
        std::cerr << "open failed\n";
        return 1;
    }

    // 如果 format=auto probe 成功，Open() 返回后这里应已有确定 codec。
    const MultiStreamInfo info = puller.GetStreamInfo();
    if (info.HasVideoStream()) {
        const auto& stream = info.stream_infos[info.video_stream_idx_];
        const auto& detail = stream.get_detail<VideoStreamInfo>();
        std::cout << "stream codec=" << static_cast<int>(stream.codec_type)
                  << " size=" << detail.width << "x" << detail.height
                  << " fps=" << detail.fps << "\n";
    } else {
        std::cout << "no video stream info\n";
    }

    // 再读一个媒体包，确认组帧器不只是探测到了 codec，而是真的能输出 AU。
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        std::shared_ptr<MediaPacket> packet;
        if (!puller.ReadPacket(packet)) {
            continue;
        }
        if (!packet) {
            continue;
        }

        std::cout << "packet codec=" << static_cast<int>(packet->codec)
                  << " size=" << (packet->buffer ? packet->buffer->Size() : 0)
                  << " pts=" << packet->pts
                  << " keyframe=" << packet->keyframe << "\n";
        puller.Close();
        return 0;
    }

    std::cerr << "no media packet within timeout\n";
    puller.Close();
    return 1;
}
