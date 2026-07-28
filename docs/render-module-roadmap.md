# Render 模块后续计划

本文档整理 `render` 模块后续演进计划，目标是把当前第一版 OpenGL/WASAPI 本地预览能力，逐步收敛成可在工程内稳定复用的音视频渲染子系统。

## 当前基线

当前工程已经具备以下能力：

1. 视频渲染第一版
   - `include/render/i_video_renderer.h` 定义视频渲染接口。
   - `include/render/opengl_video_renderer.h` 与 `src/render/opengl_video_renderer.cpp` 实现基于 GLFW + GLAD + OpenGL 的窗口渲染。
   - `include/render/frame_converter.h` 与 `src/render/frame_converter.cpp` 将 `MediaFrame` 转为 RGBA8 CPU 缓冲，再上传到 OpenGL 纹理。
   - `test/render/test_frame_converter.cpp` 覆盖 RGB/BGR/GRAY/NV12/NV21/I420 等格式转换。
   - `test/render/test_opengl_video_renderer.cpp` 提供隐藏窗口 smoke 测试和手动摄像头 render 测试。

2. 音频处理第一版
   - `include/filter/audio/audio_resample_filter.h` 与 `src/filter/audio/audio_resample_filter.cpp` 使用 FFmpeg `swresample` 完成采样率、采样格式、声道布局转换。
   - `include/filter/audio/pcm_frame.h` 定义 render/audio 消费的 interleaved PCM 数据结构。
   - `test/filter/test_audio_resample_filter.cpp` 覆盖 packed/planar、重采样和异常输入。

3. 音频渲染第一版
   - `include/render/audio/i_audio_renderer.h` 定义音频渲染接口。
   - `include/render/audio/wasapi_audio_renderer.h` 与 `src/render/audio/wasapi_audio_renderer.cpp` 实现 Windows WASAPI shared-mode 同步写入播放。
   - `test/render/test_opengl_video_renderer.cpp` 的 `TestCameraAudioVideoDecodeEncodeRender()` 已把解码后的音频帧送入 WASAPI，同时保留 AAC encode 覆盖。

4. 构建开关
   - `VIDEO_PIPELINE_BUILD_RENDER` 控制 render 模块是否参与构建。
   - OpenGL 使用 Windows 自带 `OpenGL32`，GLFW/GLAD 由 vcpkg 提供。
   - WASAPI 使用 Windows SDK 与系统库，不需要从 vcpkg 安装额外音频库。

## 总体目标

Render 模块最终应承担以下职责：

1. 本地预览
   - 支持摄像头 RTSP、文件、内存帧等输入的本地音视频预览。
   - 支持视频窗口关闭、暂停、恢复、低延迟播放、统计信息输出。

2. 音视频同步
   - 使用统一播放时钟协调音频和视频。
   - 默认以音频时钟为 master；无音频时可使用系统时钟或视频帧 PTS 驱动。
   - 支持帧丢弃和缓冲深度控制，避免预览越播越晚。

3. 工程复用
   - 让 render 能被测试程序、native player、后续 Tauri/C API 复用。
   - render 层不直接依赖 puller/decoder/encoder/publisher 的具体实现，只消费 `MediaFrame`。

4. 可诊断
   - 提供渲染帧数、丢帧数、音频 underrun、队列长度、当前播放 PTS、平均延迟等统计。
   - 关键错误保留 `LastError()`，并通过 spdlog 输出上下文。

5. 可扩展
   - 视频后续可从 CPU RGBA 转换演进到 GPU YUV shader 转换。
   - 音频后续可从同步写入演进到独立音频线程 + ring buffer。
   - Windows 优先稳定，之后再考虑 Linux/macOS 后端。

## 分层边界

推荐保持以下边界：

```text
media/puller     负责读包
media/decoder    负责解码为 MediaFrame
filter           负责格式转换、重采样、颜色空间转换等纯处理能力
render           负责设备、窗口、播放时钟、队列、同步和输出
media/encoder    负责编码
media/publisher  负责发布
```

关键原则：

