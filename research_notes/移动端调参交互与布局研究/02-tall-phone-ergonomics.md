# Tall-phone ergonomics: grip, thumb reach, gaze, occlusion, and layout rules for a RAW photo editor

Scope: how people hold and operate tall slab phones (about 19.5:9 to 21:9) and what follows for placing the photo and the controls in Dapple. Foldables are covered elsewhere. Evidence quality varies and is flagged in each section. Fetched in full: Hoober 2013 column, Hoober 2017 column, Samsung One UI basic-layout page, and the PDFs of Le et al. 2018, Mayer et al. 2019 and Vogel & Balakrishnan 2010 (text extracted locally). Search-snippet-only: Bergstrom-Lehtovirta & Oulasvirta 2014, Trudeau et al. 2012, Eardley et al. 2017, the Karlson/Parhi target-size study, Apple HIG and Android gesture docs.

A short HCI primer for the owner is in "Inferences" under each section.

---

## 1. How do people hold phones, and where does the thumb actually reach?

### Takeaway
People hold phones one-handed about half the time, switch grips often, and the thumb of the holding hand reaches an arc-shaped region that does not cover the top of a tall phone. Studies that include larger phones show the comfortable zone moves up and toward the upper-left of the screen as the phone grows (for right-handed use), so the "bottom third" rule of thumb is only half right: the bottom is reachable, but the grip itself shifts upward on big phones.

