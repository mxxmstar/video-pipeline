# AVTP 模块嵌入当前工程评估

## 背景

本文评估 `E:\project\camera-player\include\avtp` 及其配套实现如何嵌入当前工程 `E:\share\project\video-pipeline`。

源工程中的 AVTP 相关代码主要包括：

- `include/avtp/avtp_packet_parser.h`
- `include/avtp/avtp_h264_assembler.h`
- `include/avtp/avtp_payload_assembler.h`
- `src/avtp/*.cpp`
- `include/puller/avtp_puller.hpp`
- `src/puller/avtp_puller.cpp`

当前工程中可复用的关键模块包括：

- `EthernetCapture`
- `IPuller`
- `MediaStreamSession`
- `MediaStreamSource`
- `MediaPacket`
- `FFmpegDecoder`
- CMake 选项 `ENABLE_PCAP`

结论是：AVTP 模块适合作为一个新的媒体输入协议接入当前工程，但应该嵌入现有媒体管线，而不是整体移植源工程播放器或 UI。

## 推荐接入形态

建议将 AVTP 作为 `IPuller` 的一种实现接入：

```text
Npcap
  -> EthernetCapture
  -> AVTP parser / assembler
  -> AvtpPuller : IPuller
  -> MediaStreamSession
  -> MediaStreamSource
  -> FFmpegDecoder
  -> render / inference / publish
```

不建议移植源工程中的 `AvtpPlayer`、`CaterAvtpPlayer` 或 Qt UI。当前工程已经有较完整的媒体流会话、源管理、解码和发布链路，AVTP 只需要补齐输入侧即可。

## 推荐目录布局

建议按当前工程的媒体模块组织方式放置 AVTP 代码：

```text
include/media/protocol/avtp/
src/media/protocol/avtp/
include/media/puller/avtp_puller.h
src/media/puller/avtp_puller.cpp
test/media/avtp/
```

其中：

- `protocol/avtp` 放协议解析、H.264/H.265/JPEG payload 组帧逻辑。
- `media/puller/avtp_puller` 只负责把以太网帧转换为当前工程可消费的 `MediaPacket`。
- `test/media/avtp` 保留源工程已有协议测试，并补充与当前工程媒体管线相关的集成测试。

当前工程已经有 `EthernetCapture`，不需要复制源工程中的抓包实现。这样可以避免 Npcap 加载、网卡枚举、抓包生命周期在两个地方各维护一套。

## 已验证情况

已用 MinGW `g++ -std=c++17` 独立编译并运行源工程 AVTP 协议测试，共 13 个测试通过：

```text
All AVTP protocol tests passed.
```

这说明 parser / assembler 的核心协议逻辑具备迁移基础。真正的集成风险主要不在 AVTP 协议类本身，而在它和当前工程媒体抽象的接口边界上。

## 关键集成断点

### 1. FFmpegDecoder 当前只接受 FFmpeg 后端包

当前工程的 `FFmpegDecoder` 只接受 `BackendHandle::FFMPEG`，而 AVTP puller 输出更适合是普通内存 buffer，backend 通常是 `NONE`。

如果不改 decoder，AVTP 输出进入解码器时会触发类似错误：

```text
non-FFmpeg backend not supported
```

建议让 `FFmpegDecoder` 支持普通 `IMediaBuffer`：当输入不是 FFmpeg backend 时，临时创建一个 `AVPacket`，把普通字节包包装成 FFmpeg 可解码的 packet。这个处理可以参考源工程 `E:\project\camera-player\src\decoder\ffmpeg_decoder.cpp` 中对普通 buffer 的支持方式。

### 2. `format=auto` 与当前静态 StreamInfo 机制不兼容

源工程 `AvtpPuller::Open()` 允许格式自动探测，但当前工程的 `MediaStreamSession::Start()` 通常只在 `Open()` 后分发一次 `MultiStreamInfo`。

如果 `Open()` 返回时 codec 仍是 `UNKNOWN`，后续即使 AVTP puller 从数据中识别出 H.264/H.265/JPEG，也缺少一条成熟的 StreamInfo 变更通知链路。

