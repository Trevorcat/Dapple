# AI-assisted masks for Dapple: research notes

Date: 2026-11-14. Method: targeted page fetches of model cards / repos / docs, plus background knowledge.
Marking: **[V]** = read on the cited page this session; **[M]** = from memory / not re-verified, check before relying on it; **[E]** = my estimate, not a published number.
This is engineering research, not legal advice: every "licence verdict" needs a lawyer pass before release.

Repo check: `docs/design/requirements.md` has no AI-mask requirement. Grep for AI / 蒙版 / 遮罩 / 局部 / 人像 hit only UI scrims (遮罩 = overlay) and the "清晰度 = 中频局部对比" row. So masks are a new area.

## 0. Summary

1. Hard truth on licences: almost every open "subject / saliency" model (U²-Net, IS-Net, BiRefNet, MODNet) has permissive code AND weights (MIT/Apache) but was trained on academic datasets (DUTS, DIS5K, HRSOD, P3M, PPM...) whose terms are research-oriented or unstated. This is a grey zone, not a clean pass. No fully clean-room open subject model was found.
2. Hard rejects (weights or data non-commercial): RMBG-2.0 (CC BY-NC 4.0 [V]), Depth Anything V2 Base/Large (CC BY-NC 4.0 [V for Base]), Sapiens (CC BY-NC 4.0 weights [V via HF cards]), EdgeSAM (NTU S-Lab License [V], NC), SegFormer/NVIDIA weights (NVlabs licence "other", NC [M]) and anything trained on ADE20K (NC research only [V]) or CelebAMask-HQ (NC [V]), RVM (GPL-3.0 [V]), FastSAM (AGPL-3.0 [M]), Depth Pro (Apple research licence [M]).
3. Cleanest paths: (a) Apple Vision on iOS (system API, no weights shipped); (b) MediaPipe Tasks (Apache-2.0 library; works without Google Play services, which matters for OPPO/China) for person/selfie-multiclass/hair and interactive segmentation (MagicTouch); (c) SAM / MobileSAM / SAM 2 (Apache-2.0 code+weights; SA-1B data is under a "Research License" [V] -> flag); (d) Depth Anything V2 **Small** (Apache-2.0 [V]).
4. ML Kit Subject Segmentation depends on Google Play services (unbundled model download, beta, no SLA [V]) -> do not rely on it in China.
5. Recommended v1 masks: brush / linear / radial / luminance-colour range (no AI); tap-to-select via MobileSAM (encoder ~5M params, decoder 3.9M [V]); person + parts via MediaPipe Selfie Multiclass on Android and Vision on iOS; subject via MobileSAM-box-prompt or BiRefNet-lite-class model with documented data risk; sky via a small model we train ourselves (no clean off-the-shelf one found); depth range via Depth Anything V2 Small, later.
6. Runtime: ONNX Runtime Mobile (MIT) with XNNPACK (CPU) on Android and Core ML EP on iOS, one ONNX file per model. NNAPI is deprecated since Android 15 [V]. Running the networks inside our own Vulkan/GLES/Metal compute HAL is not realistic for v1 (section 3).
7. Refinement: run net at 512-1024 px, refine with guided filter (fast guided filter) in our GPU pipeline using the image as guide, tile-wise at full-res on export. Fits the existing compute HAL perfectly (box filters, a few passes).
8. Mask = one R8/R16F plane in the node graph feeding local-adjustment nodes as a blend factor.
9. Total download (suggested): MobileSAM encoder+decoder ~40 MB as ONNX [E], MediaPipe multiclass ~16 MB [E], sky ~5 MB [E], runtime ~3-8 MB [E].

## 1. Mask kinds and candidates

### 1a. Salient subject / matting

