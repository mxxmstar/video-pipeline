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

状态列中的目录编号对应第 16.1 节的实施情况目录；“待实施”表示当前还没有对应的
代码落地和实施记录。

<table style="width:100%; table-layout:fixed; word-break:break-word; overflow-wrap:anywhere;">
<colgroup>
<col style="width:11%;">
<col style="width:15%;">
<col style="width:22%;">
<col style="width:34%;">
<col style="width:18%;">
</colgroup>
<thead>
<tr><th>问题</th><th>当前行为</th><th>媒体风险</th><th>要求</th><th>实施状态</th></tr>
</thead>
<tbody>
<tr><td>Node Start 顺序</td><td>按 AddNode 顺序</td><td>Source 可能在 Decoder/Publisher 未启动时产生数据</td><td>支持拓扑逆序启动，或首期严格按 sink 到 source 添加节点</td><td>已完成<br>目录：A1<br>详情：第 18.1 节</td></tr>
<tr><td>Node Stop 顺序</td><td>按 AddNode 顺序调用 Stop</td><td>Publisher 可能早于 Encoder flush 关闭</td><td>明确 source 到 sink 的 Graceful Stop 顺序</td><td>已完成<br>目录：A3、A4-3<br>详情：第 20.3、21.7.9 节</td></tr>
<tr><td>在途任务</td><td>Node::Stop 时任务可能仍在执行</td><td>Decode 与 Close、Encode 与 Close 并发</td><td>增加 pending-task barrier，并在节点 Executor 上执行 flush/close</td><td>已完成<br>目录：A3、A4-3<br>详情：第 20.3、21.7.9 节</td></tr>
<tr><td>Executor Stop</td><td><code>io_context::stop()</code> 丢弃排队任务</td><td>残留 frame/packet 丢失</td><td>Graceful 模式先 drain，再停止 Executor</td><td>已完成<br>目录：A3<br>详情：第 20.3 节</td></tr>
<tr><td>共享 Executor</td><td>Graph::Stop 会停止其见到的 Executor</td><td>一路停止可能影响其他 Graph</td><td>拆分 Executor 所有权，M1 前不要跨 Graph 共享</td><td>未开始<br>目录：待实施</td></tr>
<tr><td>节点致命错误</td><td>无独立控制面停止入口</td><td>节点线程直接 Stop 可能自 join</td><td>事件上报到 PipelineController，由控制线程执行停止</td><td>未开始<br>目录：待实施</td></tr>
<tr><td>ExecutorDispatch</td><td>直投路径需要把消息直接交给目标 Node Dispatch；目标队列满时必须回传拒绝</td><td>发送方看不到实际拒绝，或媒体分类丢失导致视频保护失效</td><td>直投消费者传递 <code>DispatchPriority</code> 并回传 <code>MailboxPushResult</code>；媒体主链路仍优先使用 QueueTransport</td><td>已完成<br>目录：A4-4<br>详情：第 21.7.14 节</td></tr>
</tbody>
</table>

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

<table style="width:100%; table-layout:fixed; word-break:break-word; overflow-wrap:anywhere;">
<colgroup>
<col style="width:8%;">
<col style="width:17%;">
<col style="width:19%;">
<col style="width:24%;">
<col style="width:18%;">
<col style="width:14%;">
</colgroup>
<thead>
<tr><th>ID</th><th>文件</th><th>当前问题</th><th>必须修改</th><th>验收</th><th>实施状态</th></tr>
</thead>
<tbody>
<tr><td>M-P0-01</td><td><code>include/media/media_packet.h</code></td><td><code>MediaType::VIDEO</code> 和 <code>MediaType::UNKNOWN</code> 都为 0</td><td>将 <code>UNKNOWN=0</code> 放首位，其余值显式且唯一；检查持久化/FFI 依赖</td><td>static_assert 各枚举值不同；发布轨道匹配测试通过</td><td>已完成<br>目录：A0<br>详情：第 17.1 节</td></tr>
<tr><td>M-P0-02</td><td><code>include/media/media_packet.h</code></td><td>PTS 注释与 <code>time_base</code> 实际语义冲突</td><td>明确 tick/time_base 契约、无时间戳表示和校验工具；Dump 判空</td><td>非微秒 time base 的解码、编码、发布测试通过</td><td>已完成<br>目录：A0<br>详情：第 17.1 节</td></tr>
<tr><td>M-P0-03</td><td><code>include/media/puller/i_puller.h</code> 及各 Puller</td><td>bool + nullptr 不能可靠区分 NoData、EOF、可恢复错误和停止</td><td>增加结构化 <code>PullReadStatus</code>，Session 按状态决定继续、完成或重连</td><td>本地文件 EOF 不重连；网络断开按策略重连；超时不误判 EOF</td><td>已完成<br>目录：A0<br>详情：第 17.1 节</td></tr>
<tr><td>M-P0-04</td><td><code>src/media/puller/ffmpeg_puller.cpp</code></td><td>重复 Open 不先 Close；所有音视频 stream 都输出，但只有单个 video/audio 索引</td><td>Open 幂等清理；增加显式选轨或完整轨道映射；事件返回结构化错误</td><td>多视频轨不串入同一 Decoder；Start/Stop/Start 资源稳定</td><td>部分完成<br>目录：A0、A1<br>详情：第 17.1、18.1 节</td></tr>
<tr><td>M-P0-05</td><td><code>include/media/stream/stream_session.h</code>、<code>src/media/stream/stream_session.cpp</code></td><td>Stop 无法中断 CONNECTING；旧 handler 可进入新一代；多线程字段有数据竞争；StreamInfo 回调未快照；Stats 实际不更新</td><td>增加 lifecycle generation、所有状态可停止、回调快照、线程边界和真实原子统计；拆分 Prepare/StartReading</td><td>连接中停止、快速重启、断线重连和统计测试通过，无迟到 packet</td><td>部分完成<br>目录：A0、A1<br>详情：第 17.1、18.1 节</td></tr>
<tr><td>M-P0-06</td><td><code>include/media/decoder/i_decoder.h</code>、<code>src/media/decoder/ffmpeg_decoder.cpp</code></td><td>Open 不清理旧 context；Close 隐式 flush；Graph 停止时可能与 Decode 并发</td><td>增加显式 Flush；Open 先关闭；Close 只释放或明确契约；节点 Executor 屏障后调用</td><td>重复 Open、flush 多帧、Stop during Decode 无崩溃和丢帧</td><td>已完成<br>目录：A0、A1<br>详情：第 17.1、18.1 节</td></tr>
<tr><td>M-P0-07</td><td><code>include/media/encoder/i_encoder.h</code>、<code>src/media/encoder/ffmpeg_encoder.cpp</code></td><td>Frame 微秒 PTS 直接写入任意 codec time base；自动视频 time base 未考虑 fps_den；音频 FIFO PTS 固定按微秒累加；输出 packet 缺少 time_base 和 track</td><td>统一重标定；修正 fps time base；按 codec time base 累加音频；补完整 packet 元数据和统一输出描述</td><td>删除测试中的手工 time_base/stream_index 后仍可播放且 A/V 同步</td><td>部分完成<br>目录：A0、A2<br>详情：第 17.1、19.1 节</td></tr>
<tr><td>M-P0-08</td><td><code>src/media/protocol/ffmpeg_mux_protocol.cpp</code></td><td>多轨使用 <code>av_write_frame</code>，独立音视频节点到达顺序不保证</td><td>使用 <code>av_interleaved_write_frame</code>，或实现有界 DTS 重排；保留多轨回归，本阶段性能验收不使用单轨</td><td>独立音视频 Encoder 并发输入时无非单调 DTS 错误，输出可播放</td><td>已完成<br>目录：A3<br>详情：第 20.4 节</td></tr>
<tr><td>M-P0-09</td><td><code>src/media/publisher/default_publisher.cpp</code>、FFmpeg publish 路径</td><td>重连成功但等待关键帧时仍返回连接中断错误，DefaultPublisher 随即把 <code>started_</code> 置 false，后续关键帧无法恢复</td><td>区分 AwaitingKeyframe、RetryableFailure 和 Closed；只有 Publisher 真正关闭时清除 started</td><td>模拟断线，先送非关键帧再送关键帧能够恢复</td><td>已完成<br>目录：A2<br>详情：第 19.1 节</td></tr>
<tr><td>M-P0-10</td><td><code>src/media/protocol/rtsp_server_protocol_adapter.cpp</code></td><td>多个相同媒体类型/codec 的 track 回退时直接取第一个候选</td><td>与 FFmpeg adapter 一致，候选不唯一时拒绝并要求显式 track</td><td>双 H264 或双 AAC 轨道不会静默写错</td><td>部分完成<br>目录：A0、A3<br>详情：第 17.3、20.6 节</td></tr>
<tr><td>MF-P0-01</td><td><code>include/mediaflow/core/graph.h</code>、<code>executor.h</code></td><td>缺少媒体 Graceful Stop 屏障和安全启动/停止顺序</td><td>增加拓扑生命周期顺序、在途任务等待和 Executor 上 flush；禁止控制线程自 join</td><td>编解码处理中停止、重复启动、启动失败回滚测试通过</td><td>已完成<br>目录：A3、A4-3<br>详情：第 20.3、21.7.9 节</td></tr>
</tbody>
</table>

### 7.2 P1：主链路完成前建议完成

