# Zero-copy display (stage d, display engineer)

Plan of record: pipeline-architecture.md section 9 / 10.5 / 16 (readback is 10-14 ms per 1.7 MP frame on the Pixel 5).

## Chosen variant: Vulkan swapchain on the SurfaceProducer Surface

Kotlin `DappleViewPlugin` (channel `dapple/view`) creates a `TextureRegistry.SurfaceProducer`; `Surface` -> `ANativeWindow` ->
`VK_KHR_android_surface` -> swapchain (`core/engine/present/vk_presenter.cpp`, libvulkan dlopen'ed, private function table so it
coexists with core/gpu's volk). Frame = host RGBA8 -> staging (write-combined) -> GPU copy -> blit (linear) to the swapchain
image -> present (FIFO). Spike on the emulator (SwiftShader/gfxstream, Android 15): device creation, swapchain 1600x900
R8G8B8A8_UNORM with 10 images, TRANSFER_DST usage supported, frames shown by `Texture`, resize (rotation) re-creates the
swapchain. The AHardwareBuffer ring was not needed (the swapchain images are the texture's buffers); it stays the documented
fallback, together with the permanent readback + `decodeImageFromPixels` path (any create/attach/present failure ends there:
`EditorEngine._disableGpu`).

Current input is host RGBA8 from `dapple_session_render_frame` (CPU executor or GPU readback). Zero READBACK needs the GPU
executor to render into a VkImage on the presenter's device: seam = `Presenter` gets `presentImage(VkImage...)` plus an
adopted-device mode; the HAL must enable `VK_KHR_swapchain` / `VK_KHR_android_surface` (or hand its device handles over).
Not done here (hal/** is the GPU engineer's); only the readback-avoiding display plumbing, loop and lifecycle are.

## Zero-readback seam: landed in the GPU executor (GPU executor engineer, 2026-11-12)

The HAL and the executor side of the seam above exist (`core/engine/hal/hal.h`, `vk/vk_device.cpp`, `exec/gpu_executor.cpp`):

1. **Enable the extensions before the first GPU frame**: `dapple::engine::gpu::setPresentExtensions(1)` (`exec/gpu_runtime.h`; or env `DAPPLE_GPU_PRESENT=1`).
   The Vulkan HAL then enables `VK_KHR_surface` + `VK_KHR_android_surface` (`VK_KHR_win32_surface` on Windows) on the instance and
   `VK_KHR_swapchain` on the device when the implementation offers them, and picks the universal graphics + compute queue family (a
   compute-only family cannot present; on the RTX 4060 Ti family 0 instead of 2). `caps.surfacePresent` / `NativeHandles::swapchainEnabled`
   say what was enabled.
2. **Adopt the device**: `dapple::engine::gpuProvider()->nativeHandles(&h)` fills `hal::NativeHandles { instance, physicalDevice, device, queue,
   queueFamily, getInstanceProcAddr, swapchainEnabled }` (null / false on GLES). Create the surface, swapchain, semaphores and
   the images on THIS `VkDevice` (images need `VK_IMAGE_USAGE_TRANSFER_DST_BIT`, format `R8G8B8A8_UNORM`, same size as the frame). The queue
   is the HAL's: use it from the thread that renders (the render loop already renders and presents on one thread); the executor never
   touches it outside `EngineSession::renderFrame`.
3. **Render into the acquired image**: fill `hal::ExternalImage { handle = VkImage, fmt = RGBA8, w, h, layoutIn, layoutOut, waitSemaphore,
   signalSemaphore }` (layouts are `VkImageLayout` values: `layoutIn` = what the swapchain image is in after acquire, normally UNDEFINED = 0;
   `layoutOut` = PRESENT_SRC_KHR = 1000001002; `waitSemaphore` = the acquire semaphore, `signalSemaphore` = the semaphore
   `vkQueuePresentKHR` waits on) and set `FrameJob::gpuTarget = &ext` before `EngineSession::renderFrame` (the C layer adds the field to
   its frame request). On success `FrameResult::external == true`, `rgba == nullptr`, `w` / `h` are the frame size, and the executor does
   NOT wait for the GPU: the semaphores order the copy, the present waits on `signalSemaphore`. When the GPU cannot take the frame (CPU
   fallback, film, device lost) `rgba` holds the CPU pixels and `external` is false: upload them as today.
4. The final pass of the frame is the same RGBA8 image as in the readback path; the copy into the external image is `vkCmdCopyImage`
   between the two barriers (`layoutIn` -> TRANSFER_DST -> `layoutOut`). The wait semaphore is attached to the submission that
   contains the copy (the last one), the signal semaphore to the same submission.

Measured on the desktop (`dapple_gpu_engine_tests external`, RTX 4060 Ti and SwiftShader): native handles come back, an image created on
the HAL device stands in for the swapchain image, two raw Vulkan semaphores (acquire signalled by an empty submission, render-finished
waited by another) order the frame, and the external image is bit-identical to the readback frame (0 of 307200 bytes differ). NOT
verified: a real swapchain / surface (no display on the test host and none attempted on a device), `VK_KHR_android_surface` on the emulator
or the phones, queue-family ownership transfer (the HAL queue family must be the presenting one; a device whose presenting family differs from
the universal one is not handled), present on a thread other than the rendering thread.

## Pieces
- `core/include/dapple/dapple_view.h`: `dapple_view_create/attach_window/detach_window/resize/bind_session/submit/set_callback/
  set_rate_divisor/set_paused/trim/take_analysis/info_json/destroy`.
- `render_loop.*`: one thread, one-slot mailbox (latest wins, overwritten snapshots counted as dropped), AChoreographer
  (ALooper + eventfd, zero wakeups when idle), timer grid elsewhere; pause, quiesce, rate divisor.
- `view.cpp`: loop + presenter + session; renders on the loop thread through the public C API (session mutex serialises with
  Dart-side calls), events to Dart via `NativeCallable.listener`, analysis via `take_analysis`, thermal headroom polled every 4 s
  while rendering (>= 1.0 -> 30 fps cap), ADPF reporting stays inside `dapple_session_render_frame`.
- Dart: `app/lib/services/gpu_view_backend.dart` (new), `EditorEngine` switch (after the first readback frame of a photo, drag and
  final frames go to the view; texture follows the frame size/aspect), `editor_page.dart` shows `Texture` while live.
- Lifecycle: SurfaceProducer `onSurfaceCleanup` detaches synchronously, `onSurfaceAvailable` re-attaches (native resubmits the newest
  snapshot); `onTrimMemory` -> `dapple_view_trim` (0 buffers, 1 + swapchain, 2 + device, rebuilt lazily); Dart lifecycle pauses the
  loop when not resumed; session is unbound before it can be closed (`attach`, `dispose`); device lost -> presenter rebuilds.

## Tests
- `dapple_present_tests` (ctest `present.loop_view`): latest wins, idle = zero wakeups/renders, <= 1 frame per vsync period,
  pause/quiesce, view attach/detach/re-attach re-renders the newest snapshot, analysis once, trim, 1.5 s multi-thread stress
  (submit/detach/attach/trim/pause), unbind then session close, destroy with a pending snapshot.
- Emulator (x86_64, EMU_PORT 5554): `build/emu/zc_*.png`.

## Zero-readback display: landed (display engineer, 2026-11-12)

Interactive frames now go GPU executor -> presenter image on the SAME VkDevice -> swapchain; no host readback, no staging upload.

- **One device**: on Android the HAL enables the surface/swapchain extensions by default (`DAPPLE_GPU_PRESENT=0` turns them off; gpu_context.cpp). `GpuProvider::lease()` (gpu_iface.h) hands the
  presenter {native handles, a keep-alive of the Context, the context mutex that serialises the shared VkQueue, a generation}. `VkPresenter` adopts instance/device/queue
  (`DAPPLE_PRESENT_ADOPT=0` forces the old private device). Every use of the queue (submit, present, waitIdle) takes the context mutex, which the executor holds for a whole frame.
- **Image on the engine device**: `Presenter::beginExternal(w,h)` returns the slot's `ExternalTarget` (ring of 2 R8G8B8A8 TRANSFER_SRC|DST images, grown on demand, size estimated from the last
  frame scaled by the long-edge ratio +3%). The view passes it as `DappleFrameRequest::external` (`DappleExternalImage`, dapple_c.h). The executor copies its final pass into it (layoutIn UNDEFINED ->
  TRANSFER_SRC) and does not wait. `presentExternal(w,h)` records barrier + scaled blit into the acquired swapchain image and presents. No semaphores between the two: same queue, the barrier at the start of the
  presenter's submission covers the executor's earlier submission (submission order).
- **Result contract**: `rc == OK, rgba == NULL, width/height > 0` = the picture is in the image. Anything else returns pixels as before and the view uses `present(rgba)` (CPU executor, film frames today,
  GLES, image too small, executor refused): `executor` falls back to readback when `!caps.externalImageWrite` or the frame does not fit (gpu_executor.cpp).
- **Follows the engine device** every frame (`syncLease`): rebuild or Vulkan->GLES demotion drops everything built on the old device (kept alive by the lease) and rebuilds on the new device or a private one; a
  device whose queue family cannot present to the surface falls back to a private device.
- **Lifecycle** unchanged on the Kotlin side (SurfaceProducer cleanup/available, setSize re-attach, trim): detach/attach rebuild the swapchain on the shared device; trim 0 also frees the external images.
- **Counters**: `dapple_view_info_json().zeroReadback` = {externalFrames, hostFrames, hostBytes, tooSmall, refused, emaFenceMs}; logcat tag `dapple_view` prints one line per 30 frames. Test double:
  `DAPPLE_VIEW_NULL_EXTERNAL` (present.loop_view covers the fallback accounting).

### Measured (Android x86_64 emulator, `-gpu host`: NVIDIA RTX 4060 Ti through gfxstream, Vulkan 1.3; debug APK)
Exposure/contrast drags by `adb input swipe` on a 24 MP-class sample, interactive 360x480 frames, 960x1280 finals: 418 shown frames, **418-2 = 416 external, 2 host** (the first frame has no size history; one
final frame was larger than the estimate), `hostBytes` constant at 4.19 MB after the first frames (no readback / upload during drags). Per frame: engine render call 70-81 ms (CPU side: record + submit through
gfxstream, includes the per-frame table work; the GPU time itself is not separated), present (acquire + blit + submit) 0.7-1.8 ms, slot fence wait 0.06-0.3 ms, latency 85-140 ms. Screenshots
`build/emu/zr_05..14*.png` (drag overlay, resume, landscape, portrait) show correct pictures. Background/foreground re-created the swapchain (9 images) and kept running; rotation to landscape and back kept
the texture correct (no swapchain change: the SurfaceProducer size follows the frame, Flutter scales the texture).

### NOT verified
SwiftShader (the HAL refuses software devices unless `DAPPLE_GPU_SOFTWARE=1`, which an app cannot get from the shell here), real phones, the N5 inner/outer fold, `am send-trim-memory` (refused for a foreground
process: native trim levels only covered by the desktop null-presenter test), device loss / GLES demotion while a view is attached, two views at once, cross-family queue ownership, the GPU time of a frame.

### On-device checklist (Pixel 5 / N5)
1. `setprop debug.dapple.gpu 1`, open a photo, drag a slider: logcat `dapple_present: device ... (shared with the engine)` and `dapple_view: ... ext N host 1-2 tooSmall <=2`; `hostBytes` must stop growing.
2. Adreno/Mali: present extensions on the HAL instance, queue family 0 presents (log says "private device" if not); swapchain format R8G8B8A8 or B8G8R8A8 (BGRA blit target is OK, RGBA external image is blit-converted).
3. 24 MP source, drag then release: final frame size estimate (`tooSmall` counts the misses), no frame > ~1 s per submission.
4. N5: unfold/fold while a view is attached and dragging (Flutter resizes the texture; expect a swapchain re-create log, no stall), rotate, home -> return, split screen/multi-window (second view = second presenter on the same device).
5. Trim: background the app, `adb shell am send-trim-memory <pkg> BACKGROUND` (while backgrounded); return and drag: view must rebuild lazily.
6. Kill the GPU context (injectFailure / `debug.dapple.gpu 0`) mid-drag: frames continue on the host path.
7. Compare `render`/`present` ms against the readback path (DAPPLE_PRESENT_ADOPT=0 and `DAPPLE_GPU_VIEW=0`).
