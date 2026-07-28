#include "render/render_session.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "render/audio/wasapi_audio_renderer.h"
#include "render/opengl_video_renderer.h"
#include "render/playback_clock.h"

namespace render {
namespace {

constexpr auto kIdlePollInterval = std::chrono::milliseconds(5);

bool IsValidConfig(const RenderSessionConfig& config, std::string& error) {
    if (!config.enable_video && !config.enable_audio) {
        error = "RenderSession must enable at least one of video or audio output";
        return false;
    }
    if (config.enable_video && config.max_video_queue_frames == 0) {
        error = "max_video_queue_frames must be greater than 0";
        return false;
    }
    if (config.enable_audio && config.max_audio_queue_frames == 0) {
        error = "max_audio_queue_frames must be greater than 0";
        return false;
    }
    if (config.target_latency_ms < 0) {
        error = "target_latency_ms must be greater than or equal to 0";
        return false;
    }
    return true;
}

} // namespace

class RenderSession::Impl {
public:
    Impl(std::unique_ptr<IVideoRenderer> video_renderer,
         std::unique_ptr<audio::IAudioRenderer> audio_renderer)
        : video_renderer_(std::move(video_renderer)),
          audio_renderer_(std::move(audio_renderer)) {}

    ~Impl() {
        Stop();
    }

    bool Init(const RenderSessionConfig& config) {
        // 允许同一个对象重新初始化。先等待旧线程退出，保证 renderer 资源已经释放。
        Stop();

        std::string validation_error;
        if (!IsValidConfig(config, validation_error)) {
            std::lock_guard<std::mutex> lock(mutex_);
            initialized_ = false;
            last_error_ = std::move(validation_error);
            return false;
        }
        if (config.enable_video && !video_renderer_) {
            std::lock_guard<std::mutex> lock(mutex_);
            initialized_ = false;
            last_error_ = "RenderSession must enable video output and have an IVideoRenderer available";
            return false;
        }
        if (config.enable_audio && !audio_renderer_) {
            std::lock_guard<std::mutex> lock(mutex_);
            initialized_ = false;
            last_error_ = "RenderSession must enable audio output and have an IAudioRenderer available";
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        video_queue_.clear();
        audio_queue_.clear();
        stats_ = {};
        initialized_ = true;
        running_ = false;
        paused_ = false;
        stop_requested_ = false;
        close_requested_ = false;
        startup_done_ = false;
        startup_success_ = false;
        audio_available_ = false;
        last_error_.clear();
        clock_.Reset();
        return true;
    }

    bool Start() {
        std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!initialized_) {
                last_error_ = "RenderSession::Start 调用前必须先成功 Init";
                return false;
            }
            if (running_) {
                return true;
            }

            stop_requested_ = false;
            close_requested_ = false;
            startup_done_ = false;
            startup_success_ = false;
            paused_ = false;
            last_error_.clear();
        }

        // 窗口自行关闭后 worker 已经退出，但 std::thread 仍处于 joinable 状态。
        // 重新 Start 前先回收旧线程对象，避免给 joinable thread 重新赋值。
        if (worker_.joinable()) {
            worker_.join();
        }
        worker_ = std::thread(&Impl::RenderLoop, this);

        // Start 必须把设备初始化结果同步返回给调用者，避免后续 SubmitFrame 悄悄丢帧。
        std::unique_lock<std::mutex> lock(mutex_);
        startup_cv_.wait(lock, [this] { return startup_done_; });
        const bool success = startup_success_;
        lock.unlock();

        if (!success && worker_.joinable()) {
            worker_.join();
        }
        return success;
    }