<table style="width:100%; table-layout:fixed; word-break:break-word; overflow-wrap:anywhere;">
<colgroup>
<col style="width:10%;">
<col style="width:20%;">
<col style="width:50%;">
<col style="width:20%;">
</colgroup>
<thead>
<tr><th>ID</th><th>文件</th><th>问题与改进</th><th>实施状态</th></tr>
</thead>
<tbody>
<tr><td>M-P1-01</td><td><code>include/media/media_frame.h</code></td><td>增加时间戳有效性；<code>Stride</code>、<code>PlaneOffset</code> 校验 index；避免 type 与 variant 不一致时直接 <code>std::get</code> 抛异常</td><td>未开始<br>目录：待实施</td></tr>
<tr><td>M-P1-02</td><td><code>include/media/i_media_buffer.h</code></td><td>推进只读 Buffer 契约，增加 <code>ConstPacketPtr</code>/<code>ConstFramePtr</code>；需要写入的节点显式复制或独占</td><td>未开始<br>目录：待实施</td></tr>
<tr><td>M-P1-03</td><td><code>include/media/stream/source_config.h</code></td><td>合并重复超时字段；处理或删除当前未生效的 <code>max_delay_ms</code>、<code>dump_packets</code>、headers、socket buffer 等配置</td><td>未开始<br>目录：待实施</td></tr>
<tr><td>M-P1-04</td><td><code>src/media/stream/stream_source.cpp</code></td><td>若保留兼容类：回调改 weak capture、Stop 解绑回调、subscriber 支持取消、锁外调用回调、保护 StreamInfo、Stop 统计线程、packet buffer 判空</td><td>部分完成<br>目录：A1<br>详情：第 18.1 节</td></tr>
<tr><td>M-P1-05</td><td><code>AdaptiveJitterBuffer</code></td><td>改为每轨实例、按 time base 计算、批量 Pop、累计与当前代次指标分离；或迁移为独立 JitterBufferNode</td><td>未开始<br>目录：待实施</td></tr>
<tr><td>M-P1-06</td><td><code>DefaultPublisher</code></td><td>查询方法与 Start/Publish/Stop 并发不安全；增加锁或由 PublisherSinkNode 保存线程安全快照</td><td>部分完成<br>目录：A2<br>详情：第 19.1 节</td></tr>
<tr><td>M-P1-07</td><td><code>RtspServerProtocol</code></td><td><code>Write</code> 成功只表示已 post，内部媒体任务没有独立总上限；增加有界媒体入口和 accepted/delivered/dropped 区分</td><td>未开始<br>目录：待实施</td></tr>
<tr><td>M-P1-08</td><td><code>FfmpegMuxProtocol</code></td><td>断线发生在音频 packet 时，也应根据配置中是否存在视频轨决定是否等待关键帧</td><td>部分完成<br>目录：A2、A3<br>详情：第 19.1、20.4 节</td></tr>
<tr><td>M-P1-09</td><td><code>PublisherResult</code></td><td>增加 recoverable、connection state、packet disposition，避免节点通过错误码字符串推断状态</td><td>部分完成<br>目录：A2<br>详情：第 19.1 节</td></tr>
<tr><td>M-P1-10</td><td><code>MultiStreamInfo</code></td><td>提供按 stream index 查找和显式 selected tracks；减少“vector 下标”和“源 stream 编号”混淆</td><td>部分完成<br>目录：A1、A3<br>详情：第 18.1、20.2 节</td></tr>
<tr><td>MF-P1-01</td><td>MediaFlow metrics</td><td>增加节点业务错误、处理时延、最后错误、生命周期状态和 Pipeline 级统计</td><td>部分完成<br>目录：A4-4<br>详情：第 21.7.14 节</td></tr>
<tr><td>MF-P1-02</td><td>Queue/Dispatch</td><td>当前 Edge 队列与 Node pending tasks 形成双层缓存；配置和指标应展示总在途数量，后续增加按字节上限</td><td>部分完成<br>目录：A4-3、A4-4、A4-5、A4-6<br>详情：第 21.7.12、21.7.14、21.7.15、21.7.16、21.7.18、21.7.19 节<br>后续：使用无坏包双轨媒体源完成三轮性能复测</td></tr>
<tr><td>MF-P1-03</td><td>Source 音视频独立 Edge、TrackRouterNode 及下游调度</td><td>入口队列已经分轨，但仍需按时间长度/字节数进行轨道级容量控制，并基于 PTS/DTS、time base、统一时钟和 generation 设计音视频调度，不能按音频帧数与视频帧数配对</td><td>部分完成<br>目录：A4-2、A4-3、A4-4、A4-5、A4-6<br>详情：第 21.6.7、21.7.12、21.7.14、21.7.15、21.7.16、21.7.18、21.7.20 节<br>已完成：Source 轨道序号和 Decoder 关键帧恢复已通过双轨本地 FFmpeg 验证<br>后续：统一时钟调度、真实网络丢包互操作</td></tr>
</tbody>
</table>

#### MF-P1-03 详细要求：音视频队列隔离与同步

音频帧数和视频帧数不能直接按数量比较。当前摄像头音频通常每帧包含约 20 ms
数据，视频约为 40 ms 一帧，因此音频帧数约为视频帧数的两倍属于正常比例。但
现场短测曾出现音频 4483 帧、视频 370 帧的明显差距，结合音频 PTS 和首批突发
日志，应该按音频历史数据追赶或突发积压处理，不能仅视为编码帧率差异。

第 21.7.15 节完成入口分轨后，当前 MediaFlow 的结构是：

```text
StreamSourceNode:video -> 视频入口 Edge -> TrackRouterNode:video-in
                                          -> 视频 Edge -> Video Decoder
StreamSourceNode:audio -> 音频入口 Edge -> TrackRouterNode:audio-in
                                          -> 音频 Edge -> Audio Decoder
```

音频和视频已经在 Source 入口及 Router 下游使用独立 Edge，音频突发不会再直接
占用视频 Edge 的队列槽位；`PreferVideoKeyframes` 继续保护视频恢复窗口。当前
仍未解决的是“队列容量如何按媒体成本计算”和“两个轨道如何在统一时间轴上调度”：

- 当前容量仅按消息条数配置，不能表达 4K 视频包、低码率音频包之间的内存差异；
- 尚未限制队列覆盖的媒体时间长度，历史音频仍可能形成较大播放延迟；
- Router 的 Serialized Dispatch 仍是共享任务队列，需要把各轨道 Edge 和节点
  pending task 一并纳入总在途预算；
- 使用统一的 PTS/DTS 调度、音频主时钟、视频迟到帧处理和重连 discontinuity；
- 以轨道时间长度或字节数控制容量，并分别记录音频/视频队列时长、PTS 差值和
  丢弃原因；
- 所有包和帧保留 PTS、DTS、time base、generation；录制/发布链路按 DTS 交织
  写入，而不是按两个队列的出队次数配对；
- 播放链路使用统一时钟，通常以音频时钟为主，视频按 PTS 等待或丢弃迟到的非
  关键帧，不能把两个 FIFO 的独立出队误认为已经完成音视频同步。

该问题属于必须修改的 P1 架构项。入口隔离已经完成，按字节/时间长度的容量控制
及其验收计划见第 21.7.16 节；在该计划完成前，当前消息数上限只能作为硬安全阀，
不能作为媒体延迟或内存预算已经达标的证明。

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

### 16.1 实施情况目录

问题表中的“实施状态”统一使用下面的目录编号。状态单元格先给出当前完成度，
再给出目录编号和详细章节；同一问题如果跨多个阶段实施，列出全部相关目录。

<table style="width:100%; table-layout:fixed; word-break:break-word; overflow-wrap:anywhere;">
<colgroup>
<col style="width:9%;">
<col style="width:15%;">
<col style="width:39%;">
<col style="width:17%;">
<col style="width:20%;">
</colgroup>
<thead>
<tr><th>目录编号</th><th>实施阶段</th><th>实施内容</th><th>详细记录</th><th>当前结论</th></tr>
</thead>
<tbody>
<tr><td>A0</td><td>公共契约</td><td>媒体类型、时间戳、Puller 读结果、Session 生命周期、Decoder/Encoder 基础契约</td><td>第 17.1-17.4 节</td><td>已完成首轮公共契约改造，部分多轨和发布问题留待后续阶段</td></tr>
<tr><td>A1</td><td>Source + Decoder</td><td>Source、TrackRouter、Decoder 单视频节点链路及其启动/接入约束</td><td>第 18.1-18.5 节</td><td>单视频本地图已完成，真实多轨和完整 EOS 仍需后续接入</td></tr>
<tr><td>A2</td><td>Encoder + Publisher</td><td>Encoder、PublisherSink、关键帧启动和发布结果语义</td><td>第 19.1-19.5 节</td><td>单视频编码发布链路已完成，真实网络长测和多轨汇合仍需验证</td></tr>
<tr><td>A3</td><td>多轨汇合与有序停止</td><td>音视频汇合、FFmpeg 交织写入、GracefulStop 和节点停止顺序</td><td>第 20.1-20.6 节</td><td>本地多轨收尾和有序停止已完成，部分协议细节仍待补齐</td></tr>
<tr><td>A4-1</td><td>RTSP 解码迁移</td><td>将 <code>test_stream_decode</code> 迁移为 Source -&gt; Router -&gt; 双 Decoder 图</td><td>第 21.1-21.5 节</td><td>单路摄像头迁移已完成，当前按 TCP URL 验证</td></tr>
<tr><td>A4-2</td><td>音频突发与视频包丢失</td><td>记录混合队列问题、启动突发改进和后续轨道隔离计划</td><td>第 21.6.1-21.6.7 节</td><td>Edge 层启动突发已改善；轨道级入口隔离当时未实施，已于 A4-5 完成</td></tr>
<tr><td>A4-3</td><td>摄像头测试问题修复</td><td>修复停止阻塞、无效视频元数据、探测恢复和 Edge 视频保护</td><td>第 21.7.7-21.7.13 节</td><td>已完成三批修复并通过短时单路验证，仍需长时验证</td></tr>
<tr><td>A4-4</td><td>Node Dispatch 视频保护</td><td>保护节点任务队列中的视频和关键帧，并回传直投拒绝结果</td><td>第 21.7.14 节</td><td>已完成第四批修复，A/V 同步仍是后续工作</td></tr>
<tr><td>A4-5</td><td>轨道入口队列拆分</td><td>Source 和 Router 增加独立音频/视频端口，入口 Edge 分别配置容量、背压和指标</td><td>第 21.7.15 节</td><td>已完成轨道级入口隔离；统一时钟和 A/V 同步仍需后续实施，容量预算已完成 C1-C7 离线实现和回归</td></tr>
<tr><td>A4-6</td><td>A/V 轨道容量控制</td><td>按消息数、字节数和媒体时间长度建立分轨预算，补充水位、丢弃原因和跨轨时间差指标</td><td>第 21.7.16-21.7.20 节</td><td>容量边界、队列和活跃帧引用已通过两轮单会话双轨 600 秒复验；66.166 当前版本基线和本机双轨初步前后对照已记录；双轨本地 FFmpeg 已验证序号丢包后的关键帧恢复，最终性能验收仍需无坏包媒体源三轮复测，统一时钟调度仍待实施</td></tr>
</tbody>
</table>

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

#### 21.7.7 第一批修复实施记录

本批次先处理可以在 MediaFlow 边界内闭环的问题，改动如下：

| 问题 | 修改内容 | 验证结果 |
|---|---|---|
| GracefulStop 超时预算按阶段累加 | `Graph::GracefulStop()` 在调用 `StopProduction()` 前建立统一的 `steady_clock` 截止时间；初始排空和每级 `Flush` 共用该截止时间，不再为每个节点重复分配完整 timeout | 代码已编译；现有 Graph/媒体节点测试通过 |
| 视频 StreamInfo 为 `0x0` 仍尝试打开 Decoder | `DecoderNode::IsUsableStreamInfo()` 增加 detail 类型、视频宽高、音频采样率和通道数校验；不完整描述直接拒绝，底层 Decoder 不执行 `Open()`/`Decode()` | 新增 `TestDecoderRejectsIncompleteStreamInfo()`，测试通过 |
| 测试无法区分元数据错误和 Decoder 打开成功 | 测试使用 `RecordingDecoder` 统计 `Open`、`Decode` 调用次数，并断言返回明确错误 `stream info is incomplete for decoder open` | `test_mediaflow_media_nodes` 退出码 `0` |

