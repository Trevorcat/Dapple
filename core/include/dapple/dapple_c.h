/* Dapple C API — the stable boundary used by the Flutter app (dart:ffi) and
 * any other binding. All strings are UTF-8. All functions are thread-safe
 * with respect to *different* sessions; a single session must not be used
 * from two threads at once.
 *
 * Edit parameters cross this boundary as JSON using exactly the schema
 * produced by dapple::toJson(EditParams) (see edit_params.h). Unknown keys
 * are ignored, missing keys mean "no change".
 */
#ifndef DAPPLE_C_H
#define DAPPLE_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define DAPPLE_API __declspec(dllexport)
#else
#define DAPPLE_API __attribute__((visibility("default")))
#endif

typedef struct DappleSession DappleSession;

/* RGBA8888, row-major, stride == width * 4. Owned by the library. */
typedef struct DappleImage {
  int32_t width;
  int32_t height;
  uint8_t* rgba;
} DappleImage;

enum {
  DAPPLE_OK = 0,
  DAPPLE_ERROR = 1,
};

enum {
  DAPPLE_QUALITY_PREVIEW = 0, /* half-size RAW decode, fast */
  DAPPLE_QUALITY_FULL = 1,    /* full resolution demosaic */
};

/* Library version string, e.g. "0.1.0". Static storage. */
DAPPLE_API const char* dapple_version(void);

/* Last error message for the calling thread (static/thread-local storage,
 * valid until the next call on the same thread). Never NULL. */
DAPPLE_API const char* dapple_last_error(void);

/* Frees strings returned by this API. NULL is allowed. */
DAPPLE_API void dapple_free_string(char* s);

/* Frees pixel memory of an image filled by this API and zeroes the struct. */
DAPPLE_API void dapple_free_image(DappleImage* image);

/* The JPEG preview embedded in a RAW file (what the camera's own engine produced), upright and
 * scaled to long_edge (0 = native size). Much faster than a session; for instant placeholders.
 * Fills *out (release with dapple_free_image). Returns DAPPLE_ERROR if the file has none. */
DAPPLE_API int32_t dapple_extract_preview(const char* path, int32_t long_edge, DappleImage* out);
/* Embeds a JPEG file as the preview (reduced-resolution SubIFD) of a DNG that has none, in place (core/src/dng_thumbnail.h). The exposure anchor of the
 * file (tier 1, the camera rendering of this frame) reads it afterwards, also when the DNG is copied elsewhere. DAPPLE_OK on success; an error when
 * the DNG already has SubIFDs, is not a classic TIFF, or the JPEG is unreadable. */
DAPPLE_API int32_t dapple_dng_embed_thumbnail(const char* dng_path, const char* jpeg_path);

/* Metadata of a file without decoding pixels. Returns JSON or NULL:
 * {"make","model","lens","iso","shutter","aperture","focalLength","timestamp",
 *  "width","height","cfa","rawFormat","inCameraLook",
 *  "camera":{"id","displayName","ftp","usbPtp","vendorAppAutoSend","vendorApp"}} */
DAPPLE_API char* dapple_probe_json(const char* path);

/* Lightroom/Camera Raw .xmp preset -> {"name": "...", "params": {...EditParams...},
 * "unsupported": ["..."]}. NULL on error. */
DAPPLE_API char* dapple_xmp_to_json(const char* xmp_path);

/* True (1) if the extension is a RAW or image format Dapple can decode. */
DAPPLE_API int32_t dapple_is_supported_file(const char* path);

/* ---- Catalogues (no session needed) ---- */

/* The nine colour styles: {"looks":[{"id","name","tags","category","padX","padY"},...]}.
 * Use the id in the "look" key of the edit params; padX/padY is the style's default colour-pad
 * point. Free with dapple_free_string. */
DAPPLE_API char* dapple_looks_json(void);

