# Metal backend of the compute HAL

Code: `core/engine/hal/metal/metal_device.mm` (+ `.h`), `metal.cmake`, `metal_tests.mm`; shader tool `tools/shaderc/main.cpp` (`--msl`).
Plan of record: `docs/engine/pipeline-architecture.md` 6.3 / 7.1.

## Shader path (build time only)

GLSL 450 -> glslc (SPIR-V) -> `dapple_shaderc --msl` (SPIRV-Cross `CompilerMSL`, iOS platform, MSL 2.3, `enable_decoration_binding`, entry
point renamed `k_<name>`) -> `xcrun metal -std=(ios|macos)-metal2.3 -fno-fast-math -O2` -> one `dapple.metallib` embedded in the library
(no shader compilation on the device). Vulkan binding number = Metal argument index of every resource class (slot i = binding i + 1,
the std140 block is `setBytes` at buffer 0); GLSL spec constants = function constants with the same id (type taken from the function's own
declaration); workgroup size = threadgroup size from the spec constants 16/17 (2D) and 18 (reduce). `-DDAPPLE_METAL_EMBED_SOURCE=ON` also embeds the MSL text
and compiles it at run time when no metallib is present.

## HAL mapping

Buffers: Upload / Readback = Shared (unified memory, mapped), DeviceLocal = Private. Images: `MTLTexture` Private, ShaderRead|ShaderWrite|PixelFormatView
(R8, RGBA8, R16F, RGBA16F, R32F, RG32F, RGBA32F, R32UI). Read-write storage access: reported from `readWriteTextureSupport` (M1: tier 2, every format
the shaders use except RG32F, which no shader reads and writes). One serial compute encoder per command-buffer part (dispatches complete in order,
`barrier()` only counts), blit copies between encoders, `flushPart()` = commit and continue in a new command buffer, fences = per-slot
completion counters (all parts waited). `copyImageToExternal`: `ExternalImage.handle` = `id<MTLTexture>` (IOSurface / CVPixelBuffer backed): RGBA8 blit, BGRA8
swizzle kernel (the texture needs ShaderWrite). Blocklist, `DAPPLE_GPU_SPLIT_GROUPS`, `DAPPLE_GPU_DEVICE` honoured. Not done: GPU timestamps per pass (only the command-buffer GPU
time is accumulated; `gpuMs` in the benches prints 0.00), pipeline binary archive (cold start compiles the used functions), `PresentExt` for a `CAMetalLayer`.

Apple specifics respected: unified memory flag (film `v3Direct`, scan threshold 12, tile 1024), threadgroup memory 32 KB, threadgroups up to 1024 threads
(per-pipeline `maxTotalThreadsPerThreadgroup` checked at creation), no fast math, `MTLCreateSystemDefaultDevice` is nil without a window server (ssh sessions) so macOS uses
`MTLCopyAllDevices`, `MTLCommandBufferErrorDeviceRemoved` is macOS only.

## Selection

`gpu_context.cpp` (executor engineer) tries `Api::Vulkan` then `Api::Gles31`. To avoid touching it, on Apple the Vulkan factory symbol is implemented
by the Metal device (`vk::createVulkanDevice` -> `metal::createMetalDevice`); `caps().api == Metal`, but JSON status / log strings from `gpu_context.cpp` and
`engine_session.cpp` still say `"vulkan"` (`"api":"vulkan","device":"Apple M1"`). When those two files learn `Api::Metal` the alias goes away.
`hal::createDevice(Api::Metal)` and `DAPPLE_GPU_API=metal` (selectDevice) work.

## Results on the M1 (MacBook Pro, 16 GB, macOS 14.7.6, Release, 2026-11-13)

All suites run with the same budgets as Vulkan / GLES (the tests assert them): `dapple_gpu_tests` (`gpu_parity`: per-stage parity, mean dE2000 0.036 - 0.079,
p99 0.58 - 0.88, max <= 1.9 on 640x448 and 1536x1024; seams: **0 differing pixels** in every scenario and tile size (1500x1000 incl. five film scenarios, 4000x2667 picture)), `--coverage`
(192 / 192 cases on the GPU executor), `--film-tiling`, `--exact`, `engine.gpu_{hal,box,sched,select,probe}`, `film_gpu.parity`, `film_gpu.frames`
(all mean <= 0.1, p99 <= 1.0), `metal.caps`, `metal.external`. `engine.gpu_external` (Vulkan swapchain handles) is disabled on Apple. Full ctest: 69 of 69 pass
(+3 skipped / disabled). Logs: `build/ios/logs/`.

| Case (`gpu_tests --film-timing [24mp]`, wall incl. readback) | M1 Metal | desktop Vulkan RTX (docs/engine/pipeline-architecture.md 23.3) |
|---|---|---|
| 1600 px film final frame, strength 100 | 44.5 - 49 ms | 15.5 - 16.6 ms |
| 1600 px, strength 60 (Blend) | 44.6 - 49 ms | 16.1 - 16.5 ms |
| 1600 px, curves + look LUT | 48.3 - 48.8 ms | 16.1 - 16.8 ms |
| 1600 px, strength 60 + curves + LUT | 49.6 - 50.4 ms | 16.6 - 17.0 ms |
| 24 MP export (6000x4000, tiled), strength 100 | 657 ms (first 713) | 134 ms (222) |
| 24 MP export, strength 60 | 687 ms (first 776) | 139 - 144 ms (191) |
| 24 MP export, curves + LUT | 676 ms (first 695) | 137 - 141 ms (182) |
| 24 MP export, strength 60 + curves + LUT | 682 ms (first 716) | 139 ms (175 - 203) |
| CPU engine on the same machine (arm64, autovectorised) 1600 px / 24 MP | 51 - 72 ms / 972 - 1959 ms | 26 - 31 ms / 462 - 805 ms (x86 AVX2) |
| film interactive, long edge 1080 (`--film-interactive`, medians) | still 7.2 ms, strength / exposure / halation drag 7.8 - 8.0 ms, grain drag 10.8 ms, warmth drag 21.9 ms (film table re-upload) | not recorded |

The M1 GPU is about 3x slower than the RTX on these frames, still 1.1 - 1.4x faster than the CPU engine at 1600 px and 1.5 - 2.9x at 24 MP. The 24 MP
export keeps a 512 MB transient pool (22 B/px). Optimisation was not attempted (no per-pass timestamps yet, first thing to add).