    bool SubmitFrame(std::shared_ptr<MediaFrame> frame) {
        if (!frame) {
            SetError("RenderSession::SubmitFrame 收到空帧");
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || stop_requested_) {
            last_error_ = "RenderSession 未运行，无法提交媒体帧";
            return false;
        }

        if (frame->type == MediaType::VIDEO) {
            if (!config_.enable_video) {
                last_error_ = "当前 RenderSession 未启用视频输出";
                return false;
            }

            ++stats_.submitted_video_frames;
            if (video_queue_.size() >= config_.max_video_queue_frames) {
                if (!config_.drop_late_video_frames) {
                    ++stats_.dropped_video_frames;
                    return false;
                }
                video_queue_.pop_front();
                ++stats_.dropped_video_frames;
            }
            video_queue_.push_back(std::move(frame));
            stats_.video_queue_size = video_queue_.size();
        } else if (frame->type == MediaType::AUDIO) {
            if (!config_.enable_audio) {
                last_error_ = "当前 RenderSession 未启用音频输出";
                return false;
            }

            ++stats_.submitted_audio_frames;
            if (!audio_available_) {
                // fail_if_device_unavailable=false 表示音频设备缺失不应让视频 pipeline
                // 失败。此时接受调用并把帧记为丢弃，便于上层从统计中发现降级。
                ++stats_.dropped_audio_frames;
                return true;
            }
            if (audio_queue_.size() >= config_.max_audio_queue_frames) {
                audio_queue_.pop_front();
                ++stats_.dropped_audio_frames;
            }
            audio_queue_.push_back(std::move(frame));
            stats_.audio_queue_size = audio_queue_.size();
        } else {
            last_error_ = "RenderSession 只接受 AUDIO 或 VIDEO 类型的 MediaFrame";
            return false;
        }

        queue_cv_.notify_one();
        return true;
    }

    void Stop() {
        std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_requested_ = true;
            paused_ = false;
        }
        queue_cv_.notify_all();

        if (worker_.joinable()) {
            worker_.join();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        video_queue_.clear();
        audio_queue_.clear();
        stats_.video_queue_size = 0;
        stats_.audio_queue_size = 0;
        running_ = false;
        startup_done_ = true;
        audio_available_ = false;
        clock_.Reset(stats_.playback_pts_us);
    }

    void Pause() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || paused_) {
            return;
        }
        paused_ = true;
        clock_.Pause();
        queue_cv_.notify_all();
    }

    void Resume() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || !paused_) {
            return;
        }
        paused_ = false;
        clock_.Resume();
        queue_cv_.notify_all();
    }

    bool IsRunning() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    bool ShouldClose() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return close_requested_;
    }

    RenderStats GetStats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        RenderStats snapshot = stats_;
        snapshot.video_queue_size = video_queue_.size();
        snapshot.audio_queue_size = audio_queue_.size();
        if (audio_renderer_ && audio_available_) {
            const auto audio_stats = audio_renderer_->GetStats();
            snapshot.audio_underruns = audio_stats.underruns;
            snapshot.audio_dropped_pcm_frames = audio_stats.dropped_pcm_frames;
            snapshot.audio_renderer_queue_size = audio_stats.queued_pcm_chunks;
            snapshot.audio_renderer_queued_frames = audio_stats.queued_pcm_frames;
        }
        snapshot.playback_pts_us = clock_.PositionUs();
        return snapshot;
    }

    std::string LastError() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_;
    }

