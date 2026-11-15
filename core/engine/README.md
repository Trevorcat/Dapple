# core/engine

Plan of record: docs/engine/pipeline-architecture.md (section 50 for the final structure). A frame flows

    ParamSet + the frame's blocks --GraphBuilder--> Graph --keys--> HostTable stage (session) --Planner--> Plan --> GPU executor | CPU executor

and nothing between the builder and the executors names a node: everything they do is derived from the node declarations.

## What a node is

One line of `nodes/node_list.inc`: `DAPPLE_NODE(Type, family::desc, gpuImpl)`. The `NodeType` enum (graph/types.h), the registry
(graph/registry.cpp) and the GPU table (nodes/gpu_nodes.cpp) are generated from it. `desc()` lives in the node's family directory and
returns a `NodeDesc` (graph/graph.h):

- ports and keys: name, output `Domain`, input count / domains, `deps` (the SMALLEST ParamMask the node reads itself; ancestors' keys come
  through keyed edges; groups in graph/param_groups.h), `orderKey`;
- port tags read by code: `inSpace` / `outSpace` (`Space`: Source / Scene / Display / Film; `Graph::validate` rejects an image input in the
  wrong space), `precision` (the planner gives each sweep its widest precision, the GPU allocates the working image in it), `rescales`
  (a Resample that changes the grid: the plan's source windows stop there);
- footprint class (`Footprint`: Point / Window / Resample / Global / Source / Sink) and fusion group (`Fusion`: Head = the point chain of a
  sweep, Film = the film stage, None = its own group);
- behaviour over its resolved block (`NodeBehaviour`: isIdentity, halo, inputRect) and where the block is (`block(FrameView)`), the
  frame-global solve it needs for that block (`solve` -> `Accum`), its tile scratch (`workPlanes`), whether it reads the head's side planes
  (`readsAux`);
- wiring: `wire` slot (Source / Scene / Display, `order` within it) and `wanted` (default: the block is not the identity), or Skeleton;
- CPU: `cpu` (`CpuImpl`: block fragment / tile / bake / solve / fuse) and `bind` (fills its slot for a frame from `FrameEnv`).

The GPU side is the family's `GpuImpl` (graph/node.h; declared in nodes/gpu_common.h, defined in the family's gpu.cpp): `group` (records a
fused head group; the highest `rank` of a group records it), `node` (a window node, gets its block, the working image and its solve), `solve`,
`epilogue` (Grain, Pack8 ride on the last pass), `film`, `lattice`.

## What adding a node costs

- An optional point or window node of an existing slot (like Vignette, NRLuma, Dehaze, Grain, LocalDetail): its family directory (block +
  resolve, `NodeBehaviour`, `CpuImpl` + `bind`, `desc()`, GPU `GpuImpl` in gpu.cpp) and **one line in nodes/node_list.inc** (plus the include of
  a new family's header in graph/registry.cpp and its GpuImpl declaration in nodes/gpu_common.h; a new family's block becomes a member of
  `FrameBlocks`, nodes/frame_blocks.h). No edit in the builder, the planner or either executor.
- A node that changes the frame's skeleton (a new branch, a new table feeding ToneColour) also edits graph/builder.cpp, which spells out the
  skeleton: source, Resize, the tables, normal branch, film branch, blend, sink.
- Then: `DAPPLE_UPDATE_SNAPSHOTS=1 dapple_engine_tests snapshots` (review tests/engine/graph_snapshots.txt), `dapple_engine_tests keys` and
  `conformance` (a node with a wrong mask fails these), the golden corpus (`dapple_golden verify tests/engine/golden/corpus.txt full`, 163
  recorded hashes, byte-identical) and `dapple_golden selfcheck` (whole vs tiled, exact vs baked, 8-bit vs float: 0 differences).

## Planner (plan/plan.h, plan/planner.cpp)

