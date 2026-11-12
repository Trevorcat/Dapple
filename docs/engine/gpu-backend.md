# Dapple GPU 计算后端设计（含 spike 与阶段 1 生产后端）

> 更新（2026-11-12，GPU 执行器工程师）：本文的 `Backend` / `Session` / `hookRenderFrame` / `hookExport` / `gpu_frame` 已被引擎 API 取代：GPU 路径现在是 `Planner::plan` -> `GpuExecutor::run` -> `gpuImplFor(...).recordPorts`，执行器选择（Vulkan -> GLES 3.1 -> CPU）在 `EngineSession`。新的结构、测试与数字见 `docs/engine/pipeline-architecture.md` 第 19 节；下面第 10 节起的文件表是阶段 1 的历史记录。

状态：**阶段 1 已完成**（生产 Vulkan 后端，见第 10 节；`core/gpu/`，`-DDAPPLE_GPU=ON`，运行时开关默认关闭）。第 0-9 节是设计与 spike 阶段的依据，第 7 节的 spike 代码已被生产后端取代（结果保留作历史记录）。本文给出决策与依据；凡标注「实测」的数字来自本机 spike，标注「待实机」的需要在真机上验证（spike 期间不得触碰车主的真机，只跑了 x86_64 模拟器）。

## 0. 结论速览

| 议题 | 决策 |
|---|---|
| 分工 | GPU：交互预览与全分辨率导出中的所有逐像素与空间算子；CPU：解码（LibRaw）、分析（256 px 代理）、LUT 烘焙（多线程）、回退；NPU：仅 v2 的 AI 模型（LiteRT）；DSP（Hexagon）：**不用于像素管线**（理由见 1.3）。 |
| API | Android：**Vulkan 1.0 核心 + compute**（volk 动态加载，运行时无链接依赖；查询 1.1/1.2 能力作可选优化）；iOS：**Metal**。 |
| 着色器 | **GLSL compute -> SPIR-V（glslc，构建期）；iOS 用 SPIRV-Cross 把同一份 SPIR-V 转 MSL**。不引入 Halide / Kompute / OpenCL。Slang 作为 shader 数量 > 30 或 film 管线变体爆炸时的备选。 |
| LUT 采样 | **手写四面体插值（texelFetch，4 次取样）**，不用硬件三线性。实测三线性相对 CPU 烘焙路径多出 mean 0.6 / p99 2.7 ΔE2000，不可接受；四面体与硬件三线性一样快（带宽受限于读源图）。 |
| 精度 | 源图 RGBA16F；LUT RGBA32F（3D 纹理，41^3 仅 1.1 MB）；中间显示量 RGBA16F；终点 RGBA8。spike 实测对 CPU 烘焙路径 ΔE2000 mean 0.06 / p99 0.86。 |
| 显示 | 交互帧不经过 CPU/Dart：原生渲染线程直接写入 Flutter `SurfaceProducer` 的 Surface（`ANativeWindow` + Vulkan swapchain），Dart 侧只有 `Texture(textureId)`。 |
| 全图导出 | 2048^2 tile + halo；大尺度算子（clarity 基底、dehaze 透射、halation）在 <=2048 px 的基础层上算一次，tile 内只做局部算子。 |
| 能耗 | vsync 对齐、latest-wins、空闲零工作；ADPF（CPU 部分）+ `AThermal` 余量驱动画质降级；Vulkan 不可用或自检失败即走 CPU 路径。 |
| 下一步 | 阶段 1（约 3 周）：把 spatial.cpp 的各算子与几何前端移植到同一套 pass，以回读方式接入 `dapple_session_render_frame`（flag），先拿到真机的数据；再做 Surface 零拷贝。详见第 8 节。 |

## 1. 分工：什么放在哪里

### 1.1 现状（CPU 参考路径）

`renderer.cpp` 的烘焙路径已经把逐像素阶段整理成适合 GPU 的形态：

1. 每像素：线性 ProPhoto -> 乘以 `wb * 2^EV` -> 亮度 `Y` -> **1D 色调表**（以 float 位模式索引：指数 + 高 8 位尾数，每倍频 256 桶，桶内线性）-> 得到亮度比并乘到 RGB；
2. **n^3 格点**（n 为 17..65，交互默认 41；平方根域，节点 `i` 位于 `1.5*(i/(n-1))^2`），**四面体插值**得到编码后的 sRGB 显示值；超出格点范围（色调映射后分量 > 1.5）的像素走精确路径；
3. 之后是邻域算子（`spatial.cpp`）：亮度 NR、色度 NR、dehaze、clarity/texture/sharpen、grain。film 模块的非空间部分同样烘焙为 LUT（33^3 float4 + 194x194 原始曝光 LUT + 密度曲线），空间部分为 grain、halation、blur、DIR 扩散、scanner blur / USM。

GPU 的映射非常直接：这些都是「纹理 + 小表 + 逐像素 ALU」，几乎无分支，天然适合 GPU；CPU 侧每次参数变化只需重新烘焙（几 ms，多线程）并上传 1 MB 级别的表。

### 1.2 分配

| 工作 | 位置 | 理由 |
|---|---|---|
| LibRaw 解码、去马赛克 | CPU | 熵解码串行性强，无合适 GPU 实现；占首次打开耗时但一张图只做一次。 |
| 色调分析（`analyzeTone`，256 px 代理渲染）、auto-adjust | CPU | 0.0x ms 量级，且**它的输出是烘焙的输入**（依赖关系在 GPU 之前），不值得往返。 |
| tone 1D 表 + 格点烘焙 | CPU 多线程 | 实测 41^3 全核 5-8 ms（桌面 28 线程）；其中 OKLab/曲线/LUT/grade 等分支复杂，且需与 CPU 参考逐位同源（单一真相）。如需进一步压缩，可在阶段 4 评估把 `op.colour()` 移植为 compute（节点数 69k，几乎免费，但会产生第二份实现，必须有 parity 测试）。 |
| tone + LUT + vignette + film 非空间 LUT | **GPU**（一个融合 pass） | 逐像素，读一次源图写一次结果，带宽受限。 |
| 几何（旋转/拉直/裁切/翻转）与重采样 | **GPU** | 4 次双线性取样实现 Catmull-Rom（RGBA16F 线性滤波为 Vulkan 强制支持格式）；缩小用 Lanczos-3 可分离 + 先 mip/box 预滤；重采样后再做 USM（与 CPU 现有「锐化在重采样之后」一致）。 |
| 高斯 / 可分离模糊 | **GPU** | spike 已验证（共享内存分块，64 像素 x 4 行/组）。大 sigma（halation、DIR 扩散、clarity 基底）用金字塔/降采样后模糊再上采样，而不是增大半径。 |
| clarity / texture / sharpen / dehaze / NR | **GPU** | 现有 CPU 实现本来就是「1/4 尺寸副本 + 引导滤波/盒式滤波」，同构移植；各算子的 halo 很小。 |
| grain | **GPU** | 过程式（哈希噪声 / film 的层粒子模型），无需读邻域，成本与点算子相当。 |
| halation / scatter | **GPU** | 指数模糊的高斯混合替代（spec 3.2），在降采样层执行。 |
| FFT 扩散滤镜（film spec 3.3，仅 diffusion filter） | 先 CPU，后评估 | 仅少数 film 预设使用；GPU FFT 是独立工程，列入后期。 |
| 编码 JPEG | CPU（现有 stb/encode）；Android 后期评估硬件 JPEG | 导出时 GPU 以 tile 为单位回读，CPU 流式编码。 |
| 直方图 / 分析显示 | 见第 3.3 节 | |
| AI 模型（分割、学习型调整） | **NPU**（v2） | 见 1.4。 |

CPU 路径永远保留：低端设备回退、无 Vulkan 的设备、单元测试的 ground truth、缩略图/批处理（后台线程，不抢 GPU）。

### 1.3 DSP（Hexagon）——评估结论：不用于像素管线

- 仅 Qualcomm；必须引入 Hexagon SDK（HVX 内建函数、FastRPC、skel 库的签名与部署流程），许可证与再分发条款不是我们可以在闭源 App 里无顾虑携带的东西；Halide 的 Hexagon 后端已进入维护/弃用状态。
- 数据路径：CPU/GPU <-> DSP 需要 ION/DMA-BUF 共享与 FastRPC 调用；每帧的同步与缓存维护开销抵消 HVX 对 8/16 位定点的优势，而我们的管线是 fp16/fp32 且以浮点 LUT 为核心，HVX 对 float 不友好（Hexagon v68+ 才有有限 fp16/qfloat）。
- 与 GPU 重叠：Adreno 本身对这类带宽受限的逐像素/可分离滤波的每瓦性能已经很好；多一套仅 Snapdragon 可用的后端，测试矩阵翻倍，维护成本远超收益。
- 唯一理由是「并行跑图像预处理省 GPU 时间」——预览场景 GPU 单帧只有亚毫秒到数毫秒，没有被占满，不成立。

**拒绝。** 对 Qualcomm 机型的 DSP/HTP 利用通过 NPU（LiteRT + QNN delegate）间接获得，只用于 ML。

### 1.4 NPU：v2 AI 模型如何接入

