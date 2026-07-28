# OSD 模块移植与后续演进计划

## 1. 文档目的

本文用于指导将以下源模块移植到当前 `video-pipeline` 工程：

```text
D:\file_mx\aaaaa\learn-media\src-cpp\modules\filter\osd
```

目标工程：

```text
E:\share\project\video-pipeline
```

本文同时定义 OSD 模块的边界、阶段任务、验收标准、风险控制，以及完成基础移植后的扩展改进路线。

## 2. 评估结论

结论：该 OSD 模块适合移植，但应采用“保留核心绘制算法、重写工程适配层、暂不移植旧 OSDNode”的方式实施。

核心原因如下：

1. `NV12Renderer` 和 `Yuv420Renderer` 是 CPU 原地绘制实现，算法相对独立，能够复用。
2. 当前工程已经支持 `PixelFormat::kNV12`、`kNV21`、`kI420`，与源 OSD 的第一阶段格式覆盖一致。
3. 当前 `MediaFrame` 已提供 `Width()`、`Height()`、`GetPixelFormat()`、`PlaneCount()`、`Stride()` 和 `PlaneOffset()`，能够承载 OSD 所需的平面信息。
4. 源模块直接访问旧版 `MediaFrame` 的公开字段，必须适配当前基于 `VideoFrameMeta` 的接口。
5. 源 `OSDNode` 依赖旧工程的 `runtime`、`pipeline`、`InferenceMessagePtr`、`PipelineState` 和日志系统，当前工程没有对应的兼容层，不适合原样复制。
6. 当前推理模块已经输出 `FrameResult`，可以在应用编排层转换为 OSD 的 `OverlayBatch`。
7. 源 `font_8x16.h` 标注 `GPL-2.0`，在未完成许可证确认前不应直接复制进入当前仓库。

预期移植难度为中等。矩形、线段和 YUV 平面绘制风险较低；文字授权、后端缓冲一致性、推理坐标映射和端到端编码可见性是主要风险。

## 3. 模块定位与边界

OSD 应作为 `filter` 下的独立视频处理子模块，不放入 `media`、`render` 或 `inference`：

```text
media       负责拉流、解码、编码和发布
inference   负责预处理、模型推理和后处理
filter/osd  负责把通用覆盖元素绘制到 MediaFrame
render      负责本地窗口、音频设备和画面显示
test/app    负责把以上模块编排成完整流程
```

推荐依赖方向：

```text
MediaFrame
    |
    v
filter/osd core <----- application/test integration -----> inference/FrameResult
    |
    v
media/encoder or render
```

必须遵守以下边界：

1. OSD 核心层只依赖 `media/MediaFrame`，不直接依赖 OpenVINO、YOLO、encoder、publisher 或窗口系统。
2. OSD 核心层不创建线程，不维护 pipeline 生命周期，不拥有拉流和发布对象。
3. `FrameResult -> OverlayBatch` 的转换暂时放在测试或应用编排层，避免 `filter/osd` 反向依赖 `inference`。
4. 如果转换逻辑后续被多个应用复用，再提取到独立的 pipeline/adapter 层。
5. 第一版允许原地修改 `MediaFrame`，但调用方必须保证该帧处于单写者状态。

## 4. 源模块能力盘点

### 4.1 可直接复用的能力

| 能力 | 源文件 | 处理建议 |
| --- | --- | --- |
| YUV 颜色定义 | `osd_color.h` | 迁移并补充明确的 BT.601/BT.709 约定 |
| 矩形、线段、文字元素 | `osd_element.h` | 迁移，统一默认值与参数校验 |
| 批量元素容器 | `osd_batch.h` | 迁移，优先使用值语义或智能指针 |
| OSD 抽象接口 | `i_osdrender.h` | 重命名为 `i_osd_renderer.h` |
| NV12/NV21 绘制 | `nv12_osdrender.*` | 适配当前 `MediaFrame` |
| I420 绘制 | `yuv420_osdrender.*` | 适配当前 `MediaFrame` |
| 裁剪和 Bresenham 线段 | `osd_geometry.h` | 迁移并补边界测试 |
| 基础渲染单元测试 | `test_osd_renderers.cpp` | 使用当前 `SimpleBuffer` 重写夹具 |