/* Every numeric edit key with range and default:
 * {"sliders":[{"key","min","max","default","group"},...],"crop":{...},"rot":{...},"flip":{...},
 *  "palette":{"min","max"},"look":{"default"}}. These are exactly the keys the params JSON accepts
 * (docs/design/requirements.md 6.3); "exposure" is in UI units (EV * 20). */
DAPPLE_API char* dapple_params_schema_json(void);

/* ---- Film simulation (docs/film/README.md) ------------------------------------------------------------------
 * The film profiles ship as one binary pack (core/data/film/dapple.dfp, an Android asset). Load it once at start
 * (or let the CLI / tests find it); the edit keys "film", "film_paper", "film_strength", ... then work. Without a
 * pack a "film" key is ignored and the normal pipeline renders. The profile data is by Andrea Volpato
 * (spektrafilm), CC BY-SA 4.0: the app's About screen must show it (dapple_film_attribution). */

/* Loads the pack from a .dfp file (or a directory of profile JSON files, slow) and installs it process-wide. */
DAPPLE_API int32_t dapple_film_load_pack(const char* path);
/* Same from memory (an asset read into a buffer); the data is copied. */
DAPPLE_API int32_t dapple_film_load_pack_memory(const void* data, size_t size);
/* Records the path only; the pack is loaded on first use (keeps app start fast). */
DAPPLE_API void dapple_film_set_pack_path(const char* path);
/* {"loaded":bool,"stocks":[{"id","name","type":"negative"|"slide"|"paper"},...]}. "negative" and "slide" are camera
 * films ("film" key); "paper" are print papers / print films ("film_paper" key, or "none" to scan the film).
 * Free with dapple_free_string. */
DAPPLE_API char* dapple_film_list_json(void);
/* The licence line for the About screen ("" when no pack is loaded). Free with dapple_free_string. */
DAPPLE_API char* dapple_film_attribution(void);
/* {"<stock id>":["rrggbb","rrggbb","rrggbb"],...} for every camera film (not papers): the stock's rendering of three
 * fixed test patches (18 % grey, skin, sky) with its default paper. Deterministic for a pack; cache it. Free with
 * dapple_free_string. "{}" without a pack. */
DAPPLE_API char* dapple_film_swatches_json(void);
/* Same shape, but the three patches are skin, sky and a deep shadow (the grey of dapple_film_swatches_json hardly differs between
 * stocks; the shadow shows each stock's toe, cast and black level). Preferred by the UI. NULL on failure. */
DAPPLE_API char* dapple_film_swatch_bands_json(void);

/* ---- Previews for pickers ---- */

/* Film control pad labels for the current photo and film (docs/engine/pipeline-architecture.md section 18): JSON
 *   {"ok":true,"shared_ev":..,"final_ev":..,"fraction":..,"shoot_ev":[-1,0,+1],"print_l":[..],"black_l":[..],"latitude":[..],"white_l":..}
 * with the actual film EV at film_shoot -1/0/+1, the print's median L* at film_print -1/0/+1, the paper black L* at film_black
 * -1/0/+1 and the in-range fraction at film_latitude -1/0/+1,
 * plus "scene_cct" / "reference_cct" (K; 0 = unknown): the correlated colour temperature of the scene illuminant as assumed by the current
 * white balance (from the RAW's as-shot white balance) and of the stock's own reference illuminant, for labelling the film_wb axis, and
 * "effective_lo" / "effective_hi": {"film_shoot","film_print","film_black","film_wb","film_latitude"} booleans, true when that end of the axis
 * actually changes the render for this photo and film (a slide's film_print +1 does not; film_wb +1 does nothing when the scene light equals
 * the stock's reference light or the scene illuminant is unknown). {"ok":false} without an active film or with a legacy
 * DAPPLE_FILM_MATCH_* mode. To preview a pad position render the 256 px proxy with params_json carrying the offsets
 * (film_shoot, film_print, film_black, film_latitude in -1..1, film_wb in 0..1): dapple_session_render with out_long_edge 256 (the match is cached
 * per key, the print exposure of a moved film_print costs a few proxy renders). Free with dapple_free_string. */
