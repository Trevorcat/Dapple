# Reference app patterns for parameter adjustment and editor layout (as of Nov 2026)

Evidence quality note: web search/fetch coverage was uneven. Apple Photos edit dial/iOS 26 details, 醒图/美图, OPPO, Instagram, Samsung editor layout, iPad/foldable layouts, and Halide/Kino editing-dial specifics could NOT be confirmed from retrievable sources; they are listed under Gaps rather than asserted.

## (a) How do apps let users choose WHICH parameter to adjust?

### Takeaway
Three families exist: (1) gesture-selected (Snapseed: vertical swipe on the image, no UI chrome), (2) a horizontal strip/list of tool icons with one active control (Photos, Lightroom, Google Photos), and (3) task-grouped bottom toolbars with fewer visible tools (Lightroom 2023 redesign). Reviewers praise Snapseed's discoverability-without-clutter; Lightroom's and Google's moves toward grouped/larger controls drew mixed reactions.

### Cited Findings
- Snapseed: swipe up/down anywhere on the image brings up a temporary widget; slide to an option; it disappears on release. Left/right swipes adjust gradual tools (contrast, brightness, colour cast). Reviewer Michael E. Cohen (TidBITS, 6 Jan 2012) called these "clever variations and expansions of the basic iOS gestural interface" with "easy discoverability without imposing clutter". A help screen appears when opening a tool group and vanishes on first touch. — [TidBITS 2012](https://tidbits.com/2012/01/06/snapseed-displays-innovative-touch-techniques/)
- Snapseed 2.13 (announced 30 Nov 2016): a new Adjust icon on the bottom bar overlays settings; the chosen parameter is shown as a slider at the top of the app, while adjustment is still left/right swipe anywhere on screen and parameter choice is still up/down swipe. — [9to5Google](https://9to5google.com/?p=158930)
- Snapseed Filter/Details flow per Google's help page (via search snippet): tap a style, swipe vertically to pick Filter Strength or Saturation, then swipe horizontally. — [Google Snapseed help](https://pss.support.google.com/snapseed/answer/6158220)
- Snapseed control points (Selective): place a point, pinch to change its radius, red overlay shows affected area; hold to copy/paste — [TidBITS 2012](https://tidbits.com/2012/01/06/snapseed-displays-innovative-touch-techniques/)
- Lightroom mobile new editor (early access announced 26 Jul 2023): on-screen options cut from 15 to five by grouping and a 3-dot overflow; expandable toolbar; tools ordered left-to-right by typical workflow; rationale: scrolling through many options overwhelmed beginners, eases one-handed use. Community quotes: "minimalist, modern, simple at first glance and less intimidating." — [Adobe blog](https://blog.adobe.com/en/publish/2023/07/26/adobe-photoshop-lightroom-has-new-mobile-interface-make-editing-even-easier-on-go)
- Lightroom 2023 bottom bar: Presets, Crop, Sliders, Masking, Healing; sliders sit under their own button; "a lot less scrolling along the edit toolbar". — [Lightroom Queen](https://www.lightroomqueen.com/whats-new-lightroom-2023-062/)
- Google Photos Android redesign (Sept 2025, v7.44): bottom carousel with search and full tool list; tools grouped Auto, Actions, Markup, Filters, Lighting, Color; slider is pill-shaped with open/close animation. — [9to5Google 2025-09-05](https://9to5google.com/2025/09/05/google-photos-editor-redesign-android/)
- Google Photos user complaints (Pixel subreddit, reported by Android Central): the update forces users to "tap in and out" of sliders to move between them, and enlarged strip needs more swipes. — [Android Central](https://www.androidcentral.com/apps-software/users-dreading-google-photos-less-functional-editor-updates-report)
- Google Photos web (2023) went the other way: Adjust shows all sliders at once rather than grouped categories. — [9to5Google 2023-08-21](https://9to5google.com/2023/08/21/google-photos-web-editor-redesign/)
- Apple Photos iOS 18: Edit button replaced by a three-slider icon; X exits editing; Apple "hasn't changed the actual editing tools". iOS 17 moved Cancel/Done to the top and added text labels to tool icons. — [MacRumors iOS 18 guide](https://macrumors.com/guide/ios-18-photos), [MacRumors iOS 17](https://www.macrumors.com/2023/06/21/8-new-photo-features-ios-17/)
- Halide: Auto by default with minimal UI; one tap to manual; exposure = vertical swipe anywhere, focus = horizontal swipe on a dial; left-edge swipe opens library, right-edge swipe opens manual controls (this mapping is from a third-party design guide, not Lux). Quote: "Hide complexity, don't remove it." — [Blake Crosley guide](https://blakecrosley.com/guides/design/halide)
- Lux's own wording: gestures "become muscle-memory over time — like dials on a camera"; "keeps everything important within reach". — [Introducing Halide](https://www.lux.camera/introducing-halide/)
- Procreate: gesture set on canvas (two-finger tap undo, three-finger tap redo, three-finger swipe down for copy/paste, four-finger tap full screen, pinch/twist), fully remappable in Gesture Controls with conflict warnings. — [Procreate Handbook](https://help.procreate.com/procreate/handbook/interface-gestures/gestures)
- Procreate sidebar sliders: sliders on the canvas edge; drag away from the slider horizontally and the increment gets finer ("farther your finger moves from the slider, the smaller the increments"). — [Procreate Handbook](https://help.procreate.com/procreate/handbook/interface-gestures/gestures)

### Inferences
- Snapseed's gesture-chosen parameter plus anywhere-on-image adjustment is the one pattern here that frees the photo from covering controls and keeps eye on image; its cost is discoverability (hence its help overlays).
- Evidence across Lightroom/Google suggests that reducing visible choices (grouping) is the mainstream direction, but Google shows grouping that adds taps per parameter hurts power users; a design with many parameters should keep switching to one gesture.
- Procreate's "drag away for finer increments" is a transferable precision gesture with no extra UI.

### Gaps
- Exact Apple Photos iOS 18/26 dial behaviour (ticks, haptic at zero) not confirmed by any fetched source.
- Snapseed 3.0 redesign (2025): I found no source confirming it exists.

## (b) How is the value changed (slider vs ruler/dial), and who abandoned or kept sliders?

### Takeaway
Evidence retrieved shows Snapseed (invisible horizontal swipe), Halide (gesture dials) and Procreate (edge slider with fine-scrub) deliberately avoid a conventional drag-the-thumb slider; Darkroom, Pixelmator/Photomator and Lightroom keep sliders but add numeric entry, reset and haptics. I did not find a documented reason for any app "abandoning sliders for rulers"; the Apple/Google ruler claims are unverified.

### Cited Findings
- Darkroom: sliders retained; value labels added so users can see exact values and reproduce/share looks. Haptics fire "when you hit the minimum and maximum values of the sliders", on tool switching and showing/hiding tools; goal "to provide weight and tactility to the app." Darkroom 7 (Dec 2025) says it rebuilt sliders around a new engine for speed; a 2019 MacStories review noted colour-tool slider stutter on RAW. — [Darkroom haptics blog 2017](https://darkroom.co/blog/2017-07-haptic-feedback), [Darkroom 7](https://darkroom.co/blog/2025-12-09-render-engine), [MacStories Darkroom 4](https://www.macstories.net/reviews/darkroom-4-0-the-macstories-review/)
- Pixelmator Photo (iPad): tap slider value opens a number pad for exact entry; double-tap the drag handle resets to zero ("a nice touch" per reviewer). — [MacStories review](https://www.macstories.net/reviews/pixelmator-photo-the-macstories-review/)
- Lightroom mobile: double-tap slider thumb resets to default (Adobe gesture page, via search; page itself returned 403). A forum post (Feb 2024) reports double-tap not resetting; a Samsung tablet S-Pen user reports it works only in portrait. These are single user reports. — [Adobe gestures](https://helpx.adobe.com/ee/lightroom/mobile/get-started/gesture-controls-in-lightroom-for-mobile.html), [Adobe community](https://community.adobe.com/questions-680/sliders-are-not-resetting-back-to-0-when-i-double-tap-on-the-screen-929608)
- Halide haptic mapping (third-party sketch, not official): light impact on exposure changes, medium when focus locks, heavy on shutter, selection feedback on mode switch; table maps film-camera click stops to haptics. — [Blake Crosley guide](https://blakecrosley.com/guides/design/halide)
- Kino: reviewer describes a "slightly curved dial" for focus. — [Digital Trends via search](https://www.digitaltrends.com/) (snippet only; verify)
- Google Photos Android: sliders are pill-shaped; no source found describing a ruler/dial. — [9to5Google 2025](https://9to5google.com/2025/09/05/google-photos-editor-redesign-android/)
- Apple Photos: Auto lights up a horizontal dial under the tool set whose intensity can be moved by swipe; return it "to the white dot" to get back to the auto level. — [MacRumors how-to](https://www.macrumors.com/how-to/how-to-use-the-editing-tools-in-photos/) (via search snippet). Filters show an intensity slider under the strip. — [Technobezz](https://www.technobezz.com/how-to-edit-photos-on-iphone)
- Photographic Styles (iPhone 16/17): control pad swiped in all directions; vertical = Tone, horizontal = Color, slider under the grid = Palette intensity; settings can be shared as numbers (e.g. tone -11, colour +15); a circular-arrow button resets (iPhone 16 guide; not confirmed for 17); editable after capture in Photos > Edit > Styles. — [iGeeksBlog](https://www.igeeksblog.com/how-to-use-photographic-styles-on-iphone-16/), [MacRumors](https://macrumors.com/how-to/iphone-16-edit-photographic-styles-after-shooting)
- Slider precision principle (Chinese PM article): the larger the range and denser the scale, the harder it is to hit an exact value. — [woshipm](https://www.woshipm.com/evaluating/4438372.html) (generic, not about a photo app)

### Inferences
- Pattern for precision with fingers: three distinct tools appear: numeric entry (Pixelmator), drag-away fine scrub (Procreate), and value labels (Darkroom). Dapple could combine them with a 2D pad's numeric readout (as Photographic Styles shares tone/colour numbers).
- Haptics in this set sit at limits/detents and mode changes, not continuous; Darkroom is the only first-party documented case.

### Gaps
- Whether Apple Photos adjustment scrolling uses ticks plus a zero haptic: not confirmed (searches returned only tutorials).
- Any published rationale for sliders to dials from Apple WWDC or Google: none found.
- Snapseed "3.0" and Google "ruler" claims in the brief: unconfirmed.
- 醒图 / 美图秀秀 / OPPO ColorOS / Instagram control shapes: search returned no gesture-level sources; only a generic competitor-analysis article exists ([woshipm](https://www.woshipm.com/evaluating/4438372.html)). Needs hands-on capture.

## (c) Strength, precision, reset, compare patterns

### Takeaway
A single strength value is handled as a second-level control tied to the item (tap a selected tile again in VSCO; Amount slider under the selected preset in Lightroom; slider under the strip in Photos; dial under Auto; Palette slider under the pad in Styles). Reset is usually double-tap on the control; compare is typically press-and-hold on the image (not verified in fetched sources).

### Cited Findings
- VSCO Pro Presets: tap a tile to apply, tap again to open strength, contrast, colour, tone sliders; older guide says default intensity at far right (+12). — [VSCO](https://www.vsco.co/features/photo-filters/pro-presets), [Gadget Hacks (older)](https://smartphones.gadgethacks.com/how-to/vsco-101-use-presets-filters-your-photos-0180607)
- Lightroom adaptive presets: select the thumbnail to reveal the Amount slider; adaptive presets use AI to find areas of the scene. — [Adobe Learn](https://www.adobe.com/uk/learn/lightroom-cc/web/adaptive-landscape-presets-lightroom-mobile)
- Lightroom preset Amount: bug reports of amount resetting to 100 and slider disappearing; desktop users asked for an option to hide it to save screen space. — [Adobe community](https://community.adobe.com/questions-680/preset-amount-resets-to-100-1644693), [feature request](https://community.adobe.com/feature-requests-676/p-please-give-us-an-option-to-turn-of-the-new-preset-amount-slider-to-conserve-screen-real-estate-665154)
- Pixelmator Photo ML Enhance: one tap applies auto adjustments; reviewer: imperfect but often enough, or a starting point for manual tweaks. Photomator support (via search) mentions an intensity slider for colour adjustments and double-tap reset. — [MacStories](https://www.macstories.net/reviews/pixelmator-photo-the-macstories-review/)
- Snapseed filter strength is parameter #1 selected by vertical swipe, adjusted horizontally. — [Google help](https://pss.support.google.com/snapseed/answer/6158220)
- Procreate: two-finger tap undo (hold to step back up to 250 actions), three-finger tap redo. — [Procreate Handbook](https://help.procreate.com/procreate/handbook/interface-gestures/gestures)
- Photos iOS 26 yellow indicator on exposure line can be misleading (Apple Community thread, unresolved). — [Apple Discussions](https://discussions.apple.com/thread/256182797)

### Inferences
- Auto-as-baseline with an Apple-style "dial under Auto" is the closest precedent to Dapple's auto-by-default + strength; the "return to the white dot" detail suggests a visible home marker is how Apple signals auto level.
- Undo via two-finger tap (Procreate) is canvas-level, which keeps the controls free of reset buttons.

### Gaps
- Compare gesture (press-and-hold for original) in Photos, Lightroom, Snapseed: not verified by a retrieved page.
- Whether Photos tap-to-reset on a slider/dial exists: unconfirmed.

## (d) Layout on phone portrait, landscape, tall phones, tablets/foldables

### Takeaway
Consistent pattern: photo on top, one control strip at the bottom, with tool-bar above it. Tablets move controls to a side panel (Pixelmator right panel, Lightroom translucent right panel, Darkroom three layouts). Foldable-specific editor layouts: no source found.

### Cited Findings
- Darkroom iPad: three layouts (Portrait, Portrait-Wide, Landscape) chosen automatically; landscape adds a photo strip; narrow vertical toolbars left and right with editing tools/filters on the right. — [Darkroom iPad 4.0](https://darkroom.co/blog/2018-12-ipad-darkroom-40), [Darkroom interface](https://darkroom.co/help/guide/interface-overview), [MacStories](https://www.macstories.net/reviews/darkroom-4-0-the-macstories-review/)
- Pixelmator Photo: tools panel on the right by default, can be dragged to the left or off to the side out of sight; reviewer says photo editors are best on large iPad and criticises no quick-edit flow on iPhone. — [MacStories](https://www.macstories.net/reviews/pixelmator-photo-the-macstories-review/)
- Lightroom iPad (2017): buttons down the right side switch views; editing panel made translucent so the image does not shrink; users complained the new interface "reduces image size dramatically". — [Jeff Carlson 2017](https://jeffcarlson.com/2017/07/18/lightroom-mobile-adds-selective-brush-new-ipad-interface-and-more/), [Adobe community](https://community.adobe.com/feature-requests-681/p-image-too-small-when-editing-661252)
- Halide: designed for one-handed use on all iPhone sizes (third-party claim); edge swipes open library/controls. — [Blake Crosley](https://blakecrosley.com/guides/design/halide)
- Google Photos Android: crop/aspect controls above the preview, close/Save at the top, tool carousel at bottom. — [9to5Google](https://9to5google.com/2025/09/05/google-photos-editor-redesign-android/)
- Samsung Fold7: larger screen allows comparing AI edits with the original; no source for a two-pane editor layout. — [Samsung support](https://www.samsung.com/us/support/answer/ANS10006885/)
- Procreate: sliders on the canvas edge (sidebar) so they do not cover the canvas; remappable gestures. — [Procreate Handbook](https://help.procreate.com/procreate/handbook/interface-gestures/gestures)

### Inferences
- Reach: bottom-biased controls + edge placement is the convention; edge sidebars (Procreate) become left/right-handed on large devices, matching Pixelmator's movable panel.
- Translucent overlay (Lightroom 2017) is the documented fix for "panel shrinks the photo" complaints; the 2017 Lightroom comments are the clearest published user criticism of photo-vs-controls space.
- Landscape and foldable inner-screen layout for editing: infer side rail from iPad apps, not from foldable-specific evidence.

### Gaps
- Apple Photos iPad/iPadOS 26 and iOS 26 Liquid Glass editor layout: no source retrieved beyond a smartprix/Neowin overview not read in full.
- Lightroom iPad current (2025-26) layout, Samsung/OPPO/Xingtu/Meitu/Instagram layouts, Android foldables posture-aware editors: not found.
- Published designer rationale: only Darkroom (haptics post), Adobe (2023 blog), Lux (Halide intro). No Apple WWDC or Google design blog on Snapseed found. Sebastiaan de With's dial-specific essays (e.g. Halide Mark II "Behind the Design") surfaced in search but weren't fetched: [cur.at link](https://cur.at/IHKzSGc?m=web), [Spectre retrospective](https://lux.camera/a-spectre-retrospective-part-2-design/).
