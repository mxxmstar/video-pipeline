# MediaFlow 封装 media 流程评估与实施指南

本文档基于当前工程中 `include/media`、`src/media`、`include/mediaflow` 和
`test/media` 的实际实现，评估如何使用 MediaFlow 封装拉流、解码、编码和发布流程，
并列出 `media` 模块在正式接入前仍需完成的修改。

本文档是 [`media-flow-design.md`](media-flow-design.md) 的媒体接入细化说明。
前者描述框架总体设计和阶段规划，本文档聚焦可直接执行的文件级改造、接口契约、
节点行为、测试方法和验收标准。

> 审计基线：2026-08-05，当前 `main` 分支。

## 1. 结论

使用 MediaFlow 封装现有 `media` 流程是可行的，但当前还不适合只增加四个节点后就
直接替换现有手工链路。主要原因不是底层编解码或协议能力缺失，而是以下跨组件契约
尚不完整：

1. 媒体类型、轨道身份和时间基存在歧义，节点化后会被多线程和多轨场景放大。
2. 拉流会话的停止、重连和回调生命周期还不能可靠覆盖快速停止/重启。
3. 解码器和编码器缺少适合图生命周期的显式刷新接口。
4. 编码器输出没有完整填写发布所需的轨道与时间基信息。
5. 发布端对音视频交织、重连后等待关键帧和多轨唯一路由仍有缺口。
6. MediaFlow 当前只有立即停止语义，尚缺少媒体组件所需的在途任务屏障和有序刷新。

推荐采用以下方式实施：

- 保留 `IPuller`、`MediaStreamSession`、`IDecoder`、`IEncoder`、`IPublisher` 和协议
  实现的领域职责。
- 在 `mediaflow` 命名空间中增加轻量适配节点，不让 FFmpeg、AVTP 或 RTSP 类直接
  继承 MediaFlow 节点基类。
- 一个有状态媒体对象只由一个 Serialized 节点拥有和调用。
- 先修复 P0 数据与生命周期契约，再实现主链路节点；不要在节点中重复补丁式猜测
  `stream_index`、`time_base` 或 `duration`。

首期目标链路为：

```text
PipelineController
  |-- 输入探测、轨道选择、错误监督、停止请求
  `-- MediaFlow Graph
        StreamSourceNode
          -> TrackRouterNode
          -> DecoderNode
          -> EncoderNode
          -> PublisherSinkNode
