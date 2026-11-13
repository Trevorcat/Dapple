# AI models and inference runtimes shipped with Dapple (AI masks)

Engineering record, not legal advice. Primary sources and the verification trail are in `docs/research/ai-models-verified.md` (section numbers below refer to it; the fetched model cards, licence files and READMEs are cached under `tools/_dl/ai-models/lic/`). The app bundles everything in the APK (the owner is in China without Play services, there is no server): nothing is downloaded at run time.

Where the licence texts are shown to the user: Settings > 开源许可 > "AI 蒙版" (`app/lib/screens/library/licenses_page.dart`, texts in `app/assets/legal/ai/`).

## Bundled model files (APK assets `ai-models/`, copied to app storage on first use)

| Bundled file | What | Source (file as fetched) | SHA-256 of the bundled file | Size | Licence of the model | Model card |
|---|---|---|---|---|---|---|
| `face_detector.tflite` | BlazeFace short range (face detection) | inside `face_landmarker.task`: https://storage.googleapis.com/mediapipe-models/face_landmarker/face_landmarker/float16/latest/face_landmarker.task | `b4578f35940bf5a1a655214a1cce5cab13eba73c1297cd78e1a04c2380b0152f` | 0.23 MB | Apache-2.0 | MediaPipe BlazeFace Model Card (Short Range) |
| `face_landmarks.tflite` | Face Mesh v2 (478 landmarks) | inside the same `face_landmarker.task` | `c7d54204ce0448474c7f3fa9af494787c0965cbdd6f20fc72867e43046bd43d5` | 2.55 MB | Apache-2.0 | Model Card MediaPipe Face Mesh V2 |
| `person_parts.tflite` | Selfie Multiclass 256x256 (background, hair, body skin, face skin, clothes, others) | https://storage.googleapis.com/mediapipe-models/image_segmenter/selfie_multiclass_256x256/float32/latest/selfie_multiclass_256x256.tflite (unchanged, renamed) | `c6748b1253a99067ef71f7e26ca71096cd449baefa8f101900ea23016507e0e0` | 16.37 MB | Apache-2.0 | Model Card Multiclass Segmentation |
| `tap_encoder.tflite`, `tap_decoder.tflite` | Interactive Segmenter "MagicTouch" (int8): tap select | inside `interactive_segmentation.task`: https://storage.googleapis.com/mediapipe-models/interactive_segmenter_v2/magic_touch/int8/latest/interactive_segmentation.task (the two `.tflite` members, unchanged) | encoder `0edbceec02cb15707ea161806a5bc6dbc961cb4153b7504cccdfaae15ea70d7c`, decoder `140d36971403c38ba2f26082b8d1920ea24dd347d6ffaedf4d58177db8d8362b` | 28.26 + 2.27 MB | Apache-2.0 | Model Card MagicTouch (Mar 13, 2023) |
| `depth.ort` | Depth Anything V2 **Small**, fp32, converted from ONNX to ONNX Runtime format (no weight change) | ONNX: https://huggingface.co/onnx-community/depth-anything-v2-small (from https://huggingface.co/depth-anything/Depth-Anything-V2-Small, repo https://github.com/DepthAnything/Depth-Anything-V2) | `858d370d1783e8b01d4f681fab1dabbf0e89e76bfc9fc2aefcbcd41f8f86e9ab` | 99.26 MB | Apache-2.0 for the **Small** model ("Depth-Anything-V2-Small model is under the Apache-2.0 license. Base/Large/Giant ... CC-BY-NC-4.0": only Small is used) | HF card `license: apache-2.0` |

Model cards are at `https://storage.googleapis.com/mediapipe-assets/` (file names in `ai-models-verified.md` 1a). Total of the files in the APK: **148.9 MB** (`-Pdapple.aiDepth=false` leaves out `depth.ort`: 49.7 MB).

### What the licence stack says that a lawyer should see (from section 1, none of it is a licence term)

