# AI models for masks and depth: licences verified, measurements, runtime choice

Date: 2026-11-14. Follow-up to `ai-masks.md` and `portrait-retouch.md`, which left the licence facts unverified and measured nothing.
Marking: **[V]** = read in a primary document fetched this session (cached under `tools/_dl/ai-models/lic/`); **[M]** = memory / secondary source, not verified; **[X]** = measured here.
Engineering research, not legal advice. A model-card "out of scope" line is not a licence term, but it is listed because a lawyer will want to see it.

Test machine: Intel Core i7-14700KF (8 P + 12 E cores, AVX2, Windows 10), ORT 1.32.0 CPU EP, ai-edge-litert 2.3.0 (XNNPACK), Python 3.13 venv in `tools/py-ai/`. Threads were not pinned. **Desktop x86 numbers are not phone numbers**: use them for relative cost and for "does it run / is it correct", never as a phone latency promise. Int8 models in particular are slow on x86 and are normally fast on ARM.

---

## 1. Corrected licence table

Verdicts: **usable** = permissive licence stated by the owner on a primary document and no disclosed third-party data with restrictive terms; **usable with stated uncertainty** = permissive code/weights licence but training data of unknown or research-only terms; **not usable** = non-commercial terms found, or not obtainable.

### 1a. MediaPipe model files (model cards fetched as PDFs from `storage.googleapis.com/mediapipe-assets`)