#### 21.7.8 第一批修复验证记录

使用 VS 18 Community x64 Debug 环境执行：

```text
test_mediaflow                 退出码 0
test_mediaflow_media_nodes     退出码 0
test_stream_decode              退出码 0（单路 RTSP，约 5 秒）
```

本次摄像头短测只建立一个 `rtsp://192.168.66.83/live/mainstream` 会话，保持现有
测试配置。连接成功识别到视频 `1920x1080/25fps` 和音频 `16kHz/单声道`；停止前视频
解码 65 帧、音频解码 176 帧，Source、Router、视频和音频 Edge 均未出现拒绝或丢弃。
这次短测没有复现 `0x0` 元数据，也没有复现超过测试窗口的停止阻塞。

本批次没有声称解决长时停止问题。`StreamSourceNode::StopProduction()` 当前仍会
同步调用 `Stop()`，而 `Stop()` 需要等待读线程和 Puller 关闭；如果底层
`av_read_frame()` 或 `avformat_close_input()` 不响应中断，Graph 仍可能在进入统一
截止时间检查之前被阻塞。下一批应拆分“请求停止”和“等待线程退出”的语义，并增加
Puller 关闭耗时、Source 线程状态和停止阶段队列指标，确保超时后有明确的降级路径。

#### 21.7.9 第二批修复实施记录

本批次处理长时停止阶段的生命周期和队列调度问题：

| 问题 | 修改内容 | 验证结果 |
|---|---|---|
| `StopProduction()` 同步等待网络关闭 | 为 `IPuller` 增加非阻塞 `RequestStop()` 契约；`FFmpegPuller` 只设置中断标志，`StreamSourceNode::StopProduction()` 不再调用 `Close()` 或等待读线程 | 阻塞 Puller 回归测试通过，停止请求耗时低于 100 ms |
| AVTP 读取器没有对应的非阻塞停止入口 | `AvtpPuller` 和 `EthernetCapture` 同样拆分停止请求与线程 `join`，保证切换到 AVTP 输入时遵守同一生命周期契约 | 相关目标随工程 Debug 构建通过 |
| Source 线程可能在排空屏障后继续发包 | 增加 `INode::IsProductionStopped()`；Graph 的排空屏障等待异步 Source 真正退出后再 Flush | Source 停止顺序测试通过 |
| Edge Drain 在关闭阶段继续批量提交 | Edge 关闭时设置取消门闩，Drain 每轮检查取消状态，停止后续消息投递并禁止重新调度 | `test_mediaflow`、`test_mediaflow_media_nodes` 通过 |
| 测试控制器读取日志不及时导致假性停止阻塞 | `test_stream_decode` 的停止观测使用独立线程；现场验证脚本持续异步读取 stdout/stderr，避免音频突发日志填满管道 | 长测可区分媒体失败和停止失败 |

#### 21.7.10 第二批验证记录

使用 VS 18 Community x64 Debug 环境执行：

```text
test_mediaflow                 退出码 0
test_mediaflow_media_nodes     退出码 0
test_stream_decode              约 30 秒运行后停止流程返回
```

本次真实设备仍只建立一个 `rtsp://192.168.66.83/live/mainstream` 会话。由于设备/会话
探测再次返回视频 `0x0`，FFmpeg 输出 `unspecified size`；MediaFlow 收到视频压缩包
742 个但没有视频解码帧，音频收到并解码 6030 帧，Edge 丢弃和拒绝均为零。因此
`test_stream_decode` 最终退出码为 `1`，原因是 A/V 双轨验收失败，而不是停止流程超时。

停止阶段已经完整执行：Source 标记 finished，Graph 完成 Flush，Executor 和所有
节点 Deinit 返回，测试控制器在停止后 15 秒窗口内没有强制终止进程。此次结果确认
第二批已关闭“长时停止卡死”的 MediaFlow 生命周期问题，但没有关闭摄像头视频
`0x0` 的触发原因。

#### 21.7.11 第二批后的剩余问题

1. **设备视频探测不稳定**：连续单路复测仍可能出现 H.264 `0x0`。当前 Decoder
   会拒绝不完整 StreamInfo，避免错误打开解码器，但 Source 仍会继续接收该代次的
   音频和视频压缩包；后续应在 Puller/Open 层增加视频轨道完整性判定和有界重探测，
   或在确认视频元数据前暂缓该轨道进入 Router。
2. **音频突发的业务策略**：本次 30 秒复测没有出现 Edge 丢弃，但设备在视频元数据
   异常时产生高密度音频。后续仍需用模拟 Puller 注入音频突发和视频关键帧，验证
   轨道级背压、关键帧保护及恢复窗口，而不能只依赖一次设备观测。
3. **完整长时媒体验收**：待设备恢复稳定视频元数据后，再执行至少 10 分钟单路
   RTSP 测试，记录首个视频包/帧延迟、generation、A/V 帧数及 Edge dropped/rejected。

#### 21.7.12 第三批修复实施记录

本批次开始处理第二批遗留的媒体质量问题，修改内容如下：

| 问题 | 修改内容 | 验证结果 |
|---|---|---|
| RTSP 视频探测偶发返回 `0x0` | `FFmpegPuller` 为不完整视频轨道建立 H.264/H.265 parser；收到压缩包后在有限包数和有限时间内恢复宽高。探测未完成前暂不向 Decoder 发送视频包，超出上限后返回明确的 FatalError，避免无限等待或用无效描述打开 Decoder | 现场单路测试复现初始 `0x0` 后恢复到 `1920x1080`，产生视频帧；未触发探测上限 |
| Source 缓存的流描述停留在初始值 | `StreamSourceNode` 在每个压缩包到达时刷新 Puller 的 `MultiStreamInfo`，使 Puller 在包级恢复的宽高能进入当前 `MediaPacketMessage` | 视频 Decoder 成功打开并输出 `1920x1080` 帧 |
| 临时不完整描述污染 Decoder 错误状态 | `DecoderNode` 在完整 StreamInfo 成功打开后清除探测阶段临时错误 | `test_mediaflow_media_nodes` 通过 |
| 音频突发挤占混合队列并丢视频 | 新增 `BackpressurePolicy::PreferVideoKeyframes` 和 `QueueItemTraits`；队列满时拒绝新增音频，视频优先淘汰排队音频，视频关键帧仍可淘汰视频非关键帧后进入队列。真实解码测试的 Source 混合包边改用该策略 | 新增队列回归测试通过；现场测试视频/音频 Edge 丢弃与拒绝均为 0 |
| 现场测试依赖交互输入，无法稳定自动化 | `test_stream_decode` 增加 `--duration-ms N`，在指定时长后自动进入统一 GracefulStop，保留默认回车模式 | 15 秒单路摄像头自动测试退出码 0 |

#### 21.7.13 第三批验证记录

使用 VS 18 Community x64 Debug 环境执行：

```text
test_mediaflow                 退出码 0
test_mediaflow_media_nodes     退出码 0
test_stream_decode --duration-ms 15000 退出码 0（单路 RTSP）
```

本次测试仍只建立一个 `rtsp://192.168.66.83/live/mainstream` 会话，并保持当前
MediaFlow 验证配置。FFmpeg 首次探测报告视频 `0x0`，随后 parser 从视频压缩包恢复
为 `1920x1080`；视频 Decoder 输出 370 帧，音频 Decoder 输出 4483 帧。视频和音频
Decoder 的 `enqueued/processed` 分别为 `370/370`、`4483/4483`，三条相关 Edge
均为 0 dropped、0 rejected，停止流程正常完成。

这次结果确认“视频元数据暂时为 `0x0`”已经从永久失败改为有界恢复路径；但仍需在
设备稳定后执行至少 10 分钟长测，确认多次重连、关键帧间隔变化和更高音频突发下的
长期统计。`PreferVideoKeyframes` 是有界队列保护策略，若队列被视频关键帧完全占满，
仍会拒绝无法替换的最新关键帧；对绝对不允许丢包的录制链路应继续使用 `Block` 并单独
配置容量和停止超时。

#### 21.7.14 第四批修复实施记录：保护 Node Dispatch 队列中的视频任务

上一批修复只覆盖了 `Source -> Router` 以及 `Router -> Decoder` 的 Edge 队列。
当 Router 的 Drain 已经成功取出消息，但 Decoder 节点自身的 Serialized
Dispatch 队列被音频突发占满时，视频任务仍可能在进入节点前被拒绝。因此，本批
继续处理同一个“音频突发导致视频包丢失”问题，但不改变当前的音视频独立 Edge
和同步模型，也不实施轨道拆分队列。

本批修改如下：

1. `NodeOptions` 增加 `prefer_video_keyframes`，允许节点单独启用视频保护策略。
2. 增加 `DispatchPriority`，由 Edge Drain 为每条任务传递音频、视频和关键帧分类。
3. Serialized Dispatch 队列改为保存任务及其分类信息。队列达到上限时：
   - 新音频任务被拒绝，不再挤占队列中的视频任务；
   - 新视频任务可以替换尚未执行的排队音频任务；
   - 新视频关键帧在没有排队音频可替换时，可以替换排队中的非关键视频帧；
   - 队列中只有关键帧或无法安全替换的任务时，仍然拒绝新任务，避免破坏视频恢复窗口。
4. 修正任务替换后的 `pending_tasks_` 计数：被淘汰的任务先释放一个待处理槽位，
   新任务再占用一个槽位，保证停止屏障和队列实际长度一致。
5. `test_stream_decode` 的音视频 Decoder 节点启用该策略，`test_mediaflow` 新增
   Dispatch 队列满载时“视频关键帧替换音频、后续音频被拒绝”的回归测试。
6. 修复 `ExecutorDispatch` 直投路径遗漏媒体分类的问题。该路径没有 `Edge::Drain`
   负责分类，因此在直接消费者中显式生成 `DispatchPriority`，确保使用直接传输时
   仍然能够保护视频关键帧。
7. 增加 Graph 级直投回归测试：阻塞首个音频任务并填满后续音频任务，验证视频
   关键帧仍进入节点，而新增音频不能挤掉已保留的视频任务。
8. 为 `ExecutorDispatchTransport` 增加可回传结果的消费者接口；当目标 Node
   Dispatch 因队列满而拒绝任务时，`OutputPort::Send` 不再错误返回成功。

验证结果（VS 18 Community x64 Debug）：

```text
test_mediaflow                 exit code 0
test_mediaflow_media_nodes     exit code 0
test_avtp_timestamp_mapper     exit code 0
test_stream_decode --duration-ms 15000  exit code 0
```