- 运行时：LiteRT（原 TFLite，Apache-2.0）。delegate 顺序：厂商 NPU delegate（Qualcomm QNN / MediaTek NeuroPilot / Google Tensor）-> LiteRT GPU delegate -> XNNPACK CPU。NNAPI 自 Android 15 起已弃用，不作为主路径。
- 数据：模型输入输出都是小分辨率（256-512 px），由 GPU 管线的降采样层直接产生一张 RGBA16F/8 小图，回读（几十 KB）喂给模型；模型输出（分割 mask、曲线参数/逐区域调整）作为 **新的小输入资源**：mask 以 R8/R16F 纹理上传，在融合 pass 中作为额外采样器参与（局部调整 = lattice 之后的 mask 混合，或按 mask 选择不同 lattice：`lattice[k]` 用 3D 纹理数组）。学习型全局调整输出的是 `EditParams` 增量，走现有烘焙路径，零额外 GPU 工作。
- 调度：NPU 推理异步，结果到达后触发一次 latest-wins 重绘；不阻塞 vsync。
- 接口位置：`core/ai/`（待建）提供 `ModelRunner`（C++ 抽象），Android 实现走 LiteRT C API，iOS 走 Core ML；与 GPU 后端互不依赖，仅通过「小图输入 / mask 输出」两个资源耦合。

## 2. API 与着色器工具链选型

### 2.1 Android：Vulkan compute

- 基线：只依赖 **Vulkan 1.0 核心 + compute 队列**，`minSdk = 29`（Android 10）。Android 10 起新出厂的设备如果支持 Vulkan 则要求 1.1，因此 1.0 核心功能面在 minSdk 以上是安全的；spike 的实例以 1.1 请求（若可用）只是为了查询子组/fp16 能力。
- 本项目只用：`rgba16f / rgba8` 存储图像（Vulkan 强制支持）、`rgba32f` 采样（强制）、SSBO、push constant（<=128 字节）、共享内存（<=16 KB，Vulkan 最低保证；实测 SwiftShader 32 KB、NVIDIA 48 KB）、特化常量。
- **可选能力**（运行时查询，缺失则用 fp32 版本的同一 shader）：`shaderFloat16`/`storageBuffer16BitAccess`（fp16 算术，Adreno 6xx+/Mali Valhall+ 通常具备，PowerVR 老驱动与低端 Mali 不一定）；子组运算（Adreno 子组 64/128、Mali 4-16、Apple 32；用于直方图归约，可退化到共享内存归约）；`VK_KHR_synchronization2`、`VK_EXT_subgroup_size_control`。
- 目标设备预期（**待实机，用 `dapple gpu-bench --info` / App 内诊断页确认，不凭记忆写死**）：Find N5（Adreno 830）预期 Vulkan 1.3、fp16 算术+存储、64 位子组；低端 Mali（G52 等）、PowerVR GE 系列、Adreno 6xx 老驱动：能力参差，所以必须有 6.3 节的启动自检与黑名单。
- 为什么 volk：运行时 `dlopen("libvulkan.so")`，没有 Vulkan 的设备在 `create()` 里干净地失败并回退 CPU；不增加链接依赖，不依赖 NDK 的 libvulkan stub 版本。

### 2.2 iOS：Metal

Metal 是 iOS 的唯一合理选择（无 Vulkan）。单一源码策略：**同一份 GLSL -> SPIR-V，再经 SPIRV-Cross 生成 MSL**（构建期，产物是 `.metal` 文本或预编译 `metallib`）。需要遵守 GLSL 子集（不使用 push descriptor、不依赖 Vulkan 特有同步语义；这些约束本来就由 pass 图抽象吸收）。

主机侧抽象：`gpu::Device`（建图像/缓冲/管线、绑定、dispatch、屏障、submit）约 15 个操作，Vulkan 与 Metal 各一份薄实现；pass 图（见 4.2）和所有 shader 共享。

### 2.3 方案对比

| 方案 | 许可 | 构建复杂度 | 体积 | 评价 |
|---|---|---|---|---|
| **GLSL + glslc + SPIRV-Cross（选）** | shaderc/glslang Apache-2.0；SPIRV-Cross Apache-2.0；均仅构建期，不随 App 发布 | NDK 自带 glslc（已验证用其编译 spike）；iOS 再加 SPIRV-Cross（单个 C++ 库） | 每个 shader 数 KB（spike 两个 shader 共 11 KB SPIR-V） | 工具链最小；GLSL 直接易读；SPIR-V 同时给 Android 与 iOS。缺点：无泛型/模块，film 管线变体多时要靠特化常量与 `#include`。 |
| Slang | MIT / Apache-2.0 w/ LLVM exception | 需要 slangc（约百 MB 工具链），团队新语言 | 同上 | 模块、泛型、自动微分、直接输出 SPIR-V/MSL/HLSL；**备选**，在 shader >30 个或 film 变体多时再评估。 |
| Halide AOT（Vulkan/Metal 后端） | MIT | 需要 Halide 编译器、每条 pipeline 提前编译、schedule 调优；Vulkan 后端在 Android 上成熟度一般；Hexagon 后端已不推荐 | 每个 AOT pipeline 数十到数百 KB | 对「大量算子融合 + 自动调度」有吸引力，但我们要对每个 pass 精确控制内存、tile、与 swapchain/ANativeWindow 的互操作，Halide 把这些藏起来了。拒绝。 |
| Kompute | Apache-2.0 | 小 | 小 | 仅 Vulkan，无 iOS；对图像、swapchain、外部内存支持弱；我们需要的正是这些。拒绝。 |
| OpenCL | -- | -- | -- | Android 上不属于官方支持面（多数 Pixel/部分厂商无 libOpenCL），iOS 无。拒绝。 |
| MoltenVK（iOS 上跑 Vulkan） | Apache-2.0 | 零额外主机代码，但多一层 Vulkan->Metal 翻译 | 数 MB | 仅作为 iOS **快速过渡**（阶段 5a，几天），正式版用原生 Metal（更好的能耗、CVPixelBuffer/IOSurface 零拷贝更直接）。 |
| OpenGL ES 3.1 compute 作为二级回退 | -- | 第二套方言 | -- | 不做：回退目标是 CPU 路径（已存在、已测试）。 |

**许可汇总**：Vulkan-Headers（Apache-2.0）、volk（MIT）随 App 编译；glslc/shaderc/SPIRV-Cross/SPIRV-Tools 仅构建期（不分发）；后续 VulkanMemoryAllocator（MIT）、LiteRT（Apache-2.0）。全部宽松许可，可闭源。

## 3. 零拷贝显示、分析

### 3.1 现状的代价

当前路径：native 渲染 RGBA -> `dapple_session_render_frame` 输出 -> Dart FFI 拷贝 -> `decodeImageFromPixels` -> `ui.Image` 上传为 GPU 纹理。1600 长边的一帧（1066x1600x4 = 6.8 MB）至少被写 3 次（native、Dart 堆、GPU 上传），约 20 MB 内存流量/帧，60 fps 拖动时 ~1.2 GB/s 的 DRAM 带宽，另有一次 isolate 消息往返。这些在手机上是耗电和掉帧的主要来源之一，与计算本身无关。

### 3.2 Android 设计

```
Dart: Texture(textureId)                       (Flutter 3.47，Impeller)
Kotlin 插件: TextureRegistry.createSurfaceProducer() -> SurfaceProducer
              surface -> JNI -> ANativeWindow_fromSurface
Native 渲染线程 (C++, 每个编辑视图一个 GpuView):
   vkCreateAndroidSurfaceKHR(ANativeWindow) -> swapchain (RGBA8, usage = STORAGE|TRANSFER_DST|COLOR_ATTACHMENT 择优)
   每帧: acquire -> 融合 pass 写入 swapchain image (支持 STORAGE 则直写, 否则写中间 RGBA8 再 vkCmdCopy/blit) -> present
   SurfaceProducer 回调 onSurfaceAvailable / onSurfaceCleanup: 重建 / 销毁 swapchain (前后台切换、Surface 被回收)
```

- 选择 swapchain 路径而非 `AHardwareBuffer` 导入 VkImage：`SurfaceProducer` 对插件暴露的是 `Surface`（底层为 ImageReader 或 SurfaceTexture，取决于 Flutter 后端），NDK 只允许通过 EGL/Vulkan swapchain 向 `ANativeWindow` 提交缓冲；`AHardwareBuffer` 方案需要自己管理缓冲队列并通过 Flutter 的 `ImageReader` 路径提交，兼容性更差。若后续发现 swapchain 在某些 ROM 上有问题，再评估 `AHardwareBuffer` + `VK_ANDROID_external_memory_android_hardware_buffer`（以及 Flutter 侧的 HardwareBuffer 纹理支持）。**阶段 2 之前需要用一个 20 行的真机/模拟器 Kotlin 小原型确认 Surface 的 usage 位与 STORAGE 写入支持**。
- 数据流向：编辑滑杆 -> Dart 把参数 JSON 通过 FFI 同步投递到原生（微秒级，非阻塞，latest-wins，见 4）-> 渲染线程按 vsync 出帧；Dart **不再接收像素**，只在 settle 后异步收到分析 JSON（直方图、色调分类）。
- 局部放大（`RenderRegion`）同理：region + outLongEdge 作为 push constant 的一部分，直接在同一个 swapchain 里渲染。

### 3.3 直方图与分析

- 色调分析（`analyzeTone` 的 p5/p50/p95 与 64 bin 直方图）**保留在 CPU 的 256 px 代理上**：它是烘焙的**输入**，必须先于 GPU 帧完成，代理渲染仅需亚毫秒到数毫秒。
- 界面上的「当前输出直方图」：融合 pass 里按工作组在共享内存中累积 256 bin 亮度直方图，再用全局 atomicAdd 汇总到 1 KB 的 SSBO，settle 后（或 10 Hz 节流）回读 1 KB。若某设备的共享内存原子较慢，则对 1/4 降采样层做归约。成本 < 融合 pass 的几 %。

### 3.4 iOS（后期）

`FlutterTexture` + `CVPixelBuffer`（IOSurface 背板，`kCVPixelBufferMetalCompatibilityKey`）-> `CVMetalTextureCache` 得到 `MTLTexture`，compute 直接写入；`textureFrameAvailable` 通知 Flutter。与 Android 对称：Dart 侧同样只有 `Texture(textureId)`。

