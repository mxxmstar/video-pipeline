# RTSP / ZLMediaKit Pipeline 排障记录

本文记录完整链路 `RTSP 拉流 -> 解码 -> 编码 -> 本机 RTSP 发布 / ZLMediaKit 推流` 中遇到的两个播放问题、根因、修复点和验证方式。

## 背景

测试链路：

```text
Camera RTSP
  -> FFmpegPuller
  -> FFmpegDecoder
  -> inference placeholder
  -> FFmpegEncoder(libx264)
  -> IPublisher
     -> RtspServerProtocol: rtsp://127.0.0.1:7852/camera/mainstream
     -> FfmpegMuxProtocol: rtsp://127.0.0.1:554/live/video_pipeline_av_test
```

测试摄像头：

```text
rtsp://192.168.66.83/live/mainstream
```

ZLMediaKit：

```text
HTTP: 8888
RTSP: 554
MediaServer.exe: E:\share\project\video-pipeline\third_apps\win32\zlmediakit\MediaServer.exe
```

## 问题 1：本机 RTSP 时间戳正常但没有画面

现象：

- 播放 `rtsp://127.0.0.1:7852/camera/mainstream`。
- 客户端显示时间戳推进正常。
- VLC 没有正常出画。
- H264 SPS/PPS 能被 VLC 解析，因此不是单纯的参数集缺失问题。

根因：

- `RtspServerProtocol` 在 SDP 中默认带上了 multicast 信息。
- VLC 看到 SDP 后可能优先按 multicast SETUP。
- 当前实现的 multicast 路径只完整支持 H264 video，audio multicast 尚未实现。
- 客户端按 video + audio 建立 multicast 时，audio SETUP 被拒绝，随后会话进入 EOF，表现为时间戳正常但没有画面。

修复：

- 在 `PublisherConfig::RtspOptions` 中新增显式开关 `enable_multicast`，默认关闭。
- SDP 只有在 `enable_udp == true && enable_multicast == true` 时才声明 multicast。
- RTSP SETUP 收到 multicast 请求时，如果 `enable_multicast == false`，直接拒绝。
- 需要测试 multicast 时，由测试用例显式打开 `enable_multicast`。

相关文件：

- `include/media/publisher/publisher_config.h`
- `src/media/protocol/rtsp_server_protocol.cpp`
- `test/media/test_rtsp_server_publisher.cpp`
- `test/media/test_local_mp4_decode_rtsp_publisher.cpp`

当前建议：

- 生产路径默认使用 TCP interleaved 或 UDP unicast。
- multicast 保留为显式能力，不作为默认 SDP 行为。
- 如果后续要支持 audio multicast，需要给每个音频 track 分配独立 multicast RTP/RTCP 端口，并补齐发送和 RTCP 处理。

## 问题 2：推到 ZLMediaKit 后画面卡顿、时间戳乱跳

现象：

- 播放 `rtsp://127.0.0.1/live/video_pipeline_av_test`。
- VLC 画面卡顿，时间戳跳变。
- ZLMediaKit 日志中曾出现 RTP 时间戳异常增长。

根因：

- 编码后 `MediaPacket` / `EncodedAccessUnit` 使用的是微秒时间基 `1/1000000`。
- FFmpeg RTSP muxer 在 `avformat_write_header()` 后，实际输出 stream time base 被改为 `1/90000`。
- 原逻辑把 `pts/dts/duration` 直接写进 `AVPacket`，没有按输出 stream time base 重缩放。
- 例如 40 ms 帧间隔本应从 `40000/1000000` 转成 `3600/90000`，但错误地被当成 `40000/90000`，导致服务端和播放器看到异常节奏。

修复：

- `FfmpegMuxProtocol::BuildPacket()` 写入 `pts/dts/duration` 后，使用 FFmpeg 的时间基转换：

```cpp
av_packet_rescale_ts(out, source_time_base, stream->time_base);
```

- 在 `avformat_write_header()` 后打印每个输出 stream 的实际 `time_base`，便于确认 muxer 最终采用的时间基。

相关文件：

- `src/media/protocol/ffmpeg_mux_protocol.cpp`

当前建议：

- FFmpeg mux 输出前必须以 `AVStream::time_base` 为准。
- 不要假设 `MediaTrackConfig.time_base_num/time_base_den` 会被 muxer 原样保留。
- packet 进入 muxer 前保留源时间基，真正写出前统一 rescale。

## 摄像头输入稳定性

排查期间还发现摄像头 RTSP over UDP 拉流存在明显丢包：

```text
jitter buffer full
RTP: missed packets
```

这会让后续解码、编码和发布都出现抖动，容易误判成 publisher 问题。

修复：

- 手动摄像头集成测试中将拉流传输改为 TCP：

```cpp
puller.SetRtspTransport("tcp");
```

相关文件：

- `test/media/test_rtsp_server_publisher.cpp`

当前建议：