| Model | Params / size | Input | Code licence | Weights licence | Training data terms | Verdict |
|---|---|---|---|---|---|---|
| U²-Net / U²-Netp | u2net 176.3 MB, u2netp 4.7 MB [V] | 320 [M] | Apache-2.0 [V] | Apache-2.0 (repo-wide) [V] | DUTS-TR [V]; DUTS images come from ImageNet (research use) [M] | usable, data grey; u2netp is the only really tiny one; quality coarse |
| IS-Net (DIS) | ~44 MB [M] | 1024 | Apache-2.0 [M] | Apache-2.0 [M] | DIS5K (academic) [M] | usable, data grey |
| BiRefNet (+ lite / swin_v1_tiny) | ~0.2B for large [V]; tiny/lite counts not given on the page | 1024 (2048 / 2560x1440 variants) [V] | MIT [V] | MIT [V] | DIS5K, DUTS, HRSOD, UHRSD, HRS10K, P3M-10k, humans... [V]; the repo gives no dataset licence terms [V], "check each dataset's page" | best quality; data grey; A100 1024 px: SwinL ~165 ms, SwinT ~94 ms in ONNX [V]: far too heavy for phones unless tiny/lite is quantised (no mobile number published) |
| RMBG-1.4 | ~44 MB [M] | 1024 | n/a | BRIA licence, non-commercial [M] | BRIA licensed data | REJECT (weights NC) |
| RMBG-2.0 | ~0.2B [V] | 1024 [V] | MIT-ish BiRefNet arch | **CC BY-NC 4.0, commercial needs BRIA agreement** [V] | 15k licensed images [V] | REJECT |
| MODNet | ~6.5 MB [M] | 512 | Apache-2.0 [V] | Apache-2.0 (repo-wide statement) [V] | PPM / paper datasets, terms not stated [V] | usable for portraits; ONNX path exists [V]; data grey |
| InSPyReNet | ~100 MB+ [M] | 384-1024 | MIT [M] | MIT [M] | DUTS / HRSOD / UHRSD [M] | usable on paper, heavy |
| BEN / BEN2 (PramaLLC) | n/a | 1024 | [M] BEN2 weights are non-commercial-ish / separate commercial licence | | | REJECT until verified |

Sources: https://github.com/xuebinqin/U-2-Net , https://github.com/ZhengPeng7/BiRefNet , https://huggingface.co/ZhengPeng7/BiRefNet , https://huggingface.co/briaai/RMBG-2.0 , https://github.com/ZHKKKe/MODNet

Quality notes: BiRefNet is the current best open dichotomous segmentation / matting quality; U²-Netp is mobile-fast but edges are soft, which is acceptable only because we refine with a guided filter. Honest conclusion: if "subject" must be a single ML subject mask, prefer deriving it from a promptable model (1b) or the platform API, and use BiRefNet-tiny only after (i) measuring phone latency, (ii) legal review of the dataset terms.

### 1b. Promptable segmentation (tap / box)

| Model | Size | Split | Speed published | Exports | Code | Weights | Data | Verdict |
|---|---|---|---|---|---|---|---|---|
| SAM (ViT-H/L/B) | 615M total, enc 611M [V] | enc + light decoder | enc 452 ms on GPU [V] | decoder ONNX only from official repo; encoder stays PyTorch [V] | Apache-2.0 [V] | Apache-2.0 [V] | SA-1B under "SA-1B Dataset Research License" [V: named, terms not read] | too big for phone |
| MobileSAM | enc 5M, dec 3.9M, total 9.66M [V] | ViT-tiny enc + SAM decoder | 8 ms enc + 4 ms dec on a GPU; ~3 s on Mac i5 CPU [V]; EdgeSAM table: 4.9 FPS on iPhone 14 [V] | ONNX export script [V] | Apache-2.0 [V] | Apache-2.0 [M] | distilled on 1% of SA-1B (100k images) [V] | **best licence-compatible tap-to-select today** (data flag) |
| EdgeSAM | 9.6M params, 22.1 GFLOPs @1024 [V] | enc + decoder | 38.7 FPS on iPhone 14 [V] (Core ML) | pre-exported ONNX + Core ML [V] | **NTU S-Lab License 1.0** [V] | same | 1-3% SA-1B [V] | **REJECT** (S-Lab licence is non-commercial [M]); technically the best |
| EfficientSAM (Meta, SAMI) | Ti 10M / S 25M [M] | | | | Apache-2.0 [M] | Apache-2.0 [M] | SA-1B + ImageNet | candidate, verify |
| EfficientViT-SAM | L0 ~35M [M] | | fast on GPU | ONNX [M] | Apache-2.0 [V repo LICENSE]; weights licence not stated on that page [V] | unverified | SA-1B | verify weights licence |
| SlimSAM | ~10M [M] | pruned SAM | | | Apache-2.0 [M] | | SA-1B 0.1% [M] | candidate |
| RepViT-SAM | ~ MobileSAM size [M] | | claimed faster than MobileSAM [M] | | Apache-2.0 [M] | | | candidate |
| NanoSAM | ResNet18 enc [M] | TensorRT | Jetson | | Apache-2.0 [M] | | | NVIDIA-targeted, skip |
| FastSAM | YOLOv8-seg [M] | single pass, no prompt encoder | | | **AGPL-3.0** [M] | | SA-1B 2% | **REJECT** (AGPL, also weak edges) |
| SAM 2 (image mode) | tiny 38.9M [V] | | | none mentioned [V] | Apache-2.0 (code and checkpoints) [V] | Apache-2.0 [V] | SA-V licence not stated on page [V]; CC BY 4.0 [M] | great quality, 4x bigger than MobileSAM; later |
| MediaPipe Interactive Segmenter "MagicTouch" | int8, 768x768 [V] | one .task bundle | **Pixel 10: 208 ms CPU / 581 ms GPU** [V] | .task, Flutter wrapper bundles it [V] | Apache-2.0 library [M] | model card PDF, terms not read [V: link only] | Google internal [M] | **strong alternative**: Android and iOS, no GMS |