## 4. 内存、精度、tile

### 4.1 资源布局

| 资源 | 格式 | 说明 |
|---|---|---|
| 源图 | RGBA16F（2D） | 每张图上传一次。1600/2048 预览源：2048x1365x8 B = 22 MB。线性 ProPhoto、1.0 = 传感器削波（RAW），fp16 范围/精度足够（12-14 位数据；暗部进入次正规区后精度仍 < 0.2/255）。alpha 通道填充（RGB16F 不能作 storage image，4 通道对齐更利于缓存）。 |
| tone 表 | SSBO float[7682]（30 KB） | 手动线性插值，与 CPU 同位模式索引，`kToneBase`/`kToneN` 常量与 `renderer.cpp` 一致（spike 的 `setLut` 在尺寸不符时报错，作为漂移检测）。 |
| 格点 | 3D RGBA32F（41^3 = 1.1 MB；65^3 = 4.4 MB） | texelFetch + 手写四面体插值；不用硬件三线性（见 4.3）。若设备不支持/不需要 fp32 可降为 RGBA16F（精度足够，编码值 0..1），但与四面体搭配时没有带宽收益，保持 fp32。 |
| 显示量中间图 | RGBA16F | 编码后的 sRGB 值（0..1），供空间算子读取；精度 2^-11 优于 8 位量化。 |
| 输出 | RGBA8（swapchain 或 storage image） | 末端 pass 写入。 |

### 4.2 pass 图

> 已实现：见 10.2（依赖分层调度、屏障合并、池内别名、描述符缓存）与 10.3（kernel 对照表）。

渲染一帧 = 一个小型 DAG：`geometry/resample -> [融合点算子: tone+lattice+vignette(+film LUT 链)] -> NR -> dehaze -> clarity/texture/sharpen(合并累积) -> grain -> pack`。每个节点声明读写的图像/缓冲与 halo，引擎在其上做：屏障自动插入、中间图像别名复用（预览 2-3 张、全图 tile 同理）、按 tile 展开。shader 与 pass 图与 API 无关，这是 Android/iOS 共享的部分。

### 4.3 精度与 CPU 一致性（ΔE 预算）

- 预算：GPU 对 **同一烘焙路径的 CPU 输出** mean <= 0.1、p99 <= 1.0 ΔE2000（差异来源：fp16 源图、fma/舍入顺序、RGBA8 转换）；GPU 对 **精确路径** 沿用烘焙路径自身对精确路径的预算（mean <= 0.5）。
- spike 实测（NVIDIA RTX 4060 Ti，Vulkan 1.4，真实 ARW，一组覆盖 tone/色彩/HSL/grading/toneMove 的编辑；n=41）：

| 项目 | 1600 px | 全分辨率 26 MP / 合成 60 MP |
|---|---|---|
| GPU（四面体）对 CPU 烘焙，ΔE2000 mean / p99 / max | **0.062 / 0.86 / 1.7** | **0.058 / 0.80 / 5.2**（max 来自下方「格点外像素」） |
| 有差异的像素 | 12.5%，且最大通道差 1/255 | 11.9% |
| CPU 烘焙 对 CPU 精确（参照） | 0.57 / 2.96 | 0.63 / 3.54 |
| GPU 对 CPU 精确 | 0.59 / 2.97 | 0.65 / 3.54 |
| 硬件三线性（RGBA16F）对 CPU 烘焙 | 0.64 / 2.67 | 0.59 / 2.59 |
| 同上，RGBA32F | 0.64 / 2.67（与 16F 相同） | 0.59 |
| 手写三线性（诊断）对 CPU 烘焙 | 0.64 / 2.67（与硬件相同） | |
| 高斯（sigma 2 / 7.8 / 11.9）对 CPU float 参考 | 0.14 / 0.97 / max 1.2 | 0.14 / 0.97 |
| SwiftShader（模拟器 x86_64）对 CPU 烘焙 | 0.025 / 0.58 | 0.016 / 0.48 |

  读数说明：
  1. **四面体插值一致性达标**，剩余差异为 1/255 量级的舍入。暗部一个 8 位码值对应较大的 ΔE，所以 p99 约 0.8。
  2. **硬件三线性不可用**：fp16 vs fp32 无差别、硬件与手写三线性无差别，说明误差来自「三线性 vs 四面体」本身：格点函数在色度方向强非线性（gamut map、grading、饱和度），四面体沿灰轴精确，三线性不然。n=41 与 65 都没有改善，**这点没有彻底定位根因**，但结论（必须用四面体）不受影响，四面体与三线性速度相同。
  3. **格点外像素（> 1.5）**：CPU 对它们走精确路径，spike 的 GPU 只做边缘钳制，造成最大 37/255 的通道差、max ΔE 5.2（极少数亮且饱和的像素）。生产实现需要解决，选项：(a) 第二张覆盖 [1.5, 8] 的粗格点（烘焙成本翻倍但仍 <20 ms）；(b) 把 `PointOp::colour` 的精确路径移植为 shader 备用分支（仅在格点外执行，发散但极少）。**推荐 (a)**，因为不引入第二份颜色算法实现。
  4. CPU 烘焙路径对精确路径本身在这组偏激的编辑上就有 mean 0.57（其中包含 toneMove 代理与格点外像素），已超过 0.5 预算，属 CPU 渲染器自己的指标，与 GPU 无关，但提示：测试集里需要「中等强度」编辑集，而不仅是这组压力参数。
- 测试：`dapple_gpu_tests`（阶段 1）= 10 张图 x 6 组参数，对比 `renderBakedRgba(Spatial::Off)`（以及加空间算子后的 `Interactive`/`Full`），门限同上；CI 用 SwiftShader/lavapipe（纯软件 Vulkan，spike 已在 Android 模拟器中验证可运行）。

### 4.4 全分辨率导出与 tile

> 已实现：见 10.7。实现与下面的方案有两处不同：全局量（dehaze 暗通道/大气光、clarity 引导滤波系数）不是从一张 <= 2048 的整图缩小图算，而是由 tile 分遍把 1/4 网格逐块累积再整体分析（与 CPU 的 1/4 尺寸副本完全同构，因此 tile 结果与整帧逐位一致）；源图在放不下时按 tile 窗口流式上传。

- 61 MP（9504x6336）源图 RGBA16F = 481 MB，加 disp/tmp/out8 约 1.7 GB，桌面可行（spike 已合成 60 MP 跑通，源图转换 146 ms + 上传 58 ms），**手机不可行**（GPU 与 CPU 共享内存；还要避开单图最大尺寸/内存碎片问题）。
- 方案：
  1. **基础层**：整图降采样到 <=2048 长边，一次性算出所有大尺度量（clarity 的引导滤波基底、dehaze 暗通道透射、halation/scatter/DIR 扩散，均为低频量），得到若干小纹理。
  2. **tile 层**：2048^2 核心 + halo（halo = 局部算子最大半径，如 sharpen sigma<=3 px@2048，全图缩放系数 x4.6 -> 半径约 42 px），读源图 tile（CPU 侧 float RGB 先转 fp16 RGBA 放入 staging，双缓冲上传 33 MB/tile），在 tile 内完成几何/重采样、融合点算子、局部空间算子、grain，大尺度量由基础层双线性上采样。
  3. 结果 tile 回读（8 MB）交给流式 JPEG 编码，峰值显存 < 200 MB。
  4. 后续可把 `SourceImage` 的 CPU 存储从 float RGB（12 B/px）改为 fp16 RGBA（8 B/px）以降低内存与转换成本（属引擎改动，另议）。
- 与 CPU 一致性：CPU 路径对大尺度量也是「1/4 尺寸副本」（spatial.cpp），因此基础层/局部层的划分与现有实现同构，不改变结果。

## 5. 调度与能耗

### 5.1 帧调度

- 单渲染线程，`AChoreographer_postFrameCallback64` 对齐 vsync；参数投递使用单槽邮箱（原子替换，latest-wins）：渲染线程在每个 vsync 取最新参数，没有新参数且图像未失效则**什么都不做**，不唤醒 GPU（空闲零功耗，不用定时器轮询）。
- 烘焙在 CPU 线程池，与上一帧的 GPU 执行重叠（两帧流水线：frame N 的 GPU 工作与 frame N+1 的烘焙并行）；格点/表通过 staging ring（2-3 个）上传，不阻塞渲染。
- 拖动中：交互质量（可降分辨率、跳过 NR/grain），松手 100-150 ms 后细化帧（现有 `interactive`/`exact` 分层语义保留），细化帧可被新输入取消（命令缓冲粒度）。

### 5.2 热与性能提示

- CPU 部分（解码、烘焙、编码）：ADPF `APerformanceHint`（API 33+）创建 hint session，报告实际耗时；API 29-32 无此接口则跳过。GPU 侧新版本 Android 提供 GPU 负载提示，**待核实可用 API 级别再接入**。
- `AThermal_getThermalHeadroom`（API 30+）+ `AThermal_registerThermalStatusListener`：headroom > 0.8 时降一档（交互分辨率 1600 -> 1200 -> 800；关闭 NR/grain 预览；帧率上限 60 -> 30）；> 0.95 时仅保留点算子；导出 tile 间插入休眠。恢复时带迟滞。
- 低电量模式 / 电池 < 15%：默认低一档，导出不受影响但串行执行。

### 5.3 GPU 与 CPU 的能耗推理