DAPPLE_API char* dapple_session_film_pad_json(DappleSession* session, const char* params_json);

/* Renders the photo with each of the nine styles (order of dapple_looks_json, default colour pad,
 * strength 100) on top of `params_json` (its own look / palette / lookStrength are replaced).
 * Fills out[0..8]; release each with dapple_free_image. On failure none is allocated.
 * Intended for thumbnails (out_long_edge e.g. 256). */
DAPPLE_API int32_t dapple_session_render_look_previews(DappleSession* session, const char* params_json,
                                                       int32_t out_long_edge, DappleImage* out);

/* Renders the photo moved towards each of the nine tonal classes (order of toneClasses() in
 * tone.h: low-short, low-mid, low-long, mid-short, mid-mid, mid-long, high-short, high-mid, high-long) on
 * top of `params_json`. These are previews meant to show the class difference clearly: the move
 * runs at strength 100 with a wide reach (a real edit with toneTarget is gentler and scaled by
 * toneStrength). strength < 0 = 100; 0..100 overrides. Fills out[0..8]; release each with
 * dapple_free_image. */
DAPPLE_API int32_t dapple_session_render_tone_previews(DappleSession* session, const char* params_json,
                                                       int32_t out_long_edge, float strength, DappleImage* out);

/* ---- Sessions: decode once, render many times (slider dragging) ---- */

/* Decodes `path`. long_edge > 0 downsizes the working image once after
 * decoding (e.g. 2048 for on-screen editing). NULL on error. */
DAPPLE_API DappleSession* dapple_session_open(const char* path, int32_t quality, int32_t long_edge);
/* A rectangle (x0, y0, x1, y1) of the oriented, cropped full-resolution picture of a RAW file at 1:1 (pixels of the picture dapple_session_info_json
 * reports as sensorWidth x sensorHeight), demosaiced from the cached mosaic (GPU when enabled). The session is an ordinary one (render, export) whose
 * source is that rectangle, with the exposure anchor of the whole picture. NULL (dapple_last_error says why) for files without a mosaic path (JPEG,
 * sRAW, linear DNG ...) and lens-warped DNGs: use dapple_session_open then. Additive. */
DAPPLE_API DappleSession* dapple_session_open_region(const char* path, int32_t x0, int32_t y0, int32_t x1, int32_t y1);
/* Cancels the region open in flight (a newer dapple_session_open_region does the same): the cancelled call returns NULL ("region open: cancelled"). */
DAPPLE_API void dapple_region_cancel(void);
DAPPLE_API void dapple_session_close(DappleSession* session);

/* Metadata JSON (same schema as dapple_probe_json) of the opened file. */
DAPPLE_API char* dapple_session_info_json(DappleSession* session);

/* Automatic per-photo correction. Returns JSON:
 * {"correction": {...EditParams...}, "stats": {...}, "notes": ["..."]}.
 * strength in [0,1]. */
DAPPLE_API char* dapple_session_auto_json(DappleSession* session, float strength);

/* Tonal analysis of the photo as rendered (camera look, no edits):
 * {"histogram":[64 fractions over encoded luminance 0..1],"p5":..,"p50":..,"p95":..,
 *  "nearest":"low-long","key":"low|mid|high","range":"short|mid|long",
 *  "distance":..,"margin":..}
 * nearest is one of the nine classes key{high,mid,low} x range{short,mid,long}; it is what an
 * empty "toneTarget" in the edit params refers to. NULL on error. */
DAPPLE_API char* dapple_session_analyze_json(DappleSession* session);

/* Same, for the photo rendered with `params_json` (EditParams JSON; NULL/"" = untouched). */
DAPPLE_API char* dapple_session_analyze_params_json(DappleSession* session, const char* params_json);

/* Renders with params (JSON, may be NULL/"" for defaults) and an optional
 * .cube LUT path (NULL/"" for none). out_long_edge > 0 limits output size.
 * Fills *out; release with dapple_free_image. */
DAPPLE_API int32_t dapple_session_render(DappleSession* session, const char* params_json, const char* lut_path,
                                         int32_t out_long_edge, DappleImage* out);