* MediaPipe cards, MagicTouch: "Human segmentation that is not consented to by the human being segmented is out of scope and not enabled by this technology"; all MediaPipe cards: surveillance / identity recognition out of scope, not for life-critical decisions. Dapple edits the user's own photos, which may show people who did not consent to being segmented: flag to counsel (as in section 1a).
* Training data: MediaPipe: "consented images of people using a mobile AR application" (face, selfie) or undescribed Google-internal data (multiclass, MagicTouch objects). Depth Anything V2 Small: 595K synthetic + 62M real unlabeled images per the card; the larger siblings are NC and Small was distilled from them (the licensor states Apache-2.0 for Small).
* Face Mesh topology tables used to draw lips / eyes / brows / face oval (`core/src/ai_masks_build.cpp`, `kFaceOval` ... `kBrowLeftLower`): index lists of the `FACEMESH_*` connection sets of `mediapipe/python/solutions/face_mesh_connections.py` (https://github.com/google-ai-edge/mediapipe, Apache-2.0, Copyright 2021 The MediaPipe Authors), chained into closed rings; the file's licence header is part of the Apache text shipped.

## Not bundled (development only)

MobileSAM (`sam_encoder.ort`, `sam_decoder.ort` / ONNX): the tap engine on desktop where LiteRT does not exist. Weights licence stated inconsistently (Apache-2.0 code `LICENSE`, HF mirrors `license: mit`, section 1b): not shipped. Not bundled either: deeplab, hair segmenter, selfie segmenter (custom ops the LiteRT Android library cannot load), blendshape model, any SAM variant, U2-Net, IS-Net, BiRefNet (DIS5K non-commercial), MiDaS.

## Runtimes

| Library (per ABI, in `lib/<abi>/`) | Version / build | Source | SHA-256 | Size | Licence |
|---|---|---|---|---|---|
| `libtensorflowlite_jni.so` arm64-v8a | LiteRT 1.4.0 (TFLite C API), unmodified from the AAR | https://dl.google.com/dl/android/maven2/com/google/ai/edge/litert/litert/1.4.0/litert-1.4.0.aar | `f9a6224f22f60426ae1b9d35f06ea040f2b108335932604a04c061d612f7f08f` | 4.32 MB (1.75 deflated) | Apache-2.0 (`google-ai-edge/LiteRT` LICENSE) |
| `libonnxruntime.so` arm64-v8a | ONNX Runtime v1.32.0, custom **minimal build** (CPU EP, `--minimal_build --disable_exceptions --disable_ml_ops --include_ops_by_config`, operators of the four shipped ONNX-format models), built from the tag with the NDK, stripped | https://github.com/microsoft/onnxruntime tag v1.32.0 | `d946dceeb87b70f00ecdcb02d9cd6b20392f2ac3c12a1a6d7a84e4c6de455333` | 2.81 MB (1.16 deflated) | MIT + `ThirdPartyNotices.txt` |
| x86_64 (emulator debug builds, `-Pdapple.x86`) | LiteRT 1.4.0 AAR `jni/x86_64`; ONNX Runtime 1.32.0 **official full** AAR `jni/x86_64` (45 MB; no minimal build was made for x86_64) | Google Maven (above); Maven Central `com.microsoft.onnxruntime:onnxruntime-android:1.32.0` | LiteRT `b1fdd3d23e58df54f8ce47c704ed4bc0882be37e479fe86a87108d5f8153c422`, ORT `d2f52d67e8c27c999d13ada4f2edd9b7d8aab5e8cf0afe59c825f01b990ea767` | 6.05 + 44.97 MB | as above |

ONNX Runtime `ThirdPartyNotices.txt` (fetched 2026-11-14, 5060 lines) lists, among others, Eigen (MPL-2.0: file-level copyleft, fine for binary distribution if the notice is shipped and Eigen files are unmodified), Abseil (Apache), pytorch/cpuinfo (BSD), dlpack (Apache), KleidiAI (Arm), mbedtls, smhasher, Google test. A search for GPL / SSPL / BUSL / CC-BY found nothing. The whole file is shipped (the custom build contains a subset; shipping all is the conservative choice). LiteRT's own third-party notices (flatbuffers, XNNPACK, ruy, farmhash, FFT2D, all permissive) were not fetched ([M] in section 1e): add the AAR's `LICENSE` and check `XNNPACK` notices before a store release.

## Licence texts shipped in the app

`app/assets/legal/ai/`: `APACHE-2.0.txt` (the standard Apache-2.0 text, from the LiteRT repository `LICENSE`; the MediaPipe models, LiteRT and Depth Anything V2 Small are Apache-2.0), `ONNXRUNTIME-MIT.txt`, `ONNXRUNTIME-THIRD-PARTY-NOTICES.txt`. Apache NOTICE: no `NOTICE` file was among the MediaPipe / LiteRT / Depth Anything files fetched (the MediaPipe `LICENSE` only adds third-party text for code we do not ship); the credit lines are in the screen text (`LicensesPage.aiNotice`). Verify against the current repositories before a store release.
