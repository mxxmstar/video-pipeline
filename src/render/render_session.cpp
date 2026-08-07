#include "render/render_session.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <algorithm>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "render/audio/wasapi_audio_renderer.h"
#include "mediaflow/media_timing.h"
#include "render/opengl_video_renderer.h"
#include "render/video_pts_normalizer.h"

namespace render {
namespace {

constexpr auto kIdlePollInterval = std::chrono::milliseconds(5);

std::int64_t MillisecondsToUs(int value_ms) {
    return static_cast<std::int64_t>(std::max(0, value_ms)) * 1000;
}

mediaflow::VideoScheduleConfig ToMediaFlowScheduleConfig(
    const AvSyncConfig& config) {
    mediaflow::VideoScheduleConfig result;
    result.enabled = config.enabled;
    result.late_threshold_us = MillisecondsToUs(config.late_threshold_ms);
    result.early_threshold_us = MillisecondsToUs(config.early_threshold_ms);
    result.max_wait_us = MillisecondsToUs(config.max_wait_ms);
    result.drop_late_video_frames = config.drop_late_video_frames;
    return result;
}

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
        video_startup_done_ = false;
        audio_startup_done_ = false;
        video_startup_success_ = false;
        audio_startup_success_ = false;
        video_available_ = false;
        audio_available_ = false;
        last_error_.clear();
        mediaflow_clock_.Reset();
        video_pts_scheduler_.SetConfig(
            ToMediaFlowScheduleConfig(config_.av_sync));
        video_pts_normalizer_.SetConfig(config_.video_pts);
        system_clock_anchored_ = false;
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
            video_startup_done_ = !config_.enable_video;
            audio_startup_done_ = !config_.enable_audio;
            video_startup_success_ = !config_.enable_video;
            audio_startup_success_ = !config_.enable_audio;
            video_available_ = false;
            audio_available_ = false;
            paused_ = false;
            last_error_.clear();
        }

        // 窗口自行关闭后工作线程已经退出，但 std::thread 仍处于 joinable 状态。
        // 重新 Start 前先回收旧线程对象，避免给 joinable thread 重新赋值。
        if (video_worker_.joinable()) {
            video_worker_.join();
        }
        if (audio_worker_.joinable()) {
            audio_worker_.join();
        }

        try {
            // OpenGL/GLFW 要求 context 在创建它的线程中使用和释放，所以视频
            // renderer 独占 video_worker_。摄像头 1080p Debug 路径里 CPU RGBA
            // 转换和纹理上传可能很慢，不能再让它顺手驱动音频。
            if (config_.enable_video) {
                video_worker_ = std::thread(&Impl::VideoRenderLoop, this);
            }
            // audio_worker_ 只负责把解码后的音频帧重采样并提交给 IAudioRenderer。
            // 对 WASAPI 后端来说，真正写声卡的是 WasapiAudioRenderer 内部的
            // event-driven 线程；这里的独立线程用于保证“喂 PCM”不被视频渲染拖慢。
            if (config_.enable_audio) {
                audio_worker_ = std::thread(&Impl::AudioRenderLoop, this);
            }
        } catch (const std::system_error& error) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_error_ = std::string("failed to start RenderSession worker: ") +
                              error.what();
                stop_requested_ = true;
                startup_done_ = true;
                startup_success_ = false;
            }
            queue_cv_.notify_all();
            startup_cv_.notify_all();
            JoinWorkers();
            return false;
        }

        // Start 必须等视频/音频两个 worker 都完成初始化判定，再把结果同步返回给调用者。
        // 其中音频设备可按配置降级为非致命错误，但视频初始化失败一定不能继续运行。
        std::unique_lock<std::mutex> lock(mutex_);
        startup_cv_.wait(lock, [this] { return startup_done_; });
        const bool success = startup_success_;
        lock.unlock();

        if (!success) {
            {
                std::lock_guard<std::mutex> stop_lock(mutex_);
                stop_requested_ = true;
            }
            queue_cv_.notify_all();
            JoinWorkers();
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

        // 同一个 condition_variable 同时服务音频和视频 worker。这里唤醒全部线程，
        // 由各自的等待谓词判断是否真的有活要做，避免提交音频帧时只唤醒视频线程。
        queue_cv_.notify_all();
        return true;
    }

    void Stop() {
        std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Stop 会同时唤醒视频和音频 worker。两个线程看到 stop_requested_
            // 后各自在自己的线程中 Shutdown renderer，保持设备资源的线程归属清晰。
            stop_requested_ = true;
            paused_ = false;
        }
        queue_cv_.notify_all();

        JoinWorkers();

        std::lock_guard<std::mutex> lock(mutex_);
        video_queue_.clear();
        audio_queue_.clear();
        stats_.video_queue_size = 0;
        stats_.audio_queue_size = 0;
        running_ = false;
        startup_done_ = true;
        video_available_ = false;
        audio_available_ = false;
        mediaflow_clock_.Reset(1, stats_.playback_pts_us);
        video_pts_normalizer_.Reset();
        system_clock_anchored_ = false;
    }

    void Pause() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || paused_) {
            return;
        }
        paused_ = true;
        mediaflow_clock_.Pause();
        queue_cv_.notify_all();
    }

    void Resume() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || !paused_) {
            return;
        }
        paused_ = false;
        mediaflow_clock_.Resume();
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
        const auto clock_snapshot = mediaflow_clock_.Snapshot(audio_available_);
        snapshot.playback_pts_us = clock_snapshot.position_us;
        snapshot.playback_clock_source =
            clock_snapshot.source == mediaflow::ClockSource::Audio ? 1 : 0;
        return snapshot;
    }

    std::string LastError() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_error_;
    }