| Model (file) | Model card | Licence of the MODEL [V] | Training-data statement [V] | Restrictions on the card [V] | Verdict |
|---|---|---|---|---|---|
| Face Landmarker: BlazeFace short-range (in `face_landmarker.task`) | `.../MediaPipe%20BlazeFace%20Model%20Card%20(Short%20Range).pdf` | "LICENSED UNDER Apache License, Version 2.0" | "trained and evaluated on consented images of people using a mobile AR application ... captured with smartphone cameras" | not for life-critical decisions; primary application "AR entertainment" | usable |
| Face Mesh v2 | `.../Model%20Card%20MediaPipe%20Face%20Mesh%20V2.pdf` | Apache-2.0 (same header) | images in a diverse capture set across 17 geographic subregions; z from synthetic data | same | usable |
| Blendshape v2 | `.../Model%20Card%20Blendshape%20V2.pdf` | Apache-2.0 | lab captures with reconstructed "GHUM Mesh" (Google's own human model), "Blendshapes for the canonical GHUM Mesh were designed by an artist"; "millions of samples generated" | same | usable (data is Google-internal) |
| Selfie Multiclass 256x256 (`selfie_multiclass_256x256.tflite`) | `.../Model%20Card%20Multiclass%20Segmentation.pdf` (model date May 10, 2023) | Apache-2.0 | the card only says "trained on thousands of samples with diverse identities"; the sentence is copy-pasted from the face cards and mentions blendshapes. Real data not described | "not intended for ... applications that require pixel perfect masks"; "Any form of surveillance or identity recognition is explicitly out of scope" | usable (data undescribed but Google-internal; card quality is poor) |
| Selfie Segmenter (`selfie_segmenter.tflite`) | `.../Model%20Card%20MediaPipe%20Selfie%20Segmentation.pdf` | Apache-2.0 | "consented images of people using a mobile AR application captured with smartphone cameras" | same surveillance line; "Some thin features of humans such as fingers might occasionally be missed" | usable |
| Hair Segmenter (`hair_segmenter.tflite`) | `.../Model%20Card%20-%20Hair%20Segmentation.pdf` (v2019-01-14) | "Licensed under Apache License, Version 2.0." | only "trained and tested thoroughly across various in-the-wild smartphone camera conditions"; no dataset named | "not suitable for people too far away (further than 5 feet)"; headwear and mohawks unreliable | usable; model is old and weak (section 2) |
| Interactive Segmenter "MagicTouch" (`interactive_segmentation.task`, int8) | `.../Model%20Card%20MagicTouch.pdf` (Mar 13, 2023) | Apache-2.0 | "consented images of people using a mobile AR application", "images captured by smartphone"; object classes beyond people not described | "Human segmentation that is not consented to by the human being segmented is out of scope and not enabled by this technology"; "Any form of surveillance or identity recognition ... out of scope" | usable. Flag the consent sentence to the lawyer: Dapple edits photos of people who may not have consented |
| DeepLab v3 (`deeplab_v3.tflite`) | none found on the solution page; it links Kaggle `tensorflow/deeplabv3` (page did not render licence text) | **not obtained** | VOC-style 21 classes (observed in the file) | n/a | not usable until a card is read. Not needed: we do not plan to ship it |

Other MediaPipe facts [V]: the `mediapipe` repo `LICENSE` is Apache-2.0 (`raw.githubusercontent.com/google-ai-edge/mediapipe/master/LICENSE`, 2026-11-14). The solution pages state "the content of this page is licensed under the Creative Commons Attribution 4.0 License, and code samples are licensed under the Apache 2.0 License" (that covers the docs, not the models). The models sit in a Google storage bucket with no separate licence file; the Apache-2.0 statement lives on each model card. What removes the residual uncertainty: ask Google (MediaPipe GitHub issue) for a written confirmation that the Apache-2.0 line on the model cards covers the `.tflite` bytes, and ship the cards in the app's licence screen.

### 1b. Promptable segmentation

| Model | Code licence | Weights licence | Training data | Verdict |
|---|---|---|---|---|
| SAM (Meta) | Apache-2.0 [V] | "The model is licensed under the Apache 2.0 license" (`facebookresearch/segment-anything` README, "License" section) [V] | SA-1B. Meta's page: licence "Limited; see full license language for use", intended use "Research purposes only", "It is intended to be used for computer vision research for the purposes permitted under our Data License." (`https://ai.meta.com/datasets/segment-anything/`, fetched 2026-11-14) [V]. The full "SA-1B Dataset Research License" text sits behind a JS download gate: **not obtained**. README: "By downloading the datasets you agree that you have read and accepted the terms of the SA-1B Dataset Research License" [V] | too big anyway |
| MobileSAM | Apache-2.0 (`LICENSE`, GitHub API spdx `Apache-2.0`) [V]. README has no licence sentence of its own | no separate weights statement in the README [V]. The HF mirror `dhkim2810/MobileSAM` carries metadata `license: mit` [V], the community ONNX `Acly/MobileSAM` also `license: mit` [V]: **the owner's own statements disagree (Apache vs MIT)**, both permissive | distilled from SAM's encoder on 1% of SA-1B (README, [V in the earlier note]) | usable with stated uncertainty |
| EfficientSAM | Apache-2.0 [V] | HF `yunyangx/EfficientSAM` `license: apache-2.0` [V]; checkpoints in the GitHub repo, no extra statement | SA-1B + ImageNet [M] | usable with stated uncertainty |
| EfficientViT-SAM | Apache-2.0 [V] | HF `mit-han-lab/efficientvit-sam` `license: apache-2.0` [V] | README: trained on SA-1B, COCO2017 and LVIS annotations [V] | usable with stated uncertainty (COCO image terms per image) |
| SlimSAM | Apache-2.0 [V] | HF `Zigeng/SlimSAM-uniform-77`, `Xenova/slimsam-77-uniform` `license: apache-2.0` [V] | README: "We use the original SA-1B dataset in our code" [V] | usable with stated uncertainty |
| RepViT-SAM | Apache-2.0 [V] | `repvit_sam.pt` is a GitHub release asset; no weights statement [V] | SA-1B [M] | usable with stated uncertainty, weights licence implicit only |
| NanoSAM (NVIDIA) | Apache-2.0 (GitHub API; the `LICENSE` raw path 404 but repo metadata says Apache-2.0) [V] | none stated; encoder/decoder ONNX are Google-Drive links [V] | distilled on COCO 2017 images [V from README steps] | not a target (TensorRT/Jetson); skip |
| MagicTouch (see 1a) | Apache-2.0 | Apache-2.0 on the card | Google-internal consented data | **usable, and measured equal in quality to MobileSAM here (section 2)** |

What Meta's Apache-2.0 weights imply for distilled weights: Meta licenses the SAM *weights* Apache-2.0 and the *data* for research only. Nothing in the fetched documents says the data licence propagates to models trained on it, and nothing says it does not. Every SAM-derived student (MobileSAM, SlimSAM, EfficientSAM, EfficientViT-SAM, RepViT-SAM) is in the same position: permissive weights licence from its authors, a data licence whose full text we could not read. This is the grey zone the earlier note called it. **What would remove it:** the full SA-1B licence text read by a lawyer, or a written answer from Meta (`segment-anything@meta.com` is the contact on the page) or simply choosing MagicTouch, which has no such dependency.

### 1c. Depth

| Model | Weights licence | Data | Verdict |
|---|---|---|---|
| Depth Anything V2 **Small** | "Depth-Anything-V2-Small model is under the Apache-2.0 license. Depth-Anything-V2-Base/Large/Giant models are under the CC-BY-NC-4.0 license." (`DepthAnything/Depth-Anything-V2` README "LICENSE", 2026-11-14) [V]; HF cards `depth-anything/Depth-Anything-V2-Small` and `onnx-community/depth-anything-v2-small` `license: apache-2.0` [V] | card: "trained from 595K synthetic labeled images and 62M+ real unlabeled images" [V]. Dataset names (Hypersim, vKITTI2 ... SA-1B, ImageNet-21K, Places365 ...) are from the paper [M]. The larger siblings are NC and Small was distilled from the big teacher [M], which the licensor evidently did not treat as a constraint | usable with stated uncertainty (small) |
| MiDaS v2.1 small | MIT (`isl-org/MiDaS` LICENSE, Intel ISL 2019) [V] | trained on a mix: README lists ReDWeb, DIML, Movies, MegaDepth, WSVD, TartanAir, HRWSI, ApolloScape, BlendedMVS, IRS, KITTI, NYU Depth V2 [V]; several of these are research-only | usable with stated uncertainty, but measured worse than DAV2 and not worth it |

### 1d. Subject / foreground

| Model | Code / weights licence | Training data and its terms | Verdict |
|---|---|---|---|
| U²-Net / U²-Netp | Apache-2.0 `LICENSE` [V]; weights Google-Drive links, no separate statement [V]; `u2netp.onnx` was fetched from `danielgatis/rembg` release v0.0.0 (a third party re-host) | DUTS-TR. DUTS page: images "collected from the ImageNet DET training/val sets"; "Copyright claim of the ground-truth annotation: All rights reserved by the original authors of DUTS Image Dataset." (`saliencydetection.net/duts/`) [V]. No licence grant at all. ImageNet's own terms (research / non-commercial) [M, not re-fetched] | usable with stated uncertainty (data: no grant, ImageNet-derived images) |
| IS-Net (DIS) | code Apache-2.0 [V] | **DIS5K Terms of Use (PDF in the repo)**: "The Dataset is available for non-commercial use in research or educational purpose ... Without permission from the original authors, commercial use of this dataset is prohibited even after copying, editing, processing or any operations of this database." [V] | **not usable** without the authors' permission. `isnet-general-use.onnx` (rembg) is a community retrain whose data is undisclosed; also not usable |
| BiRefNet (+ lite) | MIT `LICENSE`; HF `ZhengPeng7/BiRefNet`, `BiRefNet_lite`, `onnx-community/BiRefNet_lite-ONNX` `license: mit` [V] | trained on DIS5K, DUTS, HRSOD, UHRSD, P3M-10k ... [V from README earlier]. DIS5K is explicitly non-commercial (above). README itself warns RMBG-2.0 weights are "only non-commercial use" [V] | **not usable** while DIS5K is in the training mix, unless permission is obtained. Also far too heavy (section 2) |
| MODNet | "The code, models, and demos in this repository ... are released under the Apache License 2.0" [V]; HF `Xenova/modnet` Apache-2.0 [V] | README: "trained on the datasets mentioned in our paper" (paper not read) [V]. The PPM-100 benchmark: images Flickr CC (commercial allowed), "alpha mattes ... released under CC BY-NC-SA 4.0" [V] | usable with stated uncertainty (training set unknown to us); portrait-only (section 2) |
| P3M-10k (dataset, for reference) | MIT: "The Dataset is under MIT license"; "images ... collected from websites with free use licenses" (release agreement PDF) [V] | n/a | the only subject/matting dataset found with an explicit permissive grant |
| HRSOD / UHRSD | no licence statement in the README fetched (HRSOD) or file not found (UHRSD repo paths 404) | unknown | treat as research-only until read |

**Realistic posture for subject models (known vs unknown).** Known: the weights of U²-Net, IS-Net, BiRefNet and MODNet are released under Apache/MIT by their authors, and models trained on research datasets are widely shipped in commercial apps. Known: DIS5K forbids commercial use even of processed data in plain words, which makes DIS-trained weights (IS-Net, BiRefNet) the worst of the group. Unknown: whether any court or the dataset owners treat trained weights as "processing of the database"; DUTS has no grant at all; MODNet's full training set. The practical way to avoid the whole question: do not ship a salient-subject network. Derive "subject" from MagicTouch/MobileSAM tap or box prompts, or use Apple Vision on iOS, or train our own small subject/sky model on data we are allowed to use (P3M-10k is MIT; own photos; CC0 raw.pixls.us previews).

### 1e. Runtimes

| Runtime | Licence | Third-party notices relevant to mobile |
|---|---|---|
| ONNX Runtime | MIT [V] (`microsoft/onnxruntime` `LICENSE`, first line "MIT License") | `ThirdPartyNotices.txt` (5060 lines, 92 notice blocks, fetched 2026-11-14): includes Eigen (Mozilla Public License 2.0, file-level copyleft, fine for binary distribution if the notice is shipped and Eigen files are unmodified), Abseil (Apache), pytorch/cpuinfo (BSD), dlpack (Apache), KleidiAI (Arm), mbedtls, smhasher, coremltools, Google test, Android Open Source Project. A search for GPL / SSPL / BUSL / CC-BY found nothing; only the phrase "commercial or non-commercial" inside permissive grants. Several entries (dawn, tfjs, emsdk, react-native, neural-speed) belong to other build targets and are not linked into the Android/iOS C library [M]. XNNPACK is a build dependency of the XNNPACK EP (BSD-3) [M] |
| LiteRT (TFLite) | Apache-2.0 [V] (`google-ai-edge/LiteRT` `LICENSE`) | not fetched; standard TFLite deps (flatbuffers, XNNPACK, ruy, farmhash, FFT2D) [M] |
| MediaPipe Tasks | Apache-2.0 [V] | bundles its own TFLite + XNNPACK etc. inside `libmediapipe_tasks_jni.so` [X, size below] |

---

## 2. Measurements (this PC)

Images: 6 CC0 embedded-JPEG previews from `build/rawsamples` (`person`=Canon EOS R10 `IMG_4361.CR3` rotated upright, `fruit`=Canon R5, `sunset`=Pixel 8 Pro DNG, `baretree`=Pentax K-1 II `_K1I9785.PEF`, `statue`=Panasonic GH6 `1-1.RW2`, `trees`=Fujifilm X-T50 `DSCF0118.RAF`), long edge 2048, in `build/ai-eval/src/`. Only one image has a person (a hard one: profile, cap, grey hair, mask hanging from the ear), so person-part conclusions rest on n=1. Latency: median of 3-15 runs after one warm-up, session creation excluded. Peak memory: peak working-set delta of a fresh process (includes ORT's default arena, so it overstates a tuned phone build).

| Model (file) | Runtime | Size MB | Input | 1 thread ms | 4 threads ms | Peak mem MB (1t) |
|---|---|---|---|---|---|---|
| U²-Netp (`u2netp.onnx`) | ORT | 4.6 | 320² | 429 | 111 | 543 |
| IS-Net general-use (not usable, measured for scale) | ORT | 178.7 | 1024² | 2146 | 630 | 1304 |
| MODNet fp32 (Xenova) | ORT | 25.9 | 512² | 301 | 101 | 293 |
| MODNet int8 | ORT | 6.6 | 512² | 396 | 185 | 363 |
| BiRefNet_lite fp32 (not usable, measured for scale) | ORT | 224.0 | 1024² | 14918 | 5068 | **12432** |
| Depth Anything V2 Small fp32 | ORT | 99.1 | 518² | 1007 | 364 | 384 |
| Depth Anything V2 Small int8 | ORT | 27.3 | 518² | 1022 | 369 | 419 |
| MiDaS v2.1 small | ORT | 66.8 | 256² | 71 | 19 | 200 |
| MobileSAM encoder (Acly ONNX) | ORT | 28.2 | 1024x683 | 1203 | 368 | 493 |
| MobileSAM decoder (per tap, includes upsample to 1024x683) | ORT | 16.5 | embeddings | 49 | n/m | n/m |
| SlimSAM-77 encoder fp32 (Xenova) | ORT | 23.3 | 1024² | 4854 | 1275 | **3658** |
| SlimSAM-77 encoder int8 | ORT | 8.9 | 1024² | 5079 | 1198 | 3623 |
| SlimSAM decoder (per tap) | ORT | 16.6 | embeddings | 47 | n/m | n/m |
| MediaPipe Selfie Multiclass 256 | LiteRT | 16.4 | 256² | 154 | 63 | 117 |
| MediaPipe Selfie Segmenter | LiteRT | 0.25 | 256² | 5.2 | 4.9 | 28 |
| MediaPipe Hair Segmenter | LiteRT | 0.78 | 512²x4 | 25 | 12.8 | 49 |
| MediaPipe DeepLab v3 | LiteRT | 2.8 | 257² | 35 | 18 | 46 |
| MediaPipe BlazeFace short-range | LiteRT | 0.23 | 128² | 1.9 | 1.7 | 24 |
| MediaPipe FaceMesh v2 | LiteRT | 2.5 | 256² | 6.4 | 3.5 | 34 |
| MediaPipe MagicTouch encoder int8 | LiteRT | 28.3 | 768² | 2739 | 1916 | 311 |
| MediaPipe MagicTouch decoder int8 (per tap) | LiteRT | 2.3 | 6 feature maps | 409 | 341 | n/m |

Reading the numbers: (1) The int8 variants gave no speed-up on x86 for ORT or LiteRT (MagicTouch's 2.7 s encoder is an x86 artefact; Google quotes 208 ms CPU on a Pixel 10 [V earlier]), so the only reliable output of this table is size, memory and relative cost. (2) BiRefNet-lite (5 s, 12 GB) and SlimSAM's 1024 encoder (1.3 s, 3.6 GB) are out for a phone. (3) Of the "heavy" models, DAV2 Small (0.36 s at 4 threads, ~0.4 GB) and the MobileSAM encoder (0.37 s) are the plausible ones; both are one-per-image jobs. (4) FaceMesh + BlazeFace together cost ~8 ms; Selfie Multiclass 63 ms. (5) MiDaS is 20x cheaper than DAV2 but visibly worse.

`build/ai-eval/bench.json` has all raw rows; scripts are in `scripts/ai-eval/` (`bench_all.py`, `eval_seg.py`, `eval_sam.py`, `eval_face.py`, `eval_magictouch.py`, `gf.py`, `gf_eval.py`, `ops.py`).

### 2a. Output sanity (I looked at every sheet)

Contact sheets (6 images each, tap points drawn as yellow circles): `build/ai-eval/sheet_<model>.jpg`. Per-image overlays at 1024 px: `build/ai-eval/<model>/<image>.png`. Raw masks at source resolution: `build/ai-eval/raw/`.

| Model | What I saw | Failure cases |
|---|---|---|
| U²-Netp | Good: person, fruit bowl contents, statue, the sunset tree pair, reflection included. Edges coarse (320 px) | selects only the trunk of the bare tree; poplar trunks in `trees` |
| BiRefNet_lite | Sharp on person, fruit, statue | **missed the sunset trees completely**; on the bare tree it selected a branch cluster with halo; useless at 5 s anyway |
| MODNet | Person fine | not a general-subject model: on `fruit` it selects the cloth, on `statue` half the statue, bare tree trunk |
| IS-Net (not usable) | Best general subject mask of the group | the sunset reflection is cut at the water line |
| Selfie Segmenter | Person, fruit, statue masked | fires on any salient object (fruit, statue, a red roof), false patches in trees; it is a person model |
| Selfie Multiclass 256 | On the hard profile photo: hair (magenta), face/skin, body/clothes, cap classes are in the right places | on non-person images it hallucinates skin classes on fruit and "clothes" on the statue: **needs a person gate** (face/body presence) before the parts are used |
| Hair Segmenter | highlights cap and hair region on the person | also marks sunset clouds as hair; the cap is labelled hair (card admits headwear is unreliable); output are logits, take `sigmoid(ch1-ch0)`; the 4th input channel is a prior, I fed zeros |
| BlazeFace + FaceMesh (own anchor decode + crop re-implementation) | detector box on the profile face at score 0.60 | **profile face: mesh face flag negative (rejected)**; false detection on the potato at 0.77 (mesh flag -7 rejects it, so the two-stage gate works); statue below threshold. Landmarks were not usable on this one real face, so face-landmark quality is **unmeasured**; needs frontal test faces (no frontal portrait among the CC0 samples) |
| MobileSAM (decoder after Acly ONNX encoder) | lemon, apple, sunset tree, statue, sky: correct, clean | tap on the face selects the **whole person** (face, cap, torso); tap on the bare-tree trunk selects the whole tree and neighbouring bushes (single-mask decoder, no part/whole choice) |
| SlimSAM-77 | same objects correct | face tap selected face plus part of neck (a better "part" choice than MobileSAM), tree again whole |
| MagicTouch | lemon, apple, tree, statue, sky: correct, mask agrees with MobileSAM (IoU 0.99 on the lemon) and shows the same whole-vs-part behaviour | click prior format is undocumented (see 3) |
| Depth Anything V2 Small | excellent detail: branches of the bare tree, statue volume, fruit bowl, water plane as a plane (reflection not mistaken for objects). int8 visually identical | none seen |
| MiDaS small | blobby; bare tree is a blur; person flat | loses all fine structure |

Take-aways: depth from DAV2 Small is clearly good enough; tap-to-select with MagicTouch or MobileSAM is good enough on object-like targets and ambiguous (whole vs part) on people and trees, so the UI needs a hierarchy control (more/less) or a second positive/negative tap; ML "subject" masks fail on at least one of six images each, so subject should come from tap/box prompts, not a standalone saliency net; people parts need a person gate.

### 2b. Refinement prototype (guided filter)

Implemented in `scripts/ai-eval/gf.py`: colour guided filter (He and Sun) with box filters via cumulative sums, plus the fast variant that solves at 1/4 resolution and applies a,b at full resolution. Tested by taking the **256 px low-res mask** (MobileSAM `low_res_masks`, Multiclass 256 probabilities), bicubic upsampling to the full 2048-long-edge preview and filtering with the preview as guide (r=16 px, eps=1e-3).

| Case | Time at 2048x1365..1542 (numpy, one core) |
|---|---|
| fast guided filter, s=4 | 220-330 ms |
| exact guided filter, s=1 | 2.2-2.7 s |

Edge crops (300 px, shown at 1.5x; the kept region is the masked part, magenta = removed): `build/ai-eval/gf_mobilesam256_trees.png` (sky vs poplar crowns), `gf_mobilesam256_sunset.png` (fir silhouette vs sunset sky), `gf_mobilesam256_baretree.png` (thin twigs vs sky), `gf_multiclass256_person.png` (cap vs grass).
Result, from looking at them: the filter snaps the blurry 64x-upsampled boundary onto real edges for solid shapes (individual leaf clusters and fir needles recovered in `trees` and `sunset`), s=4 is visually the same as exact s=1 at ~10x less time, and a soft halo remains where the edge is itself soft (the out-of-focus cap) which is correct behaviour. **Failure:** thin structure absent from the network mask cannot be recovered: in `baretree` SAM's mask is blobby with a checkerboard pattern from the 256 px logits and the guided filter smooths it but cannot find the twigs (needs matting-like refinement or a higher-res decoder). There is no fine hair against sky in the CC0 set, so hair-against-sky refinement is **not demonstrated**. The numbers are numpy on one core, a GPU version in the HAL would be box filters only.

---

## 3. Runtime recommendation (Part C)

### 3a. Evidence

| Item | Measured / found |
|---|---|
| ONNX Runtime Android AAR 1.32.0 (Maven Central, full build) | AAR 59.3 MB; arm64 `libonnxruntime.so` **36.3 MB raw (13.7 MB deflated)**; `libonnxruntime4j_jni.so` 0.12 MB (not needed if called from Dart FFI) [X] |
| ONNX Runtime "mobile" prebuilt 1.18.0 (last one published, discontinued after that) | AAR 7.2 MB; arm64 `libonnxruntime.so` **3.7 MB raw (1.5 MB deflated)** [X]. Its reduced operator set vs our needs is unverified |
| ORT custom minimal build | ORT docs: `--minimal_build` loads ORT-format models only and the kernels can be reduced to the models' operators [V]. A search snippet of the ORT mobile tutorial quotes 1.18 AAR 24.4 MB full vs 7.5 MB custom [M, secondary]. **I did not build one.** Our chosen ONNX models use 47 operator types in total (union of DAV2 Small, MobileSAM encoder+decoder, U²-Netp, MODNet): Add ArgMax BatchNormalization Cast Clip Concat Constant ConstantOfShape Conv ConvTranspose Cos Div Equal Erf Expand Flatten Floor Gather Gemm GlobalAveragePool InstanceNormalization LayerNormalization MatMul MaxPool Mul Not Pad Pow Range Reciprocal ReduceMax ReduceMean Relu Reshape Resize Shape Sigmoid Sin Slice Softmax Split Sqrt Squeeze Sub Transpose Unsqueeze Where [X, `ops.py`]. A custom build for this set is realistic in the 4-8 MB range [E]. |
| LiteRT Android AAR 2.3.2 (current Maven line) | AAR 7.6 MB; arm64 `libLiteRt.so` **5.6 MB raw (2.3 MB deflated)**; GPU accelerator add-on `libLiteRtClGlAccelerator.so` 3.3 MB raw [X] |
| LiteRT 1.4.0 (previous interpreter line) | AAR 8.3 MB; `libtensorflowlite_jni.so` 4.3 MB raw; GPU delegate `libtensorflowlite_gpu_jni.so` 2.5 MB raw [X] |
| MediaPipe Tasks Android 1.2.3 | `tasks-core` AAR 32.6 MB, arm64 `libmediapipe_tasks_jni.so` **16.0 MB raw (7.3 MB deflated)** + Java jars ~1.9 MB [X]. It carries its own TFLite and pipeline code in that library: a **second runtime** next to ORT, and Google-defined graph/calculator code we do not need |
| iOS | ORT `onnxruntime-c` pod archive 1.22.0: 47.1 MB zip (multi-slice xcframework, per-app arm64 size smaller, not measured) [X HEAD]; 1.32.0 pod archive URL 404. LiteRT / MediaPipe iOS sizes **not measured** (no Mac) |
| Accelerators | ORT: NNAPI EP "requires Android 8.1 or higher" (ORT docs [V]; the earlier note records Android's own deprecation of NNAPI in Android 15 [V earlier]), XNNPACK EP, QNN EP in a separate AAR whose last Maven Central release is 1.29.0 (stale vs 1.32.0) [X Maven], CoreML EP (listed in the preview tier on the EP overview page, ambiguous) [V]. LiteRT: GPU accelerator libs (OpenCL/GL) shipped in `litert-gpu`; MagicTouch card claims ~30 FPS on a Pixel 7 via TFLite GPU [V]. NPU delegates for OPPO's MediaTek/Qualcomm SoCs: **not evaluated** |

Can Dapple run MediaPipe `.task` models through plain LiteRT? **Yes, demonstrated.** `face_landmarker.task` and `interactive_segmentation.task` are zip files; I read the `.tflite` members as data and ran all of them in the plain `ai-edge-litert` Interpreter with no MediaPipe code. What you lose is MediaPipe's pre/post-processing, which we must re-implement: BlazeFace anchor decoding + rotated crop (about 60 lines, done in `eval_face.py`), the blendshape input from the 146 mesh points, and MagicTouch's click encoding. The last one is **undocumented**: the card says a 512x512x4 prior, the shipped v2 file takes a 768x768x3 tensor into the decoder next to five encoder feature maps; by experiment, a filled disk in **channel 0** gives a correct mask (IoU 0.99 vs MobileSAM on the lemon), channels 1 and 2 behave differently (probably negative click and previous mask) [X]. Re-implementing it blindly is a maintenance risk until we read MediaPipe's C++ source (the unauthenticated GitHub API was rate-limited, I could not fetch it). Two caveats: `hair_segmenter` and `selfie_segmenter` use MediaPipe custom ops (`Convolution2DTransposeBias`, `MaxPoolingWithArgmax2D`, `MaxUnpooling2D`) which the pip `ai-edge-litert` resolved but the Android LiteRT AAR may not (**unverified, test on device**); `selfie_multiclass`, face landmarker and MagicTouch use only standard TFLite ops [X]. TFLite union of ops across all models: 31 types.

Which single runtime runs ALL chosen models? Neither without conversion: the heavy models (Depth Anything V2 Small, MobileSAM/SlimSAM) are ONNX or PyTorch artefacts, the MediaPipe models are TFLite. ORT cannot read `.tflite`; LiteRT cannot read `.onnx`. Conversions needed: ORT-only needs 3 small standard-op TFLite files converted to ONNX (multiclass, mesh/detector/blendshape, MagicTouch) with `tf2onnx` (mature for plain conv nets; **not attempted here**, needs TensorFlow, which I did not install). LiteRT-only needs DAV2 Small (and MobileSAM if used) converted PyTorch to TFLite (`ai-edge-torch`; ViT attention and LayerNormalization conversion is **unverified**, and needs torch in an offline build env).

### 3b. Recommendation (provisional until one phone measurement)

**ONNX Runtime, built as a custom minimal build per platform (Android arm64, iOS arm64), XNNPACK on CPU, with the CoreML EP on iOS and (optionally, measured) NNAPI/QNN on Android; models shipped in ORT format.**

Why: (1) the models that need the most care (DAV2 Small, SAM family, our own sky model, any future subject model) all have ONNX paths today, while the MediaPipe models are small, use standard operators and convert in one direction that is well trodden; (2) one runtime and one model-file format in the app; (3) a custom build for our 47 operators removes the 36 MB full library problem (full ORT is 7x bigger than LiteRT; minimal brings it into LiteRT's range [E]); (4) no GMS dependence and a clean MIT licence with a short notice list; (5) it keeps iOS and Android on the same code. Do **not** add the MediaPipe Tasks AAR: its 16 MB JNI library duplicates a runtime and its API cannot take our own models.

Trade-offs, honestly: LiteRT is smaller out of the box (5.6 MB), has the better-documented mobile GPU path, and runs the MediaPipe models without conversion, but forces converting the ViT depth model to TFLite, which I could not test. ORT's mobile GPU story is weaker (NNAPI is on its way out, QNN is a separate stale AAR), so GPU acceleration of these networks on the OPPO phone is not something ORT gives for free; CPU+XNNPACK is the baseline (DAV2 Small 0.36 s at 4 x86 threads is an upper-end guess for ARM big cores, unmeasured). If phone measurements show CPU latency is the problem, revisit LiteRT + its GPU delegate for the specific model.

Model set this evidence supports for v1: **MagicTouch (via LiteRT-to-ONNX conversion, or MobileSAM as fallback) for tap-to-select; Selfie Multiclass 256 + BlazeFace/FaceMesh v2 for people parts, gated by a face/person check; Depth Anything V2 Small fp32 or int8 for depth; own sky model; guided filter at s=4 for edge refinement.** Do not ship: BiRefNet, IS-Net, MiDaS, SlimSAM 1024 encoder, hair segmenter (replace by multiclass hair class).

---

## 4. Open questions

1. Full text of the "SA-1B Dataset Research License" (JS-gated) and Meta's view on derived students; decides MobileSAM vs MagicTouch. Easiest exit: MagicTouch.
2. Written Google confirmation that the Apache-2.0 line on MediaPipe model cards covers the model files; whether "consent" in the MagicTouch card has legal weight for a photo editor.
3. DUTS/ImageNet and MODNet training-data terms (we did not read the MODNet paper); DAV2's distillation from the NC teacher.
4. MagicTouch click encoding: read MediaPipe's source (`interactive_segmenter` v2 graph) instead of relying on the channel-0 experiment; also what channels 1 and 2 do (negative taps, previous mask).
5. Face landmarks on frontal faces (none in the CC0 samples) and hair-against-sky edges are unmeasured; add 2-3 frontal portraits with a clear licence.
6. Build ORT minimal for arm64 with the 47 operators in ORT format and measure the real size; check whether the prebuilt 1.18 mobile package already covers them.
7. tf2onnx conversion of multiclass, face detector/mesh/blendshape and MagicTouch (does the int8 QDQ conversion survive?); `ai-edge-torch` for DAV2 as the alternative.
8. On the OPPO phone: DAV2 Small and MagicTouch/MobileSAM encoder latency and RAM on CPU vs GPU/NPU; whether the LiteRT Android AAR resolves MediaPipe's custom ops (hair, selfie segmenter) if those are kept.
9. iOS sizes (ORT pod per-arch, LiteRT, MediaPipe) need a Mac.
10. Sky model: no off-the-shelf clean one found; plan to train on permissively licensed data.

## 5. Files

- Licence documents and model cards read: `tools/_dl/ai-models/lic/` (PDF + text for all MediaPipe cards, DIS5K and P3M agreements, READMEs/LICENSEs, ORT ThirdPartyNotices).
- Downloaded model files (untrusted data, loaded only as weights): `tools/_dl/ai-models/{mp,mp_x,onnx,aar}/`; venv `tools/py-ai/` (onnxruntime 1.32.0, ai-edge-litert 2.3.0, numpy, Pillow, scipy, psutil, onnx). All are git-ignored.
- Scripts (new, uncommitted): `scripts/ai-eval/`. Outputs (git-ignored): `build/ai-eval/` (sheets, overlays, `gf_*.png` edge crops, `bench.json`, `raw/`).
