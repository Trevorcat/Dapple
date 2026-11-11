# HCI theory and peer-reviewed evidence for Dapple (automation-first mobile photo editing and sharing)

Method note. I ran about 8 web verifications (marked "[verified]" with a link). Many classic items below are from background knowledge and were NOT re-fetched this session. They are marked "[background]" with a canonical DOI/URL I am confident of, but the report writer should treat them as unverified-in-session. Corrections found while verifying: (a) Sequential Line Search is SIGGRAPH 2017 (ACM TOG), not CHI/UIST; SelPh is CHI 2016. (b) Bakhshi et al. "Why we filter our photos" is ICWSM 2015, not CHI, and uses Flickr data, not Instagram.

Evidence grading used: STRONG = replicated or large-sample peer-reviewed; MODERATE = single peer-reviewed study or well-established theory with limited direct test on this task; FOLKLORE = widely repeated, weak or no primary evidence.

---

## 1. Mixed-initiative interaction, automation levels, trust, and presenting/explaining/adjusting/undoing automatic corrections

### Takeaway
Auto-develop should be a high-automation default (act first) that is cheap to inspect, override, scale, and undo. Evidence says to keep the human as decision-maker on subjective output (look), expose calibrated uncertainty or "what changed", and avoid hiding the automation (automation bias and complacency risk is real but is low-stakes for a photo that is trivially reversible).