/* ---- Interactive frames (slider dragging, zoom) ----------------------------------------------
 * One call = one frame: the image and the tonal analysis of the same render. Point-wise stages run
 * through a lattice that is baked once per parameter change and cached in the session; scaled copies
 * of the source are cached too, so a frame costs one pass over the pixels plus the neighbourhood
 * stages. A session must still be used by one thread at a time. */
enum {
  DAPPLE_FRAME_INTERACTIVE = 1, /* drag quality: no grain; noise reduction only when its sliders moved */
  DAPPLE_FRAME_ANALYSIS = 2,    /* also return the tonal analysis JSON (same schema as analyze_json) */
  DAPPLE_FRAME_EXACT = 4,       /* reference pipeline (no lattice), for tests and export parity */
  DAPPLE_FRAME_BACKGROUND = 8,  /* low priority: calling thread on efficiency cores, nice 10, never uses the pool */
};

/* Zero-readback display (docs/engine/present.md): an image on the GPU device the engine renders with (see the view API), into which
 * the final 8-bit picture is copied instead of being read back. Vulkan: image = VkImage (R8G8B8A8_UNORM, TRANSFER_DST), layouts are
 * VkImageLayout values, semaphores VkSemaphore handles (0 = none). width/height = the capacity of the image. */
typedef struct DappleExternalImage {
  void* image;
  uint32_t width, height;
  uint32_t layout_in, layout_out;
  uint64_t wait_semaphore, signal_semaphore;
} DappleExternalImage;

typedef struct DappleFrameRequest {
  int32_t size;             /* sizeof(DappleFrameRequest) (versioning) */
  int32_t long_edge;        /* long edge of the whole picture, 0 = session source size */
  int32_t flags;            /* DAPPLE_FRAME_* */
  int32_t bake_size;        /* lattice size 17..65; 0 = 57 interactive / 65 otherwise (a cached finer lattice of the same colour stage is reused) */
  float region[4];          /* x0,y0,x1,y1 normalised in the output picture; all 0 = whole picture */
  int32_t region_long_edge; /* long edge of the region image (only with a region) */
  /* ---- additive: honoured only when `size` covers it (older callers pass a shorter struct) ---- */
  const char* active_param; /* key of the slider being dragged ("grain", "film_grain", "exposure"...), NULL / "" = none. With
                               DAPPLE_FRAME_INTERACTIVE, grain stays on while a grain-family slider is dragged
                               (docs/engine/pipeline-architecture.md 14, Q8) */
  const DappleExternalImage* external; /* NULL = pixels come back in *out (default). Set: when the GPU executor takes the frame the
                               picture is in this image and *out has width/height with rgba == NULL (DAPPLE_OK); when it does not
                               (CPU frame, image too small, GLES) *out holds the pixels as usual. */
} DappleFrameRequest;

/* Fills *out (release with dapple_free_image). *analysis_json (may be NULL) receives a string to
 * release with dapple_free_string when DAPPLE_FRAME_ANALYSIS is set, else NULL. */
DAPPLE_API int32_t dapple_session_render_frame(DappleSession* session, const char* params_json, const char* lut_path,
                                               const DappleFrameRequest* request, DappleImage* out,
                                               char** analysis_json);

/* Latest-wins cancellation (DAPPLE_ENGINE=full): makes a render / export in flight on this session return early with
 * DAPPLE_ERROR and dapple_last_error() == "cancelled". Lock-free, callable from any thread; a no-op when nothing runs. */
DAPPLE_API int32_t dapple_session_cancel(DappleSession* session);

/* ---- Scheduling / power (all optional, safe no-ops where the platform lacks the feature) ------------------
 * Foreground frames run on one dedicated render thread, pinned to the performance cores on big.LITTLE, and are
 * reported to Android's ADPF (APerformanceHint) when available. Idle workers block (no spinning). */
