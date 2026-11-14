# Portrait reshaping (瘦脸 / 瘦身 / 推): what the mainstream apps offer, open-source references, the technique chosen (2026-11-14)

Legend: [V] = read on the cited page today; [U] = not verified (memory or secondary source). Engineering notes, not legal advice.
Engine design and measurements: `docs/engine/pipeline-architecture.md` section 45. Earlier retouch research: `portrait-retouch.md` (1b recommended "manual only, default zero" for any liquify: kept).

## 1. What the mainstream apps offer

| App | Face items | Body items | Manual / protection | Source |
|---|---|---|---|---|
| 美图秀秀 (Meitu) | 「瘦脸瘦身」, 「面部重塑」 "二十余种能力": 去双下巴, 小脸, 下颌线, 发际线; 大眼 / 小头 for several faces at once (「多选人脸」); 面部对称 | 「增高塑型」: 天鹅颈, 直角肩, 超模腿; 身材塑形: 瘦小臂, 瘦大臂, 瘦大腿, 瘦斜方肌, 纤细; 「一键美体」; 「手动瘦身」 | 瘦脸瘦身 "保护背景"; 背景保护 "新增「手动选区」"; 橡皮擦 ("哪不满意擦哪里") | App Store description and release notes [V] https://apps.apple.com/cn/app/id416048305 |
| 醒图 (ByteDance) | 「精准美型」, 五官 "个性化微调, 保留个人特征" (no per-item list on the store page) | 「体态管理」 one-tap posture; users report 瘦手臂 / 瘦身 done with 变形 → 挤压, and 恢复 to repair a bent background | 变形 (挤压 / 膨胀 / 推), 恢复 brush | store page [V] https://apps.apple.com/cn/app/id1500526240 ; user tutorials (Douyin) [U] |
| Evoto (desktop + mobile, pro retouch) | Face Shape: Face, Temple, Cheekbone, Jaw, Face Size, V-Shape, Jaw Length, Face Width, Hairline, Forehead Height / Width, Philtrum, Middle / Lower Section, Taper Chin, Chin Length, Chin Shape; Eyes: Full Eye Size, Eyeball Size, Height, Width, Distance, Inner / Outer Corner, Tilt, Position; Nose: Size, Length, Horizontal, Bridge, Width, Tip; Mouth: Size, Width, Vertical, Horizontal, Tilt, M-shaped, Upper / Lower Lip; Head Pose; Symmetrize (-100..100, default 0) | Full Body Reshape: AI Reshape (posture, auto), Body, Height, Head, Neck Width / Length (-100..100), Arms, Breasts, Waist, Hips, Leg Width (thigh / calf), Leg Length (thigh : calf 80 : 100); several are "unavailable in group images" or "non-full-body photos" | "Fix Background" repairs the background bent by reshaping (a generative step, one credit) | [V] https://support.evoto.ai/portrait-retouching-facial-reshape/ , https://support.evoto.ai/?p=2250 |
| Photoshop Face-Aware Liquify | Eyes: Size, Height, Width, Tilt, Distance; Nose: Height, Width; Mouth: Smile, Upper / Lower Lip, Width, Height; Face Shape: Forehead, Chin Height, Jawline, Face Width | (none: plain Liquify) | Forward Warp, Reconstruct, Freeze / Thaw mask | [V via search summaries] https://www.adobe.com/learn/photoshop/web/face-aware-liquify , https://photoshopessentials.com/?p=8951 ; Chin Height [U] |
| PortraitPro | Face Sculpt: a master control plus forehead, jaw, nose, neck, eyes, mouth shape | | | reviews [V] https://digital-photography-school.com/anthropics-portraitpro-22-review/ , https://www.bhphotovideo.com/explora/photography/hands-review/hands-portraitpro-12-retouching-face-sculpting-simplified |
| BeautyPlus | Slim, Reshape (face), Head size | Body Tuner: waist, legs (length), shoulders, arms, hips, neck; one tap | | [V, first-party marketing] https://beautyplus.com/body-editor |
| Facetune | Reshape (face proportions, cheekbones, jawline), Face | Reshape body [U] | Reshape brush, Vanish (object removal, not reshaping) | [V marketing] https://www.facetuneapp.com/features/face-slimmer |
| 轻颜 / 美颜相机 / Lensa | not found on primary pages today | | | [U] |