### Cited Findings
- Horvitz 1999 (CHI '99) "Principles of Mixed-Initiative User Interfaces" lists 12 principles, including: develop significant value-added automation; consider uncertainty about user goals; employ dialog to resolve key uncertainties; weigh costs and benefits of action under uncertainty (expected utility, act vs. ask vs. do nothing); allow efficient direct invocation and termination; provide mechanisms for efficient agent-user collaboration to refine results; maintain working memory of recent interactions; minimize cost of poor guesses. [background] — https://doi.org/10.1145/302979.303030 ; PDF: https://erichorvitz.com/chi99horvitz.pdf
- Parasuraman, Sheridan & Wickens 2000 (IEEE Trans. SMC-A 30(3)) define a 10-level scale of automation applied independently to four stages: information acquisition, information analysis, decision selection, action implementation. Level choice should be evaluated by human performance consequences, automation reliability, and cost of decision/action outcomes. [background] — https://doi.org/10.1109/3468.844354
- Parasuraman & Riley 1997 (Human Factors 39(2)) "Humans and Automation: Use, Misuse, Disuse, Abuse": users over-rely (misuse) on or under-use (disuse) automation depending on reliability perceptions; complacency and automation bias are documented. [background] — https://doi.org/10.1518/001872097778543886
- Lee & See 2004 (Human Factors 46(1)) "Trust in Automation: Designing for Appropriate Reliance": trust should be calibrated to actual capability; trust depends on performance (what it does), process (how), and purpose (why) information; transparency of process supports calibration. [background] — https://doi.org/10.1518/hfes.46.1.50_30392
- Parasuraman & Manzey 2010 (Human Factors 52(3)) review: automation complacency and automation bias occur in both naive and expert users and are not eliminated by training alone; they arise most under multitasking and high reliability. [background] — https://doi.org/10.1177/0018720810376055
- Amershi et al. 2019 (CHI '19, Honorable Mention) "Guidelines for Human-AI Interaction": 18 guidelines in four groups (initially; during interaction; when wrong; over time), validated with 49 practitioners across 20 AI products; authors say not a checklist, tradeoffs expected. G1 make clear what the system can do; G2 make clear how well it can do it; G15 encourage granular feedback; G16 convey consequences of user actions; G17 provide global controls; G18 notify users about changes. [verified] — https://www.microsoft.com/en-us/research/publication/guidelines-for-human-ai-interaction/ ; PDF https://research.microsoft.com/~horvitz/Guidelines_Human_AI_Interaction.pdf ; DOI https://doi.org/10.1145/3290605.3300233
- The full 18 (from the paper, [background]): G1 what it can do; G2 how well; G3 time services based on context; G4 show contextually relevant info; G5 match relevant social norms; G6 mitigate social biases; G7 support efficient invocation; G8 support efficient dismissal; G9 support efficient correction; G10 scope services when in doubt; G11 make clear why the system did what it did; G12 remember recent interactions; G13 learn from user behavior; G14 update and adapt cautiously; G15 encourage granular feedback; G16 convey consequences of user actions; G17 provide global controls; G18 notify users about changes.
- Google PAIR "People + AI Guidebook" (2019, updated 2021): chapters on mental models, explainability and trust, feedback and control, errors and graceful failure, user needs and defining success. Advice includes: calibrate trust by showing confidence only where actionable, explain with "what and why" at the level the user needs, prefer undo/reversible actions, and plan for AI errors as "first-class" design cases. [background; guidebook is design guidance, not peer reviewed] — https://pair.withgoogle.com/guidebook/
- Wickens et al. / Endsley: out-of-the-loop performance problem — the higher the automation of action implementation, the poorer operators' ability to take over; mitigated by keeping them informed. [background] Endsley & Kiris 1995, Human Factors 37(2), https://doi.org/10.1518/001872095779064555
- Explanation research: Kulesza et al. 2013 (VL/HCC) "Too much, too little, or just right? Ways explanations impact end users' mental models": sound and complete explanations best build mental models but cost attention; Bansal et al. 2021 (CHI) "Does the whole exceed its parts?": AI explanations increased acceptance regardless of correctness (did not improve team accuracy) — i.e., explanations can raise over-trust. [background] — https://doi.org/10.1109/VLHCC.2013.6645235 ; https://doi.org/10.1145/3411764.3445717
- Dietvorst, Simmons & Massey 2015 (J. Exp. Psych: General 144(1)) "Algorithm aversion": people lose confidence in an algorithm faster than in a human after seeing it err; letting users modify the algorithm's output even slightly reduces aversion (Dietvorst et al. 2018, Management Science 64(3)). [background] — https://doi.org/10.1037/xge0000033 ; https://doi.org/10.1287/mnsc.2016.2643
- Strength/blend control: personalization literature (below, section 2) and the Horvitz principle "efficient collaboration to refine results" both support a single "how much" control rather than many parameters. Direct experimental evidence for an auto-strength slider in mobile photo editors specifically: not found.

### Inferences
- Map to Dapple (Amershi guidelines): G1/G2 onboarding states "Dapple develops every shot to your Look, then fixes exposure/white balance per photo; it can misjudge backlit and mixed-light scenes". G3/G4 develop at arrival, surface the newest batch first. G5 avoid skin-tone "beautification" defaults (bias, G6: test auto-correction across skin tones). G7/G8 one gesture to open the tweak panel and one to dismiss. G9 "Reset to auto" and per-slider reset. G10 when scene classification is low-confidence, apply the user's Look only at a conservative strength (scope down) and flag the photo ("check me"). G11 a one-line chip "Lifted shadows +0.6 EV, WB cooler" (minimal explanation). G12/G13 remember last tweak per scene type; learn only after repeated consistent edits (G14 cautious updates; announce changes per G18). G15 thumbs/"too strong/too weak" quick feedback that adjusts strength, not a modal survey. G16 show effect of "Apply to selection". G17 global settings: auto-develop on/off, strength default, exclusions.
- Automation level: per Parasuraman et al., stage 4 (action) is automated at high level (applied before the user looks), stage 2 (analysis) is shown lightly, stage 3 (which photos to share) stays human. Keep "select best shots" and "post" human-triggered; do not auto-post.
- Present auto-correction as the default rendition of every thumbnail (no "enhance" button), with an always-available instant compare to Original (hold) and a visible "Auto" state. A single master "Auto strength" 0-100% slider (default 100% or the user's learned value) is the main adjustment; a 3-5 slider tray follows.
- Undo: non-destructive edit stack, one-tap "Revert to auto", second tier "Revert to original", and an undo toast after batch operations (Nielsen heuristic 3/9 support; Shneiderman rule 6). Trust risk is lower here than in safety-critical domains, so automation-bias concerns are modest; the primary risk is "silent bad corrections" the user does not catch when bulk-sharing. Mitigation: flag outliers (clipped highlights, large auto moves) before share.

### Gaps
- No peer-reviewed study found specifically on an "auto-strength slider" or on trust calibration for consumer photo auto-enhance. No verification fetch of the Horvitz 1999 or Parasuraman 2000 text this session. Explainability form for pixel-level corrections (text chips vs. overlays) lacks direct evidence.

---

## 2. Photo enhancement research (preference variability, personalization, SLS/SelPh, filters, compare, gesture vs. slider)

### Takeaway
Experts and ordinary viewers genuinely disagree on "best" rendition, so one universal auto look is wrong; a personal Look plus per-photo correction is evidence-aligned. Personalization from few examples is feasible (FiveK, Kang, SelPh, SLS). Filters/looks measurably help engagement for some content, and mild correction is preferred by serious photographers.

### Cited Findings
- Bychkovsky, Paris, Chan & Durand 2011 (CVPR) MIT-Adobe FiveK: 5,000 RAW photos each retouched by 5 trained photographers in Lightroom; shows variability between retouchers, that personalization from a carefully chosen small training set is possible, and introduces "difference learning" of between-user differences. [verified] — https://people.csail.mit.edu/vladb/photoadjust ; DOI https://doi.org/10.1109/CVPR.2011.5995332
- Kang, Kapoor & Lischinski 2010 (CVPR 2010) "Personalization of Image Enhancement": learns individual enhancement preferences from ~25 representative images the user adjusts; clusters of similar images inform the parameters. Exact author list/venue should be checked before citing. [background, unverified] — CVPR 2010, https://doi.org/10.1109/CVPR.2010.5539850
- Koyama, Sakamoto & Igarashi 2016 (CHI) "SelPh: Progressive Learning and Support of Manual Photo Color Enhancement": learns from the user's editing history, then supports subsequent edits (suggestions, "self-reinforcing"). [verified existence] — https://koyama.xyz/project/SelPh
- Koyama, Sato, Sakamoto & Igarashi 2017 (ACM TOG 36(4), SIGGRAPH) "Sequential Line Search for Efficient Visual Design Optimization by Crowds": Bayesian optimization where the user picks a best point along a 1D slider line repeatedly to navigate a multi-dimensional photo-enhancement space, designed to be crowdsourced and easier than multi-slider tuning. [verified] — https://koyama.xyz/project/sequential_line_search/ ; DOI https://doi.org/10.1145/3072959.3073598. Follow-up: Sequential Gallery (Koyama et al., 2020, arXiv/CHI-related) https://www.koyama.xyz/project/sequential_gallery/
- Bakhshi, Shamma, Kennedy & Gilbert 2015 (ICWSM, not CHI) "Why We Filter Our Photos and How It Impacts Engagement": 7.6M Flickr photos; filtered photos 21% more likely to be viewed and 45% more likely to be commented on; filters that increase warmth, exposure, contrast help most; 15 interviews: serious users treat filters as correction tools and prefer milder effects. Follow-up reported natural scenes less engaging when filtered, food more. Observational (confounded by user self-selection). [verified] — https://ojs.aaai.org/index.php/ICWSM/article/view/14622
- Before/after comparison: Interaction practice (press-and-hold to toggle; split slider; side-by-side). I did not find peer-reviewed user studies of mobile before/after interaction specifically. Related perceptual evidence: change blindness means flicker/toggle at the same location (A/B flip) reveals differences better than side-by-side at different locations (Rensink, O'Regan & Clark 1997, Psychological Science 8(5)). [background] https://doi.org/10.1111/j.1467-9280.1997.tb00427.x — FOLKLORE for preference of hold-to-compare vs. split in photo apps.
- Gesture vs. slider on touch: no definitive photo-editing study found. General evidence: direct manipulation of the image (drag on the photo) vs. remote sliders trade off precision for immediacy; "one-finger drag on the image to change brightness/contrast" is used by some apps (e.g., Snapseed's vertical/horizontal swipes) but I found no peer-reviewed comparison. Also "Quickly changing two parameters with a 2D control" results unknown. Gap.

### Inferences
- A "Look" (global style) should be chosen per user and per photographer, because inter-retoucher variability is a measured feature of this task, not noise. Treat auto-correction as the neutral per-photo normalization (exposure, WB, horizon) and the Look as the stylization layer; this separation matches difference learning (user = baseline + personal delta).
- Learn from the user's tweaks (SelPh, Kang): after N consistent adjustments to similar scenes, offer "make this your default for backlit portraits" (confirmation, not silent change, per Amershi G14).
- Provide a "Which looks better?" A/B choice during onboarding (SLS-style 1-D line pick) to set the initial Look from ~5-8 comparisons, instead of a grid of 30 presets.
- Serious photographers prefer mild effects (Bakhshi) → default Look strength moderate; Look strength slider available.
- Compare: press-and-hold on image to flip Original/Auto (same screen location; change-blindness-friendly), plus a split drag for deliberate comparison. Needs in-app usability testing.

### Gaps
- Kang 2010 exact bibliographic details and any follow-up not verified. No peer-reviewed data on mobile before/after UI or gesture-vs-slider editing speed. No study on whether consumers prefer stronger-than-expert edits for WeChat Moments in China (cultural preference; see section 6 gaps).

---

## 3. Classic principles (Norman, Nielsen, Shneiderman, Hick–Hyman, Fitts, KLM/GOMS, defaults, response times)

### Takeaway
Classic laws mostly argue: make the default the right action, keep choices few and recognizable, make targets big and near the thumb, show results within ~100 ms and never block on 1-10 s work without progressive feedback, and use a KLM-style budget to hold the happy path to under about a minute.

### Cited Findings
- Norman, "The Design of Everyday Things" (1988; rev. 2013): affordances (Gibson), signifiers, mapping, feedback, conceptual model, gulfs of execution and evaluation. [background; book] — https://jnd.org/
- Nielsen 1994 (CHI '94 / usability heuristics) 10 heuristics: visibility of system status; match with real world; user control and freedom; consistency and standards; error prevention; recognition rather than recall; flexibility and efficiency of use; aesthetic and minimalist design; help recognize/diagnose/recover from errors; help and documentation. [background] — https://www.nngroup.com/articles/ten-usability-heuristics/
- Shneiderman 1983 (IEEE Computer 16(8)) "Direct manipulation: A step beyond programming languages": continuous representation of objects, physical actions over complex syntax, rapid reversible incremental actions with immediate feedback. Eight Golden Rules (Designing the User Interface): consistency; universal usability / shortcuts; informative feedback; dialogs with closure; error prevention and simple handling; easy reversal of actions; internal locus of control; reduce short-term memory load. [background] — https://doi.org/10.1109/MC.1983.1654471
- Response-time limits: 0.1 s feels instantaneous; 1 s keeps flow of thought but user notices delay; 10 s is the limit for keeping attention; show progress for >1 s (and percent-done indicator for >10 s). Origin: Miller 1968; Card, Robertson & Mackinlay 1991; popularized by Nielsen 1993. [background; NN/g summary, not itself peer reviewed] — https://www.nngroup.com/articles/response-times-3-important-limits/
- Hick–Hyman law: choice RT = a + b·log2(n+1); applies best to unpracticed, unordered menus; with practice and ordering, search time follows other models (e.g., Seow 2005 review; Cockburn). Landauer & Nachbar 1985 (CHI '85) show menu selection time depends on breadth via Hick plus Fitts. Large-breadth menus can outperform deep ones (Miller 1981; Kiger 1984). [background] — Hick 1952 https://doi.org/10.1080/17470215208416600 ; Hyman 1953 https://doi.org/10.1037/h0056940 ; Seow 2005 https://doi.org/10.1207/s15327051hci2003_3 ; Landauer & Nachbar https://doi.org/10.1145/317456.317470. Note: HCI folklore of "7±2 items" (Miller 1956) is a short-term-memory result, not a menu-size rule — FOLKLORE as a design limit.
- Fitts's law: MT = a + b·log2(D/W + 1) (MacKenzie 1992 Shannon form). Touch pointing deviates because of finger occlusion and imprecision (see section 4). [background] — Fitts 1954 https://doi.org/10.1037/h0055392 ; MacKenzie 1992 https://doi.org/10.1207/s15327051hci0701_3
- KLM/GOMS: Card, Moran & Newell 1980 (CACM 23(7)) "The keystroke-level model for user performance time with interactive systems" operators K≈0.2 s (average typist) 0.28 s, P≈1.1 s, H≈0.4 s, M≈1.35 s, D variable, R system response. [background] — https://doi.org/10.1145/358886.358895 . Touch-adapted operator times: Rice & Lartigue 2014 "Touch-level model (TLM)" ACM SE '14 [background, unverified numbers]; El Batran & Dunlop 2014 "Enhancing KLM (Keystroke-Level Model) to fit touch screen mobile devices" MobileHCI '14. https://doi.org/10.1145/2628363.2628385 [background, not re-fetched].
- Default effects: Johnson & Goldstein 2003 (Science 302) organ donation consent 4.25-fold-ish differences between opt-in/opt-out countries (e.g., ~12% vs ~99%); Thaler & Sunstein "Nudge" (2008). Defaults are sticky due to effort, implied endorsement, and loss aversion. [background] — https://doi.org/10.1126/science.1091721
- Progressive disclosure: Nielsen (1995) / Carroll & Rosson's "training wheels" research; advanced options deferred to secondary screens improve learnability and error rate. [background] — https://www.nngroup.com/articles/progressive-disclosure/
- Progressive rendering: Show low-resolution/embedded JPEG preview first, refine on demand — consistent with the 0.1 s / 1 s limits. No peer-reviewed mobile-photo-specific study found; practice evidence only.

### Inferences
- Presets: with Hick–Hyman, time grows logarithmically, so cost is smaller than folklore implies when the user can recognize options (thumbnails); the issue is decision quality and choice overload (section 6), not just reaction time. Show 1 (current Look) + a horizontal strip of ~5-8 visually previewed alternatives, "more" behind a tap. [MODERATE; design judgment]
- Default effect: the auto-developed state is the default; most users will keep it, so the default's quality is the product. Opt-out must be easy and global (Amershi G17).
- Performance budget: grid thumbs from embedded preview <100-200 ms; full-res develop swap <1 s; per-slider drag feedback at 60+ fps via GPU preview at reduced resolution; show progress for batch share render (>1 s).
- Norman: use signifiers for "Auto" (a badge on thumbnails), mapping (slider direction = effect direction), clear conceptual model "Original → Auto Look → My tweaks → Posted".

### Gaps
- Not re-fetched: Norman, Shneiderman, Card et al., Hick-Hyman. Touch-specific KLM operator values need checking against El Batran & Dunlop / Rice & Lartigue before use. No primary source for an ideal number of presets in a mobile editor.

---

## 4. Mobile ergonomics (grip, thumb reach, target size, foldable one-handed)

### Takeaway
Most users hold phones one-handed and operate by thumb, so frequent controls belong in the lower half; targets should be about 9-10 mm for thumb. A large OPPO Find N5 (folded ~6.4 in cover screen; unfolded ~8.1 in) means top-of-screen reach is poor and unfolded mode needs a two-hand/tablet layout.

### Cited Findings
- Hoober 2013 ("How Do Users Really Hold Mobile Devices?", UXmatters; observation of 1,333 people in public): ~49% one-handed grip, ~36% cradled, ~15% two-handed in portrait typing; observational, not peer reviewed; 2013 phones were smaller. FOLKLORE-to-MODERATE. [background] — https://www.uxmatters.com/mt/archives/2013/02/how-do-users-really-hold-mobile-devices.php
- Bergstrom-Lehtovirta & Oulasvirta 2014 (CHI '14) "Modeling the Functional Area of the Thumb on Mobile Touchscreen Surfaces": quadratic model of reachable thumb area from surface size, hand size and index finger position; 20 participants. [verified] — https://doi.org/10.1145/2556288.2557354
- Parhi, Karlson & Bederson 2006 (MobileHCI '06) "Target size study for one-handed thumb use on small touchscreen devices": ~9.2 mm for discrete and 9.6 mm for serial tasks are large enough; small screens (PDA-sized). [verified] — https://doi.org/10.1145/1152215.1152260 . A less formal replication suggested 6.8 mm minimum on a small device, not peer reviewed.
- Other target-size sources: Apple HIG 44 pt; Material 48 dp (~9 mm) with 8 dp spacing; MIT Touch Lab fingertip widths 8-10 mm (Dandekar et al. 2003). [background] https://developer.apple.com/design/human-interface-guidelines/ ; https://m3.material.io/
- Bottom-placed controls: Karlson & Bederson 2007 (INTERACT?) "ThumbSpace" and Karlson, Bederson & SanGiovanni 2005 (CHI '05) "AppLens and LaunchTile": thumb one-handed navigation designs. [background] https://doi.org/10.1145/1054972.1055040
- Foldable one-handed use: no peer-reviewed study of foldables I could verify. Vendor/OS guidance (Android large screens / foldables guidelines, "canonical layouts") exists. [background] https://developer.android.com/guide/topics/large-screens

### Inferences
- Primary controls (select, share, Auto-strength slider, compare) in the bottom ~40% of the screen, in the thumb arc; destructive/rare controls at the top. Sliders horizontal at bottom, thumb-draggable, with ≥48 dp track height. Targets ≥9-10 mm (≈48-56 dp on Find N5 density), spacing ≥ 8 dp.
- Unfolded mode: use a list-detail layout (grid left, edit right), with controls near the bottom edges where both thumbs rest; cover-screen mode = the default "fast path".
- Selection: multi-select via tap on a thumbnail checkmark zone (large) plus drag-select across rows; the share bar anchored at the bottom.

### Gaps
- No direct foldable reach data (OPPO Find N5 unfolded 8.12 in). Hoober's sample is dated and non-peer-reviewed.

---

## 5. Color perception, viewing conditions, and light vs. dark UI for an editor

### Takeaway
Judging brightness and color is context-dependent: the surround shifts perceived lightness/contrast and the adapted white point. Professional standards prescribe neutral, mid-gray, dimmer surrounds for critical evaluation. A light UI is defensible for readability (positive polarity), but should use neutral gray chrome and a mitigated image-viewing mode; auto-correction should be computed from image data, not from the viewer's perception on screen.

### Cited Findings
- Simultaneous contrast and the crispening/Bartleson–Breneman effect: images viewed in a dark surround look lower in contrast than in a lighter surround; dim-surround viewing requires gamma/contrast adjustment (Bartleson & Breneman 1967, JOSA 57(7)). [background] https://doi.org/10.1364/JOSA.57.000953 — Fairchild, "Color Appearance Models" (3rd ed., 2013, Wiley) covers simultaneous contrast, chromatic adaptation, CIECAM02 surround parameter. [background]
- Chromatic adaptation: the visual system partially discounts the illuminant; display white point and ambient light affect perceived neutral. [background] Fairchild 2013; Brainard & Stockman.
- ISO 3664:2009 "Graphic technology and photography – Viewing conditions": for critical evaluation of reflection prints/transparencies, D50 illumination (5003 K), P1 (critical) 2000 lx ± 250 for prints, surround neutral gray and not higher than ~(about) 60 % reflectance / 'Munsell N5-ish' neutral; for displays, recommends display luminance ~ and ambient lower. ISO 12646:2015 "Displays for colour proofing – Characteristics and viewing conditions": display white around 80-120 cd/m², white point D65 (or D50 by agreement), ambient illuminance < 32 lux... The numerical values are from memory, not verified. [background, unverified] — https://www.iso.org/standard/72690.html (ISO 3664:2009) ; https://www.iso.org/standard/63241.html (ISO 12646)
- Piepenbrock, Mayr, Mund & Buchner 2013 (Ergonomics 56(7):1116-1124) positive polarity (dark text on light) advantage in acuity and proofreading in both young and older adults; Piepenbrock, Mayr & Buchner 2014 (Ergonomics) show smaller pupils and better proofreading with positive polarity; explained by luminance, pupil constriction, depth of field. [verified] — https://doi.org/10.1080/00140139.2013.790485 . Caveat: these tests concern text legibility, not photo evaluation. Related: Dobres et al. 2016 on dark mode (Applied Ergonomics) [background, not verified].
- Dark mode and eye comfort: weak evidence for general benefits; benefits include lower glare in dim environments; harms include halation for astigmatic users; no peer-reviewed evidence found specifically for photo editing quality.

### Inferences
- Light theme: use neutral gray chrome (no tinted whites; avoid colored accents near the image); a surround around the photo of mid-light neutral gray (e.g., L*≈ 50-80) rather than pure #FFF so that white surrounds do not make shadows look crushed or the image look darker. On pure white surrounds, images appear darker and more contrasty (simultaneous contrast) — users may over-brighten; auto-development must not depend on the user's viewing adjustment.
- Offer an automatic "focus view": when dragging an adjustment slider or holding compare, fade chrome to neutral gray (like Lightroom's "lights out"/dim mode) — mitigates white surround effects while keeping the light UI. This is a design inference, no direct test found.
- Use display-referred color management: P3 on iPhone 17 Pro Max; OPPO Find N5 display-mode differences (vivid vs. natural) will shift perceived color: provide "color-accurate" mode caution (Gap: panel calibration). Brightness-adaptive outdoor viewing cannot be solved in-app.
- Because WeChat Moments is viewed on arbitrary phones in arbitrary conditions, avoid over-tuning to a single display; moderate Look.

### Gaps
- Exact ISO 3664/12646 numerical tolerances not verified (ISO docs paywalled). No peer-reviewed study of light-UI surround effects on mobile photo-editing decisions. Dark-mode literature not surveyed.

---

## 6. Photo-sharing behavior: selecting best shots, curation fatigue, choice overload

### Takeaway
Selecting among many similar photos is a real cognitive load; choice-overload effects are context-dependent (meta-analysis: mean effect near zero, but appears with high complexity, no clear preference, and difficult tasks). Reduce the candidate set (grouping, ranking, burst stacks) and keep final selection human.

### Cited Findings
- Iyengar & Lepper 2000 (JPSP 79(6)) jam study: 24 vs. 6 options; purchase 3% vs. 30%; widely cited; replication evidence mixed. [background] https://doi.org/10.1037/0022-3514.79.6.995
- Scheibehenne, Greifeneder & Todd 2010 (J. Consumer Research 37(3)) meta-analysis of 50 experiments: mean effect size roughly zero; overload moderated by choice-set complexity, decision task difficulty, preference uncertainty, and decision goal. [background] https://doi.org/10.1086/651235 . Chernev, Böckenholt & Goodman 2015 (J. Consumer Psychology) meta-analysis identifies similar moderators. [background] https://doi.org/10.1016/j.jcps.2014.08.002
- Schwartz et al. 2002 (JPSP 83(5)) "Maximizing versus satisficing": maximizers report less satisfaction and more regret. [background] https://doi.org/10.1037/0022-3514.83.5.1178
- Photo selection and curation: Kirk et al. 2006 (CHI '06) "Understanding Photowork" — digital photowork includes downloading, reviewing, selecting, organizing, editing, sharing; considerable "photowork" burden. [background] https://doi.org/10.1145/1124772.1124885 . Also Frohlich et al. 2002 (Personal and Ubiquitous Computing) on photo sharing practices. Not verified this session.
- WeChat Moments: up to 9 images per post (platform fact). Image-sharing behavior in China-specific studies: not found. [background; platform behavior should be re-checked]
- Bakhshi 2015 (above) is the only verified link between editing and audience engagement.

### Inferences
- Stack near-duplicates (burst/time/scene) and show the "pick" first (sharpness, closed eyes, blink). Keep human override; this reduces candidate count and curation fatigue. The 9-image cap is a natural selection UI: a counter "6/9" in the share bar.
- Provide "best 9" suggestion but never auto-post (Horvitz: cost of poor guesses; social-norm risk G5).
- Avoid maximizing: encourage satisficing with "good enough" cues.

### Gaps
- No peer-reviewed study of WeChat Moments posting workflows. Overload evidence is from consumer choice; transfer to photo picking is by analogy.

---

## 7. Concrete design requirements and KLM-style task-time estimate

### Takeaway
Twenty-six requirements follow, each tagged with sources and strength. The happy path "photo arrives to posted in Moments" should be about 25-45 s for a 6-9 photo post, dominated by selection and the WeChat handoff rather than editing.

### Cited Findings (requirements)
1. Auto-develop on arrival, before the user opens the app; the default state of every thumbnail is the developed rendition. (Defaults: Johnson & Goldstein 2003; Horvitz 1999 value-added automation; STRONG for defaults)
2. Show a persistent "Auto" badge plus a one-line what-changed chip for each photo. (Amershi G11, Lee & See process/purpose info; MODERATE)
3. One global "Auto strength" slider (0-100%), default from the user's learned value; per-photo override. (Horvitz refine collaboratively; Dietvorst 2018 modifiable algorithm; MODERATE)
4. Press-and-hold on the image toggles Original vs. Auto at the same screen position. (Rensink et al. 1997 change detection; Nielsen visibility; MODERATE; usability test needed)
5. "Revert to auto" and "Revert to original" one tap each; per-slider double-tap reset; batch undo toast after bulk ops. (Shneiderman rule 6; Nielsen H3; Amershi G9; STRONG practice)
6. Non-destructive edits; originals (RAW+JPEG pair) never overwritten. (Nielsen H3/H5)
7. Onboarding: set the Look by 5-8 A/B comparisons (SLS-style) or by choosing a sample photo set, not a 30-preset wall. (Koyama 2017; Bychkovsky 2011 variability; MODERATE)
8. Look strength separate from auto-correction; defaults moderate. (Bakhshi 2015 serious users prefer mild; MODERATE)
9. Learn from tweaks but only propose changes ("Make this default for backlit scenes?"); announce when defaults change. (SelPh 2016; Kang 2010; Amershi G13, G14, G18)
10. When auto confidence is low (extreme backlight, mixed light), reduce correction and flag the photo "check". (Horvitz expected-utility; Amershi G10; Lee & See calibration)
11. Show at most ~5-8 preset thumbnails at once, with live previews; the rest behind "More". (Hick–Hyman; Scheibehenne 2010 moderators; MODERATE)
12. Edit panel: 3-5 sliders first (Brightness, Contrast/Tone, Warmth, Color, Sharp), advanced behind disclosure. (Progressive disclosure; Nielsen H7/H8; MODERATE)
13. Preview feedback ≤100 ms per slider change; ≤1 s to display full-res after tap; progress indicator for operations >1 s. (Response-time limits; Miller 1968; Nielsen 1993; STRONG practice)
14. Progressive rendering: embedded JPEG preview first, then develop result swap-in without layout shift. (Response-time limits; practice)
15. All frequent controls in the lower half of the screen (thumb arc); rare/destructive controls at top. (Bergstrom-Lehtovirta & Oulasvirta 2014; Hoober 2013; MODERATE)
16. Touch targets ≥9-10 mm (≥48 dp) with ≥8 dp spacing; checkmark selection zones generous. (Parhi et al. 2006; Material/Apple guidance; MODERATE-STRONG)
17. Fold-aware layouts: cover screen = single-column fast path; unfolded = list-detail with grid plus edit pane; keep continuity across fold state. (Android large-screen guidance; no peer-reviewed support; WEAK)
18. Multi-select: tap to toggle, drag to range select, "select all in stack"; sticky bottom share bar with count (n/9). (Fitts; Shneiderman direct manipulation; WeChat 9-image limit)
19. Group near-duplicates into stacks with a suggested pick; one tap to expand/swap. (Choice overload moderators; Kirk et al. 2006 photowork; MODERATE)
20. Never auto-post; the share action is explicit and shows what will be sent (the developed version, resolution). (Horvitz cost of poor guesses; Amershi G16, G5)
21. Pre-share outlier check: flag large auto moves, clipped highlights, or low-confidence photos. (Parasuraman & Manzey 2010 complacency; Amershi G2)
22. Neutral gray UI chrome; no saturated accent colors adjacent to the image; image surround mid-neutral rather than pure white; chrome dims on drag/compare. (Bartleson & Breneman 1967; ISO 3664/12646 neutral surround; Fairchild 2013; MODERATE; mitigation is inference)
23. Light theme default is acceptable for readability; offer dark/system theme setting. (Piepenbrock 2013; text-legibility evidence only; WEAK for photo judgment)
24. Auto-correction computed from image statistics and color-managed (P3/sRGB), independent of display brightness. (Fairchild; practice)
25. Global controls: auto-develop on/off, default strength, excluded folders or cameras, and "reset learning". (Amershi G17; Google PAIR feedback and control)
26. Granular lightweight feedback: "Too strong / Too weak / Good" per photo that adjusts strength priors; no modal surveys. (Amershi G15; SelPh)
27. Fast path optimizes keystroke count: if user selects and shares without opening any edit panel, zero M-operators for editing. (KLM; Card et al. 1980)
28. Test with skin-tone diverse photo set; avoid beautification defaults. (Amershi G6; Roth 2009 "Looking at Shirley, the Ultimate Norm" Camera Obscura — [background])
All items need usability validation; items 17, 22-23 are weakly evidenced.

KLM-style estimate, "photo arrives → posted to Moments", using Card et al. 1980 operators (P 1.1 s, K/tap ≈0.2-0.3 s, M 1.35 s, H 0.4 s) and assumptions flagged below. Touch-specific values are approximations; system latencies R are assumptions.

Assumptions: 8 photos from a shoot; app already open or launched from notification; no edit tweaks on 6 of 8; 1 photo gets a single strength tweak; WeChat Moments handoff via share sheet.

| Step | Operators | Time (s) |
|---|---|---|
| Tap notification / open app (P + tap + R≈1.0) | 1.1 + 0.25 + 1.0 | 2.4 |
| Scan the grid, decide which to share (M, recognition) | 2 × M | 2.7 |
| Select 8 photos (tap each ≈ P_short 0.6 + tap 0.25; or drag-select 1 range ≈ 1.5) | tap select: 8 × 0.85 = 6.8 ; drag-select alt: ~2 | 2-7 |
| Spot-check 1 photo, open it (P+tap 1.35), M 1.35 | 2.7 | 2.7 |
| One tweak: drag Auto strength slider (P 1.1 + drag 1.0), M check 1.35, back (P+tap 1.35) | ~4.8 | 4.8 |
| Tap Share (P+tap) | 1.35 | 1.4 |
| System share sheet / WeChat selection, then Moments (R 1.0 + 2 × (P+tap 1.35)) | 3.7 | 3.7 |
| WeChat compose: R 1.5 for image load, optional caption skipped (M 1.35), tap Post (P+tap 1.35) | 4.2 | 4.2 |
| Total, happy path with drag-select | | about 25 s |
| Total, tap-selecting 8 photos and one tweak | | about 30 s |
| Total, with a caption typed (about 10 characters at K 0.3 s plus H 0.4 and M 1.35) | | about +5-10 |

Target: 25-35 s for no-edit/one-tweak posts, p90 below 60 s. Baseline for comparison: the conventional workflow of per-photo opening Lightroom Mobile or a preset app, adjusting, exporting, then sharing is commonly several minutes; I found no published measurement, so no baseline is claimed.

Caveat: the number of images Moments accepts through third-party share intents, and whether Dapple can deep-link directly to the Moments composer with multiple images, is not verified (WeChat Open Platform sharing SDK supports limited types). This is the largest uncertainty in the estimate and could add manual steps (saving to gallery then selecting in WeChat: +8-15 s). Must be checked at https://developers.weixin.qq.com/doc/oplatform/ (not fetched).

### Inferences
- The edit step contributes <10 s of a ~30 s flow in this design, validating the "remove editing as bottleneck" promise; the dominant remaining costs are selection (M and taps) and the WeChat handoff.
- Biggest levers: drag-select ranges, stack-pick suggestions, and a 1-step WeChat handoff.

### Gaps
- KLM touch operator values are approximate and unverified in-session; actual times need user timing studies. WeChat Moments multi-image handoff capability unverified.