- 局域网摄像头如果 UDP 丢包明显，优先用 RTSP over TCP 做链路验证。
- 等 publisher 时间戳和 SDP 稳定后，再单独评估 UDP 拉流性能。

## 验证方式

重新编译：

```powershell
.\build.ps1 build Release -Tests
```

运行默认测试：

```powershell
.\build.ps1 test Release
```

运行手动摄像头链路：

```powershell
.\build\bin\test_rtsp_server_publisher.exe
```

播放本机 RTSP：

```powershell
vlc rtsp://127.0.0.1:7852/camera/mainstream
```

播放 ZLMediaKit 输出：

```powershell
vlc rtsp://127.0.0.1/live/video_pipeline_av_test
```

验证重点：

- VLC 能创建 1920x1080 视频输出。
- 本机 RTSP 不再因为默认 multicast SDP 导致 SETUP 失败。
- ZLMediaKit 新日志中不再出现新的 `rtp stamp abnormal increased`。
- FFmpeg mux 日志能看到输出 stream time base，例如 video 为 `1/90000`。
- 输入端不再持续出现 `jitter buffer full` 或大批量 `missed packets`。

## 后续事项

- ZLMediaKit 推流已使用 H264 + AAC 双 track；摄像头 G711 音频先解码，再编码为 AAC 后进入 FFmpeg RTSP mux。
- 本机 RTSP 使用 H264 + 源 G711 双 track，也可配置 H264 + AAC。
- multicast 音频暂未实现，开启 `enable_multicast` 前需要明确客户端只订阅 video 或补齐 audio multicast。

## ZLMediaKit 推流无声音（已解决）

现象：

- 播放 ZLMediaKit 输出：

```text
rtsp://127.0.0.1:554/live/video_pipeline_av_test
```

- 视频正常，但没有声音。

历史结论：

- 这个问题来自当时的测试链路只向 ZLM publisher 注册了视频轨，不是本机 RTSP server 底层不支持音频。
- 本机 RTSP server 路径已经支持 `AAC / G711A / G711U`。
- 摄像头源音频是 16 kHz G711，直接推送时还存在服务端和播放器兼容性风险。

当前修复：

- `test/media/test_rtsp_server_publisher.cpp` 中已经创建 AAC audio encoder。
- 摄像头 G711 packet 先解码为 PCM，再编码为 AAC packet。
- `MakeCameraZlmPublisherConfig()` 同时加入 H264 video track 和 AAC audio track。
- `MakeCameraRtspServerPublisherConfig()` 使用 H264 video track 和源 G711 audio track。

为什么 ZLM 路径不直接使用 G711：

- 摄像头源音频是 G711 16 kHz 单声道。
- 本机 RTSP server 可以直接按 G711 RTP payload 发布。
- 但 ZLMediaKit 的 FFmpeg RTSP mux 推流路径更适合使用通用的 `H264 + AAC` 组合。
- 直接把 G711 塞进 ZLM RTSP push，容易遇到播放器兼容性、SDP 描述、时间戳和服务端处理差异问题。

详细技术原因：

1. **非标准采样率**
   - 标准 G711 采样率为 8 kHz，但摄像头源是 16 kHz
   - 大多数播放器（VLC、浏览器等）期望 G711 是 8 kHz
   - 16 kHz G711 可能导致播放器无法正确解码或静音

2. **FFmpeg RTSP Muxer 行为**
   - 使用 `rtsp_transport=tcp` 时，FFmpeg 自动生成 SDP 协商
   - G711 的 Payload Type（G711A=8, G711U=0）可能被 FFmpeg 分配为动态 PT
   - SDP 中 `a=rtpmap` 描述可能与 ZLM 期望的格式不一致

3. **ZLMediaKit 服务端处理**
   - ZLM 对 H264 视频流处理成熟
   - 对非标准采样率的 G711 可能需要特定的 SDP 格式
   - 时钟率不匹配可能导致 RTP 时间戳计算错误

4. **播放器兼容性**
   - G711 播放器支持有限（主要用于 VoIP）
   - AAC 被广泛支持（VLC、浏览器、移动端等）
   - AAC 压缩率更高（128kbps 音质优于 G711 64kbps）

当前实现链路：

```text
Camera G711
  -> FFmpegDecoder
  -> PCM frame
  -> audio resample / sample format convert
  -> FFmpegEncoder(AAC)
  -> FfmpegMuxProtocol
  -> ZLMediaKit
```

实现要点：

- 音频 encoder 输出的 packet 使用独立 `track_id=1`，并按 AAC 帧长推进时间戳。
- ZLM publisher 使用 AAC encoder 的 `extra_data` 作为 AudioSpecificConfig。
- FFmpeg muxer 在写出前将音频 packet 时间戳换算到输出 `AVStream::time_base`。

当前建议：

- ZLMediaKit 使用 `H264 + AAC` 验证通用播放器兼容性。
- 本机 RTSP 使用 `H264 + G711` 验证源音频直通能力。
- 若更换摄像头采样率或 sample format，需要重新验证 AAC encoder 的输入格式和重采样路径。