### 4.2 不应原样迁移的能力

| 内容 | 原因 | 处理方式 |
| --- | --- | --- |
| `osd_node.h/.cpp` | 依赖旧 runtime/pipeline 消息体系 | 只提炼无状态辅助逻辑 |
| 源模块 `CMakeLists.txt` | 依赖旧 `common_lib`、`runtime_lib`、`pipeline_lib` | 接入当前顶层 CMake |
| 旧 `MediaFrame` 字段访问 | 当前字段已经迁入 `VideoFrameMeta` | 使用当前访问器 |
| `LOG_MAIN_ERROR_AT` | 当前工程使用 spdlog 适配层 | 使用现有日志约定 |
| `font_8x16.h` | GPL-2.0 许可证风险 | 完成授权决策后再引入或替换 |

## 5. 当前工程适配差异

### 5.1 MediaFrame 差异

源实现使用：

```cpp
frame.width
frame.height
frame.pixel_format
frame.stride[index]
frame.plane_offset[index]
frame.plane_count
```

当前工程应使用：

```cpp
frame.Width()
frame.Height()
frame.GetPixelFormat()
frame.Stride(index)
frame.PlaneOffset(index)
frame.PlaneCount()
frame.VideoMeta()
```

绘制前必须验证：

1. `frame.type == MediaType::VIDEO`。
2. `frame.VideoMeta() != nullptr`。
3. `frame.buffer != nullptr` 且 `frame.buffer->Data() != nullptr`。
4. 宽、高大于零。
5. 像素格式与 renderer 匹配。
6. 平面数量满足格式要求。
7. 每个平面的 stride 不小于最小可见行宽。
8. offset、stride、平面高度计算结果不超过 `buffer->Size()`。
9. 计算 offset 和所需大小时使用 `size_t` 并检查整数溢出。

### 5.2 buffer 与 backend 一致性

当前 `FFmpegFrameBuffer` 会把 `AVFrame` 的多平面数据复制到连续的 `packed_data_`。OSD 修改的是 `MediaFrame::buffer`，而 `MediaFrame::backend` 可能仍指向原始 `AVFrame`。

如果后续 encoder 优先读取 backend，OSD 修改可能不会进入编码结果。因此第一版必须明确执行以下策略：

```text
decode
  -> infer（读取原始 frame）
  -> OSD（修改 packed buffer）
  -> 清除或失效化 backend
  -> encode（强制读取已修改的 packed buffer）
  -> publish
```

成功完成 OSD 后，应将 backend 标记为 `BackendHandle::NONE` 并清空指针，或者通过统一的 `InvalidateBackend()` API 完成。清除前必须确认当前 encoder 对 packed buffer 的 fallback 路径可用。

长期应把“CPU buffer 已修改”抽象为明确的帧状态，而不是由每个 filter 手工操作 backend。

### 5.3 推理结果坐标

当前 `FrameResult::objects[*].object.rect` 使用浮点坐标。接入前需要确认坐标属于：

1. 原始输入帧像素坐标；
2. 模型输入尺寸坐标；
3. `0.0-1.0` 归一化坐标；
4. 包含 letterbox padding 的模型坐标。

OSD 只能接收已映射到当前 `MediaFrame` 可见区域的像素坐标。转换层必须负责：

1. 反向消除 letterbox padding；
2. 按原始帧宽高缩放；
3. 对坐标做 `floor/ceil`；
4. 裁剪到 `[0, width] x [0, height]`；
5. 丢弃宽或高不大于零的框。

## 6. 目标目录结构

第一版建议形成以下结构：

