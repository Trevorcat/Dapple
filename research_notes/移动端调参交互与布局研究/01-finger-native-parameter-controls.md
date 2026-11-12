# Finger-native alternatives to linear sliders for continuous parameter control on touch (Dapple RAW editor)

Method note: about 19 searches/fetches. The search tool mostly returned abstracts/snippets; where I did not open the full paper, I say so. Items in "Inferences" that rely on my background knowledge of the literature (not re-verified this session) are tagged [unverified-background]. Several asked-for items ("Rotate-and-Press", Apple Photos edit dial internals, iOS scrub-speed behaviour, Material 3 slider haptics) were NOT found; see Gaps.

## 1. Direct manipulation on the image (drag-anywhere, tap-to-target, histogram drag)

### Takeaway
Snapseed's scheme (vertical swipe picks parameter, horizontal swipe changes value, value shown at top, drag anywhere on the photo) is the best-documented on-image pattern; its weakness is discoverability, which Snapseed itself later patched with a visible adjust icon. Finger occlusion of the image being judged is the main cost of any on-image drag, but drag-anywhere lets the finger stay away from the region of interest.

### Cited Findings
- Snapseed: swipe up/down on the image to switch settings, swipe left/right to change the value; the value is shown at the top of the screen. — [Photo Workout guide](https://www.photoworkout.com/?p=132476); [iPhone Photography School](https://iphonephotographyschool.com/snapseed/)
- 2012 description: swipe up/down brings up a temporary selection widget; sliding the finger to an option picks it; left/right swipes change linear tools (contrast, brightness, colour cast). — [TidBITS 2012](https://tidbits.com/2012/01/06/snapseed-displays-innovative-touch-techniques/)
- Snapseed 2.13 (2016) kept the swipe gestures but added an "adjust" icon in the bottom bar that overlays the settings, i.e. a discoverable fallback for a hidden gesture. — [9to5Google 2016](https://9to5google.com/2016/11/30/snapseed-2-13-simplified-editor-android-ios)
- Snapseed selective control points: after placing a point, left/right swipe lightens/darkens that region (tap-to-target plus drag value). — [Photo Workout guide](https://www.photoworkout.com/?p=132476) (third-party, not official Google docs)
- Occlusion/targeting: Shift (Vogel & Baudisch, CHI 2007) argues finger targeting errors come from the finger hiding the target and ambiguity about which part of the finger defines the touch point; Shift shows a callout of the occluded area, engages only when needed, and gave much lower error rates on small targets than unaided touch. — [CHI 2007 PDF](https://projects.csail.mit.edu/hci-reading/papers/chi2007-vogel.pdf), DOI 10.1145/1240624.1240727 (abstract-level only read)
- Apple WWDC18 "Designing Fluid Interfaces": keep touch and content attached one-to-one using the touch's relative position (not content centre); recognise a swipe after ~10 pt hysteresis; use recognisers that report continuous position/velocity; detect candidate gestures in parallel and cancel losers once intent is clear; teach hidden gestures with visual affordances and behaviour that mirrors the gesture. — [Apple WWDC18 803](https://developer.apple.com/wwdc18/803)

### Inferences
- Drag-anywhere for the active parameter removes the hit-target problem and the slider-width limit, and works one-handed on tall 20:9/21:9 phones because the thumb stays in its zone while the image stays visible. Cost: a hidden-mode design needs a visible indicator of the active parameter and a persistent numeric HUD (Snapseed's 2016 retrofit suggests the hidden version had discoverability problems).
- Horizontal drag for value conflicts with pan/pinch zoom; arbitration is needed (e.g. one finger = value, two fingers = zoom), using WWDC18's parallel-recognition principle.
- [unverified-background] Shneiderman's direct-manipulation criteria (continuous representation of the object, physical actions, rapid reversible incremental actions, immediate visible feedback) are the standard theory frame. On-image drag is truly direct only for local/targeted edits; for global parameters like exposure there is no spatial referent, so a one-axis drag is an indirect (but image-visible) mapping.
- Histogram drag (Lightroom-style) is a plausible direct-manipulation handle for exposure/shadows/highlights, but I found no sourced evidence on its speed or precision.

### Gaps
- No official Google/Adobe docs fetched for Snapseed or Lightroom targeted adjustment; no peer-reviewed study of Snapseed-style swipe-to-select/swipe-to-change found.
- No empirical data on histogram-drag editing on mobile.
- Shneiderman's 1983 paper not fetched/cited.

## 2. Scrubbing dials, rulers, wheels, detents

### Takeaway
Rulers/dials map finger travel to value with a visible scale and inertia, trading at-a-glance range for unlimited range and fine resolution. The GarageBand precedent shows a radial control can accept circular, vertical or horizontal drags, with distance from centre modulating precision. I found no peer-reviewed evidence that ruler/wheel beats slider on speed; the case is mostly theoretical.

### Cited Findings
- GarageBand iPad virtual knob (2011), per a reverse-engineering write-up: spin anywhere on the knob; dragging farther from centre gives finer control; a vertical or horizontal drag locks into slider mode and can make several revolutions; multi-touch lets several knobs be adjusted at once. — [jherrm "How Apple Designs a Virtual Knob"](https://jherrm.github.io/knobs) (third-party blog, not Apple documentation)
- Knob visuals: hardware-like frames every 3 deg (120 frames) used because 5 deg steps looked jarring. — same source.
- NN/g: knobs fit angular parameters; knobs chosen only for hardware style are criticised; a linear slider is a good default when range is defined but exact value matters less; narrow sliders make precise selection hard; pair coarse control with exact entry; provide default marker/reset; sliders need response within 0.1 s. (NN/g does not discuss touch or occlusion.) — [NN/g Sliders, Knobs, and Matrices](https://www.nngroup.com/articles/sliders-knobs/)
- WWDC18: motion should have no abrupt changes; project momentum from gesture velocity (UIScrollView normal deceleration rate 0.998) to find the rest point; rubber-band at soft boundaries. — [Apple WWDC18 803](https://developer.apple.com/wwdc18/803)
- OrthoZoom Scroller (Appert & Fekete, CHI 2006): in a 1D multi-scale technique, one axis moves the value and the orthogonal axis changes scale, in a single continuous gesture. — [OrthoZoom PDF](https://www.lri.fr/~mbl/Teach/SituatedComputing/2017/papers/Scrolling/OrthoZoom.pdf) (abstract-level only)
- Zliding (Ramos & Balakrishnan, UIST 2005, DOI 10.1145/1095034.1095059): zoom-sliding fuses scale (pressure) and parameter (x-y) control; a single stylus with pressure was marginally better than a bimanual split. — [Zliding PDF](https://www.dgp.toronto.edu/~ravin/papers/uist2005_zliding.pdf)

### Inferences
- Why a ruler/wheel can feel better than a slider (my reasoning from the sources, not measured): (a) resolution is decoupled from screen length: a slider covering 0-100 over ~60 mm gives ~0.6 mm per unit, whereas a ruler can show several units per cm and scroll beyond the screen; (b) the value is held by the scale moving under a fixed indicator, so number and tick stay visible and not under the thumb; (c) inertia gives "lightweight input, amplified output" for coarse moves; (d) ticks/detents give landmarks (0, default).
- Trade-off: relative scrubbing needs clutching or inertia for large ranges; a ruler loses the slider's at-a-glance "where am I in the range" cue, mitigated by a small range bar or default tick.
- [unverified-background] On touch, a linear horizontal strip driven by horizontal drag anywhere is more finger-native and thumb-reachable than a circular rotary, which needs a circular path.
- The Zliding/OrthoZoom idea transfers to touch without pressure: a second axis (vertical offset or finger count) selects scale/CD gain, giving coarse and fine in one gesture.

### Gaps
- No peer-reviewed head-to-head of slider vs ruler vs rotary for touch parameter tuning retrieved (a Colley et al. 2019 LMU paper surfaced but the fetch failed; Corsten et al. 2018 PDF exceeded the size limit). Search for "Rotate-and-Press" returned nothing.
- Apple Photos edit dial (iOS 13+) and iOS Camera zoom wheel mechanics: no primary source retrieved.

## 3. Precision techniques: CD gain, fine-scrub, relative vs absolute, Fitts/FFitts, occlusion

### Takeaway
Touch pointing error has an absolute-precision component (finger imprecision) that classical Fitts' law ignores, so small slider thumbs are intrinsically error-prone. Standard remedies are variable control-display gain (slower finger gives finer value), a second axis for gain, and relative (clutched) rather than absolute mapping.

### Cited Findings
- FFitts law (Bi, Li, Zhai, CHI 2013, pp. 1363-1372): endpoint distribution modelled as the sum of two normals (relative precision plus absolute finger precision); tested on 1D, 2D pointing and touchscreen typing; reported more accurate modelling than standard Fitts. — [Google Research](https://research.google/pubs/ffitts-law-modeling-finger-touch-with-fitts-law/); contradicted in part by a 2021 study finding FFitts fit sometimes inferior to Fitts with nominal width — [arXiv 2101.05244](https://arxiv.org/pdf/2101.05244.pdf)
- CD gain = cursor velocity / device velocity; gain below 1 gives precision, above 1 speed with less clutching; acceleration curves lower gain at slow speed. A 1985 touch-tablet study found moderately low gain (~0.875-1.0) gave faster movement; findings on absolute vs relative direct pointing are mixed. — [Forlines et al., MERL TR2006-066](https://shadow.merl.com/publications/docs/TR2006-066.pdf); [Input slides (LRI)](https://www.lri.fr/~fanis/courses/ISI2014/slides/input.pdf) (secondary summaries from snippets)
- Shift as an occlusion/precision technique. — [CHI 2007](https://projects.csail.mit.edu/hci-reading/papers/chi2007-vogel.pdf)
- Press-and-hold to temporarily expand a slider's scale (finer increments) appears in a technical disclosure. — [TD Commons](https://www.tdcommons.org/dpubs_series/6469) (defensive publication, not peer reviewed)
- Direct-pen work notes that lifting the pen slightly can act as a clutch. — [MERL TR2006-066](https://shadow.merl.com/publications/docs/TR2006-066.pdf) (via snippet)

### Inferences
- Slider precision on a phone is bounded: finger absolute error (order of a millimetre) over a ~60-100 mm track gives practical resolution on the order of 1% at best, worse with occlusion. Order-of-magnitude only; not from a source.
- iOS-style fine scrubbing by moving the finger away from the track is CD-gain-by-second-axis (like OrthoZoom). I found no primary source on iOS speed tiers (commonly described as Hi-Speed/Half/Quarter/Fine) [unverified-background]. Recipe: gain = f(vertical distance from track) in 2-3 bands with a visible label.
- Prefer relative mapping (value change = finger delta x gain) for fine-tuning from the current value: no jump at touch-down, arbitrary precision, consistent with WWDC18's relative-touch rule.
- Dwell/long-press precision modes are slow and hidden; velocity-based gain (slow = fine) is free and discoverable by use.

### Gaps
- No peer-reviewed mobile study directly comparing slider/knob/dial/gesture for parameter tuning retrieved. "FlowSlider" turned out to be an unrelated 2026 diffusion image-editing method, not an interaction technique ([arXiv 2604.02088](https://arxiv.org/html/2604.02088)).
- Holz & Baudisch (perceived input point) not retrieved.
- No quantitative gain values for iOS/Android scrubbing.

## 4. Haptic feedback (detents, snap-to-default)

### Takeaway
Haptic detents reliably improve subjective experience and reduce visual attention; evidence for objective speed/precision gains is small and inconsistent. Android 14 (API 34) added slider-oriented haptic constants; iOS offers a selection generator for detent ticks.

### Cited Findings
- Touch-slider haptic detent study (12 participants): haptic slider ~6.4% faster, mainly in the harder condition; overall not significant (p = .055). — [slides, aviz.fr](https://www.aviz.fr/wiki/uploads/Research/slides-parallel-universes-alt.chi.pdf) (slide deck, small N)
- Surface-haptics review: lack of haptic feedback on virtual controls is known to reduce performance/precision; in HapTable knob work, performance gains were not demonstrated though subjective experience improved. — [arXiv 2004.13864](https://arxiv.org/pdf/2004.13864); [HapTable arXiv 2103.16510](https://arxiv.org/pdf/2103.16510)
- Mid-air haptics with a slider reduced overshoots (company summary of a university study; not touchscreen). — [Ultraleap](https://ultraleap.com/company/news/blog/study-values-automotive-haptics)
- Android: `SEGMENT_TICK` (segmented UI, e.g. segmented slider) and `SEGMENT_FREQUENT_TICK` (many fast choices, e.g. minutes on a clock or percentages; use lighter/suppressed haptics at high density), both API 34; AndroidX compat maps them to CLOCK_TICK below 34. `GESTURE_THRESHOLD_ACTIVATE` for long-press-style activation. Raw integer values differ across sources; use symbols. — [Android reference](https://developer.android.com/reference/android/view/HapticFeedbackConstants); [AndroidX compat](https://developer.android.com/reference/androidx/core/view/HapticFeedbackConstantsCompat)
- iOS: `UISelectionFeedbackGenerator` signals selection changes (pickers/slider steps); `UIImpactFeedbackGenerator` simulates physical impacts (snaps); some system controls play haptics by default; Apple warns against overuse. — [UIFeedbackGenerator docs](https://developer.apple.com/documentation/uikit/uifeedbackgenerator.md); [Apple haptics article](https://developer.apple.com/tutorials/data/documentation/applepencil/playing-haptic-feedback-in-your-app.md) (Apple Pencil-specific; HIG "Playing haptics" page itself not retrieved)

### Inferences
- Use haptics as landmarks (snap/tick at default, zero, major steps), not at every value; at fine ticks use SEGMENT_FREQUENT_TICK with throttling.
- Magnetic snap to default should be visual plus haptic; keep the snap zone narrow in value terms and shrink it as CD gain gets finer so fine control past it remains possible.
- Find N5 motor quality is unverified; test on device. Always provide a non-haptic visual cue since users can disable haptics.

### Gaps
- Material 3 slider haptics guidance, Apple HIG "Playing haptics", Core Haptics docs not retrieved. No evidence on ColorOS vibration behaviour.

## 5. Other modalities: marking/radial/pie menus, pressure, pinch, gaze

### Takeaway
Marking/pie menus are well evidenced for fast, eyes-free selection of a discrete choice such as a parameter, and pair naturally with "pick parameter then drag value" in one stroke. Pressure is not a reliable cross-device channel; gaze is not viable on phones today.

### Cited Findings
- Marking menus rest on self-revelation, guidance and rehearsal; a 1994 CHI field study found expert use of marks averaged 3.5x faster than menu selection. — [Kurtenbach, Moran & Buxton, Graphics Interface 1994](https://graphicsinterface.org/wp-content/uploads/gi1994-11.pdf); [Autodesk Research listing](https://www-pt.autodesk.com/research/publications/the-design-and-evaluation); [billbuxton.com](https://www.billbuxton.com/MMExpert.html) (snippet-level)
- Marking menus are pie menus where the cursor path leaves an ink trail; Callahan, Hopkins, Weiser, Shneiderman (1988) studied seek time and errors for 8-item pie menus vs linear. — [Buxton, Pie Menus](https://billbuxton.com/PieMenus.html)
- Zliding used pressure as the scale channel; OrthoZoom notes pressure needs extra clutching for precise scale control. — [Zliding](https://www.dgp.toronto.edu/~ravin/papers/uist2005_zliding.pdf); [OrthoZoom](https://www.lri.fr/~mbl/Teach/SituatedComputing/2017/papers/Scrolling/OrthoZoom.pdf)
- Apple Photographic Styles (iPhone 16): 2D pad for tone (Y) and colour (X) plus a separate intensity slider; values shown at top; round-arrow reset; same controls in Photos. — [Apple Support](https://support.apple.com/en-is/guide/iphone/iph629d2cd37/ios); [MacRumors](https://www.macrumors.com/guide/iphone-16-photographic-styles/)

### Inferences
- A long-press radial menu on the photo (6-8 parameter wedges around the finger, flick to pick, keep dragging to change value) gives parameter and value in one stroke and suits the thumb; wedges near screen edges need edge-aware layout.
- [unverified-background] 3D Touch was deprecated on iPhone; Android exposes MotionEvent.getPressure but it is contact-area based and inconsistent across devices. Not recommended as a primary channel.
- Two-finger pinch/spread for magnitude is viable only in an explicit parameter mode because it conflicts with zoom. No mainstream phone gaze API; skip.
- Unfolded Find N5 has a tablet-sized canvas, allowing a side rail or two-handed use; no sourced evidence gathered.

### Gaps
- No empirical source on pinch-for-magnitude or pressure on Android, or radial menus on tall phones.

## 6. Feedback design: latency, numeric readout, before/after

### Takeaway
Direct-touch dragging degrades measurably with latency, so aim for as little as possible (tens of ms), well under NN/g's 0.1 s slider ceiling. Show a transient numeric value and a one-touch before/after.

### Cited Findings
- Jota, Ng, Dietz, Wigdor, CHI 2013 (DOI 10.1145/2470654.2481317): latency effects on direct-touch dragging/pointing; performance decreases as latency increases; commercial devices then had up to 125 ms latency. — [dblp](https://dblp.uni-trier.de/rec/conf/chi/JotaNDW13.html); [unpaywall](https://unpaywall.org/10.1145%2F2470654.2481317) (thresholds from the full text not retrieved)
- Ng et al., "Designing for low-latency direct-touch input", UIST 2012, pp. 453-464, DOI 10.1145/2380116.2380174: citation confirmed; contents not retrieved. — [researchr](https://researchr.org/publication/NgLWSD12/bibtex)
- Deber, Jota, Forlines, Wigdor, CHI 2015 "How Much Faster is Fast Enough?" — [PDF mirror](https://dcmpx.remotevs.com/com/tactuallabs/www/SL/papers/howMuchFasterIsFastEnoughCHI15.pdf) (not read)
- NN/g: sliders unsuitable with significant delay; limit 0.1 s. — [NN/g](https://www.nngroup.com/articles/sliders-knobs/)
- WWDC18: response is the foundation; eliminate latency everywhere. — [WWDC18 803](https://developer.apple.com/wwdc18/803)

### Inferences
- A RAW editor cannot re-render full-res per frame; render a downscaled GPU proxy (~1-2 MP) during drag and refine at full-res on release; target touch-to-pixel latency well below ~50 ms. Engineering judgement; numbers not sourced.
- Place the numeric HUD near but not under the finger (Shift's occlusion lesson) and fade it after release; offer press-and-hold compare-to-original.

### Gaps
- Exact perceptual thresholds from Ng/Jota/Deber full texts not confirmed. No source on numeric readout or before/after design beyond NN/g's default/reset marker.

## 7. Mixed-initiative strength control and candidate techniques for Dapple

### Takeaway
Apple's pattern for "style plus strength" is a 2D pad for qualitative axes plus a separate intensity slider with numeric readout and reset. I found no study comparing slider, dial and drag-on-photo for a single strength dimension, so the recommendations below are reasoned design, not evidence-ranked.

### Cited Findings
- Apple Photographic Styles: pad plus intensity slider plus top readout plus reset. — [Apple Support](https://support.apple.com/en-is/guide/iphone/iph629d2cd37/ios); [MacRumors](https://www.macrumors.com/guide/iphone-16-photographic-styles/)
- NN/g: give defaults and easy reset or a default tick. — [NN/g](https://www.nngroup.com/articles/sliders-knobs/)
- Snapseed 2016 added a visible control beside hidden swipe gestures. — [9to5Google](https://9to5google.com/2016/11/30/snapseed-2-13-simplified-editor-android-ios)

### Inferences
Candidate techniques, ranked (my synthesis):
1. Drag-anywhere relative scrub on the photo for the active parameter/strength, anchored at the auto result: velocity-sensitive CD gain; vertical-offset fine-gain bands (OrthoZoom/iOS-scrub style); magnetic detent plus SEGMENT_TICK at default/0/100; transient numeric HUD; press-and-hold for before/after. No hit target, thumb zone, image visible.
2. Horizontal ruler strip (ticks moving under a fixed indicator) at the bottom as the visible twin of #1 (same value, same gesture), with inertia and detents; better for multi-parameter manual editing where range awareness matters.
3. Pad plus strength (Apple pattern) for coupled 2D parameters such as tone x warmth.
4. Long-press radial parameter picker for experts, continuing into value drag.
5. Tap-to-target local adjustments (Snapseed control point / Lightroom targeted adjustment analogue), separate from the global strength.
- Keep a plain slider or ruler fallback for accessibility (TalkBack adjustable actions) and for users who never discover gestures. [unverified-background on TalkBack specifics]
- Theory to teach: Fitts/FFitts (small target plus absolute finger error), CD gain, relative vs absolute mapping, clutching, direct manipulation (Shneiderman), occlusion (Shift), latency (Jota et al.), self-revelation/rehearsal (Kurtenbach & Buxton) for hidden gestures.

### Gaps
- No peer-reviewed comparison of controls for single-parameter AI strength blending; Apple Photos "Auto" dial mechanics not sourced; no Snapseed/Lightroom discoverability data.
- Material 3/Android guidance on gesture alternatives to sliders not retrieved.
- Pressure/gaze viability on Android: background knowledge only.