What repeats across all of them (the commonly used set):
* **Face**: 瘦脸 (jaw / cheek width), 小脸 (face size), 下巴 (chin length), 大眼 (eye size), 瘦鼻 / 鼻翼 (nose width). These five are in Meitu, Evoto, Face-Aware Liquify (as Face Width / Jawline, Chin Height, Eye Size, Nose Width) and BeautyPlus. Long-tail items (temple, cheekbone, hairline, philtrum, eye tilt / corners, lip shapes, smile) appear only in the pro tools.
* **Body**: 瘦腰 / 瘦身 (waist), 瘦手臂 (arms), 瘦腿 (leg width), 长腿 (leg length), 小头 (head). Shoulders (直角肩), neck (天鹅颈), chest and hips are the next tier.
* **Manual**: a push brush (推 / 挤压 / Forward Warp) and a restore brush (恢复 / Reconstruct) everywhere.
* **Background**: every app shows that reshaping bends the background and offers a fix: protection by a person mask (Meitu, with a manual selection), a restore brush (醒图), a generative background repair (Evoto). Group photos: per-face selection (Meitu 多选人脸); body tools are often disabled in group photos (Evoto).

**First version (necessity before count)**: face 瘦脸, 小脸, 下巴, 大眼, 瘦鼻; body 瘦腰, 瘦手臂, 瘦腿, 长腿, 小头; manual 推 with 恢复. Left out on purpose: eye distance / tilt / corners, lip and smile shaping, hairline, temple, cheekbone, philtrum (identity-changing and long-tail), symmetrize and head pose (a 3D change a 2D warp fakes poorly), chest / hips (sexualised edits; not requested), shoulders / neck (second tier: revisit if asked), one-tap presets (defaults stay 0; a natural preset can come later), generative background repair (no generative fill, `portrait-retouch.md` 4).

UI words: the plain words these apps use (瘦脸 小脸 下巴 大眼 瘦鼻; 瘦腰 瘦手臂 瘦腿 长腿 小头; 推 / 恢复).

## 2. Open-source references

