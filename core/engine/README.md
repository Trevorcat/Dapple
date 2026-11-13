# core/engine: how to add a node

Plan of record: docs/engine/pipeline-architecture.md. Stable IR headers (change only additively, tell the team):

- `graph/types.h`   Hash128/Hasher, ParamKey/ParamMask, NodeType enum (append only: order enters the structure hash), Footprint, Domain.
- `graph/params.h`  ParamRegistry (sliders from paramFields() + group/context keys), ParamInputs, FrameRequest, SessionCtx, ParamSet.
- `graph/graph.h`   NodeDesc, NodeRegistry, Graph (validate, structureHash, computeKeys, dump), GraphBuilder.
- `graph/node.h`    CpuImpl / GpuImpl / NodeBehaviour / ImageView / Halo / Rect / Status.
- `exec/result_cache.h` content-addressed cache; `exec/engine_session.h` per-photo session.

Adding a node:
1. Append the type to `NodeType` (types.h, before `Count`).
2. Declare it in `graph/registry.cpp`: `def(type, name, outputDomain, footprint, minIn, maxIn, deps)`. `deps` must be the SMALLEST
   set of ParamKeys the node reads itself (ancestors' keys are inherited through keyed edges). New slider = automatic key; new
   non-scalar input = add a group/context key in `params.cpp` with a `hash` and a `perturb` function.
3. Wire it in `graph/builder.cpp` (conditional dependencies are decided by wiring, e.g. ToneTable gets the ToneAnalysisPre edge only
   when toneStrength > 0).
4. Update snapshots: `DAPPLE_UPDATE_SNAPSHOTS=1 dapple_engine_tests snapshots`, review the diff of tests/engine/graph_snapshots.txt.
5. Run `dapple_engine_tests keys` (undeclared-influence check on every node/key) and, for table nodes, `conformance`
   (equal key => bit-identical output; has negative controls). A node with a wrong mask fails these.
6. Golden corpus: `dapple_golden verify tests/engine/golden/corpus.txt off full` must stay byte-identical, and `dapple_golden selfcheck`
   (old Renderer vs engine on this machine, any architecture) must report 0 differences.
   A table node's evaluation is its `CpuImpl::bake` (`nodes/main/cpu_nodes.cpp`; `engine::bakeNode`); `EngineSession::runHostTables` caches it.
Edge `keyed=false` = scheduling edge only (producer key does not enter the consumer key).

## Planner / executor (CPU engineer, docs/engine/pipeline-architecture.md section 17)

- `plan/plan.h`: `Planner::plan(graph, PlanRequest, cache) -> Plan` (fused point chain, cumulative window groups per sweep, halos, global solves, cache ops).
- `exec/cpu_executor.h`: `CpuExecutor::run(plan, ExecEnv, ExecStats*)`; `CancelToken` is polled between tiles/groups and in the ThreadPool chunk loop.
- `nodes/main/cpu_nodes.cpp`: `cpuImpl(NodeType)`: `block` fragments (3-float RGB, n <= 256) for point nodes, `tile` fns (additive `CpuImpl::tile`) for window nodes.
- `DAPPLE_ENGINE=full` runs every frame and export through them; `dapple_golden verify corpus.txt full`, `dapple_exec_tests <whole|tiles|plan|cancel|analysis|bench>`.

## GPU executor (GPU engineer, docs/engine/pipeline-architecture.md section 19)

- `exec/gpu_iface.h`: `IGpuExecutor::run(GpuRun&, GpuOut&)` consumes the same `Plan` as the CPU executor; `GpuProvider` is the process-wide device selection (installed by `gpu::installProvider()`). `EngineSession::renderFull / exportGpu` pick the executor per frame (Vulkan -> GLES 3.1 -> CPU, failure semantics of 5.9).
- A node gets a GPU implementation by registering `GpuImpl::recordPorts` in `nodes/gpu_nodes.cpp` (`gpuImplFor`), a `Walk::sweep` case in `exec/gpu_executor.cpp` that builds its resolved block, and its type in `headNodeOk` / `windowNodeOk`. A plan that contains any other node is `Unsupported` and runs on the CPU executor.
- Kernels declare the workgroup shape as spec constants 16 / 17 (2D) and 18 / 19 (reduce); group counts come from `GpuRecorder::gx / gy` (`docs/engine/gles-shader-patches.md`).
- Tests: `dapple_gpu_tests`, `dapple_gpu_engine_tests` (`hal box sched select external`), `dapple_film_gpu_tests`; bench `dapple gpu-bench`.

## Film nodes (core/engine/nodes/film)
Three table nodes (`FilmTableRaw`, `FilmTableCurves`, `FilmTablePrintScan`) and eight image nodes (`FilmExposure` ... `FilmScan`), each with
its own minimal mask; `GraphDetail::FullFilm` builds them. CPU impls: `filmnode::cpuImpl(type)` / `behaviour(type)` (`film_nodes.h`); the
whole-frame composition `runFilmFrame` is what a Film group executes (byte-identical with `FilmSimulator::render`). GPU: `film_gpu.h`. Every film frame runs on the GPU executor (no CPU fallback): strength < 100 (Blend), display curves / look LUT, tiled export (`recordFilmPre` once for the frame, `recordFilmWindow` per tile with `filmWindowHalo`, tiles bit-identical with the whole frame), the exact variant, dye-cloud blur and micro-structure (docs/engine/pipeline-architecture.md 23). A new film pass takes the window origin and hashes noise on absolute coordinates; gate: `gpu_tests --film-tiling` (0 differing pixels), `--coverage` (no film edit may fall back).
Two more image nodes carry the enlarger diffusion filter (`FilmPrintExposure`, `FilmEnlargerBlur`, identity without the filter) and the highlight boost
is part of `FilmBlurExposure`; both are driven by `FilmResolved::fx` (docs section 22, `film_boost.comp`, `film_point.comp` MODE 6 / 7).
Gates: `film_nodes.*` (CPU, `film_nodes.fx` for the two effects) and `film_gpu.parity` (`-DDAPPLE_GPU=ON`, `DAPPLE_GPU_API=gles` for GLES). `DAPPLE_FILM_NODES=0` makes `renderer.cpp` use the simulator again.

## Demosaic node (core/engine/nodes/demosaic, docs/engine/pipeline-architecture.md section 32)

RAW mosaic -> linear working RGB: `decodeRawMosaic` (dapple/mosaic.h) returns the 16-bit mosaic with CFA, black / white, multipliers, matrix; `demosaic::demosaic` (CPU, std::thread tiles + halo, region of interest) and `demosaic::GpuDemosaic` (single-source GLSL `shaders/demosaic_*.comp`, Vulkan / GLES 3.1 / Metal) run the same Bayer (gradient-weighted Hamilton-Adams) and X-Trans (directional green + guide-weighted colour differences + median) kernels; `MosaicCache` caches the mosaic and the demosaiced picture by file content hash. Not yet a graph node: planner / executor / C API wiring is the hunk list in 32.9. Tests `demosaic.*` (ctest), evaluation against LibRaw `dapple_demosaic_eval file --out build/demosaic --gpu 1024 --proxy --cache`.

## Local adjustments (docs/engine/pipeline-architecture.md section 40)

`EditParams::local` (dapple/local_adjust.h, JSON key `"local"`) -> `local::bind` (core/src/local_eval.{h,cpp}) resolves the list for a frame
(`FramePlan::local`, `FrameKernels::localFrame`): analytic components + low-resolution photo-space planes (brush coverage, raster guided-filter
coefficients; process-wide plane cache keyed by content). Nodes: `LocalLinear` (point, after `Gain`/`Hdr`: masks + scene-linear ops, writes the
stage-W amounts into aux planes; GPU = `point.comp` built with `LOCAL` -> `point_local`, code in `shaders/local.glsl`) and `LocalDetail` (window,
after `Detail`; `shaders/local_detail.comp`, CPU `local::detailWindow`). Film frames: stage L on the film's scene input (`FrameKernels::runFilm`,
`shaders/local_film_in.comp`). New op = a row in `localOps()` + its arithmetic in local_eval.cpp and the GLSL twins. Tests: `dapple_local_tests`
(ctest `engine.local_*`), golden `local/*`.