```

对于不需要转码的场景，可使用：

```text
StreamSourceNode -> TrackMapNode -> PublisherSinkNode
```

后续再接入推理、OSD、渲染、录像和 WebRTC，不应在首期节点中预先混入这些职责。

## 2. 适配边界

### 2.1 应由 MediaFlow 管理的内容

- 节点拓扑和端口类型。
- 节点使用哪个 Executor。
- Edge 的队列容量、背压和丢弃统计。
- 节点生命周期、启动顺序和停止顺序。
- Pipeline 级错误、重启策略和指标汇总。
- 消息的流、轨道、代次和序号上下文。

### 2.2 继续由 media 管理的内容

- FFmpeg demux、decode、encode 和 mux 细节。
- AVTP、RTP、RTCP、RTSP 的协议解析和网络发送。
- 编解码器参数、extradata、像素格式和采样格式转换。
- 拉流器的协议专用配置和 I/O 中断机制。
- Publisher 的协议能力和输出 URL。

### 2.3 不推荐的改造方式

- 不让 `FFmpegDecoder`、`FFmpegEncoder` 或 `DefaultPublisher` 直接继承 `INode`。
- 不把 MediaFlow 的队列逻辑塞入 `MediaStreamSession`、FFmpeg muxer 或 RTSP session。
- 不继续用 `MediaPacket::stream_index` 同时表示输入 stream 和输出 track。
- 不在测试或 PublisherNode 中手工猜测编码 packet 的时间基和持续时间。
- 不允许节点在自己的工作线程中直接调用 `Graph::Stop()`；这可能让 Executor
  尝试 join 当前线程。节点只上报事件，由外部控制线程停止 Graph。

## 3. 当前可复用程度

| 区域 | 当前状态 | 接入判断 |
|---|---|---|
| MediaFlow Graph、Node、Port | 已具备类型安全连接和基本生命周期 | 可复用，但需补媒体停止屏障和拓扑生命周期顺序 |
| QueueTransport | 已具备有界容量、Block、DropNewest、DropOldest | 可复用；媒体链路需按数据类型单独配置 |
| Node Dispatch | 已有 Serialized、Concurrent 和任务上限 | 有状态媒体节点只能使用 Serialized |
| FFmpeg/AVTP Puller | 已输出统一 `MediaPacket` | 可复用；需补读结果、轨道选择和重复启动契约 |
| MediaStreamSession | 已有重连、watchdog 和回调 | 需较大生命周期修复后再由 SourceNode 包装 |
| FFmpegDecoder | 已支持 packet 输入和多 frame 回调 | 可复用；需显式 Flush、重复 Open 和停止并发修复 |
| FFmpegEncoder | 已支持视频、音频和 `Encode(nullptr)` | 可复用；需修复 PTS、time base、track 和输出描述 |
| DefaultPublisher | 已统一协议适配和结构化结果 | 可复用；需修复线程查询、重连结果和失败状态语义 |
| FFmpeg mux | 已支持多轨、超时、filter 和基础重连 | 需改音视频交织和关键帧恢复状态 |
| RTSP Server | 已有异步会话、代次闸门和客户端隔离 | 可复用；需补协议入口队列上限和多轨唯一匹配 |
| 媒体适配节点 | 尚未实现 | 需要新增 |

## 4. 消息与数据契约

### 4.1 不再把 stream_index 当作通用轨道 ID

当前工程至少存在三种编号域：

| 编号 | 含义 | 生命周期 |
|---|---|---|
| `MediaStreamInfo::stream_index` | 输入 demuxer 中的 stream 编号 | 当前输入连接 |
| MediaFlow `source_track_id` | Pipeline 内识别输入轨道的逻辑编号 | 当前 Pipeline 代次 |
| `MediaTrackConfig::track_id` | 当前 Publisher 任务中的输出轨道编号 | 当前发布任务 |
| FFmpeg `AVStream::index` | 输出 mux context 内的 stream 编号 | 当前输出连接 |

这些编号可能碰巧相等，但定义不同。节点间消息应显式携带输入轨道和输出轨道，
PublisherSinkNode 再完成逻辑输出轨道到 Publisher 配置的映射。

首期可以令 `source_track_id` 等于输入 `stream_index`，但必须把这个关系限制在
PipelineBuilder 的一次配置映射中，不能继续作为 Publisher 的隐含全局约定。

### 4.2 MediaPacket 时间戳契约

当前 `MediaPacket` 注释把 `pts`、`dts`、`duration` 写成微秒，同时类中又保留
`time_base`。编码器实际输出的是 codec time base 下的 tick，但没有填写
`time_base`，因此测试只能手工改成微秒。

建议统一为以下契约：

- `MediaPacket::pts`、`dts`、`duration` 均为 `time_base` 下的整数 tick。
- `MediaPacket::time_base` 必须满足 `num > 0 && den > 0`。
- Puller 可以继续把 packet 规范化到 `1/1000000`，但这只是合法实现之一。
- Encoder 输出 packet 使用编码器实际 `AVCodecContext::time_base`。
- Publisher 和 Decoder 必须根据 `time_base` 做显式重标定。
- `MediaFrame::MediaTime` 继续使用字段名明确的微秒单位。
- 无时间戳不能继续与合法的 `0` 混用，应定义统一的 `kNoTimestamp`，或使用
  显式有效标志。

建议增加公共时间工具，所有组件复用同一实现：

```cpp
namespace mediaflow {

bool IsValidTimeBase(const Rational& value);
int64_t RescaleTimestamp(int64_t value,
                         const Rational& source,
                         const Rational& destination);

} // namespace mediaflow
```

### 4.3 建议的消息结构

新消息只放在 `mediaflow` 命名空间，不增加更长的嵌套命名空间：

```cpp
namespace mediaflow {

struct MessageContext {
    std::string pipeline_id;
    std::string stream_id;
    int source_track_id{-1};
    int output_track_id{-1};
    std::uint64_t generation{0};
    std::uint64_t sequence{0};
};

struct PacketMessage {
    MessageContext context;
    std::shared_ptr<const MediaPacket> packet;
};

struct FrameMessage {
    MessageContext context;
    std::shared_ptr<const MediaFrame> frame;
};

struct StreamDescriptorMessage {
    std::string pipeline_id;
    std::string stream_id;
    std::uint64_t generation{0};
    MultiStreamInfo streams;
};

enum class PipelineEventSeverity {
    Info,
    Warning,
    RecoverableError,
    FatalError,
};

struct PipelineEvent {
    PipelineEventSeverity severity{PipelineEventSeverity::Info};
    std::string node_id;
    MessageContext context;
    std::string code;
    std::string message;
};

} // namespace mediaflow
```

消息约束：

- 同一 `(stream_id, source_track_id, generation)` 内的 `sequence` 单调递增。
- 重连导致 codec、分辨率、采样率或 extradata 变化时，递增 `generation`。
- 下游不得把旧代次 packet/frame 与新代次状态混合处理。
- fan-out 后输入默认只读。需要修改载荷时创建新对象或使用显式 copy-on-write。
- `BackendHandle::ptr` 的有效期必须由同一个消息中的 `buffer` 保证；不得脱离
  `buffer` 单独缓存裸指针。

### 4.4 控制消息的顺序

流描述必须先于该代次的第一条 packet 被下游观察到。由于独立 Edge 之间没有全局
顺序保证，不能简单地把 `StreamDescriptorMessage` 和 `PacketMessage` 放在两个并行
Edge 后假设描述一定先到。

首期推荐采用“启动前探测”方式：

1. PipelineController 让 Session 进入 Prepared 状态，打开输入但不开始读循环。
2. 读取 `MultiStreamInfo`，完成轨道选择、Decoder 和 Encoder 配置。
3. Encoder 准备完成后生成 Publisher 所需的输出轨道描述。
4. 构建并启动 Graph，下游全部就绪后最后激活 SourceNode。

后续需要在线变更时，再引入同一 FIFO 消息流中的 `variant<Descriptor, Packet, EOS>`，
或实现明确的控制面屏障。

## 5. 目标节点设计

### 5.1 StreamSourceNode

建议直接包装 `MediaStreamSession`，不要在新链路中再经过 `MediaStreamSource` 的
subscriber fan-out。MediaFlow 的 `OutputPort` 已经承担 fan-out、队列和背压，继续
叠加 subscriber 会产生重复分发层和不清晰的线程边界。

职责：

- 在开始读取前设置 StreamInfo、Packet、State 和 Event 回调。
- 将 session packet 包装为 `PacketMessage`，补充流 ID、轨道、代次和序号。
- 只接受 PipelineBuilder 选中的输入轨道，或把所有轨道交给 TrackRouterNode。
- Stop 时先递增节点代次并关闭 Emit 闸门，再解绑回调，最后停止 Session。
- 迟到回调必须因为代次不匹配而被丢弃。
- 不在 session 回调内执行解码、发布或长时间日志 I/O。

建议生命周期：

| 阶段 | 行为 |
|---|---|
| 构造 | 保存 session、流 ID 和选轨配置，不启动 I/O |
| `Init` | 校验 Prepared session 和 StreamInfo，注册回调 |
| `Start` | 激活读循环，Source 必须是最后启动的业务节点 |
| `Stop` | 关闭 Emit、解绑回调、停止读循环和定时器 |
| `Deinit` | 释放 session；确保不存在可触达节点的回调 |

Source 的阻塞 `ReadPacket()` 应运行在专用输入线程或专用输入 Executor，不应占用
decode、publish 或 Pipeline 控制线程。

### 5.2 TrackRouterNode

当前 FFmpegPuller 会缓存所有音视频 stream，并输出这些 stream 的 packet，但
`MultiStreamInfo::video_stream_idx_` 和 `audio_stream_idx_` 只记录最后一个对应类型。
只按 `MediaType` 路由会把多个同类型 stream 送进同一个 Decoder。

因此需要二选一：

1. 在 Puller 层增加显式选轨，只输出配置中的 stream index；首期推荐。
2. 新增 TrackRouterNode，按 `source_track_id` 路由到不同命名端口。

DecoderNode 必须再次校验 `source_track_id`，不能只检查 VIDEO/AUDIO。

### 5.3 DecoderNode

首期一个 DecoderNode 只负责一个输入轨道和一个 `IDecoder` 实例。

职责：

- `Init` 中按不可变 `MediaStreamInfo` 打开 Decoder。
- 使用 Serialized Executor 调用 `Decode()`。
- FrameCallback 只把 frame 包装为 `FrameMessage` 并 Emit。
- packet 的轨道或 generation 不匹配时拒绝处理并计数。
- 解码错误按配置上报 `RestartNode` 或 `StopPipeline`，不在节点线程直接停止 Graph。
- Graceful Stop 时先等待最后一个 Decode 结束，再 Flush，Emit 残留 frame，最后 Close。

重连处理：

- StreamInfo 未变化：可以清理解码状态并从下一关键帧恢复。
- codec、extradata、分辨率或音频参数变化：停止当前 Pipeline 代次并重建 Decoder。
- 首期不建议在运行中的 DecoderNode 内原地切换所有参数；重建整路 Graph 更容易
  保证 Publisher 和其他分支同步更新。

### 5.4 EncoderNode

首期一个 EncoderNode 只负责编码一个输出轨道。

职责：

- 在 `Init` 中打开 Encoder，并保存实际输出 time base、codec、extradata 和媒体参数。
- 将 `MediaFrame::time.pts_us` 重标定到编码器 time base 后送给 FFmpeg。
- 遍历一次 Encode 产生的全部 packet，并逐个 Emit。
- 为每个输出 packet 填写 `stream_index` 或独立的输出 track、`time_base`、duration、
  media type、codec 和 keyframe。
- Graceful Stop 时调用显式 Flush，Emit 所有 packet 后再 Close。
- 编码器实例和 audio FIFO 只能在 Serialized Executor 上访问。

EncoderNode 不应依赖 `dynamic_cast<FFmpegEncoder*>` 获取 extradata。应由 `IEncoder`
暴露统一输出描述，例如：

```cpp
struct EncodedTrackInfo {
    MediaType media_type{MediaType::UNKNOWN};
    CodecType codec_type{CodecType::UNKNOWN};
    Rational time_base{1, 1000000};
    int width{0};
    int height{0};
    int sample_rate{0};
    int channels{0};
    std::vector<std::uint8_t> extra_data;
};
```

### 5.5 PublisherSinkNode

一个 PublisherSinkNode 对应一个 Publisher 任务，可以接收一个或多个输出轨道。

职责：

- 在所有必需的 `EncodedTrackInfo` 准备完成后创建并启动 Publisher。
- 把 `PacketMessage::output_track_id` 映射为 Publisher `track_id`。
- 只在独立 Serialized publish Executor 上调用 Start、Publish 和 Stop。
- 把 `PublisherResult` 转换为 PipelineEvent 和节点指标。
- 区分“packet 被策略丢弃”“Publisher 仍已连接”“需要重连”和“Publisher 已关闭”。
- Graceful Stop 时等待编码器 flush packet 全部写完，再写 trailer/关闭 Publisher。

对于当前只接受 `MediaPacket` 的接口，可以在 PublisherSinkNode 内复制 packet 头：

```cpp
MediaPacket routed = *message.packet;
routed.stream_index = message.context.output_track_id;
publisher->Publish(routed);
```

该操作只复制元数据和 `shared_ptr`，不会复制编码载荷。长期建议让 Publisher API
显式接受逻辑 track，彻底取消 `stream_index == track_id` 的兼容假设。

### 5.6 JitterBufferNode

抖动缓冲处理网络到达抖动和乱序，MediaFlow Edge 处理处理速度不匹配，两者职责不同。
不应因为有 Edge 队列就直接删除抖动缓冲，也不应把二者配置成两个大缓存。

当前 `AdaptiveJitterBuffer` 不建议直接用于首期多轨链路：

- 一个实例混合音频和视频 DTS，迟到判断也是全局的。
- 定时器每次只 Pop 一条 packet，输入速率高于定时频率时会持续积压。
- 时间戳计算默认按微秒，未使用 packet `time_base`。
- Reset 会清零丢包累计，不利于 Pipeline 长期指标。
- Reset、Push、Pop 和查询在当前 Session 多线程模型下缺少完整生命周期屏障。

首期可对 FFmpeg RTSP 输入关闭该缓冲，依赖 demuxer 已有顺序；需要 AVTP/RTP 重排时，
应改为每轨一个 JitterBufferNode，并支持一次批量弹出全部 ready packet。

## 6. 生命周期与线程模型

### 6.1 当前 MediaFlow 的集成阻塞项

这些问题位于 MediaFlow 核心，不属于 `media` 文件，但必须与媒体节点同步解决：

| 问题 | 当前行为 | 媒体风险 | 要求 |
|---|---|---|---|
| Node Start 顺序 | 按 AddNode 顺序 | Source 可能在 Decoder/Publisher 未启动时产生数据 | 支持拓扑逆序启动，或首期严格按 sink 到 source 添加节点 |
| Node Stop 顺序 | 按 AddNode 顺序调用 Stop | Publisher 可能早于 Encoder flush 关闭 | 明确 source 到 sink 的 Graceful Stop 顺序 |
| 在途任务 | Node::Stop 时任务可能仍在执行 | Decode 与 Close、Encode 与 Close 并发 | 增加 pending-task barrier，并在节点 Executor 上执行 flush/close |
| Executor Stop | `io_context::stop()` 丢弃排队任务 | 残留 frame/packet 丢失 | Graceful 模式先 drain，再停止 Executor |
| 共享 Executor | Graph::Stop 会停止其见到的 Executor | 一路停止可能影响其他 Graph | 拆分 Executor 所有权，M1 前不要跨 Graph 共享 |
| 节点致命错误 | 无独立控制面停止入口 | 节点线程直接 Stop 可能自 join | 事件上报到 PipelineController，由控制线程执行停止 |
| ExecutorDispatch | Send 先记 Accepted，目标 Dispatch 失败不可回传 | 发送方看不到实际拒绝 | 媒体主链路首期使用 QueueTransport |

### 6.2 推荐启动顺序

```text
1. PipelineController 创建并 Prepare 输入会话
2. 读取 StreamInfo，完成选轨和参数校验
3. 准备 Decoder 和 Encoder，取得输出轨道描述
4. 创建 PublisherConfig
5. 打开 Edge 和节点 Dispatch
6. 启动 Executor
7. 启动 PublisherSinkNode
8. 启动 EncoderNode
9. 启动 DecoderNode
10. 最后启动 StreamSourceNode
```

现有 `MediaStreamSession::Start()` 同时执行 Open 和启动读循环，需要拆成类似：

```cpp
bool Prepare();       // Open + GetStreamInfo，不开始 ReadPacket 循环
bool StartReading();  // Prepared -> Running
void Stop();          // 对所有非停止状态有效
```

原 `Start()` 可以保留为兼容入口，内部依次调用 `Prepare()` 和 `StartReading()`。

### 6.3 推荐 Graceful Stop 顺序

```text
1. PipelineController 把状态切到 Stopping，拒绝新的外部操作
2. StreamSourceNode 关闭 Emit 闸门并停止输入
3. 等待 source -> decoder Edge 和 Decoder 在途任务排空
4. DecoderNode Flush，Emit 残留 frame
5. 等待 frame Edge 排空
6. EncoderNode Flush，Emit 残留 packet
7. 等待 publisher Edge 排空
8. PublisherSinkNode Stop，写 trailer 并关闭连接
9. 关闭 Edge 和节点 Dispatch
10. 停止 Executor
11. 按逆序 Deinit 节点
```

Immediate Stop 可直接关闭入口、取消任务并丢弃队列，但必须与 Graceful Stop 使用不同
枚举和指标，不能让调用方猜测本次停止是否保证输出完整。

### 6.4 Executor 建议

| Executor | 建议线程数 | 节点 | 说明 |
|---|---:|---|---|
| input | 每路 1 个阻塞读取任务，线程数按路数规划 | StreamSourceNode | `ReadPacket` 可能阻塞，不能占用控制线程 |
| decode | 1 至 CPU 核数 | DecoderNode | 每个 DecoderNode Serialized，不同轨道可并行 |
| process | 按推理/滤镜能力规划 | 后续处理节点 | 与编解码隔离 |
| encode | 1 至 CPU 核数或硬件队列数 | EncoderNode | 同一 Encoder 实例 Serialized |
| publish | 每个慢同步 Publisher 至少独立调度 | PublisherSinkNode | FFmpeg 写入和重连会同步阻塞 |
| control | 1 | PipelineController | 处理事件、重建和 Stop 请求 |

## 7. media 必须修改的问题

### 7.1 P0：接入前必须完成

| ID | 文件 | 当前问题 | 必须修改 | 验收 |
|---|---|---|---|---|
| M-P0-01 | `include/media/media_packet.h` | `MediaType::VIDEO` 和 `MediaType::UNKNOWN` 都为 0 | 将 `UNKNOWN=0` 放首位，其余值显式且唯一；检查持久化/FFI 依赖 | static_assert 各枚举值不同；发布轨道匹配测试通过 |
| M-P0-02 | `include/media/media_packet.h` | PTS 注释与 `time_base` 实际语义冲突 | 明确 tick/time_base 契约、无时间戳表示和校验工具；Dump 判空 | 非微秒 time base 的解码、编码、发布测试通过 |
| M-P0-03 | `include/media/puller/i_puller.h` 及各 Puller | bool + nullptr 不能可靠区分 NoData、EOF、可恢复错误和停止 | 增加结构化 `PullReadStatus`，Session 按状态决定继续、完成或重连 | 本地文件 EOF 不重连；网络断开按策略重连；超时不误判 EOF |
| M-P0-04 | `src/media/puller/ffmpeg_puller.cpp` | 重复 Open 不先 Close；所有音视频 stream 都输出，但只有单个 video/audio 索引 | Open 幂等清理；增加显式选轨或完整轨道映射；事件返回结构化错误 | 多视频轨不串入同一 Decoder；Start/Stop/Start 资源稳定 |
| M-P0-05 | `include/media/stream/stream_session.h`、`src/media/stream/stream_session.cpp` | Stop 无法中断 CONNECTING；旧 handler 可进入新一代；多线程字段有数据竞争；StreamInfo 回调未快照；Stats 实际不更新 | 增加 lifecycle generation、所有状态可停止、回调快照、线程边界和真实原子统计；拆分 Prepare/StartReading | 连接中停止、快速重启、断线重连和统计测试通过，无迟到 packet |
| M-P0-06 | `include/media/decoder/i_decoder.h`、`src/media/decoder/ffmpeg_decoder.cpp` | Open 不清理旧 context；Close 隐式 flush；Graph 停止时可能与 Decode 并发 | 增加显式 Flush；Open 先关闭；Close 只释放或明确契约；节点 Executor 屏障后调用 | 重复 Open、flush 多帧、Stop during Decode 无崩溃和丢帧 |
| M-P0-07 | `include/media/encoder/i_encoder.h`、`src/media/encoder/ffmpeg_encoder.cpp` | Frame 微秒 PTS 直接写入任意 codec time base；自动视频 time base 未考虑 fps_den；音频 FIFO PTS 固定按微秒累加；输出 packet 缺少 time_base 和 track | 统一重标定；修正 fps time base；按 codec time base 累加音频；补完整 packet 元数据和统一输出描述 | 删除测试中的手工 time_base/stream_index 后仍可播放且 A/V 同步 |
| M-P0-08 | `src/media/protocol/ffmpeg_mux_protocol.cpp` | 多轨使用 `av_write_frame`，独立音视频节点到达顺序不保证 | 使用 `av_interleaved_write_frame`，或实现有界 DTS 重排；保留单轨回归 | 独立音视频 Encoder 并发输入时无非单调 DTS 错误，输出可播放 |
| M-P0-09 | `src/media/publisher/default_publisher.cpp`、FFmpeg publish 路径 | 重连成功但等待关键帧时仍返回连接中断错误，DefaultPublisher 随即把 `started_` 置 false，后续关键帧无法恢复 | 区分 AwaitingKeyframe、RetryableFailure 和 Closed；只有 Publisher 真正关闭时清除 started | 模拟断线，先送非关键帧再送关键帧能够恢复 |
| M-P0-10 | `src/media/protocol/rtsp_server_protocol_adapter.cpp` | 多个相同媒体类型/codec 的 track 回退时直接取第一个候选 | 与 FFmpeg adapter 一致，候选不唯一时拒绝并要求显式 track | 双 H264 或双 AAC 轨道不会静默写错 |
| MF-P0-01 | `include/mediaflow/core/graph.h`、`executor.h` | 缺少媒体 Graceful Stop 屏障和安全启动/停止顺序 | 增加拓扑生命周期顺序、在途任务等待和 Executor 上 flush；禁止控制线程自 join | 编解码处理中停止、重复启动、启动失败回滚测试通过 |

### 7.2 P1：主链路完成前建议完成

| ID | 文件 | 问题与改进 |
|---|---|---|
| M-P1-01 | `include/media/media_frame.h` | 增加时间戳有效性；`Stride`、`PlaneOffset` 校验 index；避免 type 与 variant 不一致时直接 `std::get` 抛异常 |
| M-P1-02 | `include/media/i_media_buffer.h` | 推进只读 Buffer 契约，增加 `ConstPacketPtr`/`ConstFramePtr`；需要写入的节点显式复制或独占 |
| M-P1-03 | `include/media/stream/source_config.h` | 合并重复超时字段；处理或删除当前未生效的 `max_delay_ms`、`dump_packets`、headers、socket buffer 等配置 |
| M-P1-04 | `src/media/stream/stream_source.cpp` | 若保留兼容类：回调改 weak capture、Stop 解绑回调、subscriber 支持取消、锁外调用回调、保护 StreamInfo、Stop 统计线程、packet buffer 判空 |
| M-P1-05 | `AdaptiveJitterBuffer` | 改为每轨实例、按 time base 计算、批量 Pop、累计与当前代次指标分离；或迁移为独立 JitterBufferNode |
| M-P1-06 | `DefaultPublisher` | 查询方法与 Start/Publish/Stop 并发不安全；增加锁或由 PublisherSinkNode 保存线程安全快照 |
| M-P1-07 | `RtspServerProtocol` | `Write` 成功只表示已 post，内部媒体任务没有独立总上限；增加有界媒体入口和 accepted/delivered/dropped 区分 |
| M-P1-08 | `FfmpegMuxProtocol` | 断线发生在音频 packet 时，也应根据配置中是否存在视频轨决定是否等待关键帧 |
| M-P1-09 | `PublisherResult` | 增加 recoverable、connection state、packet disposition，避免节点通过错误码字符串推断状态 |
| M-P1-10 | `MultiStreamInfo` | 提供按 stream index 查找和显式 selected tracks；减少“vector 下标”和“源 stream 编号”混淆 |
| MF-P1-01 | MediaFlow metrics | 增加节点业务错误、处理时延、最后错误、生命周期状态和 Pipeline 级统计 |
| MF-P1-02 | Queue/Dispatch | 当前 Edge 队列与 Node pending tasks 形成双层缓存；配置和指标应展示总在途数量，后续增加按字节上限 |

### 7.3 P2：性能和扩展优化

| 文件/区域 | 建议 |
|---|---|
| `FFmpegFrameBuffer` | 当前同时保留 AVFrame 并复制 packed buffer，内存接近双份；按消费者能力增加零拷贝 frame view，必要时再 pack |
| `BackendHandle` | 用带类型和共享所有权的后端引用替代公开 `void*`，减少 buffer 与裸指针不一致 |
| Encoder | 增加动态码率、请求关键帧和能力查询接口，支持拥塞恢复 |
| Publisher | 增加主备 URL、指数退避、熔断、健康检查和有界 QueuedPublisher |
| Transport | 增加按字节预算、按时间预算和关键帧感知的背压策略 |
| Pipeline | 支持多个 Graph 共享受控 Executor，但 Executor 生命周期由 Manager 管理 |

## 8. 文件级修改清单

| 文件或目录 | 结论 | 具体动作 |
|---|---|---|
| `include/media/media_packet.h` | 必须修改 | 修复枚举值；明确时间基；增加安全校验和空 Buffer Dump |
| `include/media/media_frame.h` | 建议修改 | 补时间有效性、轨道关联由消息上下文承载、访问器边界检查 |
| `include/media/i_media_buffer.h` | 建议修改 | 建立只读共享契约，保留兼容写接口并逐步收敛 |
| `include/media/puller/i_puller.h` | 必须修改 | 结构化 Read 结果、错误类别和 EOS |
| `src/media/puller/ffmpeg_puller.cpp` | 必须修改 | 幂等 Open、选轨、读结果、事件和多轨信息 |
| `src/media/puller/avtp_puller.cpp` | 小幅修改 | 适配结构化 Read 结果，保留 parser/assembler 和统计实现 |
| `src/media/puller/ethernet_capture.cpp` | 暂无需节点化 | 继续作为 AvtpPuller 内部组件；只补充停止/超时结果映射 |
| `include/media/stream/source_config.h` | 建议修改 | 删除未生效配置或完成下发，避免 UI/配置文件产生虚假控制感 |
| `include/media/stream/stream_session.h` | 必须修改 | Prepared/Running 状态、generation、统计和回调解绑 |
| `src/media/stream/stream_session.cpp` | 必须修改 | 修复并发、Stop、重连、回调快照、旧任务和统计 |
| `stream_source.*` | 兼容性修复 | 新节点不依赖 subscriber；旧测试仍使用时必须修复引用环和锁内回调 |
| `jitterbuffer/*` | 首期可禁用，后续修改 | 每轨化、批量出队、time base 和指标 |
| `include/media/decoder/i_decoder.h` | 必须修改 | 增加 Flush/状态契约，输入逐步 const 化 |
| `src/media/decoder/ffmpeg_decoder.cpp` | 必须修改 | Open 幂等、Flush 与 Close 分离、时间戳有效性 |
| `include/media/encoder/i_encoder.h` | 必须修改 | 增加 Flush 和 EncodedTrackInfo；定义输出时间基 |
| `src/media/encoder/ffmpeg_encoder.cpp` | 必须修改 | 修正视频/音频 PTS，填充 packet 元数据和 duration fallback |
| `include/media/publisher/publisher_result.h` | 必须修改 | 表达 packet disposition、可恢复性和连接是否仍有效 |
| `src/media/publisher/default_publisher.cpp` | 必须修改 | 修复等待关键帧导致永久 stopped；补线程安全快照 |
| `src/media/protocol/ffmpeg_protocol_adapter.cpp` | 小幅修改 | 优先接收 PublisherNode 显式输出 track，兼容路径保留测试 |
| `src/media/protocol/ffmpeg_mux_protocol.cpp` | 必须修改 | 多轨 interleave、重连状态和指标语义 |
| `src/media/protocol/rtsp_server_protocol_adapter.cpp` | 必须修改 | 多候选唯一性、显式 track 映射 |
| `src/media/protocol/rtsp_server_protocol.cpp` | 建议修改 | 有界媒体任务、drain/stop 语义和 delivered 指标 |
| `media/protocol/rtsp/*` | 原则上无需重构 | 保留 connection/session/transport 边界，只补上层队列要求触发的窄接口 |
| `media/protocol/avtp/*` | 无需迁移为节点 | 保留纯 parser/assembler/timestamp mapper，节点只包装 Puller 输出 |

## 9. 需要新增的 MediaFlow 文件

推荐目录：

```text
include/mediaflow/
  messages.h
  nodes/
    stream_source_node.h
    track_router_node.h
    decoder_node.h
    encoder_node.h
    publisher_sink_node.h

src/mediaflow/
  nodes/
    stream_source_node.cpp
    track_router_node.cpp
    decoder_node.cpp
    encoder_node.cpp
    publisher_sink_node.cpp

test/mediaflow/
  test_media_messages.cpp
  test_stream_source_node.cpp
  test_decoder_node.cpp
  test_encoder_node.cpp
  test_publisher_sink_node.cpp
  test_media_pipeline.cpp
```

所有新增 C++ 类型只使用：

```cpp
namespace mediaflow {
// ...
}
```

当前 `video_pipeline_mediaflow` 是只包含核心头文件的 INTERFACE target。媒体适配节点
依赖 FFmpeg 和 `media` 实现，其 `.cpp` 首期继续进入 `video_pipeline_lib`，并链接
`video-pipeline::mediaflow`。不要让通用 MediaFlow 核心 target 反向依赖全部媒体库。

## 10. 背压配置建议

不能给所有 Edge 使用统一的 `capacity=64 + DropNewest`。容量应结合消息大小、帧率、
允许延迟和 codec 依赖关系设置。

| Edge | 实时链路初始建议 | 文件处理建议 | 注意事项 |
|---|---|---|---|
| Source -> Decoder，视频 packet | 32 至 128，优先有界 Block 或 DropNewest + 等待关键帧恢复 | Block | 任意丢压缩 packet 可能破坏当前 GOP |
| Source -> Decoder，音频 packet | 64 至 256，Block 或有明确时限的丢弃 | Block | 音频不能简单 DropOldest 后继续假装时间连续 |
| Decoder -> 实时预览/推理 | 2 至 4，DropOldest | Block | 低延迟场景保留最新 frame |
| Decoder -> Encoder | 4 至 16，按业务决定 DropOldest 或 Block | Block | 丢 frame 后 Encoder 仍需连续 PTS |
| Encoder -> Publisher，视频 | 8 至 32，关键帧感知丢弃 | Block | 当前三种通用策略都不能完整表达 GOP 恢复 |
| Encoder -> Publisher，音频 | 32 至 128，优先 Block/限时 Block | Block | 发生丢弃时记录时间缺口 |
| 控制消息 | 8 至 32，Block | Block | StreamInfo、EOS、错误事件不得静默丢失 |

额外约束：

- `EdgeOptions.capacity` 和 `NodeOptions.max_pending_tasks` 会共同形成在途上限。
- 解码后 4K frame 很大，容量应按估算字节数复核，不能只看消息条数。
- Source 回调若使用 Block，必须有独立输入线程，且 Transport Close 能唤醒等待。
- FFmpeg Publisher 重连会占用 publish Executor 数秒，应观察 publish Edge 高水位。
- RTSP Server 内部 post 队列也要有上限，否则外部 Edge 有界仍不能限制全部内存。

## 11. 错误与恢复策略

建议每个节点声明固定失败策略：

| 场景 | 建议策略 |
|---|---|
| 单个坏 packet | 记录并忽略；连续达到阈值后重建 Decoder |
| Source 暂时 NoData | 继续读取，不增加重连次数 |
| Source 网络断开 | Session 内按退避重连；成功后递增 generation |
| 本地文件 EOS | 发送 EOS，走 Graceful Stop，不重连 |
| StreamInfo 未变化的重连 | 清理解码状态，从关键帧恢复 |
| StreamInfo 变化 | 停止并重建整路 Graph，首期不在线热切换 |
| Encoder 单帧失败 | 上报 FatalError，避免继续输出不可预测码流 |
| Publisher 等待关键帧 | packet disposition 记为 Dropped，Publisher 保持 started |
| Publisher 重连失败 | StopBranch 或 StopPipeline，由配置决定 |
| RTSP 单个慢客户端 | 关闭该客户端，不停止 Publisher 和其他客户端 |

PipelineEvent 至少包含：节点 ID、流 ID、输入/输出轨道、generation、错误码、原生错误、
是否可恢复和建议动作。日志文本不能成为控制逻辑输入。

## 12. 分阶段实施顺序

### A0：修复公共契约

改动：

- 修复 `MediaType` 枚举冲突。
- 明确 packet 时间基和无时间戳语义。
- 增加 PullReadStatus。
- 修复 Session generation、Stop、回调和统计。
- 为 Decoder/Encoder 增加 Flush 与输出描述。

验收：相关纯单元测试全部通过，现有 media 测试编译通过。

### A1：实现 Source + Decoder 单视频链路

改动：

- 实现 StreamSourceNode、TrackRouterNode 和 DecoderNode。
- 使用启动前 Prepare，保证 Decoder 先于 Source 就绪。
- 建立 RTSP/本地文件到解码 frame 的图。

验收：

- 不再存在“先 Start Source，再注册 subscriber”的启动丢包窗口。
- 本地文件 EOS 正常结束。
- RTSP 断线重连后旧 generation 数据不会进入新 Decoder。

### A2：实现 Encoder + 单视频 Publisher

改动：

- 实现 EncoderNode 和 PublisherSinkNode。
- 修复 encoder PTS/time base/output track。
- 修复 Publisher 等待关键帧的结果语义。
- 建立 MP4/RTSP -> Decode -> Encode -> RTSP/RTMP。

验收：现有测试不再手工设置 packet `stream_index`、`time_base` 和正常 duration；输出
可由 ffplay/VLC 播放 30 分钟。

### A3：音视频多轨和 Graceful Stop

改动：

- 音视频使用独立 DecoderNode 和 EncoderNode。
- PublisherSinkNode 汇合多轨。
- FFmpeg mux 使用 interleaved write。
- 完成 Graph pending barrier 和逐级 flush。

验收：A/V 同步、无非单调时间戳、停止后尾部音视频完整、无 Decode/Close 并发。

### A4：分支、推理和高级背压

改动：

- Frame fan-out 到推理、渲染、OSD 或录像。
- 增加关键帧感知发布队列、按字节预算和端到端延迟指标。
- 增加多路 Pipeline 管理和受控共享 Executor。

验收：慢推理或慢 Publisher 时内存有界，其他 Pipeline 不受单路故障影响。

## 13. 测试指导

### 13.1 media 单元测试

- `MediaType` 每个值唯一，UNKNOWN 不再等于 VIDEO。
- 任意合法 `Rational` 的时间戳往返重标定误差可控。
- `FFmpegPuller::Open -> Close -> Open` 重复 100 次无资源增长。
- Puller 能区分 NoData、EOS、RetryableError、FatalError 和 Stopped。
- FFmpeg 多视频 stream 显式选轨，只输出选中 stream。
- Session 在 CONNECTING、CONNECTED、RECONNECTING 中调用 Stop 都有限时返回。
- Session 快速 `Start/Stop/Start` 时旧 read/timer handler 不进入新 generation。
- Session Stats 的 bytes、packets、bitrate、reconnect 和 jitter drop 实际变化。
- Decoder 连续 Open 两种配置不泄漏，Flush 可输出残留帧。
- Encoder 使用 `1/25`、`1/90000`、`1/1000000` 等 time base 输出正确 PTS。
- 音频 FIFO 使用 `1/sample_rate` 和微秒 time base 时均保持连续。
- Encoder packet 自动带正确 track、time base、duration 和 extradata 描述。
- Publisher 断线后非关键帧被丢弃但保持可接收下一关键帧。
- RTSP adapter 对两个同 codec track 且缺少显式 track 的 packet 返回歧义错误。
- FFmpeg 多轨乱序到达时正确交织并输出可播放文件。

### 13.2 MediaFlow 节点测试

- SourceNode Stop 后即使 session 触发迟到回调，也不再 Emit。
- DecoderNode 收到错误 track/generation 时拒绝并计数。
- DecoderNode FrameCallback 一次输出多 frame 时全部进入下游。
- EncoderNode 一帧输出多 packet 时全部保序发送。
- PublisherSinkNode 只在必需 track 信息完整后 Start。
- PublisherSinkNode 所有 IPublisher 调用都发生在同一 Serialized 节点上下文。
- 节点异常只上报事件，不在节点 Executor 内直接停止 Graph。
- Graceful Stop 等待在途任务并按 Decoder -> Encoder -> Publisher 顺序 flush。

### 13.3 集成测试迁移

| 当前测试 | 建议处理 |
|---|---|
| `test/media/test_stream_decode.cpp` | 改为 SourceNode -> DecoderNode；当前先启动 Source 后注册 subscriber，会丢启动 packet |
| `test/media/test_local_mp4_decode_rtsp_publisher.cpp` | 保留媒体素材和验证客户端，替换手工状态机为 Graph |
| `test/media/test_rtsp_decode_encode_push_zlm.cpp` | 替换手工 Encoder/Publisher 串联，删除 packet time base 补丁 |
| `test/media/avtp_decode_rtsp_publisher.cpp` | A3 后接入 SourceNode，多轨音频/视频分别路由 |
| `test/media/test_rtsp_server_publisher.cpp` | 保留协议级测试；另增加 PublisherSinkNode 级测试，不把两者合并 |
| `test/media/test_publisher_protocol.cpp` | 增加重连等待关键帧、明确 track 和多候选拒绝测试 |
| `test/mediaflow/test_mediaflow.cpp` | 增加拓扑生命周期顺序、pending barrier 和控制线程停止测试 |

### 13.4 压力与故障测试

- 1080p 和 4K 连续运行 1 小时，记录 Edge/Node 高水位和进程内存。
- Publisher 每次阻塞至 I/O timeout，验证 source/decode 不被同一线程拖住。
- 模拟 ZLMediaKit 重启，验证重连后从关键帧恢复。
- 输入每 5 秒断开一次，连续 100 次，检查线程、timer、socket 和 generation。
- 在 Decode、Encode、Publish 正在执行时分别触发 Immediate/Graceful Stop。
- 两路及以上 Pipeline 并行，一路重建不能停止另一条 Executor。
- 音视频输入乱序和不同起始 PTS，检查 mux 输出 DTS 单调与 A/V 同步。

## 14. Windows 构建与验证

使用现有 Visual Studio 环境和工程当前 CMake 配置。构建示例：

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

实施时应先运行纯本地、无外部服务依赖的测试，再运行摄像头、AVTP 和 ZLMediaKit
集成测试。所有网络测试必须设置有限超时，避免停止回归因外部环境永久阻塞。

## 15. 验收标准

| 项目 | 最低标准 |
|---|---|
| 编译 | Windows Debug/Release 均通过，新增代码无未处理警告 |
| 启动 | Source 最后激活，第一条 packet 前下游已经就绪 |
| 停止 | Immediate 有限时返回；Graceful 输出 Decoder/Encoder 残留数据 |
| 重启 | 同一 Graph 或整路 Pipeline 重启无旧 generation 数据 |
| 时间戳 | 不需要测试手工修补；多轨无非单调 DTS，A/V 可同步播放 |
| 背压 | 所有队列有界，满队列行为和丢弃数量可查询 |
| 错误 | 能区分 EOS、NoData、可恢复网络错误、坏 packet 和致命配置错误 |
| 发布恢复 | 断线后等待关键帧不会让 Publisher 永久进入未启动状态 |
| 多轨 | 同 codec 多轨缺少显式 track 时拒绝，不静默写错 |
| 线程安全 | 有状态媒体对象仅由所属 Serialized 节点调用；查询使用快照 |
| 内存 | 长时间运行不随处理速度差持续增长，4K 队列内存符合预算 |
| 可观测性 | 可查询 Pipeline、Node、Edge、Source、Decoder、Encoder、Publisher 指标 |

## 16. 最终实施建议

建议按以下提交边界推进，便于回归和问题定位：

1. `fix(media): clarify media identity and timestamp contracts`
2. `fix(media): harden stream session lifecycle and read results`
3. `fix(media): add decoder and encoder flush contracts`
4. `fix(media): complete encoder output metadata and mux interleave`
5. `fix(media): correct publisher reconnect and track routing semantics`
6. `feat(mediaflow): add media messages and source/decoder nodes`
7. `feat(mediaflow): add encoder and publisher nodes`
8. `feat(mediaflow): add graceful media pipeline shutdown`
9. `test(mediaflow): migrate media end-to-end pipelines`

第一批实现不应同时接入推理、渲染和动态拓扑。先让
`Source -> Decode -> Encode -> Publish` 具备明确的轨道、时间、代次、背压、错误和
停止契约，再扩展业务分支。这样 MediaFlow 才真正接管流程编排，而不是只在现有回调
链外再包一层类。

## 17. A0 实施记录

### 17.1 已完成修改

本节只记录本次实际落地的 A0 修改。尚未实现的多轨发布、关键帧恢复和图级有序
停止仍保留在后续阶段，不视为本次完成项。

| 领域 | 实际修改 | 主要文件 |
|---|---|---|
| 媒体类型 | 将 `MediaType` 改为显式唯一值：`UNKNOWN=0`、`VIDEO=1`、`AUDIO=2`、`PCAP=3`，消除未知类型与视频冲突 | `include/media/media_packet.h` |
| 时间戳契约 | 增加 `kNoTimestamp`、时间戳有效性和时间基校验；`MediaPacket` 时间戳统一解释为 `time_base` tick，`MediaFrame` 继续使用微秒 | `include/media/media_packet.h`、`include/media/media_frame.h` |
| FFmpeg 拉流 | `Open()` 重复调用前先关闭旧上下文；输出 packet 保留 demuxer 原始时间基；读取结果区分 `EOS`、暂时错误、致命错误和主动停止 | `src/media/puller/ffmpeg_puller.cpp` |
| AVTP 拉流 | 保留旧 `bool ReadPacket()`，新增结构化读取结果适配，区分无数据、可重试读取和已关闭 | `include/media/puller/i_puller.h`、`include/media/puller/avtp_puller.h`、`src/media/puller/avtp_puller.cpp` |
| Puller 公共接口 | 增加 `PullReadStatus` 和 `PullReadResult`；旧接口继续保留，避免现有手工链路一次性迁移 | `include/media/puller/i_puller.h` |
| Session 生命周期 | 为 Start/Stop/重连/read/timer handler 引入 generation；CONNECTING 可停止；流信息回调改为锁内快照；旧代次回调不能进入新代次 | `include/media/stream/stream_session.h`、`src/media/stream/stream_session.cpp` |
| Session 统计 | 实际累计字节、包数、码率、重连次数和 jitter 丢弃数；查询返回受锁保护的快照 | `include/media/stream/stream_session.h`、`src/media/stream/stream_session.cpp` |
| Decoder 生命周期 | 增加显式 `Flush()`；`Open()` 先关闭旧上下文；`Close()` 不再隐式回调残留帧；输入 packet 的来源时间基不同时显式重标定 | `include/media/decoder/i_decoder.h`、`include/media/decoder/ffmpeg_decoder.h`、`src/media/decoder/ffmpeg_decoder.cpp` |
| Encoder 生命周期 | 增加显式 `Flush()` 和 `EncodedTrackInfo`；`Close()` 不再静默丢弃 flush packet | `include/media/encoder/i_encoder.h`、`include/media/encoder/ffmpeg_encoder.h`、`src/media/encoder/ffmpeg_encoder.cpp` |
| Encoder 时间与元数据 | 修正视频帧率时间基、Frame 微秒 PTS 到 codec time base 的换算、音频 FIFO PTS 累加；输出 packet 自动填写 stream、time base、duration 和 keyframe | `src/media/encoder/ffmpeg_encoder.cpp` |
| Encoder 选择 | 优先使用 FFmpeg 默认 codec encoder，避免平台专用 AAC 编码器因采样率限制导致同一配置在不同机器上行为不同 | `src/media/encoder/ffmpeg_encoder.cpp` |
| Jitter 时间轴 | 无效时间戳不再参与排序；合法非微秒时间基先换算到微秒轴 | `src/media/stream/jitterbuffer/adaptive_jitter_buffer.cpp` |

### 17.2 验证结果

- 配置环境：Visual Studio 2026 Community，使用 `VsDevCmd.bat -arch=x64 -host_arch=x64` 初始化工具链。
- 构建：现有 `build` 目录的 Debug 目标完整构建通过，`video_pipeline_lib` 及全部测试目标均成功链接。
- 回归测试：`test_mediaflow`、`test_publisher_protocol`、`test_ffmpeg_audio_encoder`、`test_ffmpeg_decoder_raw_packet` 共 4 项通过。
- 扩展本地回归：排除外部流、摄像头、AVTP 网卡、图形窗口和推理模型依赖后，共 21 项测试通过。
- AAC 编码测试验证了 16 kHz 输入、flush 输出和 packet 元数据；原始 FFmpeg packet 解码测试验证了保留 stream time base 后仍可正常解码。
- 尚未执行依赖摄像头、网络服务、图形设备或 AVTP 网卡的集成测试，因此本次验证不覆盖外部服务故障恢复。

### 17.3 尚未完成项

以下内容仍按原计划进入后续 P0/A1-A3，不应被当前 A0 记录替代：

| 项目 | 当前状态 | 后续阶段 |
|---|---|---|
| FFmpeg 多轨显式选轨和独立路由 | Puller 仍可输出多个同类型 stream，`MultiStreamInfo` 仍需完善 selected track 契约 | A1 |
| Publisher 重连后的等待关键帧状态 | 已增加 `AwaitingKeyframe` 结果；默认 Publisher 在该状态保持会话，失败重连仍区分为 `RuntimeDisconnected` | 已完成/A2 |
| RTSP 多轨唯一匹配 | 同 codec 多候选的显式 track 拒绝策略尚未完成 | A2/A3 |
| FFmpeg 多轨交织 | mux 的 interleaved write 和有界 DTS 重排尚未实施 | A3 |
| Graph Graceful Stop | MediaFlow 仍需 pending-task barrier、拓扑逆序停止和逐级 Flush 屏障 | A3 |
| SourceNode、DecoderNode、EncoderNode、PublisherSinkNode | A1 已完成 Source/Router/Decoder 节点和本地单视频图；Encoder/Publisher 尚未接入 | A2 |

### 17.4 A0 后的接入约束

后续节点实现必须直接使用本节已经落地的契约：读取循环按 `PullReadStatus` 分支，
节点代次由消息上下文携带，Decoder/Encoder 在停止屏障中显式 Flush，Publisher 使用
`EncodedTrackInfo` 和 packet 自带 `time_base`。测试中不得再通过手工覆盖 packet 的
`stream_index` 或 `time_base` 来掩盖适配层缺失。

## 18. A1 实施记录：Source + Decoder 单视频链路

### 18.1 已完成修改

本阶段完成 MediaFlow 到现有 `media` 拉流器和解码器的第一条可测试适配链路。
本阶段的目标是先固定节点边界、启动顺序和连接代次语义，不把 Encoder、Publisher
以及音视频汇合的停止屏障提前混入。

| 领域 | 实际修改 | 主要文件 |
|---|---|---|
| 媒体消息 | 增加 `MediaPacketMessage` 和 `MediaFrameMessage`，包/帧都携带 `generation`；包消息同时携带所属 `MediaStreamInfo` | `include/mediaflow/media_nodes.h` |
| 源节点 | 增加 `StreamSourceNode`，在独立读取线程中调用 `IPuller::ReadPacketResult()`，区分 Packet、NoData、EOS、RetryableError、FatalError 和 Stopped | `include/mediaflow/media_nodes.h`、`src/mediaflow/media_nodes.cpp` |
| 源节点重连 | 仅对 `RetryableError` 执行有限或无限重连；每次成功重连递增 generation，旧连接消息不会冒充新连接 | `src/mediaflow/media_nodes.cpp` |
| 轨道路由 | 增加 `TrackRouterNode`，按显式 `stream_index` 和可选 `CodecType` 选择单路视频，音频及其他视频轨不进入视频 Decoder | `include/mediaflow/media_nodes.h`、`src/mediaflow/media_nodes.cpp` |
| 解码节点 | 增加 `DecoderNode`，Init 阶段先安装帧回调；已有 StreamInfo 时提前 Open，只有连接后才能获知 StreamInfo 时在首个包到达前完成延迟 Open | `include/mediaflow/media_nodes.h`、`src/mediaflow/media_nodes.cpp` |
| 旧代次隔离 | Decoder 记录当前 generation；晚到的旧代次包直接丢弃；新代次到达时先关闭旧 decoder，再使用新轨道信息打开上下文 | `src/mediaflow/media_nodes.cpp` |
| Graph 启停顺序 | 增加 `StartPriority()`；普通节点先启动，Source 最后启动；停止时按相反优先级先停止 Source，减少首包竞争和停止期间继续生产 | `include/mediaflow/core/node.h`、`include/mediaflow/core/graph.h` |
| 构建与测试 | 新增本地脚本 Puller/Decoder 测试，覆盖音频旁路、重连、EOS、代次输出和旧包丢弃 | `test/mediaflow/test_mediaflow_media_nodes.cpp`、`test/CMakeLists.txt` |

### 18.2 当前推荐组图方式

```text
StreamSourceNode<MediaPacketMessage>
        -> TrackRouterNode(video)
        -> DecoderNode
        -> 下游 Frame 节点
```

`StreamSourceNode` 构造时接收现有工程的 `IPuller` 和 URL，因此可以使用
`FFmpegPuller` 处理本地文件、RTSP 等输入，也可以使用 AVTP Puller。节点不再要求
业务侧先 `Start()` 再添加 subscriber；Graph 会先启动 Router/Decoder/下游节点，
最后才打开 Puller 并读取第一条包。

当前 A1 采用独立读取线程直接调用 `IPuller`，没有复制或改写旧的
`MediaStreamSession` / `MediaStreamSource`。旧回调链仍可继续使用，后续迁移时应以
本节点链路作为新入口，并逐步移除手工 subscriber 编排。

### 18.3 验证结果

- 使用 `C:\Program Files\Microsoft Visual Studio\18\Community` 的 VS 2026 x64 工具链，重新配置并编译 `video_pipeline_lib` 和 `test_mediaflow_media_nodes`。
- `test_mediaflow_media_nodes` 通过：输出 generation 为 `1, 2`，首个连接和重连各打开一次 Decoder，音频包未进入视频 Decoder，EOS 后读取线程结束。
- 测试手工注入 generation=1 的晚到视频包，在 Decoder 已处理 generation=2 后未产生第三帧。
- 本阶段未执行真实 RTSP、摄像头或 AVTP 网卡测试；这些测试依赖外部服务/设备，不能由本地脚本测试替代。

### 18.4 尚未完成项与下一阶段入口

| 项目 | 当前状态 | 后续阶段 |
|---|---|---|
| Source 与现有 MediaStreamSession 统一 | A1 节点直接使用 IPuller，旧 Session 仍保留；Session 的 watchdog/jitter 配置尚未完整映射到节点 | A1 后续/A3 |
| 多轨 selected track 契约 | Router 支持显式 stream index，但 Source 尚未发布完整的选轨控制对象，多个同 codec 轨道仍需上层明确选择 | A3 |
| Graph EOS 传播 | Source 内部可识别 EOS，但当前 Graph 没有通用 EOS 控制消息，Decoder 不会因 EOS 自动 Flush | A3 |
| Decoder Flush 屏障 | `DecoderNode::Flush()` 已提供，但 Immediate Stop 不调用它，避免与未完成 Decode 并发；需等 Graph Graceful Stop barrier | A3 |
| Encoder/Publisher | A2 已完成 `EncoderNode` 和 `PublisherSinkNode` 的单视频节点链路；真实网络长时播放仍待外部环境验证 | A3/集成验证 |
| 现有媒体测试迁移 | `test_stream_decode`、AVTP 和 RTSP 发布测试仍是旧手工回调链 | A1 后续/A2 |

### 18.5 阶段结论

A1 的本地单视频 Source -> Router -> Decoder 图已经具备可编译、可运行和可重连的
最小实现，首包窗口和旧代次穿透问题已由节点边界处理。下一阶段入口为 A2：接入
`EncoderNode` 和 `PublisherSinkNode`，同时完善 Publisher 重连后的等待关键帧语义；
Graph 的完整 EOS/Flush/Graceful Stop 屏障继续留在 A3。

## 19. A2 实施记录：Encoder + 单视频 Publisher

### 19.1 已完成修改

本阶段在 A1 的 `MediaFrameMessage` 基础上完成单路视频的编码和发布适配。节点
不把 Publisher 启动放在 Graph 的静态 Start 阶段，而是等 Encoder 提供实际输出轨道
信息后再创建/启动 Publisher，避免使用错误的 time base、尺寸或 extradata 写 header。

| 领域 | 实际修改 | 主要文件 |
|---|---|---|
| 编码消息 | 增加 `EncodedPacketMessage`，携带编码包、`EncodedTrackInfo`、`track_id` 和 generation | `include/mediaflow/media_nodes.h` |
| Encoder 节点 | 增加 `EncoderNode`，Init 打开编码器，逐帧转发 Encode 返回的全部 packet，并把输出 stream_index 统一设置为 MediaFlow track_id | `include/mediaflow/media_nodes.h`、`src/mediaflow/media_nodes.cpp` |
| Encoder 重连代次 | 输入 generation 变化时关闭旧 Encoder 并重新 Open；旧代次 Frame 不进入新 Encoder；旧上下文不在换代时隐式 Flush | `src/mediaflow/media_nodes.cpp` |
| Encoder Flush | `EncoderNode::Flush()` 显式转发全部尾包，保留 Encoder 生成的 PTS、DTS、time_base 和 duration | `src/mediaflow/media_nodes.cpp` |
| 输出轨道 | `EncodedTrackInfo` 增加视频 fps，FFmpegEncoder 从实际配置填充 fps；Publisher 使用实际 time_base、尺寸和 extradata | `include/media/encoder/i_encoder.h`、`src/media/encoder/ffmpeg_encoder.cpp` |
| Publisher 节点 | 增加 `PublisherSinkNode`，收到完整编码轨道信息后生成/合并 `MediaTrackConfig`，再在 Serialized 节点上下文中 Start/Publish | `include/mediaflow/media_nodes.h`、`src/mediaflow/media_nodes.cpp` |
| 首次发布 | 默认先等待视频关键帧，避免从无法独立解码的 P/B 帧启动发布；音频不受该策略误阻塞 | `src/mediaflow/media_nodes.cpp` |
| 关键帧恢复 | 新 generation 或 Publisher 断线后等待关键帧；非关键帧丢弃，关键帧重新启动或恢复 Publisher | `src/mediaflow/media_nodes.cpp` |
| Publisher 结果语义 | 增加 `PublisherErrorCode::AwaitingKeyframe`；FFmpeg mux 重连后等待关键帧返回该状态，`DefaultPublisher` 不再把会话错误关闭 | `include/media/publisher/publisher_result.h`、`src/media/protocol/ffmpeg_mux_protocol.cpp`、`src/media/publisher/default_publisher.cpp` |
| 测试 | 扩展 MediaFlow 本地测试，覆盖一帧多 packet、track_id、轨道元数据、Flush 尾包和 AwaitingKeyframe 后续关键帧 | `test/mediaflow/test_mediaflow_media_nodes.cpp` |

### 19.2 当前推荐组图方式

```text
StreamSourceNode
        -> TrackRouterNode
        -> DecoderNode
        -> EncoderNode
        -> PublisherSinkNode
```

视频 Encoder 的 `EncoderNodeOptions::track_id` 必须与 Publisher 配置中的
`MediaTrackConfig::track_id` 对应。Publisher 配置可以预先提供 URL 和部分轨道字段，
但最终启动前必须能够从 Encoder 输出补齐有效的媒体类型、codec、尺寸、fps、time_base
和必要的 extradata。没有显式轨道配置时，A2 单视频节点会根据第一条编码包动态创建
一条轨道。

### 19.3 验证结果

- 使用 `C:\Program Files\Microsoft Visual Studio\18\Community` 的 VS 2026 x64 工具链编译 `video_pipeline_lib` 和 `test_mediaflow_media_nodes`。
- `test_mediaflow_media_nodes` 通过：Encoder 一帧输出多个 packet，Publisher 收到的 packet 顺序保持不变，packet 的 stream_index 为配置的 track_id。
- 测试验证 Publisher 首次丢弃非关键帧、关键帧启动发布、Flush 尾包继续发布；轨道 width、height、fps、time_base 和 extradata 均来自 Encoder 输出。
- 测试验证 `AwaitingKeyframe` 不会导致 Publisher 被标记为关闭，后续关键帧仍使用第一次启动的 Publisher。
- 联合回归通过：`test_mediaflow`、`test_mediaflow_media_nodes`、`test_ffmpeg_audio_encoder`、`test_ffmpeg_decoder_raw_packet`。
- 尚未执行真实 RTSP/RTMP 服务、ffplay/VLC 30 分钟长时播放或网络断线互操作测试；这些仍是 A2 集成验收项。

### 19.4 尚未完成项与下一阶段入口

| 项目 | 当前状态 | 后续阶段 |
|---|---|---|
| 真实 Publisher 断线重连 | 协议结果和节点状态已区分；真实远端断开后的重连、关键帧恢复仍需服务端验证 | A2 集成验证 |
| Graph EOS 传播 | Source 能识别 EOS，但没有通用 EOS 消息驱动 Encoder/Publisher 自动 Flush | A3 |
| Graph Graceful Stop | Encoder Flush API 已接入，仍没有 pending-task barrier，Immediate Stop 不自动 Flush | A3 |
| 音视频多轨 | 当前节点链路按单视频设计，PublisherSink 尚未汇合多轨并处理交织 | A3 |
| 现有 RTSP/RTMP 测试迁移 | 现有媒体端到端测试仍保留手工 Puller/Decoder/Encoder/Publisher 回调 | A3/集成验证 |

### 19.5 阶段结论

A2 的本地 `Source -> Decode -> Encode -> Publish` 单视频节点链路已具备完整的
消息代次、轨道元数据、编码多包转发、显式 Flush 和关键帧等待语义。下一阶段进入
A3：音视频多轨汇合、FFmpeg mux interleaved write，以及 Graph 的有序停止和逐级
Flush 屏障。

## 20. A3 实施记录：多轨汇合与有序停止

### 20.1 阶段目标

A3 将 A2 的单视频链路扩展为可承载音频和视频的完整 MediaFlow 终端链路，重点
解决三个会直接影响文件完整性和长时播放稳定性的边界问题：

1. 音频、视频必须带着独立的轨道描述进入同一个 Publisher，不能依靠消息到达顺序
   猜测轨道，也不能因某一条轨道先结束而提前关闭输出。
2. Source 正常结束后，Decoder 和 Encoder 内部仍可能有尚未输出的数据，必须按照
   上游到下游的顺序排空并 Flush，才能保留尾帧、尾包和正确的 EOS。
3. 多个节点和 Edge 共享 Executor 时，停止线程必须确认业务任务和 Edge Drain 都已
   结束，才能调用有状态媒体对象的 Flush/Close。

### 20.2 架构与消息设计

当前推荐的多轨组图如下：

```text
StreamSourceNode<MediaPacketMessage>
        -> TrackRouterNode
             |-> video -> DecoderNode -> EncoderNode(track_id=video) -+
             |                                                         +-> PublisherSinkNode
             `-> audio -> DecoderNode -> EncoderNode(track_id=audio) -+
```

| 设计项 | A3 实施内容 | 解决的问题 |
|---|---|---|
| EOS 消息 | `MediaPacketMessage`、`MediaFrameMessage`、`EncodedPacketMessage` 增加 `eos` 标志；控制消息只携带 generation 和轨道描述，不携带空媒体包 | EOS 不再被误当成坏包，且能沿每一级节点继续传播 |
| 轨道路由 | `TrackRouterNode` 增加独立 `video`、`audio` 输出端口；两类轨道分别按 stream index、codec 和 generation 选择 | 音频不再进入视频 Decoder，多视频/多音频轨道不会因类型相同而混用 |
| 轨道汇合 | `PublisherSinkNode` 维护 `track_id -> MediaTrackConfig` 的描述和已见轨道集合；配置多轨时收齐轨道描述后才启动 Publisher | FFmpeg 输出上下文一次性创建完整，避免缺少音频或视频流 |
| 有界等待 | 多轨描述等待和首次关键帧等待均使用 `max_pending_packets` 有界队列 | 某一轨道长时间不出包时不会无限增长内存 |
| EOS 汇合 | Publisher 按 `track_id` 记录 EOS，显式配置的所有轨道都结束后才 Stop Publisher | 单轨提前结束不会截断另一条轨道的尾部数据 |
| 写入顺序 | `FfmpegMuxProtocol` 改用 `av_interleaved_write_frame()` | 由 muxer 按各轨道 DTS 交织写入，降低跨轨非单调 DTS 风险 |

`EncodedPacketMessage` 始终携带 Encoder 实际生成的 `time_base`、codec、尺寸、
帧率、采样率、声道数和 extradata。`track_id` 只负责 MediaFlow 内部轨道身份，
不会用手工覆盖 packet 的时间基来掩盖 Encoder 适配问题。

### 20.3 GracefulStop 停止流程

`Graph::GracefulStop(timeout)` 采用明确的四段式屏障：

1. 调用所有节点的 `StopProduction()`。Source 立即停止读取并关闭 Puller，普通
   Decoder/Encoder/Publisher 暂不拒绝已进入 Graph 的消息。
2. 等待所有 NodeContext 的 pending task 和所有 Edge 的排队消息、Drain 调度任务
   同时归零。Edge 新增 `IsIdle()`，只检查队列为空是不够的，必须连同正在执行的
   Drain 一起纳入屏障。
3. 按节点注册顺序逐级调用 `Flush()`，每一级 Flush 后再次等待全图静默。这样
   Decoder 的尾帧先进入 Encoder，Encoder 的尾包再进入 Publisher。
4. 执行既有的逆启动优先级 Stop、关闭 Edge、停止 Executor，并按逆注册顺序
   Deinit。超时或任一 Flush 失败时仍完成资源回收，但返回 `false`，明确告知调用
   方尾部数据可能不完整。

Immediate `Graph::Stop()` 仍保留原有快速语义，不执行等待和 Flush，适用于致命错误
或不需要保留尾部数据的场景。媒体节点的 Flush 具有幂等保护，EOS 传播和 Graph
级 Flush 不会重复输出同一批尾数据。

### 20.4 实际修改

| 领域 | 实际修改 | 主要文件 |
|---|---|---|
| Graph 屏障 | 增加 Node pending task 计数、Edge idle 判断、`StopProduction()`、`Flush()` 和 `GracefulStop()`；保持 Source 最后启动、按逆优先级停止 | `include/mediaflow/core/graph.h`、`include/mediaflow/core/node.h` |
| Source EOS | FFmpeg/Puller 返回 EOS 时发送带 generation 的控制消息，不触发错误重连 | `src/mediaflow/media_nodes.cpp` |
| Decoder | 支持音频/视频轨道；收到 EOS 后自动 Flush 并发送帧级 EOS；增加重复 Flush 保护 | `include/mediaflow/media_nodes.h`、`src/mediaflow/media_nodes.cpp` |
| Encoder | 收到帧级 EOS 后自动 Flush 并发送编码轨道 EOS；尾包保留 Encoder 的时间和元数据 | `include/mediaflow/media_nodes.h`、`src/mediaflow/media_nodes.cpp` |
| Publisher | 支持多轨描述合并、有限等待队列、按轨 EOS 汇合和所有轨道结束后的关闭 | `include/mediaflow/media_nodes.h`、`src/mediaflow/media_nodes.cpp` |
| FFmpeg mux | 将单包写入改为交织写入 API | `include/media/protocol/ffmpeg_mux_protocol.h`、`src/media/protocol/ffmpeg_mux_protocol.cpp` |
| 本地回归 | 新增双轨编码包源和 EOS 停止测试，覆盖轨道收齐、关键帧启动、两轨发布和 Graph graceful 停止 | `test/mediaflow/test_mediaflow_media_nodes.cpp` |

### 20.5 验证结果

使用 `C:\Program Files\Microsoft Visual Studio\18\Community` 的 VS x64 环境
完成本地构建和回归，已通过：

- `test_mediaflow`
- `test_mediaflow_media_nodes`
- `test_publisher_protocol`
- `test_ffmpeg_audio_encoder`
- `test_ffmpeg_decoder_raw_packet`

其中 `test_mediaflow_media_nodes` 额外验证：音频/视频轨道配置完整后才启动
Publisher、EOS 按轨道汇合、Publisher 在两轨结束后关闭，以及
`Graph::GracefulStop()` 返回成功并进入 `Stopped` 状态。

完整测试树曾在 `test_stream_decode` 链接阶段报告
`LNK1136: invalid or corrupt file`。定位后确认是 `build-mediaflow-vs` 生成目录中
单个旧对象损坏；删除该对象并重新编译后，`test_stream_decode` 目标已成功编译和
链接。该目标及其他依赖外部流、摄像头、AVTP 网卡或 ZLMediaKit 的测试仍未执行
真实运行验证。

### 20.6 尚未完成项与下一阶段入口

| 项目 | 当前状态 | 下一步 |
|---|---|---|
| 真实多轨发布 | 本地 RecordingPublisher 已验证消息和生命周期；真实 RTSP/RTMP 服务的多轨兼容性未验证 | 运行服务端集成测试 |
| 长时断线恢复 | `AwaitingKeyframe` 状态和本地关键帧恢复已验证；真实远端断开、重连、关键帧恢复未做长时测试 | 使用受控网络故障做有限时长互操作测试 |
| 现有端到端测试迁移 | `test_stream_decode` 已完成 Source/Router/双 Decoder 迁移；摄像头、AVTP 和 ZLMediaKit 测试仍有旧手工编排 | 后续阶段逐个改为 MediaFlow 图 |
| 动态拓扑与跨轨同步 | 当前 Graph 拓扑启动后冻结，Publisher 依赖 FFmpeg 交织写入，尚未增加动态增删轨和 A/V 同步策略 | 进入集成阶段前先补时间戳、轨道选择和指标验收 |

A3 的本地链路已完成“正常 EOS 可收尾”的第一版闭环，但真实网络和设备环境仍是
下一阶段的验收边界。后续迁移应选择一条真实 RTSP/RTMP 流验证多轨交织、断线恢复
和长时内存稳定性。

## 21. A4 实施记录：迁移 RTSP 音视频解码测试

### 21.1 实施范围

A4 从现有端到端测试中选择 `test_stream_decode` 作为第一条迁移链路。该测试原先
由主线程手工启动 `MediaStreamSession`、轮询 `StreamInfo`、创建两个 Decoder，再
通过 subscriber 按 packet 类型分流。迁移后只保留测试自身的 RTSP 地址、超时配置
和统计输出，媒体生命周期由 MediaFlow Graph 统一管理。

### 21.2 当前组图

```text
StreamSourceNode<MediaPacketMessage>
        -> TrackRouterNode
             |-> video -> DecoderNode -> FrameCounterSink(video)
             `-> audio -> DecoderNode -> FrameCounterSink(audio)
```

| 领域 | 实施内容 | 主要文件 |
|---|---|---|
| 拉流入口 | 使用 `StreamSourceNode` 直接持有 `FFmpegPuller`；连接、读取、代次和停止由节点处理 | `test/media/test_stream_decode.cpp` |
| 音视频分流 | 使用 `TrackRouterNode` 的 `video`、`audio` 端口分别连接两个 Decoder，不再在 subscriber 回调中按 packet type 判断 | `test/media/test_stream_decode.cpp` |
| 解码生命周期 | 两个 `DecoderNode` 在首包到来时根据 `MediaStreamInfo` 延迟 Open；Graph 停止时统一 Flush | `test/media/test_stream_decode.cpp` |
| 统计终端 | 增加带媒体类型校验的 `FrameCounterSink`，忽略 EOS 控制消息，只统计真实解码帧 | `test/media/test_stream_decode.cpp` |
| 停止流程 | 回车后调用 `Graph::GracefulStop(5s)`，等待 Source 停产、Edge/节点任务排空并输出残留帧 | `test/media/test_stream_decode.cpp` |

这次迁移没有引入新的媒体线程、回调订阅或独立的 StreamInfo 等待循环。Graph 的
启动优先级保证下游先于 Source 就绪，Source 的读取线程不会占用媒体节点的业务
Executor；主线程只负责等待用户输入和读取统计快照。

### 21.3 验证结果

使用 `C:\Program Files\Microsoft Visual Studio\18\Community` 的 VS x64 环境，
`test_stream_decode` 已成功重新编译和链接。该程序依赖固定地址
`rtsp://192.168.66.83/live/mainstream` 和现场设备，当前未在无设备环境中执行
真实播放验证。现场测试时 `192.168.66.83` 可 ping 通，RTSP 控制端口 554 可连接；
当前 `live/mainstream` URL 按已有成功测试使用 TCP，另一个摄像头 RTP over UDP
地址暂不纳入本阶段；
`182.168.66.83` 不可达，因此当前代码中的 `192` 地址是有效目标。

当前测试显式设置 `SetRtspTransport("tcp")`、`SetRtspAutoSwitchToTcp(false)`、
`SetLowLatency(true)`，连接和读取超时均为 5 秒，与现有成功的摄像头发布/渲染
测试保持一致。此前针对 UDP 的长时观测属于另一条地址/传输路径的排查结果，不作为
当前 `live/mainstream` MediaFlow 迁移的验收结论。测试仍保留双轨帧计数和 Source/
Edge/Decoder 指标，避免设备没有实际媒体包时误报通过。

### 21.4 下一阶段入口

下一步优先迁移 `test_local_mp4_decode_rtsp_publisher` 的本地文件视频链路：先将
Source、Decoder、Encoder 和 Publisher 接入 MediaFlow，再保留其本地 RTSP 自测客户
端。完成后才能在不依赖摄像头的环境验证 `Source -> Decode -> Encode -> Publish`
的 EOS、关键帧启动和本地 RTSP 互操作；摄像头、AVTP 和 ZLMediaKit 链路继续按同样
边界逐项迁移。

### 21.5 与旧手工摄像头链路的对比

为确认问题边界，重新配置构建目录启用了 `VIDEO_PIPELINE_BUILD_ZLMEDIAKIT_TEST`
和 common-process 支持，并运行了 `test_rtsp_server_publisher`。其中
`TestCameraAudioVideoDecodeEncodePublish` 使用的是 `SetRtspTransport("tcp")`、
`SetLowLatency(true)` 和手工同步 `ReadPacket()` 循环；运行日志显示它成功打开了
H.264/A-law 双轨，启动 H.264/AAC Encoder，并启动本地 RTSP 发布器。该函数本身是
无限循环，现场运行 30 秒后由测试超时终止。

当前 `test_stream_decode` 已按这套成功配置统一为 TCP；此前 UDP 观测显示 Source
在 UDP 读超时后进入第 4 个 generation、`finished=1`，Source->Router Edge 接收数
为 0，该结果仅说明 UDP 地址/传输路径不适用于当前 URL。另一个 RTP over UDP 地址
留待后续单独接入，不影响当前 `live/mainstream` 的 MediaFlow 迁移。

### 21.6 已知问题记录：音频突发导致视频包丢失

#### 21.6.1 问题现象

使用当前已经验证成功的配置连接 `rtsp://192.168.66.83/live/mainstream`：

- RTSP 传输为 TCP；
- `SetRtspAutoSwitchToTcp(false)`；
- `SetLowLatency(true)`；
- 连接和读取超时均为 5 秒。

旧的同步测试可以同时解码摄像头的 H.264 视频和 G.711 音频，但将同一条拉流链路
接入 MediaFlow 后，初始实现出现了“音频持续有帧、视频没有帧或视频帧数量明显不足”
的现象。现场排查确认这不是摄像头 RTP over UDP 地址的问题，也不是当前 TCP
传输配置的问题，而是 MediaFlow 异步队列在启动阶段处理突发数据时丢失了视频包。

#### 21.6.2 实际数据路径

问题发生在下面这条路径中：

```text
FFmpegPuller::ReadPacketResult()
        -> StreamSourceNode::Emit(MediaPacketMessage)
        -> source:out -> router:in
        -> TrackRouterNode
             |-> router:video -> video-decoder:in -> FrameCounterSink(video)
             `-> router:audio -> audio-decoder:in -> FrameCounterSink(audio)
```

`StreamSourceNode` 使用独立读取线程连续调用 `FFmpegPuller::ReadPacketResult()`，
读取成功后立即向 Graph 发送包。它不会按照包的 PTS 主动等待，也不会因为下游正在
处理上一批包而同步调用 Decoder。`QueueTransport` 再把这些包转换为目标节点的异步
任务，由共享的 `AsioExecutor` 执行。

#### 21.6.3 根因分析

初始实现使用了 MediaFlow 的默认配置：

| 位置 | 初始配置 | 直接影响 |
|---|---|---|
| 每个节点的任务队列 | `max_pending_tasks=64` | Router 或 Decoder 尚未处理完上一批任务时，新任务达到上限并被拒绝 |
| Source 到 Router 的包边 | 容量 `64`，`DropNewest` | 队列满后，新读出的包直接丢弃，不会重试 |
| Router 到两个 Decoder 的包边 | 容量 `64`，`DropNewest` | 音频和视频分别排队，但各自的突发仍可能超过队列容量 |
| Decoder 到统计 Sink 的帧边 | 容量 `64`，`DropNewest` | Decoder 产生帧的速度短时高于统计节点时，新帧可能被丢弃 |

触发过程如下：

1. 摄像头流的音频轨道为 16 kHz G.711，解码后每个包通常包含约 320 个采样，
   因此音频包到达频率高于视频帧频率。
2. RTSP/FFmpeg 在连接建立后可能先把接收缓冲区中的一段数据快速交给
   `av_read_frame()`。这段时间 Source 读取速度远高于 Decoder 的处理速度。
3. 音频包先进入 `source:out -> router:in` 的有限队列，占用大部分或全部槽位。
   `DropNewest` 的语义是保留队列中已有的包、丢弃刚到达的包，因此随后到达的
   视频包无法进入队列。
4. 即使部分视频包进入 Router，视频包还要经过独立的 `video` 边。如果 H.264 的
   关键帧、SPS/PPS 或关键帧后的必要连续包在任一层被丢弃，Decoder 就不能建立
   可输出的画面；音频则可能继续正常解码，造成“只有音频”的表象。
5. `OutputPort::Send()` 和 `QueueTransport::Send()` 对 `DropNewest` 的结果不会
   自动重试，Source 也不会重新读取已丢弃的包。因此这不是延迟增加，而是不可恢复
   的媒体数据损失。

#### 21.6.4 为什么旧同步测试没有复现

旧测试在同一个读取循环中执行以下操作：

```text
ReadPacket()
    -> 根据 packet type 选择 Decoder
    -> Decoder::Decode()
    -> 继续 ReadPacket()
```

读取线程在调用 `Decode()` 返回之前不会继续快速填充另一个异步队列，因而不存在
`Source` 和 `Decoder` 之间的突发积压。旧测试的正常结果只能证明摄像头地址、TCP
会话、FFmpeg 解码器和媒体包本身可用，不能直接证明异步 Graph 的默认背压参数适合
该流。

#### 21.6.5 当前改进

已在 `test/media/test_stream_decode.cpp` 中针对这条解码验证图调整为：

| 位置 | 当前配置 | 目的 |
|---|---|---|
| Source、Router、Decoder、Sink 节点 | `max_pending_tasks=4096` | 给启动突发提供足够的任务缓冲空间 |
| Source 到 Router、Router 到 Decoder 的包边 | 容量 `4096`，`DropNewest` | 保持包的 FIFO 顺序，同时避免默认 64 条队列过早溢出 |
| Decoder 到统计 Sink 的帧边 | 容量 `1024`，`DropNewest` | 避免解码突发阶段统计节点成为瓶颈 |
| Source 启动顺序 | 下游节点先启动，Source 最后启动 | 确保首包到达前 Router、Decoder 和 Sink 已完成注册 |
| 运行指标 | 统计每个节点和每条 Edge 的接收、处理、拒绝、丢弃数量 | 区分“没有收到包”和“收到包但解码没有产出” |

这里使用更大的有界队列是验证阶段的改进，不等同于无限扩大缓存。`DropNewest`
仍然可能在持续吞吐不匹配时丢包，只是把启动阶段的短时突发与长期处理能力问题
区分开来；验证程序也必须继续观察 `dropped_newest`，不能仅检查最终帧数。

#### 21.6.6 修复后的现场验证

使用相同的 TCP URL 和 5 秒超时配置运行约 5 秒，结果如下：

| 指标 | 结果 |
|---|---:|
| 视频解码帧 | 66 |
| 音频解码帧 | 178 |
| Source -> Router 接收/丢弃 | `267 / 0` |
| Router -> Video Decoder 接收/丢弃 | `89 / 0` |
| Router -> Audio Decoder 接收/丢弃 | `178 / 0` |
| 进程退出 | 正常，退出码 `0` |

验证表明，扩大启动阶段的任务和包队列后，音频突发不再挤掉当前测试所需的视频
包，视频和音频均能解码。摄像头 RTP over UDP 的另一条 URL 按要求暂不处理。

#### 21.6.7 后续架构改进计划

当前参数只能解决已观测到的启动突发，后续正式封装 `media` 各流程时应继续实现：

1. **轨道隔离队列**：Source 进入 Router 前按媒体类型分成独立队列，避免音频流量
    直接挤占视频队列容量。
2. **视频关键帧保护**：在丢包策略中优先保护 SPS/PPS、关键帧和关键帧后的恢复窗口，
    避免普通视频包丢弃后整个视频解码器长时间无输出。
3. **可中断背压**：对不能丢失的录制、转发链路增加可中断的 `Block` 策略；停止
     Graph 时必须先解除生产者等待，避免 Source 在关闭阶段永久阻塞。
4. **按 PTS 的实时调度**：对本地缓冲突发或重连后的积压数据增加限速/追赶策略，
    避免 Source 长时间以远高于实时的速度填充下游。
5. **按轨道指标和验收门槛**：分别记录音频/视频的 accepted、dropped、rejected、
     keyframe 等指标，并把“视频帧数大于零且 Edge 丢弃为零”纳入解码测试验收。

### 21.7 其它 MediaFlow 测试检查结果

#### 21.7.1 检查范围

当前工程中与 MediaFlow 直接相关的测试入口如下：

| 测试 | 类型 | 是否连接摄像头 | 检查结果 |
|---|---|---:|---|
| `test_mediaflow` | Graph、Executor、Transport、生命周期和背压单元测试 | 否 | 退出码 `0` |
| `test_mediaflow_media_nodes` | ScriptedPuller、Router、Decoder、Encoder、Publisher 节点测试 | 否 | 退出码 `0` |
| `test_stream_decode` | `StreamSourceNode -> TrackRouterNode -> 双 Decoder` 真实 RTSP 测试 | 是 | 短时成功；长时停止暴露问题 |

没有发现第二个可以直接使用 `192.168.66.83` 摄像头地址的 MediaFlow 测试。现有
`test_mediaflow_media_nodes` 使用模拟 Puller、模拟 Decoder 和 RecordingPublisher，
不会额外占用摄像头 RTSP 会话；真实设备测试始终只启动一个 `test_stream_decode`
进程，符合摄像头只支持一路 RTSP 拉流的限制。

#### 21.7.2 本地 MediaFlow 测试结果

`test_mediaflow` 已覆盖以下基础行为并通过：

- `DropNewest`、`DropOldest`、`Close/Open` 的队列语义；
- ExecutorDispatch 传输；
- 节点和端口类型校验、拓扑冻结、启动失败回滚；
- Edge 和 Node 指标；
- 有界任务队列达到上限后的拒绝计数；
- 同一个 Graph 连续停止并重新启动 100 次。

`test_mediaflow_media_nodes` 已覆盖以下媒体节点行为并通过：

- Source 第 1 代次收到音频包时由 Router 丢弃，不进入视频 Decoder；
- RetryableError 触发重连并递增 generation；
- 晚到的旧代次视频包不会重新进入 Decoder；
- Encoder 一帧产生多个 packet 时全部保序发送；
- Publisher 等待视频关键帧、多轨 EOS 汇合和 Flush 尾包。

这两个测试没有发现新的失败，但它们的输入量很小，模拟 Decoder 会立即返回，
因此不能覆盖真实摄像头的以下场景：音频先于视频的大批量突发、1080p H.264 解码
耗时、FFmpeg 网络读取阻塞、视频元数据暂时不完整，以及长时间 Graph 停止。

#### 21.7.3 摄像头单路长时观测

使用 `test_stream_decode` 连接 `rtsp://192.168.66.83/live/mainstream`，配置保持
TCP、低延迟、5 秒连接/读取超时。测试只建立一个 RTSP 会话。

一次约 60 秒的观测中，连接初期成功识别到：

- 视频：H.264，1920x1080，25 fps，stream index `0`；
- 音频：G.711，16 kHz，单声道，stream index `1`；
- 视频从第 1 帧开始正常解码，日志至少增长到视频第 `271` 帧；
- 音频日志至少增长到第 `501` 帧；
- 观测前段没有出现新的 Edge 丢弃日志。

这说明队列扩大后的短时突发问题没有在本次连接初期再次出现。但在约前 12 秒
之后，测试日志不再出现新的音视频帧计数；随后发送回车请求停止，进程在额外等待
30 秒后仍未退出，只能由外部测试控制器强制终止。因此本次长时测试不能报告为
“60 秒完整通过”，只能确认“前段 A/V 解码成功，长时停止失败或超时”。

#### 21.7.4 新发现问题一：长时间运行后的停止不可靠

这是当前可以确认的 MediaFlow 风险。短时运行约 5 秒时，`GracefulStop(5s)` 可以
完成并输出统计；长时运行后，停止请求没有在 30 秒观察窗口内返回。当前可能的
阻塞位置包括：

1. `StreamSourceNode::Stop()` 调用 `FFmpegPuller::Close()` 时等待
   `ReadPacketResult()` 释放 `io_mutex_`；
2. `av_read_frame()` 或 `avformat_close_input()` 的网络 I/O 关闭过程没有在预期
   时间内返回；
3. GracefulStop 先等待 Edge/Node 排空，再逐节点 Flush，长时间突发形成的任务积压
   使停止屏障超过预期；
4. 当前测试只有回车停止入口，没有在停止阶段输出 Source、Edge 和 Node 的实时状态，
   因此强制终止前无法精确区分是 Puller 关闭阻塞还是 Graph 排空阻塞。

这不是摄像头“只支持一路 RTSP”本身造成的结论，因为单路限制已经遵守；但强制终止
会让设备端会话释放变慢，后续复测必须先确认本机没有残留进程并给设备足够的会话
释放时间，不能并行启动第二个拉流测试。

#### 21.7.5 新发现问题二：视频 StreamInfo 可能暂时为 `0x0`

第二次单路复测发生在上一次长时进程被强制终止之后。FFmpeg 打开同一 URL 时报告
视频为 `0x0`，并输出：

```text
Could not find codec parameters for stream 0 (Video: h264, none): unspecified size
```

该次连接仍然读到了音频，并在极短时间内产生大量音频帧，但没有产生视频帧；停止
请求同样未在等待窗口内返回。由于摄像头只支持一路 RTSP，且该次复测紧接着发生在
强制终止之后，这个现象暂时分为两层：

- **已确认的防御性缺口**：`FFmpegPuller::Open()` 允许视频 `width/height` 为 `0` 的
  `MediaStreamInfo` 继续进入 MediaFlow；`DecoderNode::IsUsableStreamInfo()` 当前只
  检查媒体类型、codec 和 time_base，没有拒绝无效视频尺寸。
- **尚未确认的触发原因**：设备残留 RTSP 会话、FFmpeg 探测时机或摄像头端视频轨道
  暂时未发送，仍需在设备会话完全释放后单路复测，不能直接归因于某一个组件。

#### 21.7.6 需要补充的测试和改进

根据这次检查，后续应补充以下验收项：

| 优先级 | 项目 | 验收条件 |
|---|---|---|
| P0 | 长时停止阶段诊断 | 停止请求后分别输出 Puller 关闭、Source 线程、Edge 队列和 Node pending 状态；在明确期限内返回，不允许测试控制器只能强杀进程 |
| P0 | 视频元数据防御 | 视频 `width <= 0` 或 `height <= 0` 时拒绝打开 Decoder，或等待后续完整 StreamInfo；错误必须带 stream index 和 generation |
| P1 | 双轨突发回归 | 使用模拟 Puller 先连续注入高比例音频再注入 H.264 关键帧，验证配置队列下视频包不丢失，并验证丢弃指标可观测 |
| P1 | 设备单路重连 | 每次测试结束后确认进程和 TCP 会话都已释放，再单路重连；分别记录首次 Open 的视频尺寸、首个视频包和首个视频帧耗时 |
| P1 | 长时媒体连续性 | 连续运行至少 10 分钟，按固定间隔记录 A/V 帧计数、Source generation、Edge dropped/rejected 和 Decoder LastError |
| P2 | 停止语义拆分 | 分别验证 ImmediateStop 和 GracefulStop；GracefulStop 超时后应有明确降级路径，而不是依赖外部强制终止 |

在上述问题关闭前，`test_mediaflow` 和 `test_mediaflow_media_nodes` 可以作为本地回归
测试通过，但不能代表真实摄像头 MediaFlow 链路已经完成长时稳定性验收。