private:
    void RenderLoop() {
        bool video_initialized = false;
        bool audio_initialized = false;
        bool video_init_attempted = false;
        bool audio_init_attempted = false;
        std::string startup_error;

        if (config_.enable_video) {
            video_init_attempted = true;
            video_initialized = video_renderer_->Init(config_.video);
            if (!video_initialized) {
                startup_error = "视频 renderer 初始化失败";
            }
        }

        if (startup_error.empty() && config_.enable_audio) {
            audio_init_attempted = true;
            audio_initialized = audio_renderer_->Init(config_.audio);
            if (!audio_initialized && config_.audio.fail_if_device_unavailable) {
                startup_error = "音频 renderer 初始化失败";
            }
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            audio_available_ = audio_initialized;
            startup_success_ = startup_error.empty();
            startup_done_ = true;
            running_ = startup_success_;
            if (!startup_success_) {
                last_error_ = std::move(startup_error);
                stop_requested_ = true;
            }
            if (startup_success_) {
                clock_.Start();
            }
        }
        startup_cv_.notify_all();

        while (true) {
            std::shared_ptr<MediaFrame> audio_frame;
            std::shared_ptr<MediaFrame> video_frame;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                queue_cv_.wait_for(lock, kIdlePollInterval, [this] {
                    return stop_requested_ ||
                           (!paused_ && (!audio_queue_.empty() || !video_queue_.empty()));
                });

                if (stop_requested_) {
                    break;
                }

                // 暂停时不消费队列，但循环仍会继续 PollEvents，窗口不会失去响应。
                if (!paused_) {
                    if (audio_available_ && !audio_queue_.empty()) {
                        audio_frame = std::move(audio_queue_.front());
                        audio_queue_.pop_front();
                        stats_.audio_queue_size = audio_queue_.size();
                    }
                    if (config_.enable_video && !video_queue_.empty()) {
                        video_frame = std::move(video_queue_.front());
                        video_queue_.pop_front();
                        stats_.video_queue_size = video_queue_.size();
                    }
                }
            }

            // 第一版先在同一个 session 线程串行驱动音频和视频。WASAPI 的异步
            // ring buffer 会在阶段 3 拆出独立消费线程。
            if (audio_frame && !audio_renderer_->Render(*audio_frame)) {
                FailAndRequestStop("音频帧渲染失败");
            } else if (audio_frame) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.rendered_audio_frames;
                const auto audio_stats = audio_renderer_->GetStats();
                stats_.audio_underruns = audio_stats.underruns;
                stats_.audio_dropped_pcm_frames = audio_stats.dropped_pcm_frames;
                stats_.audio_renderer_queue_size = audio_stats.queued_pcm_chunks;
                stats_.audio_renderer_queued_frames = audio_stats.queued_pcm_frames;
                stats_.playback_pts_us = audio_renderer_->PlayedPtsUs();
                clock_.SetPositionUs(stats_.playback_pts_us);
            }

            if (video_frame && !video_renderer_->Render(*video_frame)) {
                FailAndRequestStop("视频帧渲染失败");
            } else if (video_frame) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.rendered_video_frames;
                if (!audio_available_) {
                    stats_.playback_pts_us = video_frame->time.pts_us;
                    clock_.SetPositionUs(stats_.playback_pts_us);
                }
            }

            if (video_initialized) {
                video_renderer_->PollEvents();
                if (video_renderer_->ShouldClose()) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    close_requested_ = true;
                    stop_requested_ = true;
                    queue_cv_.notify_all();
                }
            }
        }

        // renderer 的 Init/Render/PollEvents/Shutdown 全部位于同一线程，满足
        // GLFW/OpenGL context 和 COM/WASAPI 资源的线程归属要求。
        if (audio_init_attempted) {
            audio_renderer_->Shutdown();
        }
        if (video_init_attempted) {
            video_renderer_->Shutdown();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        audio_available_ = false;
    }

    void FailAndRequestStop(std::string message) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = std::move(message);
        stop_requested_ = true;
        queue_cv_.notify_all();
    }

    void SetError(std::string message) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = std::move(message);
    }

    mutable std::mutex mutex_;  ///< 保护配置状态的互斥锁。
    std::mutex lifecycle_mutex_;
    std::condition_variable queue_cv_;
    std::condition_variable startup_cv_;
    std::thread worker_;

    RenderSessionConfig config_;
    RenderStats stats_;
    PlaybackClock clock_;
    std::deque<std::shared_ptr<MediaFrame>> video_queue_;
    std::deque<std::shared_ptr<MediaFrame>> audio_queue_;
    std::unique_ptr<IVideoRenderer> video_renderer_;
    std::unique_ptr<audio::IAudioRenderer> audio_renderer_;

    bool initialized_{false};  ///< 是否初始化完成。
    bool running_{false};  ///< 是否正在运行。
    bool paused_{false};  ///< 是否暂停暂播放。
    bool stop_requested_{false};
    bool close_requested_{false};
    bool startup_done_{false};
    bool startup_success_{false};
    bool audio_available_{false};
    std::string last_error_;
};

RenderSession::RenderSession()
    : RenderSession(std::make_unique<OpenGLVideoRenderer>(),
                    std::make_unique<audio::WasapiAudioRenderer>()) {}

RenderSession::RenderSession(std::unique_ptr<IVideoRenderer> video_renderer,
                             std::unique_ptr<audio::IAudioRenderer> audio_renderer)
    : impl_(std::make_unique<Impl>(std::move(video_renderer),
                                  std::move(audio_renderer))) {}

RenderSession::~RenderSession() = default;

bool RenderSession::Init(const RenderSessionConfig& config) {
    return impl_->Init(config);
}

bool RenderSession::Start() {
    return impl_->Start();
}

bool RenderSession::SubmitFrame(std::shared_ptr<MediaFrame> frame) {
    return impl_->SubmitFrame(std::move(frame));
}

void RenderSession::Stop() {
    impl_->Stop();
}

void RenderSession::Pause() {
    impl_->Pause();
}

void RenderSession::Resume() {
    impl_->Resume();
}

bool RenderSession::IsRunning() const {
    return impl_->IsRunning();
}

bool RenderSession::ShouldClose() const {
    return impl_->ShouldClose();
}

RenderStats RenderSession::GetStats() const {
    return impl_->GetStats();
}

std::string RenderSession::LastError() const {
    return impl_->LastError();
}

} // namespace render