摄像头单路 RTSP 测试仍使用 `rtsp://192.168.66.83/live/mainstream`。本次短时
测试识别到视频 `1920x1080/25fps` 和音频 `16kHz/mono`，Decoder 指标为视频
`enqueued=372, processed=372, rejected=0`、音频 `enqueued=5264, processed=5264,
rejected=0`；Source -> Router、Router -> Video 和 Router -> Audio 三条相关
Edge 的 dropped/rejected 均为 `0`。这证明 Dispatch 层保护策略已接入并未影响
当前稳定设备流，但仍需在设备状态稳定后进行更长时间、更多音频突发强度的验证。

本批修复没有改变 `MF-P1-03` 中关于音视频同步的结论。轨道入口隔离已在第
21.7.15 节实施，但后续同步设计仍必须以 PTS/DTS、time base、统一时钟和时间
长度为约束，不能按音频帧数与视频帧数配对。

#### 21.7.15 第五批修复实施记录：拆分音频和视频入口队列

本批继续处理 `MF-P1-03` 的入口队列问题。目标是消除
`StreamSourceNode -> TrackRouterNode` 的混合 MediaPacket Edge，使音频突发只
影响音频入口的容量和指标，不再与视频包共享同一组队列槽位。本批只完成轨道级
队列隔离，不把两个独立 FIFO 的出队过程当作音视频同步，也没有改变后续 PTS/DTS
交织和统一时钟的实施要求。

本批修改如下：

1. `StreamSourceNode` 保留兼容的 `out` 混合端口，同时增加 `video` 和 `audio`
   两个专用输出端口。Source 根据 `MediaPacket::type` 将包发送到对应队列，
   未连接的兼容端口不会创建实际队列。
2. `TrackRouterNode` 保留兼容的 `in` 输入，同时增加 `video-in` 和 `audio-in`
   两个专用输入端口。拆分输入再次校验媒体类型，并只向对应轨道的输出发送，
   防止错误连线把音频包交给视频 Decoder。
3. Source 和 Router 的 EOS 在拆分路径上按轨道各发送一次；混合兼容路径仍广播
   到两个输出。这样两个 Decoder 都能获得自己的 Flush 边界，且不会收到重复 EOS。
4. 为 `MediaPacketMessage::eos` 增加控制消息分类。队列满载时，EOS 可以替换
   一条普通媒体消息进入队列，避免音频突发把生命周期结束信号一起丢弃。
5. `test_stream_decode` 改为连接四条压缩包边：
   `source:video -> router:video-in`、`source:audio -> router:audio-in`、
   `router:video -> video-decoder` 和 `router:audio -> audio-decoder`。入口视频
   队列容量为 `2048` 并使用 `PreferVideoKeyframes`，入口音频队列容量为 `8192`
   并使用 `DropNewest`；两类入口和下游边分别输出 Edge 指标。
6. 增加 `TestSourceTrackQueuesAreIndependent()`：阻塞音频消费者并制造音频突发，
   验证音频边发生丢弃时视频关键帧仍进入独立视频边，并验证两条边各收到一次 EOS。

验证结果（VS 18 Community x64 Debug）：

```text
test_mediaflow                 退出码 0
test_mediaflow_media_nodes     退出码 0
```

现场单路测试仍只建立一个
`rtsp://192.168.66.83/live/mainstream` RTSP 会话，运行
`test_stream_decode --duration-ms 15000`，结果如下：

| 指标 | 结果 |
|---|---:|
| 视频 Decoder enqueued/processed/rejected | `375 / 375 / 0` |
| 音频 Decoder enqueued/processed/rejected | `745 / 745 / 0` |
| Source 视频入口 accepted/dropped | `375 / 0` |
| Source 音频入口 accepted/dropped | `745 / 0` |
| Router 到视频 Decoder accepted/dropped | `375 / 0` |
| Router 到音频 Decoder accepted/dropped | `745 / 0` |
| 解码帧 | 视频 `353`，音频 `745` |
| Source generation / finished | `4 / 1` |
| 进程退出 | 正常，退出码 `0` |

本批已关闭“Source 到 Router 混合队列会被音频突发占满”的结构性问题，但仍有
以下边界：

- 当前容量仍按消息条数配置，尚未按字节数或时间长度动态预算；
- Router 节点自身仍是一个 Serialized Dispatch 队列，入口隔离主要保护 Source
  到 Router 以及 Router 到 Decoder 的 Edge；
- 音视频同步仍必须依据 PTS/DTS、time base、generation 和统一时钟处理，不能
  按音频帧数与视频帧数配对；
- 现场验证为 15 秒短测，仍需设备稳定后执行至少 10 分钟单路长测。

#### 21.7.16 A/V 轨道容量控制实施与验收计划

##### 21.7.16.1 目标与边界

本计划承接第 21.7.15 节的轨道入口隔离，解决“每条轨道应该允许积压多少数据”
的问题。目标不是简单地把 `capacity=2048/8192` 换成另一组固定数字，而是让每条
轨道同时受消息数、载荷字节数和媒体时间长度约束，并能够解释每一次限流和丢弃。

本阶段包含：

- 音频和视频独立容量配置；
- Edge 队列与 Node pending task 的总在途预算；
- 按轨道统计当前字节数、时间跨度、水位和丢弃原因；
- 视频关键帧恢复窗口、音频低延迟淘汰和 EOS 控制消息保护；
- generation 变化、时间戳缺失和时间戳回退时的容量状态复位；
- 模拟突发、生命周期和单路摄像头长时验收。

本阶段不直接实现完整播放同步。容量控制只保证队列有界、时间戳不被破坏，并提供
A/V 时间差指标；音频主时钟、视频等待/迟到丢帧和渲染节拍仍属于后续同步阶段。

##### 21.7.16.2 目标数据模型

建议在 MediaFlow 核心增加与消息类型无关的预算结构，由媒体消息 traits 提供成本：

```cpp
namespace mediaflow {

struct QueueBudget {
    std::size_t max_items{0};
    std::size_t max_bytes{0};
    std::int64_t max_span_us{0};
    std::uint32_t high_watermark_percent{80};
    std::uint32_t low_watermark_percent{60};
};

struct QueueItemCost {
    std::size_t bytes{0};
    std::int64_t timestamp_us{kNoQueueTimestamp};
    std::int64_t duration_us{0};
    std::uint64_t generation{0};
};

} // namespace mediaflow
```

字段语义：

| 字段 | 约束 |
|---|---|
| `max_items` | 最后的硬安全阀，防止缺少 Buffer 或时间戳时退化为无界队列；`0` 表示该维度不启用 |
| `max_bytes` | 统计消息直接持有的媒体载荷；同一共享 Buffer 在同一队列中只按每条消息的逻辑载荷计费，不尝试推断进程全局引用数 |
| `max_span_us` | 同一 `(track, generation)` 队列中最早与最晚有效时间戳覆盖的媒体时长；压缩包优先使用 DTS，显示/渲染帧使用 PTS |
| 高/低水位 | 高水位触发告警或主动降载，低水位用于解除告警，避免在阈值附近反复抖动 |
| `generation` | 变化时清空上一代的时间跨度基准，不允许跨重连代次计算队列时长或 A/V 差值 |

时间戳无效或发生明显回退时，队列不能伪造时间跨度。此时继续使用消息数和字节数
限制，并增加 `timestamp_invalid` 或 `timestamp_discontinuity` 指标；只有新 generation
或明确的 discontinuity 处理完成后才重新建立时间窗口。

##### 21.7.16.3 分阶段实施步骤

| 阶段 | 实施内容 | 主要文件 | 完成标志 |
|---|---|---|---|
| C1：预算与成本契约 | 增加 `QueueBudget`、`QueueItemCost`、容量触发原因和配置校验；为普通类型提供零成本默认 traits，为 `MediaPacketMessage`/`MediaFrameMessage` 提供字节、时间戳、duration、generation 分类 | `include/mediaflow/core/types.h`、`transport.h`、`media_nodes.h` | 独立单元测试能从音频、视频、EOS 和无时间戳消息得到稳定成本 |
| C2：多维队列记账 | `QueueTransport` 在 Push、Pop、Drop、Open、Close 时维护 items/bytes/span；任一启用维度达到上限都执行配置策略；超大单包明确返回 oversized，不允许突破硬上限 | `include/mediaflow/core/transport.h` | 任意入队/淘汰序列后，快照与队列实际内容一致，三种上限均不被静默突破 |
| C3：分轨背压策略 | 视频优先淘汰非关键帧并保留可解码恢复窗口；实时音频优先淘汰最旧完整包以限制延迟；录制/发布链路使用可中断 `Block`；EOS 始终优先于普通媒体消息 | `transport.h`、`graph.h`、`media_nodes.cpp` | 音频突发不会导致视频关键帧因共享容量丢失，停止可以解除所有 Block 等待 |
| C4：总在途预算 | 将 Edge 队列与目标 Node Dispatch 的 pending items/bytes 合并为轨道快照；为 Dispatch 任务传递媒体成本和轨道分类，避免 Edge 已排空但 Node 中仍隐藏大量媒体数据 | `types.h`、`graph.h` | 可查询每轨 Edge、Dispatch 和总在途 items/bytes/span，三者关系可核对 |
| C5：指标与诊断 | 增加当前值、历史最高水位、进入/离开高水位次数，以及 count/bytes/span/oversized/keyframe/timestamp 各类丢弃原因；Pipeline 汇总时按轨道保留明细 | `types.h`、`graph.h`、测试日志 | 每一次未接收消息都能归因，不能只看到笼统的 `dropped_newest` |
| C6：配置接入 | 为视频、音频分别配置预算；保留 `max_items` 兜底。实时解码初始值通过码率和目标延迟计算，不把现场测试中的 `2048/8192` 固化为所有场景默认值 | `test_stream_decode.cpp`、后续 PipelineBuilder | 同一图可以为实时播放、录制和发布选择不同配置，不需要修改 Transport 代码 |
| C7：回归与现场验收 | 增加确定性突发、变码率、时间戳异常、generation 和停止测试；最后执行单路摄像头长测并填写第 21.7.16.6 节结果 | `test_mediaflow.cpp`、`test_mediaflow_media_nodes.cpp`、`test_stream_decode.cpp` | 第 21.7.16.5 节全部必选项达到通过标准 |

建议按 `C1+C2`、`C3+C4`、`C5+C6`、`C7` 四批提交。每批都应保持旧的仅消息数
配置可用，避免容量控制改造要求所有现有 Graph 一次性迁移。

##### 21.7.16.4 初始配置原则

初始预算应由“允许积压的媒体时间”反推，而不是直接根据当前帧数比例决定：

```text
recommended_bytes = bitrate_bits_per_second / 8
                    * max_span_seconds
                    * burst_safety_factor
```

建议第一轮现场调参使用以下起点，最终值必须由长测指标确认：