1. `filter` 不持有窗口、声卡、线程主循环等运行时资源。
2. `render` 可以使用 `filter`，但 `filter` 不反向依赖 `render`。
3. `render` 不应该直接拉流、解码或发布；这些由外层 pipeline 编排。
4. 测试中的 `DecodeEncodeRender` 可以保留 encode 覆盖，但正式 render API 不应依赖 encoder。

## 目标架构

建议后续形成以下组件：

```text
IRenderSession
  - Init(RenderSessionConfig)
  - SubmitFrame(MediaFrame)
  - Start()
  - Stop()
  - Pause()
  - Resume()
  - GetStats()

RenderSession
  - AudioFrameQueue
  - VideoFrameQueue
  - PlaybackClock
  - OpenGLVideoRenderer
  - WasapiAudioRenderer
  - AVSyncController

filter/audio
  - AudioResampleFilter

filter/video
  - VideoScaleFilter 或 PixelConvertFilter
  - 后续可迁移 CPU swscale 转换
```

外层调用关系：

```text
FFmpegPuller
  -> FFmpegDecoder
     -> RenderSession::SubmitFrame(decoded MediaFrame)
```

## 阶段计划

### 阶段 2：RenderSession 与线程模型

> 实施状态：已完成（2026-07-28）。已落地统一 session API、播放时钟、
> 音视频有界队列、线程内 renderer 生命周期、统计快照和无设备单元测试。

目标：

- 新增统一的音视频渲染会话，避免在 decoder callback 中直接调用 OpenGL/WASAPI。
- 建立 render 线程模型，保证 OpenGL context 在同一线程创建和使用。
- 建立音频、视频 frame queue，为后续 AV sync 做准备。

建议新增文件：

- `include/render/render_session_config.h`
- `include/render/render_stats.h`
- `include/render/i_render_session.h`
- `include/render/render_session.h`
- `src/render/render_session.cpp`
- `src/render/playback_clock.cpp`
- `include/render/playback_clock.h`

接口草案：

```cpp
namespace render {

struct RenderSessionConfig {
    RenderConfig video;
    audio::AudioRenderConfig audio;
    bool enable_video{true};
    bool enable_audio{true};
    int max_video_queue_frames{6};
    int max_audio_queue_frames{50};
    int target_latency_ms{120};
    bool drop_late_video_frames{true};
};

struct RenderStats {
    int64_t submitted_video_frames{0};
    int64_t submitted_audio_frames{0};
    int64_t rendered_video_frames{0};
    int64_t rendered_audio_frames{0};
    int64_t dropped_video_frames{0};
    int64_t audio_underruns{0};
    int64_t playback_pts_us{0};
    int video_queue_size{0};
    int audio_queue_size{0};
};

class IRenderSession {
public:
    virtual ~IRenderSession() = default;
    virtual bool Init(const RenderSessionConfig& config) = 0;
    virtual bool Start() = 0;
    virtual bool SubmitFrame(std::shared_ptr<MediaFrame> frame) = 0;
    virtual void Stop() = 0;
    virtual void Pause() = 0;
    virtual void Resume() = 0;
    virtual RenderStats GetStats() const = 0;
};

} // namespace render
```

实现要点：

1. OpenGL renderer 只在 render 线程中创建、Render、PollEvents、Shutdown。
2. `SubmitFrame()` 只负责入队，不做阻塞渲染。
3. 视频队列满时优先丢旧帧，保证低延迟预览。
4. 音频队列满时可短暂阻塞或丢旧数据，第一版建议丢旧帧并统计。
5. `Stop()` 必须可重复调用，并保证线程退出后释放窗口和声卡。

验收标准：

- `test_opengl_video_renderer --smoke` 迁移为 `RenderSession` 路径。
- 摄像头手动测试不再在 decoder callback 中直接操作 OpenGL context。
- 关闭窗口后拉流循环能正常退出。
- render 关闭构建仍能通过 filter 测试。

实际实现：

1. 新增 `RenderSessionConfig`、`RenderStats`、`IRenderSession`、
   `RenderSession` 和 `PlaybackClock`。