第一阶段建议要求 URL 显式携带格式：

```text
format=h264
format=h265
format=jpeg
```

后续如果需要自动探测，再增加 StreamInfo changed 回调或重新协商机制。

### 3. `MediaType` 枚举存在重复值风险

当前工程中 `MediaType` 类似如下：

```cpp
enum class MediaType {
    VIDEO,       // 0
    AUDIO,
    PCAP,
    UNKNOWN = 0
};
```

`UNKNOWN` 与 `VIDEO` 同值，这会让默认值和真实视频类型混淆。AVTP 接入时如果依赖 `UNKNOWN` 表示尚未识别，就可能被误判为视频。

建议调整为：

```cpp
enum class MediaType {
    UNKNOWN = 0,
    VIDEO = 1,
    AUDIO = 2,
    PCAP = 3
};
```

该改动会影响共享类型定义，需要检查现有代码是否隐含依赖旧枚举值。

### 4. StreamInfo API 与源工程已有分叉

源工程 AVTP puller 使用了 `SetVideoDetail()`、`AddStream()` 等接口，但当前工程的 StreamInfo 结构没有这些方法。

迁移时不能直接照搬 `AvtpPuller`，需要按当前工程的数据结构手工填充：

- `MediaStreamInfo.detail`
- `MultiStreamInfo.stream_infos`
- `video_stream_idx_`

第一阶段可只支持单路视频流，降低 StreamInfo 复杂度。

### 5. CMake 需要按 `ENABLE_PCAP` 分层

AVTP parser / assembler 不依赖 pcap，应始终编译并运行单元测试。

`AvtpPuller` 依赖 `EthernetCapture` 和 Npcap，应放在 `ENABLE_PCAP=ON` 条件下编译。当前工程在 `ENABLE_PCAP=OFF` 时已排除 `ethernet_capture.cpp`，新增 AVTP puller 后也需要同步排除，否则会出现无 pcap 环境下的链接或编译问题。

## 源 AVTP 实现的主要风险

### AVTP subtype 命名与标准定义不一致

按 IEEE 1722-2016 参考实现 libavtp 的定义，常见 subtype 应理解为：

```cpp
AVTP_SUBTYPE_AAF = 0x02
AVTP_SUBTYPE_CVF = 0x03
AVTP_SUBTYPE_CRF = 0x04
AVTP_SUBTYPE_ADP = 0xFA
AVTP_SUBTYPE_AECP = 0xFB
AVTP_SUBTYPE_ACMP = 0xFC
```

源工程当前定义为：

```cpp
constexpr uint8_t kSubtypeCvf = 0x02;
constexpr uint8_t kSubtypeCustom = 0x03;
```

这与标准 subtype 表不一致。结合样例 pcap 看，`subtype=0x03` 才是真正的视频 CVF 包，且 `format=0x02`、`format_subtype=0x01`，对应 H.264；而 `subtype=0x02` 是 AAF 音频类包，不应按 CVF 视频格式解析。

因此迁移时建议先把 parser 层改成 subtype 分发：

- `0x02`：AAF，第一阶段可直接跳过，后续如需音频再单独实现。
- `0x03`：CVF，解析 `format`、`format_subtype`、marker、payload。
- `0xFA/0xFB/0xFC`：AVDECC 控制面，只有抓包中实际出现且需要发现/连接管理时再实现。

这比保留 `kSubtypeCustom=0x03` 更贴合协议，也能解释当前 pcap 中“0x02 看起来像信息帧但拿不到视频格式”的现象。

### H.265 / JPEG 丢包恢复不够严格

`AvtpPayloadAssembler` 遇到序列号缺口时只统计丢包，但仍继续拼接 H.265/JPEG access unit，可能输出损坏帧。

建议参考 H.264 assembler 的策略：一旦发现缺口，丢弃当前 access unit，并进入 `dropping_until_marker` 状态，直到下一个 marker 后再恢复。