- 数据并行、带宽受限、访存规则的逐像素/可分离滤波，在 GPU 上每瓦性能通常显著高于 big CPU 核（GPU 低频低压、专用纹理单元与缓存）；CPU 路径需要唤醒多个大核并抬高频率，且尾延迟敏感。spike 在桌面 GPU 上的比值（CPU 28 线程 vs GPU）：1600 px 点算子 3.0 ms vs 0.06 ms GPU 时间；26 MP 48 ms vs 2.2 ms；60 MP 110 ms vs 5.4 ms；高斯 1600/26 MP/60 MP：21/372/922 ms vs 0.2/12/41 ms。**桌面离散 GPU 与手机 SoC 的比值不可外推**，真机比值待阶段 1 测量（含功耗：`dumpsys batterystats`、Perfetto 的 GPU/CPU 频率与 `power.rails`）。
- 零拷贝本身节能：去掉每帧约 20 MB 的内存流量（3.1 节）与 isolate 往返；CPU 在预览期间保持空闲，可以降到低频簇。
- 反例：很小的帧（缩略图 256 px）GPU 提交/同步开销（本 spike 提交+等待约 0.15 ms）可能高于 CPU 计算，**缩略图/批处理留在 CPU**，不抢占 GPU 与不唤醒大核（用 ADPF 把批处理绑到小核）。

### 5.4 回退与自检

> 已实现：见 10.9（自检用 192x128 合成图 + 两个用例，门限 mean <= 1.0 而不是 0.2，只抓坏驱动；黑名单为文本规则 + 默认拒绝软件实现；DEVICE_LOST 目前为本进程回退 CPU，重建留给阶段 4）。

1. `volkInitialize()` 失败 / 无 compute 队列 / `maxImageDimension2D < 4096` / 缺 `rgba16f` storage 支持 -> CPU 路径。
2. **启动自检**：首次启动（及驱动版本变化时）用 64x64 合成图跑一遍完整管线，与 CPU 参考比较 ΔE（mean <= 0.2），失败则把 (设备名, 驱动版本) 记入本地黑名单并回退 CPU。目的：对抗 Mali/PowerVR 老驱动的 shader 编译或 fp16 bug。几毫秒。
3. 运行期 `VK_ERROR_DEVICE_LOST` / 内存不足：销毁并重建一次，再失败则永久回退 CPU，本帧由 CPU 补出。
4. 后台：释放大块 GPU 内存（源图/中间图），回前台重建（源图保留在 CPU）。

## 6. 与 film 模块的整合

film 管线（`docs/film/architecture.md`）映射为同一套 pass 图的扩展：

| film 阶段 | GPU 对应 |
|---|---|
| A 曝光：3x3、色度坐标 -> 双三次原始 LUT（194x194 RGBA32F，Mitchell 权重） | 2D 纹理，4 次双线性取样实现 4x4 双三次（需要 RGBA32F 线性滤波，桌面/多数手机可用；缺失则退化为 16 次 texelFetch）。 |
| hook "exposure"：lens blur、halation、scatter | 降采样层高斯混合 + 上采样叠加（与第 1 节同一套基础层机制）。 |
| B 对数、C 显影：密度曲线 3x256x2、DIR 抑制剂 `q = Q d + q0` | 1D 表（SSBO）+ 点算子；DIR 扩散（hook "inhibitor"）是对 `q` 的小半径高斯。 |
| hook "density"：grain | 过程式噪声 / 层粒子模型：并行无邻域依赖，计算量与精度要点另见 film spec 5.2，需单独评估 Monte-Carlo 采样数与 GPU 随机数。 |
| D 打印+扫描：33^3 float4 LUT，四面体插值 | 与现有 lattice pass 同一个 shader（`LutSampler` 的 GPU 版），本 spike 即是它的原型。 |
| hook "scan"：scanner blur / USM | 可分离高斯 + USM，同 spike 的 blur pass。 |

film 的 CPU 内核已经是 `float4` 通道在 lane 内的布局，说明数据流本身适合移植；`renderPixelDirect()`（双精度）是 GPU parity 测试的 oracle。点算子阶段 A-D 在 GPU 上尽量融合为一个 pass（A..D 之间只有 hook 点需要落盘中间图：曝光后 / inhibitor / density / scan），所以 film 开启时是 3-4 个 pass，而不是 8 个。

## 7. spike 说明

> 历史记录。spike 的 `FramePipeline`、`tone_lut.comp`、`blur.comp` 已被生产后端取代并删除；`dapple gpu-bench` 现在是 10.8 的全管线 bench（`--mode/--sigma/--exact/--synth` 的旧语义不再存在，`--synth W H` 保留）。

### 7.1 内容

| 文件 | 作用 |
|---|---|
| `core/gpu/CMakeLists.txt` | 仅在 `-DDAPPLE_GPU=ON` 时由顶层 `CMakeLists.txt` 引入；Vulkan-Headers + volk 来自 `tools/_dl/vulkan`（缺失则 FetchContent，带 SHA256）；glslc 取自 NDK `shader-tools`；SPIR-V 经 `cmake/embed_spv.cmake` 内嵌为 C 数组。 |
| `core/gpu/shaders/tone_lut.comp` | 融合点算子：读 RGBA16F -> `* gain` -> 亮度 -> tone 表（浮点位模式索引）-> 格点（平方根域，手写四面体或硬件/手写三线性，运行时选择）-> 写 RGBA8 与 RGBA16F。 |
| `core/gpu/shaders/blur.comp` | 可分离高斯（一个 shader，方向与工作组形状为特化常量；64 像素 x 4 行，半径最大 48，共享内存分块，边缘钳制；竖直 pass 输出 RGBA8）。 |
| `core/gpu/src/vk_backend.cpp` | Vulkan 实例/设备选择（独显优先，`DAPPLE_GPU_DEVICE=n` 可覆盖）、内存/图像辅助、两条管线、时间戳查询、上传/回读。 |
| `core/gpu/src/gpu_bench.cpp` | `dapple gpu-bench`：GPU 对 CPU 烘焙/精确路径的 ΔE2000 与计时。 |
| 现有文件的改动（均为附加）：顶层 `CMakeLists.txt`（option + `add_subdirectory`），`cli/CMakeLists.txt`（`if(TARGET dapple_gpu)` 链接），`cli/main.cpp`（`#ifdef DAPPLE_HAVE_GPU` 的声明与分发）。**未改动 `renderer.*`**：`BakedLut` 的 `n / rgb / tone` 已是公开字段，无需新增访问器；`kBakeMax / kToneBase / kToneN` 在 spike 中重复定义，并在 `setLut` 里用 tone 表长度校验漂移。 |

### 7.2 用法

```
# 桌面（独立构建目录，不影响 build/desktop 与 build/desktop-app）
cmake -S . -B build/desktop-gpu -G Ninja -DDAPPLE_GPU=ON -DDAPPLE_BUILD_TESTS=OFF -DDAPPLE_WITH_OPENCV=OFF ...
cmake --build build/desktop-gpu --target dapple_cli
build/desktop-gpu/bin/dapple gpu-bench <ARW> [--mode tetra|hw|hw32|trimanual|both] [--exact] [--no-full]
                                      [--n 41] [--repeat 10] [--sigma S] [--synth 9504 6336] [--info]
# Android（NDK 28.2，android-24，arm64-v8a 与 x86_64 均已编译通过；x86_64 已在模拟器上运行）
cmake ... -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=... -DDAPPLE_GPU=ON
```

`--synth W H` 把已解码的图平铺为 WxH，作为 61 MP 机身的替身（样张里最大的 ARW 只有 26 MP）。

### 7.3 计时结果（桌面，RTX 4060 Ti 16 GB，Vulkan 1.4，驱动 591.44；CPU 为 28 线程）

单位 ms。「GPU」为时间戳查询；「提交+等待」含提交与 fence 等待（每个 pass 单独提交，真实管线会合并，这部分将下降）。

| 项目 | 1600 px (1.7 MP) | 26 MP | 60 MP（合成） |
|---|---|---|---|
| CPU 烘焙路径整帧（全核，spatial 关闭） | 3.0 | 48 | 110 |
| CPU 精确路径 | 46 | 584 | 1330 |
| 烘焙格点 41^3（CPU 多线程） | 5-8 | 13.5 | 30 |
| 源图转换 float RGB -> RGBA16F（CPU 多线程） | 11 | 53 | 146 |
| 源图上传（staging -> 设备，每张图一次） | 2-3 | 25 | 58 |
| 格点 + tone 表上传（每次参数变化） | 1.1 | 6.7（含重建图像，可优化） | 1.8 |
| **融合点算子 pass：GPU 时间** | **0.06** | **2.2** | **5.4** |
| 同上，提交+等待 | 0.15 | 2.4 | 5.7 |
| 回读 RGBA8（含 map + memcpy，PCIe） | 2.4 | 30 | 79 |
| 高斯（sigma 2 / 7.8 / 11.9）GPU 两个 pass | 0.22 | 12.0 | 40.8 |
| 同上 CPU（全核，float） | 21 | 372 | 922 |

要点：

1. 源图常驻时，**一次参数变化 -> 一帧** = 格点上传 + 一个 pass，1600 px 为 1.2-1.7 ms（见 `gpu-bench` 的「interactive frame」），60 MP 约 7.5 ms；CPU 烘焙路径分别为 3 ms / 110 ms。
2. **这只是桌面独显**：回读受 PCIe 影响（手机统一内存没有这个问题，也不需要回读：直接显示）；GPU 本身的绝对速度也不能搬到手机。手机真机的 GPU/CPU 比与功耗待阶段 1。
3. 瓶颈很清楚：全分辨率下 GPU 融合 pass 约 11-13 Gpx/s（带宽受限：读 8 B + 写 4+8 B/像素），一次上传（源图转换 + 拷贝）约 200 ms 级别，所以**源图要常驻，不要每帧重传**；导出 tile 化后转换/上传可与 GPU 计算流水线重叠。
4. 模拟器（Android x86_64，SwiftShader = **CPU 上的软件 Vulkan**，设备类型 4）：功能与 ΔE 通过（见 4.3），计时（1600 px 融合 pass 10.9 ms GPU / 35 ms 整帧，26 MP 160 ms，高斯 2064 ms，且解码 48.8 s）**没有代表性**：它在 4 个 CPU 核上模拟 GPU，仅证明构建、加载 libvulkan、管线创建、内存/图像/时间戳路径在 Android 上走得通。arm64 仅验证编译通过，未在真机上运行（真机不得触碰）。

