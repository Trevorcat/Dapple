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
/* Prebake of every film stock at its default settings (raw + curves + coarse print tables) on a utility-QoS thread, paused while dapple_set_interaction(1).
 * Start once the pack is loaded; the tables persist on disk when the GPU cache dir is set. status: {"done","total","finished","running","rawMs","curvesMs","coarsePrintMs"}. */
DAPPLE_API void dapple_film_prebake_start(void);
DAPPLE_API char* dapple_film_prebake_status(void);
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

/* Batch swatches of rendering bases on one photo (film map, 成像 page): renders the session's picture once per base id at long_edge (default 96 when <= 0)
 * with params_json (the photo's own correction; any "film" key is replaced by the id) and returns 8-bit sRGB images in out[0..n-1] (n <= 256; release each
 * with dapple_free_image). A base id is a film stock id, a modern rendering id (modern_clean / modern_vivid / modern_soft), or "" / NULL = the standard
 * rendering; an unknown id renders as the standard rendering (one log line). Each image is the interactive tier of dapple_session_render_frame (coarse film
 * tables, which the prebake has for every stock; modern bases through their lattice; GPU frames when the engine has a device, else CPU), sharing the source
 * downscaled to long_edge, the tone proxy and the engine plans between bases. Runs on the calling thread at background priority, holds the session lock one frame
 * at a time, and waits between frames while dapple_set_interaction(1) or a touch-hint burst is active. Latest wins: a newer call on the session, or
 * dapple_session_base_previews_cancel, makes this one return at the next frame boundary; images not rendered stay {0,0,NULL}. Returns DAPPLE_OK (check each
 * rgba) or DAPPLE_ERROR for bad arguments or when no frame could be rendered. Additive. */
DAPPLE_API int32_t dapple_session_base_previews(DappleSession* session, const char* params_json, const char* const* base_ids, int32_t n,
                                                int32_t long_edge, DappleImage* out);
/* Lock-free, any thread: stops the base_previews call in flight on this session (see above). A no-op when none runs. */
DAPPLE_API void dapple_session_base_previews_cancel(DappleSession* session);

/* ---- Local adjustments through masks (docs/engine/pipeline-architecture.md section 40). Additive. ----
 * The adjustments travel in the params JSON under "local" ({"v":1,"items":[...]}); every render / export call honours them
 * (DAPPLE_ENGINE=off ignores them). Mask coordinates are the photo's own normalised space (before crop / rotate / straighten / flip).
 *
 * Raster masks (AI masks, depth): a single-channel 8-bit plane referenced by id from a "raster" component. The store is process-wide
 * (exports, other sessions and the batch previews see the same id), at most 64 planes / 96 MB (oldest dropped first); planes above 1024 px long edge are
 * area-downsampled. stride = bytes per row (0 = w). Replacing an id changes the content key: frames re-render. */
DAPPLE_API int32_t dapple_mask_raster_set(const char* id, int32_t w, int32_t h, const uint8_t* bytes, int32_t stride);
DAPPLE_API int32_t dapple_mask_raster_remove(const char* id);
/* 1 when the id is in the store (re-supply it after an app restart before rendering an edit that uses it), else 0. */
DAPPLE_API int32_t dapple_mask_raster_has(const char* id);
/* The mask of local adjustment `index` (position in the "local" items) of params_json, as an RGBA image (r = g = b = mask 0..255,
 * a = 255) for the UI's overlay: over the edit's output frame (crop / rotate applied) at long_edge (<= 0: 1024), or, with flags bit 0,
 * over the whole photo in its own orientation. Disabled adjustments and adjustments without ops are shown too. Release with
 * dapple_free_image. */
DAPPLE_API int32_t dapple_session_mask_preview(DappleSession* session, const char* params_json, int32_t index, int32_t long_edge, int32_t flags,
                                              DappleImage* out);
/* Maps a point between the edit's output frame rendered at long_edge (pixels, x right, y down) and the photo's normalised space of the
 * masks. direction 0: frame (x, y) -> photo (u, v); 1: photo (x, y) -> frame (u, v). Points outside the frame map too (affine). */