```text
include/
  filter/
    osd/
      i_osd_renderer.h
      osd_batch.h
      osd_color.h
      osd_element.h
      nv12_osd_renderer.h
      yuv420_osd_renderer.h

src/
  filter/
    osd/
      osd_geometry.h
      nv12_osd_renderer.cpp
      yuv420_osd_renderer.cpp

test/
  filter/
    osd/
      test_osd_renderers.cpp
```

文字能力完成许可证决策后再增加：

```text
include/filter/osd/font_provider.h
src/filter/osd/bitmap_font_provider.cpp
```

推理到 OSD 的转换逻辑第一版放在端到端测试文件内。出现第二个正式调用方后，再考虑：

```text
include/pipeline/overlay/detection_overlay_builder.h
src/pipeline/overlay/detection_overlay_builder.cpp
```

## 7. API 设计建议

### 7.1 第一版接口

```cpp
namespace filter::osd {

enum class OsdStatus {
    kOk,
    kUnsupportedFormat,
    kInvalidFrame,
    kInvalidPlaneLayout,
    kBufferTooSmall,
    kInvalidElement,
};

class IOsdRenderer {
public:
    virtual ~IOsdRenderer() = default;
    virtual OsdStatus Draw(MediaFrame& frame,
                           const OverlayBatch& batch) const = 0;
};

}  // namespace filter::osd
```

相比单纯返回 `bool`，状态枚举能区分格式不支持、平面布局错误和 buffer 越界，便于测试、日志和线上诊断。如果第一阶段希望控制改动量，也可以先保留 `bool`，但必须提供 `LastError()` 或输出参数。

### 7.2 元素模型

第一版保留三种基础元素：

```text
OverlayRect
OverlayLine
OverlayText（受字体方案闸门控制）
```

建议所有元素具备：

```text
color
thickness
z_order（后续）
clip_rect（后续）
```

不建议让 OSD 核心直接理解 `class_id`、`score`、YOLO 或 `FrameResult`。这些属于上层业务语义。

### 7.3 renderer 选择

可先由调用方按像素格式选择 renderer：

```text
kNV12/kNV21 -> Nv12OsdRenderer
kI420       -> Yuv420OsdRenderer
其他格式     -> unsupported
```

后续可增加无状态门面：

```cpp
OsdStatus DrawOverlays(MediaFrame& frame, const OverlayBatch& batch);
```

门面负责选择 renderer，但具体格式实现仍保持独立，以便测试和扩展。

## 8. 分阶段实施计划

### 阶段 0：前置决策与基线冻结

目标：在复制代码前消除许可证、接口和端到端数据路径的不确定性。

任务：

1. 确认源 OSD 代码的所有权和允许移植范围。
2. 确认当前工程许可证及其与 GPL-2.0 字库的兼容性。
3. 在以下字体方案中做出选择：
   - 不迁移源字库，第一版只支持矩形和线段；
   - 替换为许可证兼容的位图字体；
   - 使用 FreeType 和项目自带/部署字体；
   - 经确认后保留 GPL-2.0 字库及许可证文件。
4. 确认 OSD 的第一阶段像素格式仅为 NV12、NV21、I420。
5. 验证当前 encoder 在 backend 清除后可以从 packed buffer 编码。
6. 记录当前 Debug 构建和现有测试基线。

交付物：

- 字体/许可证决策记录。
- OSD v1 支持范围。
- encoder packed-buffer 验证结果。
- 构建和测试基线。

退出条件：

- 不存在未决的源代码授权问题。
- 明确第一版是否包含文字。
- backend 失效策略经过小型验证。

### 阶段 1：模块骨架与构建接入

目标：建立独立、可开关、尚不承载业务逻辑的 OSD 模块。

任务：

1. 新建 `include/filter/osd`、`src/filter/osd` 和 `test/filter/osd`。
2. 在顶层 CMake 新增：

```cmake
option(VIDEO_PIPELINE_BUILD_OSD
       "Build OSD filter module under include/filter/osd and src/filter/osd"
       ON)
```

