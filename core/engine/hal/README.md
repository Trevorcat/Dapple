# core/engine/hal, exec/gpu_*, nodes/*/gpu.cpp: the GPU side of the engine

Plan of record: docs/engine/pipeline-architecture.md (5.2, 5.9, 6, 16). This is stage (b) steps b0..b3.

## Layers

| Layer | Files | Role |
|---|---|---|
| HAL | `hal/hal.h`, `hal/hal.cpp`, `hal/vk/vk_device.cpp` | Compute-only device: buffers, images, pipelines (SPIR-V table generated at build time), `CommandList` (bind / setParams / dispatch / barrier masks / copies / `flushPart`), fences, blocklist, caps. Vulkan 1.0 backend; GLES 3.1 maps onto the same interface. |
| Scheduler | `exec/gpu_scheduler.{h,cpp}` | `GpuGraph` (passes with resource uses), dependency layering, barrier class masks from real RAW/WAR/WAW only, `GpuPool` of transient images (acquire at first use, release after last use), submission splitting by estimated workgroups. |
| Node records | `nodes/gpu_common.*`, `nodes/{point,nr,dehaze,detail,resize}/gpu.cpp`, `nodes/gpu_nodes.*` | One record function per main node; `gpuImplFor(NodeType)` is the `GpuImpl` table (shader names, typed `recordPorts`, recipe / epilogue flags). |
| Executor | `exec/gpu_executor.{h,cpp}`, `exec/gpu_context.cpp`, `exec/gpu_iface.h`, `exec/source_store.h` | `gpu::Core` (device, pool, per-slot staging / readback / weights buffers), `gpu::Context` (process-wide: Vulkan -> GLES 3.1 selection through `hal/select.cpp`, start-up self-test through the whole engine, device rebuild, epoch), `gpu::Executor` (`IGpuExecutor`, one per `EngineSession`): walks the stages of a `Plan` (HostTable, TileSweep with fused PointChain + cumulative Window groups, GlobalSolve) and calls `gpuImplFor(type)->recordPorts`; device copies of the source live on a `SourceStore` (shared per context), device tables on a `ResultCache`. |
| Selection | `exec/engine_session.cpp` (`runOnGpu`) | unsupported -> CPU frame; transient -> release the device copies, retry once, else CPU frame (GPU stays); device lost -> `GpuProvider::recover`: rebuild once; a failed rebuild or a second loss gives the API up and the next one takes over (Vulkan -> GLES 3.1 -> CPU). Counters in `dapple_gpu_status_json`. |
| Shaders | `engine/shaders/*.comp` | GLSL 450 compute; variants are listed in `core/gpu/CMakeLists.txt` (name, source, defines, binding signature). |

## Mobile GPU rules kept (docs 16)

1. Submissions are cut by the scheduler (`DAPPLE_GPU_SPLIT_GROUPS`, default 250000 weighted workgroups; `CommandList::flushPart`).
2. Read-only inputs go through `sampler2D` + `texelFetch`; storage images only for writes and read-modify-write.
3. Lane-fast axis along the filter axis in both directions (sepconv.comp, boxscan.comp: odd shared row pitch).
4. Shared tiles sized for the radius; **no shared array is sized by a specialisation constant** (boxscan.comp sizes from `#define`s).
5. Big-core pinning of the thread that waits for the fence and copies the readback (`BigCoreScope`, Android).
6. Readback buffers: HOST_CACHED (non-coherent, invalidated after the wait) first, memory type chosen per buffer from `memoryTypeBits`.
7. Unified memory: pool budget <= 512 MB, export tile 1024.
8. A start-up check runs the running-sum box against the direct sum on the device (`boxSelfTest`); a mismatch switches the scan off instead of rejecting the GPU.

## O(1) box (boxscan.comp)

Fixed-point (value * 65536, |v| <= 16) inclusive prefix sums in shared memory, three-step scan, `P[j+2R] - P[j-1]` per output. Integer sums are exact
and modular, so the result is independent of where a segment starts: bit-identical for any tile decomposition and group size, no fp32
cancellation. Quantisation 1.5e-5 per sample (random, averaged). Used from radius 12 (unified memory) / 24 (discrete) for the
single-channel box passes (NR luma, texture, sharpen, dehaze, 4-channel chroma / clarity stay on the direct kernel). `DAPPLE_GPU_BOX_SCAN_MIN=n`
overrides (0 = never).

## Tests

- `dapple_gpu_tests` (ctest `gpu_parity`, `gpu_parity_swiftshader`, `gles_gpu_parity_angle_*`): per-stage parity (per node) vs the CPU reference through `EngineSession` (Planner -> GpuExecutor, `core/gpu/src/gpu_rig.h`), tile seams (0 differing pixels), streamed source, C API. `DAPPLE_GPU_API=gles` forces the GLES 3.1 device, `DAPPLE_GPU_WG=128` the 128-invocation workgroup shape.
- `dapple_gpu_engine_tests` (ctest `engine.gpu_{hal,box,sched,select,external}` and `_swiftshader`, `gles_engine_*`): HAL, box accuracy / tile invariance, barrier layering, steady-state pool, split submissions, selection policy through `EngineSession` with injected failures and a real device rebuild, and the zero-readback seam (native handles, frame into an externally owned image with semaphores). `bench-box` prints the direct vs scan table.

## On-device checklist (Pixel 5 and others, not run in this period)

1. `dapple_gpu_tests` on the device: self-test report must show `box scan vs direct ... ok`; if it says `FAIL` the scan is off and numbers equal phase 1.
2. `bench-box` crossover radius on Adreno 620 (the default 12 is a guess for unified-memory GPUs); vertical `bx_y_v` load pattern (32 rows x 4 columns per group, uncoalesced texture loads) may need a different group shape.
3. Split threshold: a 24 MP frame must finish (no multi-second submission); compare `parts` in the stats with `DAPPLE_GPU_SPLIT_GROUPS`.
4. Export with tile 1024: pool near 320 MB, `trimPool` behaviour, LMK.
5. Readback with `AHardwareBuffer` / zero copy (stage d) is the real fix for the 10-14 ms copy-out.
6. Persisted `VkPipelineCache`: cold vs warm start time of the self-test.