private:
    void VideoRenderLoop() {
        // 视频 renderer 的完整生命周期固定在 video_worker_ 中：
        // Init 创建窗口和 OpenGL context，Render/PollEvents 使用 context，
        // Shutdown 再在同一线程释放它。
        const bool initialized = video_renderer_->Init(config_.video);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            video_available_ = initialized;
            video_startup_success_ = initialized;
            video_startup_done_ = true;
            if (!initialized) {
                last_error_ = "视频 renderer 初始化失败";
            }
            CompleteStartupLocked();
        }
        startup_cv_.notify_all();
        queue_cv_.notify_all();

        if (!initialized) {
            video_renderer_->Shutdown();
            return;
        }

        while (true) {
            std::shared_ptr<MediaFrame> video_frame;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                // 视频线程只等待视频队列。音频队列不参与这个谓词，避免视频帧
                // 转换/上传变慢时把音频消费也锁在同一个循环节奏里。
                queue_cv_.wait_for(lock, kIdlePollInterval, [this] {
                    return stop_requested_ ||
                           (!paused_ && !video_queue_.empty());
                });

                if (stop_requested_) {
                    break;
                }

                // 暂停时不消费队列，但循环仍会继续 PollEvents，窗口不会失去响应。
                if (!paused_) {
                    if (!video_queue_.empty()) {
                        video_frame = std::move(video_queue_.front());
                        video_queue_.pop_front();
                        stats_.video_queue_size = video_queue_.size();
                    }
                }
            }

            if (video_frame) {
                const auto sync_action = PrepareVideoFrameForRender(video_frame);
                if (sync_action == VideoFrameSyncAction::Stop) {
                    break;
                }
                if (sync_action == VideoFrameSyncAction::Requeued ||
                    sync_action == VideoFrameSyncAction::Drop) {
                    video_frame.reset();
                } else if (!video_renderer_->Render(*video_frame)) {
                    FailAndRequestStop("视频帧渲染失败");
                } else {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.rendered_video_frames;
                    const auto clock_snapshot =
                        mediaflow_clock_.Snapshot(audio_available_);
                    stats_.playback_pts_us = clock_snapshot.position_us;
                    stats_.playback_clock_source =
                        clock_snapshot.source == mediaflow::ClockSource::Audio ? 1 : 0;
                }
            }

            video_renderer_->PollEvents();
            if (video_renderer_->ShouldClose()) {
                std::lock_guard<std::mutex> lock(mutex_);
                close_requested_ = true;
                stop_requested_ = true;
                running_ = false;
                queue_cv_.notify_all();
            }
        }

        // OpenGL renderer 的 Init/Render/PollEvents/Shutdown 全部位于同一线程，
        // 满足 GLFW/OpenGL context 的线程归属要求。
        video_renderer_->Shutdown();

        std::lock_guard<std::mutex> lock(mutex_);
        video_available_ = false;
    }

    void AudioRenderLoop() {
        // 音频 renderer 的 Init/Render/Shutdown 固定在 audio_worker_ 中。
        // WASAPI 后端内部还会启动一个设备写入线程，但外层 session 不直接碰声卡 buffer。
        const bool initialized = audio_renderer_->Init(config_.audio);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            audio_available_ = initialized;
            audio_startup_success_ = initialized;
            audio_startup_done_ = true;
            if (!initialized &&
                (config_.audio.fail_if_device_unavailable || !config_.enable_video)) {
                last_error_ = "音频 renderer 初始化失败";
            }
            CompleteStartupLocked();
        }
        startup_cv_.notify_all();
        queue_cv_.notify_all();

        if (!initialized) {
            audio_renderer_->Shutdown();
            return;
        }

        while (true) {
            std::shared_ptr<MediaFrame> audio_frame;

            {
                std::unique_lock<std::mutex> lock(mutex_);
                // 音频线程只等待音频队列，因此摄像头音频约 50fps 到达时可以持续
                // 喂入 WASAPI PCM 队列，不再受视频渲染帧率影响。
                queue_cv_.wait_for(lock, kIdlePollInterval, [this] {
                    return stop_requested_ ||
                           (!paused_ && !audio_queue_.empty());
                });

                if (stop_requested_) {
                    break;
                }

                if (!paused_ && !audio_queue_.empty()) {
                    audio_frame = std::move(audio_queue_.front());
                    audio_queue_.pop_front();
                    stats_.audio_queue_size = audio_queue_.size();
                }
            }

            // 音频喂帧与视频渲染解耦：视频 CPU 转 RGBA 或 OpenGL 上传变慢时，
            // 音频仍能按输入节奏重采样并送入 WASAPI 内部 PCM 队列。
            if (audio_frame && !audio_renderer_->Render(*audio_frame)) {
                FailAndRequestStop("音频帧渲染失败");
            } else if (audio_frame) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.rendered_audio_frames;
                UpdateAudioStatsLocked();
                stats_.playback_pts_us = audio_renderer_->PlayedPtsUs();
                // 音频 renderer 上报的是已经播放到的媒体 PTS。统一时钟以此
                // 作为 master；0 是合法起始位置，不能按“未初始化”处理。
                mediaflow_clock_.UpdateAudioPosition(1, stats_.playback_pts_us);
                const auto clock_snapshot =
                    mediaflow_clock_.Snapshot(audio_available_);
                stats_.playback_pts_us = clock_snapshot.position_us;
                stats_.playback_clock_source =
                    clock_snapshot.source == mediaflow::ClockSource::Audio ? 1 : 0;
            }
        }

        audio_renderer_->Shutdown();

        std::lock_guard<std::mutex> lock(mutex_);
        audio_available_ = false;
    }

    void CompleteStartupLocked() {
        // 两个 worker 并行启动。只有当已启用的 worker 都给出初始化结果后，
        // Start() 才能返回；这样调用方不会在半初始化状态下提交帧。
        if (startup_done_ || !video_startup_done_ || !audio_startup_done_) {
            return;
        }

        const bool video_ok = !config_.enable_video || video_startup_success_;
        // 预览场景中音频设备可能缺失。只要同时启用了视频并且配置允许降级，
        // 音频初始化失败不会阻止视频预览；后续提交的音频帧会计入 dropped_audio_frames。
        const bool audio_optional =
            config_.enable_video && config_.enable_audio &&
            !config_.audio.fail_if_device_unavailable;
        const bool audio_ok =
            !config_.enable_audio || audio_startup_success_ || audio_optional;

        startup_success_ = video_ok && audio_ok;
        startup_done_ = true;
        running_ = startup_success_;
        if (startup_success_) {
            // RenderSession 内部没有网络重连边界，固定使用本地代次 1；外层
            // MediaFlow 消息的 generation 仍由 Source/Decoder 独立维护。
            mediaflow_clock_.Start(1);
        } else {
            stop_requested_ = true;
        }
    }

    enum class VideoFrameSyncAction {
        /// 当前视频帧可以立即交给 IVideoRenderer。
        Render,
        /// 当前视频帧已被同步策略丢弃。
        Drop,
        /// 当前视频帧因 Pause 等状态变化被放回队列头部。
        Requeued,
        /// 会话正在停止，视频线程应退出。
        Stop,
    };

    VideoFrameSyncAction PrepareVideoFrameForRender(
        std::shared_ptr<MediaFrame>& video_frame) {
        if (!video_frame) {
            return VideoFrameSyncAction::Drop;
        }

        // AV sync 只应该看到稳定的媒体时间轴。这里先把明显异常的输入 PTS
        // 转换为连续 PTS，并把修正后的值写回 frame，保证后续统计/调试看到的是
        // 实际参与调度的时间戳。
        const auto video_pts_us = NormalizeVideoPts(*video_frame);

        // 无音频 master 时，第一帧视频用自己的 PTS 锚定 fallback 系统时钟。
        // 否则如果输入流首帧 PTS 不是 0，第一帧会被误判为“大幅早到”。
        AnchorSystemClockForFirstVideoFrame(video_pts_us);

        while (video_frame) {
            // 每次等待后都重新读取 clock，因为 audio master 可能已经继续前进。
            // 对早到帧来说，这个循环会把“短等待”拆成多次小步，保持 Stop/Pause 响应。
            const auto clock_snapshot = CurrentClockSnapshot();
            const auto decision = video_pts_scheduler_.Decide(
                video_pts_us, false, 1, clock_snapshot);

            if (decision.action == mediaflow::VideoScheduleAction::Render) {
                return VideoFrameSyncAction::Render;
            }

            if (decision.action == mediaflow::VideoScheduleAction::Drop) {
                // 这里的丢帧是 AV sync 主动丢晚帧；SubmitFrame() 中队列满丢帧不会
                // 进入这个分支。两个计数拆开后，camera 日志能区分“渲染慢导致队列满”
                // 和“同步策略认为帧已经过期”。
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.dropped_video_frames;
                ++stats_.av_sync_dropped_video_frames;
                stats_.playback_pts_us = clock_snapshot.position_us;
                stats_.playback_clock_source =
                    clock_snapshot.source == mediaflow::ClockSource::Audio ? 1 : 0;
                video_frame.reset();
                return VideoFrameSyncAction::Drop;
            }

            {
                // 只要 AV sync 要求 Wait，就先记录一次等待意图。即使等待过程中
                // 被 Pause/Stop 提前唤醒，该统计也能反映同步策略施加过节奏控制。
                std::lock_guard<std::mutex> lock(mutex_);
                ++stats_.av_sync_video_waits;
                stats_.av_sync_video_wait_us += decision.wait_us;
            }
            if (!WaitForVideoSync(
                    std::chrono::microseconds(decision.wait_us))) {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stop_requested_) {
                    return VideoFrameSyncAction::Stop;
                }
                if (paused_) {
                    // Pause 发生在帧已经出队之后。为了不丢掉这帧，把它放回队列头部；
                    // Resume 后它会重新走 PTS normalizer/AV sync。当前 normalizer
                    // 已经消费过这帧 PTS，后续阶段若要严格避免重复归一化，可把
                    // normalized PTS 缓存在 frame 扩展字段中。
                    video_queue_.push_front(std::move(video_frame));
                    stats_.video_queue_size = video_queue_.size();
                    return VideoFrameSyncAction::Requeued;
                }
            }
        }

        return VideoFrameSyncAction::Drop;
    }

    std::int64_t NormalizeVideoPts(MediaFrame& video_frame) {
        // duration_us 对异常 PTS 的兜底生成很重要：有 duration 就按真实帧间隔推进；
        // 没有 duration 才使用配置里的 fallback_fps。
        const auto normalized =
            video_pts_normalizer_.Normalize(video_frame.time.pts_us,
                                            video_frame.time.duration_us);
        if (normalized.normalized) {
            // normalizer 只在 video worker 使用，不需要自己加锁；但统计属于 session
            // 共享状态，必须通过 mutex_ 更新。
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.normalized_video_pts_frames;
        }

        // 写回 frame，保证后续 Render()、日志和调试器里看到的是参与同步的 PTS。
        video_frame.time.pts_us = normalized.pts_us;
        return normalized.pts_us;
    }

    void AnchorSystemClockForFirstVideoFrame(std::int64_t video_pts_us) {
        std::lock_guard<std::mutex> lock(mutex_);
        // 只有真正拿到音频播放位置后，audio master 才算 ready。音频设备初始化成功但
        // 尚未播放出第一批 PCM 时，仍然需要用视频首帧锚定 fallback clock。
        const bool audio_master_ready =
            audio_available_ && mediaflow_clock_.HasAudioPosition();
        if (audio_master_ready || system_clock_anchored_) {
            return;
        }

        mediaflow_clock_.SetSystemPositionUs(video_pts_us);
        stats_.playback_pts_us = video_pts_us;
        system_clock_anchored_ = true;
    }

    mediaflow::ClockSnapshot CurrentClockSnapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        // audio_available_ 只表示音频 renderer 初始化成功；MediaClock 内部还会判断
        // 是否已经有有效音频播放位置，没有时自动回退到 system clock。
        return mediaflow_clock_.Snapshot(audio_available_);
    }

    bool WaitForVideoSync(std::chrono::microseconds duration) {
        if (duration.count() <= 0) {
            return true;
        }

        // 不用 sleep_for，而是挂在 queue_cv_ 上等待，这样 Stop/Pause 可以立刻唤醒
        // 视频线程，不必等满本次 AV sync wait。
        std::unique_lock<std::mutex> lock(mutex_);
        return !queue_cv_.wait_for(lock, duration, [this] {
            return stop_requested_ || paused_;
        });
    }

    void UpdateAudioStatsLocked() {
        // WASAPI 的内部队列和 underrun 统计来自 audio renderer。这里在 session
        // 统计里做一份快照，便于手动摄像头测试直接看到音频是否还在挨饿。
        if (!audio_renderer_ || !audio_available_) {
            return;
        }

        const auto audio_stats = audio_renderer_->GetStats();
        stats_.audio_underruns = audio_stats.underruns;
        stats_.audio_dropped_pcm_frames = audio_stats.dropped_pcm_frames;
        stats_.audio_renderer_queue_size = audio_stats.queued_pcm_chunks;
        stats_.audio_renderer_queued_frames = audio_stats.queued_pcm_frames;
    }

    void JoinWorkers() {
        if (video_worker_.joinable()) {
            video_worker_.join();
        }
        if (audio_worker_.joinable()) {
            audio_worker_.join();
        }
    }

    void FailAndRequestStop(std::string message) {
        std::lock_guard<std::mutex> lock(mutex_);
        last_error_ = std::move(message);
        stop_requested_ = true;
        running_ = false;
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
    std::thread video_worker_;
    std::thread audio_worker_;

    RenderSessionConfig config_;
    RenderStats stats_;
    mediaflow::UnifiedClock mediaflow_clock_;
    mediaflow::VideoPtsScheduler video_pts_scheduler_;
    VideoPtsNormalizer video_pts_normalizer_;
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
    bool video_startup_done_{false};
    bool audio_startup_done_{false};
    bool video_startup_success_{false};
    bool audio_startup_success_{false};
    bool video_available_{false};
    bool audio_available_{false};
    bool system_clock_anchored_{false};
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