3. 当 `VIDEO_PIPELINE_BUILD_OSD=OFF` 时，从 `VIDEO_PIPELINE_SOURCES` 排除 `src/filter/osd`。
4. 当功能开启时定义 `VIDEO_PIPELINE_HAS_OSD=1`。
5. 在 CMake 状态输出中打印 `Build OSD`。
6. 测试目标只在 OSD 开关开启时创建。
7. OSD 核心不链接 OpenVINO、GLFW、OpenGL 或旧 pipeline 库。

交付物：

- OSD 目标目录。
- CMake 构建开关。
- 空模块 ON/OFF 构建验证。

退出条件：

- `VIDEO_PIPELINE_BUILD_OSD=ON` 可以配置和构建。
- `VIDEO_PIPELINE_BUILD_OSD=OFF` 可以配置和构建。
- 关闭 OSD 不影响 media、inference 和 render 的现有构建。

### 阶段 2：基础数据结构与 NV12/NV21 renderer

目标：完成风险最低、使用频率最高的半平面 YUV OSD。

任务：

1. 迁移并整理 `YuvColor`、预设颜色、基础元素和 `OverlayBatch`。
2. 将 `i_osdrender.h` 重命名为 `i_osd_renderer.h`。
3. 将 `nv12_osdrender.*` 重命名为 `nv12_osd_renderer.*`。
4. 将类名统一为 `Nv12OsdRenderer`。
5. 迁移裁剪、填充矩形和 Bresenham 线段算法。
6. 使用当前 `MediaFrame` 访问器构造 Y/UV 平面视图。
7. 正确处理 NV12 的 UV 顺序和 NV21 的 VU 顺序。
8. 对奇数宽高、非紧密 stride 和非零 plane offset 做完整边界校验。
9. 空 batch 返回成功且不修改 buffer。
10. 未支持格式返回明确错误。

自动测试：

- NV12 矩形边框。
- NV12 斜线与水平/垂直线。
- 元素完全在画面内。
- 元素部分越界。
- 元素完全在画面外。
- 负坐标。
- 粗线宽度大于元素尺寸。
- NV21 U/V 顺序。
- 自定义 stride。
- 非零 plane offset。
- 奇数宽高。
- buffer 过小。
- plane_count 不足。
- 空 batch。
- 不支持的 RGB24 格式。

交付物：

- NV12/NV21 原地绘制能力。
- 无设备依赖的自动化测试。

退出条件：

- 所有单元测试通过。
- AddressSanitizer 可用环境下无越界写入；Windows 主环境至少完成保护区字节验证。
- renderer 不访问 `buffer->Size()` 以外的内存。

### 阶段 3：I420 renderer 与通用分发

目标：完成源模块第一版的全部 YUV420 格式覆盖。

任务：

1. 将 `yuv420_osdrender.*` 重命名为 `yuv420_osd_renderer.*`。
2. 将类名统一为 `Yuv420OsdRenderer`。
3. 构造 Y、U、V 三平面视图并验证每个平面范围。
4. 处理 chroma 平面的向上取整尺寸。
5. 增加 renderer 门面或工厂，统一按 `PixelFormat` 分发。
6. 统一 NV12/NV21/I420 的错误码和日志文本。

自动测试：

- I420 矩形。
- I420 线段。
- 三平面自定义 stride 和 offset。
- 奇数宽高。
- U/V 平面大小不足。
- 相同 OverlayBatch 在 NV12 和 I420 上的可见区域一致。

交付物：

- I420 原地绘制能力。
- 通用 OSD 调用入口。

退出条件：

- NV12、NV21、I420 测试全部通过。
- 公共几何算法没有 renderer 间重复实现。

### 阶段 4：文字能力与标签布局

目标：在许可证明确的前提下提供稳定的检测标签文字。

前置闸门：字体许可证方案必须已确认。

任务：