### 私有头和 RTP-like 头语义尚未完全收口

源工程 parser 能识别 8 字节私有前缀，以及后续 12 字节 RTP-like 头；但 puller 当前只剥离 8 字节，可能把 RTP-like 头当作媒体 payload 送入解码器。

建议让 parser 输出统一的 `media_payload`，puller 不再自己理解私有头长度。协议细节尽量集中在 parser 层，puller 只消费“已经规整好的媒体数据”。

不过从样例 pcap 看，`subtype=0x03` 的视频 payload 直接以 Annex-B start code 开始，并没有出现源工程定义的 8 字节私有前缀。因此私有前缀应作为设备兼容分支处理，而不应作为 `0x03` 的默认解释。

### AVTP timestamp 尚未转成媒体时间轴

当前实现解析了 AVTP timestamp，但构造 `MediaPacket` 时主要使用 Npcap capture timestamp。

这意味着目前还没有处理：

- TV / TU / MCR 标志
- 32 位 timestamp 回绕
- gPTP 时钟映射
- 音视频同步

第一阶段这可以接受，目标是“单路视频先跑起来”。如果后续要做 AVB/TSN 级别同步，需要单独设计 `AvtpTimestampMapper`。

### 当前实现是设备画像，不是完整 IEEE 1722 栈

当前 AVTP 代码假定 payload 是可直接拼接的 Annex-B 分片，并以 marker 表示 access unit 结束。这更像是针对现有设备输出格式的解析器，不是一个完整通用的 IEEE 1722/AVB 协议栈。

这不影响工程接入，但需要在文档和配置中说清楚：第一阶段支持的是当前设备的 AVTP 视频流画像。

### 多流场景必须配置过滤条件

如果没有 source MAC、stream ID 或接口过滤，多路 AVTP 流会共享同一个 assembler 状态，可能导致帧序号和 payload 混杂。

生产配置中建议必须指定至少一种过滤条件，优先级建议为：

1. 网卡接口
2. stream ID
3. source MAC
4. VLAN / EtherType 辅助过滤

### live read / stop 控制需要进一步收敛

现场验证时已经复现过一种关闭卡住现象：上层 `ReadPacket()` 已按超时返回，`ProbeCodec()` 也已经判定失败，但进程仍可能卡在 `EthernetCapture::Close()` 的后台抓包线程 join 阶段。

初始实现中 `EthernetCapture` 后台线程执行 `pcap_loop()`，上层通过 `ReadPacket()` 从队列取包。`Close()` 主要依赖：

```text
pcap_breakloop()
  -> capture_thread_.join()
```

这意味着在某些 Npcap/网卡状态下，即使上层读包逻辑已经超时，进程仍可能卡在关闭或 probe 收尾阶段，表现为 `avtp_live_probe`、`avtp_decode_rtsp_publisher` 等现场工具没有按预期时间退出。

当前代码已先做一版缓解：把后台捕获循环从长时间阻塞的 `pcap_loop()` 改为可周期检查 `running_` 的 `pcap_dispatch()` 循环，并在 close 路径增加进入、退出和丢包统计日志。实测旧 source MAC 过滤失效、probe 超时失败时，工具已经可以稳定退出。

这个问题与 AVTP AAF/CVF parser 本身无关，更像是抓包线程生命周期控制不够明确。后续仍建议继续收敛：

1. 明确区分 `ReadPacket()` 的“无包超时”和“底层已停止/错误”返回语义。
2. 保留并完善 `EthernetCapture::Close()` 的诊断日志，例如进入 close、调用 `pcap_breakloop`、开始 join、join 完成。
3. 继续评估 `pcap_dispatch()` 在不同 Npcap 版本和网卡驱动上的行为，必要时改为 `pcap_next_ex()` + 超时轮询。
4. 在 `AvtpPuller::ProbeCodec()` 中遇到持续无包时，应能稳定按 `probe_timeout_ms` 返回，不被底层 capture 线程 join 阻塞。
5. 手动工具应避免无限等待，必要时增加 `--open-timeout` / `--close-timeout` 或 watchdog 日志。

