# 录像模块实现评估与技术方案

**文档状态：** 设计评审稿  
**评估日期：** 2026-08-01  
**适用仓库：** video-pipeline

## 1. 结论摘要

当前工程已经具备录像模块所需的媒体基础能力，但还没有形成可持续录像的产品级闭环。

可以复用的基础设施：

- FFmpegPuller：从 RTSP、文件等输入源获得编码后的 MediaPacket。
- MediaStreamSession / MediaStreamSource：管理拉流生命周期、重连、抖动缓冲和多订阅分发。
- FFmpegDecoder / FFmpegEncoder：在需要转码、叠加 OSD 或统一音频格式时使用。
- FfmpegMuxProtocol：封装 FFmpeg libavformat 的轨道创建、extradata、时间基转换、bitstream filter、header/trailer 和基础错误处理。
- MediaPacket、MediaTrackConfig、EncodedAccessUnit：可作为录像链路的数据模型基础。

把 DefaultPublisher 的输出 URL 改成 .mp4，只能证明单文件复用可行，不能得到完整的录像能力。主要缺口是：

1. 没有 Recorder 生命周期、分片和文件命名模型。
2. MediaStreamSource 的订阅回调同步执行，文件 I/O 会阻塞拉流分发。
3. FfmpegMuxProtocol::Write() 面向网络发布，不适合直接承担本地文件轮转。
4. 没有关键帧对齐切片、音视频时间戳连续性和断流标记。
5. 没有临时文件、原子落盘、异常退出恢复、磁盘配额、保留策略和索引。
6. 没有录像专用的长时间、故障、磁盘压力和播放器兼容性测试。

总体建议：新增独立 Recorder 模块，复用现有媒体模型和 FFmpeg 低层复用能力；不要把录像语义继续堆进 PublisherConfig/IPublisher。

| 评价对象 | 结论 |
|---|---|
| 媒体采集/解码/编码/复用基础 | 基础完整，可作为录像底座 |
| 面向生产的录像能力 | 尚未实现，不建议直接上线 |

## 2. 当前实现评估

### 2.1 当前链路

