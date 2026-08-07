#include "mediaflow/media_timing.h"

#include "media/simple_buffer.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using namespace mediaflow;

std::shared_ptr<MediaPacket> MakePacket(std::int64_t pts,
                                        std::int64_t dts,
                                        std::uint64_t generation) {
    auto packet = std::make_shared<MediaPacket>();
    packet->type = MediaType::VIDEO;
    packet->codec = CodecType::H264;
    packet->pts = pts;
    packet->dts = dts;
    packet->duration = 40;
    packet->time_base = Rational{1, 1000};
    packet->keyframe = pts == 0;
    packet->buffer = std::make_shared<SimpleBuffer>(std::vector<std::uint8_t>{1});
    (void)generation;
    return packet;
}

void TestTimestampRescaleKeepsPtsAndDtsSeparate() {
    MediaPacket packet;
    packet.pts = 90'000;
    packet.dts = 89'100;
    packet.duration = 3'000;
    packet.time_base = Rational{1, 90'000};

    const auto timing = GetMediaTiming(packet);
    assert(timing.pts_valid);
    assert(timing.dts_valid);
    assert(timing.duration_valid);
    assert(timing.pts_us == 1'000'000);
    assert(timing.dts_us == 990'000);
    assert(timing.duration_us == 33'333);

    packet.pts = 0;
    packet.dts = kNoTimestamp;
    const auto missing = GetMediaTiming(packet);
    assert(missing.pts_valid);
    assert(missing.pts_us == 0);
    assert(!missing.dts_valid);
    assert(missing.dts_us == kNoTimestamp);
    assert(TimestampToMicroseconds(1, Rational{0, 1}) == kNoTimestamp);
}

void TestAudioMasterAndGenerationReset() {
    UnifiedClock clock;
    clock.Start(7, 0);
    clock.UpdateAudioPosition(7, 0);
    auto snapshot = clock.Snapshot();
    assert(snapshot.valid);
    assert(snapshot.source == ClockSource::Audio);
    assert(snapshot.position_us == 0);
    assert(snapshot.generation == 7);
    assert(clock.HasAudioPosition());

    clock.UpdateAudioPosition(8, 50'000);
    snapshot = clock.Snapshot();
    assert(snapshot.generation == 8);
    assert(snapshot.position_us == 50'000);
    assert(snapshot.discontinuity > 0);

    clock.NotifyDiscontinuity(9, 100'000);
    snapshot = clock.Snapshot(false);
    assert(snapshot.generation == 9);
    assert(snapshot.position_us == 100'000);
    assert(snapshot.valid);
    assert(!clock.HasAudioPosition());
}

void TestVideoPtsScheduling() {
    UnifiedClock clock;
    clock.Start(3, 1'000'000);
    clock.UpdateAudioPosition(3, 1'000'000);
    const auto master = clock.Snapshot();

    VideoPtsScheduler scheduler;
    auto decision = scheduler.Decide(1'010'000, false, 3, master);
    assert(decision.action == VideoScheduleAction::Render);
    assert(decision.delta_us == 10'000);

    decision = scheduler.Decide(1'100'000, false, 3, master);
    assert(decision.action == VideoScheduleAction::Wait);
    assert(decision.wait_us == 20'000);

    decision = scheduler.Decide(900'000, false, 3, master);
    assert(decision.action == VideoScheduleAction::Drop);

    // 关键帧即使晚到也必须保留，给解码恢复提供新的锚点。
    decision = scheduler.Decide(900'000, true, 3, master);
    assert(decision.action == VideoScheduleAction::Render);
    assert(decision.keyframe);

    decision = scheduler.Decide(1'010'000, false, 2, master);
    assert(decision.clock_reset);
    assert(decision.action == VideoScheduleAction::Render);
}

void TestDtsInterleavingUsesDecodeOrder() {
    DtsInterleaverConfig config;
    config.max_pending_packets = 2;
    config.max_pending_span_us = 0;
    DtsInterleaver interleaver(config);

    auto make_item = [](std::int64_t pts, std::int64_t dts) {
        DtsPacket item;
        item.packet = MakePacket(pts, dts, 1);
        item.timing = GetMediaTiming(*item.packet);
        item.generation = 1;
        return item;
    };

    assert(interleaver.Push(make_item(0, 0)).empty());
    assert(interleaver.Push(make_item(80, 40)).empty());

    // 第三个包的 PTS 更早，但 DTS 更晚；触发容量边界时必须先吐 dts=0，
    // 不能按 PTS=0/40/80 的显示顺序误排。
    auto emitted = interleaver.Push(make_item(40, 80));
    assert(emitted.size() == 1);
    assert(emitted.front().timing.dts_us == 0);
    assert(interleaver.PendingSize() == 2);

    emitted = interleaver.Flush();
    assert(emitted.size() == 2);
    assert(emitted[0].timing.dts_us == 40'000);
    assert(emitted[1].timing.dts_us == 80'000);

    // 代次变化先刷旧缓存，再建立新缓存，不能跨重连排序。
    interleaver.Push(make_item(120, 120));
    emitted = interleaver.Push([&]() {
        auto item = make_item(0, 0);
        item.generation = 2;
        return item;
    }());
    assert(emitted.size() == 1);
    assert(emitted.front().generation == 1);
    assert(interleaver.PendingSize() == 1);
}

} // namespace

int main() {
    TestTimestampRescaleKeepsPtsAndDtsSeparate();
    TestAudioMasterAndGenerationReset();
    TestVideoPtsScheduling();
    TestDtsInterleavingUsesDecodeOrder();
    std::cout << "MediaFlow timing tests passed\n";
    return 0;
}