| Project / paper | What | Licence | What we take |
|---|---|---|---|
| Schaefer, McPhail, Warren, "Image Deformation Using Moving Least Squares", SIGGRAPH 2006 (https://people.engr.tamu.edu/schaefer/research/mls.pdf [U]) | point-handle deformations (affine / similarity / rigid), as-rigid-as-possible | paper | idea: handles + smooth interpolated field; rigid MLS avoids shear. Not used directly (see 3) |
| Jarvis73/Moving-Least-Squares (https://github.com/Jarvis73/Moving-Least-Squares) | NumPy / PyTorch MLS (affine, similarity, rigid) | MIT [V: repo lists MIT] | reusable, but nothing copied: our operators are simpler fields |
| clbarnes/molesq (https://github.com/clbarnes/molesq) | MLS point transforms | licence not checked [U] | none |
| jonghewk/ImageDeformation_MovingLeastSquare | C++ / OpenGL MLS | not stated [U] | none |
| Classic "local scaling / local translation warps" of interactive liquify (Gustafsson, "Interactive Image Warping", 1993 thesis [U]) | radial kernels for push (translate), bloat / pinch (scale) | thesis | the push brush and the 大眼 local magnification are exactly these kernels |
| Ren et al., "Structure-Aware Flow Generation for Human Body Reshaping", CVPR 2022 (https://openaccess.thecvf.com/content/CVPR2022/html/Ren_Structure-Aware_Flow_Generation_for_Human_Body_Reshaping_CVPR_2022_paper.html), code https://github.com/JianqiangRen/FlowBasedBodyReshaping | a network predicts a body flow from skeleton + PAF | code: "© Alibaba, 2022. For academic and non-commercial use only." [V]; weights: no licence stated [V]; BR-5K data: by signed agreement only [V] | ideas only (flow = displacement field driven by the skeleton, per-part control); no code, no weights |
| Zhou et al., "Parametric Reshaping of Human Bodies in Images", SIGGRAPH 2010 [U] | fits a 3D morphable body, reshapes it, image warp with background-aware mesh | paper | the idea that the body warp must be confined by the silhouette |
| Shih, Lai, Liang, "Distortion-Free Wide-Angle Portraits on Camera Phones", ToG 2019 (https://research.google/pubs/pub48423/) | per-pixel warp mesh: stereographic on faces, perspective on the background, line-preserving energy, runs on phones | paper | the mesh / field representation, background stays straight: we use a cheaper version (protection band) |
| Lightroom / Photoshop "Freeze mask", Meitu "背景保护" | mask-confined warps | products | person matte confines the field |
| MediaPipe Face Mesh v2 (already shipped) and Pose Landmarker (BlazePose GHUM 3D) | landmarks | Apache-2.0 (model cards) [V] | the anchors of every operator |

Rule kept: no GPL / CDDL code ported, no weights with unclear or non-commercial terms. Nothing from the list above is copied; all code is our own.

## 3. Technique chosen

* **One backward displacement field per photo** (a grid of nodes, 1024 cells on the long edge; 512 while a control moves), built on the CPU from the anchors, applied inside the geometry fetch of the existing pipeline (CPU and GPU: one bilinear field lookup per pixel, Lanczos-3 resampling where the field moves a read, the old exact path where it is zero). Why not MLS per pixel: MLS solves a small least-squares system per pixel (cost grows with handles, needs a GPU twin of the solve), and its handles are global (every handle pulls everywhere unless weighted down), which fights background protection; a field from compact kernels is local by construction, exact zero outside, cheap to evaluate, and identical on CPU and GPU (only the grid is uploaded). Why not a learned flow (Ren 2022): no commercial licence, a network per edit, and it cannot be re-rendered at another resolution bit for bit.
* **Face**: operators are compact forward fields around Face Mesh anchors (jaw / cheek contour toward the face axis with a smooth profile along the contour, uniform shrink about the face centre, chin along the axis, local magnification of the eyes, nose wings toward the nose axis), then inverted numerically. Robust to landmarks a few pixels off: the kernels are wide (a quarter of the face width), so a 2 px landmark error moves the field by a fraction of a pixel.
* **Body**: capsule fields along the BlazePose segments (arms, legs; widths measured on the person plane), a torso band at waist height, a head shrink anchored at the neck, and 长腿 as a stretch in height only inside a horizontal window around the main person's legs (horizontal lines stay horizontal inside it, a smooth falloff to zero at the sides), the ground below the feet compressed inside the window so the frame keeps its content (section 47; since section 48 the default 长腿 is the whole-frame vertical stretch B, the window is variant W).
* **Push tool**: the classic liquify forward-warp kernel, composed dab by dab in backward form (each dab fold-free: step at most 0.2 brush radius); 恢复 scales the accumulated field toward zero under the brush.
* **Background** (superseded for body operators by the layered mode, section 4 and `pipeline-architecture.md` 48; this is what remains as the fallback when the matte is unreliable): the field of the face and body operators is multiplied by a protection weight: 1 inside the person plane, smooth decay over a band (15 % of the face width, 25 % of the shoulder width) outside; beyond the band the field is exactly zero (measured: section 45). The long-legs window (main person only, smooth falloff, section 47) and the push tool are not protected (by design).
* **Fold-free**: the forward field's Jacobian norm is limited to 0.75 (scaled down with a log line), so the inversion exists and is unique.


## 4. Keeping the background intact, shadows, and leg lengthening (2026-11-15)

Marks used here (stricter than section 1): **[P]** the page was fetched today, but the fetch tool answers with a model-written extract of it, so a claim marked [P] was read in that extract and not in the full text; **[S]** only a search-result snippet was seen; **[F]** the fetch failed (nothing was read). Nothing in this section was written from memory except where it says "ours". The searches ran four times (English and Chinese); they found no source that gives a complete recipe, only the pieces below.

### 4.1 What was actually opened

| Source | Licence / status | What it does about the background (and shadows) | Mark |
|---|---|---|---|
| Kuaishou Y-Tech, "How Kuaishou's Y-Tech Fixes Background Distortion in Portrait Beautification" (besthub.dev copy) https://besthub.dev/articles/how-kuaishou-s-y-tech-fixes-background-distortion-in-portrait-beautification-67b43db9173f | article, no licence stated; idea only | The face / body warp is followed by a second optimisation over a background triangle mesh (about 150 points tied to the contour keypoints): a **slope term** keeps every detected background line segment's orientation, a **shape term** keeps each triangle's edge lengths (needed where a triangle holds no line); Gauss-Newton / Levenberg-Marquardt. Segments are detected on the original (LSD is in the references). No person matte is described; shadows are not mentioned. No energy formula or weights are given. | [P] |
| Kuaishou Y-Tech, body-shaping overview (besthub.dev copy) https://besthub.dev/articles/technical-overview-of-kuaishou-y-tech-body-shaping-effects-and-underlying-algorithms-bd85c2b1fb12 | same | Three warps: **stretching for long legs with a smooth gradient controlling the strength**, a triangle-mesh affine warp for slim thigh, a circular liquify for slim waist; liquify radius, curve and strength are tuned per body part to limit the effect on the background; a background correction "keeps line slopes unchanged and preserves triangle shapes". Shadows: not mentioned. | [P] |
| Patent CN111028145A / CN111028145B (Chengdu Pinguo Technology), "A method for processing a human image to prevent background distortion", https://patents.google.com/patent/CN111028145A/en | **granted 2023-07-07, status Active, expected to run to 2039-12-10 (Google's assumption, not a legal conclusion)** | The layered recipe: (1) segment the portrait into a mask; (2) slimming deformation of the portrait **and the mask**; (3-7) image pyramid, expand the mask, distance transform, patch-match search from the top level down, fill the unknown region (what the slimmed person uncovered) from best-matching blocks; (8) fuse deformed portrait + deformed mask + filled background. Shadows are not mentioned. | [P] |
| Ren et al. is in section 2. NeuralReshaper (arXiv 2203.10496) https://arxiv.org/abs/2203.10496 | paper under arXiv's distribution licence; the page links no code | Two encoders for foreground and background; foreground information guides the background branch through feature-space warping; the abstract argues that warping the whole image distorts both. A network: excluded for us (size, no usable licence). | [P] |
| Perfect Corp "AI Body Editor" https://yce.perfectcorp.com/products/body-reshape | marketing page | Claims the person is isolated so "backgrounds, lines, and textures remain perfectly straight" and "everything outside it (doorframes, tile lines, other people, background objects) stays geometrically untouched"; leg elongation is a listed feature with no method; **no word about shadows**. | [P] |
| Shih, Lai, Liang, ToG 2019 https://research.google/pubs/pub48423/ | paper | Abstract only: an optimised warping mesh, stereographic on faces, perspective over the background. Whether line preservation is an energy term was not on this page; the full PDF (people.csail.mit.edu) could not be fetched (over 10 MB). | [P] abstract, [F] pdf |
| core.ac.uk/download/295558149.pdf (a paper on separating the figure from the background to reshape it) | unknown | not read | [F] 403; only a search snippet: "separate the human figure from the background ... reshape the figure, while preserving the background" [S] |
| github.com/johnzh/NeuralReshaper | unknown | not read | [F] 404 (the URL was a guess) |
| Chang and Chuang 2012, line-structure-preserving image resizing (csie.ntu.edu.tw/~cyy) | paper | mesh deformation plus similarity transforms of line features analysed in Hough space | [S] only |
| Perfect Corp "make legs longer" (perfectcorp.com/zh-tw/consumer/features/make-legs-longer), Evoto "Full Body Reshape" (support.evoto.ai/?p=618) | marketing / help | the leg tool asks for neck, waist and ankle positions and a slider | [S] only |
| Wikimedia Commons file pages of the three new test images (API) | CC0 each | licence lines in `ai-test-images.md` | [P] |

What the search found about **shadows**: nothing. None of the opened sources mentions a shadow (patent, Kuaishou, Perfect Corp, NeuralReshaper abstract). Everything below about shadows is our own measurement.
What the search found about **how leg lengthening is done by the mainstream apps**: only Kuaishou's "stretching with a smooth gradient" (above). The owner's recollection (a band stretched across the whole width, between hips and feet) is consistent with that sentence but no opened source states band edges, canvas handling, oblique axes or several people; those choices are ours.

### 4.2 Methods considered

| Method | Uses | Needs | Verdict for Dapple |
|---|---|---|---|
| **Layered** (patent claim above; Perfect Corp's marketing; NeuralReshaper in spirit): warp the person only, keep the background where it is, fill what the person uncovered | person matte | the matte, a fill | **used** (ours differs from the patent in the fill: a mirror read from just outside the old silhouette, no pyramid or patch-match; see section 48; **the patent is granted and active in China to 2039: a legal review is needed before shipping there** (not legal advice)) |
| Line-preserving background mesh (Kuaishou; Shih 2019 in spirit) | detected line segments | an optimiser, a line detector, GPU twin | not now: heavier than the layered fill and still moves the background (it bends it to keep slopes); candidate for a later "bend the background gently where the matte is unreliable" |
| Mask-confined smooth field with a decay band (what section 45 did; Meitu "背景保护") | matte | nothing new | kept as the fallback when the matte is unreliable; it drags a band of background by design |
| Whole-frame 1-D stretch for 长腿 (Kuaishou "stretching") | pose | nothing | **used as the 长腿 default** (section 48): geometry-stable, no matte |
| Generative fill / learned flow | model | non-commercial or unclear licences, size | excluded (unchanged) |

### 4.3 Clean-room statement

Only ideas were taken (layering, mask-driven fill, 1-D stretch with eased ends, slope-keeping as an objective). No code was read or ported; the fill (nearest-outside vectors by two-pass propagation, a mirror read) and the stretch are our own. Licences of the opened sources: all are articles, papers or a patent; none gives code.

### 4.4 Main-session reading (2026-11-15)

What the main session read itself, with the level of verification (nothing here was run; these are reading notes):

| Source | Claim | Verification |
|---|---|---|
| Zhou, Fu, Liu, Cohen-Or, Han, "Parametric Reshaping of Human Bodies in Images" (SIGGRAPH 2010) | One continuous mesh warp of the whole image, solved as a sparse linear system. Energy = body terms + E_reg (adjacent triangles deform alike) + E_dis = sum u_i \|A_i - I\|^2 with u_i the content saliency; borders fixed. They explicitly prefer this to "first segmenting the human region, then reshaping the segmented subject, and finally pasting ... by completing in-between holes", and state the remaining weakness: background artefacts "especially noticeable at regions with structural features (e.g. ground and wall)", to be alleviated through the saliency weights. | full text read |
| Kuaishou Y-Tech write-up (besthub.dev, "how Kuaishou's Y-Tech fixes background distortion in portrait beautification") | A background triangle mesh (about 150 points) is re-optimised after the beauty warp so that detected straight lines keep their slope, plus a triangle-shape term where no lines exist; nonlinear least squares; shipped on phones. No layers, no inpainting. Leg lengthening: the region below the hip line is stretched toward the feet with a smooth gradient. | page extract |
| Ren et al., "Structure-Aware Flow Generation for Human Body Reshaping" (CVPR 2022) | The flow warps the whole image; limitation section: the flow "will also affect the overlapped areas in the background", causing "twisted lines or deformed objects"; matting + inpainting is only suggested as a possible fix. | ar5iv text extract |
| Meitu 增高 (since 2014) | Full-width stretch of a band chosen by two lines. Confirms the 长腿 default (whole-frame vertical stretch, section 48). | main-session reading, secondary source |

### 4.5 Mode C: structure-aware continuous field (2026-11-15, section 49 of the pipeline notes)

Third mode for the slimming operators, built from the reading above: Zhou's single continuous field with the saliency idea inverted for the background, and the Kuaishou "straight lines keep their slope" objective expressed through a structure tensor instead of a line detector (so no line detector, no model). Results and the default recommendation are in section 49 of `docs/engine/pipeline-architecture.md`.
