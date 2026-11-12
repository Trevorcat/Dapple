# Dapple render engine: generic node pipeline, CPU/GPU executors, graphics HAL

Status: **design, v1 (architect draft)**. No code was changed for this document; no new measurements were taken (every number is cited from a repo file or marked *estimate*). Supersedes the "phase 2..5" outlook of `docs/engine/gpu-backend.md`; the facts in that document (phase-1 Vulkan results, section 10) stay valid and are cited here as *GB §n*. Film facts are cited as *FA §n* (`docs/film/architecture.md`), device measurements as *TR* (`tools/throttle/README.md`, "Results" tables), product requirements as *REQ* (`docs/design/requirements.md`).

---

## 摘要（给负责人，中文）

**一句话**：把渲染做成"一张节点图 + 两个执行器（CPU / GPU）+ 一层很薄的 compute HAL"。曝光、色调、几何、降噪、清晰度、胶片模拟全部是图里的节点；融合、tile 与 halo 传播、中间结果缓存、按参数依赖失效，只在执行器/规划器里实现一次；每个节点的 CPU 内核和 GPU shader 分别手写，用逐节点 parity 测试保持一致。

**关键决定（都在你已定的 5 条决策之内）**

1. **三层**：节点图 IR（`Graph`/`NodeDesc`/`ParamSet`）→ 共享规划器（`Planner`：融合、tile、halo、别名、缓存决策）→ 两个执行器（`CpuExecutor`、`GpuExecutor`）。规划器只写一份，执行器只管"怎么跑一个 stage"。
2. **缓存用内容寻址**：每个节点输出的 key = hash(节点类型、它声明依赖的那几个参数、输入的 key、帧几何)。拖一个滑杆 = 改了几个参数 = 一部分 key 变了 = 只有这部分重算，没有手写的"脏标记"。每个节点的依赖声明由**一致性测试**证明（扰动一个未声明的参数，输出必须逐位不变）。
3. **表（格点、色调表、胶片 LUT）永远在 CPU 上烘焙**，作为 `Table` 节点缓存，GPU 只上传；这样颜色算法只有一份实现。例外（建议，需要你批准）：低端机上 57³ 格点烘焙是 CPU 瓶颈，后期可加一个 GPU 烘焙节点，CPU 版保留为参照。
4. **融合**：CPU 用"256 像素块在 L1 里串起所有逐点节点"（和现在 film 的做法一样，通用）；GPU 用"构建期登记的融合配方"（一个 shader 里用 specialization 常量开关若干节点，和现在的 `point.comp` 一样），没有配方时每个节点自己有独立 shader，正确性不依赖融合。
5. **HAL**：约 15 个调用（缓冲、图像、compute 管线、绑定、dispatch、屏障、fence、回读、外部内存）。Vulkan（主）与 GLES 3.1（备）映射到同一接口，Metal 以后加。着色器 GLSL 450 → glslc → SPIR-V → SPIRV-Cross → GLES 310 / MSL，**构建期完成，设备上不编译 GLSL 以外的任何东西**。
6. **GLES 3.1 的硬约束会反过来影响 kernel 写法**：只有 `r32f/r32ui` 的存储图像可以读写同一个绑定（现在 4 个 shader 对 `rgba16f` 做读写，必须改成乒乓）；工作组不保证 256 个线程（现在 16×16）；没有 specialization 常量（用宏注入）；纹理最大尺寸只保证 2048。这些已经写进规划器的"能力约束"，见第 6 节表格。
7. **失败语义**：不支持（规划期发现）→ 整帧走 CPU，结果缓存；瞬时失败（内存、超时）→ 本帧 CPU、会话继续用 GPU；设备丢失 → 重建一次，再失败本会话固定走 CPU。C API 不变，调用方看到的永远是一张图。
8. **取消 = latest-wins**：每个渲染请求带一个代号，执行器在 tile/stage 边界检查；GPU 命令缓冲提交后不能中止，所以终稿渲染按 sweep/tile 拆成多次提交。
9. **胶片**：拆成 3 个 `Table` 节点（原始曝光 LUT、曲线+DIR 矩阵、打印+扫描 LUT，各自按更细的依赖缓存，拖"暖色"只重烘 ~10 ms 的打印 LUT）+ 逐点/邻域节点（曝光、显影、DIR 扩散、颗粒、打印扫描、眩光、扫描）。它们复用主管线的 `Blur`（金字塔 + 高斯混合）、`Reduce`、四面体 LUT 采样、tile/缓存。
10. **迁移**：先把 CPU 渲染器逐段搬成节点（每步 **字节级一致** 的哈希门禁，旧代码仍是金标准），再上 GPU 执行器（沿用 phase-1 的 shader 与 parity 测试作门禁），再 GLES，最后零拷贝显示。C API 的函数与结构体保持兼容，只加不改。

**需要你（或 lead）拍板的问题（详见第 12 节）**

- **Q1 真机**：任何 GPU 数字（Adreno/Mali 的 fp16 舍入、管线编译时间、回读成本、功耗）都还没在真机上测过。需要你明确允许在 N5 和一台低端机上跑只读基准（现在禁止碰 adb 真机）。
- **Q2 缩放滤波器**：CPU 预览缩小用面积平均，GPU 用 Lanczos-3，两者在高频内容上差 0.7–1.2 ΔE（GB 10.6）。建议统一成 Lanczos-3（改 CPU，放在"逐位一致迁移"之后单独一步）。
- **Q3 源图内存**：CPU `SourceImage` 是 float RGB（12 B/像素），6000 万像素 = 722 MB，导出"在手机内存内"做不到。需要改成 fp16 / 磁盘分块的 `SourceStore`（会碰解码器和会话，别人正在改）。
- **Q4 GPU 烘焙格点**：是否允许颜色阶段有第二份实现（GPU 版），换取低端机 colour 拖动的速度。
- **Q5 默认开启策略**与最低支持设备（Vulkan 1.0 / GLES 3.1 都没有的设备是否直接 CPU-only）。

---

## 1. Goals, non-goals, measurable targets

### 1.1 Goals

1. One description of the work (a node graph) that two executors run, with fusion, tiling/halo, intermediate caching and invalidation implemented **once** (planner), not once per renderer.
2. Interactive preview and export on the GPU when the device passes the startup self-test; CPU path stays correct, is the test oracle and the fallback.
3. Film simulation is part of the same graph and inherits GPU execution, tiled export and caching.
4. C API (`dapple_c.h`) stays source- and binary-compatible for the Dart side.
5. Everything is testable without a phone: per-node CPU-vs-GPU parity on desktop GPU, SwiftShader and a GLES-on-ANGLE target.

### 1.2 Non-goals

- Halide-style single source for CPU and GPU kernels (decided: separate kernels + parity tests).
- Dawn/wgpu, OpenCL, DSP (Hexagon) in the pixel path, runtime shader generation on the device.
- Further extreme performance work on the CPU path (bug fixes, correctness and "no regression" only). The CPU executor must not be slower than today's renderer by more than the measurement noise (about 10-15 %, TR) on the same `edit-bench` scenarios.
- Mixed CPU/GPU execution of one graph in v1 (a frame runs wholly on one executor; see 5.9).
- HDR output, AI/NPU nodes (the node domain enum reserves `Npu`, nothing else is designed).

### 1.3 Targets

Baselines are the v3 rows of the TR tables (emulator throttled to the CPU speed of the class; **GPU/raster time is not modelled there**, TR "Limits"). Targets marked *target* are decisions; numbers marked *estimate* are my extrapolations and must be replaced by measurements.

| Metric | Today (CPU path, source) | Target | Notes |
|---|---|---|---|
| Low-end (SD680 class): drag updates/s | 3.2 (TR lowend v3) | **>= 15** | needs a real device to verify (Q1); emulator proxy below |
| Low-end: edit->present p95 | 663 ms (TR) | **<= 120 ms** | |
| Low-end: finger-up -> final frame | 915 ms (TR) | **<= 400 ms** | |
| Low-end engine proxy, throttle `lowend`, GPU off | 90-100 ms per interactive 1080 exposure frame (TR "colour-lattice reuse") | CPU critical path per GPU frame **<= 30 ms** (*estimate*: bake 41^3 = 45 ms at low-end TR; reuse removes it for tone drags; colour drags need Q4) | |
| Mid (SD 7 Gen 3) | 11.4 upd/s, p95 162 ms, final 320 ms (TR) | no regression; >= 30 upd/s | informational |
| N5 (8 Elite), drag | 13.2 upd/s, p50 39 ms, p95 76 ms, final 137 ms (TR n5 v3) | **120 Hz friendly**: engine frame <= 8.3 ms (GPU <= 4 ms + CPU critical path <= 2 ms, two frames in flight); edit->present <= 2 vsync (16.7 ms) p50, <= 25 ms p95 | REQ 4.15.16 / 6.10 say render <= 8 ms on N5 (still marked "to verify"). Desktop GPU time for the 1600 px interactive graph is 1.47 ms, full quality 2.35 ms (GB 10.8); a phone GPU is *estimated* 3-6x slower, i.e. 4-9 ms: **does not obviously meet 4 ms; measure first** |
| Export 60 MP, memory | GPU tile export 9504x6336: pool 318 MB at tile 1024, 565 MB at tile 2048 (GB 10.7, desktop) | **peak anonymous memory added by export <= 500 MB** (GB section 8 phase 3 criterion), any tile size chosen by the planner from the budget | needs `SourceStore` (5.6, Q3): a float-RGB 60 MP source alone is 722 MB |
| Export 60 MP, time | desktop GPU 1.0-1.4 s (GB 10.7) | GPU: **<= 10 s** on N5/mid (*estimate*); CPU fallback: must finish without OOM, no time target (*estimate* 1-2 min on low-end: 26 MP full quality = 812 ms on 28 desktop threads, GB 10.8, scaled by ~0.12 x 4 cores) | |
| Parity, non-film graph, GPU vs CPU baked path | mean 0.036-0.073, p99 0.69-0.88, max <= 1.92 dE2000 (GB 10.6) | mean <= 0.1, p99 <= 1.0 | per-node budgets in 8.5 |
| Idle | 0 work, 0.28 s/s CPU left after last frame (TR v3) | 0 wakeups, 0 GPU submissions when no new params | |
| Self-test | desktop ~1.0 s (GB 10.9), Android not measured | **<= 300 ms off the UI thread** with a warm pipeline cache (*estimate*) | |
| Binary size | +288 KiB APK for 14 SPIR-V variants + Vulkan code (GB 10.11) | <= +1.5 MB for the whole engine incl. GLES strings and film shaders (*estimate*) | |

---

## 2. Starting point (what exists)

| Piece | Where | Role in the new design |
|---|---|---|
| `Renderer` (bake + `renderBakedImpl`), geometry, `LutSampler`, `PointOp` | `core/src/renderer.cpp` | CPU kernels of the point nodes; keeps the public `Renderer` API for tests/CLI; `pointKey()/colourKey()` string keys are replaced by node keys |
| `applySpatial` and its operators (NR luma/chroma, dehaze, `guidedBase`+`detailPass`, `addGrain`) | `core/src/spatial.cpp` | CPU kernels of the spatial nodes, whole-frame float planes (running-sum box blur) |
| `ThreadPool` (persistent, spin-then-block, chunked `run`, big-core affinity, `inBackground` bypass) | `core/src/parallel.h` | CPU executor's only parallel primitive (stays) |
| `FramePlan` | `core/include/dapple/frame_plan.h` | the first "resolved parameters" struct; generalised into per-node resolved params |
| Film: `FilmModel` bake, `FilmSimulator`, kernels (`film_kernels_impl.inc`), spatial engine (`film_spatial*`) | `core/src/film*.cpp` | bake -> `Table` nodes; kernels -> film node CPU impls |
| Vulkan phase-1: `vk_core` (Engine, pool, pipelines, descriptor cache, `Graph` with dependency layering + merged barriers), `frame.cpp` (`buildFrame`), `session.cpp` (resident source, tile export, self-test), 11 shaders | `core/gpu/**` | HAL Vulkan backend + GPU node impls + generic tile executor (section 10.3) |
| C API `DappleSession` with `bakes[3]`, `scaled[]`, `proxy`, `lastParams`, one `RenderThread` | `core/src/c_api.cpp` | session state moves into `EngineSession`; the C functions stay |
| App: interactive tier 1080 px (ladder 1080/810/640/480 with bake 57), final 1600 px (clamped 1080..2048), zoom detail region on a 4096 px detail session after 200 ms idle, latest-wins in Dart (`_dirty`) | `app/lib/services/editor_engine.dart`, `adaptive_quality.dart`, `session_cache.dart` | defines the frame-request shapes the engine must serve |

Measured bottlenecks that shape the design (TR): on the low-end profile the UI-thread build (49 % of frames > 16.7 ms) and the engine both miss the targets; the engine part is the 41^3/57^3 colour bake (45 ms at 41^3), the tone analysis (40 ms exact, ~20x cheaper with the fast proxy path), the neighbourhood stages and the 8-bit pack. The GPU removes the per-pixel and neighbourhood costs; it does **not** remove the bake or the Dart/UI cost.

---

## 3. Architecture overview

```
 C API (dapple_c.h, unchanged)           Dart (unchanged until stage d)
        |
 EngineSession (per photo)  -- SourceStore, ResultCache, PlanCache, proxy, film leases
        |  build
 GraphBuilder(EditParams, FrameRequest, SessionCtx) --> Graph  (pure function, variants)
        |  plan
 Planner(Graph, ParamSet, caps, budget, cache state) --> Plan   (fusion, tiles, halos, aliasing, cache ops)
        |  run
 +------------------+            +-------------------+
 | CpuExecutor      |            | GpuExecutor       |  <- hal::Device (Vulkan | GLES 3.1 | Metal)
 | SIMD, ThreadPool |            | node shaders      |
 +------------------+            +-------------------+
        Node library: per node  NodeDesc + cpu impl + gpu impl(+shader) + tests
```

