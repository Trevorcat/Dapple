# Foldable / small-tablet grips, postures and adaptive layout for a photo editor (Dapple)

Research date: 2026-11-12. Evidence quality varies; every claim carries its source, and unverified items are in Gaps. Several primary sources (Huawei, OPPO, ACM full texts) could not be retrieved.

## 1. How people hold tablets and foldables, and where thumbs can work

### Takeaway
Tablet-class grips are mostly two-handed side grips (thumbs on the edges) or one-hand-supports / other-hand-taps. Thumb-reachable area is the side edges and the upper-middle region, not the screen centre, and people re-grip often. The bimanual literature (BiTouch/BiPad) says the supporting hand's thumb is a legitimate second input channel on edge zones. Foldable-specific grip studies are almost absent in what I could verify.

### Cited Findings
- Odell and Chandrasekaran (HFES Annual Meeting 2012, pp. 1907-1911, DOI 10.1177/1071181312561278) start from the premise that holding a tablet in two hands and using the thumbs is a common method. They built a method for placing interface elements into easily reachable areas, used in developing Windows 8. — [Bohrium abstract page](https://www.bohrium.com/paper-details/enabling-comfortable-thumb-interaction-in-tablet-computers-a-windows-8-case-study/813271262854905856-19959) (abstract/snippets only; I did not read the full paper)
- A later review reports from that work that the centre of the tablet was unreachable, movements needing thumb flexion are uncomfortable, and the thumb covered a larger area with a side grip than a corner grip. — same search result set as above (secondary summary; verify against the paper)
- Microsoft's Windows 8 team published a thumb reach heat map (green very comfortable, yellow reachable, red uncomfortable) when designing the split thumb keyboard. — [Microsoft Windows 8 blog, "Designing the Windows 8 touch keyboard" (2012)](https://learn.microsoft.com/en-us/archive/blogs/b8/designing-the-windows-8-touch-keyboard)
- Wagner, Huot, Mackay, "BiTouch and BiPad", CHI 2012, pp. 2317-2326: a preliminary study found five grips in which one hand supports the tablet while the other interacts. People change position frequently to combat fatigue. BiPad lets the supporting hand's thumb/fingers interact via zones on the tablet edges. In the controlled study, bimanual taps beat the one-handed control in both landscape and portrait. Chords and gestures helped only in portrait. Thumbs were faster than fingers but more tiring and less stable. — [HAL record](https://hal.archives-ouvertes.fr/hal-00663972); [ACM DOI 10.1145/2207676.2208391](https://unpaywall.org/10.1145%2F2207676.2208391)
- Hoober and Shank tablet observation (as summarized by Josh Clark, A List Apart): 88% of tablet use is seated (19% for phones). Large tablets are put down in nearly 2 of 3 sessions. Lying on a surface/lap is 40% of large-tablet use, upright stand 22%. 7-8 inch tablets are handheld 69% of the time. Portrait is favoured about 60:40. Thumbs grip the sides and usually settle in the middle-to-top third of the screen. Side edges and top corners are most reachable. Top and bottom edges are hard to reach, bottom hardest (it can be hidden when the tablet lies flat). Reaching into the middle takes arm effort, so group frequent controls within thumb reach. — [A List Apart, "How We Hold Our Gadgets" (Clark, 2013-2014)](https://alistapart.com/article/how-we-hold-our-gadgets/)
- Hoober's phone study: users change grip very often, sometimes every few seconds. — [UXmatters, "How Do Users Really Hold Mobile Devices?" (2013)](https://www.uxmatters.com/mt/archives/2013/02/how-do-users-really-hold-mobile-devices.php)
- MobileHCI 2016 (video observation): each phone type affords specific grips; users shift grip, tilt and rotation depending on interaction context. Not foldable-specific. — [Cardiff Met record, "Understanding grip shifts"](https://pure.cardiffmet.ac.uk/en/publications/understanding-grip-shifts-how-form-factors-impact-hand-movements-/)
- Foldable one-handed claim: a newsletter says a CHI 2026 paper (DOI 10.1145/3772318.3790528) found that using an open foldable one-handed with content on both halves forces users to change grip to reach each half. The newsletter does not name the title or authors. — [Access*Ability newsletter (2026)](https://buttondown.com/access-ability/archive/foldable-phones-and-accessibility-benefits) (secondary; unverified)
- Samsung/Google material treats the unfolded device as tablet-like for layout and offers no grip-zone numbers (see section 2).

### Inferences
- Theory to explain to the owner: two coupled constraints. (a) Biomechanics: the thumb is a pivoting limb anchored at the grip, so reach is an arc around the base of the thumb, which sits at the side edges; the arc is comfortable when extended/abducted and uncomfortable when flexed deep into the palm (Odell's "flexion is uncomfortable"). (b) Support vs interact: any finger holding the device is not free to act, so the controls should sit where the gripping thumb already rests, not where a free hand would reach.
- For a ~229 g (Find N5 weight, from the brief) device with a nearly square 8.12" panel, a landscape two-handed side grip is the most plausible active-editing posture. Therefore each thumb's arc covers a vertical strip on its own side edge, upper-middle to lower-middle height, and the screen centre is the "unreachable" zone. The centre is the best place for the photo (look, don't touch), and edges are where controls belong. This matches the "split controls to left and right edges" idea but is my inference, not a tested result.
- The photo itself needs touch (pinch/pan/zoom), which is fine with a free index/thumb because the supporting hand tolerates short interactions (BiTouch). Long slider drags are the tiring ones; thumbs were "more tiring and less stable" than fingers.
- A grip-shift every few seconds means do not design for one fixed grip; allow controls to be reachable from at least two grips, or let the user move the panel to either side (Pixelmator Photo allows this, section 3).

### Gaps
- No peer-reviewed thumb-reach map for an 8" near-square foldable inner screen was found. The CHI 2026 foldable paper is unverified.
- Odell and Chandrasekaran full text and numeric zone sizes were not accessed.
- No hard data on grip distribution for the Find N5 inner screen specifically.

## 2. Official adaptive-layout guidance (Android/Material, Samsung, OPPO, Huawei, Apple)

### Takeaway
All platforms say: decide layout from the window, not the device; use size-class breakpoints and canonical pane layouts; preserve state across fold/unfold; in half-folded postures put content on the upper half and controls on the lower half; and keep interactive elements away from the crease. Apple's HIG says the same in different words (resizable windows, split views, hideable panes). OPPO and Huawei official design/dev docs could not be retrieved.

### Cited Findings
- Window size classes (Android, current docs): compact width <600dp; medium 600-<840dp; expanded 840-<1200dp; large 1200-<1600dp; extra-large >=1600dp. Height: compact <480dp, medium 480-<900dp, expanded >=900dp. Docs note most large unfolded inner displays in landscape are at least expanded width, and in portrait are medium. Classes derive from window size, not device ("not intended for isTablet logic") and can change at runtime through orientation, multi-window and folding. — [Use window size classes](https://developer.android.com/develop/ui/compose/layouts/adaptive/use-window-size-classes)
- Same page: also consider the height class for phones or open flippables in landscape where width is medium but height is compact, "two pane layouts are not practical". Start by optimizing for expanded width, then decide medium. — [Use window size classes](https://developer.android.com/develop/ui/compose/layouts/adaptive/use-window-size-classes)
- Do not use physical hardware values for layout decisions; use window metrics from Jetpack WindowManager; hoist state so it survives layout switches (fold/unfold, resize, rotation). Always give composables all data, even if a pane is hidden at some sizes. — [Support different display sizes](https://developer.android.com/develop/ui/compose/layouts/adaptive/support-different-display-sizes)
- Android 16 (API 36): for apps targeting it, the system ignores orientation, aspect-ratio and resizability restrictions on devices with smallest width >= 600dp. — same page as above
- Canonical layouts: list-detail, supporting pane, feed. Supporting pane places a secondary pane left or right of the main pane in expanded/large windows; on compact/medium it defaults to push navigation. `SupportingPaneScaffold` implements it; Compose Material 3 Adaptive reached 1.0. — [Android canonical layouts (Compose)](https://developer.android.com/develop/ui/compose/layouts/adaptive/canonical-layouts) via [search summary](https://www.infoq.com/news/2024/09/compose-material-3-adaptive)
- Postures: tabletop is HALF_OPENED with a HORIZONTAL fold; book posture is HALF_OPENED with a VERTICAL fold. Android's camera codelab moves the viewfinder above the fold for tabletop, to the right of the fold for vertical half-open, and restores for FLAT. — [Android camera foldables codelab](https://developer.android.com/codelabs/android-camera-foldables); [Foldable postures and states](https://developer.android.com/guide/topics/large-screens/foldable-postures-and-states)
- Microsoft's doc on Jetpack WindowManager: on non-dual-screen foldables the FoldingFeature appears only when HALF_OPENED. — [Microsoft Learn: WindowManager](https://learn.microsoft.com/dual-screen/android/jetpack/window-manager)
- Samsung One UI Flex mode: content on the top (angled up), controls and buttons on the bottom (lies flat); "Avoid positioning interactive elements near the crease"; show a visual transition when entering/leaving Flex mode; apps should adjust automatically when partially folded; some screens need no change. — [Samsung Developer: Flex mode](https://developer.samsung.com/one-ui/foldable-and-largescreen/foldable-excl-flex.html)
- Samsung secondary points (via search summary, not verified on the page): keep ratio/shape of key items (e.g. camera preview) between normal and Flex mode; typically support landscape Flex only; pop-ups/toasts may need repositioning. — [Samsung Developer: designing for foldable](https://developer.samsung.com/one-ui/largescreen-and-foldable/designing_for_foldable.html)
- App continuity: restore user state on configuration change; keep UI state in a ViewModel so fold/unfold is smooth. — [Samsung/Google summary via search](https://developer.samsung.com/sdp/blog/en/2021/09/14/adapt-your-app-to-foldable-devices-for-an-optimal-user-experience)
- Google's current adaptive app quality guidelines (replace the large-screen guidelines, April 2026 archive) define Tier 3 adaptive ready, Tier 2 adaptive optimized, Tier 1 adaptive differentiated (foldable postures, multitasking, drag and drop, stylus). Recommended test device: Foldable 841x701 dp (similar to a near-square inner screen). — [Adaptive app quality](https://developer.android.com/docs/quality-guidelines/large-screen-app-quality)
- Apple HIG Split views: iPadOS windows are fluidly resizable, so design split layouts at narrow, compact and intermediate widths. A split view can have sidebar/canvas/inspector. "Consider letting people hide a pane" in an editing context, "provide multiple ways to reveal hidden panes". Prefer split views in regular, not compact, environments. (Updated June 2025.) — [Apple HIG: Split views](https://developer.apple.com/design/human-interface-guidelines/split-views.md)
- WWDC25 session 208 notes: sidebar becomes tab bar in portrait/smaller windows; make size-driven layout changes "non-destructive"; extend content behind sidebars with scroll edge effect. — [WWDCNotes on session 208](https://wwdcnotes.com/documentation/wwdc25-208-elevate-the-design-of-your-ipad-app/)
- Huawei: community articles mention HarmonyOS `FoldSplitContainer` (API 12+) for expanded/hover/folded states, and a "smart grip" API (needs `ohos.permission.DETECT_GESTURE`) that detects left hand, right hand, both hands, or none, to move floating buttons. — [51CTO article](https://ost.51cto.com/posts/35699); [CSDN guide](https://harmonyosdev.csdn.net/693c02be0800f3458b81c949.html) (third-party, not Huawei design docs)
- OPPO: only press coverage found; one article says apps often just enlarge icons/text on unfolded screens and show less content, and says Find N's inner-screen ratio leaves letterboxing for 16:9 video while 4:3 content fits well. — [IT之家](https://www.ithome.com/0/927/085.htm); [ifanr](https://www.ifanr.com/1461412)

### Inferences
- Window-size-class mapping for Dapple: Find N5 inner (about 8.12", 2480x2248) is roughly 840-950 x 760-860 dp (my estimate from density; Google's own foldable test size is 841x701 dp). That is expanded width but medium height, nearly square. A "expanded width = two panes" rule works but must not assume landscape-like height; use aspect ratio of the window as a secondary input for how wide the inspector may be.
- Android 16 ignores orientation locks on >=600dp; Dapple cannot force landscape on Android. The "primarily landscape" intent for iPhone Duo is a content decision, not a lock: design for both orientations and bias the default panel placement.
- A supporting-pane scaffold fits the editor exactly: photo = main pane, adjustments = supporting pane. On the hinge: avoid putting a vertical hinge through the photo (book posture), and in tabletop put the photo above the horizontal fold.
- Samsung's one rule ("view top, control bottom") equals the standard HCI principle that the surface the hand rests on is the input surface, and the angled surface is the display.
- For continuity cover to inner: the guidance is state hoisting (ViewModel). A photo editor should keep edit stack, selected tool, zoom/pan expressed in image coordinates (not pixels), so a new window size just re-lays out; this is my application of the hoisting principle.

### Gaps
- OPPO Open Platform foldable/large-screen guide (open.oppomobile.com): not found/retrievable. No official OPPO hold-zone guidance verified.
- Huawei official 折叠屏设计指南 "手持热区": the developer.huawei.com page returned 404; no official hot-zone numbers found. Needs a direct manual lookup.
- Apple HIG Inspectors and Layout pages returned 404/no text; the iPadOS inspector wording is unverified. "Apple official iPhone Duo 1878x2670 @430ppi" comes from the owner's brief; I have not verified it, and it is not a shipped product I can confirm.
- Samsung pages I reached are sparse; the continuity/ViewModel text is from a 2021 blog and a search summary.

## 3. Layout patterns for maximal image with thumb-reachable controls

### Takeaway
Professional iPad editors keep the image dominant and place controls in a right-side (movable) panel or in a responsive layout that changes with size. Documented evidence is thin; there are no published comparative studies. The grip research supports edge-anchored controls and discourages centre- or edge-top/bottom-anchored controls.

### Cited Findings
- Pixelmator Photo on iPad: editing tools open in a panel that defaults to the right side of the screen, can be dragged to the left or off-screen; histogram at top, then adjustments. — [MacStories review](https://www.macstories.net/reviews/pixelmator-photo-the-macstories-review/); [AppleInsider hands-on (2019)](https://appleinsider.com/articles/19/04/09/hands-on-pixelmator-photo-is-king-of-the-ipad-photo-editing-apps)
- Darkroom iPad uses a responsive interface with three layouts (Portrait, Portrait-Wide, Landscape), picked per device and size. — [iClarified (2018)](https://www.iclarified.com/68815/darkroom-image-editor-launches-for-ipad)
- Lightroom mobile: Edit panel is accordion-based (e.g. "Light"); I found no description of its landscape side-panel geometry. — [Adobe Help](https://helpx.adobe.com/africa/lightroom-cc/using/edit-photos-mobile-ios.html)
- Hoober/Shank: tablets are often put down (40% flat on a surface for large tablets) and the bottom edge can be hidden when lying flat. — [A List Apart](https://alistapart.com/article/how-we-hold-our-gadgets/)
- Procreate sidebar, Photomator and Lightroom iPad specifics: no reliable source retrieved.

### Inferences
- Pattern comparison for the owner (HCI reasoning, not tested):
  - Fixed right inspector (current Dapple): good for right-handers' support hand? No: the right thumb reaches its own edge strip, but the inspector's inner edge (near the photo) is mid-screen, which Odell calls hard to reach. Controls near the inspector's far (outer) edge are reachable; those near the photo edge need arm travel.
  - Split left/right edge strips: shortest thumb travel; use the left for global/frequent (tool tabs, undo, compare) and the right for the active-tool slider. Cost: two attention zones and eye travel photo-to-edge; mitigate by keeping the value readout on or next to the photo.
  - Movable panel (Pixelmator pattern): addresses left-/right-handedness and the "supporting hand vs interacting hand" swap; BiTouch shows grips differ among users.
  - Collapsible chrome / full-screen photo with edge-swipe controls: maximises image, but hidden affordances hurt discoverability (Apple's advice: multiple ways to reveal hidden panes).
  - Floating palette: flexible but can occlude the photo and gets in the grip zone; low evidence.
  - Bottom inspector: bottom edge is hardest to reach when the device is lying down or on a stand, and the hand covers it when holding from below; on a 1.1:1 screen it also eats height from an already short window. Likely the weakest option in landscape.
- Near-square (Find N5): square windows have no "long axis", so the photo-vs-panel split is the only structure; a landscape-photo shown in a ~0.6 width pane still has a 3:2 shape with letterbox bands above and below that could hold readouts or a histogram. Portrait photos benefit most from a side panel. Choose panel width as a fraction (for example ~30-35%), not a fixed dp, then compare with Google's 841x701 dp test size.
- Eye-hand coupling theory: Fitts's law and visual-motor separation. Dragging a slider at an edge while looking at the photo mid-screen forces saccades; keep (a) live preview on the image, (b) numeric readout at the thumb, and (c) the touched slider thickened while active.

### Gaps
- No study comparing side-panel vs bottom-panel in photo editors on foldables.
- No retrievable Procreate/Photomator/Lightroom layout documentation; screenshots would need a manual check.

## 4. Tabletop / Flex posture for the photo editor

### Takeaway
Viable but narrow. Officially supported by both Google and Samsung (content on top, controls on bottom, hinge not crossing the content), but it works only in a horizontal-hinge half-open posture, which is mostly a media/camera/video-call use case. For an editor, it makes sense as an optional layout, not the primary design.

### Cited Findings
- Android: tabletop = half-open + horizontal fold; viewfinder above the fold, controls below. — [Camera foldables codelab](https://developer.android.com/codelabs/android-camera-foldables)
- Samsung: "Show content on the top and provide controls on the bottom"; Flex mode normally supports landscape only; visual transition recommended. — [Samsung Flex mode](https://developer.samsung.com/one-ui/foldable-and-largescreen/foldable-excl-flex.html)
- Android guidance says foldable postures count under Tier 1 (differentiated) rather than required baseline. — [Adaptive app quality](https://developer.android.com/docs/quality-guidelines/large-screen-app-quality)
- Microsoft docs: FoldingFeature only appears when HALF_OPENED on single-screen foldables. — [Microsoft Learn](https://learn.microsoft.com/dual-screen/android/jetpack/window-manager)

### Inferences
- Geometry: in tabletop the top half of an 8" near-square screen is about 8.1" x ~4" (wide, short), which suits a landscape photo and fits a RAW editor's need to see the image at an angle on a desk. The bottom half becomes a flat control surface that does not cover the image with the hand (reducing the "finger occlusion" problem). Sliders lie in a wide strip, which is good for fine horizontal drags, and the surface is stable (device rests on the table), so precision tends to be better than when held in the air; this is a reasoned prediction, not evidence.
- The Hoober note (hard-to-reach bottom edge when lying flat) is opposite in tabletop: the bottom half is now the flat reachable surface.
- Cost: with the photo confined to half the screen, pixel-peeping is limited; zoom controls must live in the bottom half.
- No published study of tabletop editing sessions; worth a quick on-device prototype with the Android emulator's foldable posture controls.

### Gaps
- No usage statistics on how often people use tabletop/Flex mode for editing.
- OPPO's Find N5 supports a hover/flex-style half-open mode, but I found no official OPPO guidance.

## 5. Continuity between cover and inner screens

### Takeaway
Treat fold/unfold as a window resize plus configuration change: hoist all state, do not store layout decisions in view state, and keep the edit session identical. Samsung and Google both call this app continuity.

### Cited Findings
- Samsung: app continuity = seamlessly restoring user state when a configuration change arrives; retain UI state with a ViewModel to make fold/unfold smooth. — [Samsung Developers blog (2021)](https://developer.samsung.com/sdp/blog/en/2021/09/14/adapt-your-app-to-foldable-devices-for-an-optimal-user-experience)
- Android: hoist state so it is preserved when layout changes between single-pane and multi-pane; window size class can change during the app's lifetime from folding. — [Support different display sizes](https://developer.android.com/develop/ui/compose/layouts/adaptive/support-different-display-sizes); [Window size classes](https://developer.android.com/develop/ui/compose/layouts/adaptive/use-window-size-classes)
- Apple: make size-driven layout changes "non-destructive". — [WWDCNotes session 208](https://wwdcnotes.com/documentation/wwdc25-208-elevate-the-design-of-your-ipad-app/)
- Samsung recommends a visual transition effect on mode change. — [Samsung Flex mode](https://developer.samsung.com/one-ui/foldable-and-largescreen/foldable-excl-flex.html)

### Inferences
- For Dapple: the cover-screen layout (compact, portrait, single pane with the inspector as a bottom/sheet) should map each control onto the same ViewModel the inner screen uses; folding open should switch from a sheet to the persistent side pane with the same selected tool and slider values. A shared-element transition for the photo helps the eye keep its anchor (HCI: object constancy).
- Keep zoom/pan in image-normalized coordinates and re-fit only when the user had not zoomed.

### Gaps
- No OPPO-specific continuity API details verified (ColorOS "app continuity" behaviours on cover-to-inner transitions).