1. 把字形来源抽象为 `FontProvider`，避免 renderer 直接绑定某个字库。
2. 第一版至少支持检测标签所需的 ASCII：
   - class id 或英文类别名；
   - 置信度；
   - 空格、小数点和常用标点。
3. 支持 scale、字符间距、行间距、背景色和 padding。
4. 提供文字尺寸测量，绘制前完成标签位置计算。
5. 当框上方空间不足时把标签放到框内或框下方。
6. 标签背景和文字都必须经过画面裁剪。
7. 明确非支持字符的 fallback 行为，例如替换为 `?`。

自动测试：

- 单字符和字符串。
- 1x/2x scale。
- 文本背景。
- 靠近四个边缘的文本。
- 换行和制表符（如果保留支持）。
- 空字符串。
- 不支持字符 fallback。

交付物：

- 许可证合规的文字绘制能力。
- 稳定的检测标签布局。

退出条件：

- 字体和代码许可证记录完整。
- 标签不会写出 buffer 边界。
- 典型 `class_id + score` 文本在 720p/1080p 下可读。

### 阶段 5：接入 YOLOv5 推理与发布测试

目标：验证真实链路中 OSD 能进入最终编码和发布画面。

目标测试：

```text
test/media/test_rtsp_server_publisher.cpp
TestCameraAudioVideoDecodeEncodePublish
```

建议流程：

```text
camera/RTSP packet
  -> video decode
  -> InferenceSession::Infer(frame)
  -> BuildDetectionOverlayBatch(result, frame)
  -> DrawOverlays(frame, batch)
  -> InvalidateBackend(frame)
  -> H264 encode
  -> publish to ZLMediaKit/local RTSP server
```

任务：

1. 在现有 `CameraAvPipelineState` 增加 OSD 帧数和错误数统计。
2. 推理成功后，根据 `FrameResult` 构造矩形和标签元素。
3. 类别颜色使用稳定映射，相同 `class_id` 在不同帧保持同色。
4. 标签格式第一版使用：

```text
c<class_id> <score with 2 decimals>
```

5. 在 OSD 成功后失效 backend，确保 encoder 读取修改后的 packed buffer。
6. OSD 失败时设置清晰错误；是否继续编码原始画面由测试参数控制。
7. 增加开关，使手动测试可分别运行：
   - inference off / OSD off；
   - inference on / OSD off；
   - inference on / OSD on。
8. 在日志中定期输出 decoded、inferred、osd、encoded、published 计数。
9. 使用项目内 `models/yolov5` 模型执行实际验证。

验收：

- 运行 5 分钟无崩溃、无持续内存增长。
- `decoded_video_frames >= inferred_video_frames >= osd_frames` 的计数关系合理。
- 发布端可以看到检测框和标签。
- OSD 开启后音频路径不受影响。
- OSD 关闭时，现有推理、编码和发布行为保持不变。
- 同一帧的框位置与推理目标视觉位置一致。

说明：该测试依赖摄像头、模型、ZLMediaKit 和人工查看，不应默认作为无设备 CTest。

### 阶段 6：回归、文档与交付

目标：让模块具备持续维护和独立关闭的能力。

任务：

1. 执行 OSD 单元测试。
2. 执行 inference smoke 和 YOLOv5 测试。
3. 执行媒体编码/发布相关回归测试。
4. 验证以下构建组合：

| OSD | Inference | Render | 预期 |
| --- | --- | --- | --- |
| ON | ON | ON | 完整功能 |
| ON | OFF | OFF | OSD 核心与测试可独立构建 |
| OFF | ON | ON | 保持当前行为 |
| OFF | OFF | OFF | 最小核心可构建 |

5. 为公共头文件补充 API 注释。
6. 记录支持格式、内存要求、线程要求和 backend 失效规则。
7. 更新本计划中的实际完成状态和偏差。

退出条件：

