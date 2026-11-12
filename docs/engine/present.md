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