2. `Init()` 只校验并保存配置；`Start()` 创建工作线程，并在线程内完成
   OpenGL/WASAPI 初始化。启动结果会同步返回调用方。
3. `SubmitFrame()` 只执行线程安全入队。视频队列满时默认丢弃最旧帧；
   音频队列在第一版中采用相同策略，并分别记录丢帧统计。
4. `Pause()` 停止消费队列但继续处理窗口事件；`Resume()` 恢复消费；
   `Stop()` 支持重复调用，并等待 renderer 在线程内释放。
5. `test_render_session` 使用 fake video/audio renderer 验证队列溢出、
   pause/resume、窗口关闭、重复 Stop，以及 Init/Render/Shutdown 的线程归属。
6. `test_opengl_video_renderer` 的 `--smoke`、`--loop` 和 `--camera`
   均改为通过 `RenderSession` 驱动；decoder callback 不再直接调用
   OpenGL 或 WASAPI。
7. 阶段 2 暂不实现按 PTS 调度和音频主时钟同步；当前 `PlaybackClock`
   提供线程安全基线，正式 AV sync 仍属于阶段 4。

### 阶段 3：音频播放异步化

目标：

- 将 `WasapiAudioRenderer::Render()` 从同步写入改为入队/写 ring buffer，减少 decoder callback 或 render session 的阻塞。
- 增加音频播放线程，由 WASAPI event callback 驱动填充声卡 buffer。
- 暴露稳定的音频播放时钟。

建议新增或调整：

- `include/render/audio/audio_render_stats.h`
- `src/render/audio/audio_ring_buffer.cpp`
- `include/render/audio/audio_ring_buffer.h`
- `WasapiAudioRenderer::Render()` 改为快速复制 PCM 到 ring buffer。

实现要点：

1. 初始化时获取 WASAPI mix format，并打开 `AudioResampleFilter`。
2. `Render(MediaFrame)` 中完成重采样，然后写入 ring buffer。
3. 音频线程等待 WASAPI event，按可写 frame 数从 ring buffer 取数据。
4. ring buffer 数据不足时写静音并增加 underrun 计数。
5. ring buffer 数据过多时按低延迟策略丢弃旧数据。
6. `PlayedPtsUs()` 应基于提交帧数、当前 padding、首帧 PTS 推算。

验收标准：

- 手动摄像头 render 时音频不因 `Render()` 阻塞导致视频卡顿。
- 声卡不可用时错误清晰，不影响 `--smoke` 自动测试。
- 能输出 audio queued frames、underrun、played pts。

### 阶段 4：音视频同步

目标：

- 让视频根据音频时钟显示，解决音视频长期漂移。
- 在低延迟预览场景下，宁可丢晚到视频帧，也不持续增加延迟。

推荐策略：

1. 有音频时：
   - 音频 renderer 提供 `PlayedPtsUs()`。
   - 视频帧 PTS 小于 `audio_pts - late_threshold_us` 时丢弃。
   - 视频帧 PTS 大于 `audio_pts + early_threshold_us` 时短暂等待。
   - 阈值第一版可配置为 late 80ms、early 20ms。

2. 无音频时：
   - 使用 `steady_clock` + 首帧 PTS 建立系统播放时钟。
   - 允许 `no_realtime` 测试模式尽快渲染所有帧。

3. PTS 异常时：
   - 对缺失、倒退、明显跳变的 PTS 做归一化。
   - 摄像头实时流可按 FPS 生成连续 PTS 作为 fallback。

建议新增：

- `include/render/av_sync_config.h`
- `include/render/av_sync_controller.h`
- `src/render/av_sync_controller.cpp`

验收标准：

- 摄像头手动测试音频和视频体感同步。
- 长时间预览时 render stats 中队列长度保持稳定。
- 输入卡顿后能够恢复，不出现无限积压。

### 阶段 5：视频渲染质量与性能

目标：

- 提升格式覆盖、渲染质量和性能。
- 避免所有视频帧都走 CPU RGBA 转换造成额外拷贝。

短期改进：