- 自动化测试通过。
- 手动端到端测试有可复现的运行方法和结果。
- OSD 开关关闭时没有残留 include、link 或测试依赖。
- 新增代码没有引入未记录的第三方许可证。

## 9. 详细测试矩阵

### 9.1 自动化单元测试

| 分类 | 用例 | 核心断言 |
| --- | --- | --- |
| 格式 | NV12 | Y 和 UV 按预期写入 |
| 格式 | NV21 | U/V 写入顺序交换 |
| 格式 | I420 | Y/U/V 三平面按预期写入 |
| 几何 | 矩形 | 四边正确、内部未填充 |
| 几何 | 线段 | 端点、斜率和粗细正确 |
| 裁剪 | 负坐标 | 不越界，画面内部分正确 |
| 裁剪 | 超出右下边界 | 不越界，画面内部分正确 |
| 布局 | stride 大于 width | padding 字节不被错误覆盖 |
| 布局 | 非零 offset | 前置保护区不被修改 |
| 布局 | 奇数宽高 | chroma 索引合法 |
| 错误 | buffer 为空 | 返回 `kInvalidFrame` |
| 错误 | buffer 过小 | 返回 `kBufferTooSmall` |
| 错误 | plane_count 错误 | 返回 `kInvalidPlaneLayout` |
| 错误 | RGB24 | 返回 `kUnsupportedFormat` |
| 批次 | 空 batch | 成功且 buffer 不变 |
| 文字 | 边缘文本 | 不越界且 fallback 正确 |

测试帧使用当前工程的 `SimpleBuffer` 构造，不再维护源模块中的独立 `TestBuffer`，除非需要 canary/guard byte 专用测试夹具。

### 9.2 回归测试

至少运行：

```text
test_filter_osd_renderers
test_inference_smoke
test_yolov5_inference
test_local_mp4_decode_rtsp_publisher
```

根据本机设备条件手动运行：

```text
test_rtsp_server_publisher
  -> TestCameraAudioVideoDecodeEncodePublish
```

### 9.3 性能基线

第一版记录但不设置过早的硬性指标。建议在 Release 构建测量：

| 分辨率 | 元素数量 | 内容 |
| --- | --- | --- |
| 1280x720 | 10 | 10 个框 + 10 个标签 |
| 1920x1080 | 20 | 20 个框 + 20 个标签 |
| 3840x2160 | 50 | 50 个框 + 50 个标签 |

记录：

- 单帧平均耗时；
- P50/P95/P99；
- 每秒处理帧数；
- OSD 开启前后的整条 pipeline CPU 占用；
- OSD 开启前后的发布延迟；
- 每帧动态内存分配次数。

## 10. 风险清单与对策

| 风险 | 等级 | 影响 | 对策 |
| --- | --- | --- | --- |
| GPL-2.0 字库许可证不兼容 | 高 | 无法合规合入 | 阶段 0 决策，优先替换字体来源 |
| backend 与 packed buffer 内容不一致 | 高 | 发布画面看不到 OSD | OSD 后明确失效 backend，并做编码验证 |
| 推理坐标未映射回原图 | 高 | 框位置错误 | 独立坐标转换函数和 letterbox 测试 |
| 多消费者共享同一帧 | 高 | 数据竞争或其他消费者看到半写状态 | v1 规定单写者；后续引入 copy-on-write |
| plane offset/stride 校验不足 | 高 | 越界写入 | 统一 PlaneView 构建器和 buffer 上界检查 |
| 4:2:0 chroma 共享导致边缘染色 | 中 | 框边缘颜色扩散 | 明确这是格式特性；后续支持只改 Y 或 alpha 混合 |
| 文字只支持 ASCII | 中 | 无法显示中文类别名 | 后续接入 FreeType/UTF-8 |
| 每帧创建大量 shared_ptr | 中 | 高频场景分配开销 | 后续改为 `std::variant` 值语义和容量复用 |
| CPU OSD 在 4K/多路流下过慢 | 中 | 延迟和 CPU 上升 | 建立 benchmark，后续 GPU/并行优化 |
| 颜色矩阵约定不明确 | 低到中 | 实际颜色偏差 | 在 API 中标注 BT.601/BT.709 和 full/limited range |

