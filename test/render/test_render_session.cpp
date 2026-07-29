#include "render/render_session.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "media/simple_buffer.h"

namespace {

using namespace std::chrono_literals;

struct FakeVideoState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::int64_t> rendered_pts;
    std::thread::id init_thread;
    std::thread::id shutdown_thread;
    bool block_first_render{false};
    bool first_render_entered{false};
    bool release_first_render{false};
    bool initialized{false};
    bool shutdown{false};
    std::atomic<bool> should_close{false};
};

/// @brief 无窗口的视频 renderer，用于确定性验证 session 队列和线程归属。
class FakeVideoRenderer final : public render::IVideoRenderer {
public:
    explicit FakeVideoRenderer(std::shared_ptr<FakeVideoState> state)
        : state_(std::move(state)) {}

    bool Init(const render::RenderConfig&) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->init_thread = std::this_thread::get_id();
        state_->initialized = true;
        return true;
    }

    bool Render(const MediaFrame& frame) override {
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (state_->block_first_render && state_->rendered_pts.empty()) {
            state_->first_render_entered = true;
            state_->cv.notify_all();
            state_->cv.wait(lock, [this] { return state_->release_first_render; });
        }
        state_->rendered_pts.push_back(frame.time.pts_us);
        state_->cv.notify_all();
        return true;
    }

    void PollEvents() override {}

    bool ShouldClose() const override {
        return state_->should_close.load();
    }

    void Shutdown() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->shutdown_thread = std::this_thread::get_id();
        state_->shutdown = true;
        state_->cv.notify_all();
    }

private:
    std::shared_ptr<FakeVideoState> state_;
};

struct FakeAudioState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::int64_t> rendered_pts;
    std::thread::id init_thread;
    std::thread::id shutdown_thread;
    render::audio::AudioRenderStats stats;
    bool shutdown{false};
};

/// @brief 无声卡的音频 renderer，只记录帧 PTS 和调用线程。
class FakeAudioRenderer final : public render::audio::IAudioRenderer {
public:
    explicit FakeAudioRenderer(std::shared_ptr<FakeAudioState> state)
        : state_(std::move(state)) {}

    bool Init(const render::audio::AudioRenderConfig&) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->init_thread = std::this_thread::get_id();
        return true;
    }

    bool Render(const MediaFrame& frame) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->rendered_pts.push_back(frame.time.pts_us);
        state_->stats.submitted_pcm_frames += 160;
        state_->stats.queued_pcm_frames = 80;
        state_->stats.dropped_pcm_frames = 40;
        state_->stats.underruns = 2;
        state_->stats.queued_pcm_chunks = 1;
        state_->cv.notify_all();
        return true;
    }

    void Shutdown() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->shutdown_thread = std::this_thread::get_id();
        state_->shutdown = true;
        state_->cv.notify_all();
    }

    std::int64_t PlayedPtsUs() const override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->rendered_pts.empty() ? 0 : state_->rendered_pts.back();
    }

    render::audio::AudioRenderStats GetStats() const override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->stats;
    }

private:
    std::shared_ptr<FakeAudioState> state_;
};

std::shared_ptr<MediaFrame> MakeFrame(MediaType type, std::int64_t pts_us) {
    auto frame = std::make_shared<MediaFrame>();
    frame->type = type;
    frame->time.pts_us = pts_us;
    frame->time.dts_us = pts_us;

    if (type == MediaType::VIDEO) {
        VideoFrameMeta meta;
        meta.pixel_format = PixelFormat::kRGB24;
        meta.width = 1;
        meta.height = 1;
        meta.plane_count = 1;
        meta.plane_info[0].stride = 3;
        meta.plane_info[0].size = 3;
        frame->meta = meta;
        frame->buffer =
            std::make_shared<SimpleBuffer>(std::vector<std::uint8_t>{0, 0, 0});
    } else {
        AudioFrameMeta meta;
        meta.sample_format = SampleFormat::S16;
        meta.sample_rate = 8'000;
        meta.channels = 1;
        meta.nb_samples = 160;
        meta.bytes_per_sample = 2;
        meta.plane_count = 1;
        meta.planes[0].stride = 2;
        meta.planes[0].size = 320;
        frame->meta = meta;
        frame->buffer =
            std::make_shared<SimpleBuffer>(std::vector<std::uint8_t>(320, 0));
    }
    return frame;
}