DAPPLE_API int32_t dapple_session_map_point(DappleSession* session, const char* params_json, int32_t long_edge, float x, float y,
                                           int32_t direction, float* u, float* v);
/* What the luminance / colour range components see at photo point (u, v) under params_json (global white balance, exposure, HDR):
 * {"L":0..100,"hue":0..360,"sat":..} (sat in the units of the "satMin" range / 0.25 * 100). Free with dapple_free_string. */
DAPPLE_API char* dapple_session_mask_sample_json(DappleSession* session, const char* params_json, float u, float v);

/* ---- AI masks: an engine service that produces raster planes (docs/engine/pipeline-architecture.md section 41). Additive. ----
 * Planes are 8 bit, long edge <= 1024, in the photo's normalised space, registered in the raster store under stable ids
 * "ai:<photo>:<key>" (see ai_masks.h): person parts (hair faceskin bodyskin clothes person background), per face f1..f4 (face eyes iris sclera lips teeth
 * brows), depth (0 = nearest .. 255 = farthest; use a raster component with lo / hi / feather to select a band), sky (rough), tap:<taps>.
 * Model input is the neutral standard rendering of the photo (default params, geometry "orig", white balance as shot + exposure anchor, no local
 * adjustments, sRGB) at <= 1024 px, so a plane never depends on the user's edit. Every function exists in every build; without the runtimes /
 * models they report available:false and the calls return an error. Jobs run on one background thread (utility priority, yielding to
 * dapple_set_interaction), planes are cached on disk per photo content hash under cache_dir/ai_masks. */
enum {
  DAPPLE_AI_PERSON = 1,       /* person parts + person + background (only when a person is found) */
  DAPPLE_AI_FACES = 2,        /* face features of up to max_faces faces (frontal faces get eyes / lips / teeth / brows) */
  DAPPLE_AI_DEPTH = 4,
  DAPPLE_AI_SKY = 8,          /* classical prior (+ depth far part when the depth model is present) */
  DAPPLE_AI_TAP_PREPARE = 16  /* run the tap encoder now (about 0.7 s on a phone) so the first tap is fast */
};
/* model_dir: the files <id>.tflite|.ort|.onnx (person_parts, face_detector, face_landmarks, depth, tap_encoder, tap_decoder, sam_encoder,
 * sam_decoder); cache_dir: the app cache directory (the same as dapple_gpu_configure's; may be NULL = no disk cache). */
DAPPLE_API int32_t dapple_ai_configure(const char* model_dir, const char* cache_dir);
/* {"compiled":bool,"available":bool,"backends":"litert+ort","tasks":{"person","faces","depth","tap","sky"},"tap_engine":"magictouch"|"mobilesam"|"",
 *  "reason":"","resident_bytes":N}. Free with dapple_free_string. */
DAPPLE_API char* dapple_ai_status_json(void);
/* The 16 hex digits the plane ids of this photo carry (hash of the decoded picture). Free with dapple_free_string. */
DAPPLE_API char* dapple_session_ai_photo_id(DappleSession* session);
/* Starts a job (tasks = or of DAPPLE_AI_*; flags bit 0: recompute although cached). Returns the job id (> 0) or -1 (error: unavailable). */
DAPPLE_API int32_t dapple_session_ai_start(DappleSession* session, int32_t tasks, int32_t flags, int32_t max_faces);
/* {"job":N,"state":"queued|running|done|cancelled|error|unknown","progress":0..1,"stage":"","result":{state,message,person_found,faces,sky_found,
 *  groups:[{group,state ok|cached|none|unavailable|error,message,ms}],planes:[{id,key,label,group,face,w,h,cached}],render_ms,total_ms}}
 * ("result" once finished). Free with dapple_free_string. */
DAPPLE_API char* dapple_ai_job_json(int32_t job);
/* Latest-wins cancel (a model run in flight returns within about 60 ms). */
DAPPLE_API void dapple_ai_job_cancel(int32_t job);
/* Tap select (synchronous, call it off the UI thread; the encoder is prepared on the first tap of a photo). (u, v) photo normalised. op 0 add the
 * object under the tap, 1 subtract it, 2 undo the last tap, 3 reset. Taps accumulate into one plane. Returns {"ok":bool,"message":"","id":"ai:..:tap:+412,531",
 * "w","h","taps":[[u,v,1],..],"ms","prepare_ms"} ("id" empty after a reset); the plane is registered. Free with dapple_free_string. */