1. 窗口尺寸变化时保持画面比例，增加 letterbox/pillarbox viewport。
2. 支持 `RenderConfig` 中配置背景色、缩放模式、是否保持宽高比。
3. 给 `FrameConverter` 增加更多错误上下文和统计。
4. 增加 `test_frame_converter` 对 stride、plane offset、奇数宽高的覆盖。

中期改进：

1. 为 NV12/I420 增加 OpenGL YUV shader 路径：
   - NV12 使用 Y plane + interleaved UV plane 两张纹理。
   - I420 使用 Y/U/V 三张纹理。
   - RGB/BGR/GRAY 仍可走 CPU 转换或简单 shader。
2. 保留 `FrameConverter` 作为 fallback 和单元测试基线。
3. 可选引入 PBO 异步纹理上传，降低 `glTexSubImage2D` 同步等待。

长期改进：

1. 评估硬件解码帧直达渲染路径。
2. Windows 可评估 D3D11/NV12 texture interop，但不建议在当前阶段抢先引入。

验收标准：

- 1080p 摄像头预览 CPU 占用明显低于纯 CPU RGBA 转换路径。
- resize 后画面不拉伸、不倒置、不出现黑屏。
- 自动测试覆盖主要像素格式，手动测试覆盖真实摄像头。

### 阶段 6：集成到应用层

目标：

- 让 render 能被 native player 或后续 Tauri/C API 使用。
- 让 “拉流 -> 解码 -> 本地预览” 成为工程内一条正式 pipeline。

建议任务：

1. 新增一个独立 manual test：
   - `test/render/test_rtsp_decode_render.cpp`
   - 只负责 RTSP pull + decode + RenderSession，不再混入 encode。

2. 将当前 `test_opengl_video_renderer.cpp` 拆分：
   - `--smoke` 保留自动测试。
   - `--loop` 保留静态窗口手动测试。
   - 摄像头 AV render 移到专门测试文件，减少单文件复杂度。

3. 为应用层设计稳定入口：
   - C++ native 直接使用 `IRenderSession`。
   - Tauri/C API 后续暴露 `create/start/submit/stop/get_stats` 风格句柄接口。

4. 支持配置输入：
   - RTSP URL
   - 是否启用音频
   - 是否启用视频
   - transport tcp/udp
   - target latency

验收标准：

- 一条命令可启动摄像头本地预览。
- 应用层可以读取 render stats。
- render session 停止后没有线程泄漏、窗口残留或声卡占用。

### 阶段 7：诊断、测试与稳定性

目标：

- 建立 render 模块持续可验证能力。
- 将设备相关测试和无设备测试分离。

自动测试建议：

1. `test_frame_converter`
   - 继续作为纯 CPU 单元测试。
   - 覆盖更多 stride/plane/尺寸边界。

2. `test_filter_audio_resample`
   - 覆盖更多采样格式，例如 FLTP -> S16、S16 -> FLT。
   - 覆盖声道布局转换，例如 mono -> stereo。

3. `test_render_session_queue`
   - 不创建真实窗口和声卡，使用 fake video/audio renderer。
   - 验证队列满时丢帧策略。
   - 验证 pause/resume/stop 生命周期。

4. `test_av_sync_controller`
   - 输入不同 PTS 序列，验证 render/drop/wait 决策。

手动测试建议：

1. `test_opengl_video_renderer --loop`
2. `test_opengl_video_renderer --camera`
3. 后续新增 `test_rtsp_decode_render --url rtsp://...`
4. 使用真实 1080p 摄像头连续运行 30 分钟，观察音画同步、CPU、内存、队列长度。

日志建议：

- 初始化参数：窗口尺寸、OpenGL version、WASAPI mix format。
- 运行统计：每 3-5 秒输出一次 rendered/dropped/queue/pts/underrun。
- 错误上下文：包含模块名、输入格式、目标格式、HRESULT 或 FFmpeg error string。

验收标准：

- 无设备自动测试可以在 CI 或普通构建环境通过。
- 设备相关测试默认不进入 CTest，只作为 manual test。
- 长时间运行 stats 稳定，没有明显内存增长。

## 优先级清单

P0：