在这个问题收敛前，现场验证时如果工具超时，需要同时检查：

- 设备是否确实仍在发 AVTP 包；
- BPF filter 中的 source MAC 是否与设备重启后的实际 source MAC 一致；
- `format=auto` 是否在等待 CVF 视频包完成 probe；
- 是否有上一次超时遗留的 `avtp_*` 进程仍占用 Npcap 句柄。

### AVTP render probe 与 AV sync 的边界

`avtp_render_probe` 的定位是现场确认链路：

```text
AvtpPuller -> FFmpegDecoder(video/audio) -> RenderSession(OpenGL/WASAPI)
```

现场验证中曾出现 `submitted_video=30` 但 `rendered_video=1` 的现象。补充统计后确认这不是 OpenGL 渲染失败，而是 AV sync 把后续视频帧判定为晚到帧并主动丢弃：`avsync_dropped_video=29`。

根因是 probe 工具为了快速验证，会在 codec probe 后短时间内消费已经积累的 AVTP 包；这种“突发提交”不等价于真实播放器按节奏喂帧。用 AV sync 参与这个测试，反而容易把测试工具自身的突发行为误判成视频晚到。

因此当前 `avtp_render_probe` 默认关闭 `RenderSessionConfig::av_sync.enabled`，只验证 AVTP 拉流、解码和渲染通路本身。A/V 同步策略仍应由持续运行的 camera/RTSP render 场景单独验证。

最新现场短测结果：

```text
read_video=41, read_audio=80
decoded_video=30, decoded_audio=80
submitted_video=30, submitted_audio=57
rendered_video=30, rendered_audio=57
dropped_video=0, avsync_dropped_video=0
audio_decoder_errors=0
```

## 分阶段实施建议

### 第一阶段：已知格式 H.264 单路视频

目标是以最小改动让 AVTP 视频进入当前工程统一解码链路。

建议工作项：

1. 移植 AVTP parser、H.264 assembler 及已有协议测试。
2. 新增 `AvtpPuller : IPuller`，复用当前工程 `EthernetCapture`。
3. URL 要求包含 `src`，建议包含 `stream`，并显式指定 `format=h264`。
4. 修改 `FFmpegDecoder`，支持普通字节包输入。
5. 按当前工程结构构造正确的 `MultiStreamInfo`。
6. 增加 AVTP 到 decoder 的集成测试。

预估工作量：1 到 2 天。

### 第二阶段：多格式和鲁棒性

目标是让 H.265/JPEG、私有头、丢包恢复和停止重连更可靠。

建议工作项：

1. 修复 H.265/JPEG 丢包恢复策略。
2. 收口私有头和 RTP-like 头的 payload 语义。
3. 增加离线 pcap 回放测试。
4. 补充停止、重连、序号回绕、marker 缺失等边界测试。

预估工作量：2 到 3 天。

### 第三阶段：时间戳和同步

目标是把 AVTP timestamp 映射到当前工程媒体时间轴，并为后续音视频同步或多流同步打基础。

建议工作项：

1. 设计 `AvtpTimestampMapper`。
2. 处理 32 位 timestamp 展开和回绕。
3. 处理 MCR 重置。
4. 明确 gPTP 与本地时间轴映射方式。
5. 用真实设备验证延迟、抖动和同步行为。

预估工作量：2 到 4 天，需要真实设备配合。

## format 如何获得

`format=h264` 不是要求使用者凭空猜测，而是把“已知或已探测到的视频编码格式”显式传给 AVTP puller。

可用来源按可靠性排序如下：

1. 设备配置或协议说明。

   这是最推荐的来源。例如摄像头、域控或发送端配置里通常能明确看到当前视频编码是 H.264、H.265 还是 MJPEG。工程启动时应把这个设备侧配置转换成 `format=h264|h265|jpeg`。