DAPPLE_API char* dapple_session_ai_tap_json(DappleSession* session, float u, float v, int32_t op);
/* The plane `id` as an RGBA gray image at long_edge (<= 0: 128) for lists; DAPPLE_ERROR when the id is not in the store. dapple_free_image. */
DAPPLE_API int32_t dapple_ai_plane_preview(const char* id, int32_t long_edge, DappleImage* out);
/* The raster ids of the enabled local adjustments of params_json that are not in the store: {"missing":[{"id","ai":bool,"photo":bool(this photo's id),
 * "cached":bool(on disk)}]}. Free with dapple_free_string. */
DAPPLE_API char* dapple_session_ai_missing_json(DappleSession* session, const char* params_json);
/* Re-creates the missing "ai:" planes of params_json from the disk cache or by recomputation. flags bit 0: as a background job (returns the job id,
 * 0 = nothing to do; tap planes are re-created synchronously), else synchronously: returns the number of ids still missing (0 = all there).
 * Exports do this themselves: an export whose edit needs a plane that cannot be re-created FAILS (DAPPLE_ERROR, dapple_last_error names the plane)
 * instead of rendering the mask as empty; interactive frames render a missing plane as empty. */
DAPPLE_API int32_t dapple_session_ai_ensure(DappleSession* session, const char* params_json, int32_t flags);
/* Frees model memory: all = 0 only what is idle (also done by the service itself every few seconds), 1 everything (memory pressure). */
DAPPLE_API void dapple_ai_release(int32_t all);

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
  DAPPLE_FRAME_BACKGROUND = 8,  /* low priority: never the shared pool (the background pool while no finger is down); calling thread on efficiency cores, nice 10, when a finger is down (dapple_set_interaction) at the frame's start */
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
/* A finger is on a control (1) / lifted (0): background demosaic tile loops (regions, previews) wait between tiles meanwhile. */
DAPPLE_API void dapple_set_interaction(int32_t active);
/* Frame trace (diagnostics, off by default): one TSV row per rendered session frame (stock, print table kind / generation, bakes and match running, timings).
 * enable(1) clears and starts, dump writes the rows so far, clear drops them. */
DAPPLE_API void dapple_frame_trace_enable(int32_t on);
DAPPLE_API int32_t dapple_frame_trace_dump(const char* path);
DAPPLE_API void dapple_frame_trace_clear(void);
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

/* Export with metadata. export_json (null = "{}"): {"metaSource":"<the user's original file>","stripLocation":true,"colorSpace":"srgb","software":"Dapple 1.0"}.
 * The stream carries an Exif APP1 block (orientation normal, camera make / model / lens, ISO, shutter, aperture, focal length, capture date with
 * sub-second and UTC offset when the source has them, pixel size, software) and an sRGB ICC profile. Camera facts come from the file named by
 * metaSource (JPEG, TIFF-based RAW / DNG, Fujifilm RAF, PNG eXIf, HEIC / AVIF) and from the session's decoder metadata where the file has none. GPS is
 * written only when the source has a fix and stripLocation is false (default true: no GPS IFD at all). colorSpace "srgb" only today: anything else is
 * an error (the render pipeline ends in sRGB). The plain dapple_session_export_jpeg / _hdr write the bare stream as before. Additive. */
DAPPLE_API int32_t dapple_session_export_jpeg_ex(DappleSession* session, const char* params_json, const char* lut_path, int32_t out_long_edge,
                                                 int32_t jpeg_quality, const char* out_path, const char* export_json);

/* Ultra HDR (gain-map JPEG) from the same edit. dapple_session_hdr_headroom reports, without writing a file, whether the picture has highlights
 * worth encoding; dapple_session_export_jpeg_hdr writes the SDR export (the base stream is byte for byte the one dapple_session_export_jpeg
 * writes) plus MPF, XMP (hdrgm), ISO 21496-1 and the gain map, or the plain JPEG when `written` comes back 0 (reason: 1 no highlight headroom,
 * 2 film stock (a print: paper white is the top), 3 the renders could not be paired). headroom_stops 0 = from the scene (0.5 .. 3), else that
 * value (capped at 3). Additive; docs/engine/pipeline-architecture.md section 34. */
