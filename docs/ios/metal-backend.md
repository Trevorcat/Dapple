# Metal backend of the compute HAL

Code: `core/engine/hal/metal/metal_device.mm` (+ `.h`, `metal.cmake`, `metal_tests.mm`), display presenter `core/engine/present/metal_presenter.mm`, shader tool
`tools/shaderc/main.cpp` (`--msl`). Plan of record: `docs/engine/pipeline-architecture.md` 6.3 / 7.1.

## Shader path (build time only)

GLSL 450 -> glslc (SPIR-V) -> `dapple_shaderc --msl` (SPIRV-Cross `CompilerMSL`, iOS platform, MSL 2.3, `enable_decoration_binding`, entry point renamed `k_<name>`) ->
`xcrun metal -std=(ios|macos)-metal2.3 -fno-fast-math -O2` -> one `dapple.metallib` embedded in the library (no shader compilation on the device). Vulkan binding number = Metal
argument index of every resource class (slot i = binding i + 1, the std140 block is `setBytes` at buffer 0); GLSL spec constants = function constants with the same id (the type
comes from the function's own declaration); threads per threadgroup from the spec constants 16/17 (2D) and 18 (reduce). `-DDAPPLE_METAL_EMBED_SOURCE=ON` also embeds the MSL text.

## HAL mapping

Buffers: Upload / Readback = Shared (unified memory), DeviceLocal = Private. Images: `MTLTexture` Private (R8, RGBA8, R16F, RGBA16F, R32F, RG32F, RGBA32F, R32UI;
ShaderRead|ShaderWrite|PixelFormatView). Read-write storage access follows `readWriteTextureSupport`. One serial compute encoder per command-buffer part, blit copies between encoders,
`flushPart()` = commit and continue, fences = per-slot completion counters. `copyImageToExternal`: `ExternalImage.handle` = `id<MTLTexture>` (RGBA8 blit, BGRA8 swizzle kernel).
Blocklist, `DAPPLE_GPU_SPLIT_GROUPS`, `DAPPLE_GPU_DEVICE` honoured. `nativeHandles` returns the `MTLDevice` / `MTLCommandQueue` (`swapchainEnabled` = "can present"), which is what the presenter leases.

### Round 2 additions

* **GPU timestamps per pass** (`caps.timestamps`, device option `profile`): every labelled pass is its own compute encoder sampled at both ends with an `MTLCounterSampleBuffer`
  (stage-boundary sampling, supported by Apple GPUs; one 256-sample buffer per command buffer part); GPU ticks -> ns through two `sampleTimestamps` pairs; a label after a copy reports 0 ms so the entries stay aligned with
  the scheduler's passes. Command-buffer `GPUStartTime/GPUEndTime` still feeds `gpuAccumMs`. The sum of the passes equals the command-buffer GPU time within 1 % (32.9 vs 32.5 ms on the M1).
  `DAPPLE_METAL_TRACE=1` prints per command buffer: commit -> kernel start, GPU time, completion latency.
* **Pipeline cache**: `MTLBinaryArchive` at `DeviceOptions.pipelineCachePath` (loaded when the file exists, every created pipeline is added, `pipelineCacheBlob()` serialises it for the executor's
  `savePipelineCache`). Not used on the simulator (its Metal asserts in `serializeToURL`). Warm-up cost without it: the metallib holds precompiled AIR; the GPU pipeline state is built on first use
  (cold start of the first frame on the phones: 125-280 ms for the plain pipeline, 340-1360 ms for the first film frame; see device-results.md).
* **Metal is a first-class API**: the Vulkan-factory alias is gone (`hal::createDevice(Api::Metal)`, `selectDevice()` tries Metal first on Apple, `vk::createVulkanDevice` returns an error there).
  `gpu_context.cpp` / `engine_session.cpp` already list `Api::Metal` (candidates, executor name). Hunks still needed in files of the executor engineer (ready to apply):

```diff
--- a/core/engine/exec/gpu_executor.cpp       (Core::create: only used by tools; fails on Apple today because there is no Vulkan)
+++ b/core/engine/exec/gpu_executor.cpp
@@ std::unique_ptr<Core> Core::create(const hal::DeviceOptions& opt, std::string& err) {
   std::unique_ptr<Core> c(new Core);
-  c->own_ = hal::createDevice(hal::Api::Vulkan, opt, err);
+#if defined(DAPPLE_HAL_METAL)
+  c->own_ = hal::createDevice(hal::Api::Metal, opt, err);
+#else
+  c->own_ = hal::createDevice(hal::Api::Vulkan, opt, err);
+#endif

--- a/core/engine/exec/gpu_context.cpp        (envMode: the default of the GPU mode; iOS / macOS builds should behave like Android: auto)
+++ b/core/engine/exec/gpu_context.cpp
@@ Mode envMode() {
 #if defined(__ANDROID__)
   ...
   return Mode::Auto;  // the default: on after the self-test passed (docs/engine/pipeline-architecture.md 24)
+#elif defined(__APPLE__)
+  return Mode::Auto;  // Metal: probed in the background, CPU frames until it passed (the iOS app also sets DAPPLE_GPU=auto itself)
 #else
   return Mode::Off;

--- a/core/engine/nodes/film/film_gpu.cpp     (v3Direct: the "unified memory => direct taps" rule is wrong for Apple GPUs, see the profile below)
+++ b/core/engine/nodes/film/film_gpu.cpp
-  return forced >= 0 ? forced == 1 : d.caps().unifiedMemory;
+  return forced >= 0 ? forced == 1 : d.caps().unifiedMemory && d.caps().vendorId != 0x106B;  // Apple (0x106B): the staged column tile is 1.9x faster
```

## The display presenter (zero-copy display, `core/engine/present/metal_presenter.mm` + `DappleViewPlugin.swift`)

Choice: **a Flutter `Texture` backed by IOSurface CVPixelBuffers**, not a `CAMetalLayer` platform view. The editor draws Flutter UI over the picture (pads, strips, histogram, overlays): a platform view is composited
as a whole above or below the Flutter layer (no per-widget z-order) and needs a thread merge for synchronised resizing; a texture takes part in Flutter's compositing exactly like the Android SurfaceProducer, the Dart side
(`GpuViewBackend`, same channel `dapple/view`) is shared and only its `supported` test gained iOS.
Per frame: `beginExternal` -> the presenter's own RGBA8 frame image on the engine's device (device lease: `GpuProvider::lease`, so the executor's final pass is written into it by `copyImageToExternal`, no readback);
`presentExternal` -> a compute pass scales the frame region into the next free surface of the Swift ring (3 buffers, the surface follows the Flutter texture size) and the command buffer's completion handler calls
`ready` (-> `textureFrameAvailable`). Host frames (CPU executor, fallback) take `present(rgba)`: the same frame image filled with `replaceRegion`, the same scale pass. Simulator check (iPhone 16 simulator, GPU executor):
6 frames submitted through the view API: `shown 3, externalFrames 2, hostFrames 1, renderFailed 0, presentFailed 0, presentMs 0.31` (the rest were dropped by the latest-wins loop).
Not verified: the editor's own use of the texture on screen (the editor's lifecycle starts the view after its first readback frame; the probe in `IosDebugOpen` exercises the same API), the thermal / memory-pressure trim, resize while frames are in flight.

## Profile of the film path on the M1 (item "ALU-bound?")

Per-pass timings of the 1600 px final film frame (`DAPPLE_GPU_PROFILE=1 DAPPLE_GPU_PASSLOG=1 dapple_film_gpu_tests bench`): 32 passes, 32.9 ms GPU, 483 MB read, 323 MB written (nominal).
Top passes: `film_v3` at full resolution (5 source planes) **10.4 ms (32 %)**, `film_h3` (4 outputs) 3.7 ms, first `film_point` 2.2 ms, `film_v3` (800 px) 2.0 ms, `film_v3` last 1.8 ms, `film_f4ve` 1.3 ms, `film_grain` 1.1 ms.

* **Not ALU-bound on the M1**: the dominant pass does about 1.1 GFLOP (5 planes x 3 channels x ~39 taps x 1.9 Mpx) = 0.4 ms at the M1's 2.6 TFLOPS, and reaches 7-16 GB/s of a ~60 GB/s memory system. Its time scales with
  occupancy, not with arithmetic: workgroup 64 -> 20 ms, 128 -> 9 ms, 256 -> 5.5 ms, 512 -> 5.5 ms (`DAPPLE_GPU_WG`): it is latency / issue bound.
* **The staged variant wins on Apple**: `DAPPLE_FILM_V3_DIRECT=0` (column tile in threadgroup memory) cuts `film_v3` full-resolution 10.4 -> 5.5 ms and the whole frame 32.9 -> 26.7 ms (-19 %). The default rule
  "unified memory => direct taps" was measured on Adreno; hunk above.
* **Relaxed precision (MSL `half`)**: the shaders declare every float `highp` explicitly (docs 7.1 portable subset), so the SPIR-V carries no `RelaxedPrecision` decorations and SPIRV-Cross emits `float` everywhere; there is
  nothing to convert in the Metal backend. Since the passes are not ALU-bound, half arithmetic would gain little on the M1 (it matters on A11/A12 where the film frame is 2.5x slower: not measured). What would have to change in the
  shared shaders to try it: `mediump` (or `RelaxedPrecision`) on the accumulators of `film_mix3.comp` MODE 1/2 and `film_post.comp` MODE 3/4/6 only, behind a spec constant, parity re-run (dE budget).
* **Raster / fragment path for the dominant blur**: a render pass with tile memory helps when a pass is bandwidth-bound between dependent passes (tile shading keeps intermediates on chip); here the cost is
  per-pass latency of the 2R+1 taps, which a fragment shader would not change. The measured alternatives that do help: bigger workgroups (already 16x16 = 256), the staged tile (above), and two output pixels per thread in `film_v3`
  (not tried: shared-shader change). Not implemented, with these numbers there is no case for a render-pass variant on Apple GPUs.
* **Host side**: wall time of the 1600 px final frame is 44-54 ms against 27-33 ms GPU: ~20 ms are outside the GPU (upload of tables, plan, readback + RGBA16F -> RGB8 conversion, completion latency 0.4 ms measured with
  `DAPPLE_METAL_TRACE`); `commit -> kernelStart` is 0.03 ms, so the queue is not the cause. Worth a CPU profile of `renderFull` (not done).

## Results on the M1 (MacBook Pro, 16 GB, macOS 14.7.6, Release)

All suites run with the same budgets as Vulkan / GLES (the tests assert them): `gpu_parity` mean dE2000 0.036 - 0.079, p99 0.58 - 0.88, max <= 1.9; seams: 0 differing pixels in every scenario and tile size;
`--coverage` 192 / 192 on the GPU executor; `engine.gpu_{hal,box,sched,select,probe}`, `film_gpu.parity`, `film_gpu.frames`, `metal.caps`, `metal.external`; `engine.gpu_external` (Vulkan handles) is disabled on Apple.

| Case (`gpu_tests --film-timing [24mp]`, wall incl. readback) | M1 Metal | desktop Vulkan RTX (pipeline-architecture.md 23.3) |
|---|---|---|
| 1600 px film final frame, strength 100 | 44.5 - 49 ms (GPU 32.9) | 15.5 - 16.6 ms |
| 1600 px, curves + look LUT | 48.3 - 48.8 ms | 16.1 - 16.8 ms |
| 24 MP export (6000x4000, tiled), strength 100 | 657 ms (first 713) | 134 ms (222) |
| 24 MP export, strength 60 + curves + LUT | 682 ms (first 716) | 139 ms (175 - 203) |
| film interactive, 1080 long edge, medians | still 7.2 ms, strength / exposure / halation drag 7.8 - 8.0 ms, grain drag 10.8 ms, warmth drag 21.9 ms | not recorded |