2. AVTP CVF header 中的 `format_subtype`。

   AVTP CVF 的 `format` 字段为 `0x02` 时表示 RFC 格式；`format_subtype` 可进一步标识 MJPEG、H.264、JPEG2000 等格式。参考实现 libavtp 中的定义如下：

   ```cpp
   kCvfFormatSubtypeMjpeg = 0x00
   kCvfFormatSubtypeH264  = 0x01
   kCvfFormatSubtypeJpeg2000 = 0x02
   ```

   源工程还额外定义了 H.265 为 `0x03`，这可能来自新版协议或厂商扩展。迁移时应把它当成兼容扩展，并用 payload NAL 特征再次确认。

   这可以作为自动探测依据，但它要等收到 AVTP 包之后才知道。

3. payload 内容探测。

   源工程 `AvtpPuller::SelectCodec()` 会先看 payload 特征：JPEG 起始字节 `FF D8 FF`，以及 Annex-B start code 后面的 H.264/H.265 NAL 类型。这个方式对 `format_subtype` 填得不规范的设备比较有用。

4. 离线 pcap 或启动前 probe。

   可以提供一个小工具或启动前探测流程，先抓取少量 AVTP 包，得到 codec 后再生成正式 URL 或配置。这样用户看到的是明确配置，系统内部仍可自动发现。

当前工程第一阶段建议不要默认使用 `format=auto`，原因是 `MediaStreamSession::Start()` 在 `puller->Open(url)` 后马上调用一次 `GetStreamInfo()`。如果 codec 要到 `ReadPacket()` 阶段才探测出来，那么首次 StreamInfo 会是 `UNKNOWN`，解码器无法稳定打开。

因此第一阶段的推荐策略是：

```text
设备配置 / 启动前 probe / 手工确认
  -> 得到 h264 / h265 / jpeg
  -> 写入 URL 或配置
  -> AvtpPuller::Open() 直接生成确定的 StreamInfo
```

后续如果要支持真正的 `format=auto`，有两种路线：

1. 在 `AvtpPuller::Open()` 内部做有界 probe，读到第一个可识别 AVTP 视频包后再返回。
2. 给当前工程增加 StreamInfo changed 机制，允许 puller 在 `ReadPacket()` 阶段探测到 codec 后通知上层重建解码器。

第一阶段更建议选择前者或显式配置，改动面更小。

### 样例 pcap 观察

对 `C:\Users\Administrator\Desktop\avtp包_a32e2.pcapng` 的只读扫描结果如下：

- 总包数：2314。
- AVTP EtherType `0x22f0` 包数：868。
- AVTP source MAC：`aa:02:54:08:69:ca`。
- AVTP subtype `0x02`：719 个，按标准应是 AAF，不是 CVF；`stream_id=0xaabbccddeeff0001`，`format=0x04`，payload 长度固定 320 字节，payload 全部为 `0xd5`。
- AVTP subtype `0x03`：149 个，按标准应是 CVF；`format=0x02`，`format_subtype=0x01`，payload 中出现 Annex-B start code `00 00 00 01`，NAL 类型为 H.264 type 1。

因此这个样例中，codec 可以从 `subtype=0x03` 的视频包头和 payload 特征中确定为 H.264；但 `subtype=0x02` 那批包不适合作为视频配置来源，因为它们按协议是 AAF 音频类包，且 payload 是固定 `0xd5` 填充值，没有看到可解析的 codec、width、height、fps 等元数据。

这个 pcap 里也没有看到 H.264 SPS/PPS/IDR 或 JPEG SOI，因此它能证明编码格式是 H.264，但不能单靠码流恢复分辨率。分辨率仍应来自设备配置、独立控制面，或包含 SPS/PPS 的更完整抓包。

### camera-player 中的参数来源

`camera-player` 并没有从 `subtype=0x02` 那批 AAF 包中解析视频参数。它的 AVTP 参数来源实际分成三层：

1. URL 参数。

   `AvtpPuller::ParseUrl()` 从 query 中解析 `width`、`height`、`fps`、`format`、`src`、`stream`、`queue`、`read_timeout` 等参数。`Open()` 时再用这些参数填充 `stream_info_`。