typedef struct DappleHdrInfo {
  int32_t written;       /* 1 = the file carries a gain map */
  int32_t reason;        /* 0 ok, 1 no headroom, 2 film, 3 not paired */
  float headroom_stops;  /* H used (0 when none) */
  float peak;            /* 99.9th percentile of the scene luminance in SDR white units (>= 1.5 = worth encoding) */
  float gain_max;        /* GainMapMax (log2) */
  float capacity_max;    /* HDRCapacityMax (log2) */
} DappleHdrInfo;
DAPPLE_API int32_t dapple_session_hdr_headroom(DappleSession* session, const char* params_json, const char* lut_path, DappleHdrInfo* info);
DAPPLE_API int32_t dapple_session_export_jpeg_hdr(DappleSession* session, const char* params_json, const char* lut_path, int32_t out_long_edge,
                                                  int32_t jpeg_quality, const char* out_path, float headroom_stops, DappleHdrInfo* info);
/* The Ultra HDR export with the metadata of dapple_session_export_jpeg_ex (the base stream and the plain fallback carry the same segments). */
DAPPLE_API int32_t dapple_session_export_jpeg_hdr_ex(DappleSession* session, const char* params_json, const char* lut_path, int32_t out_long_edge,
                                                     int32_t jpeg_quality, const char* out_path, float headroom_stops, DappleHdrInfo* info,
                                                     const char* export_json);
/* Export colour space "p3" (Display P3) of the *_ex calls: share (0..1) of the picture after the edit that lies outside sRGB (see c_api.cpp); the
 * app offers the wide-gamut export only when it is not negligible. Additive. */
DAPPLE_API int32_t dapple_session_wide_gamut_fraction(DappleSession* session, const char* params_json, const char* lut_path, float* fraction);

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
 * An Ultra HDR cover (gain map between the primary and the video) is checked against the directory, the MPF index and the bytes and reported as
 * "gainMap":true,"gainMapOffset","gainMapLength" (docs section 30.14).
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
/* What the clip is written as: 0 SDR BT.709, 1 HLG BT.2020, 2 PQ BT.2020 (HDR source, edit without film; crop / rotate / straighten / flip keep HDR, the float source is warped like the frame; docs section 30.8), -1 on error.
 * tags = primaries, transfer, matrix, range (FFmpeg AVCOL_*); note (static string) says why. Format 3 / 5 are then HLG / PQ encoded BT.2020 frames. */
DAPPLE_API int32_t dapple_video_edit_output_kind(DappleVideoEdit* edit, int32_t tags[4], const char** note);
DAPPLE_API const char* dapple_video_edit_last_error(void);

/* Ultra HDR cover of an HDR Motion Photo (docs section 30.14). `frame` = the cover frame (half-float RGBA as above), rendered through the same edit as the
 * clip; its SDR result and its HDR rendition (30.8, geometry included) give a gain map that is attached to the SDR cover JPEG at sdr_jpeg_path (the
 * still export of the same edit): the base stream of out_path is that file byte for byte, plus the MPF / XMP / ISO / gain-map segments, so
 * dapple_motion_photo_write accepts it as the cover and lists Primary, GainMap and MotionPhoto in one Container directory. When the picture has no
 * headroom (peak of the HDR luminance < 1.5 x SDR white, or >= 1.1 with force_hdr = 1: the user asked for HDR), the source is SDR, the edit has a film
 * stock or the sizes do not pair, out_path is a copy of the SDR cover and info->written = 0 (reason 1 no headroom / SDR source, 2 film, 3 not paired).
 * Returns DAPPLE_OK in both cases. info may be NULL. */
DAPPLE_API int32_t dapple_video_edit_cover_uhdr(DappleVideoEdit* edit, const uint16_t* rgba_f16, int32_t width, int32_t height, int32_t colour_kind,
                                                int32_t rotation_deg, const char* sdr_jpeg_path, const char* out_path, int32_t force_hdr,
                                                DappleHdrInfo* info);