Sources: https://github.com/ChaoningZhang/MobileSAM , https://raw.githubusercontent.com/chongzhou96/EdgeSAM/master/README.md , https://github.com/facebookresearch/segment-anything , https://github.com/facebookresearch/sam2 , https://developers.google.com/edge/mediapipe/solutions/vision/interactive_segmenter , https://github.com/mit-han-lab/efficientvit/blob/master/LICENSE

Design: encoder runs once per image (at ~1024 long edge, ~SAM native), embedding cached; decoder runs per tap (milliseconds, interactive). The SAM decoder outputs 256x256 low-res logits upsampled to image size, so edges are blobby and need refinement (1f).

### 1c. People

- MediaPipe Image Segmenter models [V] (Pixel 6, CPU / GPU): SelfieSegmenter 256x256 33 / 35 ms; HairSegmenter 512x512 58 / 52 ms; **SelfieMulticlass 256x256 (background, hair, body-skin, face-skin, clothes, accessories) 218 / 71 ms**; DeepLab-V3 257x257 124 / 103 ms. Model cards are PDFs (Selfie, Hair, Multiclass) at storage.googleapis.com/mediapipe-assets/ (links on https://developers.google.com/edge/mediapipe/solutions/vision/image_segmenter). I did not read the cards' licence/data sections: **open item** [verify before release]. Library is Apache-2.0 [M]. Resolution is low (256): the body-part edges must come from refinement against the photo.
- Portrait matting: MODNet (above), RVM (**GPL-3.0** [V] -> reject; weights/data not stated), PP-Matting (PaddleSeg, Apache-2.0 code [M]; weights/data [M] unverified), P3M (dataset: privacy-preserving portraits, academic [M]; BiRefNet trains on it [V]).
- Human parsing: SCHP (MIT code [M]; trained on LIP / ATR / Pascal-Person-Part, research datasets [M]), **Sapiens: weights CC BY-NC 4.0 on every HF model card [V via search], REJECT.**
- Face parsing: BiSeNet face-parsing weights (zllrunning, MIT code [M]) are trained on **CelebAMask-HQ, "non-commercial research purposes only", restricts commercial use of images and derived data [V]** -> REJECT. FaRL (MIT [M], also CelebAMask-trained for parsing) -> REJECT. SegFace [M] same data problem.
- Hair: MediaPipe HairSegmenter (above); otherwise from Multiclass.
- Apple: see section 2.

### 1d. Sky and scene parsing

- SegFormer (NVIDIA): the HF card for b0 ADE20K-512 says licence "other" -> the NVlabs repo licence [V]; that licence is non-commercial [M]. b0 = 3.75M params, 512x512 [V]. **Reject** (weights + ADE20K).
- ADE20K: images "only for non-commercial research and educational purposes", employer-bound for for-profit companies; annotations/software BSD-3 [V: https://ade20k.csail.mit.edu/terms/]. Anything trained on it (SegFormer-ADE, Mask2Former-ADE, OneFormer-ADE) is tainted. **Reject.**
- COCO-Stuff (has a "sky-other" class): annotations CC BY 4.0, images Flickr with mixed licences [M]: grey, but the usual practice.
- Mask2Former / OneFormer: code licences MIT / Apache with Swin / DiNAT backbones [M]; too heavy for phone (>40M params) [M].
- I found **no dedicated, clearly commercially-licensed, mobile sky segmentation model** (not verified exhaustively; repo hits like "SkyAR"/"sky segmentation unet" are small research repos with unspecified data). Recommendation: train our own ~1-3M param U-Net/MobileNetV3-LR-ASPP sky model on (a) COCO-Stuff sky + OpenImages-style data with permissive images, (b) pseudo-labels from our own pipeline, and (c) photos we own, then publish under our terms. Cheap compared to the legal risk. Classical sky prior (blue/gradient/top-of-image) + guided filter works as a fallback.

### 1e. Depth

| Model | Code | Weights | Data | Verdict |
|---|---|---|---|---|
| Depth Anything V2 Small (~25M [M]) | Apache-2.0 [M] | **Apache-2.0** [V HF card] | 595K synthetic labelled + 62M+ unlabeled real [V] | usable; relative depth, fine for depth-range mask and blur |
| Depth Anything V2 Base / Large | | **CC BY-NC 4.0** [V Base] | same | REJECT |
| MiDaS v3.1 small | MIT [M] | MIT [M] | mixed datasets, some restrictive [M] | possible, verify |
| Depth Pro (Apple) | Apple sample-code licence [M] | repo says "LICENSE", type not stated [V]; Apple research terms [M] | | REJECT unless verified; 0.3 s per 2.25 MP on a GPU [V]: not phone-class |
| Marigold | Apache-2.0 [M] | OpenRAIL++-M [M] | SD-based | REJECT (diffusion, heavy, use restrictions) |
| Apple Vision / ARKit depth | system | system | | iOS: depth only from capture (HEIC) [M] |

Sources: https://huggingface.co/depth-anything/Depth-Anything-V2-Small , https://huggingface.co/depth-anything/Depth-Anything-V2-Base , https://github.com/apple/ml-depth-pro . Note: our inputs are RAWs, so no capture depth; a monocular net is the only option.

### 1f. Refinement to full resolution

Constraint: network output is 256-1024 px, image is 40-60 MP. Standard techniques:
- **Guided filter** (He, Sun, Tang, ECCV 2010 / TPAMI 2013) and **Fast Guided Filter** (He and Sun 2015, subsample + local linear model, upsample coefficients). Cost is box filters only: O(N), ideal for compute shaders. Use the full-res (or tile) image luma/RGB as guide; coefficients (a, b) computed at low-res and bilinearly upsampled give edge-aligned alpha at any resolution. [M for paper details; algorithm is standard]
- Joint bilateral upsampling (Kopf et al. 2007) [M]; guided-filter upsampling generally beats it and is simpler on GPU.
- Matting refinement nets (e.g. BiRefNet's multi-scale refinement, MODNet fusion branch, "Mask-guided matting") are model-internal; patch-based refinement nets exist [M] but add a second network.
- Typical editor practice (Lightroom/Capture One/Photoshop select-subject output a mask at preview scale then re-edge to the image; exact method not public [unverified]).
- Proposed for Dapple: (1) net at 1024 long-edge (or 512 for person); (2) upsample logits bilinearly to preview res (2-4 MP), run fast guided filter (radius ~ 2-4% of short edge, eps ~1e-3-1e-2 [E]) with the colour preview as guide; (3) at export, run the same filter tile-wise (tiles + halo = radius) at full res, producing the mask on the fly inside the tile pipeline, not stored at 60 MP; (4) user controls: feather, contrast/threshold, invert, add/subtract brush; (5) cache the low-res logits and the (a, b) coefficients per mask, not the 60 MP plane.

## 2. Platform-provided alternatives

**Apple Vision** (on device, no weights to ship, free with OS): VNGeneratePersonSegmentationRequest (iOS 15+ [M]); VNGenerateForegroundInstanceMaskRequest and VNGeneratePersonInstanceMaskRequest (iOS 17+, macOS 14+; returns VNInstanceMaskObservation with per-instance masks scaled to the image [V partly: the doc page loaded as title only, details from model knowledge]). Runs locally [M]. Not offered: sky / hair / skin / teeth mattes for arbitrary images: those are AVSemanticSegmentationMatte attached **at capture** in HEIC [M]; a RAW from another camera has none. iOS 17 requires iPhone XS/XR (A12) or newer [M], so an A11 phone (iPhone 8/X) stays on iOS 16 and has only person segmentation, no foreground instances -> need a bundled-model fallback there. Doc: https://developer.apple.com/documentation/vision/vngenerateforegroundinstancemaskrequest

**Android ML Kit** Subject Segmentation: beta, "no SLA or deprecation policy", artifact `play-services-mlkit-subject-segmentation:16.0.0-beta1`, **unbundled: model downloaded via Google Play services**, minSdk 24, advises >=512x512 input [V: https://developers.google.com/ml-kit/vision/subject-segmentation/android]. Selfie Segmentation (ML Kit): beta, CPU, 20+ FPS, same terms [V: https://developers.google.com/ml-kit/vision/selfie-segmentation]. China: Play services usually absent on OPPO/Chinese-market phones [M], so unusable as the only path. ML Kit's terms of service apply to the SDK; not read.

**MediaPipe Tasks**: Image Segmenter and Interactive Segmenter are standalone .task bundles, not GMS dependent [M]; Flutter bundles MagicTouch [V]. Library Apache-2.0 [M]. Model-card licence/data terms: open item. Latencies in 1b/1c [V].

## 3. Runtimes

| Runtime | Licence | Mobile accelerators | Notes |
|---|---|---|---|
| ONNX Runtime Mobile | MIT [M] | XNNPACK (CPU), NNAPI, QNN (Qualcomm), Core ML EP (marked preview) [V: https://onnxruntime.ai/docs/execution-providers/] | one .onnx for both OSes; reduced-operator custom build to cut size (a few MB [E]); NNAPI deprecated Android 15 [V: https://developer.android.com/ndk/guides/neuralnetworks] |
| LiteRT (TFLite) | Apache-2.0 [M] | GPU delegate (OpenCL/GLES), Core ML delegate, NNAPI-successor path | Google recommends its GPU runtime over NNAPI [V]; needs TFLite conversion (ai-edge-torch); SAM-like attention ops conversion risk [E] |
| NCNN | BSD-3 [M] | Vulkan | small, no dependency; pnnx converter; good with Mali/Adreno; ViT ops need care |
| MNN | Apache-2.0 [M] | OpenCL / Vulkan / Metal / CPU ARM82 | strong in China ecosystems; ONNX in |
| ExecuTorch | BSD-style [M] | XNNPACK, Core ML, Qualcomm QNN, Vulkan | newer, PyTorch-native |
| Core ML | system | ANE + GPU | best on iPhone; convert ONNX/PyTorch with coremltools; EdgeSAM already ships .mlpackage [V] |

Recommendation: ONNX as the shared format; ORT Mobile with XNNPACK on Android (QNN optional on Snapdragon later), ORT with Core ML EP on iOS, or direct Core ML for the heaviest net. Rationale: one conversion path, MIT licence, fixed-shape static models. Performance numbers for mid-range Snapdragon / A11-A12 are not published for these nets: **measure, do not assume**. Known data points: MediaPipe on Pixel 6 and MagicTouch on Pixel 10 above [V]; EdgeSAM 38.7 FPS on iPhone 14, MobileSAM 4.9 FPS on iPhone 14 [V]; so an A11/A12 encoder pass for MobileSAM is likely 0.5-2 s [E].

**Run the nets inside our own compute HAL?** Not realistic for v1: needs a conv/attention/normalisation/upsample operator library with fp16, layout transforms, weight packing, per-vendor tuning, and quantised kernels; vendor NPUs (ANE, Hexagon) are unreachable from Vulkan/GLES/Metal compute anyway; GPU cycles compete with the render pipeline; ncnn-Vulkan already does that job. Keep the HAL for pre/post (resize, normalise, guided filter, mask apply). Optional later: share Vulkan device with ncnn/ORT, or zero-copy via AHardwareBuffer / IOSurface.

## 4. Shipping editors (public information only)

- Lightroom mobile masking (Select Subject, Sky, Background, People with parts): Adobe's help page was blocked (HTTP 403) when I tried; whether processing is on-device or cloud is **unverified**. My recollection: Select Subject / Sky run on device on recent hardware, "Select People" parts detection is a newer model, and some AI features (Generative Remove) are cloud [M, unverified]. Adobe help URL tried: https://helpx.adobe.com/lightroom-cc/using/masking-mobile.html
- Snapseed: Selective, brush and control points; no AI mask known [M].
- Photoshop Express: has cloud-assisted features; unverified.
- Pixelmator Photo: Core ML based "ML" tools on Apple platforms [M].
- Darkroom: uses Apple Vision / depth-based masks (subject, background, sky from captured mattes) [M, unverified].
- 醒图 / 美图秀秀: mix of on-device (portrait, hair, skin) and cloud (generative) [M, unverified]; domestic apps usually ship their own models via MNN/NCNN or vendor NPU SDKs [M].

## 5. Recommendation for Dapple

Licence table (summary of everything usable / rejected):

| Candidate | Code | Weights | Training data | Verdict |
|---|---|---|---|---|
| MobileSAM | Apache-2.0 | Apache-2.0 [M] | SA-1B (Research License, 1% subset) | USE v1, data flag |
| SAM 2 tiny | Apache-2.0 | Apache-2.0 | SA-V (CC BY 4.0 [M]) + SA-1B | v2 candidate |
| MediaPipe Interactive (MagicTouch) | Apache-2.0 | card unread | Google | USE if card allows; Android fallback / second option |
| MediaPipe Selfie Multiclass / Hair | Apache-2.0 | card unread | Google | USE (person parts), verify cards |
| Apple Vision | n/a | system | n/a | USE on iOS 17+ |
| ML Kit Subject Seg | proprietary SDK | Google | n/a | NOT for China (Play services) |
| U²-Netp | Apache-2.0 | Apache-2.0 | DUTS | fallback subject, data grey |
| BiRefNet tiny/lite | MIT | MIT | DIS5K, DUTS, HRSOD... | candidate, legal review + phone benchmark |
| MODNet | Apache-2.0 | Apache-2.0 | PPM etc. | portrait fallback |
| Depth Anything V2 Small | Apache-2.0 | Apache-2.0 | synthetic + pseudo-labels | USE (later) |
| Own sky model | ours | ours | COCO-Stuff + permissive | BUILD |
| RMBG-1.4 / 2.0 | | CC BY-NC / BRIA | | REJECT |
| EdgeSAM | | NTU S-Lab | | REJECT |
| Depth Anything V2 Base/Large | | CC BY-NC 4.0 | | REJECT |
| Sapiens | | CC BY-NC 4.0 | | REJECT |
| SegFormer (NVIDIA) / ADE20K-based (Mask2Former, OneFormer) | NC [M] | | ADE20K NC research [V] | REJECT |
| BiSeNet face-parsing / FaRL / SegFace (CelebAMask-HQ) | | | NC research only [V] | REJECT |
| RVM | GPL-3.0 | | | REJECT (copyleft) |
| FastSAM | AGPL-3.0 [M] | | | REJECT |
| Depth Pro, Marigold | research / OpenRAIL [M] | | | REJECT |
| BEN / BEN2 | | unverified | | REJECT until verified |

First set of masks:
1. No-AI: brush, linear, radial, luminance range, colour range. Ship first.
2. Tap-to-select: MobileSAM (encoder ~5M, decoder ~3.9M [V]) on both platforms via ORT, or MagicTouch via MediaPipe. Decide by on-phone benchmark; keep the interface identical (image embedding + point/box -> 256 logits).
3. Person (+ parts: hair, skin, face skin, clothes): Android MediaPipe Selfie Multiclass (256x256); iOS Vision person instance mask for the person, Multiclass bundle for parts. Fallback for A11: bundled Multiclass.
4. Subject: iOS Vision foreground instance mask (iOS 17+); Android: BiRefNet-tiny/U²-Netp or SAM with an auto box (centre-weighted). Choose after benchmark and legal review.
5. Sky: own small model (+ classical prior fallback).
6. Depth range (v2): Depth Anything V2 Small, 518 px input [M].

Delivery: bundle only the cheap/essential (MediaPipe multiclass, sky net, ~20 MB [E]); download MobileSAM encoder/decoder and depth on first use (~40-60 MB [E]) from our own CDN (China-reachable mirror needed; Hugging Face is unreliable in China [M]). Verify checksums.

Latency expectations on mid-range Android (Snapdragon 6/7-class) [E, not measured]: multiclass 100-250 ms; sky 50-150 ms; MobileSAM encoder 0.7-2 s once per image then decoder 10-40 ms per tap; BiRefNet-tiny 1024 likely >2 s. Old iPhone (A11/A12): Vision 100-400 ms; MobileSAM Core ML 0.3-1 s [E]. Refinement on GPU: guided filter at preview ~10-30 ms; full-res tile pass folded into export.

Node-graph integration: a Mask node outputs one single-channel plane (R8 or R16F, display-space or image-space coordinates, plus transform for crop/rotate/lens correction so it follows geometry). Local-adjustment nodes take (image, mask, params) and compute `out = mix(in, adjusted(in), mask)` in the node's working space. Mask combinators (add, subtract, intersect, invert, feather) are graph nodes. AI masks are baked results (low-res logits + metadata + model id/version) stored in the edit sidecar, so the edit is reproducible and re-runnable at export. Aligns with the project's bake/run split.

Risks:
- Legal: training-data terms of "permissive" models (DUTS/DIS5K/HRSOD/SA-1B); get legal review; keep model files swappable; keep a record of source/licence per model; prefer own-trained models for core paths.
- Android in China: no GMS -> use MediaPipe/ORT only, host models on a domestic CDN.
- A11 and older iPhones: Vision features absent below iOS 17.
- Quality: low-res nets make hair/fine edges mushy; refinement is mandatory and cannot invent edges.
- Latency and memory (SAM encoder activations) on 3-4 GB phones; keep ORT sessions short-lived.
- Binary size growth (ORT + MediaPipe + two runtimes): keep to ORT only where possible, MediaPipe only if its models are chosen.
- ONNX conversion of attention models on NNAPI/CoreML EP can silently fall back to CPU; benchmark per device.
- Unverified items in this note ([M]) must be checked, especially MediaPipe model card terms, EfficientViT-SAM / SlimSAM weights, Lightroom/Darkroom behaviour, and exact patents on the guided filter [M, none known to me].

## Sources index
https://huggingface.co/briaai/RMBG-2.0 , https://huggingface.co/ZhengPeng7/BiRefNet , https://github.com/ZhengPeng7/BiRefNet , https://github.com/xuebinqin/U-2-Net , https://github.com/ZHKKKe/MODNet , https://github.com/PeterL1n/RobustVideoMatting , https://github.com/ChaoningZhang/MobileSAM , https://raw.githubusercontent.com/chongzhou96/EdgeSAM/master/README.md , https://github.com/facebookresearch/segment-anything , https://github.com/facebookresearch/sam2 , https://github.com/mit-han-lab/efficientvit , https://huggingface.co/depth-anything/Depth-Anything-V2-Small , https://huggingface.co/depth-anything/Depth-Anything-V2-Base , https://github.com/apple/ml-depth-pro , https://huggingface.co/nvidia/segformer-b0-finetuned-ade-512-512 , https://ade20k.csail.mit.edu/terms/ , https://github.com/switchablenorms/CelebAMask-HQ , https://github.com/facebookresearch/sapiens (+ HF sapiens cards), https://developers.google.com/edge/mediapipe/solutions/vision/image_segmenter , https://developers.google.com/edge/mediapipe/solutions/vision/interactive_segmenter , https://developers.google.com/ml-kit/vision/subject-segmentation/android , https://developers.google.com/ml-kit/vision/selfie-segmentation , https://onnxruntime.ai/docs/execution-providers/ , https://developer.android.com/ndk/guides/neuralnetworks , https://developer.apple.com/documentation/vision/vngenerateforegroundinstancemaskrequest