Source layout (new, additive; `core/gpu/` is migrated into it step by step, 10.3):

```
core/engine/
  graph/    types.h node.h params.h hash.h graph.h builder.cpp validate.cpp registry.cpp
  plan/     planner.cpp fusion.cpp tiling.cpp alloc.cpp cache.cpp   (backend independent)
  exec/     cpu_executor.cpp gpu_executor.cpp engine.cpp session.cpp
  nodes/    <name>/{desc.cpp, cpu.cpp, gpu.cpp, <name>.comp, test.cpp}
  hal/      hal.h  vk/  gles/  mtl/  null/
  shaders/  common.glsl ... ; tools/shaderc/ (host tool: SPIR-V -> ESSL/MSL + manifest)
```

Rules that keep the layers honest:

- Executors and nodes never see `EditParams`. They see a `ParamSet` snapshot and per-node *resolved* parameter blocks produced by the node's `resolve()`. This is what makes a node testable alone.
- Nodes never allocate output images; the planner owns every image (so aliasing, pooling and caching are possible).
- The planner never calls a kernel; the executor never decides what to compute.

---

## 4. Graph IR

### 4.1 Basic types

```cpp
namespace dapple::engine {

struct Hash128 { std::uint64_t a = 0, b = 0; };            // two independent 64-bit lanes (same scheme as film bakeKey)
using NodeTypeId = std::uint16_t;  using NodeId = std::uint32_t;  using ParamKey = std::uint16_t;

enum class Domain    : std::uint8_t { Image, Table, Npu /*reserved*/ };
enum class Footprint : std::uint8_t {
  Point,     // out(x,y) = f(in(x,y)); halo 0; fusable
  Window,    // out(x,y) depends on in within +-halo; fusable into tile groups
  Resample,  // out(x,y) = f(in(M(x,y))): arbitrary mapping (geometry, resize); bounding box computed by node
  Global,    // needs a whole-frame quantity (reduction/analysis); splits tile sweeps
  Source, Sink,
};
enum class Boundary  : std::uint8_t { Replicate, Reflect /*scipy 'reflect', film*/, Zero };
enum class Space     : std::uint8_t { LinearProPhoto, DisplaySRGB, Luma, Chroma, FilmExposure, FilmLogE,
                                      Inhibitor, FilmDensity, Mask, Table, Opaque };
enum class Precision : std::uint8_t { F32, F16Ok /*storage may be fp16 on GPU, node rounds explicitly*/, U8 };

struct Ratio { std::uint8_t num = 1, den = 1; };           // scale of a grid relative to the frame: 1, 1/2, 1/4 ...

struct ImageDesc {                                         // *logical* image: the planner picks the physical format
  Space space = Space::Opaque;
  std::uint8_t channels = 3;                               // 1..4 ; CPU may be packed (RGB) or planar (1 plane x N)
  enum Layout : std::uint8_t { Packed, Planar } layout = Packed;
  Precision precision = Precision::F32;
  Ratio scale;                                             // relative to the frame size of the graph
  int gridAlign = 1;                                       // tile origins/sizes must be multiples of this (phase 1: 4)
};
```

Physical mapping (planner, per executor): CPU `Packed` = interleaved float (`ImageF`), `Planar` = float planes (`spatial.cpp` `Plane`, film planes); GPU `channels 3|4` -> RGBA16F (`F16Ok`) or RGBA32F (`F32`), `channels 1` -> R32F, 2 -> RGBA32F or 2x R32F when the HAL says RG32F lacks the needed access (GLES, 6.4). A logical image never has a format the HAL cannot bind; the planner checks `FormatCaps`.

### 4.2 Node descriptor

```cpp
struct PortDesc { const char* name; Domain domain; ImageDesc image; /* or table kind */ bool optional = false; };

struct ParamMask { std::bitset<256> bits; ParamMask& add(ParamKey k); };   // which ParamKeys this node reads

struct NodeDesc {
  NodeTypeId  type;  const char* name;  std::uint16_t version;            // version enters the hash
  std::span<const PortDesc> inputs, outputs;
  Footprint   footprint;
  Boundary    boundary = Boundary::Replicate;
  ParamMask   deps;                                                       // ParamKeys + context keys (4.4)
  // Resolution of EditParams -> numbers the kernels need (radii in pixels at this node's scale, gains, geometry...).
  // Pure function of (ParamSet, FrameGeom). Result is POD, <= 256 bytes, std140-compatible (uploaded as the GPU uniform block).
  std::size_t resolvedBytes;
  void (*resolve)(const ParamSet&, const FrameGeom&, void* out);
  bool (*isIdentity)(const void* resolved);                               // true: node is elided from the graph
  // Neighbourhood, in pixels of the node's input grid; evaluated per instance (radii depend on parameters and scale).
  Halo (*halo)(const void* resolved);                                     // Point: always {0,0}
  // Footprint::Resample: input rect needed for an output rect (bounding box of the mapped quad + filter support)
  Rect (*inputRect)(const void* resolved, const Rect& outRect);
  Precision (*requiredPrecision)(const void* resolved, int outputPort);   // e.g. film exposure planes: F32
  CacheHint cache;                                                        // { never | keepIfCheap | keep, recomputeCostEstimate }
  std::span<const FusionRole> fusion;                                     // Point: may be a fragment of recipe(s); Window: prologue/epilogue slots
  ParityBudget parity;                                                    // 8.5; used by the generic parity test
  std::span<const TestCase> testCases;                                    // synthetic inputs + params for conformance/parity tests
};
struct Halo { int x = 0, y = 0; };                                       // symmetric; asymmetry is not needed so far
```

Implementation tables, one per executor, registered next to the descriptor:

```cpp
struct CpuImpl {
  // Whole-tile entry: views are windows (origin + stride) into planner-owned buffers; the node must produce `outRect`
  // from inputs that cover outRect+halo, applying `boundary` against the FRAME rect (ctx.frame), not the tile rect.
  Status (*run)(const CpuCtx&, const void* resolved, std::span<const ImageViewIn>, std::span<const ImageViewOut>);
  // Point nodes only: block fragment for fused streaming (n <= 256 float4 pixels, in place, ctx gives absolute x,y).
  void (*block)(const void* resolved, float* px /*AoS 4*/, int n, const PixelPos& pos);
};
struct GpuImpl {
  // Appends dispatches to the recorder; may use its own pipelines (names resolved through the shader manifest).
  Status (*record)(GpuRecorder&, const void* resolved, std::span<const GpuImageIn>, std::span<const GpuImageOut>);
  std::span<const RecipeMember> recipes;                                  // fused kernels this node can be a member of
  std::span<const char* const> shaders;                                   // for the self-test/precompile list
};
```

A node with `cpu == nullptr` or `gpu == nullptr` makes any plan containing it unsupported on that executor (checked at plan time, 5.9).

### 4.3 Graph and instances

```cpp
struct NodeInstance {
  NodeTypeId type;  NodeId id;
  std::vector<Edge> in;                    // (producer NodeId, output port)
  ResolvedBlock    resolved;               // filled by Graph::resolve(ParamSet, FrameGeom)
  Hash128          key;                    // content key (4.4)
};
class Graph {
 public:
  NodeId add(NodeTypeId, std::span<const Edge> inputs);
  void   markOutput(NodeId, OutputRole);                   // Frame (RGBA8 or float), Analysis (histogram JSON), ...
  Status validate(const Registry&) const;                  // 4.7
  Hash128 structureHash() const;                           // node types + wiring, NOT parameter values (plan cache key)
};
```

*Composite nodes.* A node type may `expand()` into a small subgraph during build: `Blur` with large-sigma mixture components expands into `Pyramid(build, Global)` + `BlurLow` + `BlurFine(Window)` + `Upsample`, so the planner only ever sees the five footprints above. Film DIR diffusion, halation and scatter all use this (8.2).

### 4.4 Parameter dependencies and hashing

`ParamKey` is an index into a registry built from `paramFields()` (`edit_params_fields.h`: one key per numeric slider) plus explicit keys for the non-scalar fields, grouped where they always travel together:

| Key family | Content |
|---|---|
| per slider | `exposure`, `contrast`, `highlights`, ..., `clarity`, `texture`, `dehaze`, `vignette`, `grain`, `sharpen`, `sharpRadius`, `nrLuma`, `nrColor`, `straighten`, film sliders `filmStrength`, `filmExposure`, `filmWarmth`, `filmTint`, `filmGrain`, `filmHalation`, ... |
| groups | `hsl[8][3]`, `grade*` zones, `curvePoints{master,R,G,B}`, `channelParametric`, `look+lookStrength+palette`, `toneTarget`, `crop`, `rotate`, `flip`, `filmStock`, `filmPaper`, `lutPath+lutAmount` |
| context keys | `ctx.source` (identity + stamp), `ctx.camera`, `ctx.lutIdentity`, `ctx.filmLibraryRevision`, `ctx.toneReach`, `ctx.frameSize`, `ctx.region`, `ctx.srcLongEdge`, `ctx.quality` (Off/Interactive/Full), `ctx.noise`, `ctx.bakeSize` |

```cpp
class ParamSet {                      // immutable snapshot of everything a build/plan/run may read
 public:
  static ParamSet make(const EditParams&, const FrameRequest&, const SessionCtx&);   // clamps, resolves defaults, hashes each key once
  float f(ParamKey) const;  std::int32_t i(ParamKey) const;  std::string_view s(ParamKey) const;
  Hash128 hash(const ParamMask&) const;      // O(#bits set): combines the per-key hashes (bit patterns, -0 -> +0)
};
// node content key
//   key = H(type, version, params.hash(desc.deps), key(input_0) ... key(input_n), geomKey if output depends on frame size)
```

Rules:

