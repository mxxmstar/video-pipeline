#include "render/av_sync_controller.h"

#include <chrono>
#include <iostream>

namespace {

bool TestRenderNearMasterClock() {
    render::AvSyncController controller;
    const render::MediaClockSnapshot clock{
        1'000'000,
        render::MediaClockSource::Audio,
        true,
    };

    const auto decision = controller.Decide(1'010'000, clock);
    return decision.action == render::AvSyncAction::Render &&
           decision.delta_us == 10'000;
}

bool TestDropLateFrame() {
    render::AvSyncController controller;
    const render::MediaClockSnapshot clock{
        1'000'000,
        render::MediaClockSource::Audio,
        true,
    };

    const auto decision = controller.Decide(900'000, clock);
    return decision.action == render::AvSyncAction::Drop &&
           decision.delta_us == -100'000;
}

bool TestWaitEarlyFrame() {
    render::AvSyncConfig config;
    config.early_threshold_ms = 20;
    config.max_wait_ms = 15;
    render::AvSyncController controller(config);
    const render::MediaClockSnapshot clock{
        1'000'000,
        render::MediaClockSource::System,
        true,
    };

    const auto decision = controller.Decide(1'080'000, clock);
    return decision.action == render::AvSyncAction::Wait &&
           decision.delta_us == 80'000 &&
           decision.wait_duration == std::chrono::milliseconds(15);
}

bool TestDisabledSyncAlwaysRenders() {
    render::AvSyncConfig config;
    config.enabled = false;
    render::AvSyncController controller(config);
    const render::MediaClockSnapshot clock{
        1'000'000,
        render::MediaClockSource::Audio,
        true,
    };

    const auto decision = controller.Decide(100'000, clock);
    return decision.action == render::AvSyncAction::Render;
}

} // namespace

int main() {
    if (!TestRenderNearMasterClock()) {
        std::cerr << "TestRenderNearMasterClock failed\n";
        return 1;
    }
    if (!TestDropLateFrame()) {
        std::cerr << "TestDropLateFrame failed\n";
        return 1;
    }
    if (!TestWaitEarlyFrame()) {
        std::cerr << "TestWaitEarlyFrame failed\n";
        return 1;
    }
    if (!TestDisabledSyncAlwaysRenders()) {
        std::cerr << "TestDisabledSyncAlwaysRenders failed\n";
        return 1;
    }

    std::cout << "AV sync controller tests passed\n";
    return 0;
}
