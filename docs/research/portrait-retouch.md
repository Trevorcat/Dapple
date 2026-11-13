# Portrait retouching on the phone: research notes (2026-11-14)

Legend: [V] = verified on the cited page during this research; [U] = unverified (from memory or secondary source; check before relying on it). Lawyer review is still needed before shipping.

Repo check: `docs/design/requirements.md` has no portrait / beauty / skin-smoothing requirement (only an unrelated hit at line 479 about wording). This is a new requirement.

## 0. Summary

- Recommended architecture: "masks first, classical retouch second". Only the masks need networks. All retouch maths (smoothing, spot healing, even tone, under-eye, whitening, face light) is classical and belongs in the engine's GPU pipeline, driven by masks plus normal sliders. This is the same model Lightroom uses (People masks, then ordinary sliders) [V, links in section 2].
- Commercially safe mask stack: MediaPipe Face Landmarker (478 landmarks, 52 blendshapes) plus MediaPipe multiclass selfie segmenter (background / hair / body-skin / face-skin / clothes / others, 256x256). The code is Apache-2.0 [V]. The model files' licence is NOT stated on the doc pages I read [U], so confirm from the model cards and the bundled LICENSE files. Eyes, iris, sclera, lips, teeth and brows come from landmark polygons, not from a parsing network.
- Face-restoration GANs (CodeFormer, GFPGAN, GPEN, RestoreFormer) have no clearly commercial option: CodeFormer is explicitly non-commercial [V]; the others depend on StyleGAN2 / FFHQ and have unclear or restricted weights. Recommendation: do not ship them; use mask-limited denoise and sharpen.
- Traps found [V]: InsightFace pretrained models are non-commercial research only; CelebAMask-HQ is non-commercial research only (so every BiSeNet face-parsing checkpoint trained on it is tainted); FFHQ metadata is CC BY-NC-SA and its images carry per-image Flickr licences including NC; Depth Anything V2 Base/Large/Giant are CC-BY-NC-4.0 (only Small is Apache-2.0); IC-Light's background removal (BRIA RMBG 1.4) is non-commercial.
- Total model download for the recommended stack: roughly 20 to 30 MB [U, order of magnitude; measure the real files].
- Blemish removal needs no network: detect with multi-scale local-contrast blobs inside the skin mask, heal with texture transfer plus low-frequency boundary interpolation (Poisson-style). LaMa / MI-GAN are unnecessary for skin spots and carry dataset-licence questions.
- Guardrails: natural default strengths, texture-retention by construction, no automatic reshaping, before/after toggle.

## 1. Retouching operations and how each is done well

### 1a. Skin

| Operation | Recommended method | Network needed? |
|---|---|---|
| Texture-preserving smoothing | Split into low and high frequency with an edge-preserving filter (guided filter, He et al. 2010 [U]; domain transform, Gastal and Oliveira 2011 [U]; or an a-trous wavelet / local Laplacian pyramid). Smooth only the mid/low bands (blotches, uneven shading); leave the finest bands (pores) at full or partly attenuated strength. Blend by skin mask times slider. Plain Gaussian frequency separation gives halos; use an edge-aware low band with radius tied to face size. | No |
| Blemish / spot removal | Detect: difference-of-Gaussian blobs, darker or redder than local skin, multi-scale, inside skin mask (also tap-to-heal at a user point). Heal: take texture (high band) from a nearby clean skin patch, fit the low band by harmonic/Poisson interpolation from the ring around the spot (OpenCV `seamlessClone` is the reference idea). Telea inpainting (OpenCV `inpaint`) is too blurry for skin; PatchMatch (Barnes 2009) works but Adobe holds patents on it [U]. | No |
| Even skin tone | In Lab/OKLab inside the skin mask, pull a/b (chroma) toward an edge-aware large-radius local mean; limit L change; strength slider. Capture One's "Even Skin" works on contrast, not hue; hue is a separate Skin Tone tool [V, alexonraw link in section 2]. | No |
| Shine / oil reduction | Mask = skin AND high L AND low chroma (specular); compress L toward the local mean with a soft shoulder, restore a little chroma. | No |
| Dark circles / redness | Under-eye region from eyelid landmarks (band below the lower lid, feathered): lift L toward adjacent cheek L, correct b (blue/yellow cast); redness: reduce a within skin by a capped amount. | No |
| Dodge and burn automation | Compute a mid-band luminance map inside the skin mask; flatten large uneven highlights and shadows toward the smoothed version, keeping the fine bands. Retouch4me ships this as a dedicated model [V, section 2]; a classical variant is acceptable because uneven shading is low-frequency. | No (a network only helps with semantic judgement) |