## 11. 后续拓展与改进计划

### P0：基础稳定性

P0 是完成移植后立即处理的工程质量项。

1. 增加统一 `OsdStatus` 和可诊断错误信息。
2. 抽取 `PlaneView`/`MutablePlaneView`，统一格式校验和边界计算。
3. 增加 `InvalidateBackend()` 或帧脏状态 API。
4. 明确原地修改的所有权规则。
5. 完成奇数尺寸、stride、offset 和 canary 测试。
6. 增加 OverlayBatch 容量预留，减少每帧分配。
7. 建立 Release 性能基线。

### P1：检测结果展示能力

1. 类别名称映射，支持 COCO labels 文件。
2. 每类稳定颜色调色板，避免相邻类别颜色过近。
3. 标签自动布局，避免框顶部越界。
4. 多标签碰撞规避。
5. 置信度、track id、自定义前缀的格式化策略。
6. 支持填充矩形和透明背景。
7. 支持虚线、圆、圆角矩形、多段线和多边形。
8. 支持检测框选中/高亮状态。

### P1：推理高级结果

1. Pose：关键点和骨架连接线。
2. Segmentation：mask 缩放、裁剪和 alpha 混合。
3. Tracking：track id、轨迹尾迹和速度信息。
4. OCR：旋转框、多行文本和识别结果。
5. Classification：全局或局部分类标签面板。
6. 统一坐标变换模块，覆盖 resize、crop、letterbox 和旋转。

这些能力仍应由上层把推理结果转换为通用 OSD 元素，OSD 核心不直接依赖特定模型结果类型。

### P2：文字与国际化

1. 使用 FreeType 或其他许可证兼容的字体栅格化方案。
2. 支持 UTF-8、中文、日韩文字和常用符号。
3. 支持字体 fallback。
4. 增加 glyph cache，避免每帧重复栅格化。
5. 支持描边、阴影、对齐和最大宽度截断。
6. 支持 DPI/分辨率相关的字号策略。
7. 将字体文件作为可配置资源，不在代码中硬编码系统字体路径。

### P2：更多像素格式与颜色质量

1. 支持 BGR24、RGB24 和 GRAY8。
2. 支持 RGBA/BGRA，前提是 `PixelFormat` 扩展。
3. 支持 YUV444、P010 等格式的可行性评估。
4. 明确 BT.601/BT.709/BT.2020。
5. 明确 limited range/full range。
6. 使用正确的 RGB 到 YUV 转换，而不是只依赖固定预设值。
7. 对填充、文字背景和 mask 使用 alpha blending。

### P2：性能优化

1. 将 Overlay 元素由 `shared_ptr` 多态改为 `std::variant` 值语义。
2. 复用 OverlayBatch、文本布局结果和临时缓冲区。
3. 对填充矩形和水平线使用连续内存写入。
4. 按 chroma block 合并重复 U/V 写入。
5. 评估按元素、按行或按 tile 并行。
6. 为多路视频增加可配置的 OSD 工作线程池。
7. 增加 perf counters：绘制耗时、元素数量、跳过数量和错误数。
8. 在性能优化前后使用相同 benchmark 和输出画面对比。

### P3：GPU 与零拷贝

1. 评估 OpenCL、OpenVINO Remote Tensor、D3D11 或 shader OSD。
2. 对本地预览，可在 render 阶段使用 OpenGL shader 绘制 overlay，避免修改视频帧。
3. 对编码发布，需要评估 GPU surface 上的 OSD 和硬件编码互操作。
4. 为 CPU/GPU 两种后端保持同一套 OverlayBatch 语义。
5. 引入 backend capability 查询，而不是通过裸指针类型猜测。
6. 建立 CPU fallback，GPU 后端失败时可降级。

