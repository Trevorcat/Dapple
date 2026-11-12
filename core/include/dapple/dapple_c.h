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

/* ---- Previews for pickers ---- */

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

typedef struct DappleFrameRequest {
  int32_t size;             /* sizeof(DappleFrameRequest) (versioning) */
  int32_t long_edge;        /* long edge of the whole picture, 0 = session source size */
  int32_t flags;            /* DAPPLE_FRAME_* */
  int32_t bake_size;        /* lattice size 17..65; 0 = 57 interactive / 65 otherwise (a cached finer lattice of the same colour stage is reused) */
  float region[4];          /* x0,y0,x1,y1 normalised in the output picture; all 0 = whole picture */
  int32_t region_long_edge; /* long edge of the region image (only with a region) */
} DappleFrameRequest;

/* Fills *out (release with dapple_free_image). *analysis_json (may be NULL) receives a string to
 * release with dapple_free_string when DAPPLE_FRAME_ANALYSIS is set, else NULL. */
DAPPLE_API int32_t dapple_session_render_frame(DappleSession* session, const char* params_json, const char* lut_path,
                                               const DappleFrameRequest* request, DappleImage* out,
                                               char** analysis_json);

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

/* Frame engine diagnostics (DAPPLE_ENGINE=1|legacy, default off): {"engine":"off|legacy|tables","frames":N,
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

/* Optional GPU backend (Vulkan compute; only present in builds configured with DAPPLE_GPU, default off at run time).
 * mode: 1 = use the GPU for render_frame / export when available (CPU fallback on any failure), 0 = never,
 * -1 = back to the default (env DAPPLE_GPU=1, Android property debug.dapple.gpu=1, else off).
 * Returns the effective state (0 in builds without the backend). */
DAPPLE_API int32_t dapple_gpu_set_enabled(int32_t mode);
/* {"enabled","available","device","deviceKey","selfTest","reason"} (or {"compiled":false,...}); dapple_free_string. */
DAPPLE_API char* dapple_gpu_status_json(void);

#ifdef __cplusplus
}
#endif

#endif /* DAPPLE_C_H */