template <typename Predicate>
bool WaitUntil(Predicate predicate,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

bool TestVideoQueueDropsOldestFrame() {
    auto video_state = std::make_shared<FakeVideoState>();
    video_state->block_first_render = true;

    render::RenderSession session(
        std::make_unique<FakeVideoRenderer>(video_state), nullptr);
    render::RenderSessionConfig config;
    config.enable_video = true;
    config.enable_audio = false;
    config.max_video_queue_frames = 2;
    config.drop_late_video_frames = true;
    config.av_sync.enabled = false;

    if (!session.Init(config) || !session.Start()) {
        std::cerr << "queue test session start failed: " << session.LastError() << '\n';
        return false;
    }

    if (!session.SubmitFrame(MakeFrame(MediaType::VIDEO, 0))) {
        return false;
    }
    {
        std::unique_lock<std::mutex> lock(video_state->mutex);
        if (!video_state->cv.wait_for(
                lock, 1s, [&] { return video_state->first_render_entered; })) {
            std::cerr << "fake renderer did not enter the first Render call\n";
            video_state->release_first_render = true;
            video_state->cv.notify_all();
            lock.unlock();
            session.Stop();
            return false;
        }
    }

    // 工作线程被第一帧阻塞时连续提交 3 帧，容量为 2，因此 PTS=10 的最旧帧被丢弃。
    const bool submitted =
        session.SubmitFrame(MakeFrame(MediaType::VIDEO, 10)) &&
        session.SubmitFrame(MakeFrame(MediaType::VIDEO, 20)) &&
        session.SubmitFrame(MakeFrame(MediaType::VIDEO, 30));
    {
        std::lock_guard<std::mutex> lock(video_state->mutex);
        video_state->release_first_render = true;
        video_state->cv.notify_all();
    }

    const bool rendered = WaitUntil([&] {
        return session.GetStats().rendered_video_frames == 3;
    });
    session.Stop();
    session.Stop(); // 验证 Stop 可重复调用。

    const auto stats = session.GetStats();
    std::lock_guard<std::mutex> lock(video_state->mutex);
    const std::vector<std::int64_t> expected{0, 20, 30};
    const bool thread_affinity_ok =
        video_state->init_thread != std::this_thread::get_id() &&
        video_state->init_thread == video_state->shutdown_thread;
    return submitted && rendered &&
           video_state->rendered_pts == expected &&
           stats.submitted_video_frames == 4 &&
           stats.rendered_video_frames == 3 &&
           stats.dropped_video_frames == 1 &&
           stats.video_queue_size == 0 &&
           video_state->shutdown && thread_affinity_ok;
}

bool TestPauseResumeAndAudioDispatch() {
    auto video_state = std::make_shared<FakeVideoState>();
    auto audio_state = std::make_shared<FakeAudioState>();
    render::RenderSession session(
        std::make_unique<FakeVideoRenderer>(video_state),
        std::make_unique<FakeAudioRenderer>(audio_state));

    render::RenderSessionConfig config;
    config.enable_video = true;
    config.enable_audio = true;
    config.av_sync.enabled = false;
    if (!session.Init(config) || !session.Start()) {
        std::cerr << "pause test session start failed: " << session.LastError() << '\n';
        return false;
    }

    session.Pause();
    const bool submitted =
        session.SubmitFrame(MakeFrame(MediaType::AUDIO, 100)) &&
        session.SubmitFrame(MakeFrame(MediaType::VIDEO, 120));
    std::this_thread::sleep_for(30ms);
    const auto paused_stats = session.GetStats();
    if (!submitted ||
        paused_stats.rendered_audio_frames != 0 ||
        paused_stats.rendered_video_frames != 0) {
        session.Stop();
        return false;
    }

    session.Resume();
    const bool resumed = WaitUntil([&] {
        const auto stats = session.GetStats();
        return stats.rendered_audio_frames == 1 &&
               stats.rendered_video_frames == 1;
    });

    // fake window 请求关闭后，session 应自行退出，调用方只需观察 ShouldClose。
    video_state->should_close = true;
    const bool closed = WaitUntil([&] {
        return session.ShouldClose() && !session.IsRunning();
    });
    session.Stop();

    const auto stats = session.GetStats();
    std::lock_guard<std::mutex> audio_lock(audio_state->mutex);
    const bool audio_thread_affinity_ok =
        audio_state->init_thread != std::this_thread::get_id() &&
        audio_state->init_thread == audio_state->shutdown_thread;
    return resumed && closed &&
           stats.submitted_audio_frames == 1 &&
           stats.rendered_audio_frames == 1 &&
           stats.audio_underruns == 2 &&
           stats.audio_dropped_pcm_frames == 40 &&
           stats.audio_renderer_queue_size == 1 &&
           stats.audio_renderer_queued_frames == 80 &&
           stats.submitted_video_frames == 1 &&
           stats.rendered_video_frames == 1 &&
           audio_state->shutdown && audio_thread_affinity_ok;
}

bool TestAudioDispatchDoesNotWaitForSlowVideo() {
    auto video_state = std::make_shared<FakeVideoState>();
    auto audio_state = std::make_shared<FakeAudioState>();
    video_state->block_first_render = true;

    render::RenderSession session(
        std::make_unique<FakeVideoRenderer>(video_state),
        std::make_unique<FakeAudioRenderer>(audio_state));

    render::RenderSessionConfig config;
    config.enable_video = true;
    config.enable_audio = true;
    config.max_audio_queue_frames = 8;
    config.av_sync.enabled = false;
    if (!session.Init(config) || !session.Start()) {
        std::cerr << "audio/video decouple test session start failed: "
                  << session.LastError() << '\n';
        return false;
    }

    if (!session.SubmitFrame(MakeFrame(MediaType::VIDEO, 0))) {
        session.Stop();
        return false;
    }
    {
        std::unique_lock<std::mutex> lock(video_state->mutex);
        if (!video_state->cv.wait_for(
                lock, 1s, [&] { return video_state->first_render_entered; })) {
            std::cerr << "fake video renderer did not block on first Render call\n";
            video_state->release_first_render = true;
            video_state->cv.notify_all();
            lock.unlock();
            session.Stop();
            return false;
        }
    }

    // 这个测试复现真实摄像头问题的核心条件：视频渲染线程被首帧阻塞，
    // 但音频帧仍持续到达。修复前 session 单线程一次只取一帧音频和一帧视频，
    // 此处 rendered_audio_frames 会一直停在 0；修复后音频喂帧线程应能独立推进。
    for (int i = 0; i < 5; ++i) {
        if (!session.SubmitFrame(MakeFrame(MediaType::AUDIO, 1000 + i * 20'000))) {
            std::cerr << "audio/video decouple test failed to submit audio frame\n";
            {
                std::lock_guard<std::mutex> lock(video_state->mutex);
                video_state->release_first_render = true;
                video_state->cv.notify_all();
            }
            session.Stop();
            return false;
        }
    }

    const bool audio_rendered = WaitUntil([&] {
        return session.GetStats().rendered_audio_frames >= 5;
    });

    {
        std::lock_guard<std::mutex> lock(video_state->mutex);
        video_state->release_first_render = true;
        video_state->cv.notify_all();
    }

    const bool video_rendered = WaitUntil([&] {
        return session.GetStats().rendered_video_frames == 1;
    });
    session.Stop();

    const auto stats = session.GetStats();
    std::lock_guard<std::mutex> audio_lock(audio_state->mutex);
    return audio_rendered && video_rendered &&
           stats.submitted_audio_frames == 5 &&
           stats.rendered_audio_frames >= 5 &&
           stats.rendered_video_frames == 1 &&
           audio_state->rendered_pts.size() >= 5;
}

} // namespace

int main() {
    if (!TestVideoQueueDropsOldestFrame()) {
        std::cerr << "TestVideoQueueDropsOldestFrame failed\n";
        return 1;
    }
    if (!TestPauseResumeAndAudioDispatch()) {
        std::cerr << "TestPauseResumeAndAudioDispatch failed\n";
        return 1;
    }
    if (!TestAudioDispatchDoesNotWaitForSlowVideo()) {
        std::cerr << "TestAudioDispatchDoesNotWaitForSlowVideo failed\n";
        return 1;
    }

    std::cout << "RenderSession queue and lifecycle tests passed\n";
    return 0;
}
