/* Dapple zero-copy display API (stage d, docs/engine/present.md). Additive: nothing in dapple_c.h changes.
 *
 * A DappleView is one editor picture on screen. It owns a render loop thread (one-slot mailbox, latest wins,
 * vsync-aligned, idle = no work) and a presenter that draws finished frames straight onto a window (the Surface of a
 * Flutter SurfaceProducer texture) through a Vulkan swapchain. The pixels never visit Dart: Dart submits parameter
 * snapshots and receives small "frame shown" events.
 *
 * Threading: every function may be called from any thread. attach/detach/bind/trim/resize/destroy block until the
 * frame in flight (if any) has finished, so the resources they release are never used afterwards. A frame never blocks
 * longer than ~100 ms on a stalled consumer.
 * Lifetime: the bound DappleSession must outlive its binding: call dapple_view_bind_session(view, NULL) BEFORE closing it.
 */
#ifndef DAPPLE_VIEW_H
#define DAPPLE_VIEW_H

#include "dapple_c.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DappleView DappleView;

enum {
  DAPPLE_VIEW_TIMER_LOOP = 1,    /* frame pacing by a fixed-rate timer grid instead of Choreographer (tests / desktop) */
  DAPPLE_VIEW_NULL_PRESENTER = 2,/* count frames instead of drawing them (tests / desktop) */
  DAPPLE_VIEW_NULL_EXTERNAL = 4  /* with NULL_PRESENTER: pretend to offer zero-readback images (tests of the fallback) */
};

typedef struct DappleViewOptions {
  int32_t size; /* sizeof(DappleViewOptions) */
  int32_t flags;
} DappleViewOptions;

enum {
  DAPPLE_VIEW_SHOWN = 0,         /* frame rendered and queued to the window */
  DAPPLE_VIEW_RENDER_FAILED = 1, /* the session could not render this snapshot */
  DAPPLE_VIEW_PRESENT_FAILED = 2,/* rendered but not shown (surface lost, device lost, consumer stalled): the Dart side falls back */
  DAPPLE_VIEW_NO_TARGET = 3      /* no window or no session at that moment: the snapshot is kept and re-rendered on attach */
};

/* Called on the loop thread after every frame, never while a view lock is held. Must return quickly (post to Dart).
 * latency_ms: submit() -> frame queued to the display. */
typedef void (*DappleViewFrameFn)(void* user, int64_t seq, int32_t status, int32_t width, int32_t height, double render_ms,
                                  double present_ms, double latency_ms);

/* NULL when the presenter cannot be created on this device; the caller keeps the readback path. */
DAPPLE_API DappleView* dapple_view_create(const DappleViewOptions* options);
DAPPLE_API void dapple_view_destroy(DappleView* view);

/* native_window: an ANativeWindow* (the view takes its own reference). width/height: surface size in pixels.
 * DAPPLE_OK, or DAPPLE_ERROR with dapple_last_error() describing why (the caller then uses the readback path).
 * A repeat call replaces the window (surface recreation, resize by the SurfaceProducer). */
DAPPLE_API int32_t dapple_view_attach_window(DappleView* view, void* native_window, int32_t width, int32_t height);
/* Releases the window and the swapchain (background / surface cleanup). The last snapshot is kept. */
DAPPLE_API void dapple_view_detach_window(DappleView* view);
/* The surface changed size without a new window object. */
DAPPLE_API int32_t dapple_view_resize(DappleView* view, int32_t width, int32_t height);

/* The session frames are rendered from (NULL = none). Blocks until the frame in flight finished. */
DAPPLE_API void dapple_view_bind_session(DappleView* view, DappleSession* session);

/* Latest wins: replaces the pending snapshot. Returns its sequence number (> 0). request may be NULL (defaults);
 * DAPPLE_FRAME_ANALYSIS in request->flags stores the analysis JSON for dapple_view_take_analysis. */
DAPPLE_API int64_t dapple_view_submit(DappleView* view, const char* params_json, const char* lut_path, const DappleFrameRequest* request);

DAPPLE_API void dapple_view_set_callback(DappleView* view, DappleViewFrameFn fn, void* user);

/* Quality / power policy: 0 = full rate; 1 = every second vsync (30 fps cap); higher = more. Applied by the loop. */
DAPPLE_API void dapple_view_set_rate_divisor(DappleView* view, int32_t divisor);
/* Stops / resumes rendering (the latest snapshot is kept while paused). */
DAPPLE_API void dapple_view_set_paused(DappleView* view, int32_t paused);
/* Memory trim: 0 transient buffers, 1 + swapchain, 2 + device. Rebuilt lazily by the next frame. */
DAPPLE_API void dapple_view_trim(DappleView* view, int32_t level);

/* Analysis JSON of the newest frame that had one (once; NULL when none). Release with dapple_free_string. */
/* Why the last create/attach on the calling thread failed. */
DAPPLE_API const char* dapple_view_last_error(void);

DAPPLE_API char* dapple_view_take_analysis(DappleView* view);
/* {"variant":..,"device":..,"loop":{submits,renders,dropped,wakeups,vsyncs,...},"thermal":..,...}; free with dapple_free_string. */
DAPPLE_API char* dapple_view_info_json(DappleView* view);

#ifdef __cplusplus
}
#endif
#endif