| 场景 | 视频 max span | 音频 max span | 字节安全系数 | 溢出策略 |
|---|---:|---:|---:|---|
| 实时解码/预览 | `1000 ms` | `500 ms` | `2.0` | 视频保护关键帧与恢复窗口；音频丢最旧完整包 |
| 实时转码发布 | `1500 ms` | `1000 ms` | `2.0` | 优先降载或请求关键帧；超过恢复窗口时重建发布入口 |
| 录像/文件转封装 | 不以低延迟值直接限流 | 不以低延迟值直接限流 | 按磁盘吞吐预算 | 使用可中断 `Block`，不得静默丢包 |

无可靠码率时，先使用保守 `max_bytes` 加 `max_items`；收集稳定码率后再计算目标值。
任何场景都必须设置至少一个硬上限，不能同时关闭 items、bytes 和 span 三个维度。

##### 21.7.16.5 验收标准

| ID | 验收项 | 验收方式 | 通过条件 |
|---|---|---|---|
| CAP-01 | 多维记账正确性 | 对 Push/Pop/DropOldest/DropNewest/Open/Close 和中间元素淘汰做确定性单元测试 | items、bytes、span 与队列实际内容逐步一致；Open/Close 后当前值归零，累计指标按契约保留 |
| CAP-02 | 硬上限 | 注入大小变化的音频包、普通视频包和超大关键帧 | `max_items`、`max_bytes` 不被突破；`max_span_us` 最多允许一个不可拆分包的 duration 误差；超大单包有独立原因 |
| CAP-03 | 音视频隔离 | 音频按正常速率的 10 倍连续突发 30 秒，同时按 25 fps 注入视频和周期关键帧 | 音频达到上限不会消耗视频预算；视频关键帧不因音频容量被拒绝；两轨指标可独立查询 |
| CAP-04 | 实时音频延迟 | 阻塞音频消费者后恢复，检查队列最老/最新 PTS | 音频队列 span 回落到配置低水位；不会持续保留超出 `max_span_us` 的最旧音频 |
| CAP-05 | 视频恢复 | 在队列满载、关键帧到达、非关键帧突发和下一关键帧恢复场景下解码 | 丢弃后最迟从下一关键帧恢复输出；不把关键帧后的依赖窗口全部淘汰；丢弃原因可定位 |
| CAP-06 | 时间戳异常 | 注入 `kNoTimestamp`、回退时间戳、跨 generation 消息和 discontinuity | 不跨 generation 计算 span；异常时退回 items/bytes 限制；没有负 span、整数溢出或无界增长 |
| CAP-07 | EOS 与停止 | 在三种上限均已触发以及生产者正在 Block 时执行 EOS、GracefulStop 和 ImmediateStop | 每轨 EOS 最多处理一次且不被普通媒体包丢弃；Block 可被关闭唤醒；GracefulStop 在 `5 s` 预算内返回 |
| CAP-08 | 总在途可观测性 | 同时填充 Edge 和 Node Dispatch，读取轨道快照 | `total_inflight = edge_queue + node_pending` 的 items/bytes 可核对；高水位和丢弃原因不串轨 |
| CAP-09 | 生命周期回归 | 同一 Graph 连续 Start/Stop 100 次，并在高水位时停止 | 无死锁、计数下溢、旧 generation 数据和累计内存增长；现有 MediaFlow 测试全部通过 |
| CAP-10 | 单路摄像头长测 | 仅建立一路 `192.168.66.83` RTSP，连续运行至少 10 分钟，每 5 秒采样一次指标 | A/V 持续输出；队列 items/bytes/span 不呈单调增长；无未分类丢弃；停止正常；进程 RSS 在预热后无持续增长趋势 |
| CAP-11 | 性能回归 | 相同输入和构建配置下，对比改造前后 10 分钟 CPU 与吞吐 | 平均 CPU 增量不超过 5%；视频/音频处理吞吐不降低；指标采集不在媒体热路径执行全队列扫描 |

容量控制阶段只要求记录 A/V PTS 差值并确认没有随运行时间单调发散，不把“达到
某个固定毫秒同步误差”列为本阶段通过条件；固定同步误差应在统一时钟和渲染/发布
调度实施后单独验收。

##### 21.7.16.6 验收结果记录

本节在每批实现完成后更新。C1-C4 的代码能力和确定性单元测试已经完成，但尚未执行
长时间突发、全部停止边界和摄像头现场验收，因此不能把“部分通过”视为最终通过。

| 验收范围 | 当前结果 | 证据或待补内容 |
|---|---|---|
| 第 21.7.15 节入口分轨基线 | 已通过 | 15 秒单路 RTSP：视频 `375/375`、音频 `745/745`，四条相关 Edge dropped/rejected 均为 `0` |
| CAP-01 至 CAP-02：成本与硬上限 | 通过 | `TestQueueBudgetAccounting` 与 `TestMediaQueueItemCostTraits` 已覆盖 items/bytes/span、超大包、generation、EOS 和无时间戳成本 |
| CAP-03：音视频隔离 | 部分通过 | `TestTrackBudgetIsolationAndControlRetention` 与 `TestCapacityRegressionScenarios` 验证独立预算、30 秒媒体时间的 10 倍音频突发和 25 fps 视频接收；双轨真实 Decoder 恢复已补，真实网络队列丢包联动待补 |
| CAP-04：实时音频延迟 | 通过（离线） | `TestRealtimeAudioLowWatermarkRecovery` 验证突发后 span 回落到低水位 |
| CAP-05：视频恢复 | 通过（本地 FFmpeg） | `TestFfmpegDecoderRecoversAfterVideoPacketGap` 使用双轨 H.264/AAC TS，跳过一个带序号的视频包后验证 Decoder 丢弃依赖帧、重开上下文并从下一关键帧新增输出；真实网络/协议丢包互操作待补 |
| CAP-06：时间戳异常 | 通过（离线） | 回退时间戳建立新 epoch，旧/新窗口不拼接；无时间戳、跨 generation 和诊断计数已有回归 |
| CAP-07：EOS 与停止 | 通过（离线） | 三维满载 EOS、超大 Block 包、GracefulStop 中断等待和 ImmediateStop 均有回归 |
| CAP-08：总在途可观测性 | 部分通过 | `TestMediaEdgeAndDispatchInflightMetrics` 核对 `edge_queue + node_pending` 的 items/bytes/span；C5 已补充队列诊断，跨多 Edge 汇总待上层 Pipeline 接入 |
| CAP-09：生命周期回归 | 部分通过 | `TestGraphStartStopStart` 已连续 Start/Stop 100 次，`TestGracefulStopAtMediaHighWatermark` 覆盖高水位停止；内存增长观测待现场长测 |
| CAP-10：摄像头长测 | 部分通过 | 单路 600 秒 A/V 持续输出、队列不增长且诊断全零；RSS 约 430 秒从 41.5 MB 跃升到约 95 MB，需调查 |
| CAP-11：性能回归 | 待执行 | 记录同一机器、同一流、同一构建配置下改造前后的 CPU/吞吐对比 |

每个“待执行”项完成后必须改为“通过”或“未通过”，并附测试命令、关键配置和
指标摘要。存在失败时保留原始结果和原因，不能只覆盖为最新一次成功数据。

##### 21.7.16.7 C1+C2 第一批实施记录

本批完成容量控制的基础契约和队列记账，代码已接入现有 Graph，但未修改实时摄像头
测试的默认 `2048/8192` 消息数配置。未显式设置 `EdgeOptions::budget` 时，
`capacity` 继续作为兼容的 `max_items` 硬上限；设置任意 `max_bytes` 或 `max_span_us`
后，消息数维度可以保持关闭。

实施内容：

- 在 `include/mediaflow/core/types.h` 增加 `QueueBudget`、`QueueItemCost`、
  `QueueLimitReason` 和 `QueueMetricsSnapshot`；校验水位百分比关系及非负时间跨度。
- 在 `QueueTransport` 中为每条消息保存成本，维护当前 `items`、`bytes`、按
  `(generation)` 分组的有效时间跨度，以及三类高水位和容量触发累计计数。
- Push、Pop、DropOldest、DropNewest、Open、Close 均同步更新记账；超大单包不会
  突破 `max_bytes` 或 `max_span_us`，并单独累计 `oversized`。
- `MediaPacketMessage` 使用 DTS 优先、PTS 兜底并按 packet `time_base` 换算微秒；
  `MediaFrameMessage` 直接使用微秒时间戳。两者均按 shared buffer 的逻辑载荷计费，
  EOS 和无时间戳消息不伪造时间跨度。
- Graph 将 `EdgeOptions::budget` 透传给队列，同时保留普通类型零成本 traits 和
  原有 Edge 聚合初始化方式。

本批验证结果：

| 验收项 | 结果 | 证据 |
|---|---|---|
| C1 成本契约 | 通过 | `test_mediaflow_media_nodes` 的 `TestMediaQueueItemCostTraits` 验证音频、视频、EOS、无时间戳和 time base 换算 |
| C2 多维记账 | 通过 | `test_mediaflow` 的 `TestQueueBudgetAccounting` 验证 items/bytes/span、DropOldest、超大单包、generation 隔离和 Open/Close 清零 |
| CAP-01 | 通过 | `test_mediaflow.exe` 退出码 `0`；当前值与队列内容一致，高水位和累计限制计数保留 |
| CAP-02 | 通过 | 10 字节上限、50/100 微秒时间跨度上限及超大消息测试均未突破硬上限 |
| CAP-03 至 CAP-11 | 待执行 | 需要后续 C3-C7 实施和现场长测，不能由本批单元测试代替 |

本批构建环境为 VS 18 Community x64 Debug；执行目标为 `test_mediaflow` 和
`test_mediaflow_media_nodes`，两个程序退出码均为 `0`。本批没有执行摄像头拉流，
遵守设备只允许一路 RTSP 会话的约束。

##### 21.7.16.8 C3+C4 第二批实施记录

本批完成分轨背压的控制消息保护，以及 Edge 队列和 Node Dispatch 之间的在途成本
衔接。容量预算仍由每条 Edge 独立配置，因此音频突发不会占用视频 Edge 的
`items/bytes/span` 额度；A/V 同步调度不在本批范围内。

实施内容：

- 增加 `QueueTrack`，并由 `MediaPacketMessage`、`MediaFrameMessage` 的 traits 填入
  音频、视频或未知轨分类；`EdgeOptions::track` 可为不含媒体类型的自定义消息显式覆盖分类。
- `QueueTransport::DropOldest` 不再淘汰 EOS 等控制消息。队列满载时，普通媒体只能
  替换另一条普通媒体；没有可替换项时拒绝当前消息。
- `DispatchPriority` 增加控制消息标记。Serialized Node Dispatch 满载时，EOS 可以
  替换一个尚未执行的普通媒体任务；视频关键帧的替换策略也跳过控制任务。