注意：本地 render overlay 和编码前 burn-in OSD 是两个不同需求：

```text
render overlay   只影响本地显示，不修改编码内容
burn-in OSD      修改待编码帧，发布端可以看到
```

后续 API 应明确区分二者。

### P3：生产化能力

1. OSD 配置热更新。
2. 按 stream/channel 设置模板。
3. 时间戳、水印、设备名和业务状态面板。
4. 隐私遮挡、马赛克和模糊区域。
5. OSD 失败降级策略和错误率告警。
6. 配置 schema 版本化。
7. 输出统计：
   - 处理帧数；
   - 失败帧数；
   - 平均/P95 耗时；
   - 平均元素数；
   - 因格式不支持而跳过的帧数；
   - 因坐标无效而跳过的元素数。
8. 长时间、多路流、频繁启停压力测试。

## 12. 建议优先级与里程碑

| 里程碑 | 范围 | 可见结果 |
| --- | --- | --- |
| M0 | 阶段 0 | 授权、字体和 backend 策略确定 |
| M1 | 阶段 1-2 | NV12/NV21 可画框和线，单元测试通过 |
| M2 | 阶段 3 | I420 支持，通用 OSD API 可用 |
| M3 | 阶段 4 | 合规的文字和标签布局 |
| M4 | 阶段 5 | YOLOv5 检测框进入编码发布画面 |
| M5 | 阶段 6 | 构建矩阵和回归验证完成 |
| M6 | P1/P2 | pose、mask、UTF-8、透明混合与性能优化 |
| M7 | P3 | GPU/零拷贝和生产化能力 |

推荐先完成 M0-M2，再接入推理链路。文字许可证如果不能及时确定，可以先用“仅矩形框”完成 M4 的数据路径验证，不阻塞核心移植。

## 13. 实施顺序与提交拆分建议

为便于审查和回退，建议按以下粒度提交：

1. `build: add optional filter/osd module skeleton`
2. `feat(osd): add overlay primitives and NV12/NV21 renderer`
3. `test(osd): cover semiplanar layout and clipping`
4. `feat(osd): add I420 renderer and format dispatch`
5. `test(osd): cover planar layout and invalid buffers`
6. `feat(osd): add licensed text renderer`
7. `test(media): burn inference overlays into published video`
8. `docs(osd): document API, ownership, formats and performance`

每个提交都应保持可构建；构建开关和核心 renderer 不应与手动摄像头测试捆绑在同一个提交中。

## 14. 完成定义

满足以下条件后，可认为 OSD 基础移植完成：

1. 模块位于 `include/filter/osd` 和 `src/filter/osd`，测试位于 `test/filter/osd`。
2. 存在 `VIDEO_PIPELINE_BUILD_OSD` 独立构建开关。
3. 支持 NV12、NV21 和 I420。
4. 矩形和线段绘制具备完整自动化测试。
5. 文字能力不存在许可证隐患；如果暂未实现，文档明确标记。
6. stride、offset、奇数尺寸和 buffer 边界经过测试。
7. OSD 核心不依赖 inference、render、encoder 或 publisher。
8. `TestCameraAudioVideoDecodeEncodePublish` 能按开关执行：

```text
decode -> infer -> OSD -> encode -> publish
```

9. 发布端可见 OSD，证明 backend 失效策略正确。
10. OSD 关闭时现有构建和测试没有行为回退。
11. 自动化测试和手动测试结果已记录。
12. API、线程模型、所有权和支持格式已形成维护文档。

## 15. 下一步

建议后续从阶段 0 开始实施，优先完成三件事：

1. 确认 `font_8x16.h` 的许可证处理方案。
2. 用一个最小实验验证 OSD 修改 packed buffer、清除 backend 后，FFmpeg encoder 能编码出修改后的画面。
3. 创建 OSD 目录和构建开关，然后实现不含文字的 NV12/NV21 基础 renderer。