1. 新增 `RenderSession`，把 OpenGL 调用移出 decoder callback。
2. 增加 fake renderer 单元测试，验证 session 生命周期和队列策略。
3. 拆分摄像头手动测试，形成专门的 RTSP decode render demo。

P1：

1. WASAPI 改为异步 ring buffer 写入。
2. 引入音频 master clock 和基础 AV sync。
3. 增加 render stats，并在手动测试中定期输出。

P2：

1. 视频 resize 保持宽高比。
2. YUV shader 直接渲染 NV12/I420。
3. 增加更多像素格式和音频格式测试。

P3：

1. Tauri/C API 集成。
2. 设备选择、音量、静音、暂停恢复。
3. 跨平台音频/视频后端评估。

## 待决问题

1. RenderSession 是否默认启用音频
   - 建议默认启用，但如果系统无默认播放设备，应允许配置为非致命错误。

2. 是否引入第三方跨平台音频库
   - 当前 Windows 优先，用 WASAPI 足够。
   - 如果要快速支持 Linux/macOS，可评估 miniaudio 或 SDL audio；但在 Windows 第一阶段不建议增加依赖。

3. 是否把视频颜色转换迁到 filter/video
   - CPU swscale/格式转换适合放到 filter/video。
   - OpenGL shader 直接采样 YUV 属于 render/video。
   - `FrameConverter` 后续可拆成 `filter/video/PixelConvertFilter`，OpenGL renderer 只保留 fallback 调用。

4. 播放时钟选择
   - 预览场景建议音频 master。
   - 静音或 video-only 场景使用系统时钟。
   - 离线测试可以提供 no-realtime 模式。

5. 丢帧策略
   - 实时摄像头预览建议队列满丢旧帧。
   - 文件播放可配置为不丢帧，或只在严重滞后时丢帧。

## 推荐下一步

阶段 2 已完成，下一步建议实施阶段 3：

1. 将 WASAPI 写入改为 event-driven 音频线程和 PCM ring buffer。
2. 让 `IAudioRenderer::Render()` 快速入队，避免同步等待声卡 buffer。
3. 增加 queued frames、underrun、dropped samples 和稳定 played PTS 统计。
4. 保持 `RenderSession` 的公开提交接口不变，只替换音频 renderer 内部实现。

## 阶段 3 实施记录

实施状态：已完成第一版落地（2026-07-28）。

本阶段实际完成内容：
1. `WasapiAudioRenderer::Render()` 已从同步写 WASAPI 改为“重采样后快速写入内部 PCM 队列”。
2. WASAPI 写声卡动作移动到 renderer 内部音频线程，由 event callback 节奏驱动。
3. PCM 队列复用 `common/queue` 中的 moodycamel 队列封装，并为 `BoundedMpmcQueue` 增加 `try_push()` 和 `clear()`，用于有界入队与停机清理。
4. 队列满时优先丢弃旧 PCM chunk，再尝试写入新 chunk，预览场景下优先控制实时性。
5. 声卡可写但 PCM 数据不足时写入静音，并累计 `audio_underruns`。
6. 新增 `AudioRenderStats`，把 `queued_pcm_frames`、`queued_pcm_chunks`、`dropped_pcm_frames`、`underruns` 等内部状态汇总到 `RenderSession::GetStats()`。
7. `test_opengl_video_renderer --camera` 的周期日志增加音频内部队列、PCM 丢弃和 underrun 统计，便于定位声音异常。
8. `test_render_session` 使用 fake audio renderer 覆盖音频 renderer 内部统计向 session 汇总的行为。

本阶段仍需真实设备手动确认：
1. 使用 `test_opengl_video_renderer --camera` 观察声音是否从明显卡顿/爆音改善为稳定播放。
2. 观察 `audio_underruns` 是否持续增长；如果持续增长，下一步应调整 camera 测试中的编码覆盖耗时、WASAPI buffer 时长或队列容量。
3. 长时间预览时观察 `audio_renderer_queue_size` 是否稳定在较低范围，避免延迟持续累积。

推荐下一步：进入阶段 4，实现以音频播放时钟为 master 的视频 PTS 调度、晚帧丢弃和早帧等待。