### Cited Findings
- Hoober's 2013 field observation (1,333 observations, 780 people touching the screen, in streets, airports, cafes, transit): one-handed 49%, cradled (one hand holds, other hand touches) 36%, two-handed 15%. In one-handed use the right thumb was used 67% and left thumb 33%. When cradling, thumb 72% / finger 28%, and the left hand held 79% of the time. Two-handed use was portrait 90% / landscape 10%. Hoober notes these are observations, not a count of distinct people. — [Hoober 2013, UXmatters](http://www.uxmatters.com/mt/archives/2013/02/how-do-users-really-hold-mobile-devices.php)
- Same column: users change grip "very often", sometimes every few seconds, often tracking task changes. — [Hoober 2013](http://www.uxmatters.com/mt/archives/2013/02/how-do-users-really-hold-mobile-devices.php)
- Hoober's 2017 follow-up: 75% of users touch the screen with only one thumb, fewer than 50% hold with one hand, 36% cradle, 10% hold in one hand and tap with a finger of the other. He calls the common "thumb sweep" reach charts incorrect (Back-button use is high even in the upper-right corner) and says people prefer to view and touch the centre of the screen. Touch accuracy is higher at the centre, so he sizes targets at about 7 mm in the centre and about 12 mm in corners (sized to contain 95% of observed taps). He recommends primary content in the centre, secondary actions along top and bottom edges, tertiary functions in menus/corners. Evidence base cited: 1,300+ people observed in public, 651 more in schools/offices/homes, and a 120M-touch-event dataset. — [Hoober 2017, UXmatters](https://www.uxmatters.com/mt/archives/2017/03/design-for-fingers-touch-and-people-part-1.php)
- Bergstrom-Lehtovirta & Oulasvirta (CHI 2014, pp. 1991-2000) built a predictive model of the area the thumb of the holding hand can reach, from screen size, hand size and the position of the index finger on the back of the device. Lab study, small set of device sizes. DOI 10.1145/2556288.2557354. — [BibTeX record](https://bibtex.github.io/CHI-2014-Bergstrom-LehtovirtaO.html); [Mayer et al. 2019 description of its limits](https://www.medien.ifi.lmu.de/pubdb/publications/pub/mayer2019findingthe/mayer2019findingthe.pdf)
- Le, Mayer, Bader, Henze (CHI 2018, DOI 10.1145/3173574.3173605), motion-capture study, one-handed portrait, four phones (S3, S4, OnePlus One, Nexus 6, i.e. up to about 6 inch): mean "comfortable area" (reachable without grip change or losing stability) of the thumb on the front is 36.4 cm^2 (SD 3.3), index finger on the back 41.1, middle 31.3. The grip shifts toward the top edge as the phone gets larger, so comfortable areas and the finger base positions move up. The upper-left corner is not reachable by the (right) thumb without a grip change, and the gap grows with device size. Authors advise placing controls higher on larger devices. Participants were right-handed. Note that the paper's focus is back-of-device input; the thumb numbers are a by-product. — [Le et al. 2018 PDF](https://www.medien.ifi.lmu.de/pubdb/publications/pub/le2018fingersrange/le2018fingersrange.pdf)
- Le et al. also state that being forced to change grip reduces usability and "can even lead to dropping the phone". — [Le et al. 2018 PDF](https://www.medien.ifi.lmu.de/pubdb/publications/pub/le2018fingersrange/le2018fingersrange.pdf)
- Mayer et al. (ISS 2019, DOI 10.1145/3343055.3359705): 45.9M touch events from 607 devices (two Android games, in the wild). The "sweet spot" (centroid of where touches land when input can be anywhere) drifts toward the upper-left corner as the device grows (regression R^2 about .55 over 3.5 to 10.1 inch; .65 for 3.5-6.0 inch), agreeing with the lab models. Limitation: games, mostly right-handed players, portrait only. — [Mayer et al. 2019 PDF](https://www.medien.ifi.lmu.de/pubdb/publications/pub/mayer2019findingthe/mayer2019findingthe.pdf)
- Mayer et al.: the area of the screen touched grows with screen size but the fraction shrinks. Tap-touched area is about 23.9 cm^2 for 5-7 inch screens, about 39% of the area at 3-4 inch but about 19.5% at 6-7 inch (Table 1 in paper; for Flappy Bird taps). So on 6-7 inch phones roughly four-fifths of the screen is never used for a tap-anywhere task. — [Mayer et al. 2019 PDF](https://www.medien.ifi.lmu.de/pubdb/publications/pub/mayer2019findingthe/mayer2019findingthe.pdf)
- Thumb biomechanics (Trudeau, Udtamadilok, Karlson, Dennerlein, Human Factors 2012, 20 right-handed participants, four phones): thumb performance (Fitts index of performance) was generally better on smaller devices; better for adduction-abduction than flexion-extension movement orientation; better for "outward" than "inward" directions. — [CDC stacks record](https://stacks.cdc.gov/view/cdc/193322) (abstract only; effect sizes not checked)
- A related Trudeau study found worst performance was associated with excessive thumb flexion, such as tapping keys closest to the thumb base in the bottom corner on the holding side. — search snippet; see [Trudeau PLoS ONE 2014 record](https://doaj.org/article/2de19cd8dd1043d79d5fe0bc7a4b530c) (snippet only)
- Grip shifts (Eardley, Roudaut, Gill, Thompson, CHI 2017, DOI 10.1145/3025453.3025835): each phone form factor affords specific grips, and users shift grip, and thereby phone tilt and rotation, depending on context of interaction. 18 participants, older phones. — [Cardiff Met repository](https://pure.cardiffmet.ac.uk/en/publications/understanding-grip-shifts-how-form-factors-impact-hand-movements-/) (abstract only)
- Karlson-line target-size work (Parhi, Karlson, Bederson, MobileHCI 2006): 9.2 mm targets for discrete tasks, 9.6 mm for serial tasks in one-handed thumb use. — [search summary of paper](https://www.telerik.com/docs/default-source/blogs-docs/parhi-mobilehci06-pdf) (snippet only; very small old screens)
- A 2026 abstract on smartphone map apps reports reachable range varies significantly with grip and is smallest in the one-handed grip; the thumb's small range is the cause. — [ICA abstract 2026](https://ica-abs.copernicus.org/articles/12/121/2026/ica-abs-12-121-2026.pdf) (snippet only)

### Inferences
- Primer: the thumb pivots around its base joint (CMC joint at the heel of the hand), so its fingertip sweeps an arc. Close to the base it must curl hard (poor performance); at far reach it must stretch (loses accuracy and grip stability). The comfortable middle is a band, not "the bottom". On a bigger phone the palm sits higher so the band moves up.
- None of the lab models in Le et al. include phones as tall as 6.6 inch / 20.6:9. Rough geometry I computed for the OPPO outer screen (6.62 inch, 2616x1140): about 67 mm wide x 153 mm tall. A typical adult thumb range is far shorter than 153 mm, so the top third is almost certainly out of reach without a grip shift. This is my arithmetic, not a measured result.
- Because grips switch every few seconds (Hoober), design must tolerate left/right/two-handed use rather than optimise for a single grip. Hoober's 67/33 split means about a third of one-handed thumbs are left thumbs, and left-handedness in the population is only about 10%, so many right-handers use the left hand too.
- Hoober's centre-of-screen preference and the thumb-reach literature are partly in tension (Hoober says old reach charts are wrong; Le/Mayer say reach moves up and in). Safest reading: the middle of the screen is the best bet for the most frequent controls; corners cost accuracy.

### Gaps
- No peer-reviewed study found that measures thumb reach on phones taller than about 6 inch / 20:9 with unrestricted grips. Large-phone numbers (e.g., 6.7 inch Pro Max sizes) must be extrapolated.
- Full text/effect sizes for Bergstrom-Lehtovirta 2014 and Trudeau 2012 were not retrieved; only abstracts/snippets.
- Hoober's 2013 data are observational, from an early smartphone era (before 6.7 inch phones), and not peer reviewed. Hoober himself says his earlier reach drawings should be ignored (2017 comment/column).

---

## 2. Tall (20:9-21:9) screens: viewing vs interaction area, Reachability, bottom sheets, how much screen the photo should take

### Takeaway
Both major platform vendors converge on "content up top, controls low": Samsung One UI formalises a top viewing area and bottom interaction area; Android's canonical layouts place supporting content below the main content or in a bottom sheet on compact windows; iOS has Reachability as a patch for what layout should have solved. Nobody publishes a numeric ratio for the split; for a photo editor the ratio is a design decision I could not source.

### Cited Findings
- Samsung One UI basic layout: "The top part of the screen is the viewing area where titles appear"; "The rest of the screen is the interaction area, where user actions occur." The viewing area is kept free of touch components, with wide margins. No proportions are given on the page. — [Samsung Developer, One UI basic layout](https://developer.samsung.com/one-ui/layout/basic.html)
- Samsung design interview (2020): One UI split the screen into passive viewing (top) and interactive (bottom); One UI 2 moved most tasks into the lower area. — [Samsung Mobile Press interview](https://samsungmobilepress.com/articles/interview-how-designers-have-made-one-uis-usability-even-better-for-galaxy-users) (snippet; secondary criticism: SamMobile says One UI 6 moved some toggles back up; unverified)
- Samsung also states stretched-out apps on large screens waste space and should use multi-pane layouts. — [Samsung One UI large screen intro](https://developer.samsung.com/one-ui/largescreen-and-foldable/intro.html) (snippet)
- Android canonical layouts (supporting pane): on compact windows place supporting content below the main content or in a bottom sheet. Android guidance covers this but says nothing about thumb reach. — [Android Developers, Canonical layouts](https://developer.android.com/develop/adaptive-apps/guides/canonical-layouts)
- Apple HIG layout: minimum tappable area 44x44 pt; align full-width bottom buttons with the bottom of the safe area so they do not collide with the Home indicator; system gestures at the bottom edge may cancel custom gestures there. — [Apple HIG Layout](https://developer.apple.com/design/human-interface-guidelines/layout) (via search summary of the page)
- Apple Reachability: Le et al. cite it (and BoD interaction, gestures, screen-shifting shortcuts) as workarounds that "require additional actions from the users" and only work for touchscreens. — [Le et al. 2018 PDF](https://www.medien.ifi.lmu.de/pubdb/publications/pub/le2018fingersrange/le2018fingersrange.pdf)
- Microsoft Research found Shift (occlusion) and ThumbSpace (reach) work well together; each addresses a separate problem. — [Karlson et al., MSR](https://www.microsoft.com/en-us/research/wp-content/uploads/2008/01/p1399-karlson.pdf) (snippet)

### Inferences
- Photo-top / controls-bottom is the pattern both Samsung and Android point at, so the owner's current layout is in the mainstream. The felt disconnect is probably not "wrong order" but (a) the control panel being a long scroll list so the thumb must travel and the eyes must jump, and (b) the photo being far from the controls it responds to (see section 3).
- Splitting at about the middle of a 153 mm screen would give a photo area and a control area each about 75 mm tall, but this is arithmetic, not research. A 3:4 or 2:3 photo in a 67 mm wide screen is about 90-100 mm tall, which fits the upper 60% of a tall phone and leaves about 55 mm for controls. That is why a bottom-sheet "controls drawer" with a peek/half/full state is natural.
- Principle behind Samsung's split: the eyes can read the top of a tall phone at no cost, but the thumb cannot reach it. Separating "what I look at" from "what I touch" is the design response.

### Gaps
- No published numeric proportion (e.g., percent of screen for content vs controls) from Samsung, Google or Apple. Material 3's bottom sheet guidance on thumb reach was not retrieved.
- Apple's HIG text on Reachability itself was not retrieved (search found no Apple-specific Reachability guidance).
- No source found for photo-editor apps' own layouts; likely covered by another researcher.

---

## 3. Visual attention vs finger position, eyes-free operation, occlusion, handedness

### Takeaway
In a photo editor the eyes must stay on the photo while the finger adjusts a value, so controls that can be operated without looking (big, stable positions, relative drags, haptic landmarks/ticks) pay off. The finger and hand also cover part of the screen; occlusion models show the hand hides a large and predictable region, so the area the user must judge should be away from it.

### Cited Findings
- Haptic surface feedback (CHI 2022 "Eyes-Off Your Fingers"): after training with visual/auditory feedback, users adjusted a setting on a haptic touchscreen without looking; gradual tactile feedback reduced visual distraction. — [Aminer record](https://www.aminer.cn/pub/6287041e5aee126c0f5b13ad/eyes-off-your-fingers-gradual-surface-haptic-feedback-improves-eyes-free-touchscreen) (abstract level)
- HaptiCase (CHI 2015): back-of-device tactile landmarks significantly improved eyes-free target hitting accuracy on a smartphone. — [Corsten et al. 2015](https://hci.rwth-aachen.de/publications/corsten2015a.pdf) (snippet)
- Tangible extendable slider (AVI 2018): the graphical equivalent needed visual attention and was about 0.9 s slower in the reported comparison. — [HAL record](https://hal.archives-ouvertes.fr/hal-03014603) (snippet)
- Vogel & Balakrishnan, Occlusion-Aware Interfaces (CHI 2010, DOI 10.1145/1753326.1753365): with direct pen input the hand/forearm covers large parts of the display; they built a configurable geometric hand-occlusion model accounting for handedness; their Occlusion-Aware Viewer cut task time by up to 23% when the monitored value was in an often-occluded position, but increased time by 24% in one position where the occlusion was ambiguous. Related earlier work (CHI 2009) found up to 47% of a 12 inch display occluded. Important: this is pen on tablet-sized displays, not thumb on phones. — [Vogel & Balakrishnan 2010 PDF](https://www.dgp.toronto.edu/~ravin/papers/chi2010_occlusionawareinterfaces.pdf); [search summary](https://bibtex.github.io/CHI-2010-VogelB.html)
- Same paper's introduction: occlusion likely contributes to errors, fatigue, inefficient movements; occlusion-aware menus can compensate for handedness (citing Hancock & Booth; Brandl et al.). — [Vogel & Balakrishnan 2010 PDF](https://www.dgp.toronto.edu/~ravin/papers/chi2010_occlusionawareinterfaces.pdf)
- Fat-finger problem (Vogel & Baudisch, Shift, CHI 2007): errors on small targets come mainly from occlusion of the target by the finger plus ambiguity about which part of the finger defines the touch point; Shift shows a callout of the hidden area. Occlusion worsens as screens are smaller, fingers larger, and reach further across the screen. — [Vogel & Baudisch 2007](https://projects.csail.mit.edu/hci-reading/papers/chi2007-vogel.pdf) (via search summary)
- Hoober: users never tap exactly where they intend; keep dangerous controls away from frequent ones. — [Hoober 2017](https://www.uxmatters.com/mt/archives/2017/03/design-for-fingers-touch-and-people-part-1.php)
- Handedness: one-handed use is 67% right thumb / 33% left thumb in Hoober's sample; Mayer et al. argue about 80% of the world population is right-handed and their data is mostly right-hand. — [Hoober 2013](http://www.uxmatters.com/mt/archives/2013/02/how-do-users-really-hold-mobile-devices.php); [Mayer et al. 2019](https://www.medien.ifi.lmu.de/pubdb/publications/pub/mayer2019findingthe/mayer2019findingthe.pdf)

### Inferences
- Primer (split attention): vision is the high-bandwidth channel for judging an image edit; the touch channel is for steering a value. When the control is a small list row at the bottom and the photo is at the top, the eye must either look at the control (losing the photo) or the thumb works "blind". Remedy: make the control legible by feel and by a value readout that appears next to the photo, e.g. an overlaid value chip near the edited area or top of photo, while the thumb stays low.
- Primer (relative vs absolute): a relative drag anywhere in a large strip (like a jog wheel) is eyes-free-friendly because thumb start position does not matter; an absolute slider needs precise acquisition of its handle, which needs vision.
- Occlusion for a right-thumb grip: the thumb and palm cover the lower-right area and a sweep upward; so the photo area to be judged (top and centre) is mostly clear in portrait. The risk is a half-open bottom sheet or the thumb crossing the photo (e.g., a swipe on the photo itself) when the photo is the target. Vogel's tablet/pen model is not directly transferable to phone thumbs; treat as an analogy. No hand-occlusion map measured for thumbs on 6.6 inch phones was found.
- A gaze-versus-touch location study was not found for this scope (see Gaps), so the claim "eyes sit on the photo while fingers are low" is a design assumption consistent with Samsung's viewing/interaction split, not measured.
- Left/right handedness: mirror-able layouts or a handedness setting; avoid a single side-anchored vertical slider rail.

### Gaps
- No primary study found on gaze position versus touch position for continuous parameter adjustment on phones (e.g., eye-tracking of photo editing). Searches returned eyes-free haptic work, not photo-editing gaze work.
- Details of Corsten 2015, the CHI 2022 haptic paper and AVI 2018 slider were from abstracts/snippets; effect sizes unchecked. Haptic touchscreen surfaces (CHI 2022) are not a feature of ordinary phones; phones offer vibration only.
- No empirical hand-occlusion map for phone thumbs found.

---

## 4. Portrait vs landscape; two-thumb grips

### Takeaway
Portrait is the dominant mode on phones; two-handed landscape gives better thumb performance and a steadier device, but landscape shrinks the photo and bottom/side reach differs. Evidence for editor-specific orientation use was not found.

### Cited Findings
- Two-handed portrait/landscape split in Hoober 2013: two-handed use 15% of touch interactions; of those 90% portrait, 10% landscape. — [Hoober 2013](http://www.uxmatters.com/mt/archives/2013/02/how-do-users-really-hold-mobile-devices.php)
- Applied Ergonomics 2016 (Trudeau et al.; 10 right-handed participants, emulated keys): the two-handed grip gave 9% higher Fitts index of performance, 7% faster movement time, 4% more precise taps, and 36-63% lower device movement variability versus one-handed. — [AMiner record](https://www.aminer.org/pub/56d8ddf8dabfae2eeeff7fbe/two-handed-grip-on-a-mobile-phone-affords-greater-thumb-motor-performance) (snippet)
- Mayer et al. cite Sahami Shirazi et al. that portrait is the more common mode, with 84.5% of use being portrait in some contexts (the exact context of that figure is not clear from my extract; the text mentions video and images as exceptions). — [Mayer et al. 2019 PDF](https://www.medien.ifi.lmu.de/pubdb/publications/pub/mayer2019findingthe/mayer2019findingthe.pdf)
- Tablet two-handed study: both orientation and key layout significantly affect thumb posture, comfort and performance; reaching farther from the palm needs more thumb extension/abduction. — [CDC record](https://stacks.cdc.gov/view/cdc/202344)

### Inferences
- Two-handed landscape on a 20:9 phone gives thumbs at both short edges, each with a nice arc over the screen edge zones, leaving the centre for the photo; this is the natural "camera app" grip and probably the natural "serious editing" grip, but the photo gets only about 67 mm of height in landscape on a 6.6 inch phone. This is my geometry, and the usability for editing was not tested in any source I found.
- Offer landscape as an optional layout with controls in left and right edge panels reachable by each thumb; do not require it.

### Gaps
- No source on how often people edit photos in landscape versus portrait, or on thumb performance specifically on 20:9 phones in landscape.
- The 84.5% figure is second-hand through Mayer et al.; check the original (Sahami Shirazi, probably 2014) before quoting.

---

## 5. Gesture comfort and system-gesture conflicts

### Takeaway
Thumb-friendly motions are arcs and outward/adduction-abduction movements, and the edges that look convenient for sliders collide with system gestures: Android's back gesture on both side edges and the mandatory bottom gesture zone, and iOS's Home indicator at the bottom.

### Cited Findings
- Trudeau et al. 2012: adduction-abduction (sideways sweep of the thumb) beat flexion-extension, and outward beat inward movements. — [CDC stacks](https://stacks.cdc.gov/view/cdc/193322)
- Bergstrom-Lehtovirta & Oulasvirta, and Le et al.: the thumb's maximum range is well described by quadratic (arc) curves (R^2 about .96 in the original model according to Le et al.). — [Le et al. 2018 PDF](https://www.medien.ifi.lmu.de/pubdb/publications/pub/le2018fingersrange/le2018fingersrange.pdf)
- Informal evidence (a single author's swipe-tracking tests, not peer-reviewed): users swipe from the screen edge toward the middle, on a diagonal downward path, rather than horizontally; suggested swipe areas at least 45 px. — [Smashing Magazine, 2016](https://smashingmagazine.com/2016/09/the-thumb-zone-designing-for-mobile-users). Treat as weak.
- Mayer et al.: the average swipe in their game was 18 mm long (SD 10.6). — [Mayer et al. 2019 PDF](https://www.medien.ifi.lmu.de/pubdb/publications/pub/mayer2019findingthe/mayer2019findingthe.pdf)
- Android gesture nav: back is an inward swipe from the left or right edge; apps may declare exclusion rects via `setSystemGestureExclusionRects()` but with a limit of 200 dp per edge, and the bottom (home) gesture area is mandatory and cannot be excluded (use `getMandatorySystemGestureInsets()`). Guidance is to move controls with window insets and reserve exclusion for things like drag handles. — [Android Developers, gesture navigation](https://developer.android.com/training/gestures/gesturenav); [Android Developers Medium post](https://medium.com/androiddevelopers/gesture-navigation-handling-gesture-conflicts-8ee9c2665c69)
- iOS: system gestures at the bottom edge may cancel custom gestures; leave space around the Home indicator. — [Apple HIG Layout](https://developer.apple.com/design/human-interface-guidelines/layout) (search summary)

### Inferences
- A horizontal slider that fills the bottom of the screen edge-to-edge is in the worst place on both platforms: bottom 24-34 dp-ish is gesture-contested and the side margins are back-gesture zones. A thumb dragging a handle toward a side edge can trigger Back. Keep drag tracks inset from side edges by an amount that respects the system gesture inset, and keep a bottom inset above the home gesture. Exact dp values should be read from `WindowInsets` at runtime rather than hard-coded; I did not verify them.
- A thumb performs a sideways arc most easily: a horizontal drag across the middle band is comfortable, while a purely vertical thumb drag along a long track requires flexion-extension. This suggests horizontal sliders or a "scrub anywhere on this strip" control in the lower-middle band, and treats vertical long scrolls as the costly gesture.

### Gaps
- No peer-reviewed study that compares vertical vs horizontal vs diagonal thumb swipe comfort on modern large phones was found.
- No source on a precise bottom gesture inset in dp across devices (varies by device/OS version).

---

## 6. Concrete layout rules for Dapple (derived; each marked with its evidence basis)

### Takeaway
Treat the screen as three vertical bands: a "look" band at the top (photo, readouts, no controls), a "steer" band in the lower-middle (the thumb arc: active control), and a "park" band at the very bottom (tab/mode switching, away from gesture zones). Keep values readable on the photo, and make the active control an eyes-free, relative, big-target one.

### Cited Findings
(Each rule rests on findings above; see sections.)
- Photo in the upper region, controls below: matches Samsung viewing/interaction split — [Samsung](https://developer.samsung.com/one-ui/layout/basic.html); Android supporting pane/bottom sheet — [Android canonical layouts](https://developer.android.com/develop/adaptive-apps/guides/canonical-layouts).
- Targets: 44 pt minimum (Apple); Hoober 7 mm centre / 12 mm corner; Parhi et al. 9.2-9.6 mm for one-handed thumb on small screens. — [Apple HIG](https://developer.apple.com/design/human-interface-guidelines/layout); [Hoober 2017](https://www.uxmatters.com/mt/archives/2017/03/design-for-fingers-touch-and-people-part-1.php); [Parhi 2006](https://www.telerik.com/docs/default-source/blogs-docs/parhi-mobilehci06-pdf)
- Place controls higher on larger phones — [Le et al. 2018](https://www.medien.ifi.lmu.de/pubdb/publications/pub/le2018fingersrange/le2018fingersrange.pdf).

### Inferences
Proposed rules (design judgments, not tested findings):
1. Photo goes in the top 55-60% of the screen and is never a touch target for fine value changes (avoid occluding the area being judged); allow tap/long-press for before/after and pinch for zoom only.
2. The active control (one parameter at a time) lives in a band roughly 20-45% up from the bottom edge, which is where the thumb arc has most reach on a one-handed right or left grip, per Le/Mayer shifts. Verify on-device with the owner's actual phones.
3. Replace "scroll through a long list of sliders" with a two-level pattern: a compact horizontal strip of parameter chips (swipe sideways, thumb-friendly), then one large scrub control for the selected parameter. This keeps the thumb in the arc and removes the vertical long-distance travel.
4. Controls should be relative: drag anywhere on the strip changes the value from where it is, so no precise handle acquisition (eyes-free principle, Section 3). Add detents and haptic ticks at 0, notable values and ends (phone vibration can emulate the haptic landmark idea; the studies used richer haptics).
5. Echo the value on the photo area (a chip near the top or at the edge of the photo, not under the thumb) so the eyes never leave the photo.
6. Hit targets at least 44 pt (about 7-9 mm); put frequent targets centre-weighted and dangerous ones (reset, delete) away from frequent ones.
7. Keep a bottom inset for the home gesture and side insets for Android back; do not put drag handles within the gesture zone, or use exclusion rects only for small handles.
8. Make layout mirror-able and handedness-agnostic (Hoober: about 1/3 of one-handed thumbs are left).
9. Provide a half-height bottom sheet with a drag handle placed in the lower-middle, with collapsed state showing parameter chips; the photo can resize to remain unobscured above the sheet rather than being covered by it.
10. Landscape: optional two-thumb layout with chip strips on both short edges and the photo in the centre; do not assume it.

### Gaps
- Rule thresholds (55-60% photo, 20-45% band) are my estimates; no source provides them. They should be A/B tested on the owner's devices, including the OPPO Find N5 outer screen.
- The owner's perceived "eye and finger not unified" problem was not studied; the most plausible mechanisms (travel distance, gaze switching, occlusion) are drawn from the above but none is directly demonstrated for photo editors.