### 7.4 spike 的已知缺口

无 vignette、无几何/重采样、格点外像素只做钳制（4.3）、tile 化只是设计、所有图像用 `GENERAL` 布局、每个 pass 一次提交+fence（无命令缓冲复用/无 pipeline cache/无异步计算队列）、没有 VMA 与分配池、不含 swapchain。以上均属阶段 1-3。

### 7.5 体积与许可影响

- 代码体积：`vk_backend` 目标文件 text 约 45 KB（arm64，未做 LTO/`--gc-sections`），volk 约百 KB 级目标文件（实际链接后仅保留用到的函数指针表），SPIR-V 两个 shader 共 11 KB。每 ABI 预计 < 200 KB，**无新的共享库**，无 Vulkan 链接依赖。
- 构建期依赖：glslc（NDK 内已有）；iOS 阶段再加 SPIRV-Cross。
- 许可：Vulkan-Headers Apache-2.0、volk MIT，均允许闭源分发；构建工具不分发。

## 8. 分阶段计划

| 阶段 | 内容 | 验收 | 估时 |
|---|---|---|---|
| 0（完成） | spike：点算子 + 高斯、桌面 ΔE/计时、Android arm64/x86_64 编译、模拟器运行 | 本文第 7 节 | -- |
| 1（完成） | 生产化 Vulkan 后端：设备/队列/能力探测、资源池、描述符缓存、pass 图、几何 + 全部空间算子 + 第二格点、tile 导出、自检 + 黑名单、`dapple_session_render_frame` / 导出 钩子（运行时开关，默认关）、`dapple_gpu_tests`（桌面 + SwiftShader，接入 ctest） | 本文第 10 节；**真机测量未做**（按约定不触碰车主真机） | -- |
| 2 | 零拷贝：Kotlin 插件（`SurfaceProducer`）+ 原生 `GpuView`（swapchain、vsync、latest-wins 邮箱）；`EditorEngine` 增加 `GpuViewBackend`（与现有 CPU `renderFrame` 并存，由能力探测切换）；直方图 GPU 归约；Dart 只收分析 JSON | 拖动滑杆无 `decodeImageFromPixels`、无像素跨 isolate；掉帧率与 CPU 占用对比 | 2-3 周（依赖 EditorEngine 稳定） |
| 3 | 全分辨率导出：基础层 + tile + halo、流式 JPEG；`dapple_session_export_jpeg` 的 GPU 路径；film 的 GPU 管线（LUT 链、halation、grain、DIR） | 61 MP 导出峰值内存 < 500 MB（App 总计）；ΔE 对 CPU `Full`；film vs `renderPixelDirect` | 3-4 周 |
| 4 | 调度与能耗：ADPF、`AThermal` 画质阶梯、低电量策略、后台释放、DEVICE_LOST 恢复、Perfetto 基线与回归阈值 | 20 分钟连续编辑温升/掉帧测试 | 1.5-2 周 |
| 5 | iOS：5a 可选 MoltenVK 快速过渡（数天）；5b 原生 Metal 后端（`gpu::Device` 的 Metal 实现 + SPIRV-Cross 管线 + `CVPixelBuffer` 纹理） | 同一套 pass/shader，同一套 parity 测试 | 3-4 周 |
| 6（v2） | NPU：LiteRT `ModelRunner`、mask 资源与 lattice 数组 | 模型延迟/功耗预算 | 单列 |

**推荐的下一步是阶段 1**：它不触碰 Dart/UI（EditorEngine 正在被别人修改），却能最早给出真机数据，并让 GPU 路径与 CPU 路径在 `dapple_session_render_frame` 后面可互换、可测试。总计到 Android 全量上线约 10-14 周（单人）。

## 9. 风险

- 驱动质量差异（Mali/PowerVR/Adreno 6xx 老驱动）：靠 6.3 自检 + 黑名单 + 持续的 `gpu-bench --info` 数据库；shader 保持简单（无子组依赖、无 fp16 必需）。
- Flutter `SurfaceProducer` 在不同 Android 版本/ROM 上的行为（Surface 重建、旋转、多窗口）：阶段 2 开头先做 Kotlin/NDK 小原型，必要时退到 `AHardwareBuffer`。
- 两套颜色实现的漂移风险：**格点烘焙只留 CPU 一份**，GPU 只做「表 + 插值 + 空间算子」；`kToneBase/kToneN/kBakeMax` 在 GPU 侧仍有一份拷贝（`session.cpp`），由 `ensureLut` 的 tone 表尺寸检查与启动自检/parity 测试兜底（尚未改成 `renderer.h` 公开常量，避免动共享文件；见 10.12）。
- fp16 源图的极暗部精度：spike 的 ΔE 已包含（源图就是 fp16）；高 EV 提亮（+4 EV）场景在阶段 1 的测试集里补充。

## 10. 阶段 1：生产后端（已实现）

### 10.1 文件

| 文件 | 内容 |
|---|---|
| `core/gpu/include/dapple/gpu.h` | 公开 API：`Backend`（设备 + 管线 + 池，全进程一份）、`Session`（每张照片：常驻源图、格点缓存）、`BackendOptions`、`FrameStats`、`runtimeEnabled()/sharedBackend()` |
| `core/gpu/include/dapple/gpu_hook.h`、`src/hook.cpp` | C API 与后端之间的唯一接缝：`hookRenderFrame` / `hookExport` / `statusJson`；任何失败都返回「未处理」，调用方走原 CPU 路径 |
| `core/gpu/src/vk_core.{h,cpp}` | Vulkan 层：设备选择 + 能力探测 + 黑名单、内存、**工作图像池**、管线（SPIR-V 构建期嵌入、specialization 常量变体、可选 `VkPipelineCache` 落盘）、**描述符集缓存**、**pass 图**（`Graph`）与帧槽（3 个：命令缓冲 + fence + 参数环 + staging + 回读缓冲） |
| `core/gpu/src/frame.{h,cpp}` | 把 `FramePlan` 变成 pass 图：点算子融合、各空间算子的 pass 链、整图分析（tile 导出用）、半径与 halo 计算 |
| `core/gpu/src/session.cpp` | `Backend`/`Session`：常驻源图、Lanczos 缩放缓存、格点上传、单帧渲染、**tile 导出**、自检、运行时开关 |
| `core/gpu/shaders/*.comp`, `common.glsl` | 11 个 shader 源文件、14 个 SPIR-V 变体（`point`、`sepconv`×4 变体、`down`、`nr_luma`、`nr_chroma`、`dehaze_apply`、`reduce`、`gf_ab`、`detail`、`resize`、`convert`） |
| `core/gpu/src/parity.{h,cpp}` | 合成测试图 + ΔE2000 比较（自检、测试、bench 共用） |
| `core/gpu/tests/gpu_tests.cpp` | `dapple_gpu_tests`：逐阶段 parity、Lanczos、tile 接缝、流式源、稳态不分配、C API 钩子、黑名单；ctest：`gpu_parity`、`gpu_parity_swiftshader` |
| `core/gpu/src/gpu_bench.cpp` | `dapple gpu-bench`：全管线分 pass 计时、上传、回读、CPU vs GPU、1600 px / 26 MP / tile 60 MP |
| `core/include/dapple/frame_plan.h`；`renderer.h/.cpp` 末尾各加一个只读访问器 | `Renderer::framePlan()`（几何、增益、暗角、空间参数，**由 CPU 参考解析**，GPU 不重复实现参数逻辑）与 `Renderer::bakeOverflow()`（第二格点） |

共享文件的改动（均为追加）：`renderer.h/.cpp`（上述两个方法）、`c_api.cpp`（`#ifdef DAPPLE_HAVE_GPU` 的 3 处钩子 + 2 个 C 函数）、`dapple_c.h`（`dapple_gpu_set_enabled`、`dapple_gpu_status_json`）、`tests/CMakeLists.txt`（`if(TARGET dapple_gpu)` 块）、`app/android/app/build.gradle.kts`（`-Pdapple.gpu=true` 时加 `-DDAPPLE_GPU=ON`）。`core/gpu/CMakeLists.txt` 末尾让 `dapple` 共享库链接 `dapple_gpu`，未开 `DAPPLE_GPU` 的构建不含任何 GPU 代码。

### 10.2 架构

