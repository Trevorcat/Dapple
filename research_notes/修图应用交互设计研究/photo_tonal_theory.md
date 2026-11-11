# 摄影影调/直方图/调色理论与 Dapple 分类体系研究

Research depth note: about 17 tool calls. Several key Chinese sources (ithome 九调 articles) returned 403 and only search-tool summaries were available. The DUMAS thesis, the PetaPixel Zone System page and the DPReview ETTR page were also blocked. Claims without a URL are marked "[unsourced]" and appear only under Inferences, Gaps or the Spec (design proposals, not facts).

## Q1. 影调理论：高/中/低调、长/中/短调、九调、Zone System、算法规则、"刻意 vs 曝光错误"

### Takeaway
九调 = 明度基调 (高/中/低) × 明暗跨度 (长/中/短). Chinese sources tie it to Adams' 11 zones (drop pure black and pure white, leaving 9). I could not retrieve citable exact zone or value cut-offs for each of the 9 classes, so the numbers in the Spec are design starting values, not textbook facts. Cambridge in Colour confirms "no ideal histogram" and that auto-exposure drags low-key and high-key scenes toward midtones. A naive "brighten if dark" rule would repeat exactly that failure.

### Cited Findings
- 九调 framework: by overall brightness 高调/中间调/低调; each is split by 明暗跨度 into 长调/中调/短调 (短 = small span, 长 = large span). That gives 9 combinations: 高长, 高中, 高短, 中长, 中中, 中短, 低长, 低中, 低短. — [ithome 707/644 (via search summary)](https://www.ithome.com/0/707/644.htm), [ithome 703/265](https://ithome.com/0/703/265.htm)
- Link to Adams: Adams' theory has 11 zones from black to white. Removing pure black and pure white leaves 9 zones. Combining the two removed ends gives a "10th" tone, 全长调. — [ithome 707/644](https://www.ithome.com/0/707/644.htm) (secondary Chinese tech-site source, seen only through a search summary, not checked against a textbook)
- Judge by histogram: if the whole photo is bright and the histogram sits mainly on the right, it is 高调. — [ithome 707/644](https://www.ithome.com/0/707/644.htm)
- Munsell: created 1898 by Albert Munsell, the first to separate hue, value (明度) and chroma into perceptually uniform independent scales. Value runs 0 (black) to 10 (white). — [zh.wikipedia 孟塞尔颜色系统](https://zh.wikipedia.com/wiki/%E5%AD%9F%E5%A1%9E%E5%B0%94%E9%A2%9C%E8%89%B2%E7%B3%BB%E7%BB%9F) (search snippet). I found NO source tying the 九调 labels directly to Munsell. The "高中低 × 长中短" tonal-scale vocabulary is commonly said to be taught in Chinese 素描/色彩构成 courses [unsourced; search did not confirm it].
- Zone System: 11 zones, 0 (black) to X (white). Zone V is middle gray, 18% reflectance. One zone is one stop. Meters are calibrated to Zone V, so white subjects underexpose and black subjects overexpose without compensation. Developed by Adams and Fred Archer in the late 1930s–40s for B&W sheet film. — [Photoworkout](https://www.photoworkout.com/zone-system/), [PetaPixel](https://petapixel.com/how-to-use-the-zone-system/) (both via search summaries). "Ten zones is a myth, Adams used eleven": [Russell Cottrell](https://www.russellcottrell.com/photo/BTDZS/zone2.htm)
- Histogram-to-zone mapping (each zone is about 1/10 of histogram width, left edge = Zone 0) is a modern blog analogy, not Adams. A gamma-encoded 0–255 histogram is not divided into equal-stop zones. — [Photoworkout](https://www.photoworkout.com/zone-system/) (flagged as a simplification by the search summary)
- Low-key: the histogram bunches left. Camera AE pulls such scenes toward midtones, so manual reduction is needed. High-key: the histogram bunches right. AE tends to underexpose these, so add exposure. — [Cambridge in Colour](https://cdn.cambridgeincolour.com/tutorials/histograms1.htm)
- Contrast: a broad histogram means high contrast; a narrow one means flat, dull low contrast (e.g. fog). Clipped highlights are unrecoverable, but some clipping is fine for speculars, the sun and light sources. "There is no one ideal histogram." Cameras cannot know absolute subject brightness. — [Cambridge in Colour](https://cdn.cambridgeincolour.com/tutorials/histograms1.htm)
- Academic: a thesis classifies photos into high/medium/low-key using only pixel populations (over 80% success). — [DUMAS "Classifying images by light aesthetics"](https://dumas.ccsd.cnrs.fr/dumas-00854874) (snippet only)
- arXiv 1209.5040 classifies high/normal/low-key for print reproduction and concludes that adjusting only low-key images seems sufficient. It notes classification is hard because the importance of regions is subjective. arXiv 1209.5041: peaks near the dark end indicate low-key, peaks near the bright end indicate high-key. — [1209.5040](https://arxiv.org/pdf/1209.5040), [1209.5041](https://arxiv.org/pdf/1209.5041)
- US patent 8577169 computes a "digital key value" from histogram shape, with rejection of certain content types. — [USPTO](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/8577169)
- Aesthetic: NIMA (Google, Talebi and Milanfar 2017) is a CNN that predicts a distribution over 1–10 scores, trained on AVA (about 200 raters per photo). Suggested uses include guiding photo-enhancement tools. — [Google Research blog](https://research.google/blog/introducing-nima-neural-image-assessment/), [arXiv 1709.05424](https://ar5iv.arxiv.org/html/1709.05424)

### Inferences
- Because AE biases scenes toward mid-grey (Cambridge in Colour), the mean luma of an intentional low-key or high-key shot will look "wrong" to a naive corrector. Targeting mean ≈ Zone V would destroy those photos.
- The arXiv finding "only low-key needs adjustment" is about print reproduction. For Dapple the equivalent is that key classification gates the target tone curve, not just brightness.
- Intentional vs mistake (design heuristics [unsourced], to be validated on the user's own library):
  - Intentional low-key: the dark mass is smooth and structured; there is a distinct bright accent region (about 2–5% of pixels above about Zone VII); blacks are near but not massively clipped (under 1–2% at 0); a highlight tail exists; the subject is bright against a dark background (face, rim light); EXIF EV comp ≤ −0.7 or manual mode; scene tag night/studio/stage.
  - Underexposed: no highlight tail at all (p99 below about 0.5), noise-dominated shadows, the subject itself (face/main object) is dark, EV comp 0 in an auto mode, daylight scene tags, and a histogram that looks like a standard shape shifted left rather than a "dark mass + small accent".
  - Intentional high-key: very little shadow mass (p5 above about 0.4), whites with preserved texture, no color-channel clipping on subject skin, EV comp ≥ +0.7, bright or white background (studio, snow, fog, window light).
  - Overexposed: large per-channel clipped area (over about 3–5%) on the subject, skin channel (R) clipped, no deep shadow anchor at all, EV comp 0 in bright mixed light with sky or subject clipping.
  - Signals available in the app: pixel stats, per-channel clipping, face detection plus face-region luma, subject saliency, EXIF (ExposureBiasValue, exposure mode, ISO, metering, scene program), RAW headroom (true raw histogram), a scene classifier.

### Gaps
- Precise textbook definitions (zone or value ranges) for each of the 9 调, their emotional character and typical subjects: not retrieved from any citable source. The ithome pages returned 403 and search returned only the framework. The mapping in the spec below is a design proposal. A follow-up fetch of 色影无忌 / 蜂鸟网 / 素描教材 pages is recommended.
- Origin of 九调 in Chinese art education versus Munsell: unverified.
- Adams' own per-zone descriptions (I–IX) not retrieved (PetaPixel returned 403). Only Zone V is sourced, via a search summary.
- Full text and thresholds of the key-classification papers not accessed.

## Q2. 直方图理论: ETTR、剪切、动态范围、形状

### Takeaway
Camera histograms and blinkies come from the embedded JPEG, not the RAW, so they show clipping earlier than the RAW actually clips. ETTR RAW files therefore routinely arrive "too bright", and the correct raw-develop move is to lower exposure. That case must be distinguished from genuine high-key.

### Cited Findings
- The camera histogram is generated from the embedded JPEG. It is commonly conservative, showing clipping before the raw clips. RawDigger and FastRawViewer show the true raw histogram. — [DPReview ETTR Exposed](https://dpreview.com/articles/6641165460/ettr-exposed), [Kasson](https://blog.kasson.com/?p=2459) (via search summary)
- ETTR is meant for base ISO. If the result is too bright, darken during raw processing; highlight detail may be intact even when the histogram shows clipping. Bracketing gives some protection. — search summary of [DPReview](https://www.dpreview.com/articles/6641165460/ettr-exposed/2) and [Photography Life](https://photographylife.com/exposing-to-the-right-explained)
- ETTR means increasing exposure as far as possible without clipping highlights. Forum skeptics say modern sensors at base ISO often don't need it, and that for scenes with no white you shouldn't push right. — [Cambridge in Colour forum](https://cambridgeincolour.com/forums/thread37685-5.htm)
- Histogram shapes: standard centered, left-heavy low-key, right-heavy high-key, broad = contrast, narrow = flat, piles at the edges = clipping. — [Cambridge in Colour](https://cdn.cambridgeincolour.com/tutorials/histograms1.htm)
- A Fujifilm Exposure Center article on getting the most from histograms exists — [Fujifilm X](https://fujifilm-x.com/en-us/?p=39838) (not read).

### Inferences
- Dapple has RAW + JPEG. Compute stats on a linearized, demosaiced RAW proxy (headroom: how many stops until the RAW clips) as well as on the display-referred rendering. If the JPEG shows a right-edge pile-up but the RAW has about 0.5–1 stop of headroom, the photo is ETTR, not overexposed: reduce exposure and recover highlights, then apply the key logic to the result.
- Comb or gappy histograms (strong tonal edits, 8-bit banding) and bimodal shapes (backlit or silhouette, high scene contrast) need distinct handling [unsourced shape taxonomy; standard but not cited].

### Gaps
- Citations for the shape taxonomy (bimodal, comb) and for scene dynamic-range and contrast-ratio theory: none found within budget. The claim that RAW holds half its levels in the top stop was not verified.

## Q3. 调色理论: 色温、配色、分离色调、饱和度、色调分类

### Takeaway
Only teal-orange theory was sourced (complementary contrast; skin sits in the orange range). Chinese usage of 冷调/暖调/高低饱和 categories and split-toning theory were not retrieved. The spec below is a design proposal.

### Cited Findings
- Teal-orange: skin tones lie in the orange range, so teal shadows or background make them stand out. The two are complementary on the colour wheel. The look became Hollywood's default in the 2000s. — [PetaPixel](https://petapixel.com/2017/02/23/orange-teal-look-popular-hollywood/), [DIYPhotography](https://www.diyphotography.net/reasons-orange-teal-look-popular-movies/)
- Caveat: an aggressive orange push can oversaturate deeper skin, and the look fights green and earth-tone scenes. — search summary of the above (blog-grade; no rigorous study found)

### Inferences
- Treat teal-orange as a "colour treatment" preset class applied with a skin-protect mask, not as a global default.
- Colour axes for classification: (a) scene colour temperature (estimated illuminant, Kelvin/tint) versus (b) image colour cast or mood (warm/cool after WB). Keep them separate: WB is correction intent, 暖/冷调 is look intent.

### Gaps
- Split-toning theory, harmony schemes (analogous, monochromatic) and Chinese 色调 vocabulary (冷/暖/中性, 高/低饱和): no citable source retrieved.

## Q4. 预设/Look 分类体系（行业做法）与提议的分类

### Takeaway
The industry groups looks by (1) rendering or profile type, (2) style era or genre, (3) film emulation, with adaptive/AI variants that adjust to the photo. None of the sources I found organize by tonal key, which is an opening for Dapple.

### Cited Findings
- Lightroom: Basic profiles (Adobe Color and Adobe Monochrome are applied by default). Adaptive (Adaptive Color / Adaptive B&W) and raw-only groups (Adobe Raw, Camera Matching) are for RAW/DNG. Creative groups are Artistic, B&W, Film-Inspired, Modern, Vintage, and profiles have an Amount slider. — [Adobe Help: Use profiles](https://helpx.adobe.com/lightroom-cc/web/edit-photos/apply-effects/use-profiles.html), [mobile profiles](https://helpx.adobe.com/lt/lightroom/mobile/adjust-light-and-color/apply-profiles.html). Modern = current fashions, Vintage = film-like, Artistic = edgy colour shifts: [Amateur Photographer](https://amateurphotographer.com/technique/work-faster-smarter-lightroom-profiles/)
- Capture One: official Styles packs (Spectrum, Cinematic, Seasonal). The Film Styles pack has 15 looks × 3 intensities = 45. Adaptive Styles need C1 16.5+. Styles can be layered with opacity. Vendor tip: apply the style first, then tweak WB/exposure. — [SLR Lounge](https://www.slrlounge.com/new-capture-one-film-styles-pack-emulated-color-contrast-and-texture-of-film-stocks/), [PetaPixel](https://petapixel.com/2020/07/29/capture-one-unveils-two-film-inspired-style-packs-for-photo-editing), [alexonraw](https://alexonraw.com/capture-one-match-look-presets/)
- Third-party film emulation packs (VSCO, Mastin Labs, RNI) are the dominant paid-preset genre. — [VSCO](https://www.vsco.co/features/photo-filters/preset-packs), [Mastin Labs](https://mastinlabs.com/collections/capture-one-styles)
- Fujifilm film simulations (Provia, Velvia, Astia, Classic Chrome, Pro Neg Hi/Std, Classic Neg, Eterna, Eterna Bleach Bypass, Nostalgic Neg, Reala Ace) come from the search tool's memory, NOT verified. Check Fujifilm's site.

### Inferences — proposed taxonomy (Chinese UI)
Looks carry independent axes, shown as facets rather than one flat list:
1. 影调 Tone key: 高调 / 中间调 / 低调
2. 影调跨度 Range: 长调 / 中调 / 短调 (contrast span)
3. 色调 Colour cast: 冷调 / 中性调 / 暖调 (plus 分离色调, e.g. 青橙 teal-orange)
4. 饱和度 Saturation: 低饱和(淡雅) / 自然 / 高饱和(浓郁)
5. 黑位/质感 Blacks: 深邃黑 / 哑光·褪色黑 (matte/faded)
6. 胶片风格 Film: 人像胶片 (Portra 类) / 柔和明亮 (400H 类) / 浓郁风光 (Velvia 类) / 纪实暗淡 (Classic Chrome 类) / 黑白颗粒 (Tri-X 类). Name by character, not trademark, in the UI.
7. 题材 Genre: 人像 / 风光 / 街头 / 夜景 / 静物·美食 / 建筑

Each preset carries tags on each axis. The auto-developer picks adaptive presets compatible with the photo's detected key. For example, don't offer 哑光高调 for a 低短调 night photo unless the user forces it.

Suggested top-level UI buckets, replacing 自然/清透/胶片暖调/冷调/黑白: 清新明亮 (高调·低饱和) · 通透自然 (中调·中性) · 浓郁风光 (中长调·高饱和) · 暗调氛围 (低调) · 胶片复古 (哑光·暖) · 电影感 (青橙/低饱和) · 黑白 (高对比 / 柔和).

### Gaps
- VSCO and Fujifilm official category organisation not verified from primary pages. Chinese app category conventions (醒图, Snapseed, 国内 Lightroom) were not researched.

## Q5. 后期处理理论: 全局/局部、操作顺序、"develop to intent"

### Takeaway
No citable source on order of operations or retoucher per-key rules was obtained within budget. I have only a vendor tip (apply the style/profile first, then tune WB/exposure) and the Cambridge in Colour principle (adjust to the intended mood, not to mid-grey).

### Cited Findings
- Apply the Style first, then fine-tune WB and exposure. Styles can be applied on layers at different opacities. — [SLR Lounge / Capture One coverage](https://www.slrlounge.com/new-capture-one-film-styles-pack-emulated-color-contrast-and-texture-of-film-stocks/)
- Adjust exposure manually whenever you want the average brightness different from midtones. — [Cambridge in Colour](https://cdn.cambridgeincolour.com/tutorials/histograms1.htm)

### Inferences
- Suggested Dapple pipeline (standard raw workflow order [unsourced, common practice]): (1) lens/profile + WB, (2) exposure re-anchor by key class, (3) highlight/shadow recovery and tone curve/contrast by range class, (4) colour (vibrance before saturation, HSL), (5) look/preset grade, (6) detail/NR/sharpen, (7) local masks (face, sky). Step 2 must be key-aware.

### Gaps
- Pro retoucher literature on per-key corrections, and local-versus-global theory: not sourced.

## Q6. 自动分类算法与启发式

### Takeaway
Published approaches: a histogram-shape key value (patent), pixel-population classifiers (over 80%), and the NIMA CNN for aesthetics. Simple percentile heuristics are enough for key and range; an aesthetic CNN is optional.

### Cited Findings
See Q1 (DUMAS thesis, arXiv 1209.5040/5041, USPTO 8577169, NIMA).

### Inferences — CLASSIFICATION SPEC (starting values, all [unsourced] design proposals; tune on the user's library)

Compute on display-referred luma L in [0,1] (sRGB-gamma luma of a neutral-WB render, downsampled to about 512px, optionally weighted 60/40 toward the center or subject). Features: mean μ, median p50, percentiles p1, p5, p25, p75, p95, p99, span S = p95 − p5, clip_lo = % pixels with L < 0.02, clip_hi = % pixels with any channel > 0.98, entropy H, skew, bimodality.

Zone mapping (Adams, gamma-encoded approximation; Zone V, 18% grey, is about 0.46 in sRGB): shadows L < 0.25 (about Zones 0–III), mid 0.25–0.70 (IV–VI), high > 0.70 (VII–X).

Key (基调) by median p50, with mean as a check:
- 低调: p50 < 0.28 (and μ < 0.33)
- 中间调: 0.28 ≤ p50 ≤ 0.65
- 高调: p50 > 0.65 (and μ > 0.60)
- Alternative "key mass" rule: 低调 if at least 50% of pixels have L < 0.30; 高调 if at least 50% have L > 0.70.

Range (跨度) by S = p95 − p5:
- 短调: S < 0.35
- 中调: 0.35 ≤ S ≤ 0.65
- 长调: S > 0.65
- Per-class refinements: 高长调 needs p95 > 0.9 and p5 below about 0.2 (a dark accent). 低长调 needs p5 < 0.05 and p95 > 0.7 (a bright accent). 高短调 has p5 > 0.55. 低短调 has p95 < 0.45. 中短调 has S < 0.35 around mid.

Confidence: report the class plus its margin. If a feature is within 0.05 of a threshold, treat it as a border case and interpolate the strategy parameters between the two classes.

Colour cast (post-WB, for look, not correction): take mean a*, b* of mid and high pixels, excluding chroma outliers. 暖调 if mean b* > +8 (and a* ≥ 0), 冷调 if b* < −5, else 中性. Estimate the illuminant (Kelvin) separately for the WB decision.
Saturation: mean HSV S over pixels with L between 0.1 and 0.9: 低饱和 < 0.18; 自然 0.18–0.40; 高饱和 > 0.40 (tune).

Intentional vs error (score 0–1, weighted):
- Low-key intent increases with: lit, well-exposed face/subject; accent highlights of at least 2% of pixels above L 0.75; clip_lo under 2% with smooth gradients; EV comp ≤ −0.7; night/studio tag. It decreases with: no highlights (p99 < 0.5), noise-heavy shadows, a dark subject, auto mode with EV 0 in daylight.
- High-key intent increases with: p5 > 0.35, skin texture preserved (no channel clip on the face), EV comp ≥ +0.7, white-background/studio/snow tag. Flag overexposed if clip_hi on subject or skin exceeds 3–5%, or if the RAW itself is clipped.
- Use RAW headroom: if the RAW has at least 0.7 stop of headroom while the JPEG clips, mark it ETTR-shifted and lower exposure first.
- Never use "mean far from 0.46" alone as an error signal.

### CORRECTION STRATEGY PER CLASS (design proposal [unsourced]; the principle of not forcing midtones comes from Cambridge in Colour)
General rule: preserve the class. Correct only technical faults (clipping, cast, noise) and push p50 toward the class centre, not toward 0.46.

| 类 | Protect | Adjust | Do NOT |
|---|---|---|---|
| 高长调 | highlight texture (p99 ≤ 0.98, no channel clip), one dark anchor | gentle highlight recovery; small black point so p5 ≈ 0.05–0.15 | lift shadows; brighten globally; pull the image toward mid |
| 高中调 | skin, cloud and white detail | exposure so p50 ≈ 0.70–0.75; mild contrast so S ≈ 0.5 | crush whites; add heavy contrast |
| 高短调 | airy, soft, low contrast; unclipped whites | keep S small; set white point so p95 ≈ 0.93; fix grey cast on whites (WB) | add contrast or blacks (kills 淡雅); leave grey-ish whites uncorrected |
| 中长调 | both ends, midtone separation | moderate S-curve; recover both ends | over-sharpen; HDR look |
| 中中调 | neutrality, skin | standard: p50 → about 0.45–0.5, mild contrast | over-processing |
| 中短调 | softness and mood (fog, overcast) | slight contrast or dehaze only if wanted; protect gentle gradation | force full range (stretching creates fake contrast and noise) |
| 低长调 | highlight accent, shadow detail near the subject | lift shadows only locally (subject); keep blacks p5 ≈ 0–0.03; limit highlight clip | raise global exposure to mid; lift all shadows (kills drama) |
| 低中调 | mood, subject separation | small dodge on the subject; p50 around 0.18–0.25 | brighten to mean 0.46 |
| 低短调 | murky, subdued atmosphere; noise | NR priority; hold p95 low; subtle shadow detail | add contrast or brightness; boost clarity/dehaze (amplifies noise) |

If underexposure is detected in any key, raise exposure on the RAW (not by shadow lift) until the key centre is reached. If overexposed, recover highlights in the RAW, then re-evaluate the class.

### Gaps
- No published numeric thresholds found; all numbers above are untested starting values. Scene-adaptive auto-exposure literature was not surveyed beyond arXiv 1209.5040/5041.
