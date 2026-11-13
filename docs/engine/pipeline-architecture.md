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

### 8.7 As built (film engineer, 2026-11-12)

Code: `core/engine/nodes/film/` (`film_nodes.h` API, `film_tables.cpp`, `film_cpu.cpp`, `film_frame.cpp`, `film_gpu.{h,cpp}`, `film_gpu.cmake`), shaders `core/engine/shaders/film_{point,mix,grain,post}.comp`, tests `tests/engine/film_nodes_tests.cpp` (CPU, ctest `film_nodes.*`) and `tests/engine/film_gpu_tests.cpp` (`film_gpu.parity`, only with `-DDAPPLE_GPU=ON`).

| Node (`NodeType`) | Foot. | Own mask (inherited keys come through keyed edges) | Out |
|---|---|---|---|
| `FilmTableRaw` | T | `filmStock`, `ctx.filmLib` | mIn + padded raw LUT |
| `FilmTableCurves` | T | `filmStock`, `ctx.filmLib` (the curves do not depend on the paper) | density / pre-coupler curves, DIR matrix |
| `FilmTablePrintScan` | T | `filmStock`, `filmPaper`, `film_warmth`, `film_tint`, `ctx.filmLib` | 33^3 print + scan LUT, glare white, sRGB decode |
| `FilmExposure` | P | `temp`, `tint`, `exposure`, `film_exp`, `ctx.camera`, `ctx.source`, `ctx.filmLib` (gain folded into the input matrix: byte-identical) | `E`, planar F32 |
| `FilmBlurExposure` | W+G | frame-size keys, `film_halation`, `filmStock`, `ctx.filmLib` | `E2` |
| `FilmDirField` | W+G | frame-size keys, `ctx.quality`, `filmStock` | `q` (half-resolution path when all widths >= 3.2 px) |
| `FilmDevelop` | P | none (tables + inputs) | density `d` |
| `FilmGrain` | W | frame-size keys, `film_grain`, `ctx.quality`, `ctx.keepGrain`, `ctx.frameIndex`, `ctx.fullLongEdge` | `d` |
| `FilmPrintScan` | P | none | display RGB |
| `FilmGlare` | W | frame-size keys, `filmPaper`, `ctx.quality`, `ctx.frameIndex` | linear light + glare |
| `FilmScan` | W | frame-size keys, `filmPaper`, `ctx.quality` | display RGB (scanner blur, unsharp, encode) |

- Graph: `GraphDetail::FullFilm` builds the film branch from these nodes (`Full` still uses the `FilmSim` super-node; flipping `Full` is the CPU engineer's call). Identity nodes are elided at plan time (`NodeBehaviour::isIdentity`); a warmth drag moves `FilmTablePrintScan` and what follows, a grain drag `FilmGrain` and what follows, halation `FilmBlurExposure` and what follows (test `film_nodes.graph`).
- CPU: each `run()` is a piece of `renderSpatial()` on a window (planar F32 intermediates, `ImageView::planeStrideFloats`), the same SIMD kernels in the same order; the composition (`runFilmFrame`, tile 256 x 64, halo accumulated backwards) is byte-identical with `FilmSimulator::render` on every case of `film_nodes.equiv`. `renderer.cpp runFilmStage` calls it (`DAPPLE_FILM_NODES=0` returns to the simulator); the golden corpus is byte-identical in all four modes with the nodes on.
- Highlight boost and the enlarger diffusion split print path are ported (section 22; they were the "falls back to the simulator" cases). Not ported: grain micro-structure (inactive below ~0.6 um pixels).
- Scale-aware grain (14.1 Q8): `ctx.fullLongEdge` names the full-resolution render a smaller frame previews. `makeSpatialPlan(..., GrainFullRes*)` divides the grain blur by the pitch ratio k and the particle count by the variance factor of box-averaging the full-resolution grain after its blur and scanner unsharp mask (`grainBoxFactor`); print glare noise gets the same treatment. Interactive frames keep grain while a grain-family slider is dragged (`ctx.keepGrain`). Video: `ctx.frameIndex` enters the grain / glare hash streams (frame 0 keeps the still's seed).
- GPU: `recordFilmFrame` appends the passes to a `GpuGraph`; the tables are the CPU bake uploaded as is. Mixtures run per channel on R32F planes through a port of `mixImpl` (pyramid down / Catmull-Rom up / FIR); grain, glare noise and the scan stage are ports of the CPU hash kernels. Porting rules found on the way: never use GLSL `%` on negative operands (NVIDIA returned a result that broke every reflect boundary), and never take `textureSize` of a pooled transient (its allocation is larger than the frame).

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

| Step | Change | Gate | Status |
|---|---|---|---|
| a0 | Golden corpus + hash harness (`tests/engine/golden`), recorded from current `Renderer`; `edit-bench` baseline numbers on the throttle profiles | corpus passes on current code; baseline file committed | **done** (148-hash corpus, `dapple_golden`; edit-bench baseline recorded 2026-11-12) |
| a1 | `core/engine/graph`: types, `ParamSet` + registry (from `paramFields()` + extra keys), hashing, `NodeDesc`, validation, `GraphBuilder` that builds the graph **without executing it**; descriptors for all current stages | graph snapshot tests per scenario; declaration-conformance harness exists (nodes still empty) | **done** (IR headers stable 2026-11-12T11:51+08:00; 36 snapshots; keys + conformance + negative controls) |
| a2 | `EngineSession` with one super-node `LegacyRender` that calls `Renderer::renderBakedImpl`; `dapple_session_render_frame` routes through it behind `DAPPLE_ENGINE` (default off) | **B** (trivially), **P** | **superseded** (owner: no LegacyRender path; the thin router exists behind `DAPPLE_ENGINE`, B + P hold) |
| a3 | Table nodes: `ColourLattice`, `LatticeOverflow`, `ToneAnalysisPre`, `ToneTable`, `Analysis` using the existing `Renderer::bake` pieces; `ResultCache` replaces `bakes[3]` and `pointKey()/colourKey()` | **B**; reuse decisions equal the old ones on the edit-bench drag scenarios (same lattice-reuse counts); conformance test passes for all table nodes | **done as table nodes** (B hold in off/legacy/tables; reuse counts identical: bake 24 / reuse 52 / hit 50) |
| a4 | `Resize` node replaces `scaledSource` | **B** | **partial** (Resize node exists; CPU path is a pass-through of the caller's scaled source; `scaledSource` still lives in the session) |
| a5 | Pixel group: `Geometry+Gain+ToneColour+Vignette+Pack8` as CPU block fragments in the same float order as `renderBakedImpl`'s per-pixel loop (same `-ffp-contract`, same `LutSampler`) | **B** (RGBA8 and float), **P** | **done** (fused PointChain block fragments, golden_full byte-identical) |
| a6 | Spatial nodes one by one wrapping `spatial.cpp` functions on whole-frame planes: `NRLuma`, `NRChroma`, `Dehaze`, `Detail`, `Grain` (declared halos, executed whole-frame) | **B** after each; halo and declaration conformance pass | **done** (tile functions with halos; tiled vs whole dE mean <= 0.00013) |
| a7 | Film: `FilmSim` super-node wrapping `acquireSimulator/setParams/render/renderRows` (including the streaming 8-bit sink path), `DisplayStages`, `Blend` | **B** on the film corpus; `film.*` tests unchanged | **partial** (FilmSim super-node on CPU; film node chain runs inside `renderer.cpp` `runFilmStage`, 0 differing floats vs simulator; `GraphDetail::FullFilm` not yet the planner's graph) |
| a8 | Delete `LegacyRender`; `Renderer` stays as a thin facade (tests/CLI) built on the exact variant; `DAPPLE_ENGINE` default on | **B** everywhere; all existing ctest suites green | **not done** (`Renderer` still owns the old render path; `DAPPLE_ENGINE` default still off) |
| a9 | CPU `TiledExport`: tiles + halos + sweeps for the spatial nodes; `SourceStore` interface with the in-memory implementation | **S** (tiled vs whole: dE mean < 0.01, max 1 code, 5.5); 60 MP synthetic export under the memory budget (measured with the harness in `build/arch-scratch`-style scratch builds) | **done** (TiledExport; 60 MP export 4.5 s, 442 MB added) |
| a10 | Cancellation tokens through executor and `ThreadPool` chunk loop; additive `dapple_session_cancel` | cancel latency test (< 1 chunk); no change in outputs (**B**) | **done** (cancel 4 ms after 150 ms request; C++ side only, no Dart binding) |
**Status of a0-a3 (2026-11-12; desktop i7-14700KF, build `build/engine-a`, all 42 ctest entries green).**

1. *a0.* `tests/engine/golden/golden_tests.cpp` (exe `dapple_golden`, ctest `engine.golden_off|legacy|tables`) renders 148 frames through `dapple_session_render_frame` (plus core-API renders: RGBA8 direct, float baked and direct, bake tables) on synthetic scenes (no private photos) and hashes pixels and analysis JSON: direct/exact, baked final and interactive, bake sizes, region, geometry (crops, rotations, straighten), curves, grading and HSL, grain, tonal class (fast vs exact analysis), all 9 looks, each detail stage and all together, a LUT, film (full, 50 % blend, 0 %, off, spatial, geometry, region, exact), and drag sequences (exposure, saturation, clarity, contrast + tonal class, NR, JPEG source, revisits A B A C A D, bake-size flows) so that cache state is part of what is hashed. Corpus: `tests/engine/golden/corpus.txt`, tagged `arch=x86_64 avx2=1 film=1`; a run on another platform exits 77 (skipped) because the film kernels pick AVX2 at run time. Baseline numbers: `docs/engine/baselines/edit-bench-desktop.txt` (default threads and `DAPPLE_THREADS=8`; the throttle profiles were not run).
2. *a2.* `DAPPLE_ENGINE` unset or `0` = old path, untouched; `legacy` = `EngineSession` with `LegacyRender` and the old `bakes[3]` lookup inside it; `1` = a3. Gate P (edit-bench on a 24 MP ARW, `--quick --repeat 20`, interleaved off / legacy / tables, 3 rounds, medians): sum of the 14 "new path" rows 402.4 / 400.7 / 400.3 ms with all threads (tables/off 0.995) and 582.1 / 583.5 / 584.5 ms with `DAPPLE_THREADS=8` (1.004). Single rows move between -9 % and +9 % in both directions, and the same row varies up to 1.5x between runs of the same mode (shared machine), so the evidence is "no slowdown beyond noise", not a speed-up. The graph and key work costs 3.5 us per frame (note 5), less than the 34.7 us the old `pointKey()` + `colourKey()` JSON strings cost.
3. *a3.* The table nodes (`ColourLattice`, `LatticeOverflow`, `ToneAnalysisPre`, `ToneTable`, `Analysis`) are thin wrappers over new `Renderer::bakeColourLattice / bakeTonePre / bakeToneTable / bakeAnalysis`, which copy the steps of `Renderer::bake` (itself unchanged, so the old path stays the oracle). `ResultCache` replaces `bakes[3]`: FIFO, 3 assembled bakes, a lattice lives while a retained bake refers to it, lookup by lattice family with "finest at least as fine as asked", which reproduces the old reuse rule. Lattice-reuse counts: bake / lattice-reuse / full-hit = 24 / 52 / 50 of 126 frames for old and new on the synthetic edit-bench sequences (identical frame by frame, identical pixels and analysis), and 24 evaluated / 52 reused lattices on the real-RAW `edit-bench --quick` for old, `legacy` and new alike. Differences that are improvements: the tonal analysis is lazy (computed only when the frame asks for it; the old bake always ran the exact analysis on final frames), a tone table is not rebuilt when the lattice changes but the tone parameters do not (toneStrength = 0), and the colour lattice key depends on `ctx.filmOnly` instead of the whole film parameter block, so a film change that does not alter the normal render reuses the lattice (golden frames `film/strength0` and `film/off` are full hits in the engine and bakes in the old path; pixels identical). `LatticeOverflow` exists, is cached and tested, but only a GPU executor demands it (`FrameRequest.wantOverflow`); the CPU renderer still builds it lazily inside `renderBakedImpl`.
4. *Declaration conformance.* Key level (`engine.keys`): 55 842 (node, key, variant) checks, no undeclared influence. Output level (`engine.conformance`: 7 baselines including a LUT, tone move fast and exact, film-only and film blend; 1 541 node evaluations): 2 243 equal-key checks with bit-identical output, plus negative controls (removing `contrast` from `ToneTable` or `saturation` from `ColourLattice` is caught). Two deviations are documented and enforced by name (a new one fails the test). (a) `crop`, `straighten` and `vignette` change the output of `ToneAnalysisPre`, of `ToneTable` (when the tonal-class move is on) and of `Analysis`, because `Renderer::bake` analyses the proxy with geometry and vignette while the old `pointKey()` masks them: **the old cache returns a stale tonal analysis / tone-class move after a crop or vignette change** (worked example 3 in 5.7 assumes the uncropped analysis, which is what the keys say and the code does not do). The engine reproduces the old behaviour because gate B is byte identity; fixing it (analyse the uncropped proxy) changes pixels and needs its own re-baselined step. (b) `ctx.fastAnalysis` changes the analysis output but is not in the key: an interactive frame analyses through the lattice, a final frame exactly, and both belong to the same bake (old semantics; edit-bench shows the same dE within 1e-4 on its drag scenarios). One more difference to the old code: an analysis requested for a bake that was made without one is computed when asked, in the requesting frame's mode, whereas the old bake always carried one made in its own mode.
5. *Overhead micro-benchmark* (`engine.overhead`; host, best of 7 x 4000 frames; interactive frame with tone move, analysis, curves and 3 cache lookups): `ParamSet::make` 0.92 us (116 keys), graph build 0.28 us, validate 1.03 us, key computation 1.05 us; total **3.5 us** per frame with the legacy graph and 3.8 us with the full 22-node graph. Budget: 0.2 ms on the low-end profile x 0.14 (risk 8) = **28 us on the host**; margin 7x. Not measured: the same code on a phone (arm64 or the throttled emulator). There is no plan cache yet (no planner exists before a5); the graph is rebuilt every frame, by design (4.6).
6. *C API.* Unchanged except the additive `dapple_engine_info_json()` (engine mode and counters; `edit-bench` prints it). The builder implements the Q8 rule (grain kept in interactive frames while a grain-family slider is dragged; snapshot `grain_dragged_interactive`), but `DappleFrameRequest` has no `activeParam` field yet, so the Dart side cannot send it until that additive field is added.

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

**Status of stage (b) (2026-11-12, integrator).** b0 done (`hal/vk`, Pixel 5 fixes kept; tests `engine.gpu_hal`). b1 done (`gpu_scheduler`; `engine.gpu_sched`). b2 partly: record functions and parity table pass (mean <= 0.073, p99 <= 0.87 vs budget 0.1/1.0; seams 0 pixels), but the GPU frame is still the fixed-order `gpu_frame.cpp`, **not driven by the Planner's stage list**. b3 partly: per-frame fallback and rebuild-once selection live in `core/gpu/src/hook.cpp` behind `EngineSession::gpuHook`; `GpuExecutor` consuming `Plan` does not exist. b4: CPU film nodes done (byte identical, in the renderer); GPU film passes parity (`film_gpu.parity`) but `recordFilmFrame` is not called from `gpu_frame.cpp`, and the GPU film is slower than the 28-thread CPU at 1600 px. b5, b6 not started.

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

Status: `hal/gles/**` and `shader_list.cmake` were started by another engineer; not built or run by the integrator. c2-c4 not started.


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

**Status of stage (d).** d1 done on the x86_64 emulator (SwiftShader): Vulkan swapchain on the SurfaceProducer Surface. d2 done: `RenderLoop`, `dapple_view_*`, `GpuViewBackend`; frames still come from the host-RGBA render, so no zero-readback yet (needs the GPU executor to render into the presenter's VkImage). d3, d4 not started; nothing measured on a phone.

### 10.6 C API stability

- All existing exported functions keep signature and semantics. `DappleFrameRequest` is size-versioned (`size` field): new fields are appended; flags get new bits only.
- `dapple_session_render_frame` becomes: parse params -> `FrameRequest` (`long_edge`, `flags`, `bake_size`, region) -> `EngineSession::renderFrame` -> `HostRgba8` -> `DappleImage`. The same `RenderThread`, `sys::adpf()` reporting, and per-session mutex stay in `c_api.cpp`; the `renderFrame()` internals (`scaledSource`, `bakes`, `proxy`, `lastParams`) move behind `EngineSession`.
- `dapple_session_render`, `_export_jpeg`, `_render_look_previews`, `_render_tone_previews`, `_analyze*` become requests of the same engine (export = `TiledExport` + JPEG sink; previews = small `Whole` frames on the CPU executor, as today: GB 5.3 keeps thumbnails on the CPU).
- `dapple_gpu_set_enabled(mode)`: `1/0/-1` kept; default `-1` changes from "off" to "auto" at b3 (decision 1). `dapple_gpu_status_json()` gains keys (`api`, `fallbacks`, `selfTestMs`), none removed.
- New, additive: `dapple_session_cancel`, `dapple_engine_info_json` (executor, API, device key, budgets), the `dapple_view_*` family in stage (d).

---

### 10.7 What remains before the old paths can be deleted (integrator, 2026-11-12, wave 3)

**Wave 4 update (2026-11-12): items 2, 3, 4, 9 and 10 below are done, item 1 is done except for the marked film seam, item 5 is not (see section 21 for the state and for exactly what can be deleted after device sign-off). `Mode::LegacyBakes` / `Mode::Tables` no longer exist.**

**Status (wave 3).** `DAPPLE_ENGINE` now defaults to `full` (`engine_session.cpp` `envMode`). Override for A/B: env `DAPPLE_ENGINE=off|0|legacy|tables`, or on Android `setprop debug.dapple.engine off|legacy|tables|full` (read when the env var is unset; takes effect at the next process start). Golden corpus re-recorded for the film changes (9 film lines; off, legacy, tables and full agree, 148 frames, 0 mismatches). `check-all` desktop: all steps pass. Android core builds needed `dapple_core` to link `log` publicly (fixed in `core/CMakeLists.txt`).

**A/B on the Android x86_64 emulator (CPU engine, GPU flag off).** Same photo, same drag (+5.00 EV), `full` vs `off`: exported JPEGs are byte-identical (128820 bytes, `cmp` clean); screenshots match. Not covered: film + grain drag, crop, a RAW source, 24 MP, frame timings (no perf-probe build was made).

**Checklist before deleting the old paths** (every item is code or tests, not a device):
1. `renderer.cpp`: `Renderer::render` is still the fallback when a CPU node has no impl or film `setParams` fails; remove the fallback by giving those nodes impls (film boost `boostEv` and enlarger diffusion: ported in section 22, CPU and GPU).
2. Look previews, tone previews, HDR and pano paths still call the Renderer directly; move them onto the engine.
3. `scaledSource` into the Resize node (a4).
4. Table nodes baked through `CpuImpl::bake` instead of `EngineSession::bakeTables` / `bakeLegacy`; then delete the legacy bake keys (`#f`/`#x`) and `Mode::LegacyBakes` / `Mode::Tables`.
5. Move the kernels out of `renderer.cpp` (FrameKernels owns them) and make the golden corpus the oracle; keep `Mode::Off` only until then, as the A/B switch.
6. `NodeDesc::gpuImpl` in the registry is stale (the executor asks `gpuImplFor`): remove the field.
7. GPU: ToneColourExact, Blend (film strength < 100), display curves / look LUT after film and tiled film export have no GPU impl (the CPU fallback stays, so these keep the CPU kernels alive).
8. No device cache of the dehaze/guided solves, no PlanCache; device-tables cache is FIFO(2).
9. Golden tests need a hash re-record per platform: the corpus is skipped on machines with a different `arch/avx2/film` signature, so the Android and NEON engines are not checked against it. Add a per-arch corpus or a cross-mode self-check (off vs full on the same machine) that runs on the emulator.
10. Dart: `dapple_view_submit` ignores `active_param` (GPU view loop); no Dart test for the cancel binding.

**Are the device checks the only blockers? No.** Items 1 to 6 and 9 are software work that does not need a phone. The device checks are additional gates for deleting the *GPU-related old code and defaults*, not the CPU kernels: Pixel 5 and Find N5 measurements (box-scan threshold, vertical scan on Adreno, 24 MP submission splitting, export tile 1024, DeviceLost on a real driver, present-queue family, 120 Hz film budget, `tooSmall` misses), arm64 and armv7 runs of the new film kernels, and on-device A/B of film frames. Until those run, `Mode::Off` and `DAPPLE_GPU_VIEW=0` must stay as switches.

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

## 17. CPU planner and executor (steps a4b - a10, CPU engineer, 2026-11-12)

Status: **landed behind `DAPPLE_ENGINE=full`** (`off`, `legacy`, `tables` still work). Numbers are measured on the desktop (i7-14700KF, `build/v4-cpu`, llvm-mingw clang -O3, other builds running: edit-bench rows vary +-8 % between runs).

### 17.1 What landed

| Piece | Where |
|---|---|
| `Plan`, `Planner` (fusion, tile + halo propagation, sweep structure, cache decisions, memory estimate) | `core/engine/plan/plan.h`, `planner.cpp` |
| `CpuExecutor` (block-fragment head chain, window nodes on tiles, frame-global solves, latest-wins cancellation) | `core/engine/exec/cpu_executor.{h,cpp}` |
| `CpuImpl` of every main-pipeline node: `Source, Resize, Geometry, Gain, ToneColour(Exact), Vignette, DisplayStages, Blend` (block fragments), `NRLuma, NRChroma, Dehaze, Detail, Grain, FilmSim, Pack8` (tile fns). Tables stay in `EngineSession` (HostTable stage) | `core/engine/nodes/main/cpu_nodes.cpp` |
| Kernel facade: the per-pixel loop of `renderBakedImpl` / `renderFloat` as block operations (`FrameKernels`), a recycled float frame (`ScratchFrame`) | `core/src/frame_kernels.h`, tail of `core/src/renderer.cpp` |
| Spatial stages one by one with the frame-global solves split out (`stageNRLuma ... stageGrain`, `dehazeLowGrid/Solve/Apply`, `lumaGrid/guidedSolveGrid`); `applySpatial` now calls them | `core/src/spatial.{h,cpp}` |
| `Mode::Full`, `EngineSession::renderFull / renderExact / cancel`; export and the exact frames go through the executor | `engine_session.*`, `c_api.cpp` (`engineRenderExact`) |
| Cancellation through the pool chunk loop (`CancelScope`), additive `dapple_session_cancel` | `core/src/parallel.h`, `dapple_c.h` |
| Tests: `dapple_exec_tests` (`whole tiles plan cancel analysis`, manual `bench export60`), `engine.golden_full` | `tests/engine/exec_tests.cpp` |

### 17.2 Gates

| Gate | Result |
|---|---|
| **B** golden corpus, 148 frames, modes off / legacy / tables / full | 0 mismatches in all four (after the re-baseline of 17.3). The pixel hashes of all 148 entries are unchanged by the re-baseline. |
| **B** `Plan(Whole)` exact variant vs `Renderer::render` (10 parameter sets: each stage alone, and all) | byte-identical |
| **S** tiled vs whole (tiles 64 / 128 / 192 on a 701x523 frame, 512 on 2301x1499; every stage alone and all together; frame-global solves accumulated tile by tile) | dE2000 mean <= 0.00013 (limit 0.01), max 1 code, 0.00-0.03 % of the pixels differ. Dehaze, clarity, sharpen, grain, veil, geometry + vignette: bit-identical. NR luma / chroma and texture differ by 1 code in <= 0.022 % of the pixels (running-sum box blur rounding, 5.5) |
| halo conformance | `Planner::windowHalo()` per node is exercised by the tile test (a halo that is too small shows as a seam); the frame-global parts are not halos but 1/4-grid solves |
| declaration conformance | `engine.keys`, `engine.conformance` (with negative controls) pass; the legacy-deviation pairs for crop / straighten / vignette are removed from the known list (17.3) |
| edit-bench (`DAPPLE_ENGINE=off` vs `full`, 5 interleaved rounds x 12 repeats, median of rounds, scenarios of the a0 baseline, 24 MP ARW at 2048 px) | 16 rows, ratio full/off 0.85 .. 1.09, median 1.01. Pixel pipeline in isolation (`exec_tests bench`, 1600x1067, executor incl. graph + plan vs `renderBakedRgba`): point only 1.05-1.11, default spatial 1.01-1.06, all stages 0.88, geometry + vignette 1.04-1.15 |
| graph + plan per frame | 17-29 us (FrameKernels + Full graph + keys + plan); 21-91 us per frame measured in the C API (`planUsPerFrame` in `dapple_engine_info_json`) |
| cancel | 3000x2000 all-stage tiled export cancelled 150 ms in: returns "cancelled" 4 ms later; the session renders correctly afterwards (bit-identical to a never-cancelled render) |
| 60 MP synthetic export (9504x6336, all stages, TiledExport, budget 500 MB) | 4.5 s, 60 tiles, 3 sweeps; working set +442 MB at peak with the 722 MB source resident and the 172 MB RGB8 output included (tile working set ~270 MB) |
| ctest | 55 / 55 green |

### 17.3 Intentional differences (documented, re-baselined)

1. **The tonal analysis follows crop, straighten, rotate, flip and vignette (a4b).** The analysis (histogram, tonal class, the tone-class move built from it) is taken from the cropped / vignetted render of the 256 px proxy, but the legacy `pointKey()` zeroed those parameters, so a crop change returned the previous crop's analysis (found by the conformance harness). Fix: the `Analysis` / `ToneAnalysisPre` nodes declare `crop straighten rotate flip vignette` (`registry.cpp`), `Renderer::pointKey()` keeps them, `colourKey()` still masks them (the lattice is reused across crops as before). The off / legacy / tables modes get the same fix, so all four modes agree.
   Re-baselined golden entries (analysis hash only, pixel hash unchanged): `geometry/crop_4:5 crop_3:4 crop_16:9 crop_9:16 crop_orig rot90 rot180 rot270 straighten-650 straighten325 straighten1700`, `detail/all`, `film/full_spatial_final`, `film/full_geometry`, `film/full_region` (15 of 148). `geometry/crop_1:1` is the first frame of its scenario and keeps its value.
   Cost: a crop change now rebuilds the tone table + analysis (proxy render, ~1-5 ms) instead of hitting the stale bake; the lattice is still reused.
2. **The exact graph has a `Gain` node** (`Gain -> ToneColourExact`, and the film branch like the baked graph): one graph snapshot changed (`exact`).
3. **Tiled export differs from whole by <= 1 code** in a small fraction of pixels (5.5, accepted). Exports above 16 MP use `TiledExport`; `DAPPLE_ENGINE_TILE=N` forces tile size N, `=0` never tiles.
4. `ResultCache` limit for the new frame-global solve entries (`Dehaze`, `Detail`): 2 each.

### 17.4 API for the GPU and film engineers

```cpp
Graph g = GraphBuilder::build(ps, GraphDetail::Full, &framePlan);  g.validate();  g.computeKeys(ps);
Plan plan = Planner::plan(g, PlanRequest, &resultCache);            // plan/plan.h, backend independent
ExecEnv env{fk, g, cache, CancelToken, out, channels};  Status s = CpuExecutor::run(plan, env, &stats);
```

* **Plan** (`plan/plan.h`): `stages = [HostTable?, FilmFrame?, (TileSweep, GlobalSolve)*, TileSweep(output)]`. `HostTable` lists the table nodes + advisory `CacheOp`s (the session evaluates them: `EngineSession::bakeTables`, unchanged). A `TileSweep` has `groups[0]` = the PointChain (point nodes fused, in node order `[Resize|Geometry] Gain ToneColour|ToneColourExact [DisplayStages] [Blend] [Vignette]` plus an implicit clamp) and `groups[1..]` = one Window group per spatial node, **cumulative** (a sweep lists every window group it covers). `Stage::halo` = summed window halos (multiple of `grid.align = 4`), `Stage::accum` = the 1/4 grid the sweep leaves for the `GlobalSolve` after it (`DehazeGrid`, `LumaGrid`), `Stage::output` = last sweep, `packInChain` = no window node: the chain writes the 8-bit pixels. `TileGrid{tile, align, nx, ny}`; `Plan::mode` Whole (one tile) or TiledExport; `peakCpuBytes`, `allocs` (lifetimes in stage indices). A solve that is in the `ResultCache` (`Dehaze` / `Detail` entries under the node key) is not planned, and the sweep that would have produced its grid disappears. Film frames are planned Whole (`Plan::why`).
* **Executor selection per frame** (`EngineSession::renderFrame`, Full mode): tables -> `job.gpuHook` (the GPU engineer replaces this by a `GpuExecutor` that consumes the same `Plan`; return null / `Unsupported` to fall through) -> `CpuExecutor`. The CPU executor returns `Unsupported` when a node has no impl or the film stage fails; the session then renders the frame with `renderBakedRgba`. A node's CPU table is `cpuImpl(NodeType)` (`nodes/main/cpu_nodes.cpp`): `CpuImpl::block` (point fragments: `px` is **3-float interleaved RGB, n <= 256**, absolute `PixelPos`), `CpuImpl::tile` (additive field in `node.h`: window nodes on an `ImageF` tile `CpuTile{img, scratch, ox, oy, frameW, frameH, dehaze, guided, interior, out8}`), `bindResolved()` builds the per-frame resolved block (`Resolved`, 128 B). `ExecStats` reports sweeps / tiles / solve hits. `cpuFusedHead()` is the CPU fusion recipe `[Resize|Geometry] + Gain + ToneColour` in one pass.
* **Film engineer**: `FilmSim` is a whole-frame node (`tileFilmSim` -> `FrameKernels::runFilm`, plus the streaming path `streamFilm` for a full-strength film to 8 bit with no neighbourhood stage); `DisplayStages` / `Blend` are block fragments that read the film plane (`FrameEnv::film`). Replace `tileFilmSim` and `FrameKernels::runFilm/streamFilm` with your nodes; keep the `FilmFrame` stage contract (a float plane of the frame, display-encoded, unclamped) or extend the planner to tile it (it forces `Whole` while a `FilmSim` node exists).
* **GPU engineer**: the graph, `Plan` and `FrameRequest`/`ParamSet` are the contract. `PlanRequest` carries the resolved neighbourhood amounts, so `Planner::windowHalo` can be the halo source for `spatialRadii`. The frame-global parts are solved on the 1/4 grid (`dehazeSolve`, `guidedSolve` in `spatial.cpp`) and looked up at absolute coordinates: a GPU sweep can use the same structure (accumulate grid, solve, apply per tile).
* **Cancellation**: `EngineSession::cancel()` / `dapple_session_cancel()` bumps the ticket; `CancelToken{&latest, mine}` is polled between tiles, between groups and in the `ThreadPool` chunk loop (`CancelScope`). A cancelled run returns `Status::Cancelled`, stores nothing partial, `dapple_last_error() == "cancelled"`.

### 17.5 Not done / not verified

* Table nodes have no `CpuImpl::bake` registration (they remain `EngineSession::bakeTables`, which the `HostTable` plan stage stands for). `Resize` is a pass-through of the caller's scaled source (a4, the `scaledSource` replacement, was not part of this task). There is no `PlanCache` (planning is 20-90 us).
* Film frames: Whole only, FilmSim is the existing super-node; an exact frame whose film `setParams` fails falls back to `Renderer::render`.
* Not run: Android / NEON builds of the new files, the throttle profiles, the Pixel 5, the app (the Dart binding of `dapple_session_cancel` is not added). Look previews, tone previews and the HDR / pano paths still call the renderer directly.
* The cached `Detail` / `Dehaze` solves are keyed by node key; for baked frames the key includes the LUT pointer identity like the lattice cache does (same pre-existing limitation). Exports use a private per-call cache.

---

## 18. Film on the planner, scale-aware grain from the C layer, automatic film exposure in film-workflow stages (film engineer, 2026-11-12)

* **Planner on `FullFilm`.** `EngineSession::renderFull` / `renderExact` build `GraphDetail::FullFilm`; the planner recognises the eight film image nodes (`FilmExposure` .. `FilmScan`) and plans them as one `FilmFrame` group (the three `FilmTable*` nodes sit in the `HostTable` stage with the other tables). The executor still runs the chain as one whole-frame stage (`FrameKernels::runFilm` -> `filmnode::renderFilmFrame`, internally tiled 256 x 64); film frames stay `Whole`. Golden corpus: byte-identical in off / legacy / tables / full with `DAPPLE_FILM_MATCH_OFF=1 DAPPLE_GRAIN_SCALE_OFF=1` (checked against the tree with the `#f` / `#x` bake-key change of another engineer reverted; with that change the 9 non-film entries move on their own).
* **C layer.** `SessionCtx::gainFn` = `Renderer::inputGain` (new static; the constructor shares `whiteBalanceGain`). `FastOptions {keepGrain, fullLongEdge, frameIndex}` are set in `renderFrame`: `keepGrain` when `INTERACTIVE` and `active_param` is `grain` / `film_grain`; `fullLongEdge` = source long edge when the frame is smaller than the source and has no region. Additive `DappleFrameRequest::active_param` (`const char*`, last field; the struct is copied by `min(size, sizeof)` now, older callers leave it NULL). With `keepGrain` the main `Grain` node also stays on in interactive frames. Frames that need scale-aware / kept grain skip the streaming simulator path (node chain only). Dart: `DappleFrameRequestStruct.activeParam`, `DappleSession.renderFrame(activeParam:)`, `EditorEngine.beginInteraction(activeParam:)` (the editor controller passes the row id). Not wired: the GPU view loop (`dapple_view_submit` ignores `active_param`).
* **Automatic film exposure, v5 (owner: normalising every stock to the same three anchors made them look alike; film's limited range must show).** `matchFilmExposure` (`core/include/dapple/film_match.h`) returns two internal EVs, `EditParams::filmKeyEv` (film exposure) and `EditParams::filmPrintEv` (print / scan exposure); both are hashed through `ctx.filmKeyEv` / `ctx.filmPrintEv` and cached per `filmMatchKey` (the edit minus film_exp / strength / grain / halation / black / spatial stages, plus LUT) in the C session (12 entries, the cache holds the pair); `Developer::develop` computes them as well. The history (v2 one full median match on the film EV; v3 70 % pull + latitude window; v4 latitude placement + print solved per image and stock + black stretched to display black) is reachable with `DAPPLE_FILM_MATCH_FULL=1`, `_PULL=<f>`, `_V4=1`; `DAPPLE_FILM_MATCH_OFF=1` = no automatic exposure and no paper mapping (v1).
  * **1. Metering, per image, stock independent** (`solveMeter`, `meterEv`, `sceneKeyEv`). Scene-linear luminance Y = 0.2880 R + 0.7119 G + 0.0001 B of the 256 px proxy (ProPhoto, after white balance and the camera baseline, before the user's develop exposure), unclipped pixels only (any source channel >= 0.98 is clipped; if more than 25 % is clipped, all pixels). Meter reading m = median of those pixels (the intended key below is a median too). `EV_meter = log2(0.184 / m)` puts the reading at 18.4 % grey. Intended key: median L* of the develop pipeline's own render of the same edit at develop exposure 0 (`keyL0`), its relative luminance Y_key = ((L + 16) / 116)^3 (L > 8); `EV_scene = clamp(kFilmSceneKeyScale * log2(Y_key / 0.184) / kFilmSystemGamma, -2, +2.5)` with `kFilmSceneKeyScale = 0.5` (owner decision, "option C": half of the compensation, so the film chain keeps depth and the highlights of a high-key scene roll off in the paper; `DAPPLE_FILM_SCENE_EV_SCALE=1` reproduces the previous default) and `kFilmSystemGamma = 1.15`, the nominal slope of the film + print system around mid-grey (display light ~ scene light ^ gamma; measured 1.25 - 1.5 per EV between -1 and +1 EV, less towards the shoulder; without it every high-key scene printed 4 - 6 L* brighter than the digital key). The develop exposure slider and `film_exp` stay offsets on top (the film gain is `wb * 2^(exposure + baseline) * 2^(film_exp + filmKeyEv)`); `film_exp` is the raw EV offset applied after everything, `film_shoot` below is the managed one.
  * **2. Film exposure** = `EV_meter + EV_scene + convention` (negative +0.5 EV = `kFilmNegativeBiasEv`, reversal -0.3 EV = `kFilmReversalBiasEv`), identical for all stocks of a type. **Usability guarantee** (not an optimum): from the stock's own green-channel density curve (toe / shoulder = the logH below / above 0 where |slope| first falls under 20 % of the mid gamma, mid gamma = mean |slope| over logH [-0.3, 0.3]; logH = log10(Y / 0.184) + EV * log10 2) the share of scene pixels inside [toe, shoulder] is computed (`inRangeFraction`, sorted per-pixel logH, binary search). If it is at least `kFilmRangeFraction = 0.80` (`film_latitude` moves it, 0.55 .. 0.98) the shared exposure stays; otherwise the film EV moves by the smallest amount (0.05 EV steps within +-6 EV) that reaches the target, or to the EV that maximises the share (ties: nearest to the shared exposure). Up to 20 % of the scene may still clip or block. Negatives that moved are printed down / up by the same amount (below). `FilmMatchInfo` reports `sharedEv`, final EV, `fracShared`, `fracFinal`, `fracBelowToe`, `fracAboveShoulder`, `stopsBelowToe`, `stopsAboveShoulder` (p2 below the toe / p99.5 above the shoulder at the final EV).
  * **3. Print / scan exposure = a per stock + paper constant** (`filmPrintConstant`, cached): `FilmParams::enlarger.printExposure = 2^-printEv` (the port's existing paper exposure multiplier) such that an 18.4 % grey card exposed at box speed + the type convention prints to L* 50 relative to the paper white (displayed Y = 0.184 * Y_white after the paper mapping), with the port's neutral filter pack. No per-image solve; for a negative whose film EV moved by d from the shared exposure the constant is re-calibrated at +d (`filmPrintConstantAt`, 0.5 EV grid, linear interpolation: a lab prints a denser negative down). A slide (scan, no print) has no constant (0): the clear film is display white by the mapping below, a lamp offset would only dim it, and reversal gets no compensation. Plumbing: `EditParams::filmPrintEv` -> `makeFilmParams` / `FilmResolved::printEv` / `FilmFrameArgs::printEv` / `FramePlan::Film::printEv`; the print-table cache key and `FilmModel::setPrintExposure` carry it (one 33^3 LUT re-bake, ~10 ms, no film re-bake).
  * **4. Paper to screen** (`FilmModel::buildPostMap`, `scanner.paperReference` + `blackMix`, set by `applyFilmPaperMapping`). The inherited stage (spectral reflectance under the viewing illuminant, Y normalised to a perfect white diffuser, CAT02 to D65, sRGB, CAM16-UCS output gamut compression, OETF) shows paper white at only L* 81.6 - 87.5 because the gamut compression soft-clips the top. Mapping, after the compression and baked into the print LUT: the displayed paper white (unexposed paper, Dmin; a slide: the clear film) is scaled to display white 0.98 (L* 98.2) by a linear scale of the display light; the black keeps its true relative luminance (v4 stretched it to display black). `film_black` (`blackMix`) -1 .. +1: -1 = lifted matte black (relative luminance 0.02, L* 15.8), 0 = true, +1 = display black (the v4 mapping). Colours keep their ratios; the white patch is within 1 % across channels. Off with `DAPPLE_FILM_PAPER_MAP_OFF`, `_FULL`, `_PULL`, `_MATCH_OFF`.
  * **Overrides** (serialised `EditParams`, normalised -1 .. 1, 0 = automatic, `film_shoot`, `film_print`, `film_black`, `film_latitude`; no UI yet): `film_shoot` moves the film EV from the automatic one between the EV where the in-range share falls to 0.55 on the under side and on the over side (`kFilmShootEdgeFraction`, never beyond +-3 EV; a slide additionally stays inside a usable median L* 33 .. 86 by bisection), negatives are re-calibrated in print so brightness stays with EV_scene (the override mostly changes grain, shadow detail, colour), a slide's brightness follows; `film_print` solves the print / scan EV so the median reaches, at -1 / +1, L* 30 / 85 relative to the paper white (clamped to the paper's usable density range: black raised by 0.15 D, white lowered by 0.05 D; a slide's lamp is capped so the clear film never exceeds display white), linear in between; `film_latitude` is the in-range share (0.55 / 0.80 / 0.98). **`film_grade` (paper contrast) is not provided**: the port has no physically meaningful paper-grade handle (only enlarger preflash, which lowers highlight contrast, and the film's `densityCurveGamma`, which is development, not paper), so nothing was faked. Pad labels: `dapple_session_film_pad_json(session, params_json)` returns the actual film EV at film_shoot -1/0/+1, the print median L* at film_print -1/0/+1 (targets and the automatic median), the paper black L* at film_black -1/0/+1 and the in-range fractions at film_latitude -1/0/+1 (`FilmPadInfo`); a pad position is previewed by rendering the 256 px proxy with `dapple_session_render` (out_long_edge 256) with the offsets in `params_json` (the match is cached per key; a moved film_print costs a few proxy renders). CLI: `dapple film --shoot f --print f --black f --latitude f [--pad-json file]`.
  * **film_wb: film white balance (illuminant fidelity; replaces film_cast, which is gone).** `film_wb` 0 .. 1, default 0 (serialised key `film_wb`; an old `film_cast` key is ignored). The film sees the scene illuminant instead of the white-balanced scene: the as-shot illuminant (from the RAW, `ShotMetadata::asShotIlluminantXy`) is put back, `mInImg_ = CAT16(scene white -> stock reference white, cone gain D*Wd/Ws + 1 - D) * CAT16(D50 -> scene white) * ProPhoto->XYZ` with D = 1 - film_wb (`FilmModel::buildInputMatrix`, `setInputAdaptation`). film_wb 0: D = 1, `mInImg_ = mIn_` exactly, bit-identical for every WB setting (golden lines unchanged). The neutral print-filter pack and the grey normalisation still use `mIn_` (the pack is solved at the film's reference light and not re-timed: a negative at film_wb 1 shows the mismatch of an un-timed print; real minilab prints sit between). Negatives and slides alike (a slide has no pack to move, but the film's balance applies to it the same way). The stocks need no pairing data: each stock's own `info.reference_illuminant` is the adaptation target (D55 for most stocks, T for Vision3 200T / 500T).
    * *User white balance.* A WB edit (temperature / tint) changes the assumed scene illuminant: the light is the as-shot light seen through 1 / g, g = the gain the edit applies (`filmInputAdaptation`). So at film_wb > 0 the WB sliders re-declare which light the film was shot under; at film_wb 0 they are the plain digital white balance. An unknown scene illuminant (no RAW metadata) means no effect at any film_wb.
    * *As-shot illuminant* (`libraw_decoder.cpp`): LibRaw `cam_xyz` inverted against the camera neutral gives xy, but this matrix-only estimate is cool-biased on the Sony ARWs (14 files: 20 - 39 mired against the camera's own Kelvin table `WBCT_Coeffs`, rms 23.6 mired; fit mired_cam = 1.242 * mired_matrix - 31.7, rms 7.2). Where the table exists the CCT comes from it (projection in log(R/G), log(B/G) against mired) and the matrix supplies only the tint offset; other cameras (not validated, e.g. Fuji RAF) fall back to the uncorrected matrix estimate. EXIF presets, matrix vs table: daylight 5633 / 5204 K, tungsten 3055 / 2718 K, cloudy 6495 / 6031 K, shade 8154 / 7495 K.
    * *Evidence* (grey card, film_wb 1, C* / hue of the print of an 18.4 % grey, `dapple film --scene-cct K --wb 1`):

| stock | 2850 K | 3200 K | 4000 K | 5500 K | 6500 K | 7500 K | 10000 K |
|---|---|---|---|---|---|---|---|
| kodak_portra_400 | 47/62 | 41/65 | 23/71 | 0 | 10/258 | 16/260 | 25/263 |
| kodak_ektar_100 | 50/60 | 44/63 | 28/69 | 0 | 11/260 | 19/260 | 28/261 |
| fujifilm_pro_400h | 48/67 | 42/70 | 25/77 | 0 | 9/258 | 15/263 | 23/263 |
| kodak_gold_200 | 48/62 | 42/64 | 25/72 | 0 | 9/258 | 15/260 | 23/262 |
| kodak_vision3_500t | 0 | 9/256 | 21/254 | 30/258 | 31/258 | 32/256 | 33/256 |
| kodak_vision3_200t | 0 | 9/260 | 21/257 | 31/261 | 33/263 | 33/261 | 34/261 |
| fujifilm_velvia_100 | 49/70 | 42/72 | 25/75 | 1 | 10/278 | 16/274 | 25/277 |
| kodak_kodachrome_64 | 37/69 | 30/69 | 17/74 | 2 | 4/257 | 8/258 | 12/258 |

    Daylight stocks go orange under tungsten light and blue under shade; the tungsten stocks are neutral under their own light and blue under daylight; the cast grows monotonically with film_wb (test). Showcases: `build/film/showcase_v8/wb_DSC00225.jpg` (warm indoor, as-shot 3313 K), `wb_DSC00111.jpg`, `wb_DSC00715.jpg`, `wb_DSC00567.jpg`: 8 stocks x film_wb 0 / 0.5 / 1; the tungsten stocks barely change under the 3300 K indoor light, the daylight stocks turn warm.
    * *Cost while dragging.* Only the 3x3 `mIn` and a copy of the shared raw LUT change (about 1 ms, no model rebuild); the raw-table cache key adds (scene xy, D) only when D < 1; the GPU raw table (about 2.4 MB) is re-uploaded. GPU parity (SwiftShader Vulkan, strict budget mean <= 0.1, p99 <= 1.0): film_wb 0.5 / 1 cases mean 0.056 - 0.069, p99 0.53 - 0.73. The GLES backend was not built in this tree and is not verified.

  * **Stocks define themselves (owner principle: the RAW is the scene, each stock's own data is the stock).**
    * *What the neutral solve is* (`FilmModel::solveNeutral`, film_model.cpp:787): the enlarger M / Y pack (C = 0) at which the print of an 18.4 % grey (`dMidDir_`, the developed mid-grey density) is neutral in display RGB, under the enlarger light TH-KG3 and the paper's viewing illuminant (D50; K75P for 2383 / 2393). Upstream spektrafilm ships the same quantity as `data/filters/neutral_print_filters.json` (paper x illuminant x film); ours reproduces it to about 0.1 filter units (Portra 400 on Portra Endura 51.65 / 52.55 vs 51.57 / 52.53). By design it is part of the stock + paper definition (upstream README: "the starting settings that make an 18% gray target photographed with the correct reference illuminant fully neutral in the final print").
    * *Input side* (film_model.cpp:271): `mIn_ = CAT16(ProPhoto D50 -> the stock's reference illuminant) * ProPhoto->XYZ`: the white-balanced scene is always mapped fully to the film's own balance white (degree of adaptation 1). Reference illuminants (profile `info.reference_illuminant`): D55 for every camera film in the catalog except vision3_200t / 500t (T, tungsten); papers and print films TH-KG3.
    * *A stock-independent printing aim* (LAD style) does not exist in the data: paper and print-film profiles carry only spectra, curves and `midscale_neutral_density`. What the data does carry is the pairing `info.target_print`: Pro 400H, C200, Xtra 400 -> `fujifilm_crystal_archive_typeii`, the Kodak stills -> `kodak_portra_endura`, cine -> `kodak_2383`, slides none. **The default paper now follows it** (`FilmStock::targetPrint`, new pack section PAIR, `defaultPaperFor`, field `paper` in `dapple_film_list_json`, Dart `FilmStock.paper` / `FilmCatalog.defaultPaperOf`; the pack `dapple.dfp` and its app copy were regenerated, older readers skip the section). Before, every non-cine negative was printed on Portra Endura, which gave the Fuji negatives an extreme solved pack (Pro 400H 33.6 / 24.7 against 52 - 75 for the rest). Effect of the new pairing: N400A / Pro 400H only (its chromatic dE to the digital render on the four photos 4.5 / 3.9 / 5.8 / 5.3 against 5.1 / 4.9 / 6.3 / 4.6 before).
    * *Each stock against the scene with self-calibration* (24-patch chart, same exposure, scene truth = the sRGB colorimetric input): mean chroma ratio to truth N400C 1.04, N100 1.09, N200B 1.05, N400A 1.04, R100B 1.10, C250D 0.97, N800+2 1.31, R64 0.81; mean hue shift 6 - 10.5 deg (worst yellow-green -22 deg in N400A, -20 N100, -19 N800+2; cyan -18 deg in R64); dark skin is boosted x1.7 - 2.2 in every stock (N800+2 x2.22, light skin x1.83 with +14 deg); mid-grey L* 50.4 - 51.8 (truth 51). Grey ramp (-6 .. +2 EV): the negatives stay within C* 2.4, the slides within 3.3, C250D on 2383 crosses over to C* 4.6 at -2 EV and 5.5 at +2 EV. So with self-calibration the stocks differ in colour rendering (chroma 0.8 - 1.3, hue 6 - 22 deg), not in grey-scale crossover.
    * *Illuminant fidelity (experiment, `dapple film --adapt D --scene-cct K`, FilmSimulator path only)*: the scene illuminant is put back (D50 -> scene white) and adapted to the film's reference white with CAT16 degree D (D = 1 is bit-identical to today). Chart, neutral-patch cast C* (hue) for D = 1 / 0.5 / 0: N400C under 3200 K 0.6 / 18.6 (65 deg) / 41.1; under 7500 K 0.6 / 8.0 (260 deg) / 15.5; N800+2 under 3200 K 52 at D = 0, R64 30; the tungsten stock vision3_500t is neutral under 3200 K (D = 0: C* 8.2) and blue under 5500 K (28.5, 255 deg). No cast at 5500 K for the D55 stocks. Photos at D = 0 go warm (as-shot 4400 - 4900 K) or teal (6600 K sky); the stock-to-stock difference does not change (4.4 -> 4.4), the shift is common to the D55 stocks.
  * **Pad effectiveness.** `dapple_session_film_pad_json` returns `scene_cct` / `reference_cct` (K, 0 = unknown: the assumed scene illuminant and the stock's reference illuminant, for labelling the film_wb axis) and `effective_lo` / `effective_hi` per axis (`film_shoot`, `film_print`, `film_black`, `film_wb`, `film_latitude`): whether that end of the axis changes the render for this image and stock, from the solved values (film_shoot: the guarded film EV differs from the automatic one; film_print: the median differs by more than 0.5 L*; film_black: the paper mapping is on; film_wb: the lower end is never effective, the upper end only when the scene illuminant is known and differs from the stock's reference by more than 0.004 in xy). A slide's film_print +1 is not effective (the lamp cannot brighten the clear film). `film_print` never moves against its sign (the automatic median may already lie beyond the end of the band) and the lamp cap of a slide never darkens a +1.
  * **Showcase v6** (new defaults: scene-key scale 0.5, film_cast 0). Mean pairwise CIEDE2000 between the 8 stock renders (300 px), v1 / v5 / v6: DSC00111 3.88 / 4.00 / 5.68, DSC00715 3.23 / 4.88 / 5.81, DSC00567 (a person, skin, foliage, saturated props) 3.87 / 4.58 / 6.07, DSC00282 (a person, green curtain) 3.20 / 3.82 / 5.01. On the two colourful photos the stocks are visibly distinct (N100 cooler / pinker, N400A cooler and greener, N800+2 warm and saturated, C250D milky highlights, R64 muted); no default render has an objectionable cast, the strongest (N400A, N100) are slightly cool / magenta in the neutral backgrounds.
  * **Per-stock latitude** (green layer):

| stock | kind | mid gamma | toe logH | shoulder logH | latitude (stops) |
|---|---|---|---|---|---|
| N400C (Portra 400) | negative | 0.47 | -1.65 | +2.46 | 13.7 |
| N100 (Ektar 100) | negative | 0.51 | -1.55 | +2.46 | 13.3 |
| N200B (Gold 200) | negative | 0.48 | -1.71 | +2.05 | 12.5 |
| N400A (Pro 400H) | negative | 0.45 | -1.74 | +2.44 | 13.9 |
| R100B (Velvia 100) | slide | 1.26 | -1.57 | +0.62 | 7.3 |
| C250D (Vision3 250D) | negative | 0.43 | -1.96 | +2.85 | 16.0 |
| N800+2 (Portra 800 push 2) | negative | 0.65 | -1.41 | +2.46 | 12.9 |
| R64 (Kodachrome 64) | slide | 1.11 | -1.68 | +0.76 | 8.1 |

  * **Per-paper range as displayed** (stock Portra 400, patches 8 EV over / under grey printed at print EV +-8; mapped, black true; "off" = the inherited stage; contrast ratio = white Y / black Y):

| paper | Dmax R/G/B | white L* off -> on | black L* off -> on | contrast on | L* range covered on |
|---|---|---|---|---|---|
| fujifilm_crystal_archive_typeii | 2.45 / 2.19 / 1.96 | 87.5 -> 98.2 | 3.4 -> 4.5 | 190:1 | 93.7 % |
| kodak_ektacolor_edge | 2.25 / 2.02 / 1.84 | 86.6 -> 98.2 | 3.4 -> 4.6 | 187:1 | 93.6 % |
| kodak_endura_premier | 2.43 / 1.89 / 1.92 | 86.6 -> 98.2 | 2.9 -> 3.9 | 218:1 | 94.2 % |
| kodak_portra_endura | 2.46 / 2.07 / 1.82 | 87.2 -> 98.2 | 2.9 -> 3.9 | 220:1 | 94.3 % |
| kodak_supra_endura | 2.48 / 2.01 / 1.92 | 87.5 -> 98.2 | 3.0 -> 4.1 | 212:1 | 94.1 % |
| kodak_ultra_endura | 2.39 / 1.52 / 2.13 | 81.6 -> 98.1 | 1.5 -> 2.4 | 364:1 | 95.8 % |

  The v4 black stretch gave 0.7 for all papers (contrast 1215:1). Slides (scan): white L* 98, black below 8.
  * **Showcase v5** (CLI, 1200 px). EV_scene +1.52 (DSC00111, key L* 82.7) / +1.20 (DSC00715, key L* 74.7). Negatives: film EV 1.77 (all inside the range, 100 %, no rescue), print constants -0.46 .. -0.54. Slides: film EV shared +0.97; R100B on DSC00111 77 % -> 80 % (final +0.82, 0.15 EV down, 1.10 stops of the scene above the shoulder), R64 86 % (kept, 0.79 stops above the shoulder); DSC00715 both 100 %. DSC00111 p1/p5/p50/p95/p99 L*, fractions below L* 3 / above L* 97: N400C 13/21/84/98/98, 0.1 % / 16.4 %; N100 12/19/83/97/97, 0.1 / 12.5; N200B 14/21/83/97/97, 0.0 / 4.5; N400A 13/21/83/97/97, 0.1 / 10.8; R100B 4/15/87/98/98, 0.9 / 23.9; C250D 9/19/80/96/96, 0.4 / 0.0; N800+2 7/14/87/98/98, 0.4 / 24.2; R64 16/26/85/98/98, 0.0 / 16.4. DSC00715: all p50 72 - 80, p99 81 - 91, nothing above L* 97, R100B 3.4 % below L* 3, C250D 1.8 %. Mean pairwise CIEDE2000 between the 8 stock renders (300 px): DSC00111 v1 4.10, v4 8.16, v5 4.00; DSC00715 v1 3.26, v4 4.39, v5 4.88 (v4's number on DSC00111 comes from two dark slides, not from character). The 9 pad positions (film_shoot x film_print = -1 / 0 / +1) of the 8 stocks on both photos: 144 renders, median L* 28 .. 89, at most 1.6 % below L* 3 and 26.7 % above L* 97 for negatives, 14.5 % / 22.5 % for slides, none broken (median in [25, 90], < 35 % below L* 3 / above L* 97).
  * **Limits / honest findings.** On these two bright scenes the print is 1 - 5 L* above the digital key (the film chain is more contrasty than the digital tone curve); the film range only shows where the scene exceeds it: negatives lose nothing here (13+ stops against 7 - 9 stops of scene), the two slides lose about 20 % above the shoulder, so the stocks differ mostly in curve shape and colour (stock difference about the same as v1). The model is luminance-only on the green layer, crop geometry is not applied to the proxy, and the print EV of `film_print` is solved on the median only. `film_black` and the paper mapping change the look of every film render in the app; the previous looks are available through the switches above.
* **Highlight boost (`boostEv`) and the enlarger diffusion split print path:** were not ported at this point (frames fell back to the simulator); ported in section 22.

## 19. GPU executor on the Plan, selection in the session, GLES workgroups, external-image seam (GPU executor engineer, 2026-11-12)

Status: **landed** (desktop: RTX 4060 Ti Vulkan, SwiftShader Vulkan, GLES 3.1 on ANGLE over Vulkan and over SwiftShader; Android x86_64 NDK build compiles). `core/gpu` no longer has a renderer of its own: the GPU path is `Planner::plan` -> `GpuExecutor::run` -> `gpuImplFor(type)->recordPorts`.

### 19.1 What landed

| Piece | Where |
|---|---|
| `gpu::Executor` (`IGpuExecutor`): walks `Plan::stages`. `HostTable` = device tables from the session's bake; `TileSweep` = fused PointChain (one `recordPoint` for `[Resize|Geometry] Gain ToneColour Vignette`) + cumulative Window groups (`NRLuma NRChroma Dehaze Detail`, `Grain` = epilogue of the last pass, `Pack8` = the RGBA8 output); `GlobalSolve` = dehaze / clarity solves recorded into the same graph (Whole) or into their own graph over the 1/4 grids the tile sweeps accumulated (TiledExport). A single-tile run keeps `D` between sweeps and records only the groups a sweep adds (`ran`). Anything else (`ToneColourExact`, `DisplayStages`, `Blend`, `FilmFrame`, `Source` as a head) makes the plan `Unsupported` | `core/engine/exec/gpu_executor.{h,cpp}` |
| `gpu::Context`: Vulkan -> GLES 3.1 device selection (`hal::selectDevice`), start-up self-test that runs the whole engine (Planner + executor through an `EngineSession`) against the CPU reference plus the box self-test, device rebuild (epoch, retired devices kept alive), `GpuProvider` | `exec/gpu_context.cpp`, `exec/gpu_iface.h`, `exec/gpu_runtime.h` |
| Executor selection per frame in `EngineSession` (replaces `FrameJob::gpuHook`, `core/gpu/src/hook.cpp`, `gpu.h`, `gpu_hook.h`, `exec/gpu_frame.{h,cpp}`): `renderFull` -> `renderGpu` -> `runOnGpu` | `exec/engine_session.{h,cpp}` |
| Source residency on `SourceStore` (`MemorySourceStore` now; the store is `window(x, y, w, h, pitch)` so a file backed store can stream tiles): the device copy (RGBA16F + resized variants) is shared by every executor of a context that reads the same store identity, so a frame session and its export session upload a 24 MP source once | `exec/source_store.h`, `ResidentSource` in `gpu_executor.cpp` |
| Device tables (lattice + overflow lattice + tone table) on a `ResultCache` (`ColourLattice` slot, key = hash of `BakedLut::key`, limit 2, payload destroyed on eviction after a `waitIdle`); the overflow lattice is a `HostTable` payload now (`LatticeOverflow` node, requested through `FrameRequest::wantOverflow` whenever a GPU executor is in play) instead of being recomputed from the source in the executor | `gpu_executor.cpp`, `engine_session.cpp` |
| GPU frame / export entry points on the engine API: `FrameJob::{store, resizeSharpen, srcLongEdge, gpuTarget}`, `FrameResult::{executor, external, gpu}`, `EngineSession::{exportGpu, useGpuExecutor, injectGpuFailure}`, `engineInUse()`, `gpuCounters()`; `FramePlan::resizeSharpen` (additive; makes the graph contain the `Detail` node for the output sharpening of a downscaled export) | `engine_session.h`, `dapple/frame_plan.h`, `graph/builder.cpp` |
| C API: frames and exports go through the engine whenever `dapple_gpu_set_enabled` / `DAPPLE_GPU` is on (a GPU session always runs the Full engine, whatever `DAPPLE_ENGINE` says); `gpuExport` = `EngineSession::exportGpu` on the export session | `core/src/c_api.cpp` |
| GLES workgroup patch (`docs/engine/gles-shader-patches.md` section 2): spec constants 16 / 17 (2D kernels), 18 / 19 (reduce), group counts from the recorder | shaders, `nodes/gpu_common.*`, `nodes/*/gpu.cpp`, `film_gpu.cpp` |
| GLES 3.1 is a production fallback: `dapple_gpu` links `dapple_hal_gles` (`-DDAPPLE_GLES=ON`) and `hal/select.cpp`; the test shim (`gles_as_vulkan.cpp`, `dapple_gpu_gles`, `dapple_gles`) is gone, the GLES ctest entries run `dapple_gpu_tests` / `dapple_gpu_engine_tests` with `DAPPLE_GPU_API=gles` | `core/gpu/CMakeLists.txt`, `hal/gles/gles_gpu.cmake` |
| Zero-readback seam: `hal::NativeHandles`, `hal::ExternalImage`, `CommandList::copyImageToExternal`, `DeviceOptions::presentExtensions` (VK_KHR_surface / android_surface / swapchain, universal queue family), `GpuTarget::external` / `FrameJob::gpuTarget`; API and limits in `docs/engine/present.md` | `hal/hal.h`, `hal/vk/vk_device.cpp`, `exec/gpu_scheduler.*` |
| Tests / bench ported to the engine API (`core/gpu/src/gpu_rig.h` = one photo on an `EngineSession` with a forced executor): same parity cases, same numbers | `core/gpu/tests/gpu_tests.cpp`, `tests/engine/gpu/gpu_engine_tests.cpp`, `core/gpu/src/gpu_bench.cpp` |

Selection (5.9) as implemented in `EngineSession::runOnGpu`:

| Result of `IGpuExecutor::run` | Session behaviour |
|---|---|
| Ok | frame is the GPU's; `FrameResult::executor` = `vulkan` / `gles31` |
| Unsupported (node without a GPU impl, film frame, image larger than the device, source too large to be resident) | this frame on the CPU executor, GPU stays; `gpuUnsupported` / `fallbacks` counted. No per-`(structureHash, caps)` result cache: planning is 20-90 us, so the check is redone |
| Transient | `release()` the executor's device copies, retry once on the GPU, else this frame on the CPU; `transients` counted |
| DeviceLost | `GpuProvider::recover`: rebuild the device once (executors re-create the source and tables from `SourceStore` + HostTable payloads after the epoch changed); this frame on the CPU. A failed rebuild or a second loss gives the API up and the **next** API (Vulkan -> GLES 3.1) takes over, then the CPU for the rest of the process; `deviceLost` / `demotions` counted |
| Cancelled | `dapple_last_error() == "cancelled"`, nothing stored |

### 19.2 Gates (measured on the desktop with the RTX 4060 Ti, llvm-mingw clang -O3; other engineers' builds and test runs were using the machine)

| Gate | Result |
|---|---|
| GPU-vs-CPU parity, `dapple_gpu_tests` (22 stages x 2 sizes, downscale, error budget, tiled seams, streamed source, C API, steady state, blocklist) on **Vulkan RTX 4060 Ti** | every row identical to the phase-1 numbers (diff of the two outputs: only the banner, wall times and the status line differ): stage mean dE2000 0.035 - 0.073, max <= 1.9; Lanczos downscale mean 0.040 - 0.043; the same pass counts (e.g. 4608 passes for the 1500x1000 picture at tile 128) |
| same on **SwiftShader Vulkan** | identical rows |
| same on **GLES 3.1** (ANGLE over Vulkan RTX, ANGLE over SwiftShader), `DAPPLE_GPU_API=gles` | passes (self-test `point+geometry mean dE 0.040, spatial 0.076, box scan ok`); with `DAPPLE_GPU_WG=128` (16 x 8 / reduce 128) the stage run passes on both |
| tiled export seams (tile 128 / 256 / 400 / 1024 / 512 / 1000 / 2048, resident and streamed source, 1500x1000 and 4000x2667, crop + straighten + rot90 + downscale 900) | **0 differing pixels** in every row (Vulkan, SwiftShader) |
| zero-readback seam (`engine.gpu_external`, Vulkan RTX and SwiftShader) | external image bit-identical to the readback frame (0 of 307200 bytes) |
| selection with injected failures (`engine.gpu_select`) | unsupported / transient (retry once) / device lost (real rebuild, `rebuilds:1`) / second loss -> next backend or CPU, as the table above |
| `gpu-bench` steady-state frame wall ms (source resident; phase-1 build / this build; 1600 px, median of 3 alternating runs of 41 frames) | point only 2.14 / 2.07, interactive 2.69 / 2.70, full quality 3.02 / 3.06 |
| `gpu-bench` 26.2 MP full resolution (9 frames) | point only 35.8 / 34.1, interactive 45.2 / 44.9, full quality 50.7 / 50.2; preview 1600 px from the resident source 2.59 / 2.60 |
| `gpu-bench` tiled export of the 26.2 MP picture (tile 2048) | streamed 573 / 584 ms wall (GPU 175 / 176 ms), resident 55 / 56 ms (GPU 46 / 46), 576 passes, 564 barriers both; tiled vs single frame 0 differing pixels; tiled GPU vs CPU full frame dE2000 mean 0.0904, max 3.17 both |
| ctest (82 tests, `build/gx`: GPU + GLES on) | 78 pass. The 4 failures are `engine.golden_{off,legacy,tables,full}`: all four modes mismatch the same 9 `film/*` entries (`full_*`, `blend50_*`, `exact_blend`), i.e. the film engineer's uncommitted key-preserving exposure change (section 18) without a re-baseline; the 139 non-film frames match in all four modes, and `off` (which does not touch the engine) fails identically |

Not slower: all steady-state numbers are within +-2 % between the builds. The first frame of a photo is not comparable (the bench's `first` now includes the engine's table bake, which used to be timed outside). The plan + graph per frame (20-90 us on the CPU side, 17-29 us in the CPU engineer's measurement) is not part of the executor's `wallMs`.

### 19.3 Differences from phase 1 (intentional)

1. A GPU session runs the Full engine: the lattice is baked by the table nodes (golden-equal to `Renderer::bake`), not by the C layer; with `DAPPLE_GPU` on and `DAPPLE_ENGINE` unset, CPU fallback frames are `CpuExecutor` frames (byte-identical to `renderBakedRgba` by gate B), and the first frame of a photo includes the bake.
2. The lattice cache is the executor's `ResultCache` (FIFO of 2, was LRU of 2); the overflow lattice comes from the session's `LatticeOverflow` payload.
3. A transient failure is retried once on the GPU (5.9) instead of going straight to the CPU; the device-lost semantics are unchanged for the first loss, the second loss now tries GLES 3.1 before giving up.
4. `ToneColourExact` (the exact graph) has no GPU implementation: `renderExact` stays on the CPU executor, only the baked pipeline runs on the GPU (as in phase 1).

### 19.4 Not done / not verified

* **Film frames** were `Unsupported` on the GPU in this period; since section 20 they run on the GPU executor (whole frames; film export tiles, `Blend` and display curves stay on the CPU).
* No on-device measurement in this period: Pixel 5 / N5 / the phones were not touched (box-scan threshold, vertical scan on Adreno, 24 MP submission splitting, export tile 1024, DeviceLost on a real driver, a real GLES 3.1 device that reports 128 invocations). The Android x86_64 NDK build of `dapple_gpu` (+ GLES, + the tests) compiles; arm64 / armv7 were not built here.
* The external-image seam is verified with a stand-in image and raw semaphores, not with a swapchain; see `docs/engine/present.md` for what a presenter must still do (surface, swapchain, queue-family ownership if the presenting family differs).
* `PlanCache`: none. The GPU plan is made without the session `ResultCache` (cached solves are CPU payloads); a device-side cache of the dehaze / guided solves keyed by the node keys (so a pure exposure drag skips the analysis sweep on the GPU, as on the CPU) is not implemented: every GPU frame redoes the 1/4-grid analysis passes.
* Mixed execution (GPU nodes with a CPU node in the middle) is still deferred (5.9).
* `NodeDesc::gpuImpl` in the registry is not updated (the executor asks `gpuImplFor`).

## 20. Film frames on the GPU executor, batched mixtures, half-precision planes (film GPU engineer, 2026-11-12)

### 20.1 Wiring

* `Renderer::framePlan` no longer rejects a film frame: for a film at strength 100 (and `DAPPLE_FILM_NODES` on) it fills `FramePlan::film` (stock, paper, `film_exp + filmKeyEv`, warmth, tint, grain / halation percent, `formatScale`, tier, `keepGrain`, `fullLongEdge`, `frameIndex`, and whether DisplayStages are the identity) and resolves the neighbourhood stages the way `FrameKernels` does (a film at full strength brings its own grain: look `std`). Strength < 100 (needs the normal branch for `Blend`) is still `supported = false`. `exportGpu` returns 1 for a film frame (no tiled film yet).
* `gpu::Executor`: `validate` accepts a plan with a `FilmFrame` stage when `FramePlan::film.active`, the display stages are the identity, the plan is Whole and there is no tile sink. The film arguments are resolved from the FramePlan (`filmFrameResolved`). A film chain with an effect the GPU nodes do not port (`filmGpuSupported`: wide dye-cloud blur, grain micro-structure; highlight boost and enlarger diffusion are ported, section 22) is `Unsupported` before anything is recorded (CPU for that frame, the GPU stays). A frame larger than 288 B/px x 45 % of the device-local heap is `Unsupported` too.
* The walk: `FilmFrame` -> `Walk::filmStage` (film input = the resident source, or the new `film_src` pass for geometry / region frames; then `recordFilmFrame`). The following `TileSweep`'s point chain (DisplayStages + Vignette) is one `film_fin` pass (vignette, clamp, grain epilogue, RGBA16F working image or RGBA8). Window nodes (NR, dehaze, detail, grain) are unchanged. The device copy of the film tables (raw LUT, print LUT, curves, sRGB decode, grain sampler tables) is cached per executor (`DeviceFilm`) while the CPU bake's payload objects stay the same, and dropped on release / device rebuild. The colour lattice is not uploaded for a film frame.
* New shared-header fields (additive): `FramePlan::film`, `SlotRes::taps`, `GpuRecorder::taps / tapNext`.

### 20.2 Pass count and speed work

| Change | Where |
|---|---|
| **Batched mixtures.** The three channels go through one RGBA plane; a pass filters up to four component slots of all three channels (`film_h3`: one source read, four outputs; `film_v3`: sums them with the accumulator and folds in `levelCombine`'s base pass and the Catmull-Rom upsample). The channel split / merge passes are gone. Tap tables live in a per-slot host-visible buffer (`taps`; the parameter block is limited to 256 B) | `film_mix3.comp`, `Mix3` in `film_gpu.cpp` |
| **Shared-memory vertical pass**: a 16-column tile of each component plane (rows + 2 r) is staged in shared memory (13.8 KB, inside the 16 KB GLES 3.1 minimum); the 2 r + 1 taps cost shared reads instead of texture fetches | `film_v3` |
| **Fusions**: FilmPrintScan + sRGB decode + glare add are one pass (`film_pd`; the glare field is computed first). The vertical unsharp blur and the encode are one pass (`film_f4ve`) | `film_point.comp` (`FUSED_POST`), `film_post.comp` MODE 6 |
| **Half-precision planes**: every intermediate plane of the film (E, E2, q, D, grain, linear light, mixture temporaries) is RGBA16F instead of RGBA32F, stored through the exact round-to-nearest-even `h16v` (the passes are bandwidth bound). The raw LUT, print LUT, curves and the glare field stay 32-bit | all `film_*.comp` |

Not done: the develop pass is not folded into the grain pass (the 256 B parameter block and duplicate curve code made it a separate step), the DIR half-resolution input path and the glare-field blur still use the older single-plane passes.

### 20.3 Measurements (desktop, RTX 4060 Ti, Vulkan, `dapple_film_gpu_tests bench`, GPU timestamps; the bench also copies the display image back: 27 MB before, 13.6 MB after, shown separately)

| Case (Portra 400) | Before (ms) | After (ms) | After without the readback copy |
|---|---|---|---|
| 1600 px final tier, all effects | 12.33 (readback 2.14), 10.2 compute | 3.83 (readback 1.12) | **2.7** |
| 1600 px interactive | 8.00 | 2.65 | 1.5 |
| 1600 px interactive, grain drag | 8.66 | 2.92 | 1.8 |
| 1080 px interactive | 3.01 | 1.33 | 0.9 |
| 3600 x 2400 final tier | 59.83 | 19.48 | about 17 |

Passes of a 1536 x 1024 film frame through `EngineSession` + `GpuExecutor` (film chain, finish, windows): 109 -> 36 (Portra, all effects), 29 for a slide without grain, 14 interactive. Executor frame GPU time (`film_gpu.frames`, 8-bit readback copy included): Portra all effects 3.3 ms, slide without grain 3.0, interactive 2.4. The target (1600 px film all effects <= 3 ms GPU time on the desktop) is met for the film chain (2.7 ms); the whole executor frame is 3.3-3.5 ms. The largest remaining cost is `film_v3` (about 1.1 ms of the 2.7).

### 20.4 Parity (all pass)

* `film_gpu.parity` (per node against the CPU nodes, 960 x 640): point chain mean 0.015-0.017 / p99 <= 0.055 (budget 0.1 / 1.0); spatial, grain off: mean 0.018-0.036 / p99 <= 0.11 / max <= 0.33 (budget 0.15 / 1.5); grain after a 4x box downscale: mean 0.007-0.009 (budget 0.3); grain per pixel: mean 0.033 / p99 0.10 (0.1 / 1.0); scale-aware grain drag: 0.026 / 0.08. Mixtures against the CPU engine: max abs difference 0.044-0.093 % of the dynamic range (budget 0.5 %).
* `film_gpu.frames` (whole frames through the session against the CPU engine render of the same job, 8-bit output, dE2000): Portra all effects 0.071 mean / 0.87 p99 / 1.85 max at 1536 x 1024; slide without grain 0.061 / 0.84; geometry + vignette 0.071 / 0.85; NR + clarity + sharpen 0.074 / 0.86; interactive 0.050 / 0.83; interactive grain drag 0.070 / 0.84. 12-17 % of the bytes differ by one level (8-bit rounding).
* The half-precision planes cost 0.01-0.04 mean dE2000 against the 32-bit version of the same passes (before: 0.0000-0.012 in `film_gpu.parity`, after: 0.007-0.036).
* `gpu_parity`, `gpu_parity_swiftshader`, `engine.gpu_*`, `film_gpu.parity`, `film_gpu.frames`, `engine.exec_*`, `engine.keys`, `engine.conformance`: pass. The graph snapshot `film_strength_100` gained `NRChroma` and `Detail` (the film frame plan now carries the neighbourhood stages of the edit, like the CPU `FrameKernels` plan): snapshot file updated.

### 20.5 Not done / not verified

* No tiled export of a film frame on the GPU (film planes are Whole). Strength < 100 (`Blend`), display curves and the look LUT after a film are CPU.
* GLES 3.1: `film_gpu.frames` passes on the GLES 3.1 backend (ANGLE over Vulkan on the RTX, `DAPPLE_GPU_API=gles`) with the same numbers as Vulkan, also with `DAPPLE_GPU_WG=128` (16 x 8 groups). The vertical pass needs the workgroup shape from the parameter block (GLES translation cannot constant-fold `gl_WorkGroupSize`). Not run on a real ES device or on SwiftShader-GLES.
* No device measurement (Find N5, Pixel 5, phones). The N5 GPU is expected to be about 10x slower on these kernels (find-n5.md); the bandwidth work helps there most, but the 120 Hz budget is not shown. No Android build of the new kernels.
* `film_v3` is still about 40 % of the film time; the develop pass is not fused into the grain pass.


---

## 21. Wave 4: the remaining features move onto the engine, the old Renderer becomes the oracle (migration engineer, 2026-11-12)

Owner ruling: no CPU-fallback to the old pipeline; the CPU fallback is the new architecture (CpuExecutor on the same Plan); the old kernels stay until everything is migrated and signed off on devices, then they are deleted. This section reports the 10.7 checklist item by item.

### 21.1 What landed

| 10.7 / wave-4 item | State | Where |
|---|---|---|
| Look previews, tone previews, HDR merge output, panorama output, thumbnails | **done** (they were already `renderSessionParams` -> `engineRenderExact`; the remaining direct `Renderer` calls moved) | the C layer's scaled copy now comes from the engine; `developer.cpp`, `cli/main.cpp bench`, `cli/merge_cmds.cpp`, `cli/eval.cpp`, `cli/fit_profile.cpp`, `film_swatch.cpp`, `dapple_film_swatches_json`, `analyzeTone` / `applyFilmHistogram` (`tone.cpp`) call `engine::renderImage8` / `renderImageF` |
| `scaledSource` into the Resize node | **done** | `CpuImpl::bake` of `Resize` (nodes/main/cpu_nodes.cpp), `EngineSession::resizedSource` (cached in the `ResultCache` under `NodeType::Resize`, LRU, 4 copies / 160 MB as before; exports and previews do not keep a copy above 64 MB). `scaledSource` and `DappleSession::scaled` are gone from `c_api.cpp` |
| Table nodes bake through `CpuImpl::bake` | **done** | `ColourLattice, ToneAnalysisPre, ToneTable, Analysis, LatticeOverflow` have `bake` + `freePayload` in `cpu_nodes.cpp` (`TableBakeCtx` is the `resolved` block); `bakeNode()` runs them; `EngineSession::runHostTables` is the HostTable stage (the cache and key logic of the old `bakeTables`, evaluation only through `bakeNode`). `bakeLegacy`, `bakeTables`, `Mode::LegacyBakes`, `Mode::Tables`, `bakes_` are deleted; `Mode` is `{Off, Full}`; `DAPPLE_ENGINE=legacy|tables` now mean `full`; `dapple_golden` / ctest modes are `off`, `full` |
| No `Renderer::render` fallback inside the engine for non-film frames | **done**, with a marked seam for film | `renderFrame` / `renderExact`: a plan no executor takes is an error (`engine: plan not supported ...`) unless the edit has a film (`Renderer::filmActive()`): `SEAM(film engineer)` in `engine_session.cpp` (two places) keeps the old renderer for film boost / enlarger diffusion / rejected film parameters. The GPU self-test (`gpu_context.cpp`) uses the CpuExecutor as its reference instead of `renderBakedRgba` |
| Off-vs-full self-check that runs on any arch | **done** | `dapple_golden selfcheck [film.dfp]` (ctest `engine.selfcheck`): 148 session frames off vs full bit for bit on this machine, plus `renderImage8` / `renderImageF` (spatial on / off) against `Renderer::render` / `renderFloat` for 7 parameter sets (film and blend included) and the Resize node |
| Dart: `dapple_view_submit` honours `active_param`; cancel binding test | **done** | `view.cpp` (`Snapshot::activeParam`, size-versioned like `dapple_session_render_frame`), `render_loop.h`, `GpuViewBackend.submit(activeParam:)`, `EditorEngine` passes `_activeParam` for interactive frames, `info_json` reports `activeParam` (tested in `present.loop_view`); `app/test/native_cancel_test.dart` (guards always, symbol binding with `DAPPLE_LIB=<host libdapple>`); `DappleBindings.openLibrary` honours `DAPPLE_LIB` on desktop |

New engine entry points: `engine::renderImage8(params, camera, lut, src, out)` and `engine::renderImageF(..., spatial, out)` (a private `EngineSession`, the exact variant through Planner + CpuExecutor, 16 MP and up in tiles); `ExecEnv::outF` is the float output of the CpuExecutor (Whole plans: the clamped, display-encoded frame; analysis, swatches, reference renders).

### 21.2 Gates (desktop, `build/w4-migrate`, llvm-mingw clang -O3, other engineers building at the same time)

* `dapple_golden verify corpus.txt off full`: 148 frames, 0 mismatches in both modes (the film lines in the corpus were being re-recorded by the film engineer while this ran; one earlier run showed the 9 film lines of their in-flight change failing in `off` and `full` alike).
* `dapple_golden selfcheck`: 148 frames off vs full, 0 mismatches; 21 one-shot comparisons and the Resize node identical (bit-exact floats and bytes). Same result on the Android x86_64 emulator (`avx2=0` kernels, emulator started on port 5580, stopped afterwards): `SELFCHECK OK (0)`; `dapple_exec_tests whole tiles plan cancel analysis` pass there too.
* ctest on the desktop: 69 of 71 pass; `film_gpu.parity` and `film_gpu.frames` fail with `parameter block too large` from the film GPU engineer's in-flight change (`vk_device.cpp` UBO range), not from this work.
* `engine.keys`, `engine.conformance` (which now evaluates the tables through `bakeNode`), `engine.exec_*`, `engine.reuse` (off vs engine, reuse decisions and pixels identical frame by frame), `present.loop_view`: pass.
* arm64-v8a: `dapple_golden`, `dapple_exec_tests`, `dapple_engine_tests` build (NDK 28, `build/w4-migrate-droid-arm64-v8a`). **Not run**: no arm64 device was assigned to this work.

### 21.3 What can be deleted after device sign-off (nothing is deleted yet)

1. `Renderer::render`, `renderFloat`, `renderBakedRgba`, `renderBakedFloat`, `renderBakedImpl` and the per-pixel loops in `renderer.cpp` that are not `FrameKernels` (the kernels `FrameKernels` wraps - `PointOp`, `LutSampler`, `makePointOp` - stay and move out of `renderer.cpp`, 10.7 item 5).
2. In `c_api.cpp`: every `Mode::Off` branch (`renderSessionParams` / `renderFrame` exact and baked fallbacks, `DappleSession::bakes`, the `#f` / `#x` key lookup, `lastParams` bookkeeping that only the old path needs) and `Renderer::bake()`'s `#f` / `#x` key. The legacy keys could not be deleted yet: the Off path still uses them and it is the golden oracle.
3. `Mode::Off`, `DAPPLE_ENGINE=off`, the `engine.golden_off` and `engine.reuse` comparisons (the corpus stays the oracle), `GraphDetail::Legacy` and the `LegacyRender` node type (it is still the table graph of `renderFrame` and the cache type of the assembled bake).
4. The two `SEAM(film engineer)` fallbacks and `Renderer::filmActive()`'s use in the engine (the film nodes cover boost / enlarger diffusion since section 22; the seams now only guard an unknown stock / paper or a failed table bake).
5. Callers that still use the old renderer as a reference or benchmark and must move to the engine (or be deleted) first: `cli/edit_bench.cpp`, `cli/film_bench.cpp`, `core/gpu/src/gpu_bench.cpp`, `core/gpu/tests/gpu_tests.cpp`, `core/gpu/src/gpu_rig.h` users, `tests/core_tests.cpp`, `tests/engine/golden` (`runCoreApi`), `cli/film_cmds.cpp` (film engineer's file, not touched here; `core/src/film_match.cpp` / `film_match_cache.cpp` moved to the engine in the bands round, `film_match_render.h`), `Renderer::bake*` pieces used by the Off path.

### 21.4 Not done / not verified

* The old renderer is still the implementation of `DAPPLE_ENGINE=off`, of the film seam, and of the callers in 21.3 item 5. `film_match.cpp` / `film_match_cache.cpp` (film exposure match) now render their proxy through `renderMatchProxy` (`core/src/film_match_render.h`: `EngineSession::renderExact` with `outFloat`, exact variant, the proxy as tone reference); `DAPPLE_ENGINE=off` keeps `Renderer::renderFloat` there as the oracle (same split as `tone.cpp`).
* The `Resize` node is a frame-global `bake` that produces the scaled source; the executor's `Resize` block fragment still copies rows of the (already scaled) source, there is no streaming Lanczos tile node yet.
* No device run of anything here (no Pixel 5, no Find N5, no arm64 execution); no performance measurement of this wave (edit-bench not re-run; the golden run time and `planUsPerFrame` are unchanged, 10-12 us). Tone-analysis proxies (256 px) now take the 50-100 us graph + plan overhead on top of the render.
* No Dart test drives a real session's `cancel()` (a session needs a photo); the test covers the guards and the symbol binding. The `GpuViewBackend.submit(activeParam:)` path runs only on Android (not exercised on a device); the native side is covered by `present.loop_view`.
* `tools/check-all.sh --android` (APK builds) was not run.

## 22. Film highlight boost and enlarger diffusion as nodes (film engineer, 2026-11-12, wave 4)

Owner ruling: no film frame may need the old simulator. The two film effects that existed only in `FilmSimulator` (highlight boost `boostEv`, the enlarger diffusion split print path) are nodes of the engine now, on the CPU and on the GPU (Vulkan and GLES 3.1). The edit schema still does not expose them (`EditParams` has no field for either; the app never sets them): they travel in `FilmFx` (`film_nodes.h`), a POD in `FilmResolved::fx` and `FilmFrameArgs::fx` (default = off, hashed into the film plan key and the print-table key).

### 22.1 What landed

| Piece | Where |
|---|---|
| **Highlight boost** (spec 03 s4.1): the maximum X of the whole exposure frame, then `v > x0: v + X k (exp(a d) - a d - 1)`, before the camera / halation mixtures. CPU: part of `FilmBlurExposure` (identity only without boost *and* mixtures); the boosted copy replaces the mixtures' input, the input planes are not touched. GPU: `film_boost.comp` (3 passes, no readback: per-group maximum, one group for the maximum of the partials and `boostConstants` in float with a series for `exp(t) - t - 1` below 0.25, then the curve; the constants live in `g[8..12]` of the slot's dehaze statistics buffer, the partials in its reduction buffer) | `film_cpu.cpp` `runBlurExposure`, `film_gpu.cpp` `recordBoost`, `shaders/film_boost.comp` |
| **Enlarger diffusion** (spec 03 s6.1): two new node types `FilmPrintExposure` (P: film density -> `10^(lut1)` linear paper exposure, frame-sized planar planes) and `FilmEnlargerBlur` (G/W: the filter mixtures `pl.enl[c]`), both identity without the filter; `FilmPrintScan` reads the filtered exposure when the filter is on (`log10`, paper curves, scan LUT `lut3` instead of the density LUT) with the reflect boundary of the frame. The builder always creates the two nodes (`FilmGrain -> FilmPrintExposure -> FilmEnlargerBlur -> FilmPrintScan`); the planner puts them in the Film group. The reference driver `runFilmFrame` runs two tile passes (develop + grain + paper exposure, then paper scan + glare + scan) around the blur. GPU: `film_point.comp` MODE 6 (`exp2(3.3219 * lut1)`) and MODE 7 (`log10 -> paper curve -> lut3`, also fused with the decode + glare add), the existing batched mixture passes for the blur; `uploadGpuTables` uploads `lut1`, `lut3` and the paper curves when the print table is a split table | `film_cpu.cpp` `runPrintExposure / runEnlargerBlur / runPrintScan`, `film_frame.cpp`, `film_gpu.cpp`, `shaders/film_point.comp`, `graph/{types.h,builder.cpp,registry.cpp}`, `plan/planner.cpp` |
| The split-path tables are baked only for a print with the filter on (`ModelEntry::split`, print-table cache key); spatial effects off (`quality 0`) drops the enlarger filter exactly like the simulator, a slide (no print) ignores it | `film_tables.cpp` |
| The `falls back to the simulator` branches for these two effects are gone: `runFilmFrame`, `runExposure`, `runPrintScan`, `filmGpuSupported`, `recordFilmFrame` no longer reject `pd->split` / `plan.boost` | |

### 22.2 Gates (desktop, `build/w4-filmfx`, RTX 4060 Ti, llvm-mingw clang -O3, other engineers building at the same time)

* `film_nodes.fx` (new, 19 cases: boost on print / slide / interactive / quality 0 / no halation / no grain / 33 x 17 / knee never reached, all four filter families, strong x2 scale, interactive, no grain, cine + 2383, slide (ignored), quality 0 (dropped), both together): the node chain is **byte-identical** with `FilmSimulator::render` (0 differing floats in every case), `renderFilmFrame` (the Renderer-facing entry; true = no simulator) is byte-identical too, the effect is visible against the same frame without it (1 683 to 320 000 differing floats; 0 where the model drops it on purpose), and FNV hashes of the output are checked against `tests/engine/golden/film_fx.txt` (re-record: `DAPPLE_FX_RECORD=1 dapple_film_nodes_tests fx`; skipped with a message on a machine whose kernel set differs). `film_nodes.equiv / rebake / keys / graph / concurrent / grain_*` and `engine.golden_*`, `engine.keys`, `engine.conformance`, `engine.snapshots`, `engine.exec_*`, `engine.reuse` still pass.
* `film_gpu.parity` (per film frame against the CPU nodes, 960 x 640, 11 new cases, budgets 0.15 mean / 1.5 p99 without grain, 0.3 / 3.0 after a 4x box with grain): boost 0.022-0.036 mean / <= 0.114 p99 / <= 0.33 max; enlarger 0.021-0.037 mean / <= 0.117 p99 / <= 0.38 max; boost + enlarger 0.032 / 0.101 / 0.30; with grain (4x box) 0.009-0.010 mean. Each case also renders the GPU frame without the effect: the effect is visible on the device (mean 0.12-1.14, max 3.0-26 dE2000) and absent where it must be (knee not reached, slide, quality 0: max 0.000). `filmGpuSupported` is true for all of them. The same numbers on **Vulkan**, **GLES 3.1 over ANGLE-Vulkan** (also `DAPPLE_GPU_WG=128`) and **GLES 3.1 over ANGLE-SwiftShader** (`DAPPLE_GPU_API=gles`, the test now selects the backend like the engine does and prints `backend:`).
* Cost (`dapple_film_gpu_tests bench`, Portra 400 final tier at 1600 x 1067, GPU timestamps, median of 10, two runs, Vulkan on the RTX 4060 Ti, machine shared with other builds): all effects 4.14 / 4.21 ms; + highlight boost 4.53 / 4.15 ms (no measurable cost, within the noise); + enlarger diffusion 7.29 / 7.51 ms (+3.1 to +3.4 ms: one point pass, the 27-component batched mixture, the paper scan). Not benchmarked on a phone or under GLES.

### 22.3 What the executors must call (GPU executor engineer)

* `filmFrameResolved(FilmFrameArgs)` must copy `fx` from wherever the FramePlan carries it (today nothing does: the edit schema has no field, `FramePlan::film` has no `fx`; add `FilmFx fx` to `FramePlan::film` when the product exposes either effect, `filmResolve` in `gpu_executor.cpp` then only sets `a.fx`).
* `uploadGpuTables` does the right thing on its own (`GpuFilmTables::split`); the executor's `DeviceFilm` cache key must include the split flag if it keys on more than the CPU payload objects (it keys on the payload objects, and a print table with split tables is a different payload).
* `recordFilmFrame` needs `GpuRecorder::gBuf / partBuf` set (the executor does; the film parity test now sets them) for the boost.
* The enlarger filter is a whole-frame operation (the mixtures have no window form): `filmWindowable()` is false for it (already so in the windowed film code), tiled export of such a frame stays on the CPU executor, which is the new architecture too.
* Extra device memory per frame with the filter: two RGBA16F planes (paper exposure, filtered exposure) = 16 B/px on top of the 288 B/px estimate; with the boost: one more RGBA16F plane (8 B/px).

### 22.4 Not done / not verified

* Neither effect is reachable from the app or from the editor schema (by decision: nobody asked for UI); `film_bench` / CLI still build `FilmParams` directly. No `EditParams` field, no ParamSet key (`resolveFilm` always leaves `fx` off), therefore the executor-level `film_gpu.frames` (whole frames through `EngineSession`) cannot exercise them; the per-node `film_gpu.parity` cases above do.
* The two `SEAM(film engineer)` fallbacks in `engine_session.cpp` are still there: they now only guard a stock / paper that is not in the library or a failed table bake. Their comments were updated, the branches were not removed (they are the migration engineer's file and the old Renderer is still the oracle until device sign-off).
* No tiled (windowed) enlarger filter on the GPU; no Android build of the new shader (`film_boost.comp` is in both shader lists, glslc / SPIRV-Cross / ESSL lint ran in the desktop GLES build only); no device run (Pixel 5, Find N5 not touched). The golden hashes of `film_fx.txt` are for the AVX2 kernel set only.
* One `film_gpu.frames` run died with a SEGFAULT when ctest ran it in parallel (`-j 3`) with the other GPU suites right after a build; five reruns (alone and with `-j 3`) passed. Cause not found.

## 23. Every film frame on the GPU: Blend, display stages, tiled export, exact variant, dye-cloud blur, solve cache (GPU film-coverage engineer, 2026-11-12, wave 4)

Owner ruling: a film frame must not fall back to the CPU. Before this section `gpu::Executor::validate` returned `Unsupported` for (1) film strength < 100 (Blend), (2) display curves / look LUT after the film, (3) a tiled export of a film frame, (4) `ToneColourExact` (so `renderExact` was CPU), (5) the dye-cloud blur and the grain micro-structure. All five run on the device now.

### 23.1 What landed

| Piece | Where |
|---|---|
| **Blend**: the normal branch (Gain + ToneColour(Exact) through the lattice, no vignette) is recorded as its own point pass; `film_fin` mixes `main + s (film - main)` after the display stages, then vignette, clamp, grain epilogue. `Renderer::framePlan` no longer refuses strength < 1 (`FramePlan::film.strength`). Strength 0 has no film nodes: the normal pipeline. | `gpu_executor.cpp` (`Walk::mainBranch`, `filmFinish`), `shaders/film_fin.comp`, `renderer.cpp` |
| **DisplayStages after the film**: the four 1024-sample curves ([master, r, g, b], identity ones not stored) and the look LUT travel in `FramePlan::display` (`Lut3D::table()/domainMin()/domainMax()` are new accessors); device copy `DeviceDisplay` (curves buffer + RGBA32F 3D image, LRU of 3, key = content), applied in `film_fin` with the CPU's curve interpolation and tetrahedral LUT (same tie rules). | `gpu_executor.cpp` (`ensureDisplay`), `film_fin.comp`, `frame_plan.h`, `lut.h` |
| **Tiled film export**: `PlanRequest::filmTiles` keeps `TiledExport` for a film frame (GPU only). The whole-frame part of the film (exposure, highlight boost, camera / halation mixtures, DIR field) runs once (`recordFilmPre`) into two persistent RGBA16F images (E2, q); each tile then runs the develop / grain / print / scan chain over its work rectangle grown by `filmWindowHalo` (= scan + unsharp + grain blur + dye / micro) and clipped to the frame (`recordFilmWindow`). Every pass of that chain takes the window origin (`film_point` `org`, `film_grain` density window + frame size, glare field origin), grain and glare noise are hashed on absolute coordinates, so a tile is bit-identical with the whole frame. `recordFilmFrame` is `Pre` + one window = the frame (unchanged results: `film_gpu.frames` numbers before/after identical). `exportGpu` no longer sends film to the CPU. | `film_gpu.cpp/.h`, `gpu_executor.cpp` (`runTiled`), `engine_session.cpp`, `planner.cpp`, `plan.h` |
| **ToneColourExact**: the GPU kernel is the lattice kernel of ToneColour. `EngineSession::renderExact` tries the GPU first (not for `outFloat` / session-cache calls): a 65^3 lattice + the 33^3 overflow lattice baked with the exact analysis (`Renderer::bake(src, 65)`), whole or tiled. It is NOT a per-pixel port of `PointOp::apply`: see 23.4. | `engine_session.cpp` (`renderExactGpu`), `gpu_nodes.cpp` |
| **Dye-cloud blur** (per sub-layer and channel kernels, then the sum) and **grain micro-structure** (blurred log-normal hash field multiplying the grain): `film_grain` writes one sub-layer per pass for the channels that have a dye blur (the other channels keep the merged sampler, as the CPU does), `film_dye.comp` does the per-channel FIR (taps from the slot's tap table, radius up to 255) with add / multiply accumulation. Visible from about 6000 px of 36 mm (r = 1), r = 2 at 61 MP pitch; micro-structure only below a 0.6 um pixel pitch. | `film_dye.comp`, `film_grain.comp`, `film_gpu.cpp` |
| **Device solve cache**: the dehaze dark channel and the clarity guided-filter coefficients of a whole-frame run stay on the device (`DeviceSolve`, `ResultCache solves_`, limit 2 per node type, key = the planner's node key). `IGpuExecutor::solveCache()` is what `renderGpu` plans against, so a hit also drops the accumulation sweep. | `gpu_executor.cpp`, `gpu_iface.h`, `engine_session.cpp` |
| **Device tables LRU** (`ResultCache::findLru`; the two latest lattices stay), **`NodeDesc::gpuImpl` removed** (the executor asks `gpuImplFor`). | `result_cache.h`, `graph.h`, `registry.cpp` |
| **Two latent bugs found by the coverage test**: `Renderer::bakeOverflow` returned an empty lattice for any film (now only when the lattice is a film-only placeholder, i.e. strength 100), and the `LatticeOverflow` node key carried `ctx.filmActive` instead of `ctx.filmOnly`, so an empty overflow cached at strength 100 poisoned every later frame of the session at strength < 100 (GPU error "overflow lattice unavailable"). | `renderer.cpp`, `registry.cpp` |

### 23.2 Gates (desktop, `build/w4-gpu`, RTX 4060 Ti, llvm-mingw clang -O3; other engineers were building on the same machine)

* `gpu_parity` (Vulkan RTX) and `gpu_parity_swiftshader`: PASSED. New film stage rows against the CPU baked path (640 x 448 and 1536 x 1024; budget mean <= 0.1, p99 <= 1.0 asserted): strength 60 / 0 / 30 (+ geometry, vignette, NR, sharpen), master + red curves, parametric curves, look LUT 70 %, strength 70 + curves + LUT + grain, strength 40 interactive + LUT: mean 0.040-0.081, p99 0.73-0.87.
* `gpu_tests --film-tiling`: 5 film cases (portra; ektar + NR / sharpen / grain / vignette; strength 55 + curves + LUT + straighten / crop; velvia + dehaze + clarity; strength 80 + rot90 + downscale 900) x tile sizes 128 / 256 / 400 / 1024 on 1500 x 1000 and 512 / 1000 / 2048 on 4000 x 2667 against the whole-frame GPU render: **0 differing pixels in all 35 runs** (also on SwiftShader at 1500 x 1000, GLES / ANGLE, `DAPPLE_GPU_WG=128`).
* `gpu_tests --exact`: `renderExact` on the GPU (forced executor, `gpuFrames` counted) against `Renderer::render`, 900 x 600, 7 cases (tone + colour, HSL + grading + curves, LUT + spatial, film, film strength 60 + curves, tiled variants): mean 0.068-0.090, p99 0.83-0.87, max 2.8 dE.
* `gpu_tests --coverage`: 112 combinations of film strength {0, 1, 25, 50, 75, 99, 100} x look LUT x curves x tiled x exact, plus all 20 film stocks x {interactive, final} x {strength 100, 45}: **192 / 192 rendered by the GPU executor** (the rig forces the executor, so any `Unsupported` is a failure). Same on GLES (ANGLE, WG 128) and SwiftShader.
* `gpu_tests --solve-cache`: dehaze + sharpen change: 24 -> 12 passes, pixels bit-identical to a session that solves; dehaze + clarity, same frame twice: 31 -> 12 passes, identical.
* `film_gpu.frames` (EngineSession -> Planner -> forced GPU executor against the CPU engine, 640 x 448 and 1536 x 1024): 9 new rows (strength 60 / 0 / 35, curves, parametric curves, LUT, strength 55 + curves + LUT, tiled export, tiled strength 50 + curves + LUT) all mean <= 0.0795 and p99 <= 0.85 (asserted <= 0.1 / 1.0). `film_gpu.parity`: dye rows at the 24 MP pitch mean 0.040 / 0.028 (portra / velvia slide) and p99 0.19 / 0.15 per pixel; 61 MP pitch (ektar) mean 0.049, p99 0.41; the extreme pitches (dye r about 12, micro-structure on) are compared after a 4x box like the other grain rows: mean 0.05-0.15 (budget 0.3). Isolated pixels reach 7-31 dE in the per-pixel dye rows (a Poisson count that flips between the CPU and the GPU float rounding); the p99 is what is budgeted.
* Backends run: Vulkan RTX; SwiftShader (`gpu_parity_swiftshader`; `film_gpu` parity + frames by hand with the ICD); GLES 3.1 on ANGLE-Vulkan and ANGLE-SwiftShader (`gles_gpu_parity_angle_*`, 4 / 4; `film_gpu` parity + frames by hand); each also with `DAPPLE_GPU_WG=128`.
* `engine.*`, `film_nodes.*`, `engine.exec_*`, `core_tests`: pass (the snapshot file changed: film strength 0 / 50 are GPU-supported frames now, so their graphs carry the NRChroma / Detail nodes; reviewed, written with `DAPPLE_UPDATE_SNAPSHOTS=1`).

### 23.3 Timings (`gpu_tests --film-timing 24mp`, wall time incl. readback / sink, medians; "before" = the CPU engine, which is what these frames used to fall back to; synthetic 6000 x 4000 test image, 8 tiles of 2048 px; the machine was shared, the CPU numbers moved by +-15 % between runs)

| Case | 1600 px frame: CPU -> GPU | 24 MP export: CPU -> GPU (warm / first run) |
|---|---|---|
| film strength 100 (was already GPU at 1600 px, export was CPU) | 26-28 ms -> 15.5-16.6 ms | 606-647 ms -> 134 ms (222 ms) |
| film strength 60 (Blend) | 27-30 ms -> 16.1-16.5 ms | 724-742 ms -> 139-144 ms (191 ms) |
| film + curves + look LUT | 30 ms -> 16.1-16.8 ms | 462 ms -> 137-141 ms (182 ms) |
| film strength 60 + curves + LUT | 31.5 ms -> 16.6-17.0 ms | 801-805 ms -> 139 ms (175-203 ms) |

The GPU frame time is the same for all four cases (the added work is one pass and a few texture reads). Peak device memory of the 24 MP export: about 2.7 GB for the process (nvidia-smi sampling, includes the resident source and the kept pool), about 110 B/px; the executor refuses (Unsupported) a tiled film export above 128 B/px x pixels > half the device memory and a whole film frame above 288 B/px x pixels > 45 %.

### 23.4 Not done / not verified

* **ToneColourExact is not exact on the GPU**: it is the lattice kernel on a 65^3 + 33^3 bake (error against the per-pixel CPU reference above: mean <= 0.09 dE). A true per-pixel port of `PointOp::apply` (OKLab, HSL bands, grading, style, colour match, tonal move, curves) was not written. `renderExact` calls that need a float result (`ExactOptions::outFloat`: analysis, swatches) and calls with `sessionCache` stay on the CPU by design.
* **Limits that still return Unsupported (CPU) for a film frame**: a tiled film export needs the (resized) source resident on the device and the 128 B/px bound above (a 61 MP film export on a device with less than about 16 GB of device memory is refused: measured nowhere but on this RTX); a whole film frame above the 288 B/px bound; the enlarger diffusion filter in a tiled export (a whole-frame blur between grain and print; not reachable from the app); a scan / unsharp / grain / glare kernel wider than 19 taps or a dye / micro kernel wider than 255 taps ("film GPU: blur kernel too wide"; the largest radii seen with the library stocks are 2 for scan / unsharp / grain and 1 for dye at 6000 px, 2 at 61 MP pitch, `film_gpu_tests plan`); `DAPPLE_FILM_NODES=0` (the A/B switch).
* The micro-structure is validated only at a pixel pitch (< 0.6 um) no product frame reaches, in the 4x-box domain.
* **Android and phones: not built, not run.** The new shaders (`film_dye.comp`, the extended `film_fin.comp`, `film_grain.comp`, `film_point.comp`) are in both shader lists and ran through the desktop GLES (ANGLE) build only; nothing was run on an emulator or a phone (the three attached phones were not touched). The memory bounds above are untested on mobile unified memory.
* `check-all.sh` was not run as a whole (only the suites listed in 23.2); the golden `engine.golden_off/full` and `dapple_exec_tests` ran and pass, the Flutter side and the JVM tests were not run.
* The device solve cache is whole-frame only (a tiled export recomputes its global grids per export); a hit relies on the planner's node key covering the frame size (it does: `ctx.srcSize`, region, `ctx.outLongEdge`), and a kept solve that disappears between planning and running (device rebuild) fails the run as Transient (retry, then CPU).


## 24. GPU on by default after a self-test, per-frame executor accounting, no second renderer (app engineer, 2026-11-12, wave 5)

Owner ruling: the GPU is on by default once the device passes the self-test, with no user switch. Before this section the app used the GPU only after `setprop debug.dapple.gpu 1` (every frame was a host frame and the view's `refused` counter counted every frame).

### 24.1 What landed

| Piece | Where |
|---|---|
| **Modes** (`gpu_runtime.h`): `off` / `on` / `auto`. Precedence: `setRuntimeEnabled(0|1)`; env `DAPPLE_GPU=0|1|auto`; Android property `debug.dapple.gpu` = `0` (developer kill switch) / `1` (explicit: device created and tested on the first GPU frame, as before) / anything else or unset = **auto**. Default: auto on Android, off elsewhere (desktop tools and tests are unchanged). `debug.dapple.gpu_api` / `DAPPLE_GPU_API` still force one API (`vulkan`, `gles`, `cpu`). The need for `=1` is gone. | `exec/gpu_context.cpp`, `exec/gpu_runtime.h` |
| **Probe off the UI thread, never blocking first paint**: `MainActivity.configureFlutterEngine` -> `GpuProbe.start` starts a daemon, minimum-priority Kotlin thread that loads `libdapple.so` and calls `dapple_gpu_configure(dir, appVersion, Build.FINGERPRINT)` + `dapple_gpu_warmup()` (JNI `DappleViewNative.startGpu`). `startWarmup` returns at once; the device creation and self-test run on a detached native thread at nice 10. In auto mode `GpuProvider::enabled()` is false until the probe passed, so frames before that are ordinary CPU engine frames (no waiting, no GPU planning, no overflow-lattice bake); the first frame after it uses the GPU. | `GpuProbe.kt`, `view_jni.cpp`, `c_api.cpp` (`dapple_gpu_configure`, `dapple_gpu_warmup`) |
| **Order**: Vulkan, else GLES 3.1 (each with its own self-test, as in 19), else CPU. A GLES-only phone works; a failed API is recorded with its reason (`skipped` in the status JSON). | `ProviderImpl::make` |
| **Cached verdicts**: `noBackupFilesDir/gpu/gpu_selftest.v1`, keyed by Build.FINGERPRINT + app version (`versionName+versionCode`) in the header and by API + `DeviceCaps::key()` (vendor:device:**driverVersion**) per record. A pass skips the pipeline self-test and the box-scan test (the box verdict is cached with it); a recorded failure is not retried. Any change of the three re-runs the test (the file is rewritten under the new header). | `SelfTestStore`, `ContextOptions::cacheLookup/cacheStore` |
| **Crash / hang guard**: `gpu_selftest.running` is written before an API is created and tested and removed after. If it is found at the next start (and names the same fingerprint + app version) the process died or hung inside the test: that API is recorded as failed (`deviceKey "*"`) and is not tried again for this fingerprint + app version; the other API still is. A marker of another fingerprint / version is dropped. Forcing the API with `debug.dapple.gpu_api` ignores the record (developer path). | `SelfTestStore::recoverMarker/arm/disarm` |
| **Executor accounting** (process-wide, `executorStatsJson()`; C API `dapple_engine_stats_json`): `gpuFrames`, `cpuFrames` (frames of `EngineSession::renderFrame`), `deviceLost`, `demotions`, `api` (executor of the last frame), `lastReason`, and `unsupported`: a histogram of the reason text of every CPU frame (at most 48 distinct texts). It is part of `dapple_gpu_status_json` (`executors`) and of `dapple_view_info_json` (`engine`). `FrameResult::why` carries the reason of the frame. Reasons: `GPU self-test running`, `GPU unavailable: ...`, `GPU disabled (developer switch)`, `no source store (host frame)`, the executor's Unsupported text (`unsupported on the GPU: ...`, `plan unsupported: ...`, `film stock / paper not available`, ...), `transient GPU failure: ...`, `device lost: ...`, `cpu plan unsupported: ...`. | `engine_session.cpp` (`noteFrame`), `gpu_iface.h` (`GpuProvider::inactiveReason`) |
| **Throttled log**, logcat tag `dapple_engine` (stderr with `DAPPLE_ENGINE_LOG=1` off Android): one line per 30 frames and on every change of executor or reason: `frame 1600x1067 tier=interactive film=on exec=cpu reason=<text> \| gpu N cpu M` (tier = `interactive` / `full` / `off` spatial quality). | `engine_session.cpp` |
| **Diagnostics page**: the hidden codec page (long-press the version on 关于) now starts with a GPU block: mode, probe state, API, device, self-test text, frames per executor, and the CPU frames by reason. | `codec_diagnostics_page.dart`, `DappleSession.gpuStatusJson` |
| **Build**: `app/android/app/build.gradle.kts` compiles the GPU executor **and** the GLES 3.1 HAL by default (`-DDAPPLE_GPU=ON -DDAPPLE_GLES=ON`, `dapple_shaderc` host tool from `build/shaderc`); `-Pdapple.gpu=false` builds a CPU-only app. (`gpu-backend.md` 10.11 describes the old opt-in switch.) | `build.gradle.kts` |
| **No second renderer**: both `SEAM(film engineer)` fall-backs to the old `Renderer` in `engine_session.cpp` (`renderFrame`, `renderExact`) are gone. A film whose stock / paper is not in the library, or whose table bake failed, is now an explicit error result (`engine: plan not supported by any executor (<reason>) [film: unknown stock / paper or failed table bake]`), never a silent old-renderer frame (`FrameResult::executor` `legacy-film` no longer exists). `FramePlan::film` did not need `FilmFx`. | `engine_session.cpp` |

### 24.2 Gates

* `engine.gpu_probe` (new, `dapple_gpu_engine_tests probe`; each step is a child process = one app start, Vulkan RTX 4060 Ti): start 1 -> `startWarmup` returns immediately, the frame during the probe is a CPU frame with `why = "GPU self-test running"`, after the probe the frame is `vulkan`; the verdict file has a `pass` record and no marker is left. Start 2 -> `state ready` with the self-test text `cached pass`. A hand-written marker (same fingerprint + app version) -> consumed, `vulkan\t*\tfail` recorded, the API is skipped (this build has no GLES: CPU frames). A marker of another fingerprint -> ignored, ready again. `DAPPLE_GPU=0` -> mode `off`, CPU frame with `why` "GPU disabled (developer switch)".
* `snapshots`: `engine.snapshots` passes with the checked-in `graph_snapshots.txt` (the `film_strength_0/50` graphs carry `NRChroma`/`Detail` because those film frames are GPU-supported now and the Legacy-detail graph is built for the GPU plan; reviewed in 23.2 and `builder.cpp`).
* Flutter: `flutter analyze` clean; `home_test.dart` (two new tests for the no-permission card wording) and `codec_diagnostics_test.dart` (two new tests for the GPU block) pass.

### 24.3 Not done / not verified

* No phone run: the Find N5 was not attached to adb in this session, so the probe, the GLES fall-back and the cached verdict were not run on a real Adreno driver, and nothing verifies that Vulkan -> GLES order on a device whose Vulkan fails. The desktop test covers the logic with Vulkan (RTX) only; GLES in the probe was not exercised (the desktop build has no GLES; the arm64 build compiles it).
* The probe cost on a phone (self-test wall time, first-frame effect) is not measured. The self-test text on the N5/Pixel earlier was 0.3-0.6 s with the test (`find-n5.md` section 1).
* A hung self-test leaves the probe thread running for the rest of the process (frames stay on the CPU); only the next start learns about it through the marker.
* A fresh install with a Vulkan driver that passes the test but is slow is not detected; only correctness is tested.


---

## 25. Film frame speed on the Adreno 840: where the time went and what changed (film perf engineer, 2026-11-12, wave 5)

Goal: interactive film frame (810x1080 and 1600 px long edge) <= 8.3 ms GPU on the Find N5, final tier <= ~16 ms, parity unchanged. Raw per-pass tables: `find-n5.md` section 9.

### 25.1 Measuring: the per-pass timestamps on Adreno were wrong, now they are not

Timestamps written with `ALL_COMMANDS` after each pass are skewed on the 840 (the first point pass read 0.006 ms, a vertical pass 4.4 ms; independent dispatches overlap and the work is attributed to a neighbouring interval). Three profiling switches in `gpu_scheduler.cpp` (profile builds only, `DAPPLE_GPU_PROFILE=1`):

| switch | effect |
|---|---|
| `DAPPLE_GPU_PROF_SERIAL=1` | a full barrier before each timestamp (not enough on the 840 alone) |
| `DAPPLE_GPU_PROF_REPEAT=N` | every dispatch runs N times back to back inside its timestamp interval (the image content is garbage afterwards); divide by N. **This fixed it**: with N = 20 the per-pass sum on the N5 was 8.54 ms against 8.48 ms for the same frame measured un-instrumented |
| `DAPPLE_GPU_PASSLOG=1` | one line per recorded pass: images bound (R/W/M, size, bytes per texel), nominal MB read / written, workgroups (`lastPassLog()` in the scheduler) |

`dapple_film_gpu_tests bench` prints the full table (time, MB, GB/s) with `DAPPLE_GPU_PASSLOG=1`, `DAPPLE_BENCH_ONLY=<substring of the case name>` selects cases, `DAPPLE_FILM_DATA=<dir with dapple.dfp>` makes it run on a phone; new cases 810x1080 (the app's interactive size) interactive / final. `dapple_gpu_tests --film-interactive [long edge]` runs interactive film frames through the whole session path and splits the wall time per phase (upload = film tables / plan / display, record, submit, wait, copy) for a still frame and strength / film exposure / exposure / grain / halation / warmth drags.

### 25.2 Where the time went (N5, thermal status 0)

810x1080 interactive Portra, 10 passes, per-pass ms with REPEAT = 20 (nominal traffic is the sum of the bound images; texture-cache hits make the real DRAM traffic smaller):

| # | pass | ms | share |
|---|---|---|---|
| 0 | film_point (exposure, bicubic raw LUT) | 0.28 | 3 % |
| 1 | film_gnoise (glare field) | 0.08 | 1 % |
| 2 | film_h3, 4 components | 1.12 | 13 % |
| 3 | film_h3, 3 components | 0.44 | 5 % |
| **4** | **film_v3, 4 components (shared-memory column tile)** | **4.24** | **50 %** |
| **5** | **film_v3, 3 components** | **1.65** | **19 %** |
| 6 | film_point (develop) | 0.18 | 2 % |
| 7 | film_pd (print + decode + glare) | 0.19 | 2 % |
| 8 | film_enc | 0.10 | 1 % |
| 9 | readback copy | 0.27 | 3 % |
| | total | 8.54 | |

* **69 % of the frame was the vertical mixture pass.** It ran at 10-26 GB/s against 100+ for the neighbouring passes: the column tile in shared memory (a barrier per component, 13.8 KB of shared memory per group) is latency-bound on Adreno (it is 3-5 % faster than direct taps on the RTX). Reading the taps straight through the texture path (`DIRECT` specialisation, no staging, no barrier) takes 4.24 -> 1.90 and 1.65 -> 0.72 ms.
* The point passes (tables, 16 raw-LUT fetches, tetrahedral print LUT) are cheap (0.1-0.3 ms): table lookups and fetches per pixel are not the problem on this part; the shared-memory kernel and the pass count are.
* The mixture structure: 7 components (radii 15, 9, 9, 6, 6, 2, 1) in two chunks of 4 + 3 (4 passes at full resolution), of which five carry a combined weight of 1.5 % (red) / 0.5 % (green).
* **CPU side, not GPU (measured on the desktop with `--film-interactive`):** `Executor::ensureFilm` called the uncached `bakeFilmTables` on every frame (three table bakes, ~12 ms) and, because the fresh payload pointers never matched the device copy, re-uploaded the film tables every frame. A still 1080 px interactive film frame cost 14.5 ms wall of which 11.7 ms "upload" and 1.5 ms GPU. On the phone this is the likely reason the app saw 28-32 ms for a frame whose GPU time is 5-8 ms (not measured on the phone). The first film frame of a session costs 149 ms on the desktop (92 ms stock tables + grain plan, 40 ms pipeline creation, 11 ms wait): the likely source of the 120 ms 360x480 frame seen in the app (not reproduced on the phone).
* Thermal / slow state: `Thermal Status` 0 throughout; the GPU frequency is not readable (`/sys/class/kgsl` permission denied); the benchmark ran under `taskset 60` (CPUs 5-6; accepted, whether the waiting thread stayed there was not verified). The baseline measured in this session (810x1080 interactive 8.5 ms, 1080x720 7.5 ms, 1600 px 14.1 ms) is about 2x lower than the numbers in the task statement (15 ms at 1080 px, 27-37 ms at 1600 px) from an earlier session, so the phone was in a faster state now. All before / after pairs below were measured back to back in the same run.

### 25.3 What changed

| change | where | effect |
|---|---|---|
| **Film tables from the process-wide cache** (`tablesFor`) instead of an uncached bake per frame; unchanged payload pointers also skip the device re-upload | `gpu_executor.cpp ensureFilm` | desktop session path: still frame 14.5 -> 3.9 ms, strength / film exposure / exposure / halation drag 14-17 -> 3.3-4.0 ms, grain drag 16.5 -> 6.4 ms (grain tables are re-planned and re-uploaded, 2.1 ms). A warmth / tint / print-EV drag still rebakes the print LUT (17.8 ms desktop; not addressed) |
| **`film_v3` direct taps** on unified-memory GPUs (`DIRECT` specialisation constant; discrete GPUs keep the staged version; `DAPPLE_FILM_V3_DIRECT=0/1` forces) | `film_mix3.comp`, `film_gpu.cpp` | N5 frame GPU time, same run: 810x1080 interactive 8.48 -> 5.16, 1080x720 interactive 7.48 -> 4.57, **1600 interactive 14.12 -> 9.78**, grain drag 15.24 -> 10.91, **1600 final 19.85 -> 14.98**, 810x1080 final 13.33 -> 9.07, 3600 px final 97.0 -> 81.5, enlarger diffusion 40.6 -> 27.9 |
| **Develop + print (+ glare, + encode) in one pass** when there is no grain and no enlarger diffusion: `film_point` / `film_pd` MODE 8 (E2 [, q] -> D -> print LUT -> decode -> glare add -> optional encode). The glare parameters moved to unused slots of the uniform block (`dmin.w, dscale.w, dsub.w, misc.w`) because MODE 8 needs the DIR matrix | `film_point.comp`, `film_gpu.cpp recordFilmWindow` | -2 full-frame passes in the interactive tier (13 -> 11 passes at 1600 px interactive on the desktop). Not measured on the N5 |
| **Interactive tier only, GPU only: merge the small components of the exposure mixtures** (neighbours in sigma, combined weight <= 2 %, widest / narrowest <= 3, variance matched). Not done to the CPU nodes: they stay bit-identical to the FilmSimulator oracle (`film_nodes.equiv`, `golden`); the GPU stays inside the parity budget | `film_gpu.cpp mergeSmallComps` (`DAPPLE_FILM_MERGE=0` turns it off) | 810x1080 interactive: 7 -> 3 components, one chunk, 8 -> 6 passes; 1600 px interactive: 5 -> 4 components, one chunk instead of two (11 -> 6 passes together with the fusion). The final tier is never merged. Not measured on the N5 |
| profiling switches, pass log, `--film-interactive`, bench cases and `DAPPLE_FILM_DATA` | `gpu_scheduler.*`, `film_gpu_tests.cpp`, `gpu_tests.cpp` | measurement only |

Merge error, hard scene (dim surround, 4 to 16 times over-white lamps), 5 stocks x 4 sizes (`dapple_film_gpu_tests merge`): merged vs exact GPU frame mean 0.003-0.008, p99 <= 0.07, max 0.6-3.2 dE2000 (a few pixels at the lamp edges); merged GPU vs CPU: mean 0.010-0.013, p99 <= 0.07 (exact GPU vs CPU: 0.009-0.012 / 0.04). Asserted: merged vs exact mean <= 0.02, p99 <= 0.2; merged vs CPU inside the parity budget.

### 25.4 Result against the goal

N5, measured: with the vertical-pass change alone **810x1080 interactive 5.2 ms (target 8.3: met), 1080x720 4.6 ms, 1600 px final 15.0 ms (target ~16: met, 1 ms margin), 1600 px interactive 9.8 ms (target 8.3: NOT met)**. The fusion and the merge were built after the phone left (25.5); they remove 2 + 2 full-frame passes at 1600 px interactive. An estimate from the per-pass table scaled by pixel count is 1.5-2.5 ms, i.e. about 7-8 ms, **which is an estimate, not a measurement**. The CPU-side fix is measured on the desktop only (it removes ~12 ms per film frame there; the phone bakes slower, so the in-app gain should be larger; not measured).

### 25.5 Not done / not verified

* **The Find N5 went off adb after the first measurement runs** (unplugged). Nothing after the vertical-pass change was run on it: the fused develop + print, the component merge, the `ensureFilm` fix, the estimate in 25.4 and the app frame time are NOT verified on the device. The 1600 px interactive target is not shown to be met.
* Final tier: 810x1080 is 9.1 ms and 1600 px 15.0 ms on the un-throttled phone; the throttle step measured earlier (-40 % throughput after ~1 min of continuous load, find-n5.md 3.3) would put 1600 px final at ~21 ms. Not re-measured.
* Not changed: the grain-drag cost (grain tables re-planned / re-uploaded per drag value, 2.1 ms desktop), the print-LUT rebake on warmth / tint / print-EV drags (17.8 ms desktop), pipeline creation on the first film frame (40 ms desktop; a pre-warm when the executor is created would hide it, it belongs to the GPU warm-up in `gpu_context.cpp`, the app engineer's), the stock table bake on first use (92 ms desktop; the app could bake in the background when a stock is picked).
* Considered and not done: half-resolution glare / dye / DIR, RG11B10 planes (no exact enough 3-channel storage format in the HAL), fusing the grain pass with the develop pass.
* Gates run on the desktop (RTX 4060 Ti, Vulkan, `build/w5-perf`): `film_gpu.parity`, `film_gpu.frames`, `gpu_parity`, `engine.*`, `film_nodes.*`, `golden`, `film.*` pass; seams: `gpu_tests --film-tiling` 0 differing pixels in every run (Vulkan, and GLES / ANGLE with `DAPPLE_GPU_WG=128`); `--coverage` 192 / 192; GLES (ANGLE Vulkan) `dapple_film_gpu_tests` and `frames` pass with default and `DAPPLE_GPU_WG=128`, with `DAPPLE_FILM_V3_DIRECT` 0 and 1. SwiftShader-GLES was not run.


---

## 26. Wave 7: Vulkan upload alignment, film pad drags, start-up warm-up, logging (engine bug-fix engineer, 2026-11-12)

Starting point: the wave 6 device run (`find-n5.md`, "N5 wave 6"). Device results of this wave: `find-n5.md`, "N5 wave 7" (the phone left adb part-way: what ran there is listed, the rest is marked NOT RUN).

### 26.1 Enlarger diffusion parity on the Adreno 840 (Vulkan only): an unaligned copy offset

`uploadGpuTables` packs the film tables into one staging buffer and copies the print LUTs from it with `vkCmdCopyBufferToImage`. The offsets of the enlarger split tables (`lut1`, `lut3`) were `raw + lut + curves + srgbDecode`, and the sRGB decode table is 4097 floats = 16388 bytes: **not a multiple of the 16-byte texel size of RGBA32F** (the Vulkan rule for `bufferOffset`). The desktop driver tolerated it; the Adreno copied from the wrong address, so every texel of both LUTs was shifted by one float (red read the previous texel's alpha = 0, hence `gpu -0.0004 0.4478 0.7788`). GLES uploads texels through its own path, which is why only Vulkan failed; the point chain and the main LUT (offset `rawBytes`, always aligned) were fine. Fix: the offsets of `lut1`, `lut3` and the paper curves are rounded up to 256 bytes (`film_gpu.cpp uploadGpuTables`, staging +1 KB). Checked on the N5: `film_gpu.parity` Vulkan and GLES, `film_gpu.frames`, `--film-tiling` all pass; the six enlarger rows are mean dE 0.009-0.037 (was ~40). The other buffer-to-image copies in the tree (`gpu_executor.cpp`: tone / lattice / look LUT) have 256-aligned offsets by construction.

### 26.2 Film pad drags: where 35-185 ms went and what changed

Method: `FrameResult::cpu` (new, `CpuSplit`: match, renderer, graph, tables, exec, total) is filled per frame; `DAPPLE_FRAME_PROFILE=1` logs one `dapple_engine` line per frame; `dapple_drag_bench` (`tests/engine/drag_bench.cpp`) drives `dapple_session_render_frame` like the app (INTERACTIVE + `active_param`). Desktop before the fix (RTX 4060 Ti, 360 px long edge): film_shoot 73 ms, film_print 48, film_cast 67, film_black 11, film_latitude 10, warmth / tint 30, develop exposure 11. The split:

1. **The film exposure match** (`withFilmKey` -> `matchFilmExposure`) re-ran from scratch for every new value of film_shoot / film_print / film_latitude, **and for every warmth / tint step because both were part of the match key**: two develop renders of the 256 px proxy, two sorted scene statistics, the solve, and one more full film render of the proxy that only fed the `FilmMatchInfo` diagnostics (40-200 ms desktop).
2. **The print-exposure calibration** (`filmPrintConstant`, ~50 ms per (stock, paper, EV shift) value, image independent) at each new 0.5 EV bucket of a moved negative.
3. **The print table bake** (33^3 spectral nodes, 12-14 ms desktop, ~30 ms on the phone) for every changed printEv / black / warmth / tint, plus its re-upload.

What changed (the render of a settled frame is unchanged):

| piece | where | effect |
|---|---|---|
| **Match statistics cache** `FilmMatchCache` (per session): scene statistics of the develop edit (gain, `stFull`, `st0`, `keyL0`) are computed once per photo + develop edit; `keyL0` (render at exposure 0) also survives an exposure drag; `matchFilmExposureCached` is the v5 flow of `matchFilmExposure` on top of them without the diagnostic render (other modes and `info` callers still use `matchFilmExposure`) | `film_match_cache.cpp`, `film_match.h` | shoot / latitude / wb drags are arithmetic; exposure drag 11.8 -> 3.4 ms desktop |
| **Lean match key** `filmMatchKeyLean`: print warmth / tint / wb leave the key unless a median of the print is evaluated (film_print != 0, or a slide's film_shoot) | same | warmth / tint drags are not a new match |
| **film_print**: the median L* of the film render at (film EV, print EV) is remembered per series; while a slider moves (INTERACTIVE frames) a median between two remembered samples closer than 1 EV is interpolated (monotone cubic); the frame after the drag solves with real renders. A result that interpolated is flagged and never served to a non-interactive frame, so **the settled frame is exactly the uncached match** | same, `c_api.cpp withFilmKey` | print drag 54 -> 4 ms median desktop; the first frames of a drag still render (63-92 ms desktop first frame) |
| **Calibration warm-up** `prewarmFilmCalibration`: the 15 print-constant calibrations of (stock, paper) for EV shifts -3.5 .. +3.5 are computed once on a background thread (efficiency core) when the stock first meets the match | same | film_shoot drag 71 -> 4 ms median; a drag started within ~1 s of choosing the stock can still hit a cold bucket (~50 ms desktop) |
| **Coarse print table while a table slider moves**: an INTERACTIVE frame whose `active_param` is not film_strength / film_grain / film_halation / grain uses the exact print table when it is cached and otherwise a 17^3 table (`FastOptions::coarseFilm` -> `FramePlan::Film::coarse` -> `FilmFrameArgs::coarse` -> `FilmResolved::coarse` -> `tablesFor`, own LRU; `FilmModel::bakePrintScan(t, lutN)`); the frame after the drag is not coarse and bakes the exact 33^3 table (same key as before). `DAPPLE_FILM_COARSE_OFF=1` disables | `film_tables.cpp`, `film_model.cpp`, `renderer.cpp`, `c_api.cpp` | black / warmth / tint drags 12-34 -> 4 ms desktop. Coarse vs exact frame at the same parameters: mean abs diff 0.48-0.81 / 255, max 3 (25 for film_black +0.37 in one pixel group) |

Desktop, drag bench, 480 px long edge (320x480 frames), medians of 19 frames (first frame of the drag in brackets). "Before" = `DAPPLE_FILM_MATCH_UNCACHED=1 DAPPLE_FILM_COARSE_OFF=1` (the same binary with the old match, key and exact tables), "after" = default:

| drag | before | after |
|---|---|---|
| film_shoot | 70.9 (75.4) | 4.3 (5.0; a cold bucket costs ~55 ms when the warm-up has not finished) |
| film_print | 53.8 (92.1) | 4.2 (63.0) |
| film_black | 13.9 (32.4) | 4.4 (3.9) |
| film_latitude | 11.7 (34.7) | 0.9 (5.6) |
| film_wb | 15.8 (11.7) | 2.9 (0.8) |
| film_warmth / film_tint | 33.8 / 34.2 | 4.3 / 4.1 |
| exposure (film on) | 11.8 | 3.4 |
| film_strength / film_grain | 0.9 / 1.7 | 0.7 / 1.6 |

N5 (native bench, 240x360 frames, after only): shoot 5.4, print 5.3, black 6.2, latitude 1.5, wb 2.7, warmth 6.6, tint 7.0 ms median (`find-n5.md` "N5 wave 7"). Not measured: the app size on the phone, the A/B on the phone.

**film_print is not an exposure scale in front of an unchanged table.** The print exposure multiplies the paper *exposure*, which sits inside the spectral chain between the film density and the paper curve; it is not a shift of the density LUT's input (only the split enlarger path has that separation). So the interpolation / coarse-table route was used, and a settled frame is exact. **film_cast** was replaced by film_wb in the tree by the film engineer during this wave, so the cast part of this work was dropped with it (measured before that: the print constant is linear in the cast mix within 0.0005 EV, but in the EV shift only to 0.015-0.034 EV at 1.5 EV node spacing, which is why the 0.5 EV buckets were kept and warmed instead of widened).

`film_latitude`: `dapple_session_film_pad_json` now reports `"film_latitude"` in `effective_lo` / `effective_hi` (index 4 of `FilmPadInfo::effectiveLo/Hi`): true only when that end moves the solved film EV against latitude 0 (`filmPadInfo`, shoot = print = 0). That it changes nothing on most photos (wave 6) is by construction: the rescue acts only when the in-range share is below the target.

`film_black`: measured on DSC00111 (bright, 2.9 % of the pixels below 20/255), Portra 400, final tier, difference to film_black 0: -1.00 mean 3.88 / 255 (dark pixels 11.1), max 35; -0.34 mean 1.42, max 17; +0.34 mean 0.35, max 5; +1.00 mean 1.04, max 12. That is what section 18 says: -1 lifts the black to L* 15.8, 0 is the true black (L* ~3.4, 8-bit ~10) and +1 can only move it down to 0, so the + half has a range of at most ~10 / 255 in the darkest pixels (the wave 6 mean of 0.98 / 255 at +0.34 on a darker photo is consistent).

### 26.3 Session path against the bench (item 3): found, not reduced

`dapple_gpu_tests --film-interactive 1600` (with `DAPPLE_GPU_PASSLOG=1` it now prints the pass names): a still film frame is `film_point film_gnoise film_h3 film_v3 film_pd film_fin gauss gauss detail readback` = 10 passes; the bench case is `film_point film_gnoise film_h3 film_v3 film_pd readback` = 6. The four extras: `film_fin` (clamp of the film plane to 0..1 before the next stage; with strength 100, no display stages and no vignette it is a pure clamp that could be fused into the `ENCODE` variant of `film_pd` by a spec constant and a `FilmImg::clamped` flag) and `gauss gauss detail` = the Detail stage, which exists because **the default edit has sharpen 40** (RAW baseline, `EditParams::sharpen`) and the bench frame has none. So the 17.9 ms wait at 1600 px is the app's real frame (film + output sharpening), not an inefficiency of the film passes. The `film_fin` fusion (~0.4 ms estimated, not measured) was not implemented. The 1600 px film_v3 cost: the component merge cannot go further (`DAPPLE_FILM_MERGE_W` = combined weight limit of a merged group, default 0.02; 0.04 and 0.08 give bit-identical results because the 4 remaining components are heavy; merged vs exact mean 0.007, p99 0.066 at 1600 px). **The 8.3 ms target at 1600 px interactive is NOT met and was not re-measured** (bench 8.74 ms in wave 6).

### 26.4 Start-up: probe log, film warm-up, persisted pipeline cache (items 4, 6)

* `dapple_engine` (logcat; stderr with `DAPPLE_ENGINE_LOG=1`) logs once per probe: `gpu probe: api=vulkan device="..." driver=N self-test fresh|cached, probe total X ms (skipped: ...)`, or `gpu probe: no backend (reason) after X ms`; and `gpu warm-up: film pipelines + default stock Y ms, pipeline cache Z KB saved [path]`.
* **Warm-up** (`gpu_context.cpp warmFilm`, in the probe thread after the context is published, so frames wait for it only on the context lock): waits up to 10 s for the film library (the app sets the pack path lazily), then renders three small film frames of the default stock (interactive, full, interactive with kept grain) through a throw-away `EngineSession` + the context's executor: the film pipelines of the used specialisations, the stock's tables (process-wide cache) and the grain plan. `DAPPLE_GPU_WARM_OFF=1` disables.
* **Pipeline cache**: the Vulkan `VkPipelineCache` is persisted to `<gpu cache dir>/pipeline_cache_<hash of fingerprint + app version + API>.bin` (the driver also validates its own header / UUID, so a driver update discards it). It was only written in the device destructor, which an Android process never reaches; `Context::savePipelineCache()` now writes it (atomically) after the warm-up. `DAPPLE_GPU_PIPELINE_CACHE=<path>` still overrides.
* Desktop (RTX 4060 Ti, drag bench with `DAPPLE_BENCH_PROBE`): probe 198 ms fresh / 129 ms cached verdict; warm-up 330 ms (first start) / 36 ms (second start); first film frame of the process 118 ms (warm-up on) vs 142 ms (off), second start 103 vs 118 ms. The remainder is image dependent (device source upload, first match statistics ~70 ms desktop) and cannot be warmed before a photo is open. **N5: NOT RUN** (the 218 ms pipeline creation + 110-120 ms table upload of wave 6 should drop to the cache load; unverified).

### 26.5 CPU time per frame in the log (item 6)

The throttled `dapple_engine` line now ends with ` | cpu_ms T (match a renderer b graph c tables d exec e; upload u gpu g) max M`: T = CPU-side time of the frame (C-layer match + Renderer construction + engine), the parts as in `FrameResult::cpu` (`exec` includes the GPU wait), `max` = the slowest frame since the previous line. Slow frames can be attributed from logcat.

### 26.6 Core pinning (item 7)

`pinCurrentThreadToBigCores()` (`big_core.h`; the cores with >= 80 % of the top frequency, permanent for the calling thread, `DAPPLE_GPU_NO_PIN=1` disables) is now applied to the two long-lived threads that record and submit besides the C layer's `RenderThread` (already pinned to the performance cores) and `Executor::run`'s scoped pin: the present `RenderLoop` thread and the GLES device's command thread (`GlThread`). The Vulkan driver's own threads and the `ThreadPool` workers (the table bake) are not touched. **Whether it changes frame times: NOT MEASURED** (phone gone).

### 26.7 Gates (desktop `build/w7`, RTX 4060 Ti; the tree also carries the film engineer's uncommitted film_wb / pack changes)

* `ctest -j2`: 42 of 44 (without `film.*`) pass; `engine.golden_off` and `engine.golden_full` report one mismatch each, `film/full_geometry`, which moved with the film engineer's pack / model change (`core/data/film/dapple.dfp` and `film_fx.txt` are modified in the tree). Nothing of this wave changes a CPU film pixel (coarse tables need `FastOptions::coarseFilm`, the rest is GPU-side or in the C layer). Nothing was re-recorded. `film.*` 29 / 29, `film_gpu.parity`, `film_gpu.frames`, `gpu_parity`, `gpu_parity_swiftshader` and the other `engine.*` pass.
* GLES 3.1 on ANGLE (Vulkan backend, `DAPPLE_GPU_WG=128`, `build/w7-gles`): `dapple_film_gpu_tests` parity and frames and `dapple_gpu_tests --film-tiling` pass.
* N5: see `find-n5.md` "N5 wave 7".
* One typo of another engineer's uncommitted hunk was fixed so that the engine runs at all: `registry.cpp`, FilmTableRaw mask `"temperature"` -> `"temp"` (unknown ParamKey aborted every frame).

### 26.8 Final gates and what is not verified (wave 7)

* `ctest -j2` on the whole `build/w7` tree (73 tests incl. the new `engine.match_cache`: the cached match equals `matchFilmExposure` for six parameter sets; a 21-frame print drag interpolates 20 frames with at most 0.026 EV of print-EV deviation and 9 real renders; the settled frame is the exact solve): 71 pass; `engine.golden_off` / `engine.golden_full` fail with the single mismatch `film/full_geometry` (film engineer's pack / model change, see 26.7; `dapple_golden selfcheck`, off against full, 0 mismatches).
* `bash tools/check-all.sh` (`JOBS=4 CTEST_JOBS=2`, the script now honours `CTEST_JOBS`): desktop build, ctest (the same two golden failures), `flutter test` (222 passed), JVM unit tests, host video tests pass; `flutter analyze` reports 5 infos, all in UI-engineer files (`window_manager.dart`, `editor_page.dart`, `debug_open.dart`, `panel_layout_test.dart`, `editor_panel_test.dart`); no errors.
* Release arm64 APK from this tree: `build/n5/app-w7-arm64.apk` (36 MB, debug key, contains the new probe / warm-up / `cpu_ms` strings). **Not installed and not launched** (the N5 was not on adb); the `adb install -r` + one launch + logcat read of the task was not done.
* NOT verified on a device: everything listed under "NOT RUN" in `find-n5.md` "N5 wave 7" (items 3, 4, 6, 7, the app-size drag numbers, GLES forced timings). The N5 drag numbers are from a native C-API bench, not the app.

### 26.9 Correction after lead review: the golden mismatch was mine (cache), film_fin fused, first drag frame

* **Cause of `film/full_geometry`** (26.7 blamed the film pack wrongly): `developEdit()` of the match cache cleared `vignette`, `nrColor` and `sharpRadius` in the develop edit the statistics are taken from; `matchFilmExposure` clears only clarity / texture / dehaze / grain / sharpen / nrLuma, so the proxy render (key L*, scene gain) differed for a frame with a vignette (the preceding corpus frames set vignette -20). Fixed to clear exactly the same fields.
* **Cache key fields** (everything the uncached solve reads): `FilmMatchCache::baseKey` = JSON of the develop edit (tone, colour, geometry, vignette, nrColor, sharpRadius, exposure, WB, look amount ...) + LUT id + exposure + as-shot illuminant `filmSceneX/Y` (not serialised); `keyL0` additionally by the edit at exposure 0; the print-median series by baseKey + stock + paper + warmth + tint + black + film_wb. The session result key (`filmMatchKeyLean`) is deliberately **identical to the legacy `filmMatchKey`** (it ignores vignette / nrColor / sharpRadius and strength / grain / halation / black), only dropping warmth / tint / film_wb when no median of the print is evaluated, so a settled frame reproduces the legacy behaviour bit for bit, stale-key behaviour included.
* **Test** `engine.match_cache_seq` (`dapple_golden seqcheck`, `DAPPLE_FILM_MATCH_UNCACHED` is now read per frame): 17 sequences (interleaved crop / straighten / rotate / vignette / WB / stock negative-slide-negative / film_wb / exposure / nrColor / sharpRadius, plus for a negative and a slide a 6-frame INTERACTIVE drag of film_shoot, film_print, film_black, film_latitude, film_wb, film_warmth, film_tint and exposure each followed by a settled frame, a second drag value and a second settled frame): 60 settled frames, all bit identical cached vs uncached (the claim "a settled frame is exact" holds for every pad parameter). `engine.golden_off/full`: 148 frames, 0 mismatches, nothing re-recorded.
* **film_fin fused**: the last film pass `film_pd` (ENCODE) clamps to 0..1 itself (spec constant 3, `FilmResolved::clampOut`: strength 1, no display stages, no vignette) and the executor skips the then pure-clamp `film_fin` (whole frame, not the last pass): the still frame at 1600 px is 9 passes (was 10). Bit identical by construction (clamp then round to half = round then clamp). Gates: `film_gpu.parity` / `frames`, `gpu_parity`, `gpu_parity_swiftshader`, `--coverage` (192/192), `--film-tiling` on Vulkan (desktop) and GLES (ANGLE). Time saved on the N5: not measured (estimate ~0.4 ms).
* **First frame of a drag**: film_print's first frame did the first print solve (about 5 proxy renders). A background thread now renders the medians on a 1 EV grid around the automatic print exposure as soon as a negative's match is first computed (`FilmMatchCache::Seed`, merged on the next call, exact renders so settled frames are unchanged; `DAPPLE_FILM_SEED_OFF=1`): desktop first film_print frame 63 -> 14 ms. film_grain's first frame (89 ms on the N5, 6-7 ms on the desktop) is the not-yet-created keepGrain pipeline variants and the grain plan: the start-up warm-up renders exactly that tier, so it should be hidden after the warm-up; NOT verified on the phone. Remaining first-frame cost with a cold stock is the 15 calibrations (background).
* `build/n5/app-w7-arm64.apk` rebuilt (36 MB). Not installed (phone not attached).

## 27. Wave 8: DNG opcode lists for phone RAW, the as-shot illuminant of any camera, N200B drift (engine-gaps engineer, 2026-11-12)

### 27.1 DNG opcodes (`core/src/decoders/dng_opcodes.{h,cpp}`, hooked into `libraw_decoder.cpp`)

* **Where, and why a decode-stage step and not an engine node.** The opcode lists describe the sensor and lens of one file, not an edit: they are the same for every edit, never change during a session and must run on the mosaic (OpcodeList2) before the demosaic, which is inside LibRaw. A node would be keyed on nothing, and a mosaic-stage node does not exist (the graph starts at the demosaiced scene-linear `SourceImage`). So they are part of `ImageDecoder::decode`; the decode result is what the app caches as the source (one decode per photo, as before), so the corrections cost nothing per frame and the film / develop nodes see corrected pixels. The geometry step (WarpRectilinear + DefaultCrop) runs after the demosaic on the CPU in the decoder; a GPU version was not written (it would be a resample node in front of the whole graph and the result is cached anyway; measured cost below).
* **Parser.** `parseOpcodeList` reads the big-endian payload of tags 51008 / 51009 / 51022 (`LibRaw::dng_levels.rawopcodes`) into `Opcode{id, version, flags, params}`; a truncated list keeps the ops before the fault and reports `error`. Unknown opcodes with the optional flag are skipped silently, unknown required ones are listed in `ApplyReport::skipped`; nothing makes the decode fail (corrupt-list test: decode still works, report `GainMap; skipped: malformed list (opcode 1 truncated)`). `SourceImage::meta.dngCorrections` carries the summary (`GainMap x4, WarpRectilinear, DefaultCrop`).
* **GainMap (9, OpcodeList2).** Per Bayer plane (`Plane`, `Planes`, `RowPitch`, `ColPitch`, area), bilinear interpolation of the gain grid with the spec's position rule (grid point (i, j) at `origin + i * spacing` relative to the image size, pixel at y / (h - 1)), edge replication, applied to the black-subtracted value (the black pattern of the raw buffer is passed in: LibRaw's common + per-colour + repeat-pattern black), then clipped to the white level. Applied in place to `raw_image` before `dcraw_process`.
* **FixVignetteRadial (3)**: gain `1 + k0 r^2 + ... + k4 r^10`, centre from the opcode, on the mosaic (list 1 / 2) or on the image (list 3).
* **WarpRectilinear (1, list 3)**: radial kr0..kr3 + tangential kt0, kt1 per plane set (one set, or one per colour plane for lateral CA), normalised by the farthest corner, destination to source mapping of the spec, bicubic resampling in float, edge clamped; then the DefaultCrop (origin / size rounded to even numbers so the half-size preview crops to exactly half) removes the invalid border, and the file orientation is applied last (the geometry runs in sensor orientation). OpcodeList1 (pre-linearisation), WarpFisheye and WarpRectilinear2 are reported as skipped.
* **Tests** (`tests/dng_opcode_tests.cpp`, ctest `dng.*`, synthetic DNGs written by the test; no public sample is in the repo): parsing of every opcode incl. truncation and the optional / required flag; a flat field through a GainMap comes out as the gain grid (interpolation, pitch phases, plane selection); FixVignetteRadial analytic; WarpRectilinear identity ramp (worst 0.0001 px) and a grid pattern against the analytic mapping (11 lines, worst centroid error 0.104 px); end to end through the decoder, a flat 'lens shading' DNG: 529 samples, worst relative error 0.0038 against the analytic gain (max gain 1.5); default crop + orientation against LibRaw's own crop: difference 0; corrupt list; illuminant solve (27.2).
* **Real phone DNGs** (public CC0 files from raw.pixls.us in `build/rawsamples`, never committed). Opcodes found: Pixel 6 Pro and 7 Pro (GainMap x4 + WarpRectilinear + DefaultCrop), Pixel 8 Pro (4 required GainMaps of 4876 bytes + optional WarpRectilinear), Xiaomi M2010J19CG (GainMap x4 + Warp + crop), Xiaomi Mi 9 Lite and Realme RMX1851 (GainMap x4 + crop), iPhone XS (FixVignetteRadial in list 3), Leica Q2 (WarpRectilinear in list 3, one OpcodeList1 op skipped), Samsung S23 / S23 Ultra / SM-S901E (LinearRaw, four identity 1x1 GainMaps: nothing to do, no longer reported as skipped), iPhone 12 Pro (ProRAW, no opcodes), OnePlus A6003, Leica M10, Pentax K-1 II (default crop only).
* **Corner brightness versus the embedded preview** (mean of four 8 % corner boxes over the centre 20 % box; the preview is tone-mapped by the vendor, so it is compared in display space after matching the centre): iPhone XS off 0.41, on 0.72, preview 0.97; Pixel 6 Pro 0.43, 0.79, 0.88; Pixel 7 Pro 0.46, 0.55, 0.50; Pixel 8 Pro 0.40, 0.68, 0.73; Leica Q2 0.48, 0.71, 0.65. In linear light the corners were 0.13 / 0.16 / 0.18 / 0.15 / 0.29 of the centre before and 0.39 / 0.54 / 0.28 / 0.45 / 0.50 after. The scenes are not flat fields (and the iPhone preview carries Apple's local tone mapping), so the numbers show direction and size, not a calibration. Straight lines after WarpRectilinear were **not** measured on a real photo (no suitable scene in the samples); the analytic grid test is the proof.
* **Cost** (desktop; preview / full decode): Pixel 8 Pro (50 MP) 0.94 -> 1.19 s preview, 3.0 -> 3.9 s full; Pixel 7 Pro (12 MP) 0.78 -> 0.95 s full. Once per photo. `DAPPLE_DNG_OPCODES=0` switches everything off (read once per process) for A/B.
* **Not done:** OpcodeList1, WarpFisheye, per-plane GainMaps on LinearRaw (every LinearRaw file seen carries identity maps), DeltaPer* / ScalePer* / MapTable / MapPolynomial / FixBadPixels (a required one is listed in `dngCorrections` as skipped), the GPU warp, a timing on a phone.

### 27.2 As-shot illuminant for any camera (`libraw_decoder.cpp`, `dng_illuminant.h`)

* The film_wb axis needs the scene illuminant. Paths in order: (1) a DNG with both calibration matrices and AsShotNeutral: the iterative solve of DNG specification chapter 6 (`solveIlluminantFromNeutral`: XYZtoCamera(T) = AB * CC * CM(T), CM / CC interpolated between the two calibration illuminants by inverse CCT, repeated until xy converges), source `dng-dual`; (2) a DNG that records only AsShotWhiteXY (Pixel 8 Pro: LibRaw ignores the tag and balances for an equal-energy white, so the decode was 5450 K instead of the file's 6020 K and `asShotWb` was 1, 1, 1): the tag is read from the first IFD, the illuminant is that point, the multipliers follow from the forward model of chapter 6 and the decode uses them (`user_mul`), source `dng-whitexy`; (3) the camera Kelvin table (LibRaw `WBCT_Coeffs`: Sony, Canon, Fujifilm, Olympus, Pentax), source `wbct`; (4) the single-matrix estimate, source `matrix` (Nikon, Panasonic, anything with one matrix and no table). `ShotMetadata::illuminantSource` names the path; `dapple info` prints it and `dump-linear` prints what the decode used.
* **Validation, synthetic:** the solve against its own forward model, 2300 - 9000 K: worst 0.0000 mired; a synthetic dual-matrix DNG at 3300 K through the decoder: 3312.0 K recovered (0.00 mired); the same light recorded as AsShotWhiteXY only: exact xy and multipliers.
* **Validation, real:** the Sony ARWs carry one matrix, so the dual solve does not apply to them and "dual versus WBCT within 5 mired on the Sony set" could not be measured; no real dual-matrix file has an illuminant measurement, so the values are only plausible (iPhone 12 Pro 5184 K, Pixel 7 Pro 6142 K, Pixel 6 Pro 3413 K, Leica Q2 7235 K, Samsung S23 4893 K). A few phones land well off the Planckian locus (Xiaomi M2010J19CG xy (0.346, 0.285), Pixel 6 Pro (0.381, 0.316)): their matrices or their AWB; the film_wb axis takes CCT and the tint offset, so a magenta estimate shows as tint. **Not verified against ground truth.**
* **Matrix fallback against the Kelvin tables on 20 real RAW** (Canon CR2 / CR3, Fujifilm RAF incl. X-T50, X-S10, X-A7, Olympus, Pentax, Sony; test hook `DAPPLE_ILL_MATRIX_ONLY=1` skips the table, procedure: `dapple info <file>` with and without it): matrix minus table in mired, mean -1.5, standard deviation 8.8, worst 15.2 (Pentax K-1 II -15, X-T50 +11, Canon R10 -12). There is no systematic cool bias left on these (the 20 - 39 mired of the Sony set came from warm-light frames); an uncorrected single-matrix estimate is about +-15 mired for makers without a table (Nikon D850 / Z 8, Panasonic S5 / GH6). Using the cameras' white-balance presets as an extra table for them was not done.
* Also fixed: monochrome DNGs (Leica M Monochrom, Pentax K-3 III Monochrome) failed with "Unexpected LibRaw output layout" (one-channel output); they decode now (grey replicated to three channels).

### 27.3 N200B versus the digital render after the target_print change

Not the paper. Gold 200 (N200B) has `target_print = kodak_portra_endura`, which was already its default paper: `--paper default` is bit-identical to `--paper kodak_portra_endura` today (dE 0.000), and the pack differs from the previous one only by the new PAIR section (all 31 STCK sections are byte-identical). The change is the removal of `film_cast` in 1088f2d: before, every stock except the reference was partially pulled towards Portra 400's neutral filter pack (`neutralMix`); now each stock keeps its own neutral calibration. Measured with a build of b353e30 against HEAD on DSC00111 (1200 px, `--auto-exposure match --no-spatial`, same explicit paper): Portra 400 (the reference, never pulled) dE2000 0.000, Gold 200 mean 1.41 (p99 2.61), Ektar 100 mean 4.32 (p99 8.05); print constants -0.46 -> -0.47 (Gold), -0.45 -> -0.47 (Ektar). So the N200B drift against the digital render (3.7 -> 3.9 and so on) is the intended consequence of "each stock's own neutral": Gold now shows its own warm / magenta cast instead of a Portra-like one. Correct; nothing to fix. (The three numbers of the report could not be reproduced exactly, the metric was not found in the repo; the mechanism and size were measured as above.)

## 28. Wave 8: device performance on a mid-range Adreno (device-perf engineer, 2026-11-12)

Device results and per-pass tables: `docs/engine/device-results/redmi-garnet.md`. The phone was the Redmi (SM7435, Adreno 710, 120 Hz); the APK could not be installed (MIUI asks for confirmation on the phone), so everything below is native (`/data/local/tmp/dapple`, big cores) and **no in-app number was measured**.

### 28.1 What was found

* On this GPU the film frame is not texture-bandwidth bound, it is ALU / register bound: dynamically indexed local arrays and integer divisions in hot loops cost far more than the fetches. Evidence: `reflectIdx` early-out (no division when the index is inside the image) took the level-0 `film_v3` from 6.0 to 4.5 ms; replacing the dynamically indexed `wE/wO` weight arrays of the Catmull-Rom upsample by two `vec4` picked once per pixel took it to 3.0 ms; explicit accumulators instead of `acc[4]` in `film_h3` took 1.88 to 1.63 ms. Tried and rejected on the device (measured slower or equal): 4 output pixels per invocation in `film_h3` (3x slower: register spill), a shared-memory tile for the upsample (6.3 vs 4.5 ms), staged `film_v3` (`DAPPLE_FILM_V3_DIRECT=0`: 18.5 vs 5.3 ms), workgroup 64 (30 % slower), the tetrahedral lookup without dynamic arrays (identical, reverted).
* Interactive tier, wide mixture components: components with sigma >= 2 (was 3) now go to pyramid level >= 1 in the interactive tier (`Mix3::fastLow`, `DAPPLE_FILM_FASTLOW=0` turns it off): level 0 keeps the sigma < 2 slots, and the 1.5 % tail (r 8) costs a quarter of the pixels. Interactive parity rows unchanged (halation interactive mean dE 0.017 p99 0.053). This is a cost change of the interactive tier only; the final tier is untouched.
* Cold GPU: the same 810x1080 frame takes 50.7 ms alone and 18.6 ms in 10 back-to-back repeats (ratio 2.7). A concurrent GPU load did not change the session-path time (17.5 vs 18.0 ms), so the steady drag cost is the frame itself; the 2.7x is the first-frames / idle effect and is not removable from native code. ADPF is created only for an app process (`adpf_session:false` for a shell binary), so its effect was not measurable here.
* Print-exposure calibration warm-up: one background thread on an efficiency core needed more than 10 s for the 15 buckets, so `film_shoot` drags a few seconds after choosing a stock paid ~190 ms per cold bucket. Two workers on the normal class (the big cores) now pull the buckets outward from 0: film_shoot median 191 -> 21 ms in the drag bench. Side effect: the workers share the process thread pool with the frame thread (jobs are serialised), so table-bake drags (black / print / warmth / tint) went from ~19 to ~23 ms while the warm-up runs.
* The film match key now contains vignette, nrColor and sharpRadius (they were missing): `filmMatchKey`, `filmMatchKeyLean`. Golden lines that legitimately changed: film/blend50_interactive, film/blend50_final, film/full_spatial_final, film/full_region, film/exact_blend (re-recorded by the previous engineer in this wave, `tests/engine/golden/corpus.txt`); `engine.match_cache_seq` (60 settled frames, 17 sequences, 0 mismatches) and the new key assertions in `match_cache_tests` pass.

### 28.2 The film check in the warm-up (GLES / driver correctness guard)

`warmFilm` renders the first interactive film frame through the GPU executor and the same frame on the CPU; mean dE > 1.0, p99 > 5 or max > 25 sets `filmWrong`, and the probe thread demotes the API exactly like a lost device (`excluded_`, Vulkan -> GLES -> CPU). Reported in the warm-up log line: `film check: mean dE 0.044 p99 0.75 max 1.67` on the Redmi for Vulkan and GLES. Not exercised: a failing driver (the Adreno 620 / 640 GLES bug of the other engineer was **not reproduced on the Adreno 710**: GLES passes film_gpu.parity, frames, tiling and the coverage run there). The persisted self-test verdict is still "pass" for that API, so a demoted device repeats the check (~0.7 s of background work) on every start.

### 28.3 Enlarger diffusion in a tiled GPU export

Stays on the CPU executor, as an explicit `Unsupported` reason that is counted (`EngineStats::gpuEnlargerTiled`): the filter blurs the whole frame with a mixture whose halo is 4 sigma of the widest component (up to the full image once the pyramid levels are involved), and the pyramid levels need windows aligned to the level's stride; a window form would re-render a halo larger than the tile for every tile. The message in `gpu_executor.cpp` says so.

### 28.4 Smaller items

* `DAPPLE_GPU_PROF_REPEAT` repeats now count towards the submission splitter (a 5 s submission lost the Pixel 5's device): `gpu_scheduler.cpp`.
* `dapple_match_cache_tests` and `dapple_golden` read the film pack from `DAPPLE_FILM_DATA` (a phone: the pushed folder), else the source tree.
* Adaptive quality (Dart): the drag budget is one display refresh (`AdaptiveQuality.budgetForRefresh`, clamped 8.3 .. 55 ms) instead of a fixed 55 ms, and a 360 px rung was added below 480; the rung is still chosen from the measured cycle time with hysteresis. Not measured in the app (no install).
* `dapple_drag_bench` takes `DAPPLE_BENCH_ADPF=<ms>`.

### 28.5 Not done

The micro-benchmark / per-device calibration seeding the quality choice, the ratio table across devices (only the Redmi was measured; the N5 numbers are the 2026-11-12 wave 6 ones), clock and Battery Saver logging with each measurement (the GPU frequency nodes are not readable without root on the Redmi), a backend choice by measurement, the interactive tier with CPU-side change (nothing CPU-side was changed, golden unchanged), and everything on the Pixel 5 (not used: battery 7-9 %, released late). The 1600 px interactive frame (76 ms) and the 810x1080 frame (14.7 ms incl. 2.8 ms readback) are far from the 8.3 ms budget: see the Redmi results for the floor and what adaptive quality does.

### 28.6 Round 3: print tables off the frame thread, Metal, film log

* **Background print tables** (`film_tables.cpp tablesFor`, `parallel.h ThreadPool::background()`): an interactive frame of a table-moving drag (`FilmResolved::coarse`) takes the coarse table of its own values when cached, else the most recent table of the same stock / paper / mapping / split path, and queues the wanted coarse table on a single latest-wins worker running on a second pool (half the capability-chosen cores). The settled frame is exact as before. A drag frame therefore shows the previous step's colour while the worker bakes (counted in `film_tables stale / baked / requested` of the `dapple_engine` line); `DAPPLE_FILM_ASYNC_TABLES=0` is the synchronous path. The print-calibration warm-up workers use the same background pool. Redmi: table drags 22-24 -> 9-10 ms at 480 px, film_shoot 21-35 -> 7-11 ms (`redmi-garnet.md` section 11).
* **Metal:** `Api::Metal` in `gpu_context.cpp` (candidates on Apple: metal, gles; JSON `api`; rebuild) and `engine_session.cpp` (`executor` string, GPU accounting).
* **Size sweep / 16 MiB step, mediump, cadence:** `redmi-garnet.md` sections 8 and 10. Open: raster passes, the warm-up micro-benchmark. The 16 MiB step is not an engine effect (it is the bench harness's RGBA32F source, `redmi-garnet.md` section 'Bands round'); `film_match*.cpp` is re-pointed to the engine.

### 29.2 Round 3 (2026-11-13): the shadow chroma floor is gated, capture exposure

* **Shadow chroma floor verified and restricted.** On 14 dark frames of the owner's A7R V library (the 10 with the most pixels below preview L* 10 plus the 4 darkest by mean; outputs only under build/), rendered with and without `suppressShadowChroma` (`DAPPLE_SHADOW_CHROMA=off`): the unconditional fade changed 12.6 - 39.9 % of the pixels by dE > 0.5 (dE76 inside the L* < 20 shadows: mean 0.32 - 2.25, worst frames DSC00597 / DSC00598 with 2.2 and 27 % of the pixels changed), lowered the shadow chroma by 0.2 - 1.8 C* on average and by up to 26 C* on single pixels. Those are legitimate colours (dark fabric, dusk foliage) with a real exposure behind them, so the rule is now gated by the statistical signature of an empty frame: the 99.5th percentile of the linear luma (1/16 subsample) must be below 0.03 of white; otherwise nothing is touched. None of the 14 frames passes the gate: the gated render is byte-identical to the render with the floor off on all 14 (cmp of the PNGs). The Note10+ dark frame (99.5th percentile far below 0.03) keeps its fix (mean RGB 35 / 21 / 31, chroma 16 against 60 / 40 / 60 and 55 before). A noise-floor based threshold (DNG NoiseProfile) was considered: it would help only DNGs and ARW has no equivalent field, the percentile gate works on every format. The golden corpus does not pass through the decoder and is unchanged. Side-by-side crops of the ungated version were not inspected by eye: the numbers above (lost chroma, changed pixel share) were the evidence, and the gate makes the images identical.
* **Capture exposure (Note10+ sample2.dng).** From the DNG's own EXIF: ExposureTime 0.25 s, ISO 977, f/1.5, BaselineExposure 0 (sample.dng, the wall: 0.14 s, ISO 1023, f/1.5). Samsung's auto exposure had already driven exposure and gain to their limits (ISO ~1000 is the top of the Note10+ raw range) and the sensor still delivered 0.9 - 2.1 DN above black: the scene was very dark, the raw is the truth about the light, and the JPEG of the same shot gets its brightness from the ISP's tone mapping / multi-frame processing, not from a different exposure. From the capture code (CaptureActivity.kt): RAW and JPEG are the same `ImageCapture` request (OUTPUT_FORMAT_RAW_JPEG), so AE and any exposure compensation are identical for both (no compensation is set, no vendor extension, no HDR / night extension, no zero-shutter-lag mode: `CAPTURE_MODE_MAXIMIZE_QUALITY`); CameraX, to my knowledge, runs its precapture sequence for flash / AF only and does not wait for AE convergence on a bare shot (not verified on a device), and our code did not either: the shutter could fire a few frames after the camera was bound, while AE was still searching. Fixed on our side: `CaptureSettle` delays a shot until 700 ms after the bind (screen start, lens switch, mode fallback), JVM test `CaptureSettleTest`. The Pixel 5 sample (build/dev2/pixel5/sample.dng, a black frame, 1/15 s, ISO 444) carries BaselineExposure +4.05 EV: the phone itself says its default render lifts that frame by four stops, which is the camera's doing (DngCreator writes the HAL's baseline). What is the camera's doing: the long, high-ISO raw of a dark scene and the brightness of its JPEG. Device check for the lead: shoot the same dark surface RAW+JPEG twice (immediately after opening the capture screen, and after 2 s); `dapple info --anchor` on both DNGs should print the same exposure (EXIF ExposureTime / ISO), the first one no longer darker; the engine's render of the DNG should land near the JPEG's mean L* (anchor tier 1, source reference-image).

### 29.3 Round 4 (2026-11-13): monochrome files solved (it was a geometry bug), arrays of the Sony lens data

* **Monochrome sensors: the "too bright" frames were a decode bug, not an exposure problem.** At preview quality (`half_size`) LibRaw halves the output of mosaic sensors only; a monochrome file (`filters` 0) comes out at full size, but the DNG default crop was divided by 2 for every preview decode, so both monochrome files (Leica M Monochrom, Pentax K-3 III Monochrome) were rendered as the top-left quarter of the picture, zoomed 2x (the render showed the sky and a tree where the camera preview shows a whole roof-top scene): the anchor then compared two different pictures, the per-pixel ratio had a spread of 0.9 - 1.5 EV and the preview was rejected. Fix (`libraw_decoder.cpp`): the crop divisor is 2 only when the raw width is at least twice the output width (`raw_width`, unchanged by `half_size`). Any other filters-0 file with a DefaultCrop was affected in the same way at preview quality.
* **Monochrome anchor** (`ShotMetadata::monochrome` = LibRaw `colors` 1; `exposure_anchor.cpp`): with the geometry right, the witness is the ratio of the median luminances of preview and raw (the camera's black-and-white rendering has a much steeper curve than sRGB: K-3 III preview 0.018 .. 0.86 linear against a raw of 0.46 .. 0.67), accepted when the two are rank correlated (Spearman 0.3 or more; wrong geometry gives -0.6 .. 0.05, the registered files 0.36 and 1.00); the preview is tried as is and turned by 180 degrees, black letterbox bars of the preview (the Leica thumbnail is a 4:3 frame around a 3:2 picture) are trimmed first, crushed preview blacks (linear luma below 0.004) are left out of the correlation and the median; the DNG tag stays a hint (tier 2 when the witness fails). The decode itself was already right: no colour matrix, one channel replicated to the three neutral channels, levels from LibRaw (K-3 III black 64 / white 16378, Leica white 3750).
* **Result** (`scripts/samples/anchor_table.py`, auto render against the preview, mean L*): Pentax K-3 III Monochrome dL +11.3 -> -1.1 (tier 1, +0.38 EV, rank correlation 1.00); Leica M Monochrom +20.4 -> -1.1 (tier 1, -0.74 EV, correlation 0.36 against a 160 x 120 thumbnail with crushed blacks). The correlation of the Leica thumbnail is low because the thumbnail is tiny, blocky and mostly black: a precise explanation of why it is the weakest witness in the set; the geometry was checked by eye (side by side image build/w8b/lm_cmp.jpg) and the median match lands the mean L* within 1.1.
* **Sony ARW lens correction data (what the file carries; no semantics guessed).** The raw SubIFD of every Sony ARW looked at (A7R V DSC00111, A6700, A7 IV) holds the correction parameter arrays of the camera's own JPEG engine: tag 0x7031 (value 272 on all three: vignetting correction setting), 0x7032 (17 int16: the first is the knot count, 16 or 11, then a monotonic table: A7R V `0 0 32 64 128 192 288 384 512 640 784 928 1456 2608 4000 5472`), 0x7034 / 0x7035 (chromatic aberration setting 1, 33 int16: knot count then two interleaved channel tables, e.g. A7R V `896 640 512 512 384 ...`), 0x7036 / 0x7037 (distortion setting 1 or 17, 17 int16: knot count then a table: A7R V `0 0 0 1 2 4 5 7 9 11 13 16 18 21 22 22`, A6700 `0 -29 -109 -245 -424 -640 -881 -1135 -1390 -1631 -1829 ...`, i.e. strongly negative: the lens needs barrel correction, which explains the A6700 frames being dark and blue in the corners), 0x7038 / 0x7039 (two crop rectangles / scale data). LibRaw does not decode them and the tables have no public specification (ExifTool lists them as raw arrays); applying them generically (a radial gain, a radial distortion and a per-channel radial shift, each knot being a fraction of the image half-diagonal) is plausible but the units and the knot positions would have to be calibrated against Sony's own JPEG output on a lens chart: not done, nothing was applied.

## 30. Wave 8: Motion Photo clips through the node graph (motion-photo engineer, 2026-11-12)

Requirement REQ 4.9.5 / 4.9.5a, roadmap stage (e) of section 15. A Motion Photo's clip follows the still's edit, film simulation included: **the same graph, the same executors, a different source**. Code: `core/engine/video/` (`VideoFrameSession`, C API, JNI), `cli/motion_render.cpp` (`dapple motion render`), `tests/video/video_session_tests.cpp`, Kotlin `ClipEdit.kt` + `SoftwareMedia.clip(edit)`.

### 30.1 Design

**Source and working space.** `core/video` already delivers `DV_RGBAF16`: half-float linear RGB, BT.709 primaries, 1.0 = SDR reference white, values above 1 for HDR. It is produced from the stream's own signal at >= 10 bit (swscale to RGB48, then the transfer table: sRGB EOTF for SDR, HLG inverse OETF, PQ EOTF, S-Log3 -> scene linear with S-Gamut3.Cine -> BT.709 in the matrix; no tone map in this format, that is `DV_RGBA64`'s job). That frame is the engine's `SourceImage`: one 3x3 (Bradford, `color::srgbToProPhoto`) into linear ProPhoto D50, exactly what the JPEG decoder does. The input transform "nodes" are therefore the video core's table plus this matrix, outside the graph (they are per-source, not per-edit, and cheap); nothing between the decoder's 10-bit samples and the graph is 8-bit. `SourceImage::sceneReferred`: false for SDR (1.0 = display white, a JPEG), true for S-Log3 / HLG / PQ (18 % grey = 0.18 like a RAW, so the base tone curve applies; HLG / PQ are relative to 203 nit = 1.0, which makes their 18 % grey 0.18 as well). A clip that carries a container rotation is rotated upright before the graph (the edit's geometry is defined on the upright still, the muxer rotation hint becomes 0); a clip of another aspect than the still is centre-cropped to the still's aspect first (never stretched), so a crop of the edit lands on the same part of the scene.

**Nodes applied.** All of them: Gain, the colour / tone point chain (lattice, tone table), the film nodes (exposure, tables, dye-cloud blur, micro-structure, grain, halation, print, scan), LUT, vignette, noise reduction, dehaze, clarity / texture / sharpen. Nothing is skipped for video because the CPU executor does a 1080p frame in 20-45 ms (section 30.5); the cheapening knobs exist (`Config::spatial`, `bakeSize`) for slower devices. The still's per-frame stochastic stages: **grain and the emulsion micro-structure follow `ctx.frameIndex`** (stateless hash, so frame n is bit-identical on every render, consecutive frames are decorrelated). **The print flare noise is held** (`pd->streamGlare` always uses frame index 0's stream, `film_tables.cpp`): it belongs to the print / enlarger, and a per-frame low-frequency flare would shimmer. A still (frame index 0) is unchanged.

**Temporal stability: held from the reference frame, never per frame.** `VideoFrameSession::setReference(frame)` (the cover frame of the Motion Photo, or the still itself via `setReferenceStill`) freezes:
- the film exposure match (`matchFilmExposure` on the reference's 256 px proxy: filmKeyEv / filmPrintEv) into the clip's `EditParams`, once;
- the table nodes (lattice, tone analysis `Analysis`, tone table, film tables): they sit in the session's `ResultCache` under keys that do not depend on pixels, so every later frame hits what the reference built (`FrameResult::bake == FullHit`, `tablesHit`; the test asserts no lattice / analysis / tone table is rebuilt);
- the frame-global solves (dehaze airlight + transmission grid, guided-filter base): their node keys inherit `ctx.frameIndex` through the film grain upstream, so by default a solve would be rebuilt every frame (found by the `hold` test: 1 build per frame). `Graph::computeKeysHoldingSolves` replaces the keys of `Dehaze` / `Detail` by those of the same frame with index 0; `FrameJob::holdSolves` turns it on. Frame B of the test then hits the reference's solve (hits 1, builds 0).
A cloud (the light drops to 35 %) therefore darkens the clip the way it darkens the scene instead of being metered away; the test shows the held render at mean 0.206 against 0.361 when the film is re-metered on that frame. `setEdit` re-meters on the held reference when the user moves a slider.

**Timing.** The graph has no notion of time; frames carry the decoder's presentation timestamps (`core/video` caps the rate at 30 fps and keeps the frame closest to each tick), the session only gets the 1-based frame ordinal for the grain. The clip keeps its container timing.

**Output and the encode side.** `VideoFrameSession::render` returns RGBA8, **I420** (8-bit BT.709 limited, 8x8 ordered dither) or **P010** (10-bit BT.709 limited, value in the top 10 bits, no dither needed at 10 bit) packed from the executor's clamped float frame (`FrameJob::outFloat`): 10 bit out is the CPU executor's float result quantised once at the end. Chroma is a 2x2 box average of the display-encoded Cb / Cr. The encoder is the capability layer's pick (HEVC Main10 rung for P010, 8-bit rungs for I420, `compat` forces 8-bit), tagged BT.709 / SDR: film output is SDR by nature (section 15), and an edited clip is display referred, so HDR static metadata is not written for it (the unedited pass-through keeps its HLG / PQ tags and metadata as before). An Ultra HDR still stays the still's business.

**GPU.** The GPU executors write RGBA8 only. `Config::allowGpu` + RGBA8 / I420 sends the frame through `FrameJob::store` to the GPU executor (parity with the CPU frame on the desktop Vulkan device: mean 0.09 codes, max 2; the 8-bit result is packed to I420 afterwards). The 10-bit path is always the CPU executor: a float / 16-bit GPU target does not exist yet (HAL change: a `RGBA16F` final pass and read-back, plus the film display stages writing it), listed under "not done". Zero-copy of a hardware decoder buffer into the graph is not done either (the frame reaches the graph as a CPU array: P010 from `AMediaCodec` is converted to RGB48 / half floats by swscale).

### 30.2 What landed

| Piece | File |
|---|---|
| Session: conversion (`toSource`), held state, render / pack | `core/engine/video/video_frame_session.{h,cpp}` |
| C API `dapple_video_edit_*` (+ `dapple_c.h` block) and JNI `DappleVideoEdit` | `core/engine/video/video_c_api.cpp`, `video_jni.cpp` (compiled into libdapple) |
| Float output, solve hold in the engine | `engine_session.{h,cpp}` (`FrameJob::outFloat`, `holdSolves`, `FrameResult::floatOut`), `graph.{h,cpp}` (`computeKeysHoldingSolves`), `film_tables.cpp` (held flare stream) |
| Desktop video core: pread / O_BINARY shims, `dapple_video_host` (Windows FFmpeg from `tools/ffmpeg/build-host.sh`; the script's `make` path bug fixed) | `core/video/dapple_video.cpp`, `core/video/host.cmake`, root `CMakeLists.txt` |
| `dapple motion render <video> -o out.mkv --film ...` (decode, graph, FFV1 8 / 10 bit; `--dump-ppm`, `--stats`, `--gpu`) | `cli/motion_render.{h,cpp}` |
| Tests, ctest ids `video_session.*` | `tests/video/video_session_tests.cpp`, `tests/video/cli_motion_render.cmake` |
| Android: `EditSpec`, `DappleVideoEdit` (JNI face), `SoftwareMedia.clip(edit)`, `LiveBridge.trim(edit)`, method channel `trim.edit`, Dart `EditedClip` | `ClipEdit.kt`, `SoftwareMedia.kt`, `LiveBridge.kt`, `MainActivity.kt`, `app/lib/services/live_bridge.dart` |

Android flow for an edited clip: `EditRun.start` opens the native session, decodes the reference frame (`EditSpec.refMs`, the cover frame, else the first frame), freezes the session and thereby learns the output size; the encoder is configured with that size; per frame `SwVideo` decodes `RGBAF16`, the session packs P010 / I420 into the encoder input buffer. `edit == null` is the unchanged pass-through.

### 30.3 Measured (desktop, Windows, llvm-mingw, AVX2, `build/w8c`)

- Oracle: frame N through the session vs the still pipeline on the same pixels and edit (5 edits: default, tone + crop, dehaze + clarity + sharpen, film negative, film slide + rotate): **max difference 0 codes** (8-bit); vs the exact variant (per-pixel tone + colour) mean 0.00-0.06 codes, max 7 (the baked path's own approximation).
- 10 bit in, 10 bit out: a 10-bit HEVC gradient with 652 distinct luma codes comes out of the graph (default edit) with 641 distinct P010 codes (an 8-bit path: 219); a 4:2:2 10-bit ramp 876 -> 850. Through `dapple motion render` with Portra: 746 codes (grain adds codes).
- Flicker: 10 identical frames, film on, grain off: max frame-to-frame difference **0**, mean luminance range 0.
- Grain: grain rms 0.0021 display units per frame, correlation of (f1 - f2) with (f3 - f4) -0.0003, mean level moves -0.0005; the same frame index is bit-identical.
- Hold: no table rebuilt for frame B (lattice, analysis, tone table), dehaze solve built once and hit once, film EV unchanged.
- CPU performance, 1080p, per frame (synthetic frame; convert + graph + P010 pack): tone + colour 21-33 ms, + clarity / sharpen / NR 31-36 ms, film Portra with grain 42-47 ms (reference frame once: 23 / 45 / 150 ms). On the real Sony clip D:/.../C0021.MP4 (1920x1080, 50 fps, 10-bit 4:2:2 H.264) with Portra: graph 28 ms/frame, software decode 66-78 ms, FFV1 encode 75-88 ms: decode and the test encoder dominate, not the graph.
- GPU (desktop Vulkan device, 8-bit I420 out, film Portra): 45.7 ms/frame for the graph incl. the per-frame source upload against 28 ms on the CPU: for video the CPU executor wins on the desktop (the source changes every frame, the upload is not amortised). Not measured on a phone GPU (no phone, see 30.5).
- End to end: `dapple motion render` on the 10-bit HEVC fixture, an S-Log3 fixture, an 8-bit fixture, the Sony clip (51 frames), and a 40-frame 1080p S-Log3 / SDR pan made from a photo (`build/w8c/data`, visual check: S-Log3 and SDR agree, Portra look as on the still).

### 30.4 Gates

`ctest` `video_session.{convert,oracle,hold,depth10,flicker,grain,perf,e2e,cli_motion_render}`; JVM `ClipEditTest`; Dart `edited_clip_test`; emulator run in 30.5.

### 30.5 Not done / not verified

- **No phone.** Hardware Main10 decode -> graph -> hardware Main10 encode, timing and thermals on a phone are unverified. Checklist for the device engineer (APK with this change, x86_64 emulator results below):
  1. Install the build; in logcat (`adb -s <serial> logcat -s DappleLive dapple_engine`) export an edited Motion Photo of a 10-bit clip (Sony S-Log3 or HLG) with a film stock: expect `clip ... edited=true`, `rung HEVC Main10` (or the demotion reason), `decode mc:<codec> (hardware)`, `codecSummary` with the encoder name, and `dapple_engine: frame ... exec=cpu` lines (the clip frames are CPU frames by design, `tier=full`).
  2. `adb shell dumpsys media.metrics` / `MediaCodec` output profile: the muxed track must be HEVC Main10 (the existing `EncoderUnreliable` check enforces it); pull the file and `dapple motion --info` / `ffprobe` it: 10-bit, BT.709, rotation 0, frame count = window x fps.
  3. Timing: the `clip ... elapsedMs` of a 3 s / 30 fps window; expect decode + graph (28-45 ms desktop 1080p, scale by the phone's CPU, see section 25 for the N5) + encode; report `elapsedMs`, thermal state before / after (`dapple_thermal_headroom`), any `slow frame` lines.
  4. Compare the clip's cover frame with the still render of the same edit (the pair must look the same: same film exposure, same colour; grain differs by design).
  5. Pass-through: export without an edit and confirm the old log lines and a byte-for-byte unchanged behaviour (HLG / PQ tags and HDR static info present).
- Emulator coverage and what it cannot show: see the emulator paragraph below.
- The 10-bit output of the GPU executor (float / RGBA16F final target) and a zero-copy hardware-buffer entry are not implemented (30.1).
- The editor does not pass `edit` yet: `LiveBridge.trim(edit: EditedClip(...))` is the plug-in point for `app_state.dart` (the owner of the editor flow supplies `paramsJson` of the cover's edit, `refMs` = the cover time, `stillAspect`, `stillLongEdge`, the as-shot illuminant and the camera); `app_state.dart` was not touched.
- Audio is not touched by the graph (copied / transcoded as before).
- A clip whose frame size changes mid-stream returns an error from `VideoFrameSession` (Motion Photo clips do not change size).

### 30.6 Emulator run (x86_64, own emulator on port 5570, stopped afterwards; no phone touched)

- Native: `dapple_video_session_tests` built for Android x86_64 (`build/w8c-droid`, NDK r28, FFmpeg x86_64 shared) and run on the emulator: convert, oracle (5 edits, max diff 0), hold, depth10 (652 -> 641 codes, ramp 876 -> 850), flicker (0), grain, e2e (4 fixtures through the graph), perf all pass with numbers identical to the desktop. Emulator CPU, 1080p, per frame: tone + colour 44 ms, + spatial 63 ms, film Portra 112 ms (graph 91). `libdapple.so` with `video_jni.cpp` links.
- Kotlin / MediaCodec: debug APK (x86_64) with a debug-only `DebugClipReceiver` (`app/android/app/src/debug`): `adb -s <serial> shell am broadcast -n com.dapple.dapple/.DebugClipReceiver -a com.dapple.dapple.DEBUG_CLIP --es uri <video> --es dest <out.mp4> --el startMs 0 --el durMs 1000 [--es paramsFile <EditParams json> --es pack <dapple.dfp>] [--ez compat true]`, result under logcat tag `DappleDebugClip`.
  - S-Log3 10-bit 4:2:2 H.264 1080p, Portra, `edited=true`, software decode, `H.264 High 8-bit` encode (the emulator has no Main10 encoder; `main10Failure=null`), 26 frames, 27 `film=on` frames in `dapple_engine`, 47 s on the emulator CPU (first film bake included); pulled and decoded: 1920x1080, film look as on the desktop.
  - 8-bit H.264 640x360, Portra: `decode h264_mediacodec (hardware)`, 16 film frames, edited output 640x360.
  - 10-bit HEVC 1080p, tone + `crop: 1:1`: encoder size 1080x1080 (the edit's geometry), software decode.
  - Pass-through (no edit): unchanged lines (`edited=false`, same codecSummary as before).
- Cannot be shown on the emulator: hardware Main10 decode / encode with the graph in between (the emulator has neither), real timing, thermal. That is the checklist in 30.5.

### 30.7 Codec layer changes (coordinator findings from the Note10+, 2026-11-12)

1. **Hardware Main10 decode was never tried on the Note10+** because `Ladder.decode` required `P010` in the decoder's colour list and `OMX.qcom.video.decoder.hevc` declares Main10 but no P010 (note10plus.md section 5). Now every hardware decoder that declares the stream's profile is a candidate (codecs that list P010 first); the self-test (`CodecSelfTest.decode10`, unchanged mechanics: asks for P010, reads the first frame) decides, and its verdict is the pure `Decode10.verdict`: pass only for P010 output with more than 256 distinct luma levels; any other layout fails with the format named, e.g. `delivers colour format 0x7fa30c04 for a 10-bit stream; the engine converts only P010 (declared: Flexible,0x7fa30c04,Surface)`, after which software takes over and the diagnostics page shows the reason. `CodecManager.decodeFor` / `runAll` / the encode self-test's decode-back use the same candidate rule. If the Note10+ decoder delivers a vendor 10-bit layout, the page now tells which one; converting it (or a Surface path) is then a defined next step, not guesswork.
2. **No HEVC Main10 encode row on the Note10+**: the Main10 encode rung required P010 too, so `OMX.qcom.video.encoder.hevc` (Main + Main10HDR10, Surface / Flexible / SemiPlanar / vendor inputs, no P010) never reached a self-test. Now a declared Main10 or Main10HDR10 profile makes the encoder a candidate (behind encoders that list P010; a codec that declares only Main10HDR10 is asked for exactly that profile, as the log of the `m10` tool suggested: asking for Main10 makes it reset the profile). The self-test requests P010 input and Main10; its result (pass or the configure / output error text) is stored and shown. The diagnostics page has a section **HEVC Main10 encode** that always exists (`main10Encode` in the report; `Diagnostics.main10EncodeRows`): one row per HEVC encoder with state `not declared` / `declared, not tested yet` / `passed` / `failed` and the reason (profiles and inputs when not declared or untested; the self-test detail otherwise); a device with no HEVC encoder gets one row saying so. **Surface-input Main10 encode is still not implemented** (the production path feeds P010 buffers): if the Note10+ encoder only accepts Surface input, its row will say `failed` with the configure error, and the Surface feed (EGL into the encoder's input surface) remains a separate work item.
3. Tests: `CodecLadderTest` has a fake Note10+ (declarations copied from note10plus.md: the Qualcomm encoder / decoder, software codecs) and checks the decode candidate, the vendor-format verdict and demotion to software, the encode candidate order and HDR10 profile, the always-present row in all four states, and the no-HEVC-encoder device. The previous expectations that a decoder without P010 is software were changed on purpose.

On-device check (Note10+, debug or release APK; settings -> about -> long-press the version -> codecs -> "all retest"; or `adb logcat -s DappleCodec`):
- Page section **HEVC Main10 encode**: a row for `OMX.qcom.video.encoder.hevc` with a state other than "declared, not tested yet" and a reason; copy the text of the page (the copy button) into the device results.
- Self-tests list: a `dec10  OMX.qcom.video.decoder.hevc` line, PASS (then `decodeLadderHevc10` starts with `hardware 10-bit (P010) · OMX.qcom.video.decoder.hevc`) or FAIL with the delivered colour format in the detail (then `decodeLadderHevc10` is software only, now with the reason on the page). Report that format number.
- `enc10  OMX.qcom.video.encoder.hevc`: PASS or FAIL with the reason. If PASS, an edited / unedited export of a 10-bit clip must say `rung HEVC Main10` and the muxed track must be Main10 (`dapple motion --info`, ffprobe).
- Logcat lines to attach: `DappleCodec: self-test PASS|FAIL (ms): ...` for both tests, and the `DappleVideo` lines `native decoder output: color-format N ...` if the decoder was opened by a job.

### 30.8 HDR policy (wave 8, decided from requirements 4.9.5 / 4.9.5a)

Requirements: the master keeps the material's colour depth (10-bit, HEVC Main10) without truncation; HDR sources stay HDR in the master; film is a print (SDR by nature, 4.9.5a); 8-bit only for sharing targets (`compat`). Decision, per case (`VideoFrameSession::decideOutput`, tested by `video_session.hdr`, CLI `--hdr off`, option `hdr` of the C API):

| Case | Output | Metadata |
|---|---|---|
| No edit (`edit == null`) | pass-through, unchanged (the decoder's own tags, HDR static info) | as before |
| Edit, SDR source (Rec.709 / 2020 SDR) | 10-bit BT.709 SDR, Main10 | BT.709 / BT.709 / BT.709, limited |
| Edit, S-Log3 source (no film) | 10-bit BT.709 SDR: the unedited clip is already converted to SDR for S-Log3 today (display transform in the video core), the edit keeps that | BT.709 |
| Edit, **HLG / PQ source, no film, identity geometry** | **stays HDR**: 10-bit HLG / PQ on BT.2020 NCL, limited range | primaries 9, transfer 18 (HLG) / 16 (PQ), matrix 9, HDR static info of the source |
| Edit, HLG / PQ source with film | SDR by nature (a print), written as **10-bit** BT.709 Main10 (no 8-bit step, so no banding) | BT.709 |
| Edit, HLG / PQ source with crop / rotate / straighten / flip | SDR 10-bit BT.709 (the highlight headroom is not warped) | BT.709 |
| `compat` (sharing version) or an 8-bit rung | 8-bit I420 BT.709 SDR | BT.709 |

How the HDR output is made. The graph is display referred (its base curve compresses highlights into 0..1), so the result of the edit is the SDR picture. For an HDR source the output adds back what the SDR render had to clip: `HDR linear = linear(graph result) + r * source`, with `r = max(0, 1 - 1 / (Y * 2^exposure))` (the luminance fraction above SDR white, so the hue of a highlight is the source's), converted to BT.2020 and encoded with the HLG OETF (scene linear x 0.265, 203 nit = 75 % signal as in the decoder) or the PQ OETF (nits = linear x 203). Below SDR white the picture is the edit's look; above it the source's highlights stay (an HLG ramp to 100 % signal comes out at the same code at 94 % of the width: 882 against 888, max code 936; exposure +1 EV moves the HDR frame's mean Y code 489 -> 584). The excess is computed from the working-space source, so it needs the output to have the source's geometry: any geometry edit therefore falls back to SDR (in the table). Pixels are packed once at the end (P010 `P010Hdr`, RGB10A2 `Rgb10A2Hdr`). Tests (`video_session.hdr`, `video_session.cli_motion_render`): output transfer / primaries per case (the stream tags read back through the video core: 9/18, 9/16, 1/1), distinct luma codes (HLG 605, PQ 473 of a 640-wide ramp: PQ is clamped at the decoder's 8x range, SDR 641, film 805), the HLG highlight round trip, exposure acting on the HDR frame.

Kotlin: `EditRun` reads `outputKind` / tags after the reference frame; the encode job's `hdrSource` follows it (so the ladder asks a Main10HDR10-only codec for that profile), the muxed format carries BT.2020 + the transfer + the source's static info, and an 8-bit rung always gets SDR frames. Not verifiable on the emulator (no Main10 encoder): the on-device item is below.

### 30.9 Surface-input Main10 encode (wave 8)

Many hardware HEVC encoders take 10-bit only through the input Surface. Two input modes, both behind the capability layer and both self-tested separately (`enc10` buffer P010, `enc10s` surface), chosen per codec by what it declares (no device names):
- **BUFFER**: P010 `ByteBuffer` input (`CodecInput.fill`). Offered to every Main10 / Main10HDR10 encoder; a codec that does not list P010 is still tried once, the self-test decides.
- **SURFACE** (`SurfaceInput.kt`, `SurfaceFeeder`): the session packs the frame as `Rgb10A2` (or `Rgb10A2Hdr`), it is uploaded as a `GL_RGB10_A2` texture and drawn as one quad into an EGL window surface on `MediaCodec.createInputSurface()`. The EGL config is chosen from what the device offers: R10 G10 B10 A2 recordable ES3 config (a device without one fails the self-test with `no 10-bit EGL surface`, it is never silently an 8-bit stream). The encoder converts RGB to YUV using `KEY_COLOR_STANDARD / RANGE / TRANSFER` of the format. Only offered when the codec declares `COLOR_FormatSurface` and the job comes from the engine session (`EncodeJob.surfaceOk`, i.e. edited clips: the pass-through clip has P010 from the decoder and no use for it).
Ladder order per codec: the mode that passed first; untested: buffer first when the codec lists P010, surface first when it does not. A failed mode is removed, both failed = the next rung (8-bit HEVC). The self-test of the surface mode encodes 8 frames of a 10-bit grey ramp, requires the output profile to really be Main10 and the stream decoded again to show more than 256 luma levels, like the buffer test.
Diagnostics: the always-present `main10Encode` rows now say per encoder which input mode passed, e.g. `input mode passed: surface. buffer P010: failed (configure failed ...); surface: passed (surface input, 10-bit EGL, 8 frames, profile Main10HDR10, 640 levels)`; `encodeLadder1080p30Edited` lists the picks with `[surface input]`; the job result carries `inputMode`.
Verified here: the Surface plumbing (EGL on the encoder surface, texture upload, presentation times, EOS through `signalEndOfInputStream`, decode back) runs on the emulator with the 8-bit H.264 encoder (`DebugClipReceiver --ez probe true`: `surface probe c2.android.avc.encoder: PASS surface input, 8-bit EGL, 8 frames, 200 distinct luma levels`); the emulator has no Main10 encoder, so the 10-bit EGL config and the real Main10 result are on-device items. Ladder / diagnostics logic: `CodecLadderTest` (surface only offered to jobs that can feed one, per-mode verdicts and demotion, the row in every state).

### 30.10 The video anchor (answer to the level shift) 

Scene-referred video (S-Log3 / HLG / PQ) is anchored explicitly: 18 % grey = 0.18, `kVideoBaselineEv` = 0 EV, no baseline contrast, no baseline HSL, no fitted colour match: `VideoFrameSession::videoCamera()`, used for every clip whatever the still's camera (the still's `Config::camera` is only kept for its id). Nothing of the RAW path applies: the per-file exposure anchor (`ShotMetadata::exposureAnchor*`, section 28; video frames have no embedded preview and no DNG tag) and the registry's generic profile are never read. Confirmed cause of the e2e level shift: before the explicit profile the clip used the registry's generic profile, so changes to that profile / baseline moved the S-Log3 and HLG test clips (mean level 0.511 / 0.532 -> 0.547 / 0.613); with the anchor fixed they are 0.511 / 0.528 again and cannot move with RAW changes (test `video_session.anchor`: a still's camera with +1.5 EV baseline gives the identical frame).

### 30.11 10-bit on the GPU and a zero-copy decoder path: design and hunk list (not implemented here; the GPU executor belongs to device-perf)

Today the GPU writes RGBA8 only (`w.o8`, final pack pass, read back by `copyImageToBuffer`), so 10-bit and HDR clips run on the CPU executor: 28 ms per 1080p frame on the desktop with film (21-33 ms without), see the emulator number below for a phone-class CPU; a 3 s clip at 30 fps is 90 frames.

**A. A 10-bit / half-float output target** (frame stays on the GPU until the encoder takes it):
1. `hal/hal.h`: no new `Format` is needed for RGBA16F (exists; `caps.formats[RGBA16F].storageWrite` is required). RGB10A2 needs `Format::RGB10A2` (append before `Count`: order is ABI), `bytesPerTexel` 4, `FormatCaps` probed like the others, Vulkan `VK_FORMAT_A2B10G10R10_UNORM_PACK32` (storage image support is optional: probe, else write RGBA16F and pack on the CPU), GLES `GL_RGB10_A2` (storage writes need ES 3.1 + `GL_RGB10_A2UI` / `rgb10_a2` image layout: probe).
2. `exec/gpu_iface.h`: `GpuTarget` gets `enum class Out { RGBA8, RGBA16F, RGB10A2 } out = RGBA8;` and `GpuOut` gets `std::uint16_t* rgba16 / std::uint32_t* rgb10` (or one `std::uint8_t* pixels` + `Format fmt`).
3. `exec/gpu_executor.cpp`: the final image (`w.o8` today, created at the end of `Walk`) is created in the requested format; the `copyImageToBuffer` readback and the `std::memcpy(px, readback.mapped, bytes)` at the end of `run` use `bytesPerTexel(fmt)`; the readback buffer size (`ensureReadback`) follows.
4. Shaders: the last pass that quantises (`film_fin.comp` / `point.comp` epilogue, the `PACK8` path of `common.glsl`) gets a spec constant `OUT_FMT` (0 = 8-bit as today): for RGBA16F store the clamped display-encoded float, for RGB10A2 `packUnorm` of x1023 with the alpha 3. The dither stays only in the 8-bit path. The film grain epilogue (`film_grain.comp`) writes the same.
5. `film_gpu.cpp`: the film final stage that writes `o8` follows the same spec constant (the film's display stages are already float until the pack).
6. Session side (mine): `VideoFrameSession::render` for `Rgb10A2` / `P010` asks `FrameJob` for the GPU target in RGBA16F / RGB10A2 and packs P010 on the CPU from RGBA16F (the HDR transfer needs the CPU excess blend anyway, or a second `OUT_FMT` mode of the same shader for the PQ / HLG encode), `gpuUsable` drops its `outFloat` exclusion for this target. Gate: the oracle test against the CPU float frame (max 1 code at 10 bit), the flicker / hold tests on the GPU, `gpu_tests --film-tiling` unchanged.
7. Cost to expect: the per-frame source upload dominates on a phone too (desktop: GPU 45.7 ms against CPU 28 ms because the source changes every frame); the gain exists only with the zero-copy input of B, or when the CPU is much slower than the GPU (phones: section 25 shows the GPU 3-10x faster than the A55/A76 CPU on film frames).

**B. Zero-copy from a hardware decoder buffer** (decoder surface -> graph, no CPU conversion):
1. Decode to a `Surface` backed by an `AImageReader` (`AIMAGE_FORMAT_PRIVATE` or `YCBCR_P010`, usage `GPU_SAMPLED_IMAGE`) instead of ByteBuffer output; `core/video`: new `hwPref` route `HW_NATIVE_SURFACE` (the NDK decoder `AMediaCodec_configure` with the reader's `ANativeWindow`) delivering `AHardwareBuffer*` per frame.
2. `hal/hal.h`: `struct ExternalSource { void* ahb; Format fmt; ...}` and `Device::importExternalImage(const ExternalSource&, Image*)`: Vulkan `VK_ANDROID_external_memory_android_hardware_buffer` (+ `VK_KHR_sampler_ycbcr_conversion` for P010 / private YUV, a `VkSamplerYcbcrConversion` with the stream's matrix / range), GLES `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)` + `GL_OES_EGL_image_external_essl3`. Capability flag `caps.externalImageRead`.
3. `exec/source_store.h`: a `HardwareBufferSourceStore` (`window()` returns null: a CPU read is not possible; `image()` null; identity per frame); the GPU executor's Resize / source node samples the imported image and performs YUV -> RGB -> linear (HLG / PQ / S-Log3 transfer as a small shader, the same formulas as `core/video`'s tables, validated against the F16 path) -> ProPhoto in one pass, replacing the upload of `RGBA16F` and the CPU half-float conversion (`toSource`: 5-10 ms per 1080p frame).
4. Tables / analysis / film match need pixels only once: the reference frame is still read on the CPU (decode that one frame to ByteBuffer).
5. Gate: parity of the GPU-converted frame with the F16 CPU path (max 2 codes), the hold / flicker tests, and a decoder-lifetime test (the `AHardwareBuffer` must outlive the GPU frame: release after the fence).

### 30.12 Cost of the CPU path for a 3 s 1080p clip

Desktop (AVX2, Windows): 90 frames x (convert 5 + graph 28 with film + P010 pack 4) ~ 3.3 s for the graph; the real Sony clip through `dapple motion render`: 51 frames in 10.4 s including the software decode (66-78 ms per frame, 10-bit 4:2:2 H.264) and the FFV1 test encoder (75-88 ms). On the x86_64 emulator the same code is 44 ms (tone + colour), 63 ms (spatial) and 112 ms (film) per frame in `video_session.perf`; the whole Kotlin export of a 3 s / 75-frame 1080p clip with film (software decode of 10-bit HEVC, the graph, the software H.264 encoder, mux) took **75.8 s** on the emulator, 75 frames (graph 707 ms per frame, software decode and the software H.264 encoder included). That APK is a debug build, so libdapple is compiled -O0: the same graph built -O3 is the 112 ms per frame of `video_session.perf` above (6-7x faster), which puts the same job near 12 s on this emulator; the -O0 figure is what a debug APK costs, not what a release app costs. (A cached background process is frozen by the OS after the broadcast ends: the first attempts stalled that way, tooling only; the debug receiver needs the app to stay alive, e.g. started from the foreground.) A phone's A76 / Oryon cores are several times faster than this emulator on the graph and have a hardware encoder, so the graph is expected near 30-60 ms per frame (about 3-6 s for the clip) on the phones measured in section 16 / 25, and the device engineer should record `elapsedMs` of the real job (checklist 30.5).

### 30.13 On-device checklist (updated, hardware Main10, edited clips)

Debug APK (the receiver is debug only); `adb -s <serial>` always explicit; clips pushed to `/sdcard/Download`, film pack `core/data/film/dapple.dfp` next to them.
1. Codec page (settings -> about -> long-press the version -> codecs -> all retest): section **HEVC Main10 encode** rows per encoder with the input mode that passed (`input mode passed: surface` / `buffer P010 + surface`) or both failure reasons; `dec10` line for the hardware Main10 decoder (PASS, or FAIL naming the delivered colour format); `encodeLadder1080p30Edited` shows `[surface input]` picks. Or without the UI: `am broadcast -n com.dapple.dapple/.DebugClipReceiver -a com.dapple.dapple.DEBUG_CLIP --ez probe true`, logcat tag `DappleDebugClip` (`main10Encode=`, `selfTests=`).
2. Edited export, film: `--es uri <10-bit clip> --es dest <out.mp4> --el startMs 0 --el durMs 3000 --es paramsFile <json with {"film":"kodak_portra_400"}> --es pack <dfp>`. Expect `OK edited=true`, `rung=HEVC Main10`, `inputMode=surface|buffer`, encoder name, `elapsedMs` (record it, and `dapple_engine` frame lines `exec=cpu`). Pull the file: `dapple motion --info` / ffprobe: HEVC Main10, 10-bit, BT.709 primaries/transfer/matrix, rotation 0, frame count = window x fps; extract a frame: the film look must be there; compare with the still of the same edit.
3. Edited export of an HLG (or PQ) clip without film (`{"exposure":0.3}`): the stream must be HEVC Main10 with primaries bt2020, transfer arib-std-b67 (smpte2084), matrix bt2020nc, and the HDR static info of the source (`ffprobe -show_streams`: side data); the picture must play HDR on the phone; with `{"film":...}` or `{"crop":"1:1"}` it must be BT.709.
4. Unedited export (no `paramsFile`): `edited=false`, the old codecSummary, HLG / PQ tags and static info as the source.
5. Thermal and time: `elapsedMs` for 3 s / 30 fps, `dapple_thermal_headroom` before / after, any `slow frame` line.
6. If `enc10s` fails with `no 10-bit EGL surface`, report the `EGL ... config` text of the failure and the codec's colour list from the page; if `dec10` fails, report the colour format number.

## 29. Per-file exposure anchor: no maker or model exposure table (engine-gaps engineer, 2026-11-13)

**Problem.** LibRaw scales every RAW so that the white level is 1.0. What the camera's own JPEG shows as mid-grey sits at a maker-, mode- and photographer-dependent headroom below that. The engine's neutral render (0 EV) was 6 - 22 L* darker than the embedded JPEG on every maker but Sony (mean dL: Fujifilm -17, Olympus / OM -18, Canon -16, Google -12, Apple -13, Panasonic -13; X-T50 -19 .. -21), because the only exposure anchor was `CameraProfile::baselineExposureEv`, fitted for the Sony bodies only (1.4 EV from the A7R V). A per-maker table (the first attempt, deleted again) is the darktable approach (a fixed +0.7 EV for everything, "empirical") and fails on the next unknown camera and on files whose settings shift the raw (Fuji DR200 / DR400, Canon highlight tone priority, exposure compensation).

**Design.** One scalar per file, computed once at decode (`core/src/exposure_anchor.{h,cpp}`, called from `LibRawDecoder::decode`), stored in `ShotMetadata` (`exposureAnchorTier / Ev / Confidence / Note` plus diagnostics) and used instead of the profile's `baselineExposureEv` (`effectiveCamera(registry, meta)` in `camera.h`; the registry profile still supplies colour match, contrast and the format facts: CFA kind, extensions). It is deterministic (no randomness, fixed proxy size 192 px), independent of the decode quality (preview and full decode agree within noise), cached with the decoded source, and the user's exposure is an offset on top, as before. `DAPPLE_ANCHOR=off` restores the profile value (A/B), `DAPPLE_PROFILE=generic` forces the generic profile (diagnostic: the anchor does not depend on the profile id, see the table).

* **Tier 1, the camera's rendering of this very frame (the embedded JPEG preview).** The scalar is the median over mid-tone pixels of log2(preview luminance / raw luminance), both linear (preview decoded from sRGB, raw = ProPhoto luma after white balance). Only this one number is taken from the preview: no curve and no colour, so the engine's own look is unchanged and a picture style, film simulation or HDR tone mapping leaks in only through the mid-tone level. The same idea as RawTherapee's auto-matched tone curve (matching the raw to the embedded JPEG), reduced to a scalar. Details: preview and raw proxy are brought to the same framing (centre crop to the narrower aspect; aspect ratios more than 1.25 apart, previews below 160 px on the long edge, and previews that fail to decode are rejected), both are area-averaged to 192 px; the outer 10 % is ignored (vignetting, phones correct it in the preview); a pixel counts when the preview's encoded luma is in 0.30 .. 0.70 (sRGB 77 .. 179, around mid-grey where every maker's curve is close to the plain sRGB curve), no preview channel is above 0.97 or below 0.03, the raw pixel is not clipped (all channels below 0.95) and above the noise floor. Statistic: median; spread = robust sigma ((q75 - q25) / 1.349); confidence = min(1, valid pixels / (0.25 * pixels in the frame)) * exp(-spread / 0.7). Accepted when at least 300 pixels and 2 % of the frame are valid, spread <= 1 EV and confidence >= 0.30. Window sensitivity (8 files: Fuji X-T50, Canon R6 II, Pixel 7 Pro, Olympus E-M1 III, Panasonic S5, Nikon Z8, two A7R V frames; `DAPPLE_ANCHOR_WINDOW=lo,hi`): the anchor for the windows 0.15 - 0.85 / 0.20 - 0.80 / 0.30 - 0.70 / 0.40 - 0.60 moves by at most 0.31 EV between the extremes (X-T50 +1.26 / +1.27 / +1.38 / +1.57, R6 II +1.75 / +1.80 / +1.87 / +1.95, Pixel 7 Pro +0.80 / +0.80 / +0.85 / +0.99, E-M1 III +1.17 / +1.20 / +1.23 / +1.24, S5 +0.99 / +0.99 / +1.15 / +1.11, Z8 +0.84 / +0.91 / +1.01 / +1.06, A7R V +1.29 / +1.30 / +1.33 / +1.32 and +1.65 / +1.68 / +1.75 / +1.78), the wider windows reading lower (toe / shoulder where the camera curves are steeper and darker than sRGB); 0.30 - 0.70 is in the flat part. The anchor of a Preview-quality decode and a Full decode agree within 0.01 EV (same 8 files). Per-file spread of the log-ratio at the final window: 0.03 - 0.54 EV.
* **Tier 2, the file's own declaration:** DNG `BaselineExposure` (+ `BaselineExposureOffset`, tag 51109, read from the file because LibRaw does not expose it). Used only when tier 1 is not usable and the tag is not 0 (0 is the specification default, i.e. no information). **Evidence on which tier wins for DNG:** on the 8 DNGs with a non-zero tag and a usable preview the tag is lower than the preview anchor in every case: Pixel 6 / 7 / 8 Pro tag +0.72 / +0.58 / +0.55 against preview anchors +0.80 / +0.85 / +1.00, iPhone 12 Pro -0.30 against +0.54, Leica M10 -0.78 against +0.03, Pentax K-1 II -0.51 against +0.47 (and the Pentax K-3 III Monochrome -0.51 against a rejected preview). The tag is what the converter wants for *its* default render (Adobe's), not what the vendor showed: tier 1 wins whenever it is usable.
* **Tier 3, no usable preview and no tag: scene statistics, confidence 0.2.** ev = 0.5 * log2(0.22 / mean raw luminance) + 0.5 * 1.0, capped so that the 99.5th percentile stays below 1.5 after the anchor, clamped to -1.5 .. 3.5. The constants are derived, not chosen: on the 36 samples with a valid tier 1 (the first derivation also counted four files without a preview as zeros; redone on the clean set the numbers below are unchanged to two digits), the mean luminance predicts the tier 1 anchor best (residual sigma 0.57 EV; median luminance 0.83, 99.5th percentile 0.64, blends of them 0.57 - 0.68); 0.22 is the median of (tier 1 EV + log2 mean luminance); shrinking halfway towards the median tier 1 EV of the set (1.0 EV) lowers the residual to 0.36 EV (max 0.94) and keeps a low-key photo from being pulled towards middle grey (a full-strength statistic gave +2.3 EV on a very dark Nikon D850 frame whose camera anchor is +1.6). The 1.0 EV prior is the only global constant and only exists on this tier.
* **Tier 4 / none:** 0 (not a camera RAW, e.g. Dapple's own linear merge DNG: `exposureAnchorTier` stays 0 and the profile value is used, so the golden corpus and merges are unchanged).
* **CLI:** `dapple info --anchor <file>` decodes (preview quality) and prints tier, EV, confidence, profile baseline, note, the tier 1 / tier 3 / DNG values.
* **Keep:** the fitted Sony A7R V colour match (colour is separate); the existing rule that auto-adjust never pulls a low-key or high-key photo to middle grey (`auto_adjust.cpp` is untouched; the anchor carries the photographer's intent, including exposure compensation, because it is the camera's own level for this frame); user exposure on top.

**Validation** (engine auto render at 400 px vs the camera JPEG preview, mean L*, `scripts/samples/anchor_table.py`; 40 public CC0 samples under `build/rawsamples` + 22 of the owner's ARWs, every 25th file of D:/图片, outputs only under build/). 62 files, 60 comparable (two do not decode). Mean absolute dL: **8.1 L* without the anchor (profile baselines only), 2.1 L* with it**; 56 of 60 within +-5 L*, 53 within +-3. By maker (mean dL off -> anchor, max |dL| anchor): Apple -13.2 -> -0.7 (0.8), Canon -15.7 -> +1.0 (2.6), Fujifilm -17.1 -> -0.8 (3.3), Google -12.0 -> 0.0 (0.8), Olympus / OM -17.8 -> +0.9 (1.2), Panasonic -12.6 -> +2.8 (3.5), Samsung -1.4 -> +1.5 (2.7), Pentax -2.9 -> +3.4 (11.3, the monochrome), Nikon -4.2 -> +6.8 (9.6: two D850 frames on tier 3), Leica -2.0 -> +6.8 (20.5: the monochrome), Sony 25 files +0.6 -> +1.0 (3.1). X-T50 (the owner's second camera): dL -19.3 / -21.4 / -20.3 -> -1.2 / -3.3 / -2.4 (anchors 1.44 / 1.38 / 1.40 EV). The full per-file table (tier, EV, confidence, JPEG L*, dL off / anchor / anchor with the profile forced to generic) is in raw-compat.md.
* **Profile independence:** with the profile forced to `generic` the result is identical for every non-Sony file (max difference 0.00 L*: they resolve to generic anyway) and differs by at most 1.4 L* for the Sony files, which is the Sony profile's own contrast / colour match, not the anchor.
* **Sony, golden corpus:** 22 A7R V frames: anchors 1.27 .. 1.79 EV, median 1.48 EV against the fitted profile value 1.40 EV (the spread is the photographer's exposure compensation and the scene); mean |dL| 1.4 -> 1.1 L*, max 3.1. The A7R IV (DSC00396, tier 3, +0.55 EV, dL +10.8 before and after) and the A6700 / A7 IV (anchors 1.07 / 1.00 EV against the profile's 1.4, dL +5.8 / +6.2 before, +0.7 / +0.3 after) show that the Sony generic 1.4 EV was too bright for other Sony bodies. The golden corpus is unchanged (ctest `engine.golden_*` 93 of 93 pass): the corpus renders synthetic and generic-profile sources that never pass through the decoder's anchor. The sessions of real A7R V photos change by (anchor - 1.40) EV, a median of +0.08 EV.
* **Fuji DR modes and exposure compensation:** `dapple info` now reports Fujifilm `DR<n>` and the exposure mid-point shift of the maker notes in the raw format field. The three public X-T50 frames anchor within 0.06 EV of each other; whether any of them is a DR200 / DR400 frame was not recorded in their tags as read by LibRaw (all report the same development range), so no DR-specific case could be shown on a real file. Exposure-compensated frames are exactly what the anchor follows (the owner's A7R V set: 1.27 .. 1.79 EV for the same lens and body); the two sources of evidence are the camera JPEG (which already contains the compensation) and the unchanged auto-adjust "never pull low-key / high-key frames" rule.
* **Limits:** previews with a strong picture style / HDR tone mapping (phones) are matched only at the mid-tone level (the Pixel 6 Pro preview carries the sharpest tone mapping: spread 0.41 EV); a preview with lens correction crops differently from the raw (handled by the 10 % border and the aspect rule, not by registration); files without a preview and without a tag (Xiaomi, Realme, OnePlus phones in the set) get the tier 3 estimate (0.39 EV sigma, not verified on those files: no preview to compare with); monochrome files: the K-3 III Monochrome and Leica M Monochrom previews are rejected (spread 0.88 / 2.08 EV, tier 2 / 3) and the render stays 11 / 20 L* too bright (the monochrome decode itself is already 5 / 19 L* brighter than the preview at 0 EV: the data are bright and no preview witness exists; open); very low-key frames (D850 night frames, tier 3) +9.5 L*.
* **Sources:** RawTherapee's "auto-matched tone curve" (matches the raw to the embedded JPEG; this is its scalar reduction); Adobe DNG Specification 1.6/1.7, tags BaselineExposure (50730) and BaselineExposureOffset (51109); darktable's fixed +0.7 EV scene-referred default ("exposure" module) is the empirical constant this design avoids.

### 29.1 Round 2 (2026-11-13): references for our own captures, out-of-sample check of tier 3, dark and monochrome frames

* **Reference image of the frame (engine side, done).** `DecodeOptions::referenceImage` (a paired camera JPEG or any sidecar image of the same frame, EXIF orientation applied by `loadReferenceJpeg`) is used first as the tier 1 witness; when it is empty and the embedded preview is missing or unusable, a sibling file with the same stem and a `.jpg` / `.jpeg` extension is tried (RAW+JPEG). `dapple info --anchor <raw> [--reference <jpeg>]` shows the source (`embedded-preview` / `reference-image`); `ShotMetadata::exposureAnchorSource` carries it. Tests (ctest `dng.anchor`): reference levels 128 / 160 give EV steps of exactly the log2 ratio of the linear levels (0.704), the sibling is discovered, the embedded variant gives the same EV.
* **Own captures.** RAW+JPEG captures: `AppState.ingestCapture` already moves `<stem>.jpg` next to `<stem>.dng` in the inbox, so the sibling rule finds the paired JPEG with no further change (a Dart test now asserts the pairing). RAW-only captures: CameraX writes the DNG without any preview, so `CaptureActivity` keeps the viewfinder frame of the shot (`PreviewView.getBitmap()` at the shutter press, scaled to 720 px, JPEG q90, `CaptureThumb.kt`, extra `thumb`), Dart embeds it into the DNG after the shot (`dapple_dng_embed_thumbnail`, `core/src/dng_thumbnail.{h,cpp}`: IFD0 is copied to the end of the file with a SubIFDs entry, the JPEG and a reduced-resolution SubIFD follow, every raw offset stays valid, a file with SubIFDs is refused) and deletes the thumbnail file. LibRaw reads the result as the embedded preview (ctest: `extractEmbeddedPreview` finds it, tier 1 with source `embedded-preview`, the raw data decode bit-identically). `DngCreator.setThumbnail` was not used because CameraX owns the DNG writer; this path also works for a file copied elsewhere. The thumbnail's orientation may be sensor or upright: the existing rules rotate 90 degree cases, and for 180 degrees the anchor tries the preview as is and turned by 180 degrees and keeps the one whose log-ratio spread is lower (it only switches when the spread improves by 0.15 EV).
* **Tier 3 out of sample.** Leave-one-maker-out on the 36 files (13 makers; constants refitted on the remaining makers, applied to the held-out one): residual sigma 0.37 EV, mean absolute error 0.30, worst 0.96 EV (one Canon file); K varied 0.212 - 0.234 and the prior 1.00 - 1.14 between folds; per held-out maker the mean error is within +-0.66 EV (OM System -0.66, Samsung +0.48, Leica +0.35, the others below 0.25). So the shipped constants (K 0.22, prior 1.0, shrink 0.5) are not an artefact of the fit.
* **The two low-key D850 frames (+9.5 L*).** Tier 1 was tried first: its confidence (0.20) was below the 0.30 acceptance, but accepting it gives the same EV (tier 1 +1.63, tier 3 +1.66), so the exposure anchor is not the cause. The error lies in the shadows: on a night scene 75 % of the pixels are below L* 30 in the camera JPEG and the engine's neutral render shows them 11 - 12 L* lighter, while the mid-tones and highlights (L* 50 and above, 21 % of the pixels) match within +-1 L*. The Nikon tone curve crushes shadows (picture style / Active D-Lighting), the engine's neutral curve does not: a look difference, not an exposure error; a mean-L* fit would want 0.9 EV (darker), at the price of dull mid-tones. Left as is, by design (only the level is taken from the camera, never its curve).
* **Monochrome files (+11 / +20 L*).** No scaling error was found: the K-3 III Monochrome DNG has black 64, white 16378, data mean 0.52 and maximum 0.72 of white after LibRaw's scaling (a bright exposure), BaselineExposure -0.51; the Leica M Monochrom has white 3750. The previews are rejected as references (K-3 III: spread 0.88 EV at 0.16 confidence, the Leica M Monochrom preview is 160 x 120) and their black-and-white rendering has a much steeper tone curve than sRGB: forcing tier 1 on the K-3 III (-2.09 EV) overshoots to -13.8 L*, the tag (-0.51 EV) leaves +11.3. Honest status: not solved; mono files fall between a contrasty preview and an unreliable tag.
* **Galaxy Note10+ dark frame (`sample2.dng`).** Diagnosis: the DNG is almost empty: black 64 (all four channels), white 1023, raw means 66.1 / 64.9 / 65.9 / 66.1 DN with sigma 7.3 DN, i.e. 0.9 - 2.1 DN above black; the black level tags are right (histogram of the bayer sites peaks at 64 - 66). LibRaw clips the black-subtracted noise at zero (a +0.4 sigma, about 3 DN, bias per channel) and the white balance multipliers (R x 1.95, B x 1.8) turn the differing per-channel bias into the magenta cast (R 0.0070 / G 0.0041 / B 0.0097 after conversion); GainMap order is correct (black is subtracted first, `blackAt`) and was not the cause. The anchor added to it: tier 3 gave +3.17 EV on a frame at 0.4 % of white. Fixes: tier 3 never exceeds +2.0 EV (the largest tier 1 anchor in the set is 1.87) and is held at the 1.0 EV prior below 1 % mean luminance; and a deep-shadow chroma floor in the decoder (linear luma below 0.008 fades to neutral, luma kept, `suppressShadowChroma`). Result: mean RGB 60 / 40 / 60 -> 35 / 21 / 31, mean chroma (max - min) 55 -> 16. What remains is a dark noise frame: the camera JPEG that looked mid grey cannot come from this raw (5.5 EV apart), so the DNG was captured with a shorter exposure / lower gain than the JPEG or the sensor was nearly covered: the capture side should check the exposure of its RAW stream (this is a capture problem, not a decode problem).

## 31. RAW decode time: measurements and the demosaic-on-GPU design (engine-gaps engineer, 2026-11-13; round 3, parts A and D measured, B and C designed, not built)

### 31.1 Where the time goes (A)

`DAPPLE_DECODE_TIMING=1` makes `LibRawDecoder::decode` print one stderr line with the split (LibRaw progress callbacks plus our own marks); `scripts/samples/` has no wrapper, the harness is a 30-line Python script around `dapple dump-linear` (peak working set polled with GetProcessMemoryInfo). Desktop = this PC (28 hardware threads, OpenMP in LibRaw, 3-pass X-Trans), `--full` = the export / full-quality path, proxy = the editor's session open (half-size decode + resize to 1600). Times in ms, files from the public CC0 set (build/rawsamples):

| file (full quality) | threads | wall s | peak MB | unpack (read + decompress) | black / white / WB scale | demosaic | colour matrix | make_mem_image + float copy | exposure anchor |
|---|---|---|---|---|---|---|---|---|---|
| X-T50 40 MP, uncompressed RAF | 28 | 3.35 | 1116 | 43 | 85 | **1961** | 107 | 213 | 390 (before this round) |
| | 4 | 5.84 | 1115 | 43 | 101 | 4412 | 110 | 219 | 390 |
| | 1 | 15.94 | 1115 | 43 | 203 | 14433 | 110 | 219 | 390 |
| X-T50 40 MP, compressed RAF | 28 | 3.43 | 1117 | 173 | 81 | 1960 | 107 | 225 | 386 |
| | 4 | 6.21 | 1115 | 414 | 106 | 4431 | 107 | 240 | 393 |
| | 1 | 17.25 | 1115 | 1352 | 197 | 14434 | 112 | 215 | 399 |
| X-S10 26 MP, compressed RAF | 28 / 4 / 1 | 2.50 / 4.07 / 11.21 | 794 | 148 / 325 / 888 | 56 / 65 / 130 | 1419 / 2786 / 9311 | 69 | 139 | 301 |
| Sony A7R IV 61 MP ARW lossless | 28 / 4 / 1 | 2.12 / 2.51 / 4.01 | 1433 | 191 | 126 / 152 / 273 | 253 / 586 / 1928 | 165 | 322 | 308 |
| Nikon D850 45 MP NEF lossless | 28 / 4 / 1 | 2.45 / 2.73 / 3.84 | 1709 | 413 | 97 / 109 / 192 | 197 / 425 / 1440 | 123 | 251 | 797 |

| proxy (editor session open) | threads | wall s | peak MB | unpack | scale | demosaic | convert + copy | anchor |
|---|---|---|---|---|---|---|---|---|
| X-T50 uncompressed | 28 / 1 | 0.53 / 0.63 | 461 | 43 | 32 / 131 | 2 (half-size: no interpolation) | 87 | 249 |
| X-T50 compressed | 28 / 1 | 0.63 / 1.88 | 461 | 166 / 1352 | 24 / 127 | 2 | 93 | 250 |
| X-S10 compressed | 28 / 1 | 0.50 / 1.30 | 421 | 129 / 894 | 16 / 83 | 1 | 56 | 226 |
| A7R IV 61 MP | 28 / 1 | 0.61 / 0.67 | 386 | 189 | 49 / 152 | 13 | 133 | 99 |
| D850 45 MP | 28 / 1 | 1.32 / 1.36 | 1312 | 416 | 32 / 111 | 9 | 101 | 649 |

Findings. (1) X-Trans demosaic is 60 - 70 % of the time of a full decode even with 28 threads (1.96 s of 2.8 s) and 90 % single-threaded (14.4 of 15.4 s); Bayer is cheap (0.2 s with threads, 1.4 - 1.9 s single). (2) Decompression is serial in LibRaw except where its own OpenMP loops exist: Fuji compressed RAF 1.35 s single, 0.17 s with 28 threads (block-parallel in LibRaw's `fuji_compressed_load_raw`); Sony ARW 0.19 s and Nikon NEF 0.41 s do not scale (single stream decoded serially; the D850 and A7R IV timings are identical at 1 and 28 threads); uncompressed RAF is file read only. (3) black / white scaling and WB (`scale_colors`) and the colour matrix are cheap and trivially data parallel (0.06 - 0.17 s each); the copy through `dcraw_make_mem_image` and our float conversion costs another 0.14 - 0.32 s and 1/3 of the peak memory (16-bit RGB frame + float RGB frame + LibRaw's own 4 x 16-bit buffer: 40 MP = 480 MB + 480 MB + 320 MB). (4) The exposure anchor needed 0.25 - 0.8 s because it decoded and converted the full-size embedded JPEG (45 MP as float): done in this round, see 31.4. (5) The editor's proxy path is already cheap: the half-size decode needs no X-Trans / Bayer interpolation (the 2 x 2 / 3 x 3 cell is binned), 0.5 - 0.65 s on desktop with a warm file cache, of which 0.25 s was the anchor.

Android x86_64 emulator (4 vCPU, OpenMP linked statically; measured in round 2 before the anchor change): X-T50 compressed full 6.8 s real (18.2 s CPU), proxy 1.1 s; Samsung S22 JPEG XL full 1.25 s. Phones were not measured; with 4 + 4 cores the X-Trans interpolation lands near 2.5 - 4 s of the emulator's 6.8 s. The throughput tables for the GPU side are docs/engine/device-results/find-n5.md (film frames: 2 - 8 ms per 1.2 MP at the interactive tier), they say nothing about a 40 MP compute pass.

### 31.2 Design: demosaic as an engine node on the GPU (B), not built in this round

Why not today: the work needs (a) a single-channel 16-bit source in the graph (`NodeType::Source` is a 3-channel float image; the planner, the CPU executor and the HAL image formats all assume RGB), (b) a demosaic kernel with X-Trans quality, and (c) tiling with a halo for 60 MP. Items (a) and (c) touch the files the performance engineer owns (`core/engine/exec/gpu_*`, `plan/planner.cpp`), (b) is a new algorithm port and must be validated against LibRaw's result on real files. The honest plan, in the order it can ship:

1. **Decoder output = the mosaic.** `ImageDecoder::decodeMosaic` returns `{uint16 plane (width x height), cfa (2x2 or 6x6 pattern ids), black[], white, wb[3], colour matrix 3x3 camera -> ProPhoto}` after LibRaw `unpack()` + `raw2image`-free access to `rawdata.raw_image` (no `dcraw_process`), with the GainMap / FixBadPixels opcodes already applied to the mosaic (they are today). That removes `scale_colors`, the demosaic, `convert_rgb`, `make_mem_image` and the float copy from the CPU path: what stays is read + decompress (0.04 - 0.4 s, 0.17 s for compressed X-T50 with threads).
2. **Source format R16.** One additive line in the HAL format enum and the planner's source description (`Source` node with `channels = 1, format = R16UI, cfa = ...`); `ResultCache` key = file content hash (size + mtime + 64 KB hash of the head, tail and the CFA tags) + decode parameters, so the mosaic is cached and a re-open skips decompression. This is the hunk for the performance engineer, nothing in `gpu_*` was edited: *add `PixelFormat::R16Uint` to `hal/image.h` and its Vulkan / GLES / Metal mappings, and let `SourceDesc` carry `cfa` and `levels`.*
3. **`Demosaic` node (new files `core/engine/nodes/demosaic/**` and `core/engine/shaders/demosaic_*.comp`).** CPU implementation first: LibRaw's own functions called on the mosaic (`LibRaw::dcraw_process` stages are not callable separately, so the CPU reference stays "LibRaw does it" and is the bit-exact oracle for the golden corpus; frames that go through it do not change). GPU implementations, single-source GLSL like the film kernels (Vulkan SPIR-V, GLES 3.1 patches, MSL via the existing translation): **Bayer: RCD** (ratio-corrected demosaic: gradient-based, one green + two chroma passes, about 12 reads per output, no iteration; visibly close to AHD, better on fine detail, far simpler to tile than AHD's homogeneity maps); **X-Trans: a Markesteijn-class method, 1-pass first (green by directional gradient over the 6 x 6 pattern, red / blue from the green-difference), the 3-pass refinement as a second kernel** (LibRaw's `xtrans_interpolate` runs 4 directions x 3 passes; its tiles are independent with a 16 px halo, i.e. exactly a planner tile). Quality gate before any phone uses it: mean dE2000 < 0.5 and p99 < 2 against LibRaw on detail crops of the public X-T50 / X-S10 / A7R IV / D850 files, a zipper metric (high-frequency colour difference along edges on a chart-like crop) not above the reference's, false colour on edges not above, identical border and hot-pixel rules (LibRaw's X-Trans copies 6 border pixels unchanged). If the GPU method differs visibly, the CPU fallback must be the same algorithm ported to C++ (RCD is about 200 lines, the X-Trans port about 400): then devices agree and the golden corpus gets new lines for RAW sources only.
4. **Tiling.** The planner's halo mechanism (`GraphDetail` tiles with halo) applies: RCD needs 5 px, X-Trans 3-pass 16 px (LibRaw's own tile halo), so a 40 - 60 MP frame is processed in 2048 px tiles: the R16 mosaic (80 - 120 MB) lives in host memory, tiles are uploaded, demosaiced into RGBA16F tiles that feed the existing fused point chain (WB, colour matrix and the exposure anchor gain are one 3 x 3 matrix and join `point.comp`); peak GPU memory is two tiles plus the output, about 150 MB, far below the full-frame float image (480 MB).
5. **Expected numbers (estimates, not measurements):** a Markesteijn-class kernel is about 3 - 4 K ALU ops per pixel (3 passes, 4 directions): 40 MP x 4 K = 160 GFLOP, 80 - 100 ms on a desktop GPU of 2 - 4 TFLOPS, 0.4 - 0.8 s on the Adreno 840 class (about 2 TFLOPS fp16); RCD is about 10 x cheaper (10 - 20 ms desktop, 50 - 100 ms phone). End-to-end full decode then = read + decompress + upload (0.2 - 0.6 s) + demosaic + readback only on export. The target "40 MP X-Trans in well under 300 ms on a desktop GPU, decode about 1 s" is plausible by this arithmetic; it is **not demonstrated**.

### 31.3 Decompression (C), not built

LibRaw threads (OpenMP) the Fuji compressed loader, the X-Trans and AHD demosaics and a few lossless-JPEG loaders; Sony ARW, Nikon NEF (lossless) and Canon CR2 decode as one serial stream per image in LibRaw (ARW 2.3 / 4 / 5 and NEF lossless have no restart markers across the whole frame). A portable `std::thread` split of our own is possible for (a) Fuji compressed RAF (blocks are independent: 4 - 6 blocks per frame), (b) DNG / CR2 lossless JPEG with tiles or slices, (c) Sony ARW 6 tiles when the tile table is present: each needs the format's decoder run on a sub-range, which LibRaw does not expose, so it means porting those decoders (Fuji: about 300 lines). Gain: Fuji compressed 1.35 s -> 0.3 s single-core-limited phones; ARW / NEF 0.2 - 0.4 s unchanged. Streaming tiles to the GPU while the next ones decompress shortens time to first pixel only for the export path; the editor opens a half-size proxy anyway.

### 31.4 What was changed in this round (D and the cheap parts)

* **Today's open sequence (D), from `app/lib/services/session_cache.dart`:** (1) `SessionCache.preview` extracts the camera's embedded JPEG (1280 px) as an instant placeholder, (2) `_openNew` opens a session with `full: false, longEdge: 2048`: the half-size decode (no demosaic) plus the resize, 0.5 - 0.65 s on desktop for a 40 MP Fuji, (3) pinch-zoom detail opens a second session with `full: false` as well (also the half-size decode: the zoomed picture is never more than half-resolution), (4) the full decode (`full: true`) happens only at export / share, where the user waits for decode + render + encode (about 3.4 s + render on the desktop for the X-T50 before this round, 4 - 7 s on the emulator, longer on a phone). So the sequence the owner asked for is half there: embedded preview immediately, proxy from a fast path; missing: a true full-resolution session in the background and a GPU path. Before / after numbers for "tap to first correct picture": desktop X-T50 compressed 0.63 s (proxy decode) + render; "tap to full resolution ready" does not exist today (export only: 3.4 s); the emulator figures are 1.1 s proxy and 6.8 s full. No change to the Dart sequence was made: opening a full-resolution session in the background costs 1.1 GB of transient memory for 40 MP on desktop and would need the device engineer's memory budget first.
* **Exposure anchor cost (measured above):** the embedded preview is now decoded on a second thread while LibRaw works (`std::async`, `DAPPLE_NO_ASYNC_PREVIEW=1` disables) and the 8-bit preview is box-averaged to at most 768 px before it becomes float. D850 full decode: anchor step 800 -> 275 ms, peak memory 1709 -> 1324 MB, wall 2.45 -> 2.13 s (28 threads); anchors unchanged (X-T50 +1.38 EV, R6 II +1.87, D850 +1.66).
* `DAPPLE_DECODE_TIMING=1` (the measurement aid) stays in the decoder.

## 32. Demosaic as an engine node: CPU + GPU implementation, mosaic decoder, cache (demosaic engineer, 2026-11-13)

What section 31.2 designed is built as far as it can be without touching the files of the other engineers: the decoder returns the mosaic, a `Demosaic` node exists as portable C++ (std::thread tiles) and as single-source GLSL for Vulkan / GLES 3.1 / Metal, both with tile + halo + region of interest, measured against LibRaw on the whole public sample set. **Not done**: the wiring into the planner / executor / `EngineSession` / C API / Dart (exact hunks in 32.9, written against the real symbols, not applied), the Markesteijn-class X-Trans quality (32.5: the node's X-Trans method is a simpler one and is visibly weaker than LibRaw's 3-pass on high-contrast foliage), a Metal compile (no Mac), real phones.

### 32.1 Files

| File | What |
|---|---|
| `core/include/dapple/mosaic.h` | `MosaicImage` (16-bit plane, CFA, black table, white, multipliers, camera -> ProPhoto matrix, flip, crop, OpcodeList3 payload, content key), `decodeRawMosaic`, `finishMosaicRgb`, `fileContentKey` |
| `core/src/decoders/libraw_mosaic.inc` | the implementation, **included by one line at the end of `libraw_decoder.cpp`** (plus `#include "dapple/mosaic.h"` at the top): the only edit in a file of `engine-gaps` (see 32.9) |
| `core/engine/nodes/demosaic/demosaic.h`, `demosaic_cpu.cpp` | the node on the CPU: `demosaic(Desc, roi, out)`, tile + halo, `bilinear` (the baseline of the tests), `mapCoord` |
| `core/engine/nodes/demosaic/demosaic_desc.h` | `MosaicImage` -> `Desc` |
| `core/engine/nodes/demosaic/demosaic_session.{h,cpp}` | `demosaicOrientedRegion` (a rectangle of the picture as the editor sees it -> the mosaic rectangle, demosaic, orient), `demosaicFull` (+ OpcodeList3 + crop + orientation), `binnedHalf` (the proxy without interpolation), `MosaicCache` (LRU by bytes, keyed by content hash) |
| `core/engine/nodes/demosaic/demosaic_gpu.{h,cpp}`, `demosaic_gpu.cmake` | `GpuDemosaic`: tiled runner on a `hal::Device` (window upload, 3 - 4 dispatches per tile, read-back) |
| `core/engine/shaders/demosaic_{common.glsl,scale.comp,bayer.comp,xtrans.comp}` | the kernels; 8 pipeline variants (`dm_scale`, `dm_bg`, `dm_bo32/16`, `dm_xg`, `dm_xc`, `dm_xo32/16`) listed in `demosaic_gpu.cmake`, appended to the Vulkan list in `core/gpu/CMakeLists.txt` and to the GLES fallback list `hal/gles/shader_list.cmake` (additive lines) |
| `tests/engine/demosaic/` | `demosaic_tests.cpp` (ctest `demosaic.*`), `demosaic_eval.cpp` (the evaluation tool against LibRaw: `dapple_demosaic_eval file --out build/demosaic --gpu 2048 --proxy --cache`), `de2000.h`, `demosaic.cmake` (+ one include line at the end of `tests/CMakeLists.txt`) |
| `docs/legal/demosaic.md` | licence analysis |

No HAL change: the mosaic travels as a storage **buffer** of packed 16-bit words (two per `uint`), not as an `R16Uint` image. Reason: GLES 3.1 does not guarantee `r16ui` as a storage image format (only `r32ui` / `r32i` / `r32f` and the `rgba*` formats), a buffer works identically on Vulkan, GLES and Metal and needs no format capability bit; the 16-bit texture would only buy cache locality. If the lead still wants `PixelFormat::R16Uint`, it is additive and only `demosaic_scale.comp` would change (`usampler2D` instead of the buffer).

### 32.2 Decoder: `decodeRawMosaic`

Returns, after LibRaw `unpack()` and no `dcraw_process`: the active area as one 16-bit plane (a copy, 2 B per pixel), the CFA (2 x 2 or 6 x 6 colour table, second green kept for the black level and multiplier), a black table (common + per colour + repeat pattern, one float per cell of the lcm of the CFA and the pattern period), `white = maximum`, the multipliers as `scale_colors` normalises them (smallest = 1, `highlight = 0`), the matrix `prophoto_rgb * rgb_cam` that `convert_to_rgb` builds (so the output is what the RGB path's `output_color = 4` gives), orientation, DNG default crop, the raw OpcodeList3 payload and the metadata (except the exposure anchor, which needs an RGB picture: attach it from the proxy). DNG OpcodeList1 / 2 (GainMap, FixBadPixels ...) are applied to the mosaic exactly as `decode()` does before copying. The black-level folding of `adjust_bl` (common part into `black`, per-colour levels include it, `span = maximum - common black`) is reproduced, which is why the scaled values agree with LibRaw's: the channel means of node and LibRaw differ by less than 0.5 % on every file.

It answers `cfa == None` + `reason` (and the caller uses the RGB path) for: monochrome, Canon sRAW / mRAW and other 3-colour data, linear DNGs, JPEG XL DNG, four-colour sensors, Fuji rotated EXR layouts, formats LibRaw delivers as 3 / 4-channel buffers, formats with `zero_is_bad` (all three Panasonic samples: LibRaw removes the zero pixels itself), and the DNG-AsShotWhiteXY-only case (32.9). Everything else with a Bayer 2 x 2 or X-Trans 6 x 6 mosaic goes through the node. The RGB path (`ImageDecoder::decode`) is untouched and remains the oracle and the fallback.

### 32.3 The node

Per pixel, from the scaled plane `S = min(max(raw - black, 0) * wb / span, 1)` (the clip to 1.0 per channel is LibRaw's own highlight handling with `highlight = 0`, so highlights behave as today; the synthetic `clip` test checks that a clipped blob keeps exactly its data colour):

* **Bayer** (halo 4): green at red / blue = gradient-weighted blend of the horizontal and vertical Hamilton-Adams estimates (`0.5 (l + r) + 0.25 (2 c - l2 - r2)`, clamped to the two neighbours), weights `1 / (0.002 + d)^2` with the discriminator `d` over the sample's row / column plus half of the two neighbouring rows / columns; red / blue at the opposite colour by the (R - G) / (B - G) difference of the two diagonals, weighted by the green gradient along each diagonal; at green by the plain mean of the two same-row / same-column differences. Two GPU passes after the scaling pass (green plane; chroma + matrix + output). **Why this and not RCD:** RCD (ratio-corrected demosaicing) exists only as GPL code (RawTherapee, darktable) and was not read; the Hamilton-Adams family with gradient weights is the cheap, well-published method of the same class (about 25 reads per output pixel, no iteration, no homogeneity maps, trivially tileable with a 4 px halo). It is not claimed to equal RCD.
* **X-Trans** (halo 6): green at red / blue from the four directions (H, V, two diagonals) through the nearest greens (at most 3 steps) of the 6 x 6 pattern, weight `1 / ((0.002 + |g- - g+|)^2 (l- + l+)^2)`, plus a Laplacian term (the sample's excess over the distance-weighted mean of its own colour within radius 3, weight 0.25, clamped to the range of the greens within radius 2); red / blue colour differences at every pixel of ROI + 1 as the guide-weighted mean over 5 x 5 (`1 / (d^2 (0.004 + |G(q) - G(p)|)^2)`: samples whose green differs from the pixel's get no say), then a 3 x 3 median of the differences (removes the isolated colour speckle of a difference estimate at a one-pixel feature; cuts the edge zipper energy to 60 - 80 % of LibRaw's), then the matrix. Three GPU passes after the scaling pass. **The 1-pass Markesteijn variant was not implemented** (no directional RGB candidates, no homogeneity maps, no LAB) and the 3-pass refinement tier neither: see 32.5 for what that costs and 32.9 for the plan. Tried and rejected on the X-T50 files (dE against LibRaw unchanged within 0.1): chroma weight exponent 1, larger guide floor, radius 3, clamping to the sample range, using the median of the difference to re-estimate green at red / blue sites.
* **Fused:** black / white / WB multiply are in the scaling pass, the 3 x 3 matrix and the clamp in the output pass; the planner could fold the matrix into the point chain (32.9). The parameters (epsilons, weights) are constants shared by the CPU and the shaders.
* **Border:** Bayer reflects (`-i`, `2 (n - 1) - i`, which keeps the CFA phase), X-Trans repeats with the period 6; the same mapping on CPU and GPU. The measured 24 px border band is as close to LibRaw as the interior (table 32.5). LibRaw's X-Trans copies 6 px unchanged; the node interpolates them.
* **Hot / dead pixels:** the node does no suppression (neither does LibRaw's default). A single hot sample at white in a flat field (test `hotpixel`): a green site stays one pixel (0.7 above the field), a red / blue site becomes a 3 x 3 (Bayer) or 18-pixel (X-Trans) coloured blob; bilinear smears it over 25 pixels at 0.3. Hot / dead pixel lists (DNG `FixBadPixels`) are applied to the mosaic upstream by the opcode code.

### 32.4 Tiling, halo, memory

* **Halo measured, not assumed:** the test `tiles` demosaics 40 random regions of interest (odd sizes, all four picture borders, every CFA phase, 2 threads) and compares them with the whole frame: 0 differing values with halo 4 (Bayer) / 6 (X-Trans). Reasons: Bayer green needs mosaic +-2 and the chroma pass +-1 green around the output, 3, rounded to 4 so the CFA phase of a tile origin is preserved; X-Trans green needs +-3 (nearest greens and the Laplacian window), the chroma differences need green at +-2 around a ring of +-1 around the output: 3 + 2 + 1 = 6 (LibRaw's own X-Trans tile halo is 16 px, 3 passes). The GPU runner agrees: the whole frame as one tile against 64 px tiles and against a region of interest: 0 differing values (`gpu_tiles`).
* **Plane sizes:** per tile of `T` px the planes are `(T + 2 halo)^2`: scaled plane and green plane 4 B / px each, X-Trans difference plane 8 B / px, output tile RGBA16F 8 B / px (RGBA32F 16 B / px for the parity runs). T = 1024: 4.3 MB, 4.3 MB, 8.6 MB, output 8.4 MB (all under the 16 MiB step of section 28); T = 2048: 17 / 17 / 33 / 34 MB (over it; use 1024 on a mobile GPU). The mosaic window (2 B / px) goes up through a host-visible staging buffer.
* **Memory, 40 MP and 61 MP:** 

peak working set of the process (GetProcessMemoryInfo, `dapple_demosaic_eval --no-ref`), mosaic decode included:

| path | 40 MP X-T50 | 61 MP A7R IV | today's RGB path (section 31.1) |
|---|---|---|---|
| CPU node, 28 threads, whole picture as float RGB | **572 MB** (mosaic 80 + RGB float 480 + tiles) | **836 MB** (mosaic 122 + 732) | 1116 / 1433 MB |
| GPU node, tile 1024, output stays on the device (the pipeline case) | **185 MB** host + 26 MB device (RGBA16F tile; 34 MB with the RGBA32F parity tile) | **240 MB** host + 18 MB device | n/a |
| GPU node, result read back as float RGB (export to a host image) | 185 + 480 = about 665 MB | 240 + 732 = about 970 MB | |

The mosaic decode itself peaks at twice the mosaic (LibRaw's raw buffer plus our copy: 160 / 245 MB) before LibRaw's buffer is freed. The CPU path needed one fix to get there: `finishMosaicRgb` copied the 480 MB image for the upright case (peak 1002 MB before). With a tile-streaming consumer (the point chain taking the RGBA16F tile, hunk 1) the host never holds the RGB picture: mosaic + two tiles, about 200 MB at 40 MP, 250 MB at 61 MP, which is the phone-class budget (against 1.1 - 1.4 GB today).

* **Region of interest:** `demosaic(Desc, roi, ...)` and `GpuDemosaic::run(roi)` demosaic any rectangle; `demosaicOrientedRegion(mosaic, x0, y0, x1, y1)` takes the rectangle in the coordinates of the oriented, cropped picture (flip 3 / 5 / 6 and the DNG crop are mapped back to the mosaic) and returns it oriented. A warp (OpcodeList3) makes the mapping non-trivial: `mosaicRectOf` then says no and the caller demosaics the frame. This is what pinch-zoom at 1:1 needs (today the zoomed picture is never above half resolution): a 1920 x 1080 region at 1:1 (what a full-screen view of the zoomed picture shows) costs, measured on the desktop: CPU 28 threads 40 ms (X-Trans), 16 ms (Bayer); one CPU thread 300 ms (X-Trans), 38 ms (Bayer); GPU 9.5 ms (X-Trans), 6.9 - 6.8 ms (Bayer) including the read-back of the region to the host (4 tiles of 1024; most of it is the host-side conversion). So full-resolution pinch-zoom is affordable everywhere for Bayer (a single phone core would need about 0.1 s per screenful) and needs the GPU (or four threads: about 80 ms) for X-Trans; a background full-resolution picture is 75 ms (GPU, X-Trans 40 MP, device-resident) to 0.4 s (CPU, 28 threads) on the desktop.

### 32.5 Quality against LibRaw (the reference), all public sample files

Method: `dapple_demosaic_eval file`: the RGB path (`decode(Full)`: LibRaw AHD for Bayer, 3-pass Markesteijn for X-Trans, `DAPPLE_ANCHOR=off`, `DAPPLE_SHADOW_CHROMA=off`) against the node (`demosaicFull`: same colour matrix, crop, orientation). dE2000 on linear ProPhoto after an exposure gain that puts the median luma at 0.18 (the gain amplifies shadows: dark, noisy scenes weigh more than in the editor); every 2nd pixel in each direction; "border" = the 24 px band; "detail" = the 2 % of 64 x 64 blocks (unclipped) with the highest relative gradient energy, every pixel; zipper = mean second difference of the chroma (R - G, B - G over G) at the CFA Nyquist at pixels where the reference is flat (gradient < 5 % of the luma) / on an edge (> 40 %), lower is cleaner. Pixel 8 Pro and the three Panasonic files are not in the table: they go through the RGB path (32.2).

| file | CFA | MP | full mean / p99 | border 24 px mean | detail crops mean / p99 | zipper flat node / LibRaw | zipper edge node / LibRaw |
|---|---|---|---|---|---|---|---|
| Apple iPhone-XS IMG 1105 | bayer | 12.2 | 0.938 / 3.916 | 1.232 | 1.134 / 4.581 | 0.1018 / 0.0842 | 0.2001 / 0.2042 |
| Canon EOS-6D-Mark-II 6DII-RAW | bayer | 26.2 | 0.625 / 3.244 | 0.898 | 0.859 / 4.882 | 0.0568 / 0.0614 | 0.1623 / 0.1672 |
| Canon EOS-R10 IMG 4361 | bayer | 24.3 | 0.475 / 1.779 | 0.646 | 0.862 / 3.678 | 0.0448 / 0.0541 | 0.1371 / 0.1659 |
| Canon EOS-R5 Canon EOS R5 RAW ISO 100 nocrop n | bayer | 44.7 | 0.702 / 2.614 | 0.788 | 1.175 / 5.294 | 0.0454 / 0.0560 | 0.1162 / 0.1170 |
| Canon EOS-R6-Mark-II canon-r6m2-img 0346-raw-n | bayer | 24.2 | 0.845 / 4.439 | 1.073 | 1.384 / 7.875 | 0.0303 / 0.0332 | 0.0976 / 0.0991 |
| Fujifilm X-A7 DSCF1595 | bayer | 24.1 | 0.402 / 1.518 | 0.440 | 0.500 / 2.234 | 0.0399 / 0.0460 | 0.0913 / 0.0944 |
| Fujifilm X-S10 Fujifilm-X-S10-compressed | xtrans | 26.0 | 0.926 / 3.749 | 0.944 | 1.489 / 5.331 | 0.0293 / 0.0774 | 0.1316 / 0.1692 |
| Fujifilm X-S10 Fujifilm-X-S10-lossless-compres | xtrans | 26.0 | 0.916 / 3.696 | 0.942 | 1.463 / 5.212 | 0.0291 / 0.0769 | 0.1323 / 0.1677 |
| Fujifilm X-T50 DSCF0118 | xtrans | 40.1 | 3.149 / 17.449 | 2.420 | 6.985 / 30.188 | 0.0397 / 0.0972 | 0.2499 / 0.4092 |
| Fujifilm X-T50 DSCF0119 | xtrans | 40.1 | 2.687 / 15.678 | 2.255 | 6.692 / 28.410 | 0.0399 / 0.1001 | 0.2290 / 0.3878 |
| Fujifilm X-T50 DSCF0120 | xtrans | 40.1 | 2.852 / 15.828 | 2.438 | 6.610 / 27.457 | 0.0413 / 0.1023 | 0.2244 / 0.3754 |
| Google Pixel-6-Pro PXL 20220910 093206982 | bayer | 12.5 | 0.593 / 4.420 | 0.504 | 1.406 / 7.103 | 0.0189 / 0.0214 | 0.1116 / 0.1318 |
| Google Pixel-7-Pro PXL 20221204 063131635 | bayer | 11.4 | 0.441 / 1.690 | 0.488 | 0.649 / 2.612 | 0.0483 / 0.0519 | 0.0641 / 0.0661 |
| Leica M10 f5381888 | bayer | 23.9 | 0.680 / 4.461 | 0.740 | 1.468 / 7.602 | 0.0377 / 0.0420 | 0.2021 / 0.2245 |
| Leica Q2 L1000750 | bayer | 47.3 | 0.639 / 4.678 | 0.735 | 1.624 / 8.002 | 0.0747 / 0.0826 | 0.3932 / 0.4210 |
| Nikon D850 Nikon-D850-14bit-compressed | bayer | 45.7 | 1.005 / 6.398 | 0.679 | 2.220 / 10.374 | 0.0320 / 0.0362 | 0.2157 / 0.2375 |
| Nikon D850 Nikon-D850-14bit-lossless-compresse | bayer | 45.7 | 0.993 / 6.284 | 0.673 | 2.196 / 10.272 | 0.0322 / 0.0364 | 0.2130 / 0.2341 |
| Nikon Z-8 Nikon Z8 high efficiency low | bayer | 45.7 | 1.282 / 8.682 | 1.126 | 1.790 / 12.203 | 0.0130 / 0.0147 | 0.2298 / 0.2647 |
| Nikon Z-8 Nikon Z8 raw 14 bit lossless compres | bayer | 45.7 | 1.410 / 9.750 | 1.120 | 2.031 / 13.712 | 0.0184 / 0.0199 | 0.2475 / 0.2837 |
| OM-System OM-1 20mpx P1010025 | bayer | 20.4 | 1.048 / 5.417 | 0.965 | 1.546 / 7.559 | 0.0447 / 0.0459 | 0.1604 / 0.1777 |
| OM-System OM-1 OM Systems OM1 RAW image HiRes  | bayer | 50.1 | 0.382 / 2.051 | 0.389 | 0.589 / 3.057 | 0.0225 / 0.0224 | 0.1111 / 0.1084 |
| Olympus E-M1-Mark-III  3160531 | bayer | 20.5 | 0.545 / 2.047 | 0.543 | 0.563 / 2.285 | 0.0453 / 0.0467 | 0.1060 / 0.1052 |
| OnePlus A6003 IMG 20201019 093659 | bayer | 16.3 | 0.665 / 3.597 | 0.763 | 1.323 / 6.205 | 0.0470 / 0.0479 | 0.1423 / 0.1669 |
| Pentax K-1-Mark-II  IMG2390 | bayer | 36.4 | 0.460 / 2.457 | 0.497 | 0.680 / 3.142 | 0.0282 / 0.0305 | 0.1540 / 0.1564 |
| Pentax K-1-Mark-II  K1I9785 | bayer | 15.6 | 1.442 / 9.083 | 1.231 | 2.023 / 10.865 | 0.0522 / 0.0537 | 0.2823 / 0.2987 |
| Pentax K-3-Mark-III IMGP9854 | bayer | 25.9 | 0.960 / 4.406 | 0.943 | 1.448 / 7.817 | 0.0193 / 0.0237 | 0.1768 / 0.1902 |
| Realme RMX1851 IMG OCT 20191005 163311 | bayer | 16.3 | 0.992 / 4.602 | 0.943 | 1.724 / 8.173 | 0.0642 / 0.0696 | 0.2305 / 0.2494 |
| Sony ILCE-6700 DSC00002 | bayer | 26.0 | 0.980 / 6.090 | 0.951 | 1.580 / 10.282 | 0.0660 / 0.0723 | 0.2707 / 0.2921 |
| Sony ILCE-7M4 ILCE-7M4 DSC06674 FullFrame-Loss | bayer | 32.9 | 0.709 / 3.623 | 0.894 | 1.074 / 8.311 | 0.0498 / 0.0551 | 0.1340 / 0.1432 |
| Sony ILCE-7M5 full compressed | bayer | 32.9 | 0.607 / 3.914 | 0.536 | 1.704 / 9.249 | 0.0227 / 0.0264 | 0.2148 / 0.2165 |
| Sony ILCE-7RM4 DSC00396 | bayer | 61.0 | 0.552 / 2.697 | 0.610 | 0.861 / 4.215 | 0.0270 / 0.0293 | 0.1319 / 0.1378 |
| Xiaomi M2010J19CG IMG 20201224 141323 | bayer | 12.0 | 1.278 / 5.104 | 1.484 | 1.916 / 7.456 | 0.0674 / 0.0881 | 0.2673 / 0.3277 |
| Xiaomi Mi-9-Lite IMG 20200324 073611 | bayer | 12.0 | 0.984 / 5.146 | 0.973 | 2.052 / 9.723 | 0.0308 / 0.0396 | 0.1668 / 0.1990 |

Reading the table: **the gate of section 31.2 (mean dE2000 < 0.5 and p99 < 2 on detail crops against LibRaw) is not met on any file** (best: X-A7 0.50 / 2.2, E-M1 III 0.56 / 2.3): two different demosaic methods differ at the finest detail by construction. The full-frame mean is 0.4 - 1.4 on Bayer files, p99 1.5 - 9.8, and the differences are not colour errors: the low-frequency dE (8 x 8 block means) is 0.22 on the D850 and 0.33 - 0.38 on the Z8. The X-T50 files are the outlier (full 2.7 - 3.1, p99 16 - 17, detail p99 27 - 30): dark foliage with bright sky holes at a high gain (~6), where both methods produce coloured specks, LibRaw's cyan-ish and fewer, ours violet-ish and more (visible in `build/demosaic/Fujifilm_X-T50_*_crop*.jpg`; low-frequency dE there 1.0, p99 7.6); on the X-S10 (an ordinary scene) the same code is at 0.92 mean / 3.7 p99.

Zipper / false colour with ground truth (test `edges`): on grey steps, 1 px and 2 px lines and a diagonal the chroma the node adds is 0.0006 (Bayer) / 0.0013 (X-Trans) against 0.021 / 0.017 for bilinear (PSNR 46 / 40 dB against 22 dB). On real files the node's zipper energy is at or below LibRaw's on flat areas (one exception: iPhone XS 0.102 against 0.084) and on edges on all files but two (OM-1 HiRes +2 %, E-M1 III +1 %; X-T50 0.23 against 0.39, X-S10 0.13 against 0.17). Highlights: no cast beyond the data (test `clip`: 0 of 784 interior pixels of a clipped blob move by more than 0.03 from the data colour); real files: dE2000 at pixels where the reference has a channel >= 0.98 is 0.00 - 0.6 on Bayer files (X-A7 1.7), 4 - 7 on the X-T50 (the specks again).

**Visual judgement** (contact sheets and 200 % crops `build/demosaic/*_sheet.jpg`, `*_crop*.jpg`; left LibRaw, then the node, the difference x4, then bilinear where it was run): fine foliage, text and diagonal edges on the Bayer files (A7R IV, D850, R5, OM-1): indistinguishable at 200 % except single-pixel differences along the sharpest twigs; the node's thin lines are a little crisper and a little more prone to a one-pixel colour fringe than AHD's. Skin: no portrait in the public set; skin-coloured regions of the iPhone XS and the R5 are within 0.3 dE of LibRaw. X-Trans: text, edges and diagonals on the X-S10 look equivalent (an "M" in `Fujifilm_X-S10_*_crop0.jpg` is as clean as LibRaw's); in the X-T50 foliage the node is **worse** than LibRaw (coloured specks around specular holes, noisier chroma) although its zipper metric is lower (the median removes the periodic pattern, not the specks). So: Bayer is a usable replacement at the same visual quality; X-Trans is a first version that is not yet a replacement for LibRaw's 3-pass method on the owner's camera in difficult light. The honest next step is the homogeneity-based selection among directional candidates (Markesteijn's actual idea), 32.9.

### 32.6 GPU against CPU, tiles, backends (ctest `demosaic.*`)

`dapple_demosaic_tests <id>`; ctest ids `demosaic.<id>` (CPU: `flat edges zoneplate hotpixel clip tiles threads cache`; GPU: `gpu_parity gpu_tiles gpu_real`, each also as `_swiftshader` (Vulkan on SwiftShader) and `_gles_angle_vulkan` / `_gles_angle_swiftshader` (GLES 3.1 through ANGLE); exit 77 = no device). All tests of the build (`build/demosaic-check`, the check-all copy with `--quick`: configure, build, ctest 125 of 125 passed in 169 s with `-j 2`, golden self-check, flutter analyze, video host tests; flutter test / JVM tests skipped by `--quick`) were green when the copy ran; the golden corpus does not decode RAW files, so it is unaffected by construction and stayed byte-identical.

| Test | What it checks | Result |
|---|---|---|
| `flat` | flat fields of 3 colours, both CFAs, including the borders; a frame at black is exactly 0, a saturated frame exactly 1 | max error 3e-5 |
| `edges` | colour steps (PSNR against bilinear) and a grey scene of steps, 1 px / 2 px lines and a diagonal (false colour) | colour edges 32.4 / 32.8 dB (bilinear 32.1 / 32.2); grey scene chroma 0.0006 / 0.0013 against 0.021 / 0.017 (bilinear), PSNR 46 / 40 dB against 22 dB |
| `zoneplate` | synthetic zone plate to Nyquist | Bayer 15.5 dB, X-Trans 13.1 dB, bilinear 12.4 / 12.1 dB |
| `hotpixel`, `clip` | one hot sample; a clipped highlight blob | hot sample: see 32.3 (green site 1 pixel, red / blue site 9 (Bayer) / 18 (X-Trans) pixels); clipped blob: 0 of 784 interior pixels deviate by more than 0.03 from the data colour, output inside [0, 1] |
| `tiles` | 40 random regions of interest against the whole frame, 2 threads, odd picture size | 0 differing values (halo 4 / 6) |
| `threads` | 1 thread against 7 threads | identical |
| `cache` | hit / miss / eviction, same object on a hit, real-file mosaic re-open | passes (re-open 0.4 ms) |
| `gpu_parity` | 333 x 271 zone plate, non-trivial matrix and multipliers, 128 px tiles, GPU against the CPU node | **Vulkan RTX 4060 Ti**: fp32 output mean dE2000 0.00001, p99 0.00006 (Bayer) / 0.00013 (X-Trans), max abs 3e-6 / 2e-5; RGBA16F output (the pipeline format) mean 0.0117, p99 0.042 - 0.045 (the fp16 rounding of the stored result, budget mean <= 0.1, p99 <= 1.0); **Vulkan SwiftShader**: fp32 bit-identical to the CPU (0 differing), fp16 as above; **GLES through ANGLE**: same numbers on the RTX (Vulkan backend) and on SwiftShader |
| `gpu_tiles` | whole frame as one tile against 64 px tiles; a region of interest against the whole frame | 0 differing values on all four device paths |
| `gpu_real` | 1200 x 800 crop of the X-T50 and A7R IV files, GPU against CPU | mean dE2000 0.00001, p99 0.00006 (RTX); 0 (SwiftShader, both APIs) |

### 32.7 Speed

Desktop: this PC (28 threads), RTX 4060 Ti over Vulkan, files from build/rawsamples, warm file cache, `dapple_demosaic_eval --no-ref --proxy --gpu 1024`, best of 3 after a warm-up. The machine is shared with the other engineers (CPU timings of the 28-thread runs varied by up to 35 % between runs, GPU timings by 5 %). "Decode" = `decodeRawMosaic` (LibRaw unpack + the copy of the active area), "GPU" = window upload + all passes, output kept on the device (RGBA16F), "+ read-back" = the same with the result read back as float RGB on the host (the export case; 480 / 732 MB of host traffic).

| file | MP | decode | GPU node | GPU + read-back | CPU node 28 / 4 / 1 threads | file -> full-resolution linear RGB: GPU device-resident / GPU + host RGB / CPU 28 threads | RGB path today (31.1, 28 threads) |
|---|---|---|---|---|---|---|---|
| X-T50 uncompressed RAF | 40.1 | 0.11 s | **74 ms** | 192 ms | 0.39 / 1.44 / 5.5 s (DSCF0119; 0.53 s in a loaded run) | **0.19 s** / 0.31 s / 0.50 s | 3.35 s (1 thread 15.9 s) |
| X-T50 compressed RAF | 40.1 | 0.21 s | **75 ms** | 190 ms | same | **0.29 s** / 0.40 s / 0.60 s | 3.43 s |
| X-S10 compressed RAF | 26.0 | 0.17 s | 49 ms | 125 ms | 0.35 s (loaded) | 0.22 s / 0.29 s / 0.52 s | 2.50 s |
| A7R IV ARW | 61.0 | 0.26 s | **32 ms** | 215 ms | 0.19 / 0.34 / 0.92 s | 0.29 s / 0.47 s / 0.44 s | 2.12 s (1 thread 4.01 s) |
| D850 lossless NEF | 45.7 | 0.48 s | 25 ms | 158 ms | 0.28 s (loaded) | 0.51 s / 0.64 s / 0.77 s | 2.45 s |

The targets of the brief are met on the desktop GPU: the 40 MP X-Trans demosaic is **74 ms** (target: well under 300 ms) and the whole way from file to full-resolution linear RGB is **0.19 - 0.40 s** (target: about 1 s or less) against 3.4 s for the RGB path. LibRaw's own X-Trans demosaic is 1.96 s (28 threads) / 14.4 s (1 thread); the CPU version of the node is 0.39 s / 5.5 s, i.e. 5x / 2.6x faster than LibRaw's on the same machine, and 4 threads (a phone's big cores) take 1.44 s for 40 MP X-Trans instead of 4.4 s. Bayer on the CPU is 0.19 s (61 MP, 28 threads) against LibRaw's 0.25 s: no faster, the CPU node is for the devices without a usable GPU and for the region of interest. What remains is the decompression (LibRaw, serial for everything but compressed Fuji: 31.3) and, for the export, the read-back: with the device-resident output feeding the point chain the sequence is decode + 25 - 75 ms.

Where the GPU time goes (X-T50, 48 tiles of 1024): the host builds each tile's window (10 ms in total), the rest is 48 serial submit-and-wait round trips with 4 dispatches each: most of the 64 ms is per-tile latency, not arithmetic (the 2048 tile is 72 ms, the A7R IV with 70 tiles of 3 dispatches is 32 ms), so a recorder that batches several tiles per submission (section 19's scheduler does) will be faster; I did not optimise the kernels (no shared-memory tiling, the scaling pass is separate).

Other devices of this machine:

* **GLES 3.1 through ANGLE** (desktop test target, RTX 4060 Ti under ANGLE's Vulkan backend): correct (32.6) but **12x slower**: X-Trans 40 MP 0.93 s, A7R IV 0.16 s (7x), regardless of the tile size (512 / 1024 / 2048), so it is the kernels' translation, not the submission count. Not investigated further; suspects are the dynamic indexing of the constant direction arrays and of the `uvec4` uniform in `demosaic_xtrans.comp` / `demosaic_common.glsl`, and `rgb[hc]` in the Bayer kernel. The film kernels run at parity on the phones' native GLES (section 25); whether a phone's GLES driver shows the same penalty for these kernels is not measured.
* **SwiftShader (Vulkan, software)**: correctness only; bit-identical to the CPU node.
* **Android x86_64 emulator** (own AVD `dapple_n5` on port 5560, 4 vCPU, software Vulkan, stopped afterwards; the NDK x86_64 build of the same eval tool and the tests; `demosaic.flat edges zoneplate hotpixel clip tiles threads cache gpu_parity gpu_tiles` pass there, GPU parity bit-identical on the emulator's SwiftShader): mosaic decode 0.39 s (X-T50 uncompressed) / 0.58 s (compressed) / 0.41 s (X-S10) / 0.34 s (A7R IV) / 0.53 s (D850); CPU node with 4 threads 2.0 s (X-T50 40 MP X-Trans), 1.2 s (X-S10 26 MP), 0.55 - 1.7 s (D850 / A7R IV Bayer, large run-to-run spread); 1920 x 1080 region at 1:1: 138 ms (X-T50, 4 threads) / 27 ms (A7R IV); binned proxy 0.27 s (X-T50, 1 thread); SwiftShader GPU 3.8 s per 40 MP, 201 ms per region (meaningless for a phone). The previous measurement on this emulator was X-T50 compressed full decode 6.8 s real (section 31.1), proxy 1.1 s: with the node the CPU full decode is about 0.58 + 1.9 = 2.5 s.

**Predictions for phones (not measured; from `device-results/find-n5.md` and `redmi-garnet.md`; mark them as such):** the kernels are not at the memory-bandwidth limit on the desktop and are not tuned, so the usual sources of error are an Adreno cost cliff above 16 MiB per bound plane (tile 1024 keeps every plane below it) and the ALU-bound passes of section 28 (the X-Trans chroma pass reads 2 x 25 taps per pixel). Scaling the desktop's 74 ms (X-Trans 40 MP) / 32 ms (Bayer 61 MP) by the measured ratios of the film passes (Find N5: 5 - 10x slower than the RTX for ALU-bound passes, about 100 GB/s on simple ones; Redmi: another 3 - 4x slower than the N5): **Find N5 (Adreno 840): X-Trans 40 MP 0.3 - 0.7 s, Bayer 61 MP 0.15 - 0.3 s, region 1920 x 1080 about 20 - 40 ms (X-Trans); Redmi (garnet): X-Trans 40 MP 1 - 2.5 s, Bayer 61 MP 0.5 - 1 s, region about 0.1 s.** Plus the decompression on the phone's CPU (0.2 - 0.6 s for the RAF, ARW, NEF samples on the desktop; the emulator needed 0.34 - 0.58 s) and 0.1 - 0.2 s of read-back only where the result is read back. File to full-resolution linear RGB on the N5 would then be about 0.6 - 1.2 s (X-Trans 40 MP), 0.5 - 0.8 s (Bayer 61 MP): inside the 1 s target on the N5, not on the Redmi.

### 32.8 Cache and the open sequence

* `MosaicCache` (LRU, byte budget, default 600 MB): key = `fileContentKey(path)` (size + 64 KB head + 64 KB tail, 128 bit, independent of path and mtime) for the mosaic, the same key xor the algorithm version for the demosaiced full picture. A hit returns the same `shared_ptr` (test `cache`: the second `full()` returns the identical object, no second demosaic; real file X-S10 compressed: re-open 0.4 ms against 360 ms; an evicted entry stays valid for a consumer that still holds it). Budget arithmetic: a 40 MP mosaic is 80 MB, a 61 MP one 122 MB, the demosaiced float picture 480 / 732 MB: on a phone the cache should hold the mosaic only (budget ~ 2 mosaics) and re-demosaic the visible region on the GPU; on the desktop it can hold both. The graph-level version (a `ResultCache` entry per `NodeType::Demosaic`, per region) is the hunk in 32.9.
* Open sequence proposed for the app (not applied, 32.9): (1) the embedded JPEG at once (exists); (2) the proxy: measured on the desktop, X-T50 40 MP: LibRaw half-size decode (section 31.1) 0.53 - 0.63 s; mosaic decode 0.11 s (uncompressed) / 0.21 s (compressed) plus `binnedHalf` (single-threaded, not optimised) 0.25 s = 0.36 / 0.46 s, and the binning is trivially parallel (a GPU pass or 28 threads: about 10 - 25 ms), so the mosaic proxy is the faster first picture as soon as `binnedHalf` is threaded, and its decompression is not thrown away (the LibRaw half-size path decodes the file again for the full picture). Until then the existing half-size decode is within 0.1 - 0.2 s of it: keep it, it is not worth a switch by itself. "Tap to first correct picture": 0.36 - 0.46 s (mosaic route, one thread) against 0.53 - 0.65 s today; "tap to the full-resolution picture of the visible region": the mosaic is already cached from the proxy step, so 7 - 10 ms (GPU) to 40 ms (CPU, 28 threads) after it, i.e. about 0.4 - 0.5 s after the tap; "tap to the whole full-resolution picture": 0.2 s (GPU, device-resident) to 0.5 s (CPU, 28 threads) after the tap, against 3.4 s today; (3) at the first pinch beyond 1:2: `demosaicOrientedRegion` of the visible rectangle + margin on the GPU where the device has one (the mosaic is cached from step 2); (4) the full-resolution picture in the background and, on export, the same node (the export path today decodes again: 3.4 s on the desktop for the X-T50).

### 32.9 Hunks for the lead, what is not done, what is not verified

**Edits I made outside my own files** (all additive, all small, re-read before merging): `core/CMakeLists.txt` (two source lines after `cpu_nodes.cpp`), `core/gpu/CMakeLists.txt` (the include + `list(APPEND DAPPLE_GPU_SHADERS ...)` after the film lines, and `${DAPPLE_DEMOSAIC_GPU_SOURCES}` in the `dapple_gpu` sources), `core/engine/hal/gles/shader_list.cmake` (the same include appended at the end), `tests/CMakeLists.txt` (one include line at the end), and in `core/src/decoders/libraw_decoder.cpp` (engine-gaps, exactly two lines: `#include "dapple/mosaic.h"` after `#include "dapple/decoder.h"`, and `#include "decoders/libraw_mosaic.inc"` just before `std::unique_ptr<ImageDecoder> makeLibRawDecoder()`). The `.inc` uses these helpers of that file: `PathDatastream`, `readMetadata`, `readDngWhiteXy`, `dngList`, `fillBlackPattern`, `dngDefaultCrop`, `dngOpcodesEnabled`, `rotateFlip`, `gcdInt`, `parallelRows` and the `dng::` calls; a rename or a signature change there breaks that include at compile time (it will say so). Nothing in `core/engine/exec/gpu_*`, `film_gpu*`, `film_match_cache.cpp`, `exposure_anchor.*`, `developer.cpp`, `hal/metal/**` was touched.

**Hunk 1, graph + planner + executors (not applied):**
1. `graph/types.h`: append `NodeType::Demosaic` before `Count`. `graph/registry.cpp`: `def(NodeType::Demosaic, "Demosaic", Domain::Image, Footprint::Window, 1, 1, P.mask({"ctx.source", "ctx.cfa"}))`; `graph/params.cpp`: a context key `ctx.cfa` hashing `MosaicImage::key`, `cfaIdx`, the black table, `white`, `blackMin`, `wb`, `camToWork` and `demosaic::kAlgoVersion`. Halo of the node: `demosaic::halo(cfa)` (4 / 6). `graph/builder.cpp`: a `GraphDetail::RawMosaic` that wires `Source(mosaic) -> Demosaic -> Resize / Geometry ...`; existing graphs and `tests/engine/graph_snapshots.txt` do not change (run `dapple_engine_tests keys` and `conformance` for the new node: its mask must list `ctx.cfa`).
2. Source: `SourceStore` / `SessionCtx` hold the `MosaicImage` (80 MB at 40 MP) instead of the float RGB image (480 MB) when the session was opened from a mosaic; `Source` then has `channels = 1`, `format = Mosaic16`, `cfa`, `levels` (that is all of `MosaicImage` except `data`).
3. Planner: a `GroupKind::Demosaic` group in front of the PointChain head of every TileSweep, `inHalo = demosaic::halo(cfa)`, output = RGB float window of the tile + the sweep's halo. The point chain already multiplies the white balance and the colour matrix: set `Desc::m` to the identity and fold `camToWork` into `point.comp`'s matrix (and `wb` stays in the scaling pass), then the output pass of the node is a plain write. `Plan::cache`: `CacheOp{NodeType::Demosaic, key = Hash(ctx.cfa, region), Lookup / Store}` for the whole-frame result of an export or a settled editor frame; per-region keys for pinch-zoom windows.
4. CPU executor: the `tile` slot of `cpu_nodes.cpp` calls `demosaic::demosaicWindow(desc, S, roi, out, stride)` (S built by the same window loop as `demosaic()`'s `buildWindow`; make that function public).
5. GPU executor (`nodes/gpu_nodes.cpp`, `exec/gpu_executor.cpp`: the performance engineer's): `gpuImplFor(NodeType::Demosaic)` -> `recordDemosaic(GpuRecorder& r, const DemosaicSrc&, WRect roi, H* out)` = the dispatches of `GpuDemosaic::run` (`r.pl("dm_scale", {{0, period}})`, `r.pl("dm_bg")` ..., `r.gx / r.gy`), the planes as `r.g.transient` images or buffers, the mosaic window uploaded like `recordConvert` (`r.g.externalBuf(staging)`), the last pass writing the `RGBA16F` working image that `recordPoint` reads (`dm_bo16` / `dm_xo16` already round like the other kernels with `h16v`); add the type to `headNodeOk` / `windowNodeOk`. About 150 lines; per-tile work is already tile-invariant (`gpu_tiles`: 0 differing values).

**Hunk 2, C API and Dart (not applied):** `dapple_session_open(path, DAPPLE_QUALITY_FULL, long_edge)`: when `decodeRawMosaic` returns a mosaic, take it from a process-wide `MosaicCache` and build the source with the node (GPU when the provider is on, else `demosaicFull`): the full decode of the X-T50 drops from 3.35 s (15.9 s one thread) / 1.1 GB to 0.2 - 0.3 s GPU, 0.6 s CPU (28 threads) / 0.57 GB, and `dapple_session_export_jpeg` finds the mosaic cached. New `dapple_session_open_region(session, x0, y0, x1, y1)` (oriented, cropped picture coordinates, via `mosaicRectOf`): replaces the session's detail source by the visible rectangle at 1:1 (GPU 7 - 10 ms for 1920 x 1080 + read-back on the desktop). `app/lib/services/session_cache.dart`: `_openNew` stays (proxy), `_openDetail` (today `full: false, longEdge: detailLongEdge`, i.e. half resolution resized) becomes a region session driven by the visible rectangle of the zoom view, plus a background full-resolution session after the first idle second. The exposure anchor needs an RGB picture: `decodeRawMosaic` leaves `meta.exposureAnchor*` unset, so attach it from `binnedHalf` (a half-size picture in 0.25 - 0.4 s single thread, the embedded-preview decode overlapped as `decode()` does).

**Hunk 3, the X-Trans quality plan (the part that is worth the most for the owner's camera):** a directional-candidate demosaic with homogeneity selection: (a) green in 4 directions (H, V, D1, D2) with the colour second-difference term, (b) red / blue per direction as colour differences along that direction, (c) each candidate to a perceptual space (the YPbPr of the 3 x 3 neighbourhood is enough), derivative map per candidate, homogeneity = count of the neighbours within the smallest derivative + epsilon, (d) blend by the 3 x 3 homogeneity sum. GPU: candidates in 4 vec4 planes (+ derivative maps), 5 passes, halo about 8; it fits the tile machinery as is. The 3-pass tier repeats (b) - (d) on the first result. Expect 3 - 4x the present X-Trans cost (0.2 - 0.3 s on the desktop GPU, still inside the target). Clean-room from Markesteijn's description, or from LibRaw `xtrans_demosaic.cpp` under CDDL-1.0 with its file-level obligations (docs/legal/demosaic.md).

**Not done:** the planner / executor / `EngineSession` / C API / Dart wiring (hunks above); Markesteijn-class X-Trans (hunk 3); the 3-pass quality tier; a parallel decompressor for Fuji compressed (31.3; `mosaic decode` is still LibRaw's `unpack`: X-T50 uncompressed 0.11 s, compressed 0.21 s, X-S10 compressed 0.17 s, D850 lossless NEF 0.48 s, Z8 HE 1.2 s); the exposure anchor for the mosaic path (needs the proxy); AsShotWhiteXY DNGs (32.2); Panasonic `zero_is_bad` files; the graph-level `ResultCache` entry (the cache here is `MosaicCache`, same semantics); fusing the scaling pass into the first kernel (one pass less per tile; the Bayer path has 3 dispatches per tile, X-Trans 4).

**Not verified:** Metal (the kernels use only storage buffers, `rgba16f` / `rgba32f` write-only images, one 144-byte uniform block and spec constants 0 / 16 / 17: `dapple_shaderc --msl` should translate them like the film kernels, but I could not compile; dynamic indexing of a `vec3` in `demosaic_bayer.comp` (`rgb[hc]`) and the `uvec4` uniform member are the two places I would look first); any real phone GPU (Adreno / Mali / Apple): the phone numbers in 32.7 are predictions from the device throughput tables; the Android emulator numbers are x86_64 software-rendered Vulkan / CPU and say nothing about a phone GPU; the Vulkan path on SwiftShader and the GLES path through ANGLE (RTX 4060 Ti Vulkan and SwiftShader) were verified, an ESSL compile on a real GLES driver was not.

### 32.10 Round 2 (2026-11-13): candidate-direction X-Trans, the decoder wiring, the white-point fix

**X-Trans method replaced.** `Desc::xtMode` 1 (default) is a Markesteijn-class method derived independently (the published idea: several candidate directions, colour differences per direction, per-pixel selection by homogeneity in a perceptual space; none of dcraw / LibRaw / RawTherapee / darktable code was read or ported, nothing was ported from CDDL code, so no licence obligation arises; the earlier "fast" method stays as `xtMode` 0 / `demosaic_xfast.comp`, CPU-parity tested only):

1. Green candidate per direction d in {H, V, D1, D2}: the two nearest greens along d (every one of the 16 non-green phases has both within 3 steps in all four directions: checked by enumeration) blended by distance, plus a directional Laplacian term (0.25 x the sample's excess over its own colour along d), clamped to the two greens.
2. Colour differences (R - G, B - G) per direction: guide-weighted mean over the samples of that colour within one pixel of the line through the pixel along d (radius 3.2, weights 1 / (r^2 (1 + perp^2) (0.02 + |G(q) - G(p)|))). **Clip rule:** differences come from unclipped samples only (a clipped sample's difference is a lower bound; extrapolating it into a pixel whose green is much lower is what made the violet specks around sky holes); only when every sample of that colour nearby is clipped and their green is as bright as the pixel's (we are inside the clipped region; similarity mass >= 4) are the clipped ones used, otherwise the difference is neutral.
3. Each direction is an RGB candidate. Derivative map in a cube-root colour space (matrix to the working space, then cbrt; sum of squared differences to the 4 neighbours) + a penalty (0.3) for a candidate whose line crosses a clipped green. Homogeneity of a candidate at p = number of the 3 x 3 pixels around p where its derivative is within a factor 1.5 (+ 2e-4) of the best candidate's; the result is the mean of the candidates within 1 count of the best.
4. 3 x 3 median of the two colour differences (isolated colour speckle is an outlier, an edge is not), matrix, output. Halo 9 (green +-3, differences +-3 around a ring of 2, derivatives and homogeneity +-1 each).
The 3-pass refinement tier is **not implemented** (see below).

**Quality, X-T50 and X-S10 against LibRaw's 3-pass (dE2000; `dapple_demosaic_eval`)**

| file | full mean / p99 | low-frequency (8 x 8) mean / p99 | detail crops mean / p99 | zipper flat node / LibRaw | zipper edge node / LibRaw | round 1 detail |
|---|---|---|---|---|---|---|
| X-T50 DSCF0118 (uncompressed) | 2.65 / 13.8 | 0.66 / 3.3 | 5.13 / 20.9 | 0.044 / 0.097 | 0.226 / 0.409 | 6.99 / 30.2 |
| X-T50 DSCF0119 | 2.28 / 12.5 | 0.56 / 2.9 | 5.01 / 20.0 | 0.045 / 0.100 | 0.212 / 0.388 | 6.69 / 28.4 |
| X-T50 DSCF0120 | 2.43 / 12.7 | 0.59 / 3.0 | 5.04 / 19.5 | 0.047 / 0.102 | 0.206 / 0.375 | 6.61 / 27.5 |
| X-S10 compressed | 0.88 / 3.6 | 0.20 / 1.0 | 1.51 / 5.3 | 0.035 / 0.077 | 0.110 / 0.169 | 1.49 / 5.3 |
| X-S10 lossless compressed | 0.87 / 3.6 | 0.20 / 1.0 | 1.49 / 5.2 | 0.035 / 0.077 | 0.111 / 0.168 | 1.46 / 5.2 |

**The working target (detail crops mean <= 1.0, p99 <= 4) is not reached and cannot be reached against this reference by any independent method**: LibRaw's own 1-pass Markesteijn (`user_qual 2`, the very method family) compared with its 3-pass (`DEMO_VS_LIBRAW1=1`) gives detail mean 0.89, **p99 10.4**, full mean 0.33, p99 5.0 on DSCF0119. We reach detail 5.0 / 20 on the X-T50 (the dark, noisy, high-gain foliage scene: the exposure gain of the metric is ~6, so noise-level colour differences of dark pixels dominate; 1.6 % of the detail-block pixels have dE > 20, most of them below 0.08 in linear value) and 1.5 / 5.3 on the X-S10. The low-frequency dE (colour accuracy without the detail) fell from 1.0 / 7.6 (round 1) to 0.56 / 2.9 on DSCF0119. Parameter sweeps (threshold factor, tie width, Laplacian weight, guide floor, clip rules, shrinkage) moved the X-T50 detail mean only between 4.9 and 5.5.

**Visual verdict at 200 % (`build/demosaic/r2_*_crop*.jpg`: LibRaw | node | difference x 4)**: DSCF0118 crop0 (bright sky holes in dark foliage, the crop the lead flagged): the violet / magenta specks and fringes of round 1 are gone, the node and LibRaw look the same at 200 %; what remains in the difference panel is single-pixel colour noise at the brightest edges, at the level LibRaw itself shows (round 1's panel had bold magenta clusters). DSCF0119 crops 0 - 2 (sky through leaves): same, residual blue / yellow single pixels at hole edges in both renderings, no worse than LibRaw. DSCF0120 crop0 (specular water highlights): equivalent. X-S10 crop0 / crop2 (text "M", "de"): letters and diagonal strokes are as clean as LibRaw's, the difference panel is flat, no maze pattern or zipper in the flat areas. Fabric / skin: no such content in the public X-T50 / X-S10 files (the Bayer sets in 32.5 cover them). No 3-pass column exists because the tier is not built. Zipper: flat areas 0.044 against LibRaw's 0.097, edges 0.21 against 0.39 (lower is cleaner). Hot pixel (test `hotpixel`): a hot red / blue sample leaves no trace (the median removes it), a hot green sample stays one pixel.

**Speed, X-Trans 1-pass (RTX 4060 Ti, Vulkan, tile 512 / 1024)**: 40 MP X-T50 file DSCF0119, device-resident output: **113 ms (tile 512) / 122 ms (tile 1024)**, of which the host window build is 10 ms (the fast tier of round 1 was 74 ms: the candidate method costs about 1.6x); target well under 300 ms: met. A 1920 x 1080 region at 1:1 including the read-back: 12 ms (GPU), 65 ms (CPU, 28 threads), 410 ms (CPU, 1 thread). Device memory of the candidate planes: 21 MB (tile 512), 80 MB (tile 1024; 68 B per window pixel: scaled plane, 4 greens, 8 colour differences, 4 derivatives). CPU node, 40 MP: **0.73 s (28 threads), 1.21 s (8 threads), 2.07 s (4 threads)**, 1 thread about 10 s (LibRaw's 3-pass: 1.96 s / 4.4 s on 4 / 14.4 s on 1): the CPU node is faster than LibRaw's from 4 threads up, slower below (the 5.5 s of the fast tier). The 3-pass tier does not exist, so there is no 3-pass cost to report.

**GLES / ANGLE slowdown: two causes, one fixed.** (1) *Shader compile time* (a bug of the first candidate-method shader, found by the full check hanging for over an hour in the `gles_*_angle_swiftshader` tests at 9 GB): 5 x 4 inlined candidate evaluations and 4 inlined 49-tap loops took minutes to compile on ANGLE + SwiftShader and would have delayed the first GLES use on a phone. The kernels now call each helper from one site in a loop (`for dir`, `for n`): `demosaic.gpu_parity` through ANGLE / SwiftShader went from over three minutes to 1.8 s, results unchanged (parity to 1e-5). (2) *Run time on ANGLE's Vulkan backend*: not the ESSL translation and not the dynamic indexing (removing every dynamic index of a uniform vector, the vec3 swizzle and the constant arrays changed nothing). The same kernels through ANGLE's OpenGL backend (the NVIDIA GL driver) run Bayer 61 MP in 75 ms (Vulkan native 34) and X-Trans 1-pass 40 MP in 189 ms; only ANGLE's Vulkan backend is slow (Bayer 160 ms, X-Trans 1-pass 1.9 s): every dispatch costs 5 - 10x its native time. The likely reason (not proven; ANGLE has no switch I could flip here) is ANGLE-on-Vulkan's robust buffer access bounds checks on every storage-buffer access, which hurt kernels with 25 - 50 SSBO reads per pixel most. A phone's native GLES driver is the ANGLE-GL case, not the ANGLE-Vulkan one. ANGLE's D3D11 backend still asserts in its HLSL output on some array constructor of a shader of the set (the old X-Trans kernel had them; which pipeline remains was not tracked down; D3D11 is not a target).

**Optional hot-sample filter** (`Desc::hot`, env `DAPPLE_DEMOSAIC_HOT=1`, off by default and off when the file's opcodes fixed bad pixels: `MosaicImage::fixedBadPixels`): a sample well above (0.1) every same-colour neighbour within 3 pixels while those neighbours are far from clipping (< 0.9: a real highlight has bright neighbours) is replaced by the trimmed mean of the neighbours; CPU and GPU (in the scaling pass, 3 more samples of staged window per side), tile-invariant and GPU / CPU within 1e-6 (test `hotfilter`). With the median stage the X-Trans node no longer needs it for red / blue sites; Bayer does (a hot red / blue site is a 9-pixel blob without it).

**The white-point finding, fixed.** `libraw_decoder.cpp` `decode()`: for DNGs with only AsShotWhiteXY the white-point multipliers went into `params.user_mul` but `use_camera_wb` stayed on, and LibRaw's `scale_colors` then ran its automatic white balance (empty `cam_mul`) over them. Now `use_camera_wb = 0` for these files (one line), the mosaic path takes them too. Pixel 8 Pro (`build/demosaic/Google_Pixel-8-Pro_*_sheet.jpg` before: LibRaw path left bluish-grey, the white-point multipliers right magenta-purple; `p8_after_sheet.jpg` after: both paths identical, channel means ours / LibRaw 1.000, dE2000 0.22 / 0.62 detail; `p8_after_preview.jpg` is the camera's own rendering: pink-purple clouds over a violet-orange horizon, which both paths now match): the intended multipliers were right, the RGB path was ignoring them.

**Wiring (nothing in the planner / executor / gpu_executor.cpp / film files touched)**

* **Decoder path.** `LibRawDecoder::decode(Full)` first calls `decodeFullFromMosaic` (core/src/decoders/libraw_mosaic.inc): process-wide `MosaicCache` (700 MB budget, content-hash key), `demosaicFull` (GPU backend when installed and the runtime switch is on, else the CPU node), shadow chroma floor, exposure anchor; any refusal (`cfa == None`: sRAW, monochrome, linear DNG, JPEG XL, Panasonic zero_is_bad ...) falls through to the unchanged RGB path with the reason logged once on stderr. `DAPPLE_DEMOSAIC=off` forces LibRaw (A/B, oracle). The C API's `dapple_session_open(path, FULL, longEdge)` and every export that opens FULL therefore use it, and a re-open is a cache hit.
* **GPU device.** The executor's provider hands out executors and Vulkan handles but no `hal::Device`, so sharing "the way the presenter does" would mean editing `gpu_context.cpp` (the performance engineer's, mid-edit): the node's backend (`demosaic_gpu.cpp`, `demosaic_backend.h`) creates its own device on first use through the same selection (Vulkan -> GLES 3.1, honours the runtime switch and `DAPPLE_GPU_API`), 30 - 80 MB of buffers, tile 512 on Android / Apple, 1024 elsewhere; `c_api.cpp` installs it next to the provider. If the lead wants one device, `GpuProvider` needs a `hal::Device* sharedDevice()` and the backend takes it instead of `selectDevice`.
* **Region open.** `dapple_session_open_region(path, x0, y0, x1, y1)` (oriented, cropped full-resolution coordinates; new in `dapple_c.h`) and `dapple_region_cancel()` (latest wins: a newer open cancels the one in flight; polled between tiles on CPU and GPU). The session's source is that rectangle with the exposure anchor of the whole picture. Lens-warped DNGs (OpcodeList3) and non-mosaic files are refused with a reason.
* **Exposure anchor on the mosaic path.** Computed once per file from the cropped, oriented half-size binned picture (`binnedOriented`), with the embedded preview / sibling JPEG logic of the RGB path, cached next to the mosaic: against the RGB path's anchor on five sample files, difference 0.0000 - 0.0066 EV (limit 0.02), same tier on all (test `wire_anchor`).
* **Dart.** `dapple_bindings.dart` / `dapple_session.dart`: `DappleSession.openRegion`, `cancelRegion`, `hasRegions` (the performance engineer's touch-hint hunks untouched); `session_cache.dart`: `regionDetail` (one region at a time, <= 3200 px on the long edge = 92 MB, a newer request cancels and replaces, closed with the detail session, files the native side refuses are remembered); `editor_engine.dart` `_renderDetail`: true-resolution region first when the edit acts on pixels only (no crop / rotate / flip / straighten / vignette, which refer to the whole picture's geometry), otherwise the existing half-resolution detail session. The first picture stays the existing proxy. `flutter analyze`: no issues.
* **Tests** (`demosaic.wire_*`, real sample files, exit 77 without them): `wire_region` region of 6 random rectangles equals the same rectangle of the full open **bit for bit** on 4 camera files (X-S10, R10, A7M5, D850; the lens-warped Pixel 7 Pro DNG is refused as designed); `wire_anchor` anchors within 0.0066 EV; `wire_cache` first open decodes (234 ms), second region 23 ms, a full open: 2 hits, 0 decodes; `wire_cancel` a whole-frame open cancelled after 30 ms returns after 244 ms; `wire_gpu` the decoder's GPU backend against the CPU node on a 1200 x 900 region: mean dE2000 0.015 - 0.020, p99 0.05 - 0.08.

**Gates**: Android arm64 build of `dapple_core` and `dapple_gpu` (NDK, own dir `build/demosaic-arm64`, -j4): compiles. (The libc++ `false_type / true_type` error the lead saw came from an earlier state of `libraw_mosaic.inc` that still had standard `#include`s inside `namespace dapple`; removed; the current tree compiles for arm64 and x86_64.)

**Not done in round 2**: the 3-pass refinement tier (the single-pass result is already within the noise of what LibRaw's own 1-pass vs 3-pass differ by; a second round would re-estimate the directional differences from the first result and re-select, +3 passes, halo +3, about 2x the 1-pass cost); one shared GPU device (needs the provider method above); true-resolution region for edits with geometry (needs the engine to accept a source rectangle + origin: planner hunk); Metal compile.

### 32.11 Round 3 (2026-11-13): the X-Trans node on mobile GPUs, the runtime verdict

**Problem.** The candidate-direction X-Trans path (32.10) is the default full-resolution decode, 113 ms for 40 MP on a desktop GPU, but 1585 ms on an Apple M1 (Metal, measured by the iOS round, no faster than the CPU node) and it had never run on a phone. On the Redmi (2312DRA50C, Adreno 710, Vulkan) the first measurement was **10.3 s for the 40 MP X-T50 file** (compute only, tile 512), the same as the CPU node at 4 or 8 threads (10.3 / 10.5 s): the GPU path was no better than the CPU there either.

**Where the time went (Redmi).** Three things, found by skipping dispatches (`DEMO_SKIP`) and timing the whole frame, because the per-pass timestamps of this driver were not trustworthy (a pass's tail is booked to the next timestamp; they sum to a fraction of the wall time):

1. *Every window pixel evaluated the candidate colours five times* (derivative pass: 20 `candCube` calls, 60 `pow`) and the homogeneity pass read 9 x vec4 of derivatives per pixel straight from storage buffers: with these two the frame was 4.3 s + 4.1 s of the 10.3 s.
2. *The green and colour-difference kernels ran loops with `colAt` (two modulo operations, a select and a shift) at every tap*: 4 directions x (6 + 49 positions) per pixel: green 2.0 s, chroma the largest single pass after the above.
3. *The read-back*: RGBA16F tile -> host float conversion on one thread, 0.5 - 7 s for 40 MP (cached host memory read at ~1 GB/s on this GPU, a single core), serial with the GPU work.

**What changed (arithmetic identical: same expressions in the same order; the fp32 GPU / CPU parity of the tests is unchanged, mean dE2000 1e-5).**

* `demosaic_xtrans.comp` MODE 2 / 5 (shared memory, any workgroup shape: a logical 16 x 16 tile is walked by the invocations of the group, group count `ceil(w / 16)`): the candidate colours (matrix + cube root) of a tile and its ring of 1 are computed **once** into shared memory and the derivatives are taken from there; the homogeneity selection, the (green, R - G, B - G) result and the 3 x 3 median are fused into one pass (derivatives and thresholds of a ring of 2 and the selection of a ring of 1 in shared memory), so the selection never goes through memory: 5 dispatches -> 4, the 32 B/pixel round trip of the selected differences is gone.
* MODE 0 / 1 read the scaled plane (and the green candidates) through shared memory and use **per-phase tables built on the host** (`demosaic_gpu.cpp`, in the `tab` buffer after the black table): for each of the 36 phases x 4 directions the nearest greens, the mask of the same-colour samples of the Laplacian term and the green flag (one word), and for each phase x direction x {R, B} a 49-bit mask of the taps of the colour-difference average (the host applies the same geometric rule, r2 <= 10, perpendicular distance <= 1.01, colour test). The kernels visit only the set bits (ascending, the order of the former 7 x 7 loop) instead of 49 positions with a colour computation each; there is no per-tap `colAt` any more. The direction loops are `[[unroll]]`.
* Read-back: two read-back buffers, the conversion of tile n runs on a thread (rows split over up to 4 threads, `__fp16` hardware conversion on arm64) while the device renders tile n + 1; joined before the buffer is reused and at the end. `GpuStats::passTimes` and `DAPPLE_GPU_PROFILE=1` in `dapple_demosaic_eval` give per-pass timestamps where the driver supports them.
* GLES: the shared-memory kernels cannot use `gl_WorkGroupSize` with specialised sizes (ESSL error found on the Redmi): the group size is the specialisation constant 19 (like `reduce.comp`), `gl_LocalInvocationIndex` as the linear id.
* Not changed on purpose: no fp16 planes and no coarser candidate set. The result is the exact tier, nothing was approximated, so no cheaper tier exists and none was needed to reach the numbers below; the picture needed no new look (parity to the CPU node is bit-level at fp32, see the table).

**Numbers (X-T50 DSCF0119, 7752 x 5178, tile 512, Vulkan, RGBA16F output).**

| | before | after |
|---|---|---|
| Redmi, GPU node, compute only | 10.3 s | **3.65 s** |
| Redmi, GPU node with read-back to host floats | 11.0 s (conversion 0.5 - 7.4 s, one thread, serial) | **3.72 s** (conversion overlapped, 44 ms visible) |
| Redmi, 1920 x 1080 region at 1:1 with read-back | 709 - 850 ms | **193 ms** (backend incl. probe-free: 300 ms) |
| Redmi, GLES (native driver), compute only / with read-back | not run | 4.7 s / 9.4 s (read-back path of the GLES HAL is slower, not touched) |
| Redmi, CPU node 4 / 8 threads | 10.3 / 10.5 s (7.1 - 10.4 s over repeated runs: thermal) | unchanged |
| Redmi, CPU node 1920 x 1080 region | 1.1 s | unchanged |
| Redmi, whole Full decode, file to linear RGB, as the app does it (decoder, mosaic path, shadow chroma, anchor) | CPU node 8.4 s | **5.5 s** with the GPU node, LibRaw (`DAPPLE_DEMOSAIC=off`) 11.5 - 13.3 s |
| Redmi, peak resident of that decode | 630 MB (CPU node), 859 MB (LibRaw) | 597 MB (GPU node); device planes 21 MB |
| Desktop RTX 4060 Ti, compute only (tile 512) | 113 ms | **59 ms** |
| Desktop, 1920 x 1080 region with read-back | 12 ms | **9.2 ms** |

The phone is noisy (the frame varied by +-40 % between runs with the same binary: GPU and CPU DVFS / thermal state after minutes of load; numbers above are the typical value of the last repeats; the pass-by-pass attribution is not reliable on this driver for the same reason). What is certain: before, GPU = CPU on this phone; after, GPU is 2.6 x the 8-thread CPU node on the frame and 3 x on the region.

**Apple M1 (not measurable in this turn).** The M1's 1585 ms had the same shape: ALU / dependent-load bound in the derivative, homogeneity and chroma kernels, not bandwidth (the planes are 68 B per window pixel but the 1585 ms is 40 ns per pixel). The same four causes apply to Metal (the colour candidates evaluated five times, nine storage-buffer reads per pixel in the selection, per-tap colour computation in loops with data-dependent trip counts), and they are removed in the single-source GLSL that the Metal backend translates. I expect a clear win, a factor of 3 - 6 on the frame (a few hundred ms), because the M1's threadgroup memory is large (32 KB, the kernels use 13 - 16 KB) and its ALU is not the limit; not measured, and the verdict below makes sure that if the M1 or any other device is not faster than its CPU node, the CPU node is used.

**Runtime choice by measurement** (`DeviceBackend::prefer`, `demosaic_gpu.cpp`; seam `GpuBackend::prefer` in `demosaic_backend.h`, called by `runNode` before the GPU run). On the first demosaic of the process (nothing at start-up: the node needs a pattern and matrices, which a file provides) the backend measures GPU against CPU on synthetic mosaics with the file's CFA pattern, black table, multipliers and matrices: a **frame probe** of 2048 x 1536 (GPU: one warm-up + best of 3, with the read-back to host floats the decoder does; CPU: all cores) and a **region probe** of 1024 x 576 (GPU best of 3, CPU best of 2). The faster one is used for work that covers at least half of the mosaic (frame) and for smaller rectangles (region), separately; a tie goes to the CPU. The verdict is cached in `<cacheDir>/demosaic_verdict_<hash>.txt` (the directory, app version and Build.FINGERPRINT of `gpu::configure`, new accessor `gpu::cacheConfig` in `gpu_runtime.h`; key = fingerprint + app version + API + `DeviceCaps::key()` + CFA + mode + hot filter), and in memory per process. No platform, vendor or device test anywhere. Log line (stderr, and Android logcat tag `dapple_demosaic`): `dapple_demosaic: verdict frame GPU, region GPU: frame GPU 301 ms vs CPU 628 ms, region GPU 72 ms vs CPU 199 ms` (`(cached)` when read from disk). `DAPPLE_DEMOSAIC_NODE=gpu|cpu` forces one (the `demosaic.wire_gpu` test forces gpu: it is about the GPU node). Redmi: GPU / GPU (301 vs 628 ms and 72 vs 199 ms); an earlier build of this round (serial read-back) measured 759 vs 605 ms and chose the CPU for the frame, i.e. the verdict does keep a GPU that loses to its CPU out of the decode (the probe is a 3 Mpx burst, so it extrapolates the 40 MP frame about 1.6 x optimistically for the GPU on a throttling phone: 5.7 s predicted, 3.9 s measured here after the fix, 9.4 s measured with the serial read-back; a tie-width margin was not added because it would be a guess).

**Gates.** `ctest -R demosaic.` on the desktop build (`build/demosaic-perf`): 21 / 21 pass, including `gpu_parity` (fp32 xtrans mean dE2000 0.00001, p99 0.00008; RGBA16F 0.016 / 0.061), `gpu_tiles`, `gpu_real` (X-T50 crop 0.00003 / 0.00005), `wire_gpu` (0.015 / 0.050 and 0.020 / 0.080), `wire_region`, `wire_anchor`, `wire_cache`, `wire_cancel` and the SwiftShader variants. Full check: see the report of the round.

**Not done.** Metal / M1 and iOS not run (see above). Not measured on the phone: the app itself (no installation allowed), a second phone. The GLES read-back (9.4 s on the Redmi) is slower than Vulkan's; it was not touched. Per-pass attribution on the phone is unreliable (driver timestamps), so the next step on mobile is bandwidth: fp16 planes were not tried (would change the arithmetic; keep the current result as the exact tier if they are). The probe could run at warm-up instead of on first use (it costs ~3 s of CPU + GPU once per install on the phone, cached afterwards). One shared GPU device with the executor, the 3-pass tier and the region of edits with geometry are as in 32.10.

**Files.** `core/engine/shaders/demosaic_xtrans.comp`, `core/engine/nodes/demosaic/{demosaic_gpu.cpp, demosaic_gpu.h, demosaic_gpu.cmake, demosaic_backend.h, demosaic_session.cpp}`, `core/engine/exec/{gpu_runtime.h, gpu_context.cpp}` (`cacheConfig`), `tests/engine/demosaic/{demosaic_eval.cpp, demosaic_tests.cpp}` (`--decode-only`, `--backend`, `DEMO_QUICK`, `DAPPLE_GPU_PROFILE` passes, `DAPPLE_VERDICT_DIR`; `wire_gpu` forces the GPU node), this section.

### 32.12 Half-resolution X-Trans from the cached mosaic (2x view, 2026-11-13)

**Problem.** The preview-quality open of a mosaic RAW block-reduces the cached mosaic (X-Trans 3 x 3: 2584 px for 40 MP). The 2x view (long edge 4096) of an X-Trans file therefore fell back to LibRaw's half-size decode (1.9 - 2.4 s on the Redmi) because the 3 x 3 picture was softer (gradient energy 0.64 - 0.88 of LibRaw's). Bayer takes 0.2 s from the cache (2 x 2 blocks are already half size).

**Method (`Desc::half`, `demosaic_half.inc`, `demosaic_half.comp`; roi and output in half-resolution pixels).** Output pixel (x, y) is the sensor area 2x..2x+1, 2y..2y+1 (3876 x 2589 for the 40 MP X-T50; crop and orientation as the other paths, same black level, white balance, matrix, clamp and exposure anchor, so nothing shifts when a 1:1 region replaces it).
1. Each output pixel is a fixed linear combination of the 8 x 8 scaled samples 2x-3..2x+4 (symmetric around the output pixel). 2x mod 6 is 0, 2 or 4, so the 6 x 6 pattern gives **9 phases** (output pixel mod 3); the 9 x 3 x 64 weights are designed at run time, once per pattern (cached), by least squares against a target response on a frequency grid of the whole plane (real part = target, imaginary part = 0 so no phase shift, sum of weights = 1, ridge against noise gain). Not a demosaic then decimate: no full-resolution picture exists.
2. Green Gh from the green taps only (the dense channel): flat to 0.25 cycles / sensor pixel (the output Nyquist), rolling to 0 at 0.5, with a gain peaking to 1.6 at the output Nyquist. Colour differences at low bandwidth (flat to 0.08, zero at 0.22): G = Gh, R = Gh + (Rl - Gl), B = Gh + (Bl - Gl) (Rl, Gl, Bl: the same low-pass fitted on each colour's own taps, so a grey scene gives no chroma).
3. Overshoot clamp: the peaked green may exceed the min / max of the central 4 x 4 green samples by 15 % of their range; the same shift goes to R and B (no ringing at steps, colour differences untouched).
4. Clip awareness: when any tap with weight is clipped (scaled value >= 0.995) the pixel switches to the fallback: each colour is the gaussian-weighted mean of its unclipped taps (all of them when less than 15 % of the weight is unclipped); no colour is mixed from a clipped sample.
5. The CPU node uses 8-lane accumulators (a single accumulator is a latency-bound chain) and no division in the window build. The GPU kernel (single-source GLSL: fixed trip counts, explicit selects, tables in the table buffer; Vulkan, GLES 3.1 and the Metal translation share the text) reads the same tables. The verdict (`DeviceBackend::prefer`) has its own kind for the half path (key suffix `half1`, probe of 2048 x 1536 samples) and requires the GPU to win by 1.3x (a tie goes to the CPU: no device work, no read-back); on the Redmi it chose the CPU (probe GPU 93 ms vs CPU 104 ms, but the whole frame GPU 2.3 s vs CPU 0.5 s, so the margin matters).

**Why not "run the candidate demosaic and decimate".** It costs a full-resolution frame (3.7 s on the Redmi, 0.5 s per 40 MP on the desktop CPU) to throw three quarters of it away; the direct filter is about 7x cheaper on the phone. Not built beyond the cost estimate.

**Design finding: the sharpness proxy and a box-reference PSNR pull in opposite directions.** LibRaw's half-size picture of these files is a nearly alias-full decimation (visible 1 px coloured speckle in foliage). Its gradient energy (the `wire_preview` proxy) counts that speckle and the noise, so an anti-aliased, box-like reduction scores 0.62 - 0.76 on it whatever its edges look like, and a design that reaches >= 1.0 on every file needs a response peaked toward the output Nyquist. The zone-plate PSNR against the ideal reduction (2 x 2 box mean of the scene) rewards matching the soft box response: the node box-averaged gets 22.3 dB by construction, a box-matched half filter 20.4 dB at gradient 0.62 - 0.76, the chosen design 14.5 dB at gradient 1.07 - 1.12. The sharpness gate was kept (it is the user-visible complaint); the luminance PSNR is a regression guard (>= 12 dB) and the colour (moire / false colour) PSNR is compared with the box-averaged node (17.0 dB against 16.0 dB: better).

**Quality (X-T50 / X-S10 against LibRaw's half-size picture; `dapple_half_eval`, `demosaic.half_real`)**

| file | size | gradient-energy ratio ours / LibRaw (>= 1.0) | same on the 3 x 3 blurred luminance | low-frequency dE2000 (4 x 4 block means) mean | full-resolution dE2000 mean |
|---|---|---|---|---|---|
| X-T50 DSCF0118 | 3876 x 2589 | 1.12 | 0.98 | 1.65 | 6.7 |
| X-T50 DSCF0119 | 3876 x 2589 | 1.12 | 0.98 | 1.46 | 5.9 |
| X-T50 DSCF0120 | 3876 x 2589 | 1.11 | 0.98 | 1.52 | n/a |
| X-S10 compressed | 3123 x 2085 | 1.07 | 0.92 | 0.50 | 2.0 |
| X-S10 lossless | 3123 x 2085 | 1.07 | 0.92 | 0.51 | n/a |

The full-resolution dE2000 against LibRaw (about 6 on the X-T50, a dark, noisy, high-gain foliage scene at a metric gain of ~6) is dominated by LibRaw's own speckle and noise; the low-frequency number (noise averaged out) is the colour agreement: 0.5 (X-S10), 1.5 - 1.65 (X-T50). Test gates: ratio >= 1.0 and low-frequency dE2000 <= 2.0. The ratio on the blurred luminance (noise and 1 px speckle suppressed) is 0.92 - 0.98: LibRaw's structure sharpness at the scale of real edges is matched to within a few percent, not exceeded; the gate proxy is met through the Nyquist peaking. X-A7 (a Bayer body) stays on the 2 x 2 reduction. Side-by-side crops (LibRaw half | ours | difference x 4, shown at 2x): `build/xhalf/crops/`. Looking at them: edges are as crisp as LibRaw's, foliage is cleaner (LibRaw's coloured 1 px specks in the sky gaps are absent), a clipped window slit is a clean white streak without coloured fringes or specks, no halos; the low-frequency tint matches.

**Timing.** Redmi 2312DRA50C (6 cores, Vulkan; `dapple_stage_bench` through the C API, X-T50 DSCF0119, three alternating repeats; `DAPPLE_XTRANS_HALF=off` is the previous behaviour):

| stage | before | after |
|---|---|---|
| preview open (2048, block reduction, cold unpack) | 1.59 - 1.64 s | 1.65 - 1.69 s (same path, noise) |
| 2x view (preview 4096) | **2.26 - 2.41 s** (LibRaw half-size) | **0.53 - 0.70 s** (half node 0.36 s of it, the rest shadow-chroma floor and resize) |
| first 1:1 region | 3.6 - 3.8 s | 3.6 - 3.85 s (unchanged; the one-off verdict probe is in it) |
| pan | 0.65 - 0.67 s | 0.65 - 0.73 s |
| export (full open) | 3.8 s | 3.8 - 4.1 s |

Desktop (RTX 4060 Ti + CPU, 40 MP): 2x view 438 - 578 ms -> 133 - 153 ms (CPU node). Redmi GPU node for the half frame: 2.3 s (read-back and window upload bound), so the CPU wins there and the verdict says CPU. Bayer (Sony ILCE-7M4 ARW on the Redmi): open 971 / 975 ms, 4096 285 / 287 ms (off / on: unchanged). 2048 preview: the half path costs more than the block reduction (355 vs 279 ms on the desktop) with no quality gain at that size, so the 2048 request stays block-reduced.

**Wiring.** `decodePreviewFromMosaic` (libraw_mosaic.inc): X-Trans and `wantLongEdge > 1.25 x (width / 3)` -> `demosaic::halfOriented(m)` (demosaic_session) instead of declining to LibRaw; `DAPPLE_XTRANS_HALF=off` keeps LibRaw's half-size for that request, `=all` uses the half path for every preview size (A/B). Outside this change's files: `dapple_session_open` installs the GPU backend only for Full opens, so the half node of a preview open runs on the CPU node (on the Redmi that is what the verdict wants; on a phone where the GPU wins the 2x view would not use it): `if (opts.quality == DecodeQuality::Full || long_edge > 2048)` at c_api.cpp line ~782 fixes it (the backend creates its device lazily, so a Bayer preview does not pay for it).

**Tests (`ctest -R demosaic.`, 30 / 30).** `half_zone` (chroma PSNR >= node box-averaged - 1 dB, luminance >= 12 dB guard), `half_tiles` (30 random rectangles and 4 threads equal the whole frame bit for bit), `half_flatclip` (flat field exact to 0.001; clipped blob interior within 0.03 of the data colour), `half_real` (the table above, gates 1.0 and 2.0), `half_gpu` (+ `_swiftshader`, + `_gles_angle_*` where ANGLE exists: fp32 parity with the CPU node mean dE2000 1e-5, max abs 2e-7, with a clipped block; RGBA16F 0.017; 128 px vs 48 px tiles identical; region identical; the full-resolution path still works afterwards; also run on the Redmi, Vulkan and GLES). Golden selfcheck unchanged.

**Not done.** The GPU half node is correct but slower than the CPU node on the Redmi (read-back / upload bound; an in-GPU hand-off to the engine as the film path does would remove the read-back, not attempted). No edge-adaptive weighting (the overshoot clamp gives the crisp edges). A DNG crop on an X-Trans file uses crop / 2 (RAF has none, untested). Metal not run. The ANGLE ctest variants of `half_gpu` are registered but were not run here (no GLES desktop build dir of my own).

**Files.** `core/engine/nodes/demosaic/{demosaic.h, demosaic_half.inc, demosaic_cpu.cpp, demosaic_session.{h,cpp}, demosaic_gpu.{cpp,cmake}}`, `core/engine/shaders/demosaic_half.comp`, `core/src/decoders/libraw_mosaic.inc`, `core/include/dapple/{decoder.h, mosaic.h}` (comments), `tests/engine/demosaic/{demosaic_tests.cpp, half_eval.cpp, demosaic.cmake}`, this section.

## 33. Single-image HDR: `hdr` 0..100, node `Hdr` (imaging engineer, 2026-11-13; round 2 = scene adaptive)

From one RAW, the look of an exposure-bracket merge on an SDR screen: the large-scale contrast of the scene is compressed where the normal rendering cannot show it, local contrast is kept, shadows come up above black, highlights are pulled with a shoulder, no halos. UI name "单张 HDR" (光 category, last spoke of the radar). Unrelated to the multi-file merge (`core/src/hdr.cpp`) and to a later gain-map Ultra HDR export.

**Curve (round 2, five lines).**
1. Base = bilateral smoothing of log luminance in a (space x brightness) grid built from a 512 px proxy (`core/src/hdr_field.{h,cpp}`; ideas: Paris & Durand 2006, Chen et al. 2007, Durand & Dorsey 2002; one spatial scale of 3.5 cells = 5 % of the long edge, range sigma 0.75 EV). Everything finer is detail and is never compressed. Cached per source + white balance; exposure and strength drags do not rebuild it.
2. **Scene adaptive:** R = p99 - p1 of the base EV (stored in the field). T = the EV span around mid-grey where the encoded output of a neutral keeps >= 60 % of its peak slope per EV, read from THIS frame's tone table (`Renderer::hdrBind`, `hdrShownRange`; 4.2 EV for the default curve, it follows contrast / highlights / shadows edits). Only the excess E = max(0, R - T) is compressed, with a soft onset E^2 / (E + 2). R <= T: the node is not in the graph (bit identical). Derivation of 60 %: below it the curve is in its toe or shoulder and a step of one EV shows less than 0.6 of the mid-tone step, which is what looks flat; 30 % (first try) gave T = 7.2 EV and changed no real photo.
3. **Fixed ends:** black (p1) and mid-grey (the scene key: frame log mean, moved by the anchor EV, clamped to mid-grey -3 / +1 EV) do not move. Shadows between them are lifted by a bump `ls * 6.75 t (1-t)^2` (t from black to key; zero slope change at the key; peak a third of the way up), highlights above the key are pulled by a shoulder `hp * u^2` up to p99 and held beyond. Peaks: ls = s min(0.25 (key - p1), 0.45 E'), hp = s min(0.30 (p99 - key), 0.45 E'), so the base map keeps a slope >= 0.4 (never inverts). Clipped highlights (within 0.4 EV of the clipped value after the gain) are not darkened; the lift fades out between the ISO noise floor (0.002 sqrt(ISO / 100) x exposure) and 8x that.
4. **Contrast:** the detail (pixel EV - base EV, clamped to +-1.5 EV) gets half of the gain that would restore the base-map slope (cap +35 %), faded out toward the noise floor. Structures finer than the base are not compressed at all, so the mid band does not drop.
5. **Colour:** all three channels are multiplied by the gain, then the chroma is corrected in a cube-root space: a darkening keeps 60 % of the colourfulness it would lose (`g^((1-0.6)/3)` on the chroma), a lift is pulled toward grey near the noise floor (`smoothstep(0.5 yn, 12 yn, Y)` weighted by the lift); luminance is renormalised exactly.

**Range remap (decision after round 2).** The user's `hdr` 100 is the internal strength 0.65 (`kHdrUserMax` in `Renderer::hdrBind`, the one place where the parameter is resolved); the stored value stays 0..100, the UI is unchanged. Reason: the internal 100 greys foliage and darkens shadows on the Z8 and A7R IV sheets. All numbers below that name strength 100 are internal strength (user 154); the golden frames and sheets3 use the remapped scale.

**Integration.** `EditParams::hdr` (`paramFields`: "hdr", 0..100, group light; JSON key `hdr`, clamped). `NodeType::Hdr` (point, after `Gain`, before `ToneColour`; deps `hdr`, `ctx.source`), only in the graph when `FramePlan::hdr.active()` (strength > 0, scene-referred, no film stock, and R > T). `Renderer::hdrBind(source, FramePlan::Hdr&, tone)` resolves it for every path (CPU `FrameKernels` themselves, the three GPU entry points of `EngineSession` after the bake). CPU: block fragment `FrameKernels::hdrBlock`, fused into `headGainTone`. GPU: `shaders/point.comp` spec constant `HDR` (id 1), 7th binding (SSBO of the field, binding string `SBSSIIB` in `core/gpu/CMakeLists.txt`), uniforms `h0..h3`, device copy `DeviceHdr` in the executor's table cache; single-source GLSL (Vulkan, GLES 3.1, Metal from one file; GLES / Metal not run here). The tonal analysis is taken with hdr = 0. Interactive tier: field cached per source + white balance, the frame pays the per-pixel gain only. `DAPPLE_ENGINE=off` / old Renderer: ignored. Film frames and JPEG / display-referred sources: ignored.

**Tests** (`dapple_hdr_tests`, ctest `engine.hdr_*`). `identity` (hdr 0 byte-identical, old Renderer ignores 100, JSON clamp / round trip). `lowrange` (a 1.4 EV overcast scene at hdr 100: node not in the graph, dE2000 0.000; a 6.2 EV scene (excess 2 EV): active, mean dE2000 1.5). `anchors` (gain(black) = 0 and nothing below it, gain(key) = 0 with slope 1, lift above black, pull at the top, slope >= 0.44 everywhere; end to end the darkest 1 % of the output is the same code at 0 and 100). `midband` (sigma 1.5 - 6 px band of log luminance, away from the scene's edges: energy ratio 1.05 at 50, 1.20 at 100, limit >= 1.0). `tiles` (tile 96 / 200 vs whole: max 1 code in at most 1 byte). `monotonic` (8 EV scene: p30 0.060 -> 0.068 -> 0.079 -> 0.090 -> 0.102 and p95 0.839 -> 0.807 -> 0.772 -> 0.736 -> 0.700 at 0 / 25 / 50 / 75 / 100, p5 within 0.012 of black, clipped core within 0.02). `halo` (6.8 EV and 4 EV steps with 10 % texture: <= 0.015 EV from the plateau; the 1.3 EV step is inside the range sigma and is informational). `noise` (dark patch next to a bright area, ISO 100: noise gain 1.22x, ISO 1600: no lift). `parity` (GPU vs CPU executor, 5 edits x 2 sizes: mean dE2000 <= 0.049, p99 <= 1.01). Golden `hdr/hdr_50`, `hdr/hdr_100_interactive`, `hdr/hdr_100_exact_clarity` (re-recorded in round 2; the 148 older frames unchanged). `dapple_hdr_tests rank <files>` lists the base range of RAW files; `synthsheet <dir>` writes the 12 EV synthetic scene.

**Cost** (RTX 4060 Ti, Vulkan; round 1 numbers, the per-pixel code changed little: one more `pow` set for the chroma): about 0.02 ms at 1080 x 720 and 0.3 - 0.5 ms at 24 MP; CPU +15 ms per 2.4 MP; field build 11 ms once per source + white balance. Not re-measured in round 2 and not measured on a phone.

**What the pictures say (round 2 sheets, `build/shdr/sheets2/`).** The public RAWs in `build/rawsamples` are not high-range scenes: ranked by the base-layer range (p1..p99), the top is 7.9 EV (Sony A7R IV), Nikon Z8 7.5, Canon R6 II 7.4, OnePlus A6003 6.6, A7 V 5.6; nothing reaches 9 EV. At 50 the effect is modest and mostly positive (clearest on the R6 II: clouds get depth, shadows open, local contrast intact); at 100 it is not safe on every picture (Z8: deep hedge shadows get darker and dirtier, a faint blotch in the sky gradient; A7R IV: foliage greys, chroma speckle at tree / sky edges). The 12 EV synthetic scene shows the limit of the approach: its interior sits under the noise floor, so it stays black (the guard refuses to lift noise) and only the window is pulled down. One frame cannot recover what a bracket would.

**Files.** `core/src/hdr_field.{h,cpp}`, `core/src/{renderer.cpp, tone.cpp, frame_kernels.h, edit_params.cpp}`, `core/include/dapple/{edit_params.h, frame_plan.h, renderer.h}`, `core/engine/graph/{types.h, registry.cpp, builder.cpp}`, `core/engine/plan/planner.cpp`, `core/engine/exec/{cpu_executor.cpp, gpu_executor.cpp, engine_session.cpp}`, `core/engine/nodes/{main/cpu_nodes.cpp, gpu_nodes.cpp, gpu_common.h, point/gpu.cpp}`, `core/engine/shaders/point.comp`, `core/gpu/CMakeLists.txt`, `tests/engine/hdr_tests.cpp`, `tests/engine/golden/{golden_tests.cpp, corpus.txt}`, `tests/{CMakeLists.txt, core_tests.cpp}`, app: `lib/model/{params.dart, pad_model.dart}`, `test/model/hdr_param_test.dart`, `test/params_test.dart`.

## 34. Ultra HDR export: gain-map JPEG from one RAW (imaging engineer, 2026-11-13)

Option on the still export: the same JPEG as always (the SDR rendering) plus a gain map, so that an HDR screen shows the highlights brighter than white. Files: `core/include/dapple/ultrahdr.h`, `core/src/ultrahdr.cpp` (container, gain map, own grayscale JPEG encoder; no library), `core/src/c_api.cpp` (`dapple_session_export_jpeg_hdr`, `dapple_session_hdr_headroom`), `encodeJpeg` in `core/src/encode.cpp`, tests `tests/uhdr_tests.cpp` (ctest `uhdr.*`), app `publish_page.dart` / `app_state.dart` / `dapple_session.dart`.

**HDR rendition and headroom rule.** The HDR rendition is the video policy of 30.8 applied to a still: HDR linear = linear(SDR result) + the part of the scene above SDR white, in the colour of the scene. The scene luminance in SDR-white units comes from the *bright rendition*: the same edit rendered with the exposure lowered by H stops (nothing else changes), multiplied back by 2^H. At lower exposure the base shoulder (`shoulder()`: identity below 0.3, exponential roll-off to 1) no longer squeezes the highlights, so `Yb = luma(bright) * 2^H` is the unshouldered scene luminance relative to SDR white (1 = SDR white; diffuse white and below stay under 1). Excess = `c * smoothstep(0, 1, c)` with `c = max(0, Yb - 1)` (onset between 1 and 2 times SDR white, then linear), added per channel in the colour direction of the bright rendition. Where `Yb <= 1` the HDR rendition is exactly the SDR picture (mid-tones, shadows, skin: identical by construction), SDR white stays at 1.0. The pipeline stays untouched (golden corpus unchanged): only an extra render with another exposure is needed, so every edit, GPU or CPU path, works the same. Costs: the SDR export as before + 2 or 3 renders at a quarter of the long edge (CPU engine below 1500 px; not timed at 24 MP: the whole `uhdr.session` test, a 1600 px preview session with seven exports and probes, runs in about 1 s on the desktop).
* **H (stops above SDR white).** Default from the scene: probe the bright rendition at the cap H = 3 (cut to what the exposure clamp leaves: `exposure + 5`), take its 99.9th percentile of luminance times 2^H = peak (SDR white units); `H = clamp(ceil(2 log2 peak) / 2, 0.5, 3)`. A caller may give H (0.5 .. 3). The 99.9th percentile keeps a few specular pixels from deciding; points beyond H are rolled off by the shoulder of the bright render itself.
* **Worth encoding:** peak >= 1.5 (0.58 stop above SDR white). Otherwise the call returns `written = 0, reason = 1` and writes the plain JPEG (the same bytes as the SDR export): this is the decision for a scene without highlights above white, a display-referred (JPEG) source, or a dark edit. The app hides the option in that case (`dapple_session_hdr_headroom`, one preview-quality session and one 384 px render; not timed on a phone).
* **Film: no headroom.** A film stock is a print: paper white is the top, the scan has no speculars brighter than the paper. `reason = 2`, plain JPEG, option hidden. (Digital edits keep it, film does not.)
* Limits of the exposure-bracket rendition: tone curves / contrast in the encoded domain see the darker bright rendition and act on it differently (a strong S curve gives less gain); the exposure clamp (-5) limits H for edits with negative exposure; the auto-tone analysis is repeated at the lower exposure.

**Gain map.** Single channel (luminance, Rec.709 weights, linear light), computed at a quarter of the base long edge from two quarter-size renders of the engine (SDR, bright) so that both have the same filter: `G = log2((Yh + o) / (Ys + o))`, o = 1/64 (OffsetSDR = OffsetHDR), >= 0. Edge guard: a pixel that has a neighbour darker than 0.6 of its SDR luminance is limited to that neighbour's gain, but never below half its own (stops): the bilinear up-scale then lights the dark side of an edge by at most a quarter of the highlight's gain (a plain 3x3 minimum killed glints and cost 0.3 stop on sky gradients). Metadata: `GainMapMin = 0`, `GainMapMax = ceil(max G * 1000) / 1000`, `Gamma = 1`, `OffsetSDR = OffsetHDR = 0.015625`, `HDRCapacityMin = 0`, `HDRCapacityMax = GainMapMax`, `BaseRenditionIsHDR = False`; 8 bit, quality 92 grayscale baseline JPEG (one component; own encoder with the Annex K tables).
**Container.** Primary: SOI, the base JPEG's own leading APP0, then APP1 XMP (Container directory: Primary, GainMap with `Item:Length`; `hdrgm:Version`), APP2 ISO 21496-1 (version bytes only), APP2 MPF (2 entries: primary type 0x030000 and size, gain map size and offset relative to the MPF header), the rest of the base stream untouched, EOI, then the gain-map JPEG (APP1 XMP with the `hdrgm` fields, APP2 `urn:iso:std:iso:ts:21496:-1` block: version 0, flags 0x40, base headroom 0, alternate headroom = capacity max, min / max / gamma / offsets as n/d with d = 10^6). **The base is today's SDR export: removing the added segments gives back the stream of `dapple_session_export_jpeg` byte for byte** (test `uhdr.session`). `parse()` reads the file back; `motion_photo.cpp` already accepts such a file as a cover (Motion Photo + Ultra HDR is possible, not wired in the app).

**Measured.** Synthetic scene (`uhdr.reconstruct`, gain map from a quarter-size box-filtered pair): reconstruction `(SDR + o) * 2^gain - o` against the HDR rendition away from a hard edge: mean 0.010 stop, worst 0.076 stop; within 8 px of a hard bright/dark edge the worst is 1.7 stops (the guard); no dark pixel lit. Real RAW (Canon R6 II, exposure 0, 1200 px, `uhdr.session`): vs the full-size HDR rendition at 8 x 8 scale, rms 0.055 stop, worst 0.58 stop, 92 % of the extra light carried (glints of one or two pixels do not survive a quarter-size map). Bounds enforced: rms < 0.15, worst < 0.6, energy 0.8 .. 1.05. Gain-map JPEG round trip: <= 11 levels of 255 (bound 16; ringing at hard edges). Metadata of the public samples and evidence: `build/uhdr/` (`metadata.txt`, `*_uhdr.jpg`, `*_gainmap.png`, `*_sdr_vs_hdr.png`). The public RAWs have little highlight headroom (p99.9 peak 0.5 - 2.3 times SDR white; 7 of 36 reach 1.5).
**Android.** Emulator `dapple_n5`, Android 15 (API 35, google_apis x86_64): `BitmapFactory.decodeFile` and `ImageDecoder` give `Bitmap.hasGainmap() == true` for the three exported files, `Gainmap.getRatioMin() = 1`, `getRatioMax() = 2^GainMapMax` (e.g. 2.3999 for GainMapMax 1.263), gamma 1, epsilon 1/64, `displayRatioForFullHdr` = ratioMax, contents 600 x 401 hardware bitmap; the plain JPEG has no gain map (`build/uhdr/emulator_api35.txt`; check = a 40-line `UhdrCheck` class run with `app_process`). Whether the platform reads the XMP or the ISO block was not separated (both carry the same numbers).
**Not done.** iOS / Apple Photos check (no Mac, no iPhone run); HEIC / AVIF gain maps; 10-bit; three-channel gain map (colour of the excess follows the bright rendition through one luminance gain, error not measured on saturated highlights); Motion Photo with an Ultra HDR cover in the app; the option is hidden for Motion Photos; GPU build of the new C API code only compiled with `DAPPLE_GPU=OFF` here (check-all builds it with the GPU).

## 35. Display-referred sources with their own colour space and depth (imaging engineer, 2026-11-13)

**Problem.** The JPEG / PNG reader treated everything as 8-bit sRGB, and the platform plugins converted HEIC / HEIF / PNG (and, on iOS, wide-gamut JPEG) to an 8-bit sRGB JPEG first: iPhone photos (Display P3, 10-bit HEIC) lost gamut and depth before the engine saw them.

**Interchange container: 16-bit RGB PNG.** The decoder already read PNG through stb (`stbi_load_16`), so the only new engine code is the tag reader (the PNG chunks cICP / iCCP / sRGB / gAMA / cHRM / eXIf, the JPEG APP2 ICC_PROFILE segments) and the colour maths; ImageIO writes 16-bit PNG natively and on Android a streaming PNG writer is 100 lines (`Png16Writer.kt`, `java.util.zip`). No new dependency, no private container, and the file is an ordinary PNG any tool opens.

**Identification order** (`core/src/colour_space.cpp`, `resolveSourceSpace`): cICP (primaries 1 sRGB, 9 Rec.2020, 11 DCI-P3, 12 Display P3; transfer 13 sRGB, 1/6/14/15 BT.709, 8 linear, 4 gamma 2.2, 5 gamma 2.8; matrix must be 0) -> iCCP / JPEG ICC profile (matrix + TRC RGB profiles: rXYZ / gXYZ / bXYZ and `curv` / `para` curves parsed by a small parser, no CMS; the colorants are D50 so the 3x3 goes straight to XYZ D50 -> ProPhoto D50) -> sRGB chunk -> cHRM + gAMA (primaries from the chunk, Bradford from their white to D50; gAMA 0.45455 beside sRGB primaries is the usual fallback pair = sRGB, beside other primaries a pure 2.2 gamma) -> no tags = sRGB. Named in the log: sRGB, Display P3, DCI-P3, Rec.2020, Adobe RGB (1998); any other matrix profile is honoured as "custom RGB profile". LUT / gray / CMYK profiles, PQ / HLG transfers, unknown cICP codes: sRGB with one log line (`dapple: unknown colour space (reason), read as sRGB`); every non-sRGB space logs `dapple: source colour space Display P3 (cICP, sRGB curve)` once per file (stderr, and logcat tag `dapple_engine` on Android).

**Bit-identity.** A profile / tag set that is sRGB (matrix within 0.004 of sRGB D50 and a curve within 0.0015 of the sRGB curve) resolves to the historic path exactly (`srgbToProPhoto()` and the 256-entry `srgbDecode` table): 8-bit sRGB JPEG / PNG, with or without sRGB profile / chunks, decode bit for bit as before (`tests/wide_gamut_tests.cpp`, golden unchanged). 16-bit samples use a 65536-entry table per channel.

**Platform side.** iOS (`DeepImage.swift`): everything that is not RAW / JPEG / PNG (HEIC, HEIF, TIFF ...) is drawn strip by strip (about 6 MP) into a 16-bit context in the source's own colour space and streamed into the PNG (cICP for sRGB / Display P3, else the ICC profile as iCCP; HDR / extended / linear spaces are drawn in Display P3); orientation, capture date, camera, GPS go into an eXIf chunk (the pixels stay in sensor orientation, the engine applies the tag like for a JPEG). JPEG and PNG pass through untouched. Peak = ImageIO's own decode + about 90 MB, independent of size. Android 9+ (`ImageTranscode.kt`): HEIC / HEIF / WebP through `BitmapRegionDecoder` in 6 MP strips with RGBA_F16 asked for (peak about 110 MB for any size), fallback `ImageDecoder` on the whole picture (peak = bitmap: 192 MB for 48 MP 8-bit, 384 MB F16, plus about 60 MB); the colour space comes from the bitmap (sRGB, Display P3, Rec.2020, Adobe RGB; anything else is re-decoded as Display P3). Below API 28 the old 8-bit sRGB JPEG path stays. PNG files are no longer transcoded on Android (the engine reads them). Dart: `MediaBridge.toPng16`.

**Export today (not changed).** The working space is mapped to sRGB in `Renderer::colour` (`core/src/renderer.cpp:521` ProPhoto -> linear sRGB; `:607-618` out-of-gamut values pulled towards the same-luminance grey, not hard-clipped; `:621` sRGB OETF), then `encodeJpeg` writes an 8-bit JPEG with no ICC profile and no colour tag (`core/src/encode.cpp:18, :35`); the Ultra HDR base image is the same untagged sRGB JPEG with Rec.709 luminance gain-map maths (`core/src/ultrahdr.cpp:14`). So P3 sources are gamut-compressed to sRGB on export, never exported as P3. Measured (desktop CLI, synthetic P3 chart): a saturated P3 red exports as (255, 38, 55) versus (254, 0, 0) for the old sRGB-clipped route.

**Display P3 export (export option `"colorSpace":"p3"`, preview stays sRGB).** `EditParams::outputSpace` (internal, 0 sRGB / 1 P3; set by the C layer, part of every colour cache key via `ctx.outSpace`). `Renderer::colour` (`core/src/renderer.cpp`, `colourP3`) takes the colour stage's linear sRGB, converts to linear P3, applies the same pull towards the same-luminance grey into the P3 cube (modern chroma compression: `fits` tests the P3 cube), then the curves / look LUT / fade run on extended sRGB-encoded values (so a colour inside sRGB gets the sRGB path's value) and the result is encoded with the P3 matrix + sRGB transfer; the lattice therefore holds P3-encoded values and the GPU shaders need nothing but the vignette (`point.comp`, `geoC.w`, sRGB-space vignette; CPU twin `applyVignette`). Film frames are authored for sRGB: the C layer renders them as sRGB and converts the 8-bit frame (`srgb8ToDisplayP3`), they never add colour. Spatial stages (sharpen, NR, clarity, grain) run on the P3-encoded values. Ultra HDR: luma weights of P3 (`uhdr::*` `p3` flag), base = the P3 SDR export. `dapple_session_wide_gamut_fraction` feeds the publish page option. Measured: `tests/p3_export_tests.cpp` (point-wise pipeline inside sRGB: dE76 0.0000 on the chart and on real RAWs; spatial stages mean 0.05 .. 0.37 on RAWs).

**Out of scope.** HDR gain maps in iPhone HEIC (and PQ / HLG HEIC) are still dropped: the SDR base image is used. Alpha is dropped (iOS / Android conversion flattens to white; a PNG with alpha read directly by the engine ignores it). Wide-gamut previews on screen are shown in sRGB like the export.

**Evidence.** Android emulator (API 35, x86_64): a P3 JPEG with ICC -> `source colour space Display P3 (ICC, sRGB curve)`; an 8-bit P3 HEIC -> `DappleIngest: deep wc_p3_8.heic: Display P3, RGBA_F16, 480x320, region strips` then `source colour space Display P3 (cICP, sRGB curve)` (the emulator cannot decode 10-bit HEVC HEIC: "Image format not supported"). iOS Simulator (iPhone 16): a 10-bit P3 HEIC -> 16-bit PNG with cICP 12/13 + eXIf (14 KB), engine log `Display P3 (cICP ...)`; P3 PNG `(cICP)`, P3 JPEG `(ICC)`. Exports of those compare to the sRGB-clipped route with the saturated patches differing by up to 54-78 levels.

## 36. Interactive film: stock sliding and pad drags on the A11 (frame trace, 2026-11-13)

Owner report (iPhone X, A11, 2 performance cores, Metal): sliding across the film map is janky, dragging the film pads makes the picture flicker. Approved plan: diagnose, then pre-baked per-stock tables, waypoint tables interpolated on the GPU for pad drags, one tier per drag, baking yields to rendering. **The trace contradicted the waypoint part of the plan; what it showed was fixed instead.**

### 36.1 The frame trace (permanent, off by default)

`core/engine/nodes/film/frame_trace.h`, `dapple_frame_trace_enable / _dump / _clear` (C API). One TSV row per frame of `dapple_session_render_frame` (touch-hint frames excluded: they are never shown): time, view sequence number, render ms, ms spent in table bakes / match on the frame thread, long edge, interactive flag, stock, bakes on the frame thread, **print table kind** (exact / coarse of its own values / stale = the previous step of the same stock / fallback-from-other-stock / lagging = the bound GPU table is older than the one wanted), table generation (creation order, `PayloadBase::gen`) wanted and bound, LUT size, match kind (hit / interpolated / previous result / computed), background table bakes and match / calibration workers running (`ftrace::Busy` counters), print EV, warmth, tint, black. Cost when off: one relaxed atomic load per hook. On the phones the autotest (`app/lib/platform/ios_frame_trace.dart`, `autotest.json` key `trace`) drives a real `RenderLoop` (view with the null presenter: frames are rendered and read back, nothing is drawn; timer grid 60 Hz) with latest-wins snapshots, the `AdaptiveQuality` controller fed by the frame events, interaction + touch hint, a settled frame on release; scripts `scripts/ios/pc-build-ipa.sh`, `run-trace-pair.sh`, `pull-traces.sh`, `analyze-trace.py`, `trace-summary.py`. Scenario A: 8 stocks at 150 ms each, first visits (A1) then the same stocks again (A2). Scenario B: a 2 s diagonal at 60 Hz input on the exposure pad (film_shoot + film_print) and on the texture pad (film_wb + film_black), two stocks. Photo: public a7m5.ARW sample. Limit: the null presenter does not exercise the Swift texture ring.

### 36.2 Diagnosis (iPhone X, "before" = tree at HEAD plus the trace)

* **Jank = CPU oversubscription, not table cost.** Every first visit of a stock started `prewarmFilmCalibration`: 2 workers x 15 print-calibration buckets at normal priority, plus a medians-seed thread and the async match thread. A slide over 8 stocks left **16 - 20 compute threads** running (trace column `bgMatch`: 5 -> 19) on 2 performance cores for minutes. Frame render times during A1: 150, 32, 114, 23, 36, 87, 13, 22 ms; the settled frame after the slide took 2.7 s (tables 650 ms, match 386 ms), after a pad drag 4.4 s. A pad drag frame that missed every cache baked the first coarse table of a stock **on the frame thread: 984 ms** (one 625 ms frame in the middle of a drag). With the warm-up switched off (`DAPPLE_FILM_PREWARM_OFF=1 DAPPLE_FILM_SEED_OFF=1`) the same drags run at 7 - 12 ms per frame, one table landing per frame: a 17^3 coarse table is a **2.7 ms** bake, the raw table 12.4 ms, curves 0.7 ms (prebake counters, A11, idle). So baking is cheap; the waypoint scheme would have optimised a cost that is not there.
* **Stock sliding showed the previous stock.** 8 of 8 frames of A1 and 6 of 8 of A2 were `fallbackStock` frames (the old fallback keeps the previous stock's look until the exact 33^3 tables of the new stock land, and the single latest-wins `StockWorker` skips every intermediate stock, so with 150 ms per stock only the last one ever landed); their print EV was the previous stock's. An exact 33^3 table is not what an interactive frame should wait for: `coarseFilm` was only set when a slider's `active_param` was passed, so a stock tap was not coarse.
* **Pad drag = slideshow plus jumps, not table A/B alternation.** Under the contention above the coarse bake took 300 - 500 ms, so the table changed 4 - 6 times in a 2 s drag (gens 212 -> 244 -> 269 -> 286 -> 318, 570 ms on the first), and once or twice per drag an older cached exact table was bound in between (gen 200 between 244 and 269; "lagging" / "stale" kinds on 4 - 110 frames per drag): the picture jumped back to the look of an earlier position. 33^3 <-> 17^3 LUT size changes 1 - 3 per drag. The adaptive controller stepped the rung down 1 - 3 times in the first 100 ms of every drag (1080 -> 360 px: a visible sharpness jump, scaled by the presenter).
* **Order of presentation:** frames are strictly ordered in the engine (one render thread; no out-of-order rows or callbacks in any of the 24 phases). The iOS texture ring (`DappleViewPlugin.swift`) had a real defect, not measurable through the null presenter: one `writing` slot; a Metal completion handler that ran after the next `acquire` published the surface that was still being drawn into (a torn frame shown) and leaked the finished one. Fixed with a FIFO of surfaces handed to the GPU (completion handlers run in commit order).

### 36.3 What was built

1. **Background work yields** (`sysperf.h`): `sys::setInteraction` (set by `dapple_set_interaction`), `yieldToInteraction`, `utilityQos`. The background `ThreadPool` threads, the match worker, the calibration warm-up (now **one** worker on Apple, starts only after the selection rested 600 ms, aborts and gives its key back when another stock is chosen, pauses while a finger is down), the medians seed and the prebake run on utility QoS; the exact-table stock worker waits for the finger when the stock's own tables are ready.
2. **Own stock within one frame while sliding.** `FilmResolved::coarse` is set for every interactive film frame (exact table when cached, else the 17^3 of its own values; `filmResolve`). A stock whose raw + curves tables are cached (`stockTablesReady`) is shown as itself; the previous-stock fallback remains only for the first minutes after install while the prebake has not reached a stock. A frame of a stock that has no match yet uses the stock's neutral print EV 0 (the value of its prebaked coarse table) with the previous film EV; the settled frame computes the real match as before. A drag frame with no stale table of the same stock bakes the coarse table itself instead of also queueing it on the worker.
3. **Prebake of all stocks (choice: background bake at idle + disk persistence, nothing shipped).** `dapple_film_prebake_start()` (called by the app after the swatches and the GPU probe): raw + curves + coarse print table of every stock at default settings, 20 stocks in about 0.3 s on the A11 (12.4 + 0.7 + 2.7 ms per stock), persisted through the existing `film_tables` disk cache. Reason not to ship tables: the disk keys carry the app version + build fingerprint (a shipped file keyed only by the pack hash would need a second key scheme), the work is about 0.3 s once per install, and the print table is per photo anyway (print EV). Size on disk (estimate from the table dimensions, not measured on the phone): raw 194^2 x 4 floats = 150 KB, coarse print 17^3 x 4 floats = 79 KB, curves a few KB: about 0.23 MB per stock, **about 4.7 MB for 20 stocks**, in the cache directory, not in the asset pack. The per-photo calibration does not block a switch: neutral EV while sliding (item 2), exact match after release.
4. **One tier per drag.** `AdaptiveQuality.beginDrag / endDrag`: the rung is frozen from pointer-down to release (the frames are still measured; at most one step is applied at the end, so it cannot oscillate; wired in `EditorEngine.beginInteraction / endInteraction`; unit test). Table size: a coarse frame takes the coarse table of its own values when cached even if the exact one is, and every exact table (settled frame, or exact used as the first choice) queues its coarse twin, so the next drag starts on the 17^3 tier.
5. **Presentation:** the FIFO fix in `DappleViewPlugin.swift`.
6. **Not built: GPU waypoint interpolation (b).** With the contention gone a coarse table is 2.7 ms and the drag shows the previous step (one 16 ms frame of lag), so waypoints would add a second / third 3D sampler and weights in three shader backends for nothing, and the 2-D pad is not separable (print EV, black and warmth change the table jointly), so there is no interpolation error to quote; none was measured. If a slower phone ever needs it, the blend would be a CPU sum of cached coarse tables (0.05 ms) feeding the existing fence-and-swap upload, not a shader change.

### 36.4 Results (frame trace, same scenarios; before = tree at HEAD + trace, after = this tree; latency = submit to frame done incl. the 16.7 ms grid)

iPhone X, interactive frames (479 after / 437 before over A1, A2 and the four drags), render ms histogram: <=8: 54 -> 133; 8-17: 361 -> 335; 17-33: 6 -> 5; 33-67: 5 -> 6; 67-134: 8 -> 0; >134: 3 -> 0; longest render 625 ms -> 64 ms.

| X, per phase | before: latency med / max | after | before: look discontinuities (previous-stock frames + table gen backwards + tier + LUT size changes) | after |
|---|---|---|---|---|
| A1 slide, first visits | 64 / 167 ms, 8 of 8 frames the previous stock, tier 2 | 35 / 106 ms, own stock 8 of 8 | 10 | **0** |
| A2 slide, second pass | 84 / 380, 6 of 8 previous stock | 22 / 126 | 9 | **0** |
| exposure pad, stock 1 | 24 / 641, 52 previous-stock frames, one 625 ms frame | 17 / 86 | 55 | **0** |
| exposure pad, stock 2 | 22 / 136, 27 previous-stock frames | 14 / 37 | 29 | 1 (33 -> 17 once, first drag of the stock) |
| texture pad, stock 1 | 21 / 132 | 16 / 51 | 2 | **0** |
| texture pad, stock 2 | 15 / 135, gen back 1, 3 LUT size changes | 24 / 56 | 5 | 3 (first drag of the stock: exact -> coarse, one older table) |

Gaps above 2 refresh periods (33 ms) between consecutive frames of a drag: before 1 in each pad drag plus 26 in one (X exposure, stock 1); **after 0 in all four pad drags** (XR as well). Scenario A has an input every 150 ms, so its frame-to-frame gap is the input spacing; the number to read there is the latency: median 35 / 22 ms (first visit / second pass) against 64 / 84 ms, about 2 refresh periods including the grid; the first frame of a slide (pipeline, table upload, rung) still costs up to 106 - 126 ms. Tier changes inside a drag: before 1 - 3, after 0 in all 24 phases on both phones. Previous-stock frames: 93 of 437 before, 0 after. Presentation order: no out-of-order row or callback before or after.

iPhone XR (A12): before A1 latency med / max 41 / 114 ms with 8 of 8 previous-stock frames, A2 82 / 355 with 7 of 8, exposure pad 29 and 38 previous-stock frames and 26 gaps above 2 periods in one drag, longest render 322 ms; after: 0 previous-stock frames, 0 gaps in the four drags, 1 discontinuity (gen back, exposure pad stock 2) in 6 phases, A1 latency 35 / 91 ms, longest render 40 ms. The XR had more headroom before; the same causes show.

Remaining, honestly: the table size changes once in the first drag of a stock whose coarse twin does not exist yet (1 - 3 on the X in 2 of 6 phases; coarse vs exact differ by mean 0.5 / 255, max 3); every pad frame shows the previous step's table (one frame of lag, kinds `stale` / `lagging`, by design); the match of a pad position is one step behind (`stalePrev`); the first frame of a slide is 91 - 126 ms; the Swift ring fix is not covered by a trace (null presenter) and was not observed on a screen; the autotest seeds its rung from a 6-frame measurement, the editor from the engine warm-up; the "before" runs lack the 100 ms touch-down pause and the rung seed of the later scenario (the "before" first frames are therefore a little pessimistic).

## 37. Modern rendering family: 通透 / 鲜明 / 柔和 (imaging engineer, 2026-11-13)

A second family of rendering bases next to film: digital renderings that use the whole tonal range (black is display black, white is display white), clean colour, which film cannot do (paper black is not black, paper white is the ceiling). Engine and evidence only; the UI (film map, a "默认成像" page) comes later.

**Selector and data model.** One base per photo: `EditParams::filmStock` (JSON key `film`) takes either a film stock id or `modern_clean` (通透), `modern_vivid` (鲜明), `modern_soft` (柔和); `film_strength` (0..100, default 100) is the strength of whichever base is chosen (0 = the standard rendering, 100 = the full rendering). Exclusive by construction (one string). The film code never sees the ids (they are no stock: `resolveFilm`, `ParamInputs::filmActive`, `matchFilmExposure`, `developer.cpp`, video and the headroom policy test `isModernId`). Engine keys: new key `modern` (hash of kind + strength, only when modern); the `filmStock` key hashes "" for a modern id so film nodes are untouched; `modern` is in COLOUR (lattice, analysis), `ToneTable` (+ `ctx.proxy` and white balance: the end points are measured after it), `Hdr`, `Detail`. `DAPPLE_ENGINE=off`: the `Renderer` constructor drops a modern id (the old path ignores the family, as it ignores `hdr`). The nine looks and all user controls stay on top. API: `core/include/dapple/modern.h` (`modernKind`, `modernRecipe`, `modernEnds`, `modernCurve`, `modernJson`).

**The model (scene-linear after white balance and the exposure anchor).**
1. End points from the scene (256 px proxy; white balance and camera baseline = the anchor, not the user's exposure): black = P0.5 of Y (never above mid-grey - 3 EV), white = P99.7 limited to 4 x P97 (specular pixels), at least 1.5 EV above mid-grey and 1.5 x P90 (a large bright area keeps 0.6 EV of headroom). Clipped data is above the white point and maps to display white. Low-contrast guard: the sRGB-encoded span white - black is at least 0.62 (stretch <= 1.6 x); a narrower scene has both end points moved away from the data about mid-grey (black not below 0). The user's exposure moves the picture against fixed end points (it behaves like exposure, and the Ultra HDR rendition keeps working).
2. Tone: in the encoded domain between the end points, two power pieces with equal slope at the pivot (toe exponent >= 1: short, no lifted blacks; shoulder exponent >= 1: zero slope at white). The pivot is mid-grey, kept at the standard curve's value; contrast 1.16 / 1.30 / 1.04. Strength blends with the standard curve in encoded units (mid-grey exact at every strength, monotone). The user's contrast / highlights / shadows / whites / blacks act before it (the end points are mapped through them); the tonal-class move after it.
3. Luminance-driven: the pixel is scaled by Yd / Y (all channels, no per-channel clipping). **Why OKLab:** uniform scaling of linear RGB multiplies L, a, b of OKLab by one factor (the cube root is homogeneous), so the OKLab hue angle is exactly invariant under the tone curve; every colour operation below changes L and C only, never the angle.
4. Colour (before the user's colour edits): chroma gain `chroma` (1.10 / 1.30 / 1.02) weighted by a lightness window (smoothstep 0.10..0.42 up, 0.78..0.97 down: richer mid-tones, nothing in deep shadow or near white) and by room under a per-hue soft ceiling (0.30, minus 0.10 around hue 142 deg green, minus 0.09 around 264 deg blue: deep, not neon); blue / cyan lowered by 0.01 / 0.02 / 0.035 L. **Skin rule (a colour-space window, no face detection):** OKLab hue 52 +- 20 deg (Gaussian), C 0.03..0.2, L 0.3..0.95; inside it `skinGuard` (0.7 / 0.75 / 0.85) of the chroma gain is withheld. After the user's edits: chroma fades with L over 0.88..1.0 (to 0 at white), then gamut mapping by chroma compression in OKLab at constant L and hue (bisection for the boundary, a smooth knee at 88 % of the way, exponential approach, nothing clipped); the standard chord only sees what is left (partial strengths).
5. Local contrast: `clarity` / `texture` added in `makeSpatial` (10 / 4, 18 / 8, 0 / 0, scaled by the strength) through the existing guided-filter Detail node (halo-free); scene adaptive base compression = the single-image HDR of section 33 at `hdr` >= 35 / 45 / 50 (it only acts when the scene range exceeds what the curve shows).
6. CPU / GPU: the colour stage lives in the lattice and the tone stage in the 1D table, both CPU baked, so the GPU executor, the interactive tier, tiles and the exact variant share one implementation (no new shader). Ultra HDR: the bright rendition of a modern base is the standard rendering lowered by H and by log2(white point) (`modernWhiteEv`, `Renderer::modernSdrWhite`), so headroom is what lies above THIS picture's white point; plain JPEG when there is none (the sample at default: peak 1.11, nothing written; at +1 EV: H 1.5, gain max 1.08, reconstruction rms 0.043 stop, worst 0.49, 93 % of the extra light). The colour of the excess follows the standard bright rendition, not the family's colour (not measured on saturated highlights).

**Tests** (`dapple_modern_tests`, ctest `engine.modern_*`): identity (strength 0 byte-identical), off, json, ends (neutral ramp: output P0.5 0.000, P99.5 0.95 .. 0.97 against standard 0.004 / 0.87; the bound is loose because the white point keeps headroom), fog (guard: P1 > 0.08, P99 < 0.92, stretch <= 1.6 x contrast), midgrey (tone table at 0.18 equal to the standard within 2e-4 at every strength, with user edits; end to end on a ten-stop scene within 0.015 because hdr and clarity also act), hue (OKLab error up to white <= 2.6 deg measured, bound 3, standard 6.2; no chroma re-growth near white), skin (hue <= 1.9 deg, chroma x0.95 .. x1.09, L within 0.03; bound 2.5 deg, x0.93 .. x1.10, 0.04), monotonic (black falls, white rises, distance from the standard grows with the strength), tiles (96 / 200 px vs whole: max 1 code in a few bytes), parity (GPU vs CPU frame: mean 0.016, p99 0.5, max 1.7 dE; with edits and interactive), plus `uhdr.session` modern cases, golden `modern/*` (6 new frames, the 151 older ones unchanged) and two conformance baselines.

**Cost** (RTX 4060 Ti, Vulkan, `dapple_modern_tests bench`, synthetic 6000 x 4000; noisy, +-0.1 ms): 1080 x 720 interactive 1.13 ms standard, +0.0 .. +0.1 ms (tables are CPU baked: one 33^3 lattice evaluation per colour change); full resolution 34.8 ms standard, +2.2 (通透), +4.3 (鲜明), +0.3 (柔和): the guided-filter local contrast is the cost. CPU exact variant at 1920 x 1280: 28 ms standard, 94 - 103 ms modern (per-pixel gamut bisection, clarity, hdr field). Not measured on a phone.

**Evidence** `build/modern/sheets/sheet_*.png` (generator `build/modern/mksheets.py`; 标准 | 通透 | 鲜明 | 柔和 | Portra 400 at its defaults; luminance histograms underneath; `develop` with its automatic correction). Verdicts: sky landscape (Galaxy S23): better in all three, black and white reached, sky and clouds deeper; foliage (Z8): better, 鲜明 greens rich not neon, 通透 cleanest; dusk (Pixel 8): better, silhouettes black, cloud contrast up; interior (duck): better, wall whiter, carpet deeper, black stays at 0.16 because the scene has none (guard); high contrast (chart, snow): best case, both ends hold; saturated flowers: reds and pinks rich without clipping, 鲜明 strongest; fog field: the sky goes near white and the ground darkens, the scene is stretched more than a foggy mood wants (the guard limits the span only, and this picture has dark trees and soil so it did not engage): crisper, mood lost; portrait (R10): skin and shirt better (hue within 1 deg), but the bright out-of-focus green background turns pale in all three (the S curve lifts an already bright standard rendering): failure for high-key backgrounds. 柔和 differs only slightly from 通透. No crushed shadows or plastic skin seen at 420 px; shadow noise at full size was not judged.

**Not done.** UI / film map / defaults page; high-key protection (background wash-out); fog mood (a rule on the median); full-resolution look at noisy shadows; iOS; the colour of the HDR excess; phone cost.

**Files.** `core/include/dapple/modern.h`, `core/src/modern.cpp`, `core/src/{renderer.cpp, c_api.cpp, tone.cpp, developer.cpp}`, `core/include/dapple/{edit_params.h, renderer.h}`, `core/engine/graph/{params.cpp, registry.cpp}`, `core/engine/video/video_frame_session.cpp`, `core/CMakeLists.txt`, `tests/engine/{modern_tests.cpp, engine_tests.cpp, golden/golden_tests.cpp, golden/corpus.txt}`, `tests/{uhdr_tests.cpp, CMakeLists.txt}`.

### 37.1 Round 2 (lead review): where the mass of the picture is, three characters, HDR colour

This round supersedes the round 1 numbers above where they differ (recipe table, chroma fade window, tests).

**Adaptive rules (`modernDistribution`, `makePointOp`).** Let t be a sample's position between the end points in the encoded domain (after the user's tone edits), 256 px proxy.
1. *Mass in the upper tones:* wHi = smoothstep(0.25, 0.55, share of samples with t > 0.72). The white point is raised by `1 + 0.7 wHi` (headroom above the bright area), the shoulder lift is kept only `1 - 0.7 wHi` (the upper piece goes straight), the mid-tone lift (柔和) is scaled by `1 - wHi`.
2. *Mass in the lower tones (symmetric):* wLo = smoothstep(0.30, 0.60, share with t < 0.22); the slope at the pivot is `1 + (contrast - 1)(1 - 0.85 max(wHi, wLo))`, so a large bright or dark area is not pushed into the shoulder / the toe.
3. *Empty middle (fog, snow, overcast):* gap = (1 - smoothstep(0.08, 0.25, share with t in 0.3 .. 0.7)) x (1 - smoothstep(0.25, 0.5, mean saturation)) x [bimodal: share of the two ends > 0.8]; the black point is pulled `1 - 0.6 gap` of the way (less stretch, the ground does not go heavy).
4. *Chroma stays longer:* the lightness window of the chroma gain ends at 0.88 .. 0.99 (was 0.78 .. 0.97) and the fade to zero at white runs over 0.94 .. 1.0 (was 0.88 .. 1.0).
5. *Strength:* chroma gain, blue deepening and lift scale with the strength (before only the tone curve did).

**Three characters** (all hue-safe: lightness, chroma gain, local contrast, and a highlight bias that only touches near-neutral highlights, C < 0.05): 通透 contrast 1.14, chroma 1.10, clarity +22, texture +14, cool clean whites (bias 0.008 at 255 deg); 鲜明 contrast 1.42, chroma 1.45, blue deepening 0.045, colours 0.01 L deeper, clarity +20, warm highlights (0.008 at 65 deg); 柔和 contrast 0.88 (the curve may flatten the mid-tones, toe and shoulder exponents never below it), chroma 1.0, colours +0.035 L lighter (coloured pixels only: neutral mid-grey is not moved), clarity -8, texture -6, hdr 55. Mean dE2000 between renderings over the 11 scenes (8 RAW + synthetic high-key, low-key, fog field), `build/modern/mksheets2.py`: 标准-通透 3.8, 标准-鲜明 4.9, 标准-柔和 3.1, 通透-鲜明 2.8, 通透-柔和 2.4, 鲜明-柔和 3.6 (1 is about a just noticeable difference). The characters are visible at 420 px on foliage, saturated, sky and portrait; 通透 and 柔和 are the closest pair (lowest on the low-key and fog scenes: 0.9 and 1.3).

**Metric on the portrait's source** (background strip, `mksheets2.py`): 标准 chroma C* 27.9, L* spread 7.3, mean L* 87.9; 通透 25.3 / 8.0 / 92.0; 鲜明 30.1 / 8.1 / 91.6; 柔和 24.8 / 7.7 / 92.0. The lightness separation is above 标准's in all three, the green is back (round 1: washed to near white), 鲜明 keeps more chroma than 标准, **通透 and 柔和 are 9 % and 11 % below 标准's chroma and the background is 4 L* lighter: not fully met** (a stronger rule keeps the real portrait but flattens the synthetic high-key scene: the two measurements pull in opposite directions; the synthetic test is the stricter bound in the suite). Synthetic high-key (`highkey`): chroma x1.06 / x1.41 / x0.95, spread x0.94 / x0.96 / x0.87 of 标准; low-key: spread x0.96 / x0.91 / x1.05 (bound 0.88), chroma x0.88 / x0.94 / x0.85 (bound 0.85); fog field (`gap`): ground mean L 0.33 / 0.32 / 0.36 against 0.35. On the real fog photo the gap rule changes little (P99.5 0.953 -> 0.948; before / after rows in `sheets2/sheet_fog_lowcontrast.png`): that picture is not an empty-middle scene by the numbers (its trees and soil fill the middle).

**Ultra HDR colour.** The gain map is one channel (luminance gain), so the displayed highlight is the SDR pixel times 2^gain: the colour of the excess is the SDR (modern) colour by construction, not the standard bright rendition's (that rendition only decides the luminance gain). Measured at +1 EV on the R6 II sample, 4839 highlight pixels: per-channel ratio reconstruction / SDR differs by 0.0000 on average, 0.0005 at most. The sample has no strongly saturated highlight; the argument does not depend on it.

**Sheets** `build/modern/sheets2/` (8 scenes + `synth_highkey`, `synth_lowkey`, `synth_fogfield`, 4 columns without Portra for the synthetic ones). One-line verdicts (标准 vs 通透 / 鲜明 / 柔和): sky landscape: all better, 鲜明 deepest sky, 柔和 hazier shadows; foliage: all better, 鲜明 rich but not neon, 柔和 airy and pale; portrait: skin fine in all, background keeps its green (柔和 palest); dusk: all better, silhouettes black; interior: all better, 柔和 flattest; high contrast (chart, snow): all better, both ends hold; saturated: all better, 鲜明 strongest without clipping; fog: all crisper, ground a bit heavy, mood partly lost (acceptable per review); synthetic high-key: 通透 and 柔和 close to 标准, 鲜明 neon green background (chroma x1.41: the synthetic green is already saturated, a real risk for vivid on saturated bright greens); low-key: all keep the dark area, 通透 / 鲜明 darker blacks; fog field: ground not crushed.