- **设备与能力**：`Engine::create` 枚举物理设备，要求 compute 队列（优先无 graphics 的独立 compute 族）、`R16G16B16A16_SFLOAT / R32G32B32A32_SFLOAT / R32_SFLOAT / R8G8B8A8_UNORM` 的 storage + sampled 支持、`maxPerStageDescriptorStorageImages >= 4`（Vulkan 1.0 保证值；所有 kernel 的 storage image <= 3，只读的中间图一律用 sampled image 绑定）、共享内存 >= 16 KB。探测并记录 fp16 算术/16 位存储、子组大小与操作、最大图像尺寸、各堆大小、时间戳、统一内存（集显/CPU 型）。**kernel 不依赖 fp16 算术与子组**（fp16 只用于存储格式）。
- **资源池**：所有工作图像来自 `(格式, 宽, 高)` 池；请求尺寸向上取整到 32（>=1024 时 128）的倍数，使略有变化的预览尺寸共享条目；tile 导出对所有 tile 用同一分配尺寸。图像在 pass 图里按「最后一次使用」即时归还，同一帧内后续同键 transient 复用同一物理图像（别名）；超过预算（默认 min(显存/4, 1 GB)）时才在帧间 LRU 回收。稳态测试：参数每帧变化、尺寸不变，12 帧后池字节数恒定（`testSteadyState`）。
- **描述符**：每个 pass 的描述符集按 `(管线, 帧槽参数缓冲, 各资源 id)` 缓存，稳态帧零 `vkAllocateDescriptorSets`/`vkUpdateDescriptorSets`；参数块走动态偏移 UBO 环（每槽 512 KB，每 pass <= 256 B），没有 push constant 大小限制。缓存过大时只在记录前整体重置。
- **pass 图**：`Graph` 记录 dispatch / 拷贝节点与它们读写的资源。`Engine::submit` 先做**依赖分层调度**（RAW/WAR/WAW，同层独立链的 pass 相邻，如 texture / sharpen / clarity 分析），再按物理资源跟踪冒险，**仅在确有依赖时**插一个合并的内存屏障（所有图像常驻 GENERAL 布局，首次使用时惰性转换）。一帧一个命令缓冲、一次 `vkQueueSubmit`、一个 fence；没有 `vkQueueWaitIdle`；等待是 `vkWaitForFences`（驱动内睡眠，无忙等）。可选时间戳（`BackendOptions::profile`）给出逐 pass GPU 时间。
- **融合**：点算子（几何重采样 + 增益 + tone 表 + 格点 + 第二格点 + 暗角）是**一个** kernel（`point.comp`，几何模式用 specialization 常量：恒等 / 精确索引 / Catmull-Rom）；链上最后一个 kernel 通过 `LAST/GRAIN` 常量同时完成 grain 与 RGBA8 打包，不需要单独的收尾 pass。
- **异步回读**：结果经 `vkCmdCopyImageToBuffer` 进入 `HOST_CACHED` 的帧槽回读缓冲，`wait()` 后 memcpy 到调用方缓冲；tile 导出用 3 个帧槽流水（上一 tile 的 sink 与下一 tile 的 GPU 工作重叠，被 sink 占用的槽被 `pinSlot` 保护）。
- **源图常驻**：`Session::setSource` 只记录身份；首次渲染时用 `convert.comp` 把 float RGB（staging 双缓冲 24 MB 分块，CPU 只做 memcpy）转成 RGBA16F 上传一次；缩放副本（Lanczos）按尺寸缓存 2 份；格点（41³/65³ RGBA32F）+ tone 表 + 第二格点按烘焙 key 缓存 2 份，仅在烘焙结果变化时上传（~1 MB）。

### 10.3 管线映射（GPU kernel <-> CPU 参考）

| 阶段 | kernel / pass | CPU 参考 |
|---|---|---|
| 几何（旋转 90、straighten + 比例裁剪、flip、区域缩放） | `point`（`GEO` 常量；`Geometry::mapF` 逐行移植，Catmull-Rom 4x4 与 `sampleCubic` 同） | `renderer.cpp` `Geometry` / `sampleCubic` |
| 点算子 + 格点 + 暗角 | `point`（tone 表按 float 位模式索引、四面体插值、tie-break 同 CPU rank 排序） | `LutSampler`, `applyVignette` |
| 格点外像素 | `point` 内分支：任一分量 > 1.5 时改取**第二格点**（33³，[0, 8]，同样平方根域节点，由 `Renderer::bakeOverflow` 用同一个 `PointOp::colour` 烘焙） | CPU 的精确路径 `overflowOp->colour` |
| 亮度 NR | 4 个 box pass（亮度提取并入第一个）+ `nr_luma` | `denoiseLuma` |
| 色度 NR | `down`（f = 2/4，输出 R-Y/B-Y/Y）+ box（各通道独立半径，一个 pass 同时糊 u/v 与 y）+ `nr_chroma`（低分辨率双线性，与 `makeAxis` 同） | `denoiseChroma` |
| dehaze | `down` -> 暗通道（min 滤波 x2 + box x2）-> **无原子的 4 步归约求大气光** -> `dehaze_apply`（逐低分辨率点算透射再双线性）；负值为雾化 | `dehazeImage` |
| clarity | `down`(Y,Y²) -> box -> `gf_ab` -> box -> `detail` | `guidedBase` + `detailPass` |
| texture / sharpen | box x2（texture）/ box x3 或直接高斯（sharpen，sigma <= 2.6）+ `detail`（clarity+texture+sharpen 合成一次累加，同 CPU） | `detailPass` |
| grain | 无状态哈希（与 CPU 同一个 `hash2`/三角噪声/双线性），用**画面坐标**，tile 之间一致 | `addGrain` |
| 高质量缩小 | `resize`：可分离 Lanczos-3，支持域随缩放比变宽（抗混叠），任意输出矩形可由对应源窗口算出；导出缩小时可加 `resizeSharpen`（沿用 sharpen 的半径） | （CPU 为面积平均，不同滤波器） |

### 10.4 格点外像素（out-of-lattice）

spike 把 tone 后分量 > 1.5 的像素钳到格点边缘，26 MP 实拍最大 ΔE 5.23（CPU 对这些像素走精确路径）。现在 `Renderer::bakeOverflow` 用同一个 `PointOp` 再烘焙一张粗格点（33³，范围 [0, 8]，平方根域，tone 之后分量不可能更大，更大则钳制），GPU 在 `v > 1.5` 的像素上改查它。26 MP 实拍点算子最大 ΔE 由 5.23 降到 **1.81**（mean 0.016），测试图的饱和亮斑（R=3.0 的红、G=2.5 的绿）覆盖了该分支。成本：一次 36 K 次 `colour()` 求值（多线程，约 1-2 ms），随烘焙 key 缓存。

### 10.5 精度：fp16 存储必须显式舍入（驱动实测发现）

- 源图与中间图是 RGBA16F。**NVIDIA 驱动对 `imageStore` 到 RGBA16F 是截断（向零舍入），不是就近舍入**：每次存储引入 -0.5 ulp 的系统性偏差，表现为 GPU 比 CPU 一致地暗 1/255，点算子 parity 的 mean 0.147（37 % 像素差 1 级）；SwiftShader 为就近舍入（mean 0.040）。修复：所有写 RGBA16F 的 kernel 先用位运算做**就近偶数舍入到 10 位尾数**（`h16()`），使存储在任意舍入模式下都精确；RGBA8 输出同理显式 `floor(x*255+0.5)`（再 +0.25 避开驱动换算边界）。修复后桌面 GPU 与 SwiftShader 的 parity 数字几乎逐位相同。这类「驱动对未指定行为的选择」是自检要抓的问题，也是 parity 测试要同时跑硬件与 SwiftShader 的原因。
- fp16 源图本身的误差预算（CPU 在 fp16 舍入后的源上渲染 vs 浮点源）：mean 0.021 / p99 0.53 / max 1.43（5.8 % 像素差 1 级），所以点算子 parity 的 0.04 里约一半来自源图精度。
- `min` 滤波曾因 `mix(1e30, x, m)`（NVIDIA 用 `fma(m, x-1e30, 1e30)` 实现，x 被吞掉）得到全 0，改用布尔选择。教训：kernel 里不要用大哨兵值做浮点 `mix`。

### 10.6 逐阶段 parity（对 CPU `renderBakedRgba`，mean ΔE2000 门限 0.5）

测试图：合成场景（渐变、色块、棋盘/斜线边缘、暗部噪声、饱和亮斑），`Renderer::bake(41)`，`SpatialQuality::Full`。下表为 1536x1024 源图（640x448 的结果同量级，见 `build/gpu-tests-*.log`）。「桌面」= RTX 4060 Ti / 驱动 591.44（Vulkan 1.4）；「SwiftShader」= Android 模拟器 x86_64（AVD dapple_n5，模拟器自带的软件 Vulkan 1.2，`dapple_gpu_tests` 通过 adb 在模拟器内运行）。

| 阶段 | 输出尺寸 (1536x1024 源) | 桌面 RTX 4060 Ti: mean / p99 / max | SwiftShader（模拟器 x86_64）: mean / p99 / max |
|---|---|---|---|
| point (tone+lattice) | 1536x1024 | 0.0396 / 0.796 / 1.73 | 0.0396 / 0.796 / 1.73 |
| geometry rot90 | 1024x1536 | 0.0396 / 0.796 / 1.73 | 0.0396 / 0.796 / 1.73 |
| geometry rot270+flip | 1024x1536 | 0.0396 / 0.796 / 1.73 | 0.0396 / 0.796 / 1.73 |
| geometry crop 16:9 | 1536x864 | 0.0405 / 0.816 / 1.73 | 0.0405 / 0.816 / 1.73 |
| geometry straighten 4.5 + crop 4:5 | 770x963 | 0.0347 / 0.575 / 1.58 | 0.0347 / 0.575 / 1.58 |
| geometry rot90+straighten -7 | 868x1303 | 0.0408 / 0.838 / 1.69 | 0.0408 / 0.839 / 1.69 |
| vignette -40 | 1536x1024 | 0.0392 / 0.779 / 1.90 | 0.0393 / 0.779 / 1.90 |
| vignette +30 | 1536x1024 | 0.0395 / 0.766 / 1.70 | 0.0395 / 0.766 / 1.70 |
| noise reduction luma 50 | 1536x1024 | 0.0616 / 0.841 / 1.80 | 0.0616 / 0.841 / 1.74 |
| noise reduction chroma 60 | 1536x1024 | 0.0384 / 0.830 / 1.72 | 0.0384 / 0.830 / 1.72 |
| dehaze +40 | 1536x1024 | 0.0491 / 0.818 / 1.80 | 0.0491 / 0.818 / 1.80 |
| dehaze -40 | 1536x1024 | 0.0399 / 0.732 / 1.69 | 0.0399 / 0.731 / 1.69 |
| clarity +40 | 1536x1024 | 0.0442 / 0.817 / 1.72 | 0.0442 / 0.817 / 1.72 |
| clarity -30 | 1536x1024 | 0.0372 / 0.741 / 1.79 | 0.0372 / 0.741 / 1.79 |
| texture +50 | 1536x1024 | 0.0518 / 0.842 / 1.69 | 0.0518 / 0.842 / 1.69 |
| texture -40 | 1536x1024 | 0.0362 / 0.587 / 1.70 | 0.0362 / 0.587 / 1.70 |
| sharpen 70 r1.0 | 1536x1024 | 0.0400 / 0.799 / 1.81 | 0.0400 / 0.799 / 1.81 |
| sharpen 70 r2.5 | 1536x1024 | 0.0477 / 0.840 / 1.67 | 0.0477 / 0.840 / 1.67 |
| grain 40 | 1536x1024 | 0.0385 / 0.787 / 1.77 | 0.0385 / 0.787 / 1.77 |
| all spatial + geometry | 1457x971 | 0.0725 / 0.875 / 1.85 | 0.0725 / 0.875 / 1.85 |
| interactive quality (no grain/NR) | 1536x1024 | 0.0362 / 0.690 / 1.62 | 0.0362 / 0.690 / 1.62 |
| zoom region 1:1 | 400x267 | 0.0398 / 0.693 / 1.42 | 0.0398 / 0.692 / 1.42 |