/* ---- Streaming (band) export (docs/engine/pipeline-architecture.md section 42) --------------------------------------------------------------
 * Nothing to call: a DAPPLE_QUALITY_FULL open of a mosaic RAW whose whole-frame export would not fit the device's memory keeps the 16-bit mosaic
 * instead of the float picture, and the dapple_session_export_jpeg* calls on it render in bands straight into the JPEG encoder (same pixels; peak
 * memory independent of the picture size). DAPPLE_EXPORT_STREAM=on|off|auto (default auto) forces it either way. Additive helpers: */
/* Progress of the exports of this session, per band (stage "export"); return non-zero to cancel (the export fails with "cancelled", nothing is
 * written). fn = NULL removes it. Called on the exporting thread. */
DAPPLE_API int32_t dapple_session_set_export_progress(DappleSession* session, DappleProgressFn fn, void* user);
/* 1 when the session was opened for a streamed export (no float picture held), else 0. */
DAPPLE_API int32_t dapple_session_is_streaming(DappleSession* session);
/* {"footprint":bytes,"resident":bytes,"available":bytes,"exportBudget":bytes,"stream":"auto"}: the process's memory as the OS counts it
 * (phys_footprint on Apple, RSS on Linux / Android, commit on Windows), what it may still allocate (os_proc_available_memory on iOS) and the budget
 * the export policy derives from it. Free with dapple_free_string. */
DAPPLE_API char* dapple_memory_json(void);

/* ---- Memory: device class, report, trim (docs/engine/pipeline-architecture.md 42.9) -------------------------------------------------------
 * The device's memory class as the app knows it, once at start-up (Android: ActivityManager.MemoryInfo.totalMem, getMemoryClass(),
 * getLargeMemoryClass(), isLowRamDevice()): {"totalBytes":12000000000,"memoryClassMb":256,"largeMemoryClassMb":512,"lowRam":false}. Missing keys
 * keep their value; without a call the engine reads the physical memory itself. Sets the export ceiling (42.9) and the mosaic cache budget.
 * DAPPLE_OK, DAPPLE_ERROR on malformed JSON. */
DAPPLE_API int32_t dapple_memory_configure(const char* json);
/* What the engine holds, per cache / pool, in bytes: {"footprint":..,"available":..,"nativeHeap":{"allocated":..,"retained":..},
 * "sessions":{"count":..,"pictureBytes":..},"mosaicCache":{"bytes":..,"budget":..,"entries":..},"gpu":{"pooled":..,"poolBudget":..,"budget":..,
 * "usage":..,"unified":..},"film":{"frameWorkspaces":..},"ai":{"modelBytes":..},"exportBudget":{"bytes":..,"how":".."}}. Free with dapple_free_string. */
DAPPLE_API char* dapple_memory_report_json(void);
/* Gives memory back. level 0 = idle (the editor closed): the cached mosaics, the GPU pool's free images, the film workspaces and idle AI models
 * freed, the allocator's free pages returned; 1 = pressure (Android TRIM_MEMORY_RUNNING_LOW / UI_HIDDEN): also every AI model and the mask plane
 * cache; 2 = critical (RUNNING_CRITICAL / BACKGROUND / COMPLETE): also the per-file exposure anchors. Never touches
 * what open sessions hold. Returns the bytes released (cache accounting; the process footprint drops when the allocator returns the pages), and
 * logs one line with the report before / after. */
DAPPLE_API int64_t dapple_memory_trim(int32_t level);
/* The edit of the export that follows: the next DAPPLE_QUALITY_FULL dapple_session_open on THIS thread takes its whole-frame / bands decision with
 * the edit known (a film stock needs ~55 B/px more) instead of decoding the whole picture first and giving it back at the export. Optional; consumed
 * by that open. DAPPLE_OK, DAPPLE_ERROR on malformed params. */
DAPPLE_API int32_t dapple_export_intent(const char* params_json);

#ifdef __cplusplus
}
#endif

#endif /* DAPPLE_C_H */