Everything in this table is purely classical given a good skin mask. The only network dependency is the mask.

### 1b. Face geometry, parts and masks

- MediaPipe Face Landmarker: 478 3D landmarks, 52 blendshape scores (optional), bundle = BlazeFace short-range detector 192x192 + FaceLandmarker-V2 256x256 + blendshape model [V: https://developers.google.com/edge/mediapipe/solutions/vision/face_landmarker]. Page states no licence and no latency. MediaPipe repo code: Apache-2.0 [V: https://github.com/google-ai-edge/mediapipe]; model-file terms [U]. The landmark set includes iris points (468 to 477) [U], so iris circle, eye polygon (so sclera = eye polygon minus iris), lips outer/inner polygons (so teeth = inner-lip polygon filtered by colour), brows and the face oval all come from one cheap model.
- MediaPipe multiclass selfie segmenter (SelfieMulticlass 256x256, float32): classes 0 background, 1 hair, 2 body-skin, 3 face-skin, 4 clothes, 5 others/accessories [V: https://developers.google.com/edge/mediapipe/solutions/vision/image_segmenter]. The model card PDF returned 404/redirect loops for me, so training data and licence stay [U]. Check the card before shipping. Face-skin is a single class: derive the precise skin mask as face-skin AND face-oval polygon MINUS eye / brow / lip / nostril polygons.
- Face detection alternatives: BlazeFace (inside MediaPipe); OpenCV YuNet: "All files in this directory are licensed under MIT" in OpenCV Zoo [V: https://github.com/opencv/opencv_zoo/tree/main/models/face_detection_yunet]; the training dataset is not named there [U: WIDER FACE terms are research-oriented; check]. YuNet only gives 5 keypoints, so it is a detector, not a mask source.
- Face parsing networks (BiSeNet etc.): the popular repo is MIT code [V: https://github.com/zllrunning/face-parsing.PyTorch], but its weights are trained on CelebAMask-HQ, which is "available for non-commercial research purposes only" and forbids using images or derived data commercially [V: https://github.com/switchablenorms/CelebAMask-HQ]. Verdict: reject unless retrained on clean data. FaRL: code MIT, no stated weight licence, pretrained on LAION-Face (50M image-text pairs, crawled web data) [V: https://github.com/FacePerceiver/FaRL]; its parsing heads are trained on CelebAMask-HQ / LaPa [U]. Reject.
- InsightFace: code MIT, but the pretrained models and their training data are "non-commercial research purposes only", and downloading via the Python library follows the same policy; commercial licences are by contact (updated 2025-11-24) [V: https://github.com/deepinsight/insightface]. Reject.
- dlib: code Boost licence [U]; the stock 68-point shape predictor was trained on iBUG 300-W data whose terms are non-commercial [U]. Reject.
- Eye / teeth whitening, eye enhancement: whitening = reduce chroma and yellow (b) in the sclera or teeth mask, raise L a little, clamp so the result never goes paper-white; iris clarity = modest local contrast and exposure in the iris mask; leave catchlights alone (protect pixels brighter than a threshold). Lip colour = a/b shift or hue-preserving saturation in the lip mask. Brow = density/contrast in the brow polygon. All classical.
- Liquify-style reshaping (slim face, big eyes): optional, NOT requested by the owner, and mostly a beauty-app feature. If ever added, make it manual only (user brush or per-slider), never automatic, default zero. Landmarks make it technically easy (mesh warp), so the risk is UX, not engineering.

### 1c. Relighting and background

- Background blur from depth: Depth Anything V2 Small (24.8M params) is Apache-2.0; Base (97.5M), Large (335.3M) and Giant are CC-BY-NC-4.0 [V: https://github.com/DepthAnything/Depth-Anything-V2]. The page does not describe the V2 training data [V]; the Small weights' provenance should be checked [U]. For a RAW editor the person mask from the segmenter (plus a manual radial/linear mask) is enough; depth blur is optional and out of scope for the first version.
- Learned relighting: Total Relighting (Google) and SwitchLight (Beeble) are not open [U]. DPR (Zhou et al. ICCV 2019) is research code with unclear licence and 512 px output [U]. IC-Light: code Apache-2.0, weights listed as iclight_sd15_*.safetensors (Stable Diffusion 1.5 based, size not stated on page), and its pipeline uses BRIA RMBG 1.4, which the README says is non-commercial (it suggests BiRefNet for commercial use) [V: https://github.com/lllyasviel/IC-Light]. A diffusion model is also far too heavy for a phone and rewrites pixels (generative). Reject.
- Practical "face light": mask (face-skin or face oval) times a soft elliptical falloff, applied as local exposure plus slight highlight/warmth, in linear light, with a direction control. Optionally use the landmark mesh depth (z) to bias the falloff, as an approximate relight. This is what photographers expect from a dodge brush anyway.

### 1d. Face restoration / enhancement

| Candidate | Code | Weights | Training data | Verdict |
|---|---|---|---|---|
| CodeFormer | S-Lab License 1.0: non-commercial only; commercial = contact the authors [V: https://github.com/sczhou/CodeFormer/blob/master/LICENSE] | same, non-commercial | FFHQ | Reject |
| GFPGAN | Apache-2.0 [V: https://github.com/TencentARC/GFPGAN] | no weight licence stated on page [V] | FFHQ plus a pretrained StyleGAN2 [V: README training section]. FFHQ metadata is CC BY-NC-SA 4.0 and images are Flickr licences including CC BY-NC; no use for face recognition [V: https://github.com/NVlabs/ffhq-dataset]. NVIDIA's StyleGAN2 licence is non-commercial [U] | Reject (unclear / tainted) |
| GPEN | no licence stated on page [V: https://github.com/yangxy/GPEN] | author says the public model "is not our best model due to commercial issues" and withdrew GPEN-BFR-2048 for the same reason [V] | FFHQ-style | Reject |
| RestoreFormer(++) | [U] | [U] | FFHQ | Reject (same FFHQ dependency; verify only if needed) |

Plain statement: I found no face-restoration GAN that is safely commercial. They also hallucinate facial detail (identity drift, "plastic" skin), which conflicts with a natural-taste editor. Alternative: within the face mask, apply mask-limited denoise (luminance and chroma, strength from ISO), and local sharpening/clarity on eyes, brows and lips (unsharp or a deconvolution-lite). Soft focus cannot be fixed by this, and that is acceptable. If a learned restorer is ever wanted, train a small one on commercially clean data (own licensed photos) with a plain L1/perceptual loss, not a GAN prior.

## 2. What shipping tools do (public information)

| Tool | What is public | On-device? |
|---|---|---|
| Lightroom / Lightroom Classic | People masking: Entire Person, Face Skin, Body Skin, Eyebrows, Eye Sclera, Iris and Pupil, Lips, Teeth, Hair (plus facial hair, clothes in some guides); Face Skin excludes eyes, brows, lips, teeth; options vanish if the feature is not visible (sunglasses, closed lips); then ordinary sliders (texture, clarity, shadows) per mask [V: https://helpx.adobe.com/lightroom-cc/web/edit-photos/apply-masks/mask-with-ai.html, https://photofocus.com/featured/masking-people-in-lightroom-faces-features-skin-smoothing-too/, https://community.adobe.com/t5/lightroom-classic-discussions/use-people-masking-for-specific-edits-in-lightroom-classic-quick-tips/td-p/13683492]. Skin smoothing is done by lowering texture/clarity in the Face Skin mask [V, photofocus]. | Desktop: on-device models [U]; cloud processing exists for some Lightroom mobile features [U] |
| Capture One (16.6+, 2025) | "Retouch Faces": auto face detection, Impact slider, Blemish removal, Dark Circles, Even Skin, Contouring; per-face or all faces; Even Skin works on contrast, hue is a separate Skin Tone tool; sources disagree whether Dark Circles is Studio-only [V search results: https://support.captureone.com/hc/en-us/articles/27336176639133, https://alexonraw.com/capture-one-ai-retouching-all-what-you-need-to-know/, https://www.digitalcameraworld.com/photography/photo-editing/capture-one-showed-me-an-early-glimpse-at-new-retouching-tools-and-i-think-its-a-huge-boon-for-portrait-photographers] | Desktop, local [U] |
| Evoto, Luminar Neo, Retouch4me, PixCake (像素蛋糕), 醒图 | Not researched in detail. Known from general knowledge only [U]: Retouch4me = separate specialised neural plugins (dodge and burn, skin tone, skin mask, eye brilliance, heal, etc.) on desktop; Evoto and PixCake are desktop/mobile AI batch retouch tools; 醒图 is a beauty-app. Whether each runs on-device is [U]. | [U] |

Take-away: the "masks plus normal sliders" model is the professional pattern, and Capture One's slider set (Impact, Blemish, Dark Circles, Even Skin) is a good template for what to expose.

## 3. Platform APIs instead of bundled models

- iOS Vision: `VNDetectFaceLandmarksRequest` finds eyes, mouth etc.; pupil locations from Revision 3 [V via secondary: https://developer.apple.com/documentation/vision/vndetectfacelandmarksrequest.md]; the often-quoted "76 landmarks" count is not verified [U]. `VNGeneratePersonSegmentationRequest` (iOS 15+, one mask for all people) and `VNGeneratePersonInstanceMaskRequest` (iOS 17+, separate masks, reportedly up to four people) [V via secondary: https://jejememe.github.io/ios/development,/vision/framework/2024/01/30/advanced-personseg.html]. These give person-level masks only, not face-skin.
- AVFoundation semantic segmentation mattes (`AVSemanticSegmentationMatte`: hair, skin, teeth, glasses): the Apple page did not render for me; the recollection is that they exist only when captured with `AVCapturePhotoOutput` with matte types enabled, device-dependent, and can be embedded in HEIC portrait photos [U]. A Sony A7R V / X-T50 RAW has no such matte, so this cannot be the primary source. It could be a bonus when importing iPhone portrait HEICs.
- Core Image: no skin-part segmentation that I could verify [U]. Use it at most for baseline filters; the engine already has its own pipeline, and a platform split would break cross-platform parity.
- Android ML Kit Face Detection: on-device, contours for face, eyes, brows, lips, nose [V: https://developers.google.com/ml-kit/vision/face-detection]. The page does not say whether the model is bundled or fetched via Google Play services, and says nothing about China [V]; from memory there are both a bundled and an unbundled (Play services) artifact [U]. Because the owner is in China and many devices lack Google services, avoid ML Kit.
- MediaPipe Tasks: bundled model files, no Play services, Apache-2.0 code [V]. Preferred on both platforms (the same .task / .tflite files on Android and iOS gives identical masks). Flutter plugin or direct native integration via the engine is a separate integration question [U].

## 4. Recommendation for Dapple

### Pipeline

1. Preview-resolution analysis (once per image, cached, invalidated on crop/rotate only): run the face detector and Face Landmarker on a downscaled image (about 1024 px long edge; the landmark model works on a 256 px face crop internally [V]). Run the multiclass selfie segmenter on a crop around each person (not the whole 60 MP frame squeezed to 256 px).
2. Build masks at preview resolution: face skin, body skin, hair, clothes, eyes/sclera/iris, lips, teeth, brows (landmark polygons feathered, intersected with the segmenter classes where useful).
3. Refine to full resolution on the GPU: upsample each mask with a guided / joint-bilateral filter using the full-res luminance (or the developed image) as guide, then a narrow-band edge snap. A 256 px network mask upsampled 100x leaves a halo of several pixels at a 60 MP framing; the retouch strengths are low and feathered, which hides most of it, but hair and shoulder edges need the guided refinement.
4. Edit ops run as GPU compute nodes in the existing graph, parameterised by (mask id, strength), in the engine's working space (linear for exposure ops, Lab/OKLab for chroma ops).
5. Expose them as masks usable by ALL existing local adjustments (the Lightroom pattern), plus a "Retouch" panel with a few sliders.

### Tools and algorithms

| Tool | Algorithm | Why |
|---|---|---|
| Skin smoothing (strength) | Edge-aware 2 to 3 band decomposition, smooth the middle band only, fine band untouched | Keeps pores; cheap on GPU (guided filter is box filters) |
| Spot heal (tap) and auto-detect (suggest, user confirms) | Multi-scale blob detection, texture transfer plus Poisson boundary fit | No network; deterministic; matches Lightroom/Capture One behaviour |
| Even tone | a/b pull toward edge-aware local mean, L change capped | Fixes blotchiness without flattening |
| Shine | L compression for specular pixels in skin | Simple, controllable |
| Under-eye | Landmark band, L lift plus b correction, toward cheek reference | Mirrors Capture One's Dark Circles |
| Teeth / eye whitening | Chroma and yellow reduction with an L clamp in the mask | Natural limits matter more than the algorithm |
| Face light | Soft mask times elliptical falloff exposure | Same tool as a dodge brush |
| Optional: auto dodge/burn | Mid-band luminance flattening in skin | Later |

What needs a network at all: only face detection/landmarks and the segmenter. Everything else is classical.

### Download and latency (estimates, [U]: measure on devices)

- Download: face_landmarker bundle on the order of a few MB; multiclass selfie segmenter on the order of 10 to 20 MB; total about 20 to 30 MB, ideally bundled in the app or fetched once on demand (the engine already does first-launch prebakes).
- Latency: both models are tiny mobile nets designed for real-time camera use, so tens of ms each on a mid-range Android GPU/CPU and maybe 100 ms to 300 ms on an old iPhone is the expectation; the dominant cost will be GPU mask refinement and the retouch passes at 40 to 60 MP, not inference. Analysis runs once per image, so the budget is a loading spinner at most. The MediaPipe docs I read give no latency numbers [V], so benchmark on target devices before committing.

### Quality risks

- 256 px masks on 40 to 60 MP: handle with crop-then-run and guided-filter refinement (above). Test hair, glasses, hands over face, profile faces and small faces (group photos, faces under about 100 px in the analysis image give poor landmarks).
- The single face-skin class includes or excludes areas inconsistently (ears, neck, under chin); always intersect with the landmark face oval and give the user a mask brush to correct (add/subtract), as Lightroom allows.
- Skin-tone fairness: segmenters and detectors can perform unevenly across skin tones [U; check the model card's fairness section]; test across diverse skin tones and keep the edits tone-preserving (do not push toward a target skin colour by default).
- Colour/ISP consistency: retouch ops must run after the camera-matched base render and before film/grade stages as designed, so the result survives re-edits. Placement in the graph is an engine decision.

### Guardrails and UX

- Defaults: all sliders at 0 or a mild preset (about 20 to 30 percent) with a "natural" label; texture preserved by design; hard caps on whitening and on L lift so skin and teeth never look painted.
- No automatic reshaping. Liquify only as a manual optional tool, off the default panel.
- Before/after (hold to compare), per-person controls when several faces, mask preview overlay, and a reset.
- No generative ops, so no AI-content labelling duties from the retouch itself [U: if generative features are ever added, check China's AI-generated content labelling rules and platform policies].
- Store masks and parameters, not baked pixels, so edits are re-editable and non-destructive.

## 5. Licence table

| Candidate | Code | Weights | Training data | Verdict |
|---|---|---|---|---|
| MediaPipe (Tasks, Face Landmarker, selfie multiclass) | Apache-2.0 [V] | not stated on doc pages [V]; verify from model cards [U] | Google-collected [U] | Use, after confirming model terms |
| OpenCV YuNet | MIT [V] | MIT (in OpenCV Zoo) [V] | WIDER FACE-type [U] | Optional detector; verify dataset terms |
| ML Kit face detection | proprietary SDK | proprietary | n/a | Reject: Play services / China risk [U], not open |
| Apple Vision / Core Image | OS API | OS | n/a | Optional iOS accelerator, not the base |
| BiSeNet face-parsing (zllrunning) | MIT [V] | no licence stated [V], trained on CelebAMask-HQ | CelebAMask-HQ: non-commercial research only [V] | Reject |
| FaRL | MIT [V] | none stated [V] | LAION-Face web crawl [V]; parsing heads on CelebAMask-HQ / LaPa [U] | Reject |
| InsightFace | MIT [V] | non-commercial research only [V] | same [V] | Reject |
| dlib 68-pt | Boost [U] | trained on iBUG 300-W [U] | non-commercial [U] | Reject |
| CodeFormer | S-Lab 1.0, non-commercial [V] | same [V] | FFHQ | Reject |
| GFPGAN | Apache-2.0 [V] | not stated [V] | FFHQ + StyleGAN2 [V]; FFHQ CC BY-NC-SA metadata, mixed Flickr image licences incl. NC [V] | Reject |
| GPEN | none stated [V] | author cites "commercial issues" [V] | FFHQ-style | Reject |
| RestoreFormer | [U] | [U] | FFHQ | Reject (unverified) |
| LaMa | Apache-2.0 [V] | big-lama on Hugging Face, licence not stated on repo page [V] | Places2 [V]; Places terms are research-oriented [U] | Not needed; avoid |
| MI-GAN | MIT [V] | MIT (LICENSE-WEIGHTS) [V] | FFHQ and Places2 [V] | Not needed; FFHQ/Places provenance is a risk even with an MIT weights file |
| Depth Anything V2 Small | Apache-2.0 [V] | Apache-2.0 [V] | not described [V] | Optional later (depth blur) |
| Depth Anything V2 Base/Large/Giant | CC-BY-NC-4.0 [V] | CC-BY-NC-4.0 [V] | | Reject |
| IC-Light | Apache-2.0 [V] | no weight licence stated [V]; SD1.5 base [U] | pipeline uses BRIA RMBG 1.4, non-commercial [V] | Reject (also too heavy, generative) |
| Total Relighting, SwitchLight | not open [U] | | | Not available |
| DPR relighting | unclear [U] | | | Reject |
| FFHQ dataset (as data) | metadata CC BY-NC-SA 4.0 [V] | | images: mixed Flickr licences incl. NC; "not for face recognition" [V] | Do not train commercial models on it |
| CelebAMask-HQ (as data) | | | non-commercial research only [V] | Do not train on it |

## 6. Open items to verify before implementation

1. MediaPipe model cards (Face Landmarker, Selfie Multiclass): training data, model-file licence, fairness evaluation (the multiclass card URL failed for me).
2. Actual file sizes and on-device latency (mid-range Android, oldest supported iPhone).
3. Apple AVSemanticSegmentationMatte availability and Vision face-landmark count (page did not render).
4. Retouch4me, Evoto, PixCake, 醒图, Luminar Neo specifics and whether they run locally.
5. Patent status of PatchMatch and any edge-aware filters used (guided filter, local Laplacian).
6. If a clean-data parsing network is ever wanted: candidate data with commercial terms (for example synthetic face datasets) [U], or a self-collected, consented set.