补充：

| 项 | 桌面 mean / p99 / max | 说明 |
|---|---|---|
| Lanczos 缩小到 1200 / 640 / 300 px（GPU 管线 vs 双精度 CPU Lanczos 参考后再走 CPU 管线） | 0.043 / 0.86 / 1.73；0.041 / 0.80 / 1.59；0.040 / 0.62 / 1.41 | 含 straighten 2 度 + sharpen |
| 26 MP 实拍（DSC00421.ARW）点算子 | 0.016 / 0.48 / 1.81 | `dapple gpu-bench` |
| 26 MP 实拍 interactive / 全质量 | 0.046 / 0.73 / 1.85；0.059 / 0.81 / 1.92 | 全部空间算子 |
| 1600 px 实拍 点 / interactive / 全质量 | 0.025 / 0.58 / 1.79；0.047 / 0.74 / 1.88；0.055 / 0.80 / 1.78 | 模拟器 SwiftShader 同样的三项：0.025 / 0.047 / 0.055（`build/gpu-bench-emu-x86_64.log`） |
| C API 钩子：`render_frame` 区域缩放（无重采样） | 0.048 / 0.80 / 1.48 | GPU 开 vs 关，同一会话 |
| C API 钩子：`dapple_session_render` 导出（CPU 精确路径 vs GPU 烘焙 tile 路径） | 0.088 / 0.83 / 2.49 | JPEG 源（display-referred） |
| C API 钩子：`render_frame` 缩到 1000 px | 0.74-1.19（门限放宽到 2.0） | **滤波器不同**：GPU 用 Lanczos-3，CPU 用面积平均；测试图是 1 px 噪声 + 6 px 棋盘，对滤波器极端敏感 |

所有阶段 mean 0.036-0.073，远低于 0.5 门限；max 来自 8 位量化与 fp16 的 1 级差 + 个别格点附近像素。

### 10.7 全分辨率 tile 导出

- **结构**（`Session::exportTiled`）：整图输出按 `tile`（默认 2048，取 4 的倍数）切成核心矩形，工作区 = 核心 + halo（夹到画面内）。halo 由各局部算子的实际半径累加：`2*r_NR亮度 + (2*rl+3)*f_NR色度 + max(2*r_texture, 3*r_sharpen 或 R_gauss)`，向上取 4 的倍数（全图 61 MP、系数 x4.6 时约 120-170 px）。**工作区原点是 4 的倍数**，使色度 NR / 低分辨率网格与整图网格对齐。
- **大尺度量只算一遍**（设计 4.4 的「基础层」）：dehaze 的暗通道/大气光、clarity 的引导滤波系数在 1/4 网格上（61 MP -> 2376x1584），由 tile 逐块填入（`down` 带偏移写入全局网格）再整体分析。因为这些量依赖前一级的结果，导出分 **3 遍**：(1) 点 + NR -> 平均进 `color` 网格 -> 暗通道/大气光；(2) 再加 dehaze -> 平均 (Y,Y²) -> 引导滤波；(3) 全部 -> RGBA8 回读。每遍对每个 tile 重算点算子与 NR（GPU 便宜，换来内存只与 tile 有关）。不需要的遍自动跳过（无 dehaze/clarity 时只有一遍）。
- **源图**：能常驻（<= 4000 万像素且在显存预算内）就常驻，否则**逐 tile 流式**：由几何算出 tile 工作区在（缩放后）源空间的包围盒（缩放时再加 Lanczos 支持域），只转换并上传这一窗口（`convert.comp`，双缓冲），并在 GPU 上缩放。61 MP 源图不需要整张上传（481 MB）。
- **缝**：核心矩形边缘、halo、全局网格对齐都按像素精确设计，测试结果是**逐位相同**：

| 图 | 路径 | tile 尺寸 | 与单帧渲染相差 |
|---|---|---|---|
| 1500x1000，全部空间算子 + 暗角 | 常驻 / 流式 | 128, 256, 400, 1024 | 0 / 255，0 个像素 |
| 1500x1000，全部空间算子 + straighten 3 + 4:5 裁剪 | 常驻 / 流式 | 128, 256, 400, 1024 | 0 |
| 1500x1000，旋转 90 + dehaze + clarity + NR，缩到 900 px（Lanczos + resizeSharpen） | 常驻 / 流式 | 128, 256, 400, 1024 | 0 |
| 4000x2667（系数 x1.95，更大半径），全部空间算子 / + straighten + crop / + 旋转 + 缩小 | 流式 | 512, 1000, 2048 | 0 |
| **9504x6336（60.2 MP，系数 x4.6）**，全部空间算子，tile 2048，流式 | 流式 vs 整帧（常驻）单次渲染 | 2048 | **0 / 255，0.0 %**（`gpu-bench --seam`） |

  tile 128 时 halo 占比极大仍然逐位一致，说明 halo 公式有余量。SwiftShader（模拟器）同一组 tile 测试同样 0 差异。
- **内存**（60.2 MP，tile 之后池中保留的图像；3 个帧槽流水中）：tile 512：255 MB，tile 1024：318 MB，tile 2048：565 MB（另有 3 个 24 MB 级 staging、全局 1/4 网格约 60 MB）。手机建议 tile 1024（峰值 < 400 MB）。
- 速度（桌面，60.2 MP，全部空间算子，流式源）：tile 512：1823 ms，tile 1024：1243 ms，tile 2048：1000-1422 ms（GPU 时间 726-962 ms；3 遍 + 逐 tile 流式上传）。对比整帧 61 MP 单次渲染 617 ms；CPU 全质量 26 MP 就要 812 ms。

### 10.8 计时（桌面，RTX 4060 Ti，CPU 28 线程；`dapple gpu-bench DSC00421.ARW --synth 9504 6336 --seam --repeat 9`）

「GPU 帧」= 源图常驻后每帧的 记录 + 提交 + 等待 + 回读 + memcpy 到调用方缓冲。桌面是独显，**回读 15-18 ms（26 MP）走 PCIe、memcpy 到调用方缓冲 33-71 ms 走主机内存**，这两项在手机统一内存上基本消失，所以下面同时给出纯 GPU 时间（时间戳）。

| 场景 | CPU 烘焙路径 | GPU 帧 | 其中 GPU 时间（passes / barriers） | 加速 |
|---|---|---|---|---|
| 1600 px 点算子 | 2.5 ms | 4.0 ms | 0.83 ms（2 / 2；点 kernel 0.06 ms，其余是回读） | 0.6x（回读主导） |
| 1600 px interactive | 19.3 ms | 4.7 ms | 1.47 ms（27 / 21） | 4.1x |
| 1600 px 全质量 | 46.7 ms | 6.1 ms | 2.35 ms（38 / 32） | 7.6x |
| 26 MP 点算子 | 40.9 ms | 74.0 ms | 17.6 ms（点 kernel 0.87 ms = 30 Gpx/s；其余回读） | 0.6x |
| 26 MP interactive | 783.7 ms | 70.9 ms | 35.2 ms | 11.0x |
| 26 MP 全质量 | 812.0 ms | 122.3 ms | 48.2 ms（38 / 32） | 6.6x |
| 1600 px 预览（由常驻 26 MP 源 Lanczos 缩小，interactive） | -- | 首帧 36.6 ms（含缩放 pass），之后 8.25 ms | -- | -- |

一次性成本：首帧 1600 px 点算子 41 ms（管线创建 + 格点上传 + 源上传 13 MB）；26 MP 首帧 209 ms（上传 210 MB 的 RGBA16F + 管线）。26 MP 全质量逐 kernel（GPU ms）：回读 18.7、box 15.8、gauss 3.7、down 2.2、detail 2.2、nr_luma 1.7、point 1.4、nr_chroma 1.3、dehaze_apply 1.1、min 0.3、gf_ab 0.2、reduce 0.05。

tile 导出 60.2 MP：见 10.7。模拟器 SwiftShader（CPU 软件光栅）只用于正确性：1600 px 全质量一帧 917 ms，数字无参考价值。

### 10.9 自检、黑名单、回退