- 增加 `InFlightCostTracker`，在 Node Dispatch 入队、替换、执行完成、关闭和重新打开时
  维护总计、音频、视频、未知轨的 `items/bytes/span` 与高水位。Edge Drain 和
  ExecutorDispatch 路径均把成本和轨道分类传入目标 Dispatch。
- `EdgeMetricsSnapshot::budget` 暴露 Edge 当前 `items/bytes/span` 及高水位，
  `NodeMetricsSnapshot` 暴露总和分轨 pending 快照，可由调用方核对
  `total_inflight = edge_queue + node_pending`。

本批验证结果：

| 验收项 | 结果 | 证据 |
|---|---|---|
| C3 分轨背压与 EOS 保护 | 通过 | `TestTrackBudgetIsolationAndControlRetention` 验证音频队列满载后 EOS 保留、视频独立预算仍可接收；`TestVideoPriorityDispatchQueue` 验证 Dispatch 中 EOS 不被音频和关键帧替换策略淘汰 |
| C4 Edge/Dispatch 在途快照 | 通过 | `TestMediaEdgeAndDispatchInflightMetrics` 在阻塞视频 Sink 时核对 Edge `2 items/7 bytes/80000 us`、Node `1 item/2 bytes/40000 us`，并在排空后确认当前值归零 |
| CAP-03 | 部分通过 | 已具备独立预算和控制消息保护；尚缺 30 秒 10 倍音频突发和关键帧恢复解码验证 |
| CAP-06 | 部分通过 | generation、无时间戳和回退时间戳诊断已覆盖；discontinuity 的时间窗口重建待 C7 补测 |
| CAP-07 | 部分通过 | Queue 与 Dispatch 的 EOS 满载保护已覆盖；Block 关闭唤醒和停止组合测试待 C7 补测 |
| CAP-08 | 部分通过 | 单 Edge 到单 Node 的总在途核对已覆盖；多 Edge 汇总待上层 Pipeline 接入 |

本批构建环境为 VS 18 Community x64 Debug。执行 `test_mediaflow.exe` 和
`test_mediaflow_media_nodes.exe`，退出码均为 `0`；未连接摄像头，避免占用设备唯一的
RTSP 会话。

##### 21.7.16.9 C5+C6 第三批实施记录

本批将队列满载从“只看到 `dropped_newest`”推进到可按容量原因、关键帧和时间戳
诊断，并把第一条真实 RTSP 解码图改为显式的分轨预算配置。预算计算不改变
`QueueTransport` 的调用方式，其他 Graph 可以继续使用原有 `capacity`，或按自己的
码率和目标延迟创建 `QueueBudget`。

实施内容：

- `QueueMetricsSnapshot` 增加当前是否处于高水位、进入/离开高水位累计次数、被丢弃
  的关键帧数、无时间戳数和时间戳回退数；原有 items/bytes/span 高水位及
  items/bytes/span/oversized 容量原因继续保留。
- `QueueTransport` 在入队、出队、淘汰和 Open/Close 清理时维护高低水位状态。控制消息
  不计入时间戳异常；带媒体成本的普通消息缺少时间戳或相同 generation 的时间戳回退时，
  分别记录 `timestamp_invalid` 和 `timestamp_discontinuity`，但本批不擅自重写其
  时间轴语义。
- 所有通过 `QueueTransport` 的诊断快照完整透传至 `EdgeMetricsSnapshot::budget`，避免
  Edge 监控只能看到当前 items/bytes/span 而丢失容量和媒体原因计数。
- `QueueBudget::FromBitrate(max_items, bitrate, max_span_us, safety_percent)` 以
  `bitrate * span * safety / 8` 向上取整生成 `max_bytes`，保留 `max_items` 和
  `max_span_us`；码率为零时只启用消息数与时间维度。
- `test_stream_decode` 的视频压缩包边配置为 `16 Mbps / 1000 ms / 256 items`，音频为
  `384 kbps / 500 ms / 512 items`，均使用 2 倍突发系数。音频实时边改为
  `DropOldest`；视频仍使用 `PreferVideoKeyframes`。视频和音频解码帧边分别配置
  items/span，未用压缩码率错误推算原始帧字节数。
- `test_stream_decode` 最终输出四条压缩包 Edge 的诊断明细，包括当前队列、三个历史
  高水位、进出水位次数、容量原因、超大包、关键帧和时间戳计数。

本批验证结果：

| 验收项 | 结果 | 证据 |
|---|---|---|
| C5 队列诊断 | 通过 | `TestQueueDiagnosticsAndBudgetFormula` 验证高/低水位进出、关键帧丢弃、无时间戳、时间戳回退和容量原因；`TestMediaEdgeAndDispatchInflightMetrics` 验证完整快照透传到 Edge |
| C6 实时解码配置接入 | 通过 | `test_stream_decode` 已按音频、视频、压缩包、解码帧四类边建立独立预算并成功编译；配置由 `QueueBudget::FromBitrate` 生成，不再采用固定 `2048/8192` 默认值 |
| CAP-06 | 部分通过 | 已记录异常和回退；discontinuity 的时间窗口重建、变码率和跨 generation 压力回归待 C7 完成 |
| CAP-08 | 部分通过 | 单 Edge 与 Node pending 的核对、Edge 容量诊断均已完成；多 Edge 汇总待 Pipeline 上层入口实施 |
| CAP-10 至 CAP-11 | 待执行 | 未连接摄像头，未执行 10 分钟单路长测和性能基准，以免影响设备唯一 RTSP 会话 |

本批构建环境为 VS 18 Community x64 Debug。已构建 `test_mediaflow`、
`test_mediaflow_media_nodes` 和 `test_stream_decode`；前两个测试程序退出码均为 `0`。
`test_stream_decode` 未运行，未产生摄像头会话。

##### 21.7.16.10 C7 第四批实施记录

本批完成不依赖设备的确定性容量回归。测试中的“30 秒”是媒体时间轴长度，音频以
10 倍速注入，因此能在毫秒级完成、不会额外创建摄像头 RTSP 会话。现场长测仍需在
设备可用且只建立一路会话时单独执行。

实施内容：

- 增加 `TestCapacityRegressionScenarios`：连续注入 1500 个 20 ms、变字节数的音频包，
  同时按 25 fps 注入并立即消费 750 个视频包。验证音频 Edge 的 items/bytes/span 始终
  不超过独立预算，音频淘汰不影响视频全部接收。
- 同一测试注入回退时间戳和新 generation，验证 span 为非负且仅取单个 generation 的
  最大窗口，并检查 `timestamp_discontinuity`。
- 增加满载 `BackpressurePolicy::Block` 的生产者线程测试，`Close()` 后等待中的
  `Send()` 返回 `Closed`，证明停止可中断阻塞生产者。
- 增加 `TestGracefulStopAtMediaHighWatermark`：阻塞首个视频处理任务，填满并触发
  `DropOldest`，随后释放消费者；`Graph::GracefulStop(2s)` 成功排空并进入 `Stopped`。
- 保留已有 `TestGraphStartStopStart` 的 100 次 Start/Stop 回归，和本批高水位停止测试
  共同覆盖生命周期的主要离线边界。

本批验证结果：

| 验收项 | 结果 | 证据 |
|---|---|---|
| CAP-03 音视频隔离 | 部分通过 | 30 秒媒体时间、10 倍音频突发和 25 fps 视频的独立队列回归通过；尚未验证真实 Decoder 在丢包后从下一关键帧恢复 |
| CAP-06 时间戳异常 | 部分通过 | 回退时间戳诊断和跨 generation 非拼接 span 通过；discontinuity 后重新建立时间窗口待实现 |
| CAP-07 EOS 与停止 | 部分通过 | EOS 满载保护、Block Close 唤醒和高水位 GracefulStop 通过；ImmediateStop 与三类上限同时触发待补 |
| CAP-09 生命周期回归 | 部分通过 | 100 次 Start/Stop 与高水位停止通过；无持续内存增长需要现场长测或专用内存工具确认 |
| CAP-10 至 CAP-11 | 待执行 | 本批没有连接 `192.168.66.83`，未执行单路 10 分钟长测和性能对比 |

本批执行 VS 18 Community x64 Debug 的 `test_mediaflow.exe` 和
`test_mediaflow_media_nodes.exe`，退出码均为 `0`。未运行 `test_stream_decode`，避免
占用设备唯一 RTSP 会话。

##### 21.7.16.11 C7 补充实施与现场验收记录

本批继续完成 C7 的离线边界，并执行一次单路摄像头 10 分钟长测。容量控制的
硬上限、时间窗口和停止语义已经落地；现场结果中发现的 RSS 跃升仍保留为后续
问题，不能把本批整体标记为最终通过。

实施内容：

- `QueueTransport` 对 `Block` 策略先判断单条消息是否为 `Oversized`。超出
  `max_bytes` 或不可拆分的 `max_span_us` 时立即返回 `DroppedNewest`，不会永久等待
  消费者释放空间。
- 新增 `MailboxPushResult::Interrupted` 和 `InterruptBlockedSenders()`。Graph 在
  `GracefulStop` 请求 Source 停止生产后，只取消已经处于 Block 等待的发送者，不关闭
  或清空 Edge，因此已进入 Graph 的尾部媒体仍可排空，后续 Flush 不受影响。
- 同一 `generation` 内的回退时间戳建立新的 `(generation, epoch)` 时间窗口。旧窗口
  与新窗口分别计算 span，队列条目保存所属窗口，淘汰时按窗口精确移除，避免回退、
  重连或时间轴重置被错误计算为超长积压。
- 新增 `TestRealtimeAudioLowWatermarkRecovery`、`TestVideoKeyframeRecoveryWindow`、
  `TestTimestampDiscontinuityRebuildsTimeWindow` 和 `TestCapacityStopBoundaries`，
  覆盖音频低水位恢复、关键帧恢复窗口、时间戳重建、三维满载 EOS、超大 Block 包、
  GracefulStop 和 ImmediateStop。
- `test_stream_decode` 的采样线程已前移到实际运行开始后，每 5 秒记录四条 Edge 的
  items/bytes/span、帧数、pending、丢弃/拒绝/时间戳诊断和 Windows 工作集 RSS；
  采样只读取快照，不执行队列扫描。

离线验收：