- A node declares the **smallest** mask that is true. Conditional dependencies are decided at graph-build time by wiring (e.g. `ToneTable` gets the `Analysis` edge only if `toneStrength > 0`, so an exposure drag does not touch the table's key; today `pointKey()/colourKey()` hand-code the same idea as JSON strings with `k.grain = 0` style masking).
- `geomKey` = `(frame w, h, region, step, scale, tile-invariant flag)`, included only for nodes whose output is spatial; table nodes omit it.
- Cache keys are 128-bit; collisions are treated as impossible (the film cache already relies on the same scheme, FA 4b "Cache keys").
- **Declaration conformance test (mandatory per node)**: for every `ParamKey` not in `deps`, perturb it within its legal range, rebuild the node's key and run the node's `testCases`; the output must be bit-identical. Catches the one dangerous bug class of content addressing: an undeclared dependency yields a *stale cache hit*.

### 4.5 EditParams -> graph (main pipeline, non-film)

Node names are the registry names. `P` = Point, `W` = Window, `G` = Global, `R` = Resample, `T` = Table.

| Node | Foot. | Reads (deps) | CPU impl (today) | GPU impl (phase 1) |
|---|---|---|---|---|
| `Source` | src | `ctx.source` | `SourceImage.linear` | resident RGBA16F (`convert.comp`) |
| `Resize` | R | `ctx.srcLongEdge`, filter | `resizeToLongEdge` (area average) | `resize.comp` (Lanczos-3), see Q2 |
| `Geometry` | R | `crop, straighten, rotate, flip, ctx.region, ctx.frameSize` | `Geometry::mapF` + `sampleCubic` | `point.comp` `GEO` const |
| `Gain` | P | `temperature, tint, exposure, ctx.camera` (+`filmExposure` in the film branch) | `v * wb * 2^EV` | fused |
| `ToneAnalysisPre` | T | `ctx.source` proxy, WB, exposure, tone group, camera | `analyzeTone` | CPU only |
| `ToneTable` | T | `contrast, highlights, shadows, whites, blacks, toneTarget, toneStrength, ctx.toneReach, ctx.camera` (+ pre-analysis if `toneStrength>0`) | `fillToneTable` (7682 floats, `kToneBase/kToneN`) | SSBO upload |
| `ColourLattice` | T | colour group (`hsl, grade*, saturation, vibrance, look*, curves, lut*, ctx.camera, ctx.lutIdentity`), `ctx.bakeSize` | `PointOp::colour` over n^3 (57^3 interactive, 65^3 final) | 3D RGBA32F upload; CPU only (Q4) |
| `LatticeOverflow` | T | same as lattice | `bakeOverflow` (33^3, [0,8]) | 3D upload |
| `ToneColour` | P | table inputs | `LutSampler` + overflow | fused in `point.comp` |
| `Vignette` | P | `vignette, crop` (centre follows the crop) | `applyVignette` | fused |
| `NRLuma` | W | `nrLuma, ctx.scale` | `denoiseLuma` | `sepconv` box x4 + `nr_luma` |
| `NRChroma` | W | `nrColor, ctx.scale` | `denoiseChroma` | `down`, box, `nr_chroma` |
| `Dehaze` | W + G | `dehaze, ctx.scale` | `dehazeImage` (1/4 grid, dark channel, airlight) | `down`, min/box, `reduce`, `dehaze_apply` |
| `Detail` | W + G | `clarity, texture, sharpen, sharpRadius, ctx.scale` | `guidedBase` + `detailPass` | `gf_ab`, box, `detail` |
| `Grain` | P | `grain, ctx.quality!=Interactive, ctx.scale` | `addGrain` (hash2 on frame coordinates) | epilogue of the last pass |
| `Pack8` | Sink | - | clamp + `*255+0.5` | epilogue / `out8` |
| `Analysis` | G (output) | demanded only when `FrameRequest.wantAnalysis` | `analyzeToneImage` | CPU on the 256 px proxy (async, Q9) |

Order is the existing order (`applySpatial`: NR luma, NR chroma, dehaze, detail, grain; `renderer.h` header comment). `Window + Global` nodes use composite expansion.

### 4.6 Graph variants (builder rules)

`GraphBuilder::build(const ParamSet&)` is a **pure function**; any state-dependent decision (e.g. `fo.noise` from `lastParams` in `c_api.cpp`) is computed by the C layer and passed in `FrameRequest`.

| Condition | Graph change |
|---|---|
| node `isIdentity(resolved)` (e.g. `nrLuma==0`, `clarity==texture==0 && sharpen==0`) | node elided (today: `sp.any()` and per-stage `if`s) |
| `ctx.quality == Off` | no spatial nodes |
| `Interactive` | no `Grain`; `NRLuma/NRChroma` only if `ctx.noise` (today `DAPPLE_FRAME_INTERACTIVE` + `fo.noise`); film `previewMode` |
| film off | main branch only |
| film on, `filmStrength == 100` | film branch only; main `ToneColour` and `ColourLattice` pruned (today "the lattice is only the blend partner"); `look = "std"` for style; film brings its grain |
| film on, `0 < strength < 100` | both branches, `DisplayStages` on each, `Blend(strength)` after, then `Vignette` |
| film on, `strength == 0` | main branch only |
| region / zoom detail | `Resample` gets the region rect and step; `Resize` target = `ceil(outLongEdge / frac * 1.15)` rounded to 32 (as `renderFrame` does); `ctx.scale` from the region (as `sp.scale`) |
| geometry identity / exact / cubic | `Resample` mode (GPU: `GEO` spec constant 0/1/2) |
| `DAPPLE_FRAME_EXACT` | `ToneColourExact` (CPU-only `PointOp::colour` per pixel) replaces lattice + overflow + tables; this is the reference variant used by parity tests |
| tile export | same graph, `Plan.mode = TiledExport` (planner decides sweeps), no graph change |
| `sceneReferred == false` (JPEG) | camera baseline EV/contrast/HSL params resolved to 0 in `resolve()` (as `makePointOp`) |

### 4.7 Validation

At graph build (every frame, O(nodes), must stay < 50 microseconds):

- DAG, every non-optional input connected, no dead outputs except demanded roles.
- Edge compatibility: producer `Space`/`channels`/`precision` against consumer port; scales are compatible (equal, or the consumer is a declared resampler such as `Upsample`).
- Every `ParamKey` in a mask exists in the registry; the node's `resolve()` output size equals `resolvedBytes`.
- Domain rules: `Table` outputs only feed `Image` nodes as bound tables; `Global` nodes have exactly one reduction input and declare their output grid.

At plan time: executor support (`impl != null`, caps in 6.4, resource budget) -> `Unsupported(reason)` (cached per `(structureHash, caps)`).

At test time (CI, per node): declaration conformance (4.4), **halo conformance** (change one input pixel outside `interior+halo` of a tile: interior output bit-identical), **tile invariance** (any tile decomposition equals whole-frame), **fusion invariance** (fused equals unfused chain within the node's budget), **parity** (8.5).

---

## 5. Executor contract

### 5.1 Planner output

```cpp
struct Plan {
  Hash128 structureHash;
  enum Mode { Whole, TiledExport } mode;
  std::vector<Stage> stages;            // topologically ordered
  TileGrid grid;                        // Whole: one tile; TiledExport: interior size, halo per group, alignment
  std::vector<Alloc> allocs;            // logical image id -> pool class + lifetime [firstStage, lastStage]
  std::vector<CacheOp> cache;           // lookup/store per node output, decided from keys + budget
  std::size_t peakCpuBytes = 0, peakGpuBytes = 0;
};
struct Stage { enum Kind { HostTable, TileSweep, GlobalSolve, Output } kind; std::vector<Group> groups; };
struct Group { std::vector<NodeId> nodes; RecipeId recipe /*GPU*/; Halo inHalo; Ratio scale; };
```

`Planner::plan()` steps, in order: (1) resolve params -> node blocks, (2) compute node keys, (3) consult `ResultCache` and prune producers whose outputs are cached or unused, (4) group nodes (fusion), (5) choose tile size and propagate halos, (6) lifetimes + aliasing, (7) decide what to store in the cache, (8) check support. The result is cached by `(structureHash, tile config)`; a slider drag changes only parameter values, so steady-state frames reuse the plan and only redo steps 2, 3 and 7 (target < 0.2 ms on the low-end profile, *estimate*).

### 5.2 Scheduling

- **CPU**: stages run in order; a `TileSweep` iterates tiles (one tile in `Whole` mode); inside a tile, groups run in order; `Table` stages run on the calling thread using `parallelFor` (the bake is parallel already). Parallelism = `ThreadPool` over tile rows or tiles; the pool is not re-entered (an inner `run` runs inline, existing behaviour); background requests set `sys::setBackground` and bypass the pool (existing).
- **GPU**: one frame = one command list per sweep. The phase-1 scheduler is kept in spirit: dependency layering (independent chains adjacent), one merged barrier per layer **only when a real RAW/WAR/WAW dependency exists**, pooled images released at last use. In the new design that logic lives in `GpuScheduler` on top of HAL handles (not in the Vulkan layer), and emits `BarrierMask` values (6.2) so that GLES gets the right `glMemoryBarrier` bits.
- Frames in flight: 3 frame slots as in phase 1 (command list + fence + param ring + staging + readback); table uploads for frame N+1 overlap GPU work of frame N.

### 5.3 Fusion rules

A *group* is a connected chain executed as one unit per tile.

1. Allowed members: `Point` and `Window` nodes; a `Resample` node only as the group head; `Global`/`Source`/`Sink` never inside.
2. No node except the last may have an output that is consumed outside the group, demanded as an output role, or selected for caching (it would have to be materialised); the planner splits there.
3. Same grid scale (no `Ratio` change inside a group; `Upsample/Down` are group boundaries).
4. **CPU**: any chain of `Point` nodes with `block` fragments streams 256-pixel blocks through L1 (this is what the film kernels do today: 5 kernels back to back over 256-pixel blocks, FA 1 "Streaming"; and what `renderBakedImpl` does per pixel). `Window` nodes inside a group run on the tile with per-tile halo accumulated backwards (film's "ONE fused tile pass 256x64 + halo" is exactly this).
5. **GPU**: fusion is **recipe based**, because shaders are compiled at build time. A recipe is a shader plus an ordered member list with spec-constant switches, e.g. `point` = `Geometry? + Gain + ToneColour + Vignette?` and an epilogue slot (`LAST`, `GRAIN`) that takes `Grain + Pack8`. The planner matches maximal chains against recipes; no match -> each node runs its own shader. Recipes are an optimisation; every node must also have a standalone shader so correctness never depends on a recipe.
6. Numerical neutrality: reference semantics are *fp32 at node boundaries*; fused GPU kernels keep fp32 in registers, unfused ones round to the storage format (fp16 with the explicit `h16()` round-to-nearest-even, GB 10.5). The fusion-invariance test bounds the difference.
7. Fusion never changes the cache decision: a node whose output is cacheable and consumed by a *different frame* breaks the group.

### 5.4 Tiling and halo propagation

- `Whole` mode (interactive, final preview, zoom regions): one tile = the output frame, no halo bookkeeping beyond boundary policy.
- `TiledExport`: the planner picks `tile` (default 1024 on phones, 2048 on desktop; GB 10.7 measured 255 MB at 512, 318 MB at 1024, 565 MB at 2048 for 60 MP) as the largest value for which `peak <= budget`, subject to `maxImageDim` and the grid alignment of the plan.
- **Halo propagation**: walking each tile's group backwards, `in = out + halo(node)`; at `Resample` nodes the rect is mapped through `inputRect()`; the total is the max over branches. Halos are in pixels of the node's own grid and are rounded **up to the plan's `gridAlign`** (phase 1: 4, because dehaze/clarity use a 1/4 grid and tile origins must coincide with grid cells; film pyramids and half-resolution fields make it `lcm` of all used denominators, still small because large scales are handled by global grids, next point).
- **Large scales are not tiled by halo**: any node whose support would exceed a tile (dehaze, clarity base, film halation/DIR tails of 100 px to thousands of pixels) is a composite with a `Global` part. The planner turns it into a *sweep*: tiles of the upstream chain accumulate into a whole-frame low-resolution grid (<= 1/4), the grid is solved once (`GlobalSolve`), later sweeps consume it (bilinear/Catmull-Rom upsample). This is exactly phase 1's 3-sweep export (GB 10.7), generalised.
- Sweeps recompute the upstream tile chain. Cost model: if `bytes(full frame of the group input) <= budget`, **store** instead of recompute (CPU whole-frame path today does this implicitly); GPU recomputes (cheap: 1.4 ms point+NR for 26 MP, GB 10.8). On CPU, recomputation of an expensive chain is undesirable, so the CPU planner prefers storing and, when over budget, accepts the recompute.
- Boundary policy is applied by the node against the **frame** rect (`ctx.frame`), never the tile rect; consequently tile results equal whole-frame results.

### 5.5 Tile-invariance and determinism

- Contract: output is a function of (inputs, resolved params) only; not of tile size, thread count or fusion grouping (fusion within the budget in 5.3.6).
- GPU: bit-exact tile invariance was achieved in phase 1 (0 differing pixels for 60.2 MP, tile 128..2048, GB 10.7). Preserved by: absolute coordinates for grain (`hash2` on picture coordinates), alignment of working origins to the global grid, direct-sum filters in shared memory.
- CPU film kernels: bit-identical for any tile decomposition and thread count (FA 5b, test `film.robust_render`).
- **CPU spatial exception (documented, not hidden)**: `boxH/boxV` use a running sum (`sum += s[x+r+1] - s[x-r]`), whose floating-point rounding depends on where the sum starts. Whole-frame results (today's) stay bit-identical in step (a); a tiled CPU run can differ in the last bit of the float plane and hence by at most 1 code in a small fraction of pixels. Gate for tiled CPU: mean dE2000 < 0.01 and max abs <= 1/255 versus whole-frame, not bit-identity. Option if bit-identity is wanted later: block-aligned prefix sums anchored at absolute multiples of 64 (costs a few percent).
- Across backends/ISAs (AVX2 vs NEON, Vulkan vs GLES vs CPU): not bit-identical; bounded by per-node ΔE budgets (8.5).
- Randomness (grain, glare): stateless hash keyed by (seed, absolute x, y, layer) (`pcg3d`, FA 2 "Grain"); no sequential RNG anywhere.

### 5.6 Memory: pooling, aliasing, source residency

- Planner assigns each logical image a lifetime in stage order and a pool class `(physical format, size bucket)`; non-overlapping lifetimes of the same class share one physical image. This is phase 1's pool (`(format, w, h)` rounded up to 32 / 128 above 1024) lifted into the planner so that the CPU executor aliases float planes the same way (today `ScratchImage`, `arena()` and film `Arena` do it ad hoc).
- Budgets: `MemoryBudget{cpu, gpu, unifiedMemory}` from the OS (Dart passes `ActivityManager.getMemoryInfo` at start; native reads `/proc/meminfo`) and `DeviceCaps`. On unified-memory SoCs CPU and GPU budgets are one number. `Plan.peak*` is checked against it; the tile size is the free variable.
- `SourceStore` (new, Q3): `Source` reads through an interface so the same graph runs on a resident source or a streamed one.

```cpp
class SourceStore {
 public:
  virtual Extent size() const = 0;
  virtual Status read(const Rect&, ReadTarget& dst) = 0;   // float RGB (CPU), or fp16 RGBA staging (GPU upload)
  virtual Residency residency() const = 0;                 // InMemory | Mapped | GpuResident
};
```
Implementations: `InMemoryFloat` (today's `SourceImage`, wraps it, no copy), `InMemoryHalf` (RGBA16F, 8 B/px, what the GPU wants), `MappedHalf` (decode once to a tiled linear fp16 file in the app cache, `mmap` pages: file-backed clean pages are reclaimable, so a 60 MP source does not count as anonymous memory; *estimate* of effect on Android LMK, to be measured). Decoders (LibRaw full-frame demosaic, 8 B/px `ushort[4]` plus raw) are a separate memory peak that this doc does not remove.

### 5.7 Caching and invalidation

Content addressing (4.4) means **there is no invalidation API**: a changed parameter produces new keys; stale entries age out.

```cpp
class ResultCache {                    // per EngineSession and per memory domain (host, device)
 public:
  Entry* find(const Hash128&);  void put(const Hash128&, Entry&&, float costMs);
  void setBudget(std::size_t bytes);  void trim(std::size_t keep);   // background / low memory
};  // eviction: lowest (recomputeCostMs / bytes) first, then LRU
```

What is cached (decided by the planner, `NodeDesc::cache` is only a hint):

| Output | Tier | Why |
|---|---|---|
| all `Table` outputs (lattice, overflow lattice, tone table, film tables) | all | small, expensive on CPU (41^3 = 45 ms low-end, TR) |
| `Resize` output (scaled copy) | Whole | replaces `DappleSession::scaled[]` (today 2 GPU / N CPU entries, LRU by bytes) |
| `Global` grids (dehaze airlight/dark channel, clarity `ab`, film pyramid levels) | all | whole-frame low-res, small |
| film `E2` and `Q` planes | Whole, preview size only | 3+3 planes x 4 B x 1600x1067 = 20 MB; avoids exposure/halation/DIR for grain/print drags (8.4) |
| `Frame` outputs | - | not cached by the engine (Dart keeps its own `image`/`original`) |
| tile interiors | never | export streams |

Interactive-tier budget default: 64 MB host + 64 MB device (*estimate*, tune on the 3 GB class where PSS is 393 MB steady, TR v3).

#### Worked example 1: exposure slider drag, 1080 px interactive, non-film, defaults (sharpen 40, nrColor 25)

Dependencies that change: `exposure` is in `Gain.deps` and in `ToneAnalysisPre`/`Analysis.deps` (the histogram), not in `ToneTable.deps` when `toneStrength == 0`, not in `ColourLattice.deps`.

| Node | Key changes? | Action |
|---|---|---|
| `Source`, `Resize(1080)` | no | cache hit (no resample of the 2048 px source) |
| `ColourLattice`, `LatticeOverflow` | no (this is the `colourKey` property: lattice reused "whatever the lattice size", TR) | hit; zero colour evaluations |
| `ToneTable` | no (`toneStrength==0`) / yes (`>0`: new pre-analysis edge) | hit / rebuild 7682 floats |
| `Gain` -> `ToneColour` -> `Vignette` group | yes (gain in params) | **recompute**: GPU: one dispatch, `gain` is a uniform, no re-upload of tables; CPU: one fused pass |
| `NRChroma`, `Detail(sharpen)` | yes (their input changed) | recompute (NRLuma/dehaze/clarity elided: zero amount) |
| `Pack8` | yes | epilogue of the last pass |
| `Analysis` (UI histogram) | yes | demanded only with `wantAnalysis`; runs on the 256 px proxy off the critical path |

CPU critical path on GPU = table hits + uniform update + record/submit; the only CPU work that can remain is the analysis when `toneStrength > 0` (today "tone table + 256 px proxy through the lattice", ~20x cheaper than exact, TR).

#### Worked example 2: film grain slider (final tier 1600 px; interactive tier has `previewMode` = no grain, FA 7)

`filmGrain` is in `FilmGrain.deps` only (plus `Pack`-side nodes downstream by key). Hits: `Source`, `Resize`, the three film tables, `FilmExposure`, `FilmMixtureExposure` (halation+scatter) -> cached `E2`, `FilmDevelopQ`, `FilmDirDiffuse` -> cached `Q`. Recomputed: `FilmDevelop`, `FilmGrain`, `FilmPrintScan`, `FilmGlare`, `FilmScan`, display stages, vignette, detail, pack. Measured CPU split at 1600 px, all effects, cached bake: exposure 2.5, halation 4.5, DIR 5.6, tile pass 6.9 ms of ~18.6 ms total (FA 2 "Measured"): the cache saves ~12.6 ms (68 %) of the CPU path for a grain drag; on the GPU the saving is smaller in absolute terms (*estimate*). At 26 MP export nothing is cached (planes are 36 B/px, FA 2 "Memory"): grain is simply part of the single sweep.

#### Worked example 3: crop change (e.g. `orig` -> `4:5`)

`crop` is in `Geometry.deps`, `Vignette.deps` (centre follows the crop) and sets `ctx.frameSize`. Key changes: `Geometry` and everything downstream (all spatial and pixel nodes, `Global` grids because the frame changed). Hits: `Source`, `Resize` (its target is the long edge of the *request*, not of the crop), `ColourLattice`, `ToneTable`, all film tables, and the tonal analysis (computed on the uncropped proxy; `pointKey()` resets `crop = "orig"` for the same reason). So a crop change costs one pass over the new frame size, no bake: the property today achieved by string masking in `pointKey()`.

### 5.8 Cancellation (latest-wins)

```cpp
struct CancelToken { const std::atomic<std::uint64_t>* current; std::uint64_t mine;
                     bool cancelled() const { return current->load(std::memory_order_relaxed) != mine; } };
```
- Every request carries a ticket (`++session.latest` at submit time). The executors poll at: CPU - between tiles, between groups, and in the `ThreadPool` chunk loop (the pool already abandons remaining chunks on exception, `parallel.h` `work()`; a `cancelled()` check there is a tiny change made in step (a)); GPU - before recording each sweep/tile and before each submission.
- A submitted Vulkan/GLES command list cannot be aborted. Interactive frames (1.5-6 ms) are never cancelled mid-flight. Final/export work is split into one submission per sweep or per tile (>= tens of ms each at most) so a new input preempts within one submission.
- Cancelled runs return `Status::Cancelled`; cache stores made before the cancel stay valid (they are content-addressed); transient allocations are released by the planner's lifetime table.
- C API: the current synchronous `dapple_session_render_frame` keeps its semantics (Dart coalesces edits itself, `_dirty`/`_kick`). Additive: `dapple_session_cancel(DappleSession*)` bumps the ticket so a long final/detail/export call returns early with `DAPPLE_ERROR` and `dapple_last_error() == "cancelled"`.
- Stage (d) render loop: a one-slot mailbox (atomic exchange of the newest `ParamSnapshot`) read once per vsync; no new snapshot and no invalidated source = nothing is submitted.

### 5.9 Errors and fallback

| Class | Detected | Behaviour |
|---|---|---|
| `Unsupported` (node without impl, caps, over budget, image larger than `maxImageDim`, film node not yet ported) | plan time | whole frame on the CPU executor; result cached per `(structureHash, caps)`; counted in `dapple_gpu_status_json().fallbacks` |
| `Transient` (OOM, timeout, surface lost, shader link failure of a lazily compiled variant) | HAL `Status` | release pools (`trimPool`), retry once on the GPU; else this frame on the CPU, session stays on GPU; a variant that failed to link is blacklisted for the process and its graphs become `Unsupported` |
| `DeviceLost` | HAL | rebuild the device and re-upload (resident data is reconstructible from `SourceStore` and tables); second loss within the session: sticky CPU until restart. Today it is sticky CPU immediately (GB 10.9) |
| Wrong results (bad driver) | startup self-test (6.6); debug builds: shadow-compare 1 frame in 100 against the CPU oracle | blocklist (device key) -> CPU |
| CPU failure (OOM) | `std::bad_alloc` caught in `guarded()` | `DAPPLE_ERROR` + message, as today |

Per-node mixed execution (GPU nodes with a CPU node in the middle, readback and re-upload) is deferred: it only matters while ports are incomplete, and the readback cost on discrete GPUs is large (15-18 ms for 26 MP, GB 10.8); on unified memory it is cheap but unmeasured (Q1). Until the film nodes have GPU impls (step b4) a film frame is simply `Unsupported` -> CPU, exactly as the phase-1 hook behaves (`FramePlan::supported = false`).

Visual consistency on a GPU<->CPU switch mid-session is bounded by the parity budgets plus the Resize filter decision (Q2); with the current filters the switch changes high-frequency content by ~0.7-1.2 dE (GB 10.6).

---

## 6. HAL

### 6.1 Scope

Compute only. Nothing below it (no Dawn/wgpu, no vendor SDKs). Optional extensions (surface presentation, external memory) are separate interfaces discovered through `caps()`.

### 6.2 Interface

```cpp
namespace dapple::hal {

enum class Api    : std::uint8_t { Vulkan, Gles31, Metal, Null };
enum class Status : std::uint8_t { Ok, OutOfMemory, DeviceLost, Unsupported, Timeout, InvalidArgument, Internal };
enum class Format : std::uint8_t { R8, RGBA8, R16F, RGBA16F, R32F, RG32F, RGBA32F, R32UI };
enum class Access : std::uint8_t { Read, Write, ReadWrite };
enum class MemKind: std::uint8_t { DeviceLocal, Upload, Readback };
enum class Samp   : std::uint8_t { Nearest, Linear };            // clamp-to-edge; baked into the pipeline layout (Vulkan immutable sampler, GL sampler object, Metal sampler state)

struct FormatCaps { bool sampled, filterable, storageRead, storageWrite, storageReadWrite; };

struct DeviceCaps {
  Api api;  std::uint32_t apiVersion, vendorId, deviceId;  std::string name, driver;  bool software;       // software = SwiftShader/llvmpipe/ANGLE-sw
  std::uint32_t maxImage2D, maxImage3D, maxWorkgroupInvocations, maxWorkgroupSize[3], maxSharedBytes;
  std::uint32_t maxStorageImages, maxStorageBuffers, maxSampledImages, maxUniformBytes, uniformOffsetAlign;
  std::uint64_t maxBufferBytes, deviceLocalBytes;  bool unifiedMemory;
  FormatCaps format[kFormatCount];
  bool timestamps, specConstants /*false on GLES: emulated by #define*/, programBinaryCache, asyncCompile;
  bool fp16Arithmetic, subgroups;                       // informational only: no kernel may depend on them
  bool externalMemoryImport, surfacePresent;            // extension interfaces available
  std::string key() const;                              // vendor:device:driver:api (blocklist / self-test cache key)
};

struct BufferDesc   { std::uint64_t bytes; MemKind kind; bool storage; };
struct ImageDesc    { Format fmt; std::uint32_t w, h, d = 1; bool sampled, storage, copySrc, copyDst; };
struct SpecConst    { std::uint32_t id; std::int32_t value; };
struct BindingDecl  { enum Kind { UniformBlock, StorageBuffer, StorageImage, SampledImage } kind; Access access; Format fmt; Samp samp; };
struct PipelineDesc { std::string shader; std::vector<SpecConst> spec; std::vector<BindingDecl> bindings /*checked vs manifest*/; };

enum Barrier : std::uint32_t {                 // consumer-oriented, derived by the scheduler from RAW/WAR/WAW analysis
  kImgStoreToImgLoad = 1, kImgStoreToSampled = 2, kBufWriteToBufRead = 4, kComputeToTransfer = 8,
  kTransferToCompute = 16, kComputeToHost = 32 };

class CommandList {
 public:
  virtual ~CommandList();
  virtual void bindPipeline(PipelineH) = 0;
  virtual void bind(std::uint32_t slot, BufferH) = 0;               // slots are per BindingDecl order; HAL maps to API binding spaces
  virtual void bind(std::uint32_t slot, ImageH) = 0;
  virtual void setParams(const void* std140, std::size_t bytes) = 0; // <= 256 B per dispatch; ring-allocated
  virtual void dispatch(std::uint32_t gx, std::uint32_t gy, std::uint32_t gz) = 0;
  virtual void barrier(std::uint32_t mask) = 0;
  virtual void copyBufferToImage(BufferH, ImageH, Rect, std::uint64_t srcOffset) = 0;
  virtual void copyImageToBuffer(ImageH, BufferH, Rect, std::uint64_t dstOffset) = 0;
  virtual void copyImage(ImageH, ImageH, Rect) = 0;
  virtual void timestamp(const char* label) = 0;                     // no-op if !caps.timestamps
};

class Device {
 public:
  virtual ~Device();
  virtual const DeviceCaps& caps() const = 0;
  virtual Status createBuffer(const BufferDesc&, BufferH*) = 0;
  virtual Status createImage(const ImageDesc&, ImageH*) = 0;
  virtual Status createPipeline(const PipelineDesc&, PipelineH*) = 0;   // thread-safe; may compile/link (blocking)
  virtual void   destroy(BufferH) = 0;  virtual void destroy(ImageH) = 0;  virtual void destroy(PipelineH) = 0;
  virtual void*  map(BufferH) = 0;                                      // Upload / Readback buffers; persistent
  virtual void   flush(BufferH, std::uint64_t off, std::uint64_t n) = 0; virtual void invalidate(BufferH, std::uint64_t off, std::uint64_t n) = 0;
  virtual std::unique_ptr<CommandList> begin() = 0;
  virtual Status submit(CommandList&, FenceH* signal) = 0;
  virtual Status wait(FenceH, std::uint64_t timeoutNs) = 0;  virtual bool poll(FenceH) = 0;
  virtual DeviceState state() const = 0;                                 // Ok | Lost
  virtual std::vector<std::uint8_t> pipelineCacheBlob() const = 0;       // Vulkan VkPipelineCache / GL program binaries / Metal archive
  virtual void   trim() = 0;                                             // release caches (background, low memory)
};
std::unique_ptr<Device> createDevice(Api, const DeviceOptions&, std::string& err);

// optional extensions (query through Device::ext<T>())
class ExternalMemoryExt { public: virtual Status importImage(const ExternalImageDesc&, ImageH*) = 0; };   // AHardwareBuffer / IOSurface
class PresentExt {                                                                                       // swapchain / EGLSurface / CAMetalLayer
 public:
  virtual Status attach(NativeWindow*) = 0;  virtual Status acquire(ImageH* target, FenceH* ready) = 0;
  virtual Status present(ImageH, FenceH done) = 0;  virtual void detach() = 0;
};
}  // namespace dapple::hal
```

Design choices:

- **Binding by slot, declared per pipeline.** The HAL's binding budget is the intersection of the backends: 1 uniform block + 4 storage buffers + 4 storage images + 8 sampled images per dispatch (ES 3.1 minimums, 6.4). Phase-1 kernels fit (largest: `point` = 3 storage images, 1 SSBO, 2 samplers; `detail` = 2 images, 3 samplers).
- **Parameters** are one UBO block <= 256 B (phase 1: ring of 512 KB per frame slot, dynamic offset; GLES `glBindBufferRange` on a ring; Metal `setBytes`).
- **Barriers are consumer-oriented masks** computed by the shared scheduler. Vulkan: images stay in `GENERAL`, one global memory barrier (what phase 1 does). GLES: mask -> `glMemoryBarrier(SHADER_IMAGE_ACCESS_BARRIER_BIT | TEXTURE_FETCH_BARRIER_BIT | SHADER_STORAGE_BARRIER_BIT | BUFFER_UPDATE_BARRIER_BIT | CLIENT_MAPPED_BUFFER_BARRIER_BIT)`. Metal: serial dispatch type or `memoryBarrier`.
- **No explicit queues/semaphores**: one queue; `submit` + `FenceH`. Cross-API sync with Flutter's consumer is handled inside `PresentExt` (stage d).
- **Readback**: copy to a `Readback` buffer, `wait(fence)`, `invalidate`, read through `map()`. On unified-memory devices this is the cheap path; zero-copy replaces it in stage (d).
- **No exceptions across the HAL**; every call returns `Status`; `DeviceLost` is sticky until the device is destroyed.

### 6.3 Backend mapping

| HAL concept | Vulkan 1.0 (primary) | GLES 3.1 | Metal (later) |
|---|---|---|---|
| Device | instance + physical device pick + blocklist; compute queue family (prefer one without graphics) | EGL display + a **surfaceless** context (`EGL_KHR_surfaceless_context`) on a dedicated render thread; shared context for async compile/upload | `MTLDevice`, one `MTLCommandQueue` |
| Buffer | `VkBuffer` + memory (device-local / host-visible / host-cached) | `glBufferStorage` is not in 3.1: `glBufferData` (SSBO/UBO) + `glMapBufferRange` (unsynchronised + flush explicit) | `MTLBuffer` shared/private |
| Image | `VkImage` (storage + sampled + transfer), GENERAL layout | `glTexStorage2D/3D` (immutable, required for image units) | `MTLTexture` (`shaderRead|shaderWrite`) |
| Sampler | immutable samplers in the layout | sampler objects bound to texture units | sampler state |
| Pipeline | `VkPipeline` from SPIR-V + `VkSpecializationInfo`; `VkPipelineCache` persisted | `glCreateShaderProgramv`/link from ESSL with `#define SPIRV_CROSS_CONSTANT_ID_n` prefix; `glGetProgramBinary` cache | `MTLComputePipelineState` from MSL with function constants |
| Binding | descriptor set per `(pipeline, resource ids)`, cached (phase 1: zero allocations in steady state) | `glBindBufferBase/Range`, `glBindImageTexture`, `glActiveTexture+glBindTexture+glBindSampler`, per-class unit numbers assigned by the remap tool | `setBuffer/setTexture/setSamplerState` |
| Params | dynamic-offset UBO ring | `glBindBufferRange` on a UBO ring (offset alignment from caps) | `setBytes` |
| dispatch | `vkCmdDispatch` | `glDispatchCompute` | `dispatchThreadgroups` |
| Barrier | one merged `vkCmdPipelineBarrier` memory barrier | `glMemoryBarrier(bits)` | encoder dependency / `memoryBarrier` |
| Fence | `VkFence` | `glFenceSync` + `glClientWaitSync` (flush first) | `MTLCommandBuffer` completion |
| Readback | `HOST_CACHED` buffer + `vkCmdCopyImageToBuffer` | copy via SSBO written by a tiny compute pass (image -> buffer) or `glReadPixels` from an FBO; then `glMapBufferRange` | blit to shared buffer |
| Timestamps | `VkQueryPool` | `EXT_disjoint_timer_query` if present | `MTLCounterSampleBuffer` (optional) |
| Device lost | `VK_ERROR_DEVICE_LOST` | `glGetGraphicsResetStatus` (needs `KHR_robustness`) or `GL_OUT_OF_MEMORY` / context loss events | command buffer error |
| External image | `VK_ANDROID_external_memory_android_hardware_buffer` (+ KHR external memory, dedicated allocation, `queue_family_foreign`; device ext set not guaranteed on 1.0 devices) | `eglGetNativeClientBufferANDROID` -> `EGLImage` -> `glEGLImageTargetTexStorageEXT` (needs `GL_EXT_EGL_image_storage` for `imageStore`); otherwise sampled-only and a final blit/draw into the target is needed | `IOSurface`-backed `MTLTexture` |
| Present | `VK_KHR_android_surface` swapchain on the `SurfaceProducer` Surface, or AHB ring | EGL window surface + `eglSwapBuffers` after a compute->framebuffer blit (the one non-compute operation, confined to `PresentExt`) | `CAMetalLayer` |

### 6.4 GLES 3.1 limitations that constrain the design

Guarantees quoted are the ES 3.1 minimum values (Khronos ES 3.1 spec, implementation-dependent-limits table; **re-verify against the spec revision at implementation time**). Real devices exceed most of them, so the policy is: *kernels are written to the minimum or to a queried value, never to what the dev GPU offers*.

| Area | ES 3.1 guarantee / fact | Consequence for the design | Where it hurts today |
|---|---|---|---|
| Storage image formats | only `rgba32f, rgba16f, r32f, rgba8, rgba8_snorm, rgba32i/16i/8i, r32i, rgba32ui/16ui/8ui, r32ui`; no `rg32f`, `r16f`, `rg16f` | `Format` set in the HAL is limited to those that are valid storage formats; 2-channel planes become RGBA32F or two R32F | `R16F/RG*` not used by phase-1 shaders; fine |
| Read-write images | an image bound without `readonly`/`writeonly` is only allowed for `r32f/r32i/r32ui` | every binding declares `Access`; the planner inserts **ping-pong** (the pool makes the second image free) or reads through a sampled texture; `Access::ReadWrite` on any other format is `Unsupported` | `nr_luma`, `nr_chroma`, `dehaze_apply`, `detail` declare `rgba16f uniform image2D uD` read-write: **must change** (read `uD` from a sampled binding, write another image) |
| Workgroup size | `MAX_COMPUTE_WORK_GROUP_INVOCATIONS` >= 128; sizes (128,128,64) | local size comes from spec constants with a default of 8x8 (64); the HAL picks 16x16 when `maxWorkgroupInvocations >= 256` | all phase-1 shaders use 16x16 = 256 and `reduce` 256 threads; Vulkan's own minimum is also 128, so this is a latent issue even on Vulkan |
| Shared memory | `MAX_COMPUTE_SHARED_MEMORY_SIZE` >= 16 KB | sepconv staging `vec4 sh[PERP][ALONG + 2*RMAX]` is sized at manifest time; the planner caps the radius per dispatch (larger radii use the pyramid/global path) and the manifest records `sharedBytes` | `sepconv` |
| fp16 | no fp16 arithmetic or 16-bit storage guarantee; `mediump` may be fp16 or fp32 at the driver's discretion | all arithmetic `highp` (explicit precision qualifiers emitted by the shaderc tool, 7.1); fp16 only as image storage with the existing explicit round-to-nearest-even (`h16()`); the rounding mode of `imageStore` to `rgba16f` is unspecified (NVIDIA truncates, GB 10.5), so the explicit rounding is mandatory on GLES too | already handled in phase 1 |
| Binding limits | per class: images >= 4, SSBO >= 4, UBO >= 12, texture units >= 16 (compute) | Vulkan binding numbers are one flat space; the remap tool renumbers **per class** (e.g. `point.comp` image at Vulkan binding 5/6 would exceed 4 image units) | `point` (3 images), `detail`; all <= 4 per class |
| Spec constants | none in ES 3.1 | SPIRV-Cross emits `SPIRV_CROSS_CONSTANT_ID_n` macros; the HAL prepends `#define`s and compiles one program per variant; variants = programs = compile+link time | 14 Vulkan variants today; ~20-30 with film (*estimate*) |
| Compile cost | driver compiles ESSL at runtime (tens of ms per program, *estimate*) | persist `glGetProgramBinary` blobs keyed by (driver, app shader hash); compile on the shared-context worker; self-test compiles only what it uses; other variants compile lazily on first use, off the render thread when possible | cold start |
| Texture size | `MAX_TEXTURE_SIZE` >= 2048 (3D: 256); phones typically 4096-16384 | tile+halo and resident-source windows clamp to `caps.maxImage2D`; sources larger than that are tiled-resident | export of 60 MP is tiled anyway; resident 2048 px sessions fit |
| Float filtering | linear filtering of 32-bit float textures needs `OES_texture_float_linear`; fp16 filterable | never rely on hardware filtering of F32 (the lattice already uses `texelFetch` + hand tetrahedral; film raw LUT uses 16 `texelFetch`) | none |
| Barriers | `glMemoryBarrier` by class only; no layouts, no per-resource barriers; `glMemoryBarrierByRegion` is fragment-only | scheduler emits class masks (6.2) | ok |
| Atomics | integer atomics on buffers/`r32ui` images; no float atomics | the reductions are already atomic-free | ok |
| Out-of-bounds | `imageLoad/Store` outside the image is undefined unless `KHR_robust_buffer_access_behavior` | kernels clamp or guard explicitly (phase 1 does) | ok |
| Threads/contexts | a context is current on one thread; no async compute queue | one render thread owns the context; a shared context for compile/upload; no overlap of compute with compute | upload/compute overlap is lost (small) |
| External image write | image store into an `EGLImage`-backed texture needs `GL_EXT_EGL_image_storage` | else compute writes a normal texture and a blit/draw copies it into the target | stage (d) |
| Timer queries, fp16 storage buffers, subgroups | optional / absent | profile only when present; never required | ok |

### 6.5 Capability-driven planning

The planner takes `DeviceCaps` as input: format availability and RW rules (ping-pong), workgroup default, shared-memory radius cap, max image size (tile clamp), binding counts. A plan that violates caps is `Unsupported` for that device, not a runtime error. Plan cache key = `(structureHash, caps.key(), shader hash)`.

### 6.6 Self-test and blocklist policy

| | Vulkan | GLES 3.1 |
|---|---|---|
| Pre-checks | loader found, compute queue, `maxImage2D >= 4096`, storage+sampled for `RGBA16F/RGBA32F/R32F/RGBA8`, `maxPerStageStorageImages >= 4`, shared >= 16 KB (phase 1, GB 10.2) | `GL_VERSION >= ES 3.1`, `EGL_KHR_surfaceless_context` or pbuffer fallback, valid storage formats (6.4) including the ones the shaders use, `maxWorkgroupInvocations`, `MAX_COMPUTE_IMAGE_UNIFORMS >= 4`, SSBO >= 4 |
| Test | 192x128 synthetic image, two cases through the **whole GPU graph** (point+geometry+vignette; all spatial + grain) against the CPU baked path: mean <= 1.0, p99 <= 5, max <= 25 (deliberately loose: catches zeros, NaN, wrong indices, not accuracy; GB 10.9) | same cases **plus** one tiny case per shader family that compiles (GLES drivers fail per program): every family is built and run once, in the background, results cached |
| Failure | `Backend` rejected, device key persisted (`vendor:device:driver`) with the app's shader hash; retried when either changes | same; additionally a program that fails to link marks only its node `Unsupported` |
| Blocklist | text rules (`vendor, name, driver substring, driver version range, reason`), software ICDs rejected by default (SwiftShader/llvmpipe; allowed with `allowSoftware` for tests/emulator); rules can be shipped as data | same format (`GL_RENDERER`, `GL_VERSION`), ANGLE-on-software rejected by default |
| Watchdog | pipeline creation and first frame on a worker thread with a 2 s timeout; timeout = reject (Mali/PowerVR driver hangs are in the risk list) | same |
| Persisted | `VkPipelineCache` blob in the cache dir (cold start) | program-binary blobs |
| Run | once per `(device key, shader hash)`; warm start does not re-run, only a 1-case smoke test | same |
| Dev override | `DAPPLE_GPU=0/1`, `dapple_gpu_set_enabled(mode)` | `DAPPLE_GPU_API=vulkan|gles` to force a backend |

Selection order: Vulkan if it passes; else GLES 3.1 if it passes; else CPU. Both are tried at most once per process, in the background; the first frames render on the CPU path until the self-test returns (the app already shows the developed placeholder first, TR).

---

## 7. Shader toolchain

### 7.1 Build steps (all at build time)

1. **Source**: GLSL 450 compute, `core/engine/shaders/*.comp` + `common.glsl`, written to the *portable subset*: no subgroup ops, no `float16_t`/8/16-bit storage, no push constants (one UBO), one descriptor set, binding numbers unique and below the HAL budget, image formats from the ES 3.1 list, RW only on `r32*`, workgroup size and spec-constant toggles via `constant_id` (`local_size_x_id/y_id`), explicit `highp`, explicit rounding on every fp16 / 8-bit store, no sentinel `mix()` (GB 10.5), no `texture()` on F32.
2. **glslc** (NDK `shader-tools`, already used): `glslc -O --target-env=vulkan1.0 -fshader-stage=compute -I shaders -D<variant defines> x.comp -o x.spv`; then `spirv-val`.
3. **`dapple_shaderc`** (host tool, ~300 lines C++ over the SPIRV-Cross library; built by CMake for the build machine, never shipped): reads each `.spv` and produces
   - a **manifest** (JSON, embedded): binding table by class and access/format, spec-constant ids/defaults, workgroup size (or its spec ids), `sharedBytes`, used storage formats;
   - **ESSL 310** (`CompilerGLSL`, `es = true`, `version = 310`, `vulkan_semantics = false`, default float precision `highp`, `enable_420pack_extension = false` since ES 3.1 supports `layout(binding)` natively, emit `SPIRV_CROSS_CONSTANT_ID_n` macros for spec constants and for `local_size_*_id`, `build_combined_image_samplers()` first, bindings renumbered **per class** with `set_decoration(DecorationBinding)` from the manifest);
   - **MSL** (`CompilerMSL`, iOS platform, `msl_version` of the oldest supported iOS, spec constants as `[[function_constant(n)]]`, `enable_decoration_binding = true`, argument buffers off at first). Not built until stage Metal.
4. **Lint of the generated ESSL**: compile each ESSL with `glslc -std=310es -fshader-stage=compute` (glslang enforces ES rules such as the read-write image format restriction; *verify the exact diagnostics once*). This is the only way to catch ES-rule violations on a developer machine whose GL driver is permissive.
5. **Embed**: `shader_table.inc` with, per variant, SPIR-V bytes, ESSL string, (MSL string), manifest. A shader-set hash feeds the pipeline-cache and self-test keys.

SPIRV-Cross is not part of the NDK shader tools (they contain glslc, spirv-as/cfg/dis/link/opt/reduce/val). Per the toolchain rule it is vendored under `tools/_dl/` at a pinned commit (Apache-2.0) and built only as part of the host tool. Binary-size cost on the device: zero code, only the extra strings (*estimate* +0.3-0.6 MB before APK compression).

### 7.2 Specialisation

| Mechanism | Used for |
|---|---|
| spec constants (Vulkan) / `#define` (GLES) / function constants (Metal) | geometry mode (`GEO` 0/1/2), box vs weight-table vs min (`OP`), direction, channel formats, recipe member toggles (`LAST`, `GRAIN`, `CLARITY`, `TEXTURE`, `SHARPEN`), workgroup size |
| UBO (runtime) | everything numeric: gains, radii, strengths, geometry matrix, table sizes, grain amplitude |
| separate shader files | different binding signatures or different image formats (`sep_d2y`, `sep_c2y`, `sep_c`, `sep_y` today) |

Every variant is enumerated in CMake (as now), so the set compiled on a device is closed and testable.

### 7.3 Parity tests per backend

One generic test `dapple_node_parity` iterates the registry: for each node with both impls, for each `TestCase` (synthetic seeded inputs, parameter sets chosen to exercise boundaries: tile edges, extreme radii, region/geometry modes), runs the CPU impl and the GPU impl and compares **in the node's own output space** (float planes: max relative / abs error; display images: dE2000 mean/p99/max) against `NodeDesc::parity`. Also runs the whole-graph comparisons of GB 10.6 (22 cases) and the tile-seam tests (GB 10.7) unchanged.

| Target | What it proves | Where |
|---|---|---|
| Desktop GPU, Vulkan (RTX 4060 Ti today) | numerical parity on a real driver, performance | developer machine, ctest `gpu_parity` (exists) |
| SwiftShader / lavapipe Vulkan | deterministic software reference, rounding mode differences vs hardware (GB 10.5 found the NVIDIA truncation this way) | CI, ctest `gpu_parity_swiftshader` (exists); also inside the Android x86_64 emulator via `adb -s emulator-NNNN` (never a physical phone) |
| GLES 3.1 via **ANGLE** on its Vulkan backend (desktop) and ANGLE + SwiftShader (CI, deterministic) | ES-profile semantics are enforced by ANGLE's translator, unlike a desktop GL compatibility context (which accepts formats ES rejects) | desktop CI (*to be set up in step c1; ANGLE must be vendored under `tools/_dl`*) |
| Android emulator, GLES through its host translator | the real Android EGL/GLES loading path, shader load, program-binary cache | emulator only; **permissive** (host desktop GL underneath) so it proves "works", not "conforms" - hence the ESSL lint and ANGLE |
| Real devices (N5, low-end) | driver bugs, fp16 rounding, compile times, thermals, power | needs owner approval (Q1) |

Results are written as `parity-<api>-<device key>.json` so regressions and device-specific deviations are diffable; budgets are per node and per backend (a backend may have a wider budget only with a written reason in the node file).

---

## 8. Film simulation as nodes

### 8.1 Decomposition

Today: `FilmModel` (bake) + `FilmSimulator` (kernels A..D, spatial engine) behind `film::acquireSimulator`, and one fused tile pass for the end of the chain (FA 1, 2, 7, 8).

| Node | Foot. | Reads (deps) | Out | CPU impl (existing code) | GPU form | Shared with main? |
|---|---|---|---|---|---|---|
| `FilmTableRaw` | T | `filmStock`, window/surface/gamut/UV-IR flags, library rev | raw LUT 194x194 f4 | `FilmModel` raw-LUT bake (2-4 ms) | RGBA32F 2D | - |
| `FilmTableCurves` | T | `filmStock`, push/pull profile, `filmPaper` | density + pre-coupler curves 3x256 f2, DIR matrix, scalars | `FilmModel` (0.2 ms) | RG32F (or 2x R32F on GLES) / SSBO | - |
| `FilmTablePrintScan` | T | stock, paper, `filmWarmth`, `filmTint`, preflash, scan flags | 33^3 f4 (density -> display sRGB) | `FilmModel` LUT bake (9-10 ms) | RGBA32F 3D, `texelFetch` tetrahedral | `Tetra` shader fragment shared with `ToneColour` |
| `Gain` (film) | P | WB, `exposure`, `filmExposure`, camera | scaled RGB | folded into the 3x3 input matrix today | fused | main `Gain` type |
| `FilmExposure` | P | `FilmTableRaw`, gain | `E` (3 planes, F32) | `pxExposure` | point (16 `texelFetch` Mitchell) | - |
| `FilmBoost` | G | boost EV (only if > 0) | `E` scaled by global max | pass over E | `Reduce` + scale | `Reduce` shared with dehaze |
| `Blur(mixture)` exposure side | W + G | halation, scatter, `filmHalation`, diffusion filter, lens blur | `E2` | `applyMixture` (FIR for sigma < 3 px, half-resolution pyramid above) | composite: `Pyramid` + FIR (sepconv) + Catmull-Rom `Upsample` | **`Blur` shared** (main pipeline instantiates the Box/Gauss modes) |
| `FilmDevelopQ` | P | `FilmTableCurves`, `E2` | `logE`, inhibitor `q` (1/2 res if all widths >= 3.2 px) | `pxLogE`, `pxCurves`, `pxQ` | point | - |
| `Blur(mixture)` DIR | W + G | DIR widths | `q'` | `applyMixture` | `Blur` | shared |
| `FilmDevelop` | P | `logE`, `q'`, curves | density `d` | develop kernel | point | fusable with `FilmGrain` |
| `FilmGrain` | W (tiny) | `filmGrain`, sub-layers, dye-cloud blur, seed | `d` | `grainRow` (`pcg3d`, stateless) | per-pixel integer hash + small separable blur | hash is film-specific; blur is `Blur` |
| `FilmPrintScan` | P | `FilmTablePrintScan` | display RGB | `pxPrintScan` (tetra) | point (tetra) | `Tetra` fragment |
| `FilmGlare` | W | glare params | + field | blurred log-normal field added after the LUT | `NoiseField` (hash) + `Blur` | `Blur` |
| `FilmScan` | W | scanner blur, unsharp | display RGB | linear-light decode (4097-entry table), blur, USM, encode (4098) | sepconv + point | `Blur` |
| `DisplayStages` | P | curves, LUT, `lutAmount` | encoded RGB | `Renderer::displayStages` | point (1D curves + `sampler3D`) | **main pipeline** too |
| `Blend` | P | `filmStrength` | `normal + s*(film - normal)` | in `renderBakedImpl` | point | main |

With the enlarger diffusion filter the tile pass splits at the print exposure (`LUT1 -> exp -> mixture M4 -> LUT3`): the builder inserts one more `Blur` and one more `Table` node variant; no new mechanism.

### 8.2 What is shared with the main pipeline

`Source/Resize/Geometry`, `Gain`, `Blur` (box iterations, direct Gaussian, Gaussian mixture, pyramid + upsample; boundary policy parameter because the main pipeline replicates edges while film uses scipy `reflect`, FA 5b), `Reduce` (global max/airlight/statistics), the tetrahedral 3D LUT fragment, `DisplayStages`, `Blend`, `Vignette`, `Detail`/NR/dehaze nodes (they run after the film branch exactly as in `renderBakedImpl`), `Pack8`, and all of planner/executor/cache. What film adds: 3 tables, 6 point kernels, the grain/glare generators.

### 8.3 Bake caching in the graph

The three `Table` nodes replace the single `bakeKey` string with three narrower keys (the doc already measures "10 ms when only the print side changed", FA 1): dragging `filmWarmth/Tint` re-bakes only `FilmTablePrintScan`; changing the stock re-bakes all three; `filmGrain/Halation/Exposure` re-bake nothing (exposure is folded into `Gain`, FA 7). The existing `acquireSimulator` pool (<= 4 simulators holding tables and plans) is subsumed by `ResultCache` entries; until step b4 decomposes the simulator, a single `FilmSim` super-node wraps it (10.2, a7) so the graph, keys and fallback already work.

### 8.4 Film cache entries and tiling

`E2` and `Q` are cached at preview scale only (Whole mode, 5.7). In `TiledExport` the film chain is one sweep for everything with a bounded halo (develop -> grain -> print/scan -> glare -> scan: grain blur + dye cloud + scanner blur + unsharp, typically 4 px, FA 2), preceded by the exposure-side sweeps that fill the global pyramid levels for halation/DIR (`Pyramid(build)` accumulates box-filtered half-resolution levels per tile). Planes cost 36 B/px at full frame (FA 2 "Memory", 0.95 GB at 26 MP): that cost never exists in tiled mode and is the reason the exposure/mixture part must be sweep-structured and not a full-frame buffer.

### 8.5 Parity budgets

Budgets are *proposals from measured precedent*, to be calibrated in step b4 and then enforced.

| Level | Budget (GPU vs CPU node, same params) | Precedent |
|---|---|---|
| Main pipeline point/geometry/vignette | mean <= 0.1, p99 <= 1.0 dE2000 | measured 0.036-0.041 / 0.69-0.84 (GB 10.6) |
| Main spatial nodes (NR, dehaze, clarity, texture, sharpen, grain) | mean <= 0.1, p99 <= 1.0 | measured 0.037-0.073 / 0.59-0.88 (GB 10.6) |
| Film tables (CPU baked, uploaded) | bit-identical upload (tested by readback) | - |
| Film point chain (`FilmExposure..FilmPrintScan`), no spatial | mean <= 0.1, p99 <= 1.0 vs the CPU baked path | CPU baked vs CPU direct 0.05 at N=33 (FA 5); CPU vs reference 0.07-0.10 |
| Gaussian-mixture `Blur` | <= 0.5 % of dynamic range at hard edges vs CPU mixture | CPU mixture vs exact double: worst 0.2-0.35 %, wide levels ~1 % of local mean (FA 2) |
| Film spatial incl. DIR, glare, scan (grain off) | mean <= 0.15, p99 <= 1.5 | *estimate* |
| Grain (same seed) | per-pixel agreement after 4x linear box downscale: mean <= 0.1; flat-patch RMS within 3 % per channel | CPU vs reference: RMS within 2 %, seed vs seed floor 0.63-0.81 at 4x (FA 5) |
| Whole film frame (spatial on) | mean <= 0.3 after 4x downscale | seed-to-seed noise floor is 0.63-0.81; same seed expected far lower (*estimate*) |

Grain caveat to carry into product decisions (FA 5b): grain depends on absolute pixel coordinates and pixel pitch, so a 1600 px preview and a full-resolution export show different patterns; previews are not grain previews unless rendered 1:1 in a region (Q8).

### 8.6 Precision

`E`, `E2`, `q`, `d` are `F32` (declared through `requiredPrecision`) in v1: the spec's dynamic range is large and the CPU path is float. `F16Ok` is a later per-node experiment only after parity data (it halves the 36 B/px figure on devices with limited memory).

---

## 9. Display path and frame loop (stage d, interface only)

```cpp
struct FrameOut {                       // what a request produces
  enum Kind { HostRgba8, HostFloat, TileSink, Present } kind;
  std::function<bool(int x,int y,int w,int h,const std::uint8_t* rgba,std::size_t stride)> sink;   // TileSink (export)
  hal::PresentExt* present = nullptr;                                                              // Present
};
class RenderLoop {                      // one per editor view; owns the render thread
 public:
  void submit(ParamSnapshot&&);         // atomic exchange into a one-slot mailbox (latest wins)
  void onVsync(std::int64_t frameTimeNs);  // AChoreographer: take mailbox, render at most one frame, present
  void setQualityHint(int rung);           // from thermal/ADPF policy
};
```

- Output: `SurfaceProducer` Surface -> Vulkan swapchain / EGL window surface (primary, as in GB 3.2), or an AHardwareBuffer ring imported through `ExternalMemoryExt` (fallback, needs `externalMemoryImport`); if neither works the existing readback + `decodeImageFromPixels` path stays (unified-memory readback cost to be measured, Q1).
- The loop is idle when the mailbox is empty and nothing is invalid. ADPF `APerformanceHint` (CPU threads: the render thread and, in CPU mode, the pool) and `AThermal` headroom drive the quality ladder (existing `AdaptiveQuality`, `dapple_thermal_headroom`, `dapple_perf_configure`) plus new GPU-side knobs (resolution rung, skip NR in interactive, frame-rate cap).
- Histogram/analysis: GPU `Reduce` for the display histogram (phase-1 gap 4) or the CPU proxy path; the C API still returns the same JSON.

---

## 10. Migration plan

### 10.1 Rules

- Every step is small enough to review alone, lands behind the old path, and has a **gate** (test that fails the step) and a **fallback** (switch back).
- Gate types: **B** = byte-exact RGBA8 and bit-exact float against the golden corpus; **dE** = ΔE budget; **S** = seam/tile invariance; **P** = performance not worse than noise.
- The golden corpus (step 0) is generated from the *unchanged* code and checked in as hashes (data, not code): extends the 24-render hash comparison of FA 7 to the whole pipeline (direct, baked, region, interactive, RGBA8, geometry, curves, grading, grain, tonal class, looks, detail stages, film on/off/blend, exact variant).

### 10.2 Stage (a): CPU renderer to nodes, bit-identical

| Step | Change | Gate |
|---|---|---|
| a0 | Golden corpus + hash harness (`tests/engine/golden`), recorded from current `Renderer`; `edit-bench` baseline numbers on the throttle profiles | corpus passes on current code; baseline file committed |
| a1 | `core/engine/graph`: types, `ParamSet` + registry (from `paramFields()` + extra keys), hashing, `NodeDesc`, validation, `GraphBuilder` that builds the graph **without executing it**; descriptors for all current stages | graph snapshot tests per scenario; declaration-conformance harness exists (nodes still empty) |
| a2 | `EngineSession` with one super-node `LegacyRender` that calls `Renderer::renderBakedImpl`; `dapple_session_render_frame` routes through it behind `DAPPLE_ENGINE` (default off) | **B** (trivially), **P** |
| a3 | Table nodes: `ColourLattice`, `LatticeOverflow`, `ToneAnalysisPre`, `ToneTable`, `Analysis` using the existing `Renderer::bake` pieces; `ResultCache` replaces `bakes[3]` and `pointKey()/colourKey()` | **B**; reuse decisions equal the old ones on the edit-bench drag scenarios (same lattice-reuse counts); conformance test passes for all table nodes |
| a4 | `Resize` node replaces `scaledSource` | **B** |
| a5 | Pixel group: `Geometry+Gain+ToneColour+Vignette+Pack8` as CPU block fragments in the same float order as `renderBakedImpl`'s per-pixel loop (same `-ffp-contract`, same `LutSampler`) | **B** (RGBA8 and float), **P** |
| a6 | Spatial nodes one by one wrapping `spatial.cpp` functions on whole-frame planes: `NRLuma`, `NRChroma`, `Dehaze`, `Detail`, `Grain` (declared halos, executed whole-frame) | **B** after each; halo and declaration conformance pass |
| a7 | Film: `FilmSim` super-node wrapping `acquireSimulator/setParams/render/renderRows` (including the streaming 8-bit sink path), `DisplayStages`, `Blend` | **B** on the film corpus; `film.*` tests unchanged |
| a8 | Delete `LegacyRender`; `Renderer` stays as a thin facade (tests/CLI) built on the exact variant; `DAPPLE_ENGINE` default on | **B** everywhere; all existing ctest suites green |
| a9 | CPU `TiledExport`: tiles + halos + sweeps for the spatial nodes; `SourceStore` interface with the in-memory implementation | **S** (tiled vs whole: dE mean < 0.01, max 1 code, 5.5); 60 MP synthetic export under the memory budget (measured with the harness in `build/arch-scratch`-style scratch builds) |
| a10 | Cancellation tokens through executor and `ThreadPool` chunk loop; additive `dapple_session_cancel` | cancel latency test (< 1 chunk); no change in outputs (**B**) |

Out-of-scope for (a) (and therefore separate, flagged): changing CPU `Resize` to Lanczos (Q2), `SourceStore` file-backed implementations (Q3).

### 10.3 Stage (b): GPU executor on the Vulkan HAL, then film

| Step | Change | Gate |
|---|---|---|
| b0 | Extract `hal/vk` from `core/gpu/src/vk_core.*`: device pick + caps + blocklist, memory, image pool, pipelines (+cache), descriptor cache, command recording, fences, readback. Behaviour unchanged. `gpu::Session`/`Backend` stay as thin users | existing `dapple_gpu_tests` (`gpu_parity`, `gpu_parity_swiftshader`) pass unchanged: **dE + S** |
| b1 | `GpuScheduler` (dependency layering, barrier-mask computation, transient pool) lifted from `vk_core::Graph` onto HAL handles; same output | same tests; steady-state-no-alloc test |
| b2 | `GpuExecutor` + `GpuImpl` for the nodes the phase-1 shaders cover (`Geometry+Gain+ToneColour+Vignette` recipe, NR, dehaze, detail, grain/pack epilogue, resize); `buildFrame` stage functions become the `record` functions; `exportTiled` becomes the generic `TiledExport` plan | per-node parity (new generic harness) + the 22-case table of GB 10.6 + tile seams **bit-identical** (GB 10.7, 0 differing pixels incl. 60.2 MP) |
| b3 | `Engine` selection replaces `gpu_hook`: GPU if healthy and plan supported, else CPU, per 5.9; `dapple_gpu_set_enabled/status_json` kept and extended (`api`, `fallbacks`); default becomes **auto** (GPU after self-test) only after b3 passes the app-level A/B on the emulator | C API tests; fallback tests (inject `Unsupported`, `Transient`, `DeviceLost`); **dE** of GPU vs CPU through the C API (GB 10.6 table) |
| b4 | Film: (1) CPU decomposition of `FilmSim` into nodes using the same kernels (`KernelSet`, `applyMixture`, tile pass fused through the generic group mechanism); (2) GPU impls per node; each substep has its own gate | (1) **B** on the film corpus and `film.*` tests, **P** (no regression on `film-render-bench`: 26 MP all effects ~659 ms, 1600 px 59 ms, FA 7); (2) budgets of 8.5 per node, grain statistics tests, `film` self-test case |
| b5 | (optional, Q4) `ColourLattice` GPU bake node; CPU stays the oracle | parity of lattice values (max abs error on encoded 0..1), end-to-end dE; **only if Q1 measurements show the CPU bake dominates on low-end** |
| b6 | `SourceStore` implementations `InMemoryHalf` and `MappedHalf` (Q3) | memory gate for 60 MP export (<= 500 MB added), **S** |

What happens to phase-1 code:

| Phase-1 item | Decision |
|---|---|
| `vk_core.cpp/h`: device, caps, blocklist, memory, pool, pipelines, descriptor cache, fences | **Reuse** (moves to `hal/vk`, renamed types, same logic) |
| `vk_core` `Graph` (pass graph, layering, merged barriers) | **Reuse the algorithm, rewrite the types** into the backend-neutral `GpuScheduler` |
| `frame.cpp` `buildFrame`, `spatialRadii`, `FrameJob` | **Reuse** as GPU node `record` functions and `halo()` implementations |
| `session.cpp`: resident source, lattice cache, `exportTiled`, self-test | **Split**: resident source -> `SourceStore`/`ResultCache`; tile logic -> generic `TiledExport` planner (the sweep logic and its tests are the specification); self-test -> `Engine` |
| `hook.cpp`, `gpu_hook.h` | **Replaced** by engine selection (the C API call sites shrink to one call) |
| Shaders | **Reuse**; GLES readiness edits listed in 6.4 (ping-pong instead of RW `rgba16f`, spec-constant workgroup size, binding budget) are done in step c0 for all shaders so Vulkan and GLES stay one source |
| `parity.cpp`, `gpu_tests.cpp`, `gpu_bench.cpp` | **Keep as the gate**, port to the engine API; bench becomes `dapple engine-bench` |

### 10.4 Stage (c): GLES 3.1 backend

| Step | Change | Gate |
|---|---|---|
| c0 | `dapple_shaderc` + CMake pipeline (7.1); ES-rule edits to the shaders; ESSL lint in the build | all shaders produce valid ESSL 310; Vulkan parity unchanged (**dE**) |
| c1 | `hal/gles` against ANGLE on desktop (Vulkan backend, then SwiftShader); caps query; program build with `#define` specialisation; barrier mapping | node parity on ANGLE for every node with a GPU impl; seam tests |
| c2 | Self-test + blocklist + program-binary cache for GLES; Android EGL surfaceless context on the render thread; emulator run (host translator) | self-test passes in the emulator; startup time measured |
| c3 | Selection logic Vulkan -> GLES -> CPU; `DAPPLE_GPU_API` override; `dapple_gpu_status_json` reports both | fallback matrix tests |
| c4 | Real-device pass on at least one Vulkan-weak or Vulkan-less device (Q1) | parity JSON per device; blocklist data |

### 10.5 Stage (d): zero-copy, loop, power, N5

| Step | Change | Gate |
|---|---|---|
| d1 | Kotlin plugin + native `PresentExt` spike (SurfaceProducer Surface -> swapchain / EGL surface; AHB ring variant) on the **emulator**; decide variant | frame shown, resize/rotate/lifecycle tests |
| d2 | `RenderLoop` (vsync mailbox, latest-wins, idle = no work); additive C API `dapple_view_*` (create/attach/submit params/destroy) in a new header; `EditorEngine` gets a `GpuViewBackend` next to the existing path | no `decodeImageFromPixels` and no pixels across isolates during drags; UI-thread jank metric (TR) |
| d3 | ADPF GPU/CPU hints, `AThermal` ladder wired to GPU knobs, low-battery policy, background release (`trim`), `DeviceLost` rebuild | 20-minute continuous edit thermal run (GB phase 4 criterion) |
| d4 | Find N5 measurement (Q1): frame times, power, 120 Hz behaviour; low-end device pass | targets of 1.3 |

### 10.6 C API stability

- All existing exported functions keep signature and semantics. `DappleFrameRequest` is size-versioned (`size` field): new fields are appended; flags get new bits only.
- `dapple_session_render_frame` becomes: parse params -> `FrameRequest` (`long_edge`, `flags`, `bake_size`, region) -> `EngineSession::renderFrame` -> `HostRgba8` -> `DappleImage`. The same `RenderThread`, `sys::adpf()` reporting, and per-session mutex stay in `c_api.cpp`; the `renderFrame()` internals (`scaledSource`, `bakes`, `proxy`, `lastParams`) move behind `EngineSession`.
- `dapple_session_render`, `_export_jpeg`, `_render_look_previews`, `_render_tone_previews`, `_analyze*` become requests of the same engine (export = `TiledExport` + JPEG sink; previews = small `Whole` frames on the CPU executor, as today: GB 5.3 keeps thumbnails on the CPU).
- `dapple_gpu_set_enabled(mode)`: `1/0/-1` kept; default `-1` changes from "off" to "auto" at b3 (decision 1). `dapple_gpu_status_json()` gains keys (`api`, `fallbacks`, `selfTestMs`), none removed.
- New, additive: `dapple_session_cancel`, `dapple_engine_info_json` (executor, API, device key, budgets), the `dapple_view_*` family in stage (d).

---

## 11. Rejected alternatives

| Alternative | Why not |
|---|---|
| Dirty flags per stage instead of content keys | Hand-maintained invalidation is what `pointKey()/colourKey()` already approximates with JSON string masking; every new parameter is a latent stale-cache bug. Content keys + a conformance test make it mechanical |
| One mega-shader with all stages and runtime branches | Register pressure and occupancy on mobile GPUs; recipes with spec constants give the same fusion with specialised code (phase 1 already does this for `point`) |
| Runtime shader composition from fragments | Needs a shader compiler on the device (or GLSL text assembly + driver compiler per combination: compile latency and driver bugs on GLES); build-time recipes are closed and testable |
| Mixed CPU/GPU per node in v1 | Readback/upload at node boundaries is expensive on discrete GPUs and unmeasured on phones; whole-graph fallback is simpler and covers the actual need (unported film nodes) |
| Hardware trilinear for lattices | measured worse by ~0.6 dE mean (GB 4.3); tetrahedral with `texelFetch` kept |
| Cross-API vendor wrappers (Dawn, wgpu, Kompute) | Decided; size and control |

---

## 12. Risks and open questions (ranked)

Ranked by (probability x impact on the targets). "Rec" = my recommendation.

| # | Risk / question | Why it matters | Rec |
|---|---|---|---|
| 1 | **No GPU measurement on a real phone exists.** Everything in GB 10 is a desktop RTX or SwiftShader. Mobile fp16 rounding, `HOST_CACHED` readback, pipeline-creation time, descriptor limits, tile-size optimum, power are unknown | The targets for N5 (8 ms frame) and low-end (15 upd/s) cannot be confirmed or rejected (Q1) | Ask the owner to allow a **read-only** `dapple engine-bench` and the self-test on N5 and one low-end device (explicit serials, no app install, no settings changes). Until then treat every GPU number as *estimate* and keep CPU-side critical path small |
| 2 | **Colour lattice bake stays on the CPU** and is the largest remaining CPU cost: 41^3 = 45 ms on the low-end profile (TR), 57^3 is 2.7x more evaluations (*estimate* ~120 ms), and every colour-slider frame pays it | Colour drags cannot reach 15 upd/s / p95 120 ms on low-end with a CPU bake, whatever the GPU does | Measure on device (1), then add the optional GPU bake node (b5) with the CPU version as oracle. Until then lower the interactive bake size or bake on a background thread for the previous parameters (latest-wins) |
| 3 | **GLES 3.1 conformance cannot be tested faithfully on a dev machine** (desktop GL and the emulator's host translator are permissive); four shaders need rework (RW `rgba16f`); 16x16 workgroups exceed the guaranteed 128; compile/link cost and driver variance are higher than on Vulkan | A GLES backend that "works in the emulator" may fail on the very devices it exists for | ESSL lint with glslc `-std=310es` in the build, ANGLE (+SwiftShader) in CI, per-family self-test with per-program failure isolation, then real-device pass (Q1). Do the shader edits (c0) once for both APIs |
| 4 | **Source memory model** (Q3): `SourceImage.linear` is float RGB 12 B/px; 60 MP = 722 MB, 26 MP = 312 MB; the GPU wants RGBA16F (8 B/px); full-frame decode peaks are outside this design | "Export of 60 MP within mobile memory" is unattainable without changing source residency; touches decoders and the session, which other engineers are editing | Introduce `SourceStore` now as an interface with the in-memory implementation (a9), implement `MappedHalf` in b6; ask the owner/lead who owns the decoder/session code |
| 5 | **Resize and other deliberate CPU/GPU algorithm differences** (area average vs Lanczos-3: 0.74-1.19 dE on a hostile test image, GB 10.6; box x3 vs Gaussian approximations inside the main spatial nodes are the same on both sides, good) | Oracle semantics: the CPU is the test oracle and the fallback; a visible shift on fallback is a product bug, a CPU change breaks "bit-identical" | Keep the migration (a) bit-identical; then, as its own step with a dE gate and golden re-baseline, switch CPU `Resize` to Lanczos-3 (separable, cached, not performance critical). Decision needed from the owner (Q2) |
| 6 | **Zero-copy into Flutter** (stage d): `SurfaceProducer`/Impeller backend differences across Android versions, Surface recreation, rotation, multi-window, sync with Flutter's consumer; AHB import needs device extensions that Vulkan 1.0 devices may lack | The 120 Hz and power story depends on it; fallback is the current readback path | Spike first on the emulator and one device (d1); keep readback as a permanent fallback; measure unified-memory readback cost before deciding how hard to push |
| 7 | **Film decomposition regressions**: fused tile pass performance (130-140 ms of ~280-330 ms at 26 MP on desktop) must survive being split into nodes; GPU mixture accuracy at the largest pyramid levels (~1 % of local mean on CPU); grain parity is statistical; 36 B/px planes | Film is a headline feature; a slower CPU film path or visibly different GPU film look would be a regression | Node CPU impls call the *same* SIMD kernels; the group mechanism reproduces the 256x64 tile pass; gate b4(1) on `film-render-bench` and the byte-exact corpus; calibrate GPU budgets in b4(2) before enforcing |
| 8 | **Planner/graph overhead on the low-end CPU** (the TR numbers show 0.14x host speed: 100 microseconds on the host is ~0.7 ms there) | Per-frame overhead directly eats the 8.3 ms / 30 ms budgets | Plans cached by structure hash, per-frame work limited to hashing masks of ~30 nodes and cache lookups; budget < 0.2 ms enforced by a micro-benchmark gate in a1/a3 |
| 9 | **Driver faults beyond the self-test** (compile hangs, silent wrong results on rare shaders) | A corrupt preview is worse than a slow one | Watchdog timeouts, per-family self-test, device-key blocklist as data, debug-build shadow comparison, a user-invisible "GPU disabled for this device" persisted flag after two faults |
| 10 | **Hot interaction between Dart UI cost and engine** : on low-end 49 % of frames still have UI build > 16.7 ms with engine work off the path (TR) | Even a perfect engine cannot reach 15 upd/s if the UI isolate janks | Out of scope here, but the target in 1.3 should be verified as "engine proxy" first; coordinate with the editor-UI engineers |
| 11 | **Binary size** with three shader representations and film shaders | APK is 26 MiB; +288 KiB measured for phase 1 | Track in CI; strip MSL from Android builds and GLES strings from iOS builds |

### Questions that need a decision

1. **Q1 (owner)**: permission and serials for read-only GPU benchmarks on N5 and one low-end phone (and which low-end model: Adreno 610 / Mali-G52 class?).
2. **Q2 (owner)**: unify the resize filter on Lanczos-3 (CPU changes after step a); or keep the area average on both (GPU loses the quality gain and speed)?
3. **Q3 (lead)**: who owns changes to `SourceImage`/session residency (decoders, `SessionCache`, detail session at 4096 px)? Accept `fp16 RGBA` and an app-cache linear file as the export source?
4. **Q4 (owner)**: accept a second implementation of the colour stage (GPU lattice bake) with the CPU as oracle, if device measurements show the CPU bake is the bottleneck?
5. **Q5 (owner)**: default policy: GPU on after a passed self-test with no user-visible switch (recommended; diagnostics only in About/dev settings); minimum supported device class (Vulkan 1.0 or GLES 3.1, else CPU-only)?
6. **Q6 (lead)**: tier sizes: REQ 6.10 says T0 ~1536 px / 1-2 MP on the GPU, the app uses 1080 px interactive (ladder 1080/810/640/480) and 1600 px final. Should the GPU raise T0 and make T1 viewport-sized (needs `Whole` frames of ~2 MP at 120 Hz: measure first)?
7. **Q7 (lead)**: Flutter/Impeller version and backend policy on target devices; permission to ship a Kotlin plugin for `SurfaceProducer` (stage d).
8. **Q8 (owner)**: grain is resolution dependent; is a 1:1 ROI "grain preview" (zoom detail path) the intended way to judge grain, or must the full-frame preview show a representative grain?
9. **Q9 (lead)**: the histogram/analysis JSON must stay the analysis "of the same render": acceptable to keep it on the 256 px proxy path (as today), delivered up to one frame late during drags?

---

## 13. Sources used for numbers

- Measured, repo: `docs/engine/gpu-backend.md` §4.3, §10.5-10.11 (desktop GPU parity and timings, tile memory, APK delta); `docs/film/architecture.md` §1, §2, §5, §5b, §7, §8 (film timings, memory, accuracy); `tools/throttle/README.md` results tables (lowend/mid/n5 v1-v3, calibration, lattice reuse and fast-analysis micro-benchmarks); `build/perf/*` (raw data referenced by the README).
- Product: `docs/design/requirements.md` §6.1, §6.10, §4.15.16 (latency tiers); `app/lib/services/editor_engine.dart`, `adaptive_quality.dart`, `session_cache.dart` (rungs 1080/810/640/480, bake 57, final 1600 clamp 1080..2048, detail 4096, 200 ms idle).
- Code facts: `core/src/renderer.cpp` (`pointKey`, `colourKey`, `bake`, `renderBakedImpl`), `core/src/spatial.cpp` (operator order, running-sum box), `core/src/parallel.h`, `core/src/c_api.cpp` (`renderFrame`, `DappleSession`), `core/include/dapple/*.h`, `core/gpu/**` (shaders' binding and image-qualifier facts, `vk_core.h`, `gpu.h`).
- Estimates are labelled *estimate* inline; ES 3.1 limits are spec minimums quoted from memory of the Khronos spec and flagged for re-verification; ANGLE/glslc `-std=310es` behaviours are flagged "verify".

---

## 14. Lead review (2026-11-12)

Status: **approved as the plan of record**, with the rulings below. Owner questions Q1, Q2, Q5 and Q8 are open with the owner; the answers will be recorded here.

| # | Ruling |
|---|---|
| Q3 | Approved: `SourceStore` with fp16 RGBA, plus an app-cache linear file as the export source. The lead serialises this work. Steps a9/b6 only start once no other engineer is editing the decoders, `SessionCache` or the session. Gate: the ΔE budget (not B), because fp16 changes values. |
| Q4 | Approved in principle under the GPU-first decision. b5 is built only when device data (Q1) shows that the CPU bake dominates colour drags on low-end. The CPU lattice stays the oracle. |
| Q6 | The app's adaptive ladder (1080/810/640/480 interactive, 1600 final) stays the contract until device measurements exist. REQ 6.10 gets a note, not a change. |
| Q7 | Approved: a Kotlin plugin for `SurfaceProducer` in stage d. The Flutter version is pinned by the repo's `tools/flutter`. |
| Q8 (interim, lead) | `Interactive` must not drop grain while the gesture targets a grain-family parameter (`grain`, `filmGrain`). Add to builder rules 4.6: grain is kept when `FrameRequest.activeParam` is grain-family (new additive field), at the interactive tier's resolution. This applies whatever the owner decides about ROI grain preview. |
| Q9 | Accepted: the histogram stays on the 256 px proxy and may arrive up to one frame late during drags. The final frame's histogram must match the final frame. |
| Risk 10 | The Dart UI-thread jank on low-end (49 % of frames > 16.7 ms) is a separate editor-UI task, scheduled in parallel with stage (a). |
| Process | a0 (golden corpus) is recorded only after the in-flight Motion Photo and film-UI changes have landed, so the corpus reflects the code being migrated. |

### 14.1 Owner answers (2026-11-12)

| # | Answer |
|---|---|
| Q1 | Real-device GPU measurements will happen when the owner is at work with the N5 and a low-end phone. Until then, all phone GPU numbers stay *estimate*. |
| Q2 | Approved: unify resizing on Lanczos-3. The CPU `Resize` changes in its own step after stage (a), with a ΔE gate and a golden re-baseline. |
| Q5 | Approved: GPU on by default once the self-test passes, with no user-facing switch (diagnostics only). Devices with neither Vulkan nor GLES 3.1 run CPU-only. |
| Q8 | The full-frame preview on the edit page is what users judge grain by. While the user drags grain strength, the fit-to-screen preview must look close to the final export viewed at that same screen size: grain must be scale-aware (statistically matched to the downscaled full-resolution grain, not the grain of a low-res render). Tiny thumbnails (filmstrip, swatches) may skip or approximate grain. Combined with the lead ruling above: grain stays in interactive frames while a grain-family parameter is being dragged. Gate: a test comparing grain statistics (variance and spectrum) of the preview against the downscaled final at 2–3 preview sizes. |

---

## 15. Roadmap addition: video frames through the same graph (owner decision 2026-11-12)

Film simulation (and the whole edit) is a general post-process that does not care whether the source is a photo or video. Requirement: REQ §4.9.5a (Motion Photo / Live Photo follows the still's edit, including film).

Scheduled **after stage (b)** (GPU executor + film nodes), as stage **(e)**:

| Step | Change | Gate |
|---|---|---|
| e1 | `VideoSource` node: decoded frames from `core/video` (10-bit YUV planes) → scene-linear via a `Linearise` node per source transfer (S-Log3/S-Gamut3.Cine, HLG, Rec.709 approximate). YUV→RGB, transfer and primaries conversion run on the GPU as nodes (replacing swscale in the hot path) | Formula reference tests (S-Log3, HLG) |
| e2 | Content-dependent parameters (tone analysis, dehaze airlight, auto WB) are computed once on the cover frame and frozen into the `ParamSet` for the whole clip (`ctx.freezeAnalysis`). No per-frame analysis | Flicker test: frame-to-frame mean luminance variance on a static synthetic clip ≈ that of the source |
| e3 | Temporal grain: `ctx.frameIndex` enters the grain seed (stateless hash), so grain changes per frame and is deterministic per frame | Consecutive frames are decorrelated; a repeat render is bit-identical |
| e4 | `ClipExport` plan: frame loop over the same graph (plan cached by structure hash, tables baked once), output to the encoder (HEVC Main10, or 8-bit when film or the compatibility export forces SDR), the still from the same graph | Cover frame vs still ΔE mean ≤ 0.1; 3 s × 30 fps @1080p ≤ 3 s on the N5 GPU (*estimate*; verify on device), CPU fallback completes with progress and cancel |

Design notes:
- The graph is unchanged apart from the source and context keys (`ctx.frameIndex`, `ctx.freezeAnalysis`), which validates the "one graph, any source" design.
- Film output is SDR by nature (print + scan). Without film, the 10-bit/HDR master rules of REQ §4.9.5 apply.

## 16. Pixel 5 findings (2026-11-12, first real-device measurement)

Details, tables and raw logs: `docs/engine/device-results/pixel5.md`. Device: Pixel 5, Snapdragon 765G, Adreno 620, Vulkan 1.1.

Results that change the plan:
- The 1600 px interactive graph costs **45 ms of GPU time** on Adreno 620 (desktop 1.36 ms), not the 4-9 ms estimated in 1.3. It is memory-bandwidth bound: every full-resolution RGBA16F read+write pass costs ~3.5 ms at 1.7 MP (~7.8 GB/s), and the graph has ~27 passes. With all fixes the GPU is 1.7x faster than the 8-thread CPU path (58 ms wall incl. readback vs 102 ms); for point-only drags the CPU (24-37 ms) is competitive.
- Readback is not free on unified memory: ~1 GB/s CPU reads out of the only host-visible buffer type (10-14 ms for 6.8 MB). Target the zero-copy display path; do not read frames back per interaction.
- The Pixel 5 CPU is ~4x faster than the throttled `lowend` emulator profile (near `mid`): it is not a low-end stand-in.
- A 24 MP frame hung for > 589 s because one submission ran ~3 s: see rule 1.
- HEVC Main10 cannot be encoded on this phone (HW encoder advertises Main only; P010 input and Main10 config are accepted and then produce no output; software encoder downgrades to Main). The app falls back to H.264 correctly, but should verify the output profile and add a no-output watchdog.

Mobile GPU rules (apply to every kernel and to the HAL):
1. **Bound the work per submission** (< ~1 s on the slowest supported GPU). Adreno's kernel driver resets the GPU and replays a long submission in a loop. `Engine::submit` splits frames by estimated workgroups (`DAPPLE_GPU_SPLIT_GROUPS`); tile export submits per tile. Split between passes; a single pass must itself stay short.
2. **Read through the texture path** (`sampler2D` + `texelFetch`), not `imageLoad`, for read-only inputs: 2-3x faster on Adreno. Keep storage images for writes and read-modify-write.
3. **Lane-fast axis along the filter/memory axis in both directions.** With the image column as the fast lane axis the vertical blur ran 3x slower (shared-memory bank conflicts).
4. **Size shared tiles for the radius**, use float tiles for single-channel data, 16x16 groups for small radii and 64x4 for large ones; the 12 KB worst-case tile for every radius was wasteful.
5. **No shared arrays sized from specialisation constants** (miscompiled on Adreno 620: wrong output). Use build-time defines; keep the startup self-test, it caught this.
6. **Count passes, not ALU**: kernels sit on the bandwidth floor; the next gains are fusing passes (NR luma + box, dehaze + detail) and lowering resolution of intermediate planes, an O(1) box only matters for the large radii of 24-60 MP exports.
7. **Readback**: the host-visible buffer types are limited by `memoryTypeBits` (Adreno: only cached+coherent, read at ~1 GB/s). Pin the thread that waits and copies to the big cores (done in `Session::render`; copy-out 12-49 ms -> 10-14 ms). For a cached mapping use `AHardwareBuffer` (`CPU_READ_OFTEN`) or avoid readback.
8. **Memory**: default pool budget (1 GB cap) is too high for 7.6 GB phones; use export tile 1024 on mid-range devices (pool ~320 MB).
9. Persist the device key (`vendor:device:driverVersion`) with the self-test result; query `memoryTypeBits` per buffer, never assume a memory type.