/* thread_limit: max threads (caller included) per job, 0 = all; target_frame_ms: ADPF target work duration of an
 * interactive frame, 0 = off. */
DAPPLE_API void dapple_perf_configure(int32_t thread_limit, int32_t target_frame_ms);
/* Marks the CALLING thread as background (nice 10, efficiency cores, pool bypassed) until called with 0. Use it
 * around open/preview calls that must not compete with interactive frames (prefetch, thumbnails). */
DAPPLE_API void dapple_thread_background(int32_t on);
/* AThermal headroom forecast (0 = cool, 1 = throttling threshold), < 0 when unavailable. */
DAPPLE_API float dapple_thermal_headroom(int32_t forecast_seconds);
/* {"cpus":N,"perf":N,"efficient":N,"pool":N,"adpf":bool,"thermal_status":N}; free with dapple_free_string. */
DAPPLE_API char* dapple_perf_info_json(void);

/* Wave 8: film tables the drag needs are baked on a background worker; while one is pending a frame shows the previous step's table. The callback fires (on a
 * worker thread, keep it short) once the newest requested table has landed and frames since then were stale: render the last frame again (a view does it itself).
 * Returns 0 on success. Pass fn = NULL to remove the callback registered with the same user pointer. */
typedef void (*DappleRepaintFn)(void* user);
DAPPLE_API int32_t dapple_set_repaint_callback(DappleRepaintFn fn, void* user);

/* Wave 8: call on pointer-down in the editor, before the first parameter change. Renders the newest interactive frame again for at most burst_ms (150 is
 * a good value) on a background thread so the GPU clock that fell during a pause is up for the first real frame; ends early when a real frame is requested.
 * Costs power only while a finger is down. No-op when no interactive frame was rendered yet. */
DAPPLE_API int32_t dapple_session_touch_hint(DappleSession* session, int32_t burst_ms);

/* Frame engine diagnostics (DAPPLE_ENGINE: default full; off = the old Renderer path, golden oracle / A/B): {"engine":"off|full","frames":N,
 * "fullHits":N,"latticeReuse":N,"latticeBake":N,...}; free with dapple_free_string. Additive. */
DAPPLE_API char* dapple_engine_info_json(void);

/* Same as render, but encodes a JPEG to out_path. */
DAPPLE_API int32_t dapple_session_export_jpeg(DappleSession* session, const char* params_json, const char* lut_path,
                                              int32_t out_long_edge, int32_t jpeg_quality, const char* out_path);

/* ===== HDR merge, panorama stitching, bracket / panorama detection, Motion Photo (phase 3) ===========
 *
 * All options are *flat* JSON objects ({"key": value, ...}); unknown keys are ignored and missing keys take
 * the defaults listed. Every function reports failure through its return value (NULL / DAPPLE_ERROR) and
 * dapple_last_error(); none of them throws across the boundary. Merge work is CPU heavy: call it from a
 * background isolate / thread. A session returned by dapple_session_open_merged is an ordinary session:
 * render / export / analyze / auto work on it exactly as on a decoded RAW, and it is scene-referred
 * (RAW input) so the base tone curve, styles and every edit parameter apply. */

/* Progress callback, called from the merging thread. `fraction` is 0..1, `stage` a stable key
 * ("load","align","deghost","merge","features","match","estimate","seams","blend","crop","detect","done").
 * Return non-zero to cancel; the running call then fails with the error "cancelled". May be NULL. */
typedef int32_t (*DappleProgressFn)(float fraction, const char* stage, void* user);

/* Progress without a native callback (dart:ffi cannot receive callbacks from a worker thread while the isolate is
 * blocked in the call): pass `dapple_progress_to_cell` as the DappleProgressFn and a pointer to a DappleProgressCell
 * as `user`; the library stores the latest fraction and a stage code, and another thread / isolate polls the cell.
 * stage codes: 0 none, 1 load, 2 align, 3 deghost, 4 merge, 5 features, 6 match, 7 estimate, 8 warp, 9 seams,
 * 10 blend, 11 crop, 12 detect, 13 done, 14 other. Never requests cancellation (use cancel_flag). */