2. AVTP 视频包探测。

   `AvtpPuller::ReadPacket()` 收到包后调用 `SelectCodec()`。该函数优先看 payload 特征，例如 JPEG SOI、H.264/H.265 Annex-B NAL；如果 payload 特征不明确，再用 `format_subtype` 作为 hint。探测到 codec 后写入 `MediaPacket::codec`，并由 `RememberCodec()` 缓存当前 stream 的 codec。

   这里可以理解为：H.264/H.265 在 `format=auto` 下需要 probe，因为两者都是 Annex-B NAL 流，需要通过 NAL header 或 `format_subtype` 区分；MJPEG 通常不需要复杂 probe，payload 以 JPEG SOI `FF D8 FF` 开始即可判断，源工程还对私有 subtype 做了 JPEG fallback。

3. 解码器输出帧。

   `AvtpPlayer` 没有使用 session 分发的 `StreamInfo` 打开 decoder，而是在收到第一个 `MediaPacket` 后，通过 `packet.codec` 动态构造 `StreamInfo` 并打开 FFmpeg decoder。解码成功后，真正用于显示的 width/height 来自 FFmpeg 解出的 `AVFrame::width` 和 `AVFrame::height`。

因此 `camera-player` 能容忍 `format=auto` 的关键原因是：播放器层推迟到第一个媒体包到达后才打开 decoder。当前 `video-pipeline` 的 `MediaStreamSession::Start()` 则是在 `Open()` 后立即分发 `StreamInfo`，所以不能直接照搬这个行为。

## 建议的第一阶段 URL 形态

可以先收敛为显式配置，例如：

```text
avtp://<pcap-device>?src=<source-mac>&stream=<stream-id>&format=h264
```

其中：

- `<pcap-device>`：抓包网卡或设备标识，可使用 `default`。
- `src`：AVTP source MAC，建议填写。
- `stream`：AVTP stream ID，强烈建议填写。
- `format`：第一阶段必填，先支持 `h264`。

后续再扩展：

```text
avtp://<pcap-device>?src=<source-mac>&stream=<stream-id>&format=auto&sync=gptp
```

## 推荐落地顺序

建议按以下顺序推进：

1. 先把 AVTP 协议代码移入 `protocol/avtp`，并保证原协议测试通过。
2. 再实现 `AvtpPuller`，只做单路 H.264，严格依赖显式 `format=h264`。
3. 接着补齐 `FFmpegDecoder` 普通 buffer 输入能力。
4. 最后串起 `MediaStreamSession` 到 `FFmpegDecoder` 的集成测试。

这个顺序的好处是每一步都有清晰验证点：协议先正确，puller 再正确，decoder 接口再打通，最后验证整条媒体管线。

## 总结

AVTP 模块可以自然嵌入当前工程，但它应该被定位为“媒体输入协议 + puller”，而不是一套独立播放器。

最稳妥的第一步是只支持当前设备的单路 H.264 AVTP 视频流：复用当前 `EthernetCapture`，新增 `AvtpPuller : IPuller`，让输出进入现有 `MediaStreamSession -> MediaStreamSource -> FFmpegDecoder` 链路。

真正需要提前处理的接口问题有三个：`FFmpegDecoder` 对普通 buffer 的支持、StreamInfo 的构造方式、以及 `ENABLE_PCAP` 下的 CMake 分层。等这三个点打通后，AVTP 模块就能比较自然地成为当前工程的一种输入源。

## 参考资料

- AVnu libavtp：IEEE 1722-2016 AVTP 的开源参考实现，包含 subtype 定义和 AAF/CVF/CRF/RVF 等格式支持说明。
- COVESA Open1722：基于 libavtp 扩展的 IEEE 1722 实现，说明 AVTP 支持 AAF、CRF、CVF、RVF 和 ACF 等格式。
- Wireshark AVTP / AAF Display Filter Reference：用于核对 AVTP/AAF 字段含义，例如 AAF 的 sample rate、channels、bit depth 和 audio data 字段。