`Planner::plan(graph, PlanRequest{size, mode, budget, tile, maxSide, view, filmTiles, device}, cache)`: elides identity nodes, puts Table
nodes in the HostTable stage (cache lookups; by family when the node has an order key), Film-fusion nodes in the FilmFrame stage, Head-fusion
nodes in the head group (a Resample joins it when it feeds a head node), every other image node in a window group with its declared halo;
cuts an accumulation sweep + GlobalSolve before every node whose solve is not cached (device slots when `device`); sizes the tiles from the
memory budget (12 B/px + the widest work planes + the head's side planes) and the executor's image limit; gives every sweep its halo
(sum of the covered nodes' halos + 8 px slack, on the 1/4 grid), precision and tiles (`TileWin`: interior, work, source window through the
head's Resample `inputRect`s). Both executors run exactly these tiles.

## Executors

- `exec/cpu_executor.{h,cpp}`: head = block fragments (fused where the lead's `CpuImpl::fuse` says so; the point family fuses
  [Resize|Geometry] + Gain + [Hdr] + ToneColour), windows = `CpuImpl::tile` with `CpuTile::solved`, GlobalSolve = `CpuImpl::solve`, output =
  the sink node's tile. Streaming export (section 42): `ExecEnv::prepare(TileWin)` hands the plan's source window to the session.
- GPU (`exec/gpu_*.cpp`, library dapple_gpu): `gpu_executor.cpp` (validation, whole frame, tiled export, `GpuFailure` + typed
  `GpuReason`), `gpu_walk.cpp` (records one job through the GpuImpls), `gpu_device_copies.cpp` (resident source, tables, buffers, display,
  film tables), `gpu_core.cpp` (device, pool, slot buffers, deferred destruction: released handles wait for the first fenced submission after
  them, never a `waitIdle`). `exec/gpu_iface.h` is the seam; `GpuProvider` selects the device (Vulkan -> GLES 3.1 -> CPU).
- Session (`exec/engine_session.{h,cpp}` frame policy: one graph per frame from the draft blocks, executor choice, CPU / exact renders;
  `exec/host_tables.cpp` the HostTable stage; `exec/gpu_dispatch.cpp` the GPU selection and failure semantics by `GpuReason`).

## Cache (exec/result_cache.{h,cpp})

One `ResultCache` per session: slots per node type (host), per node type on the device (`slotOf(type, true)`: the GPU executor's tables,
solves, buffers, display stages) and named slots (`kBakeSlot`). Every entry carries host and device bytes and its build cost; limits are
one budget per session: host = 1/32 of the device memory from the memory configuration (`dapple_memory_configure` totalBytes, else the
physical memory), 96..384 MB, halved on a low-RAM device; device = half the device pool budget (set by the GPU executor; the pool keeps its
free images within what the cache leaves). Eviction: GreedyDual-Size (LRU weighted by build cost per MB), never the entry being inserted.
Every member takes the cache's mutex and returns payloads by value (`Hit`), so nothing points into the cache after a call; evicted payloads
are released outside the lock (device payloads go to `gpu::Core::retire`).

## Families (section 50)

`nodes/<family>/`: `point` (frame: Geometry, Gain, Hdr, ToneColour(Exact), DisplayStages, Blend, Pack8, the five table nodes; `nodes.cpp`
declares them), `vignette`, `nr`, `dehaze`, `detail`, `grain`, `local` (LocalLinear, LocalDetail), `resize`, `film` (three tables, ten
image nodes, run as one film stage: `runFilmFrame` on the CPU, `recordFilmFrame` / `recordFilmPre` + `recordFilmWindow` on the GPU),
`demosaic` (not a graph node yet). `FrameBlocks` (nodes/frame_blocks.h) are the families' resolved blocks of a frame; `FrameView` points
at them and at the point family's `Blocks`.

Tests: `dapple_engine_tests` (registry, validate, snapshots, keys, conformance, cache, reuse, overhead), `dapple_exec_tests`
(whole, tiles, plan, cancel, analysis, bench), `dapple_gpu_tests`, `dapple_gpu_engine_tests`, `dapple_film_gpu_tests`, `dapple_local_tests`,
`dapple_golden`.