typedef struct DappleProgressCell {
  volatile float fraction;
  volatile int32_t stage;
} DappleProgressCell;
DAPPLE_API int32_t dapple_progress_to_cell(float fraction, const char* stage, void* user);

/* 1 if this build can align HDR frames and stitch panoramas (OpenCV linked), else 0. When 0,
 * kind "hdr" still merges (without alignment) and kind "pano" fails with an explanatory error. */
DAPPLE_API int32_t dapple_merge_available(void);

/* Merges `count` files into one session.
 *  kind = "hdr"  (2..9 frames)  options: {"base":-1,"deghost":"mid","align":true,"quality":"full","longEdge":0}
 *    base     frame index that is kept where frames disagree and defines the geometry; -1 = middle exposure
 *    deghost  "off" | "low" | "mid" | "high"
 *    align    false for tripod shots
 *  kind = "pano" (2..20 frames) options: {"projection":"cyl","crop":true,"fill":false,"base":-1,
 *                                          "exposureComp":true,"quality":"full","longEdge":0}
 *    projection "sph" | "cyl" | "persp"; crop = largest inner rectangle; fill = inpaint empty edges (only when
 *    crop is false); base = frame at the projection centre (-1 automatic)
 *  common: "quality":"preview"|"full" (RAW decode quality), "longEdge" N>0 reduces the work to N px
 *    (HDR: every frame; pano: the output) for fast previews. Run again without it for the export.
 * progress / cancel_flag may be NULL. cancel_flag points to an int that another thread may set to non-zero.
 * report_json_out (may be NULL) receives a JSON report (free with dapple_free_string):
 *  hdr:  {"kind":"hdr","base":1,"fusion":false,"maxValue":3.2,"seconds":{"load":..,"align":..,"merge":..},
 *         "frames":[{"path","evExif","evUsed","evFromData","aligned","alignMethod","alignScore","shiftPx","ghostFraction"}],
 *         "warnings":["..."]}
 *  pano: {"kind":"pano","projection":"cyl","framesUsed":4,"used":[0,1,2,3],"canvas":[w,h],"crop":[x,y],
 *         "validFraction":1.0,"seconds":{...},"frames":[{"path","focalPx","yaw","pitch","roll","inliers"}],"warnings":[]}
 * Returns NULL on error (e.g. "Could not align frame 2 ..." -> offer another base frame). */
DAPPLE_API DappleSession* dapple_session_open_merged(const char* const* paths, int32_t count, const char* kind,
                                                     const char* options_json, DappleProgressFn progress, void* user,
                                                     const volatile int32_t* cancel_flag, char** report_json_out);

/* Saves the session's (merged) linear image for archival. format = "dng" (linear DNG, 16 bit, BaselineExposure
 * restores headroom; re-opens in Dapple and in Lightroom / Camera Raw) or "tiff" (16 bit linear ProPhoto RGB
 * TIFF). Files are limited to 4 GB. */
DAPPLE_API int32_t dapple_session_save_linear(DappleSession* session, const char* out_path, const char* format);

/* Finds bracket and panorama candidates in a list of files (any order, any mix of supported formats; unreadable
 * files are skipped and listed). options: {"maxGapSeconds":4,"panoGapSeconds":10,"minEvStep":0.7,
 * "minEvSpread":1.0,"verifyContent":true}. verifyContent reads embedded previews (RAW) to confirm "same scene" for
 * brackets and "partial overlap" for panoramas; it costs ~0.1-0.3 s per candidate frame.
 * Returns JSON: {"frames":[{"index","path","timestamp","ev","iso","shutter","aperture","focalLength",
 *                          "sequence":{"length","number","file","releaseMode"}|null}],
 *               "groups":[{"kind":"bracket"|"panorama","confidence":0..1,"reason":"...","start":ts,"end":ts,
 *                          "frames":[{"index","path","evStep"}]}],
 *               "unreadable":[indices]}
 * index refers to the position in `paths`. NULL only on bad arguments. */
