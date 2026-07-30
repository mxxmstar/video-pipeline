#include "media/protocol/avtp/avtp_timestamp_mapper.h"

#include <cassert>
#include <cstdint>

namespace {

void TestNormalMappingUsesAvtpDelta() {
    media::avtp::AvtpTimestampMapper mapper;
    constexpr std::int64_t capture_start_us = 10'000'000;

    const auto first = mapper.Map(1, 1'000'000'000U,
                                  capture_start_us,
                                  true,
                                  false,
                                  false);
    // 首包建立锚点，所以输出等于该包抓包时间。
    assert(first == capture_start_us);

    // 抓包间隔故意设成 41 ms；AVTP presentation 间隔为 40 ms，PTS 应以后者为准。
    const auto second = mapper.Map(1, 1'040'000'000U,
                                   capture_start_us + 41'000,
                                   true,
                                   false,
                                   false);
    assert(second == capture_start_us + 40'000);
}

void TestTimestampWrapIsExpanded() {
    media::avtp::AvtpTimestampMapper mapper;
    constexpr std::int64_t capture_start_us = 20'000'000;
    constexpr std::uint32_t first_raw = 0xFFFFF000U;
    constexpr std::uint32_t delta_ns = 1'000'000U;
    constexpr std::uint32_t wrapped_raw = first_raw + delta_ns;

    mapper.Map(1, first_raw, capture_start_us, true, false, false);
    const auto mapped = mapper.Map(1, wrapped_raw,
                                   capture_start_us + 1'100,
                                   true,
                                   false,
                                   false);
    assert(mapped == capture_start_us + 1'000);
    assert(mapper.GetStats().forward_wraps == 1);
}

void TestInvalidAndUncertainFallBackToCaptureTime() {
    media::avtp::AvtpTimestampMapper mapper;
    assert(mapper.Map(1, 123, 1000, false, false, false) == 1000);
    assert(mapper.Map(1, 456, 2000, true, true, false) == 2000);
    assert(!mapper.IsInitialized());
    assert(mapper.GetStats().invalid_fallbacks == 1);
    assert(mapper.GetStats().uncertain_fallbacks == 1);
}

void TestMediaClockRestartReanchorsTimeline() {
    media::avtp::AvtpTimestampMapper mapper;
    mapper.Map(1, 1'000'000U, 1'000'000, true, false, false);

    // 发送端显式置 MCR 后，不延续旧 timestamp，而以当前抓包时刻重建锚点。
    const auto restarted =
        mapper.Map(1, 20'000U, 2'000'000, true, false, true);
    assert(restarted == 2'000'000);
    assert(mapper.GetStats().media_clock_restarts == 1);
}

void TestLargeClockJumpTriggersDiscontinuityReset() {
    media::avtp::AvtpTimestampMapper mapper;
    mapper.Map(1, 1'000'000U, 1'000'000, true, false, false);

    // capture 已过去 10 秒，但 AVTP 低 32 位无法得到相符走势，应主动重新锚定。
    const auto mapped =
        mapper.Map(1, 2'000'000U, 11'000'000, true, false, false);
    assert(mapped == 11'000'000);
    assert(mapper.GetStats().discontinuity_resets == 1);
}

} // namespace

int main() {
    TestNormalMappingUsesAvtpDelta();
    TestTimestampWrapIsExpanded();
    TestInvalidAndUncertainFallBackToCaptureTime();
    TestMediaClockRestartReanchorsTimeline();
    TestLargeClockJumpTriggersDiscontinuityReset();
    return 0;
}
