# On-device inference runtime for the AI masks: decision, phone numbers, core/ai

Date 2026-11-14. Follows `ai-models-verified.md`. Phone: Redmi 2312DRA50C (SM7435, 4 x A78 @2.4 GHz = cpu4-7, 4 x A55 = cpu0-3, 11 GB), adb serial 268a8ef9, all runs pinned to cpu4-7, median of 5-9 runs after one warm-up, native benchmark binaries in `core/ai/bench` (no app). Equality = max abs diff / output range against the desktop Python reference (`scripts/ai-eval`). Raw rows: `build/ai-eval/phone/results.jsonl`.

## Decision: two runtimes, one interface

* **LiteRT 1.4.0 (TFLite C API) for the MediaPipe family** (face detector, face mesh, selfie multiclass, MagicTouch int8 encoder/decoder). `libtensorflowlite_jni.so` is 4.32 MB stripped arm64 (1.75 MB deflated). Runs the extracted `.tflite` files unchanged, XNNPACK is applied by the interpreter. Outputs equal the desktop LiteRT to 1e-5 (float models).
* **ONNX Runtime, custom minimal build for Depth Anything V2 Small and MobileSAM**: `libonnxruntime.so` **2.81 MB stripped arm64 (1.16 MB deflated)**, built from tag v1.32.0 with the NDK (`--minimal_build --disable_exceptions --disable_ml_ops --include_ops_by_config` for the 4 models' operators, models converted to ORT format, "Fixed" optimisation), CPU EP only. Outputs equal desktop ORT to 3e-5.
* Why not one: the MediaPipe int8 MagicTouch is LiteRT-only (ORT cannot read tflite; a tf2onnx QDQ conversion would be slow and unfaithful), DAV2/MobileSAM are ONNX-only (LiteRT would need torch conversion). MagicTouch is also 3x faster than MobileSAM for the tap encoder on this phone (0.74 s vs 2.1 s). Total added per ABI: 4.3 + 2.8 = **7.1 MB raw (2.9 MB deflated)**; ORT-only would still need LiteRT for MagicTouch.
* ORT's XNNPACK EP was rejected: slower on the ViT encoder (5.4 s vs 2.1 s at 2 threads) and it fails to create DAV2's Resize (`xnn_create_resize_bilinear2d_nhwc_fp32 failed`). The ORT prebuilt mobile package 1.18 (3.7 MB) cannot run our models (`Where(9)` has no kernel) and is discontinued; the prebuilt full 1.32 AAR is 36.3 MB.
* **Threads: 2 on the big cores is the default.** On this SoC 4 threads were slower than 2 for ORT (dav2 2.1 s -> 3.3 s, MobileSAM enc 2.1 -> 2.7-6 s) and no faster for LiteRT (multiclass 202 -> 211 ms); all 8 cores were worse still (650 ms). `core/ai` defaults to `min(2, perf cores)`; this must be re-measured on a Snapdragon 8-class phone before it is treated as general.
* GPU (LiteRT GPU delegate, +2.5 MB `libtensorflowlite_gpu_jni.so`): works for the float models (face mesh 6 ms, multiclass 77 ms vs 200-320 ms CPU, MagicTouch decoder 112 ms vs 200-260) but **first load costs 1.4 s (face models), 27 s (multiclass), 73 s (MagicTouch decoder)** without a serialized cache, and the MagicTouch encoder had not finished compiling after ~25 minutes (killed). Not usable without an OpenCL program cache; kept out of core/ai.
* Custom-op segmenters: `selfie_segmenter.tflite` and `hair_segmenter.tflite` do **not** load in the LiteRT Android library (`Encountered unresolved custom op: Convolution2DTransposeBias / MaxPoolingWithArgmax2D`). They are not needed: the multiclass model has a hair class.

## Table (person image 682x1024, phone, big cores; peak RSS = process VmHWM)

| Model | Runtime | File MB | load ms | first ms | steady 1t / 2t / 4t ms | peak RSS MB | equality vs desktop |
|---|---|---|---|---|---|---|---|
| BlazeFace short-range 128 | LiteRT CPU | 0.23 | 1-3 | 5 | 3.5 / 3.9 / 3.7 | 17-19 | 3e-7 rel |
| Face mesh v2 256 | LiteRT CPU | 2.5 | 2 | 20-27 | 14.6 / 16.6 / 17.4 | 30-38 | 3e-6 rel |
| Selfie multiclass 256 | LiteRT CPU | 16.4 | 2 | 242-336 | 318 / 202 / 211 | 82-102 | 1e-5 rel, class map 100% |
| Selfie multiclass 256 | LiteRT GPU | 16.4 | 26 892 | 84 | 77 | 130 | same |
| MagicTouch encoder 768 int8 | LiteRT CPU | 28.3 | 3 | 790-1176 | 1123 / 738 / 989 | 227 | 3.6e-3 rel (int8, x86 vs ARM kernels) |
| MagicTouch decoder int8 (per tap) | LiteRT CPU | 2.3 | 2 | 220-330 | 257 / 198 / 287 | 133 | mask IoU 0.993-0.9998 |
| MagicTouch decoder | LiteRT GPU | 2.3 | 73 380 | 121 | 112 | 133 | mask max diff 0.13 |
| Depth Anything V2 Small fp32 | ORT min | 99.3 (.ort) | 262-330 | 2150 | 4117 / 2114 / 3304 | 337 | 4.8e-6 rel (phone vs desktop ORT); 0 diff desktop-ORT vs Python |
| Depth Anything V2 Small int8 | ORT min | 27.5 | 85-100 | 1440 | 2542 / 1376 / 2378 | 376 | 0.22 rel: the x86 reference is the inaccurate one (u8s8 overflow), fp32 is the shipping candidate |
| Depth Anything V2 Small fp32 | ORT full 1.32 | 99 | 410-430 | 2116 | 4048 / 2088 / 3148 | 407 | same |
| MobileSAM encoder | ORT min | 28.3 | 75 | 2170 | 3923 / 2119 / 2724 | 498 | 1.6e-6 rel |
| MobileSAM decoder (per tap) | ORT min | 20.8 | 70 | 140 | 210 / 128 / 313 | 106 | 5e-7 rel |
| MobileSAM encoder, XNNPACK EP | ORT min + xnn | 28.3 | 96 | 3485 | - / 5380 / - | 503 | 2e-6 rel |

Library size per ABI (arm64-v8a, `llvm-strip --strip-unneeded`, deflated in parentheses): LiteRT 1.4 interpreter 4.32 MB (1.75); LiteRT GPU delegate +2.50 MB (1.04); ORT custom minimal 2.81 MB (1.16); ORT minimal+XNNPACK+extended 6.71 MB (2.68); ORT prebuilt full 36.3 MB (13.7); ORT mobile 1.18 3.74 MB (1.47, unusable). LiteRT 2.3.2 AAR (new API, no C headers in the AAR): 5.63 MB + 3.25 MB GPU accelerator, not benchmarked. Other ABIs were not built (arm64 only was requested: armeabi-v7a/x86_64 add the same again each).

Latency summary for the engine: faces 4 ms + mesh 15 ms (per face), person parts 200 ms, MagicTouch 0.74 s once per image + 0.2 s per tap, MobileSAM 2.1 s + 0.13 s per tap, depth 2.1 s (fp32) / 1.4 s (int8). All CPU, 2 big cores.

## iOS (official packages, sizes of the ios-arm64 slice; static libraries before link-time stripping, no Mac, not built or run)

* LiteRT: CocoaPods `TensorFlowLiteC` 2.17.0 (Google, Apache): 80.2 MB tar.gz, `TensorFlowLiteC.xcframework` ios-arm64 **23.4 MB**; Core ML delegate `TensorFlowLiteCCoreML` +15.8 MB; Metal delegate +20.3 MB. (`tools/ai-rt/dl/ios_tflitec`)
* ORT: pod `onnxruntime-c` 1.30.0 (1.32.0 pod not published yet; MIT): 64.0 MB zip, ios-arm64 **48.0 MB**, includes the Core ML EP. A custom minimal build on a Mac (same recipe as Android) is the way to get a few MB; this task did not do it.
* Both prebuilts are 10-50x bigger than the Android custom builds; iOS needs its own minimal builds (or Apple Vision / Core ML models) and a Mac measurement.

## iOS and macOS, built and run (2026-11-14, Mac M1 / macOS 14.7 / Xcode 16.2; CPU / XNNPACK only)

Choice per model: the MediaPipe `.tflite` models (face detector, face landmarks, person parts, MagicTouch encoder / decoder) run on the official TensorFlow Lite C 2.17.0 (`TensorFlowLiteC.xcframework`, Apache-2.0, XNNPACK inside; the C functions `backend_litert.cpp` uses, including `TfLiteInterpreterCancel`, are the same as in LiteRT 1.4); the `.ort` models (MobileSAM encoder / decoder, Depth Anything) run on the official ONNX Runtime `onnxruntime-c` 1.30.0 (MIT). Both prebuilts (source, version, licence and checksums in `docs/legal/ai-models.md`) are static archives, linked statically into `dapple.framework` (no extra embedded framework). No GPU, Core ML or Metal delegate is created. The CMake wiring is in `core/ai/CMakeLists.txt` (`if(APPLE)`: slices ios-arm64 / ios-arm64_x86_64-simulator / macos), `core/CMakeLists.txt` and `scripts/ios/build-engine.sh`; the link needs `-framework Foundation CoreML Network` (+ `UIKit` on iOS: the prelinked ORT object reads `UIDevice`).
macOS has no TFLite C prebuilt: the 2.17.0 source tarball was built with CMake on the Mac (`tools/ai-rt/dl/tf_src`; CMake 4 needs `CMAKE_POLICY_VERSION_MINIMUM=3.5` in the environment), only to run the host tests.

Proof, `dapple_ai_tests` (core/ai, the same C++ test as the Android phone run; reference data `build/ai-eval/ai-ref`, models `tools/ai-rt/ai-models`):
* macOS arm64 (LiteRT = source-built dylib, ORT = official): `--only pil,face,person,tap,sam,api` 143 checks, `--only depth` 15, `--only findfaces` 37: 0 failures. Face counts on the nine CC0 portraits of `docs/research/ai-test-images.md`: p1 1, p2 1, p3 1, p4 0 (strict profile), p5 1, p6 1, p7 0 (backs), p8 3, p9 3, as expected. Landmarks vs the Python reference max 0.005 px; MobileSAM IoU 1.0, MagicTouch IoU 0.993 - 0.9998, depth max diff 1e-4.
* iOS simulator (arm64, iOS 18.3, `xcrun simctl spawn`, the shipped prebuilt TFLite and ORT simulator slices): findfaces / face / person / tap / sam / depth, 148 checks, 0 failures, same counts. Timings (M1 simulator, one run): whole-frame face + mesh 12 - 14 ms, `findFaces` with person-guided windows 50 ms for three people, MagicTouch encoder 0.28 - 0.32 s + decoder 85 ms, MobileSAM encoder 0.75 - 0.85 s + decoder 45 ms, depth 0.84 - 1.0 s.
* Engine level (macOS, `DAPPLE_AI=ON`): `ctest -R ai_masks` 15 / 15 when run alone (with `-j3` one run of `ai_masks.recreate` failed once and passed on the rerun: the tests share a cache directory); `dapple_ai_e2e` on `p9_three_people_frontal.jpg` produced the face (3 faces x 9 features), person and depth planes.
* iOS app: `build-xcframework.sh` (device + simulator slices, engine binary 49.1 / 50.0 MB) and `flutter build ios --release --no-codesign` build; the app carries `ai-models/` with seven of the eight files (depth left out); the engine exports `dapple_ai_*`. NOT done: running the app (device or simulator UI), memory and latency on the A11 / A12 phones (a later step), a minimal iOS ONNX Runtime build.

## core/ai (new, not in the main build)

`core/ai/include/ai/ai.h` (runtime-agnostic): `ai::Model::open(id)` (lazy, files `<id>.tflite|.ort|.onnx` in `ai::setModelDir`), `openFile`, `openMemory`, `inputs()/outputs()` specs, `ai::run(model, inputs, outputs, {threads, cancel, outputs})`, `releaseIdle()/releaseAll()/residentBytes()`. The runtime is chosen by extension. Threads/affinity: `sysperf.h` (`poolCpus`, `setAffinity`, header only, not modified) — sessions are created inside a scoped affinity so pool threads inherit the big cores, the calling thread joins them during a run. Cancel flag: a 2 ms watcher calls `TfLiteInterpreterCancel` / `RunOptionsSetTerminate` (measured: cancel at 20% returns in 56 ms of a 238 ms run, 180 of 815, 513 of 2471 on the phone). ORT runs shrink the CPU arena after every run so idle memory returns to the OS.

`core/ai/include/ai/tasks.h`: `detectFaces`, `faceLandmarks`, `segmentPerson` (+ gate), `prepareTap` -> `TapSession::tap` (MagicTouch or MobileSAM), `depth`. Pre/post-processing is `scripts/ai-eval/ai_ref.py` ported line by line (BlazeFace anchors + weighted NMS 0.3, rotated 1.5x crop, 478 points back in image pixels, MagicTouch click = disk r=12 in channel 0 of the 768x768x3 tensor, MobileSAM point + padding point with the decoder outputs 1,2 only).

**Person gate** (fixes the fruit-as-skin false positive): trusted if a validated face exists (detector score >= 0.5 AND mesh face flag > 0), else if the largest 4-connected blob of hair/skin/clothes classes is >= 3% of the frame, contains hair >= 0.4% and (face skin >= 0.4% or body/clothes >= 1%). Untrusted -> class map all background (the raw probabilities stay available). Result: `fruit` (network says person on 9.6% with 2.8% face skin and 6.7% body) -> gate none, map zeroed; `statue` (10.8% "clothes") -> none; person, profile, backs, small faces -> trusted. Gate numbers come from n=15 images and are not tuned beyond them.

**Resampling note:** PIL picks the pass order of `Image.resize` by a rule I could not reproduce (horizontal-first matched 68% of random sizes; person 682x1024->518 is vertical-first, which differs from horizontal-first by one grey level on 8% of pixels and moved depth by up to 3% at isolated pixels). The reference now spells the passes out (horizontal then vertical, `ai_ref.rs`), and `imageops.cpp` matches it **bit for bit** (0 differing bytes at 256 and 518 on every test image; affine crop: <=1 level on 0.01% of bytes from float32 coefficients). Raw `PIL.resize` and the C++ therefore differ by +-1 grey level on a few percent of pixels; irrelevant for quality.

### Equality of the C++ with the Python reference (phone, 5 images: person, fruit, p1, p4, p9; 126 checks, 0 failed)

* resample: bit-exact; faces: same count, boxes/keypoints within 2e-4 px; mesh flag within 0.05 logit; 478 landmarks within 0.11 px (max, profile) / 0.003 px (frontal); person parts: gate decision + reason identical, class map 100%, probability planes within 2/255, blob area within 0.002; depth max diff 1.1e-4 (range ~6; 2e-5 relative); MobileSAM mask IoU 1.0000, max diff 0; MagicTouch IoU 0.9934-0.9998 (int8, ARM vs x86 kernels; max prob diff 0.12-0.21 on boundary pixels).
* Desktop (llvm-mingw clang, official `onnxruntime-win-x64` 1.32.0, sha256 verified against the GitHub digest): resample bit-exact, depth and MobileSAM diff exactly 0, cancel works (153 ms of a 689 ms depth run).
* Tests: `core/ai/tests/test_ai.cpp`; phone driver `scripts/ai-eval/phone_ai_tests.py`; desktop `build/ai-host` (no LiteRT desktop C library exists, so the tflite tasks are phone-only).

## Portrait findings (9 CC0 images, `ai-test-images.md`; overlays in `build/ai-eval/portraits/out/*_sheet.jpg`, `*_hair_edge.png`; I looked at all of them except p6 hair edge and the p5/p2 hair edges)

* **Frontal faces (p1, p2, p3, p5, p6), glasses, dark and light skin, headscarf:** detector + mesh valid on all, 478 points sit on the features, including through glasses (p1, p3) and on dark skin (p2, p5). Multiclass parts: hair (magenta), face skin, neck/body skin and clothes are in the right places on all five; the headscarf is "clothes" (correct, no hair class); dark dreadlocks against light grey (p2) and curly hair against a busy outdoor background (p3) are well segmented at 256 px.
* **Hair edges:** the 256 px hair probability upsampled bicubic is soft and blobby; the guided filter (r16, s4) snaps strands and the forehead hairline on p3 (see `p3_..._hair_edge.png`); fine flyaway hair still comes from the filter, not the network, and a matting model is not tested.
* **Profile (p4, B/W, hand at mouth):** BlazeFace finds the face (0.6+), **the mesh rejects it (flag -14.8)**, so no landmarks; multiclass still gives hair and a patchy face/hand skin; the region gate trusts it (hair 18%, face skin 1.6%, no clothes). Same for `person` (flag -20.9).
* **Backs of heads (p7):** no face found, parts are good (hair, black top, jeans, arms) and the gate trusts them via hair + body.
* **Multiple people:** p9 (three people at 1-2 m): BlazeFace finds only the left man (mesh flag -7.0: rejected), none of the other two; multiclass nevertheless parts all three bodies and faces. p8 (three small figures): no face detected; parts find hair caps, shirts and skirts of all three but faces are hidden by hands. Short-range BlazeFace misses faces smaller than ~10% of the frame: for groups use the multiclass map, not the detector, or tile the detector.
* **Tap select:** a tap on the face selects the whole person including clothes (MagicTouch and MobileSAM both), exactly the whole-vs-part ambiguity of the earlier note; on p9 the tap on the left man selects only him (good), on p8 only one girl.
* **Depth:** clean person/background separation on all portraits, hair detail on p2/p3 visible.
* **Covered:** frontal, glasses, dark/light/East-Asian skin, headscarf, dark and curly and straight hair, hair on light and busy backgrounds, profile (one, B/W), backs, small and multiple people. **Not covered:** children, beards/facial hair on dark skin in low light, hats and sunglasses, blond/white/red hair, hair against sky (the hard matting case), strong backlight, extreme angles other than the one profile, motion blur, faces below ~100 px.

## Not done / limits

* MediaPipe models were **not converted to ONNX** (tf2onnx needs TensorFlow; MagicTouch is int8): not cheap and it would not remove LiteRT.
* No iOS build or measurement (no Mac). LiteRT 2.x (CompiledModel API) not evaluated. GPU path unusable cold (above); NNAPI skipped as instructed; QNN/NPU not evaluated.
* Only arm64-v8a built; ORT custom build ran for one operator set (the 4 models); adding the conversions of other models means re-running `convert_onnx_models_to_ort` + the build (3 minutes on this PC).
* 2 threads default is measured on one SoC. Numbers are from a warm, idle phone (single temperature state; no thermal series).
* MobileSAM decoder pruning to the low-res output did not make it faster (ORT executes the whole graph): the 1024-wide upsampled mask is still computed.
* LiteRT AAR 1.4.0 comes from Google Maven (dl.google.com/dl/android/maven2), not Maven Central: sha1 verified for litert and litert-gpu; ORT AAR sha1 verified against Maven Central.
* The `core/ai` library compiles in C++23 inside this repo conventions but has not been built with the main project's flags (not hooked in).

## Reproduce

```
source tools/env.sh
# ORT custom build: tools/ai-rt/ort-src (tag v1.32.0), config tools/ai-rt/ort-models/onnx/required_operators_and_types.config,
#   python tools/ci_build/build.py --android ... --minimal_build --disable_exceptions --disable_ml_ops --include_ops_by_config <config>
tools/py-ai/Scripts/python.exe -I scripts/ai-eval/ai_ref.py                 # reference dumps
cmake -S core/ai -B build/ai-android -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DDAPPLE_AI_TESTS=ON
tools/py-ai/Scripts/python.exe -I scripts/ai-eval/phone_ai_tests.py         # phone tests
tools/py-ai/Scripts/python.exe -I scripts/ai-eval/phone_run.py run litert|ort-min <model>   # latency rows
tools/py-ai/Scripts/python.exe -I scripts/ai-eval/portrait_report.py        # overlays
```