DAPPLE_API char* dapple_detect_sequences_json(const char* const* paths, int32_t count, const char* options_json,
                                              DappleProgressFn progress, void* user, const volatile int32_t* cancel_flag);

/* Video facts for the motion-photo picker: {"duration","width","height","rotation","fps","videoCodec","audioCodec",
 * "keyframes":[seconds...],"streamCopyOk":true}. NULL on error. */
DAPPLE_API char* dapple_video_probe_json(const char* mp4_path);

/* Motion Photo (Google format 1.0: JPEG + XMP Container directory + appended MP4).
 * dapple_motion_photo_from_video cuts a window from an MP4/MOV *without re-encoding* (at keyframes, so the length is
 * about, not exactly, `duration`; PCM audio is dropped) and writes jpeg_path + clip to out_path.
 *   options: {"start":0.0,"duration":3.0,"coverTime":-1,"keepAudio":true,"legacyMicroVideo":false}
 *   coverTime is the source-video time of the frame the still shows (-1 = middle of the clip).
 * dapple_motion_photo_write only builds the container from a JPEG and an already trimmed MP4 (e.g. trimmed on the
 * phone with MediaCodec/MediaMuxer); cover_time_sec is relative to that clip (<0 = unset).
 * Both validate the output before returning. info_json_out (may be NULL) receives
 * {"valid":true,"fileSize","jpegLength","videoOffset","videoLength","presentationTimestampUs","videoMime",
 *  "duration","width","height","videoCodec","audioCodec","problems":[...],"notes":[...],
 *  "clip":{"start","duration","frames","audioKept","coverInClip","note"}} (clip only for from_video).
 * Name the output "*MP.jpg" (spec filename pattern). */
DAPPLE_API int32_t dapple_motion_photo_from_video(const char* video_path, const char* jpeg_path, const char* out_path,
                                                  const char* options_json, char** info_json_out);
DAPPLE_API int32_t dapple_motion_photo_write(const char* jpeg_path, const char* mp4_path, const char* out_path,
                                             double cover_time_sec, char** info_json_out);
/* Checks a file against the spec: same JSON as info_json_out (without "clip"). NULL if the file cannot be read. */
DAPPLE_API char* dapple_motion_photo_validate_json(const char* path);

/* Optional GPU backend (Vulkan / GLES 3.1 compute; only present in builds configured with DAPPLE_GPU).
 * mode: 1 = use the GPU for render_frame / export when available (CPU fallback on any failure), 0 = never,
 * -1 = back to the default (env DAPPLE_GPU=0|1|auto, Android property debug.dapple.gpu=0|1; unset: auto on Android, off elsewhere).
 * Returns the effective state (0 in builds without the backend). */
DAPPLE_API int32_t dapple_gpu_set_enabled(int32_t mode);
/* {"enabled","available","device","deviceKey","selfTest","reason"} (or {"compiled":false,...}); dapple_free_string. */
DAPPLE_API char* dapple_gpu_status_json(void);

/* Default behaviour (Android): the GPU is used once a background probe created a device and it passed the self-test (Vulkan, else GLES 3.1), with no
 * user switch; until then, and when nothing passes, frames render on the CPU. The developer kill switch is the property debug.dapple.gpu=0
 * (debug.dapple.gpu_api=vulkan|gles|cpu forces an API).
 * dapple_gpu_configure: app-private directory for the persisted self-test verdicts and the "self-test in progress" marker (a crash or hang
 * inside the self-test marks that API as failed for this OS build + app version on the next start), the app version and Build.FINGERPRINT they are
 * keyed by (null/empty fingerprint = ro.build.fingerprint). Call before dapple_gpu_warmup; without a directory nothing is persisted.
 * dapple_gpu_warmup: starts the probe on a detached low-priority thread and returns at once (idempotent; no-op when the GPU is off or the build has none). */
DAPPLE_API void dapple_gpu_configure(const char* cache_dir, const char* app_version, const char* fingerprint);
DAPPLE_API void dapple_gpu_warmup(void);