~~~text
输入 RTSP/文件/AVTP
  -> FFmpegPuller / AvtpPuller
  -> MediaStreamSession
  -> MediaStreamSource
  -> MediaPacket subscriber
       +-> FFmpegDecoder -> FFmpegEncoder -> IPublisher -> RTSP/RTMP/ZLM
       `-> （拟新增）Recorder -> SegmentWriter -> 本地文件/索引
~~~

### 2.2 组件与缺口

| 组件 | 现状 | 可复用点 | 需要补强 |
|---|---|---|---|
| MediaPacket | 编码数据、PTS/DTS、duration、time base、keyframe、buffer/backend | 直接作为复用输入 | 增加或外挂 discontinuity、wall-clock、序号 |
| MediaStreamSource | 多订阅分发、缓存 StreamInfo | 作为 Recorder 上游 | callback 不应持锁同步执行；需要可移除订阅 |
| FFmpegEncoder | 支持 H.264/H.265/AAC/Opus，提供 extradata | OSD、转码、G.711 转 AAC | 增加 RequestKeyframe/IDR 能力 |
| FfmpegMuxProtocol | 创建 AVStream、写 header/trailer、时间基转换、filter、基础重连 | 抽取通用 mux core | 分片、恢复、文件状态、存储策略 |
| DefaultPublisher | 统一 Start/Publish/Stop | 参考错误结果设计 | 网络重连语义不适合录像 |
| PublisherConfig | 网络 URL、轨道和 FFmpeg 选项 | 借鉴 track 描述 | 不承载 segment、retention、index |
| MediaStreamSession | 重连、jitter、watchdog | 提供源状态 | 重连后传播 discontinuity，等待关键帧建段 |
| 现有测试 | RTSP、ZLMediaKit、编解码和 publisher | 复用 test.mp4 和 ffprobe | 增加 file mux、分片、恢复和压测 |

## 3. 优先级问题清单

### P0-1：写文件不能阻塞拉流线程

MediaStreamSource::OnPacket() 当前在 subscriber_mutex_ 保护下同步调用 subscriber。若 Recorder callback 直接调用 av_write_frame()，磁盘抖动或 FFmpeg I/O 会阻塞整个流。

处理方案：callback 只入有界队列；Recorder 使用独立 writer 线程；OnPacket() 先复制 subscriber 列表，释放锁后再调用外部 callback；提供 subscription token；记录队列高水位、丢包数和写入延迟。

### P0-2：时间戳必须统一

工程通常使用微秒时间基 1/1,000,000，FFmpeg muxer 最终可能使用视频 1/90,000、音频采样率时间基。已有 rescale 逻辑应保留，但录像还需要 first_dts/last_dts/last_pts、段内时间戳归零、倒退/跳变策略、重连 discontinuity 以及音频早于视频的处理。

建议内部统一用微秒整数，写入 FFmpeg 前以 AVStream::time_base 为准转换；每段以首个可写视频关键帧为基准，输出非负时间戳。

### P0-3：文件必须可恢复

不要直接写最终文件名，采用 .part 临时后缀。正常关闭时写 trailer、关闭文件、刷新并原子重命名。启动时扫描 .part，删除、标记损坏或按容器能力恢复；不能把未完成文件伪装成完整录像。

### P1：MVP 后补齐

- 关键帧对齐的时间/大小分片及最大段时长。
- 磁盘空间阈值、保留天数、最大容量和写满降级。
- 录像索引：起止时间、路径、时长、字节数、轨道和完整状态。
- H.264 + AAC 的 MP4 兼容性矩阵，以及 G.711/H.265 的容器策略。
- 输入重连、参数变化、状态、队列、丢帧、延迟和磁盘空间指标。

### P2：增强功能

事件前后缓存、检测结果绑定、OSD、加密、远端对象存储、上传重试和多路调度。

## 4. 推荐架构

~~~text
MediaStreamSource
  -> RecorderSession::OnPacket(shared_ptr<MediaPacket>)
  -> BoundedRecordQueue
  -> RecorderWriterThread
       -> TimestampNormalizer
       -> SegmentController
       -> FfmpegSegmentMuxer
       -> StorageManager
       -> RecordIndex
~~~

| 模块 | 职责 |
|---|---|
| RecorderSession | 生命周期、状态、统计、上游订阅 |
| BoundedRecordQueue | 有界队列、停止唤醒、容量统计和丢弃 |
| TimestampNormalizer | 时间基、倒退/跳变修正、discontinuity |
| SegmentController | 时长/大小、关键帧切换、最大时长保护 |
| FfmpegSegmentMuxer | AVFormatContext、AVStream、packet、trailer |
| StorageManager | 目录、临时文件、原子重命名、空间检查、清理 |
| RecordIndex | segment 元数据、查询和恢复 |

不建议直接复用 IPublisher：Publisher 面向网络连接和重连；Recorder 面向多个本地 segment、完整状态、索引和存储失败处理。建议抽取公共 FfmpegMuxerCore，让网络 publisher 和 file muxer 共享 codec 映射、extradata、时间基转换、bitstream filter 和 FFmpeg 错误转换，但分离各自生命周期。

## 5. 配置和接口建议

### 5.1 RecorderConfig

建议新建 include/media/recorder/recorder_config.h：

~~~cpp
enum class RecordContainer { Mp4, FragmentedMp4, MpegTs, Matroska };
enum class RecordQueuePolicy { RejectNewest, DropNonKeyVideo, DropAudioFirst, BlockWithTimeout };

struct RecorderConfig {
    std::filesystem::path root_dir;
    std::string stream_id;
    RecordContainer container{RecordContainer::Mp4};
    int segment_duration_sec{300};
    int max_segment_duration_sec{360};
    std::uint64_t max_segment_bytes{0};
    std::size_t queue_capacity{2048};
    RecordQueuePolicy queue_policy{RecordQueuePolicy::DropAudioFirst};
    int queue_block_timeout_ms{20};
    int min_free_space_mb{1024};
    int retention_days{7};
    std::uint64_t max_storage_bytes{0};
    bool write_fast_start{true};
    bool recover_incomplete_segments{true};
};
~~~

校验规则：root_dir 可创建；stream_id 为安全文件名；segment_duration_sec > 0；最大时长不小于目标时长；MP4 首版只接受 H.264/H.265 + AAC；codec、尺寸、采样率变化时自动切段。

### 5.2 生命周期接口

~~~cpp
enum class RecorderState {
    Idle, Starting, Recording, Rotating, Degraded, Stopping, Stopped, Error
};

class IRecorder {
public:
    virtual ~IRecorder() = default;
    virtual bool Start(const RecorderConfig&, const MultiStreamInfo&) = 0;
    virtual bool OnPacket(std::shared_ptr<MediaPacket>) = 0;
    virtual void Stop() = 0;
    virtual bool Rotate() = 0;
    virtual RecorderState GetState() const = 0;
    virtual RecorderStats GetStats() const = 0;
    virtual std::optional<RecordSegment> GetCurrentSegment() const = 0;
};
~~~

OnPacket() 只入队，不做 FFmpeg I/O。Stop() 依次停止接收、排空队列、写 trailer、关闭当前段；超时后才放弃未完成段。

### 5.3 文件和索引

建议布局：

~~~text
<root>/<stream_id>/
  2026/08/01/
    120000_120500_000123.mp4
    120000_120500_000123.json
    120500_121000_000124.mp4
    index.jsonl
~~~

元数据至少包含 stream_id、相对路径、起止 wall-clock、媒体时长、字节数、容器、complete、音视频轨道、codec、discontinuity_count。

单路低并发 MVP 可用按天 index.jsonl 加原子追加；多路和复杂查询建议增加 SQLite，并通过 RecordIndex 隔离。当前 vcpkg.json 没有 SQLite，选择 SQLite 时要同步增加依赖和 CMake target。

## 6. 核心实现方案

### 6.1 编码包直接复用优先

对于摄像头已经输出 H.264 + AAC 的场景，不要先解码再编码：

~~~text
MediaPacket(H264/AAC)
  -> RecorderQueue
  -> TimestampNormalizer
  -> SegmentController
  -> FfmpegSegmentMuxer
  -> MP4/fMP4
~~~

只有以下情况才进入解码/编码路径：

- 需要 OSD、检测框或图像处理；
- 原始编码格式不被目标容器或播放器接受；
- 需要将 G.711 统一转成 AAC；
- 需要统一分辨率、帧率、码率或 GOP。

### 6.2 轨道建立

- MediaPacket::stream_index 只表示源 stream，不作为持久化业务 ID；
- Recorder 内部为每条轨道分配稳定 track_id；
- 保存 codec、宽高、fps、sample rate、channels、time base、extradata；
- 同一录像任务中轨道顺序固定；
- codec、尺寸或采样率变化时标记 discontinuity，关闭当前段并重新建段。

### 6.3 时间戳策略

1. 入队后把输入 pts/dts/duration 转成微秒整数。
2. 每条轨道记录 first_dts_us、last_dts_us、last_pts_us。
3. DTS/PTS 小幅倒退时做单调修正；大幅跳变按 discontinuity 处理。
4. segment 开始时，以首个可写视频关键帧的 DTS 为基准，所有轨道减去该基准。
5. 写入 FFmpeg 前使用 av_rescale_q() 或 av_packet_rescale_ts() 转换到目标 AVStream::time_base。
6. 使用 av_interleaved_write_frame()，避免音视频到达顺序与 DTS 顺序不一致时破坏容器交错。
7. 不要把 wall-clock 直接当作媒体 PTS；wall-clock 只用于文件命名和索引。

### 6.4 分片算法

推荐时间到期后等待视频关键帧：

~~~text
current_duration >= target_duration
        -> pending_rotate=true
        -> 继续写入，直到下一个视频 keyframe
        -> flush/trailer/close 当前段
        -> 原子重命名并写索引
        -> 用该 keyframe 开启下一段
~~~

必须有 max_segment_duration 保护。超过最大时长仍没有关键帧时：

- 转码模式请求编码器生成 IDR；
- 复用模式允许在 discontinuity 处强制切段，并标记该段需要关键帧恢复；
- 不要无条件在 P/B 帧处切 MP4，否则下一段可能无法独立解码。

### 6.5 容器选择

| 容器 | 建议用途 | 优点 | 缺点 |
|---|---|---|---|
| MP4 | 首版 H.264 + AAC 分段 | 兼容性好、检索方便 | 未写 trailer 时恢复能力弱 |
| fMP4 | 对掉电/崩溃敏感的连续录像 | 适合恢复和低延迟 | 兼容性和索引更复杂 |
| MPEG-TS | G.711、异常网络和快速落盘 | 中断更宽容 | 文件较大、随机定位较差 |
| Matroska | H.265/G.711 等组合 | codec 容忍度高 | 播放生态不如 MP4 |

路线建议：MVP 使用普通 MP4，段长 1～5 分钟；稳定性增强时增加 fMP4 或 TS；H.265/G.711 按实际播放端选择 MKV/TS，不强行塞进 MP4。

### 6.6 队列和丢弃策略

- 队列按包数和字节数双重限制；
- 音频优先丢弃，视频非关键帧次之；
- 视频关键帧不能静默丢弃；
- 丢包后设置 discontinuity_pending，下一个视频关键帧重新建立段；
- 记录 queue_high_watermark、dropped_audio_packets、dropped_video_non_key_packets、write_latency_ms；
- 持续超过阈值时进入 Degraded，由上层决定停录、降码率或切换存储盘。

## 7. 建议修改的代码位置

### 7.1 新增文件

~~~text
include/media/recorder/recorder_config.h
include/media/recorder/i_recorder.h
include/media/recorder/record_segment.h
include/media/recorder/record_stats.h
include/media/recorder/record_queue.h
include/media/recorder/timestamp_normalizer.h
include/media/recorder/segment_controller.h
include/media/recorder/ffmpeg_segment_muxer.h
include/media/recorder/record_index.h
include/media/recorder/recorder_session.h

src/media/recorder/record_queue.cpp
src/media/recorder/timestamp_normalizer.cpp
src/media/recorder/segment_controller.cpp
src/media/recorder/ffmpeg_segment_muxer.cpp
src/media/recorder/record_index.cpp
src/media/recorder/recorder_session.cpp
~~~

CMakeLists.txt 使用 src/*.cpp 递归 glob，新文件会自动进入 video_pipeline_lib；仍应确认新增目录被收集，并补充独立测试目标。

### 7.2 需要修改的现有文件

| 文件 | 修改内容 |
|---|---|
| include/media/stream/stream_source.h 与 src/media/stream/stream_source.cpp | 增加可移除 subscriber 或 subscription token；复制回调列表后再调用，避免持锁执行外部代码 |
| include/media/media_packet.h | 可选增加 discontinuity、wallclock_us、sequence；也可以新增 RecordPacketMeta 包装结构 |
| include/media/protocol/ffmpeg_mux_protocol.h/.cpp | 抽取公共 mux core；网络重连和文件 segment 逻辑分离；统一时间基和 interleaved write |
| include/media/encoder/ffmpeg_encoder.h/.cpp | 后续增加 RequestKeyframe()；转码录像轮转时需要 |
| test/CMakeLists.txt | 增加 recorder 单元、文件复用、分片、恢复和压力测试 |
| vcpkg.json 与 CMakeLists.txt | 若采用 SQLite 索引，增加 SQLite 依赖和 target；否则先使用 Boost.JSON 的 JSONL 索引 |

## 8. 分阶段实施计划

### M0：可行性 Spike（1～2 个开发日）

- 用 test.mp4 或现有 RTSP 流作为输入；
- 不解码、不编码，直接把 H.264 + AAC packet 送入临时 FfmpegSegmentMuxer；
- 用 ffprobe 验证轨道、时长、帧率和关键帧；
- 验证时间基、DTS 排序和音视频同步；
- 输出 packet/容器兼容性结论。

验收：连续写入 10 分钟，VLC、ffplay、FFmpeg 均可播放，音视频无明显漂移。

### M1：单文件 Recorder MVP（3～5 个开发日）

- 新增 IRecorder、RecorderConfig、独立 writer 线程和有界队列；
- 支持 H.264 + AAC 的 MP4；
- .part 临时文件、正常 Stop、原子重命名；
- 统计写包数、字节数、时长、队列深度和错误码；
- 通过 MediaStreamSource 订阅接入，不阻塞拉流回调。

验收：启动/停止 100 次无资源泄漏；写入失败有明确错误；源流重连不会卡死主线程。

### M2：分片和索引（3～5 个开发日）

- 目标时长和最大时长；
- 视频关键帧对齐；
- 每段元数据和按天索引；
- 时间范围查询接口；
- 段完成后再对外可见。

验收：连续运行 24 小时，段间无重叠或明显空洞；任意完整段可独立播放；索引与文件一致。

### M3：故障恢复和存储治理（3～4 个开发日）

- 启动扫描 .part 和孤儿索引；
- 磁盘剩余空间阈值、保留天数、最大容量；
- 写满、权限失败、目录不可用、设备断开等错误状态；
- 重连和 codec 参数变化时自动切段；
- 指标和日志接入。

验收：模拟进程崩溃、强制终止、磁盘写满、输入断流；重启后不阻塞新录像，坏段状态可追踪。

### M4：转码、事件和多路

按需求拆分 H.265/G.711/Opus 容器策略、解码/OSD/检测链路、事件前后缓存、多路队列隔离、SQLite 查询和远端归档。

## 9. 测试与验收矩阵

### 9.1 单元测试

- RecorderConfig 合法性校验；
- 输入 stream 到 recorder track 的映射；
- PTS/DTS rescale、倒退、跳变和归零；
- 关键帧识别和分段边界；
- 队列满时的丢弃策略；
- 文件名、目录和路径安全校验；
- .part 恢复扫描和索引一致性。

### 9.2 集成测试

- H.264 only；
- H.264 + AAC；
- H.264 + G.711（MP4 应按配置拒绝或切换 TS/MKV）；
- RTSP over TCP/UDP 输入；
- 输入重连、无关键帧等待、参数集变化；
- 1 分钟、1 小时、24 小时连续录像；
- 录像同时推 RTSP/ZLM，确认 Recorder 不影响 Publisher。

### 9.3 故障和压力测试

- 任意 packet 写入期间强制终止进程；
- 写 trailer 期间关闭文件句柄；
- 模拟磁盘剩余空间不足；
- 降低 writer 线程速度，验证队列告警和丢弃策略；
- 多路同时录像，观察 CPU、内存、句柄、线程和磁盘吞吐；
- 用 VLC、ffplay、浏览器/业务播放器和目标 NVR 验证播放。

### 9.4 建议验收指标

| 指标 | MVP 目标 |
|---|---:|
| 录像写入线程阻塞上游 | 不允许同步阻塞 |
| 单路 1080p H.264 + AAC CPU 增量 | 复用模式尽量低于 5%～10%，以实测机器为准 |
| 正常分段时长误差 | 目标时长 ±1 个 GOP，不超过最大段时长 |
| 完整段独立播放成功率 | 100% |
| 进程异常退出影响范围 | 最多当前 .part 段，历史完整段不受影响 |
| 磁盘写满行为 | 明确错误/降级，不无限增长内存 |
| 长时间 A/V 漂移 | 1 小时内小于 100 ms，最终以业务要求为准 |

## 10. 不建议的实现方式

1. 不要在 MediaStreamSource::OnPacket() 中直接写文件。
2. 不要把 PublisherConfig 扩展成同时描述网络发布和本地录像的万能配置。
3. 不要把输入 stream_index 直接当作持久化 track ID。
4. 不要以 wall-clock 直接填充媒体 PTS。
5. 不要在没有关键帧的情况下频繁强切 MP4 段。
6. 不要默认把 G.711 直接写 MP4 并假设所有播放器兼容。
7. 不要仅依赖 Stop() 写 trailer 来解决崩溃恢复。
8. 不要使用无上限队列；录像故障不应拖垮整个拉流和推流进程。

## 11. 第一批具体任务

1. REC-001：完成 H.264 + AAC 编码包直接写 MP4 的 Spike，并用 ffprobe 验证。
2. REC-002：抽取 FfmpegMuxerCore，让 network publisher 与 file muxer 共用轨道、时间基和 codec 代码。
3. REC-003：实现 BoundedRecordQueue 和独立 RecorderWriterThread。
4. REC-004：实现 RecorderSession、Recorder 状态机和统计。
5. REC-005：实现 .part、trailer、原子重命名和启动恢复扫描。
6. REC-006：实现关键帧分段和 segment 元数据。
7. REC-007：补充 RTSP 输入、重连、参数变化和长时间录像集成测试。
8. REC-008：根据产品要求确定 MP4/fMP4/TS/MKV 和 SQLite/JSONL 索引路线。

## 12. 最终建议

推荐的最小闭环：

~~~text
MediaStreamSource
  -> RecorderSession（有界队列）
  -> FfmpegSegmentMuxer（H264 + AAC，直接复用）
  -> 1～5 分钟 MP4 分片
  -> .part + 原子重命名
  -> JSONL 索引
~~~

这条链路经过 24 小时和故障测试后，再加入转码、OSD、事件录像、H.265/G.711 和 SQLite 查询。这样能最大化复用当前工程已有能力，同时把网络发布和本地持久化的生命周期、错误处理和存储责任分开，避免继续在 Publisher 模块上堆积录像特有逻辑。