- **启动自检**（`Backend::create`，默认开）：桌面冷启动实测整个 `gpu-bench --info` 进程 2.7-3.6 s，其中设备/实例初始化约 1.6-2.1 s，自检约 1.0 s（主要是首次创建约 15 个管线变体，之后的帧本来也要付；有 `DAPPLE_GPU_PIPELINE_CACHE` 时更短；Android 真机待测）。**首次 GPU 帧会同步等待它**，所以宿主应在启动后于后台线程调用一次 `dapple_gpu_status_json()` 预热。用 192x128 合成图，两个用例（点 + 几何 + 暗角；全部空间算子 + grain）经**整条 GPU 管线**渲染并与 CPU `renderBakedRgba` 比较：mean <= 1.0、p99 <= 5、max <= 25 才通过（刻意比 parity 门限宽，只抓「坏驱动」：全零、乱码、NaN、错误的格点索引）。失败 -> `Backend::create` 返回空并给出原因，本进程不再重试，调用方走 CPU。设备 `key()`（`vendor:device:driverVersion`）供宿主持久化「此设备自检失败」。
- **黑名单**（`BackendOptions::blocklist` / 环境变量 `DAPPLE_GPU_BLOCKLIST`，格式 `vendor=0x5143;name=Adreno (TM) 6;driver=子串;drvmin=;drvmax=;reason=...`，规则用换行或 `|` 分隔）；内置规则：**软件 Vulkan 实现（`VK_PHYSICAL_DEVICE_TYPE_CPU`：SwiftShader/llvmpipe）默认拒绝**，只有 `allowSoftware`（测试 / 模拟器，env `DAPPLE_GPU_SOFTWARE=1`）才放行。被拒设备会换下一个候选。内置的硬件设备规则为空（目前没有已知坏设备，自检是主要闸门）；规则可随数据更新下发。
- 运行期：`VK_ERROR_DEVICE_LOST` -> 钩子置位 `broken`，本进程余下时间走 CPU；任何一次 GPU 渲染失败返回空，当帧立即由 CPU 补出；film 开启（`FramePlan::supported = false`）、源图超过显存预算或设备图像尺寸限制 -> 走 CPU。
- 管线缓存：环境变量 `DAPPLE_GPU_PIPELINE_CACHE=<路径>` 时落盘 `VkPipelineCache`（Android 上由宿主给出 `getCacheDir()` 下的路径，缩短冷启动）。

### 10.10 运行时开关与集成（Dart / EditorEngine 无需改动）

- 判定顺序：`dapple_gpu_set_enabled(1/0)`（C API，`-1` 恢复默认）> 环境变量 `DAPPLE_GPU=1` > Android 系统属性 `debug.dapple.gpu=1` > **默认关**。用 adb 在真机上开关：`adb -s <serial> shell setprop debug.dapple.gpu 1`（重启 App 或下一帧生效，每帧读取）。`dapple_gpu_status_json()` 返回 `{"enabled","available","device","deviceKey","selfTest"[, "reason"]}`。
- `dapple_session_render_frame`：在原 `renderBakedRgba` 之前，若开关为开，调用 `hookRenderFrame(session->gpu, s->source, renderer, bake, fo, srcLong)`：**使用完整分辨率源图 + GPU Lanczos 缩放**（不再用 CPU 的缩放副本），返回同样的 malloc'd RGBA8，Dart 侧完全无感；返回空则落回 CPU。烘焙（`renderer.bake`）仍在 CPU（缓存的两个槽），GPU 只上传结果。
- `dapple_session_export_jpeg` / `dapple_session_render`（经 `renderSessionParams`）：输出长边 <= 0 或 >= 1500 且 `toneReach == 1` 时，用 `bake(65)` + `exportTiled`（`resizeSharpen = 0.2`）得到 RGB，再交给原有 JPEG 编码；缩略图/look 预览仍走 CPU。注意**导出由「CPU 精确路径」变为「烘焙路径」**（65³ 格点，对精确路径 ΔE 约 0.1-0.3，见上表），这是设计 4.3 的既定取舍。
- 构建：默认不含 GPU 代码。桌面/CLI：`-DDAPPLE_GPU=ON`；Android APK：`flutter build apk --release --target-platform android-arm64 -Pdapple.gpu=true`（`build.gradle.kts` 追加 `-DDAPPLE_GPU=ON`；glslc 取自 NDK 的 shader-tools）。

### 10.11 APK 体积（`flutter build apk --release --target-platform android-arm64`）

两次构建源码状态相同（连续构建），均为 release、`android-arm64`，Gradle 属性控制：

| 构建 | 命令 | APK | `lib/arm64-v8a/libdapple.so`（未压缩） |
|---|---|---|---|
| 开关关（默认） | `flutter build apk --release --target-platform android-arm64` | 27 238 078 B（25.98 MiB） | 6 596 600 B |
| 开关开 | 同上 + `-Pdapple.gpu=true` | 27 532 990 B（26.26 MiB） | 6 893 528 B |
| **差值** | | **+294 912 B（+288 KiB，+1.08 %）** | +296 928 B（+290 KiB） |

增量来自后端代码 + 内嵌 SPIR-V（14 个变体，约 100 KB 量级）+ volk（运行时 `dlopen libvulkan.so`，无链接期依赖）。两个 APK 的 `libdapple.so` 都导出 `dapple_gpu_set_enabled`（关闭的构建里恒返回 0），只有开的构建含 Vulkan 代码；运行时开关默认关，所以开的构建在不设开关时行为与关的构建相同。

### 10.12 film 的 pass 如何接入 / 已知缺口

**film 的 GPU 化是后续工作，本阶段未触碰 `film*`。** 接入点已经留好：

- `FramePlan::supported` 现在在 film 开启时置 false（`why = "film simulation"`），钩子自动回退 CPU。
- film 的非空间部分已经是「LUT 烘焙」：33³ float4 打印+扫描 LUT = 与 `point` 同形的四面体采样，可直接作为 `point.comp` 的另一对 `sampler3D` 绑定（或融合进同一 kernel 的第二段，`GEO`/`LAST` 之外再加一个 specialization 常量 `FILM`）；曝光段的 194x194 原始 LUT 是 2D 纹理 + 一个新的点 kernel 变体。`Graph` 的绑定串（`"IBSSII"` 一类）与 `Engine::pipeline` 的 specialization 机制已支持任意新绑定组合。
- film 的空间部分（grain、halation、DIR 扩散、scanner blur/USM）= 新的 `sep` 变体（已有的 box / 高斯 / min 与按通道半径的 kernel 可复用）+ 过程式 grain kernel；它们是 `buildFrame` 里在 `point` 与 `detail` 之间/之后的新 stage 函数。**大尺度量（halation 的大 sigma、DIR 扩散）用现有的 `GlobalRes`（1/4 或更低分辨率的整图网格，tile 先累积后分析）机制**，halo 公式加一项即可；tile 逐位一致的测试框架可原样复用。
- 需要在 `spatialRadii()` 里加入 film 的半径，`FrameJob` 里加 film 的表资源；pass 图、调度、池、自检（再加一个 film 用例）不需要改。

已知缺口 / 优化机会：

1. `box` 滤波是 O(R) 的共享内存直加（26 MP 全质量里最贵的 kernel，15.8 ms）；改为共享内存前缀和 / 子组扫描可降到 O(1)（真机 Mali/Adreno 上预计更有价值）。
2. 桌面回读（PCIe + memcpy）占 26 MP 帧的一半以上；零拷贝（阶段 2）后消失。
3. 常量 `kBakeMax/kToneBase/kToneN` 在 GPU 侧有一份拷贝（见第 9 节风险）；理想做法是 `renderer.h` 暴露公开常量，需要碰共享文件，留给协调后再做。
4. `reduce` 每帧 4 个 pass + 4 个 barrier；整图统计（直方图）可并入同一机制。
5. fp16 源图的高 EV（+4 EV）暗部精度未单独测试集。
6. 真机未测：Adreno/Mali 的 `maxPerStageDescriptorStorageImages`、共享内存、`HOST_CACHED` 回读性能、tile 大小的最佳值、功耗；管线创建时间（冷启动）。

### 10.13 阶段 2（零拷贝 + 真机测量）需要什么

1. **真机测量（需车主同意）**：在 Find N5 与低端机上跑 `dapple gpu-bench`（交叉编译的 arm64 `dapple` 已可构建：`build/droid-gpu-arm64-v8a`），记录自检结果、逐 pass 时间戳、统一内存下的回读成本、3 个 tile 尺寸的峰值内存与耗时、功耗（Perfetto / battery stats）、`adb shell setprop debug.dapple.gpu 1` 下的 UI 帧率。把设备 `key()` 与自检结果收集成黑名单依据。
2. **输出到 Surface 而不是回读**：`FrameJob` 已把最后一个 pass 的目标抽象为 `out8` 图像；阶段 2 在 `Engine` 里加 swapchain / `AHardwareBuffer` 导入（`VK_ANDROID_external_memory_android_hardware_buffer`，需要 1.0 之外的设备扩展与队列族所有权处理）并把 `copyImageToBuffer` 换成 `vkQueuePresent`/blit；Flutter 侧 `SurfaceProducer` + `Texture` 的 Kotlin 插件。
3. **vsync 对齐与 latest-wins**：当前 `render()` 是同步的；需要一个渲染线程 + 单槽邮箱（`submit`/`wait` 已拆开，帧槽支持流水），ADPF/AThermal 接入。
4. **直方图/分析**：GPU 归约（`reduce.comp` 的模式可扩展），Dart 只收分析 JSON。
5. **背景释放 / DEVICE_LOST 重建**：`Session` 的常驻资源可从 CPU 源图重建（已有 `dropResident`）；还需要「Engine 重建」路径（目前 DEVICE_LOST = 本进程回退 CPU）。
6. 解决预览缩放滤波器差异（GPU Lanczos vs CPU 面积平均）的产品决定：保持 GPU Lanczos，或加 `FrameOptions` 的 box 模式以便 CPU/GPU 切换时画面一致。