| 验收项 | 结果 | 证据 |
|---|---|---|
| CAP-04 实时音频延迟 | 通过 | `TestRealtimeAudioLowWatermarkRecovery` 验证突发后 span 为 `100000 us`，消费恢复后回落到 `60000 us` 并离开高水位 |
| CAP-05 视频恢复 | 部分通过 | `TestVideoKeyframeRecoveryWindow` 验证满载时关键帧替换非关键帧、关键帧后的依赖窗口保持连续；尚未接入真实 FFmpeg 解码器做丢包后解码验收 |
| CAP-06 时间戳异常 | 通过（离线） | `TestTimestampDiscontinuityRebuildsTimeWindow` 验证回退后新 epoch、旧窗口不与新窗口拼接、span 上限仍生效；无时间戳、跨 generation 和回退诊断已有覆盖 |
| CAP-07 EOS 与停止 | 通过（离线） | `TestCapacityStopBoundaries` 验证 items/bytes/span 满载时 EOS 保留、超大 Block 包立即拒绝、GracefulStop 中断等待后排空、ImmediateStop 关闭并唤醒 |
| CAP-09 生命周期回归 | 部分通过 | `test_mediaflow.exe` 和 `test_mediaflow_media_nodes.exe` 均通过；100 次 Start/Stop 无死锁，内存趋势仍需专用工具重复确认 |

现场长测命令与配置：

```text
build-mediaflow-vs/bin/test_stream_decode.exe --duration-ms 600000
URL: rtsp://192.168.66.83/live/mainstream
RTSP transport: tcp
```

本次只建立一路摄像头会话。重定向日志为
`build-mediaflow-vs/test_stream_decode_long.out` 和
`build-mediaflow-vs/test_stream_decode_long.err`，最终结果为视频解码
`15001/15001`、音频解码 `32434/32434`，四条 Edge 的最终 items/bytes/span 均为
`0/0/0`，dropped、rejected、timestamp diagnostics 均为 `0`，Source 正常 finished，
GracefulStop 完成。运行期间 A/V 帧持续增长，未观察到队列单调增长。

现场问题与状态：

| 问题 | 现象 | 状态与后续 |
|---|---|---|
| 初始探测告警 | FFmpeg 日志出现一次 `Could not find codec parameters ... unspecified size`，随后持续输出 1920x1080 视频和音频 | 可恢复但未消除；下一阶段检查 Puller 探测参数与告警分类，不能将告警当作完全无问题 |
| RSS 跃升 | 约 360 秒前工作集约 `41.5 MB`，约 430 秒跃升至 `94.9 MB`，结束时约 `95.2 MB`；队列和诊断未同步增长 | 未通过，列为必须调查的问题；需要增加进程/FFmpeg buffer 分层统计并重复长测，定位是否为解码器缓存、帧引用或系统工作集行为 |
| 瞬时 pending | 个别 5 秒采样出现 video decoder 或 audio counter pending `1`，下一采样恢复，Edge 仍为 `0/0/0` | 观察通过，不构成积压；后续性能基线中继续记录峰值 |

因此当前 C7 结论为：CAP-04、CAP-06、CAP-07 的离线验收通过，CAP-05 已通过本地
真实 FFmpeg 解码验证但仍缺真实网络丢包互操作，CAP-09 部分通过，CAP-10 部分通过
且被 RSS 问题阻断最终通过，CAP-11 仍待无坏包媒体源的最终性能复测。A/V 轨道容量
控制仍不能宣称已经完成全部现场验收。

#### 21.7.17 下一阶段：FFmpeg 解码帧内存诊断与 CAP-10 复验

本阶段先处理上一阶段长测发现的 RSS 跃升问题，目标是把“仍有媒体帧被引用”与
“对象已经释放但 Windows 工作集或 FFmpeg 内部缓存仍保留”区分开，再决定是否
需要零拷贝或延迟 pack 改造。当前不直接改变 `FFmpegFrameBuffer` 的数据所有权，
避免在没有泄漏证据时扩大解码链路风险。

##### 21.7.17.1 已实施修改

- `FFmpegFrameBuffer` 增加线程安全的分层内存快照：活跃 wrapper 数量、packed
  数据字节数、AVFrame 引用的 FFmpeg `AVBuffer` payload 字节数，以及三项峰值。
- 统计在 wrapper 构造成功后登记，在析构前扣除；按 FFmpeg 的固定 buffer 引用和
  `extended_buf` 额外引用累加，不创建临时容器。统计读取只使用原子快照，不扫描队列。
- `test_stream_decode` 每 5 秒同时记录 RSS 和帧缓冲快照，并在 `GracefulStop`
  后检查活跃帧缓冲回到零。
- `test_ffmpeg_decoder_raw_packet` 增加解码器关闭后的资源基线回归，确认 raw
  packet 路径没有遗留 `FFmpegFrameBuffer`。

##### 21.7.17.2 验证结果

以下回归测试通过：

- `test_mediaflow.exe`；
- `test_mediaflow_media_nodes.exe`；
- `test_ffmpeg_decoder_raw_packet.exe`，输出单帧峰值约为 packed `3110400`
  字节、FFmpeg payload `3139437` 字节，关闭后存活统计回到基线。
- 摄像头 `192.168.66.83` 单路 RTSP TCP 长测 600 秒，退出码 `0`。第二轮使用
  cmd 原始重定向保存完整日志：`build-mediaflow-vs/test_stream_decode_memory_cmd.out`
  和 `.err`。
- 第二轮长测视频 Decoder `14993/14993`、音频 Decoder `33417/33417`，四条
  Edge 最终 `items/bytes/span=0/0/0`，dropped/rejected/timestamp diagnostics
  均为 `0`，`GracefulStop` 完成。
- 长测采样中活跃 frame wrapper 通常为 `0`，偶发在处理瞬间为 `1`；峰值
  wrapper `12`、packed `3111040` 字节、AVBuffer `3140077` 字节。RSS 最高约
  `45.1 MB`，后段回落，未与帧数、Edge 或活跃 frame bytes 单调同步增长。

##### 21.7.17.3 阶段结论与后续

本阶段未发现 `MediaFrame`/`FFmpegFrameBuffer` 存活对象持续增长，原有 RSS 跃升
不能归因于当前帧 wrapper 泄漏，CAP-10 的队列和帧引用边界已通过本轮复验。RSS
仍可能来自 FFmpeg 解码上下文、网络/格式探测缓存或 Windows 工作集保留，后续应
使用专用堆分析工具或增加解码上下文级基线后再继续定位。

CAP-11 性能回归尚未完成：下一阶段建立同一机器、同一 URL、同一 Debug/Release
构建配置下的 CPU、解码吞吐、采样开销基线，并补真实 FFmpeg 丢包后的关键帧恢复
验收。零拷贝和延迟 pack 暂列 P2，待性能基线确认双份数据确实构成可接受的主要
成本后再实施。

#### 21.7.18 CAP-11 当前版本 CPU 与吞吐基线

本阶段为性能回归增加统一采集口径。测试只统计稳定运行区间：在进入
`GracefulStop` 前记录墙钟和进程 CPU 时间，并使用此刻已完成的 A/V 帧数计算吞吐，
不把停止阶段的 Decoder Flush 尾帧混入性能数据。采样线程仍只读取原子指标快照，
不执行队列遍历。

##### 21.7.18.1 已实施修改

- `test_stream_decode` 增加 Windows `GetProcessTimes` 进程 CPU 时间采集，输出
  kernel/user 合计的 100 ns 时间和单核等效 CPU 百分比。
- 增加稳定运行墙钟、采样次数、停止前视频/音频帧率输出；CPU 和吞吐采用同一
  时间边界，便于后续与改造前构建比较。
- 采集失败或墙钟为零时 CPU 百分比输出 `-1`，不会伪造可比较数据。

##### 21.7.18.2 当前版本采集结果

使用当前工作区构建的 `test_stream_decode --duration-ms 600000`，建立一个包含
视频轨和音频轨的 RTSP TCP 会话：`rtsp://192.168.66.166/live/mainstream`。
本次不使用单轨输入，结果如下：

| 指标 | 当前版本结果 |
|---|---:|
| 稳定运行墙钟 | `600015 ms` |
| 进程 CPU（单核等效） | `13.60%` |
| 视频吞吐 | `24.96 fps`，停止前 `14978` 帧 |
| 音频吞吐 | `50.00 fps`，停止前 `30001` 帧 |
| 指标采样次数 | `120` |
| 活跃帧峰值 | `3 wrappers / 3111040 packed bytes / 3140077 AVBuffer bytes` |
| 最终活跃帧 | `0 / 0 / 0` |
| 最终 Edge 诊断 | dropped/rejected/timestamp `0` |

完整日志：`build-mediaflow-vs/test_stream_decode_perf_66166_current.out` 和
`build-mediaflow-vs/test_stream_decode_perf_66166_current.err`。本次运行 RSS 峰值约
`39.0 MiB`，没有与活跃帧或 Edge 队列同步单调增长，停止后活跃帧为 `0/0/0`。

##### 21.7.18.3 验收状态与后续

CPU/吞吐采集能力和当前版本的 66.166 双轨基线已完成，CAP-11 仍不能标记为最终
通过。上一已提交版本的双轨对照在约 73 秒时同时结束视频和音频输入，FFmpeg
报告 `Failed reading RTSP data: End of file`；进程随后只是等待测试时长结束，最终
得到视频 `1810` 帧、音频 `3662` 帧，不能作为 600 秒吞吐或 CPU 对照。结合现场
判断，本次断流归因于摄像头高温不稳定，暂不归因于 MediaFlow，也不据此修改代码。
设备冷却并确认稳定后，应在同一 URL、同一构建配置下串行运行至少三轮双轨 600 秒
测试，再按中位数比较 CPU 增量和 A/V 吞吐；基线若再次中途断流，则该轮仍应标记
为设备异常无效样本。真实 Decoder 丢包关键帧恢复已在第 21.7.20 节补充离线验证，
后续仍需在受控网络丢包或协议层丢包环境中验证 Edge 丢弃与 Decoder 恢复统计的一致性。

#### 21.7.19 改造前双轨对照：摄像头高温导致样本无效

本次对照只使用 `192.168.66.166/live/mainstream`，且每次只建立一个 RTSP
会话；该会话在探测阶段明确包含 1 路视频和 1 路音频，不使用视频单轨或音频单轨
测试。隔离构建基于提交 `fcd4a78`，除将测试 URL 指向 66.166 外没有修改 MediaFlow
实现。

对照先进行 10 秒双轨连通性验证：视频解码 `228` 帧，音频解码 `500` 帧，视频
和音频 Edge 的 dropped、rejected、timestamp 诊断均为 `0`，说明当时输入确实是
双轨且短时链路可用。

随后进行 600 秒双轨长测。约 73 秒时，视频和音频同时停止增长，stderr 出现两条
`Failed reading RTSP data: End of file`，Source 标记为 `finished=1`；截至断流前
已解码视频 `1810` 帧、音频 `3662` 帧，Edge dropped/rejected/timestamp 仍为 `0`。
程序因测试时长参数继续等待到约 `602790 ms` 后退出，外部采集到的单核 CPU
`1.68%` 仅反映断流后的空等阶段，不具备性能比较意义。