/* Which executor rendered the frames of this process: {"gpuFrames":n,"cpuFrames":n,"deviceLost":n,"demotions":n,"api":"vulkan|gles31|cpu|none",
 * "lastReason":"...","unsupported":{"<reason the GPU did not render a frame>":count,...}}. Free with dapple_free_string. */
DAPPLE_API char* dapple_engine_stats_json(void);

/* ---- Edited Motion Photo clips (docs/engine/pipeline-architecture.md section 30) ----------------------------------------------------
 * Decoded video frames go through the same node graph as the still, with the still's edit (film simulation included). The frames are
 * half-float linear RGBA as the video core delivers them (DV_RGBAF16: BT.709 primaries, 1.0 = SDR white; S-Log3 scene linear), so no
 * 10 to 8 bit step exists before the graph. The film exposure match, tone analysis and frame-global solves are taken once from the
 * reference frame and held for the clip; the grain follows frame_index.
 *   options_json: {"cameraMake","cameraModel","sceneX","sceneY" (as-shot illuminant xy, 0 = unknown),"maxLongEdge","stillAspect",
 *                  "stillLongEdge","allowGpu"}  (all optional)
 *   colour_kind: 0 standard SDR, 1 BT.2020 SDR, 2 HLG, 3 PQ, 4 S-Log3 (DvColorKind).  rotation_deg: container rotation, clockwise.
 *   format: 0 RGBA8 (sRGB), 1 I420 8-bit BT.709 limited, 2 P010 10-bit BT.709 limited (value in the top 10 bits of 16-bit LE words),
 *           3 P010 in the clip's output transfer (HDR: HLG / PQ BT.2020), 4 RGB10A2 packed (input Surface), 5 RGB10A2 in the output transfer.
 *   options also: "hdr":0 forces an SDR output.
 * The first set_reference / render defines the clip's frame size. Frames are rendered one at a time per handle. Errors: dapple_last_error()
 * is not used here; dapple_video_edit_last_error() holds the text for the calling thread. */
typedef struct DappleVideoEdit DappleVideoEdit;
DAPPLE_API DappleVideoEdit* dapple_video_edit_open(const char* params_json, const char* lut_path, const char* options_json);
DAPPLE_API void dapple_video_edit_close(DappleVideoEdit* edit);
DAPPLE_API int32_t dapple_video_edit_set_reference(DappleVideoEdit* edit, const uint16_t* rgba_f16, int32_t width, int32_t height,
                                                   int32_t colour_kind, int32_t rotation_deg);
/* Size of the frames dapple_video_edit_render produces (after the edit's geometry, even). Needs a reference. */
DAPPLE_API int32_t dapple_video_edit_output_size(DappleVideoEdit* edit, int32_t out_wh[2]);
DAPPLE_API int64_t dapple_video_edit_frame_bytes(DappleVideoEdit* edit, int32_t format);
/* frame_index: 0 = like a still, n >= 1 = clip frame n (seeds the film grain). */
DAPPLE_API int32_t dapple_video_edit_render(DappleVideoEdit* edit, const uint16_t* rgba_f16, int32_t width, int32_t height,
                                            int32_t colour_kind, int32_t rotation_deg, uint32_t frame_index, int32_t format, uint8_t* dst,
                                            int64_t dst_size);
/* What the clip is written as: 0 SDR BT.709, 1 HLG BT.2020, 2 PQ BT.2020 (HDR source, edit without film, no geometry edit; docs section 30.8), -1 on error.
 * tags = primaries, transfer, matrix, range (FFmpeg AVCOL_*); note (static string) says why. Format 3 / 5 are then HLG / PQ encoded BT.2020 frames. */
DAPPLE_API int32_t dapple_video_edit_output_kind(DappleVideoEdit* edit, int32_t tags[4], const char** note);
DAPPLE_API const char* dapple_video_edit_last_error(void);

#ifdef __cplusplus
}
#endif

#endif /* DAPPLE_C_H */