根据现场判断，设备处于高温状态导致 RTSP 会话不稳定。本条记录归类为测试环境的
设备异常，不作为 MediaFlow 缺陷或单轨架构问题；本轮不修改代码，CAP-11 保持
“待稳定设备复测”。复测前需先冷却或更换稳定设备，并继续遵守单设备单 RTSP
会话、音视频双轨和串行测试约束。

##### 21.7.19.1 备用复测方法：本机 FFmpeg 双轨循环推流

为避免摄像头温度、网络和设备重启影响，可使用本机视频文件通过 FFmpeg 推送到
本机 RTSP 服务，再让当前版本和 `fcd4a78` 基线分别拉取同一个本地 RTSP URL。
本方法已完成一轮双轨 600 秒初步对照，最终验收仍需使用无坏包的稳定媒体源复测。

已确认的视频目录为：
`D:\file_mx\新建文件夹 (2)\初级go工程师训练营\17第十五周：支付服务设计与实现`。
其中三段 MP4 均为双轨输入：H.264 视频 `1920x1242/25 fps` 加 AAC 音频
`44100 Hz/2 channels`，时长约 2.8 至 3.1 小时，满足 600 秒长测需求。

FFmpeg 位于：
`D:\file_mx\aaaaa\learncpp\tools\win32\ffmpeg-2025-05-01-git-707c04fe06-full_build\ffmpeg.exe`。
建议先启动本机 ZLMediaKit 的 RTSP 服务，并确认监听端口，例如 `554`；再使用
以下命令循环、实时推送一段双轨文件，避免文件播放到尾部产生 EOS。由于源文件
的 AAC 没有 RTSP muxer 所需的 global header，不能直接使用 `-c copy`；实际推流
保留视频码流复制，仅将音频重新编码为 AAC：

```powershell
& 'D:\file_mx\aaaaa\learncpp\tools\win32\zlmediakit\MediaServer.exe' '-c' 'D:\file_mx\aaaaa\learncpp\tools\win32\zlmediakit\config.ini'

& 'D:\file_mx\aaaaa\learncpp\tools\win32\ffmpeg-2025-05-01-git-707c04fe06-full_build\ffmpeg.exe' `
  '-re' '-stream_loop' '-1' `
  '-i' 'D:\file_mx\新建文件夹 (2)\初级go工程师训练营\17第十五周：支付服务设计与实现\第四十九讲：支付服务实现（二） .mp4' `
  '-map' '0:v:0' '-map' '0:a:0' '-c:v' 'copy' '-c:a' 'aac' `
  '-ar' '44100' '-ac' '2' '-b:a' '128k' `
  '-f' 'rtsp' '-rtsp_transport' 'tcp' `
  'rtsp://127.0.0.1:554/mediaflow/benchmark'
```

推流建立后，将 `test_stream_decode` 的 URL 设置为
`rtsp://127.0.0.1:554/mediaflow/benchmark`，分别运行当前版本和基线版本，
每轮只保留一个拉流会话，测试结束后先停止拉流程序，再停止 FFmpeg 推流程序。
验收仍要求视频和音频均被探测和解码，视频/音频 Edge 的 dropped、rejected、
timestamp 均为 `0`，并完成 600 秒稳定吞吐和 CPU 采集；不使用视频单轨或音频
单轨样本替代双轨结果。

##### 21.7.19.2 本机双轨 600 秒初步对照结果

本轮复测复用了已运行的本机 ZLMediaKit `127.0.0.1:554`，每次只建立一个拉流
会话，推流和拉流均使用 TCP。输入文件为第四十九讲 MP4，测试 URL 为
`rtsp://127.0.0.1:554/mediaflow/benchmark`。当前版本使用工作区提交
`0b404bc` 构建，基线使用隔离工作树中提交 `fcd4a78` 构建；两次测试使用相同
的 FFmpeg 推流命令并串行执行。

| 指标 | 当前版本 | `fcd4a78` 基线 |
|---|---:|---:|
| 测试墙钟 | `600002 ms` | `600487 ms` |
| 进程 CPU（单核等效） | `9.79%` | `9.51%` |
| 视频解码吞吐 | `24.73 fps`，`14841` 帧 | `24.73 fps`，`14848` 帧 |
| 音频解码吞吐 | `42.99 fps`，`25796` 帧 | `42.97 fps`，`25803` 帧 |
| 音频启动丢弃 | `dropped_oldest=8` | `dropped_oldest=13` |
| Decoder rejected | `0` | `0` |
| timestamp 诊断 | `0` | `0` |
| 当前版本停止后帧内存 | `wrappers/packed/AVBuffer=0/0/0` | 基线未采集该指标 |

当前版本相对基线 CPU 增加 `0.28` 个百分点，约 `2.9%` 相对增幅，低于 CAP-11
规定的 `5%`；视频和音频吞吐没有下降。本轮可作为性能回归的初步通过样本，但
不能直接替代最终验收，因为 FFmpeg 在读取该训练视频的原始 AAC 音轨时报告了
少量坏包。坏包发生在推流源侧，MediaFlow 两版均未出现 Decoder rejected 或
timestamp 错误；完整日志如下：

- 当前版本：`build-mediaflow-vs/test_stream_decode_local_current_600s.out`、
  `build-mediaflow-vs/test_stream_decode_local_current_600s.err`；
- 当前版本推流：`build-mediaflow-vs/ffmpeg_local_dual_current_600s.err`；
- 基线版本：隔离目录
  `build-mediaflow-baseline-66166/test_stream_decode_local_baseline_600s.out`、
  `build-mediaflow-baseline-66166/test_stream_decode_local_baseline_600s.err`。

##### 21.7.19.3 本轮测试问题与处理结论

1. 直接使用 `-c copy` 推送失败。FFmpeg 报告 `AAC with no global headers is
   currently not supported`，因此改为视频复制、音频 AAC 转码；这不是 MediaFlow
   的失败。
2. 三段训练视频的原始 AAC 在 60 秒扫描中均出现坏包，不能作为无错误音频源；本轮
   仍保留原文件音频以保证对照输入真实一致，因此结果标记为初步样本。
3. 尝试实时重新编码视频时，FFmpeg 推流速度接近 `1x`，推流端自身跟不上并导致
   MediaFlow 队列丢弃持续增长；该方案会把推流端编码能力混入测试，已废弃。
4. 尝试由 FFmpeg 生成独立音频时，未给第二个 `lavfi` 输入配置实时读取会造成音频
   高速突发；补充 `-re` 后本机 RTSP 服务仍出现周期性接入突发，因此没有采用该
   结果作为最终长测数据。

后续正式验收应先准备一段经过完整校验的 H.264/AAC 双轨文件，继续使用视频复制、
音频复制或无需额外实时转码的推流方式，至少串行复测三轮 600 秒；禁止改成视频
单轨或音频单轨来规避音视频队列问题。

#### 21.7.20 下一阶段：真实 Decoder 丢包后的关键帧恢复

本阶段承接 21.7.18 和 21.7.19 中“补真实 FFmpeg 丢包关键帧恢复验收”的要求。
目标是让队列或 Dispatch 丢包能够被 Decoder 识别，并把视频输出边界明确收敛到
“丢包后等待下一关键帧”，而不是继续用损坏的参考帧输出不可预测的画面。本阶段
不连接摄像头，不改变当前唯一 RTSP 会话的现场测试约束，也不把单轨输入作为验收
样本；回归使用仓库内的 H.264/AAC 双轨 TS 文件。

##### 21.7.20.1 已实施修改

- `MediaPacketMessage` 增加 `sequence`。`StreamSourceNode` 对音频和视频分别维护
  代次内递增序号，重连后从新的序号窗口开始；旧的手工构造消息保持 `0` 时仍可
  兼容，不强制所有非网络 Puller 立即改造。
- `DecoderNode` 增加视频输入序号间隙和乱序检测。发现同一 generation 内的间隙
  后立即关闭旧 Decoder，不执行会输出不完整 GOP 尾帧的 Flush，并进入等待关键帧
  状态。
- Decoder 在等待状态只接受视频关键帧，后续非关键帧被丢弃并计数；关键帧到达后
  按当前 `StreamInfo` 重开 Decoder，成功解码后解除等待。具体 Decoder 返回 false
  时复用同一恢复路径。
- 增加 `DecoderRecoveryStats` 和 `DecoderNode::RecoveryStats()`，记录序号间隙、
  解码失败、等待期间丢弃包和成功恢复关键帧，便于与 Edge 的 dropped/rejected
  指标交叉定位。
- `test_mediaflow_media_nodes` 增加
  `TestFfmpegDecoderRecoversAfterVideoPacketGap`：读取双轨 H.264/AAC TS，同时
  运行音频 Decoder；视频跳过一个非关键包并继续送入后续包，验证视频等待下一
  关键帧后产生新的解码输出，音频轨仍能正常解码。

##### 21.7.20.2 验证结果

| 验收项 | 结果 | 证据 |
|---|---|---|
| Source 代次内序号 | 通过 | Source 为音频、视频分别计数，重连时清零；旧消息序号为 `0` 的兼容路径保持可用 |
| Decoder 丢包状态机 | 通过 | 新测试断言 `sequence_gaps >= 1`、`dropped_until_keyframe >= 1`、`recovered_keyframes >= 1`，且最终 `awaiting_keyframe=false` |
| 真实 FFmpeg 视频恢复 | 通过（本地双轨） | 双轨 TS 中视频跳过一个非关键包后，真实 `FFmpegDecoder` 重开上下文并在下一关键帧后新增视频帧 |
| 音频并行影响 | 通过（本地双轨） | 同一测试的 AAC 音频 Decoder 和音频 Sink 均收到解码帧，未使用单轨替代输入 |
| 既有 MediaFlow 回归 | 通过 | `test_mediaflow.exe`、`test_mediaflow_media_nodes.exe` 均退出码 `0`；`test_stream_decode` 已重新编译通过 |

##### 21.7.20.3 边界与下一步

本阶段的“真实”指使用真实 FFmpeg Puller、H.264/AAC 压缩包和 FFmpeg Decoder，
丢包由测试在 MediaFlow 消息边界注入序号间隙。它已经验证 Decoder 的重建行为，
但还没有模拟 RTSP UDP 丢 RTP 包、TCP 断流重连或真实 Edge 满载后的自动统计联动。
因此 CAP-05 的本地 FFmpeg 验收可以通过，网络互操作验收仍需单独补充。

下一阶段进入统一时钟调度：保留 PTS/DTS、time base、generation 和 discontinuity，
以音频时钟作为实时播放参考，视频按照 PTS 等待或丢弃迟到的非关键帧；编码发布
链路则按 DTS 做有界交织。该阶段不能通过音频帧数与视频帧数配对实现，也不能把
当前 Decoder 的恢复状态误认为已经完成音视频同步。
