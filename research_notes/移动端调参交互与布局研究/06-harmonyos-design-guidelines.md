# HarmonyOS design guidelines: interaction and foldable/large-screen design (for Dapple RAW editor)

Research date: 2026-11-12. Method: official developer.huawei.com design-guides / best-practices pages fetched via WebFetch (a small model summarised each page, so numbers are as reported by the fetch tool, not eyeballed from the page; treat as "verified from official page text" unless marked). Some official URLs returned HTTP 503 (e.g. foldable-0000001927080721, ux-guidelines-foldable-screen-0000001953111265, harmonyos-guides/responsive-layout-V5); gitee openharmony docs returned 405. Official pages carry no publication dates in the fetched text; dates below are "fetched 2026-11-12".

Scope note: I did not find official Huawei pages that cover several of the owner's questions (thumb-reach study numbers, 手持热区 zone diagrams with dimensions, vibration design spec, Slider/ruler design spec, Gallery editor interaction spec). These are listed under Gaps rather than guessed.

---

## 1. Design philosophy and motion (设计理念, 动效, 跟手, 抛滑)

### Takeaway
Huawei's motion language is explicitly physics-based ("引力动效体系": gravity animation system) and organised around three phases of a gesture: finger-following (跟手), release (离手, inherit velocity) and post-release (离手后, spring). Springs are preferred for anything the finger touches because they inherit velocity and are interruptible. Huawei gives concrete spring parameters and per-scope durations, but I found no official touch-to-response millisecond latency target beyond "touch-down feedback, ~100-130 ms press".

### Cited Findings
- Animation overview names a "引力动效体系" (gravity animation system) with three principles: 力的即视感 (visible sense of force: elements feel like objects), 力的秩序感 (sense of order: consistent rhythmic motion, esp. in transitions), 力的控制感 (sense of control: friction, gravity and repulsion are felt; speed and rhythm convey realistic physics). No numbers on that page. — [动效概述](https://developer.huawei.com/consumer/cn/doc/design-guides/design-animation-overview-0000001750985338)
- Gesture animation page defines three phases for continuous gestures (tap, swipe, flip, pinch, drag): 跟手 (object follows finger closely), 离手 (object inherits the finger's speed and direction at release; page calls this key for continuity), 离手后 (animation continues from inherited velocity with elastic physics curves). It advocates "无阻塞感的动效设计" (non-blocking animation) built on friction/elasticity/collision physics. — [手势动效](https://developer.huawei.com/consumer/cn/doc/design-guides/gesture-animation-0000001797038213)
- Tap: touch-down phase "about 100-130 ms"; feedback should start at touch-down, not wait for touch-up. This is the only explicit latency-like number on that page; no "跟手 latency ms" target is stated. — [手势动效](https://developer.huawei.com/consumer/cn/doc/design-guides/gesture-animation-0000001797038213)
- Damping / 显控比 (display-control ratio): past boundaries the object moves less than the finger. Examples: overscroll at top (content and title area damp differently, giving layered motion), horizontal card swipe (resistance begins after a set distance), pull-to-return (damping grows progressively). — [手势动效](https://developer.huawei.com/consumer/cn/doc/design-guides/gesture-animation-0000001797038213)
- Paging has clear limits: unsuccessful swipe stays on current page, successful shows next; overscroll drag can be a hint. — [手势动效](https://developer.huawei.com/consumer/cn/doc/design-guides/gesture-animation-0000001797038213)
- Animation attributes = duration, curve, frame rate. 60 fps frame = 16.67 ms. Duration guidance by motion extent: simple (color only) ~100 ms, complex (rotation) ~300 ms; small (switch icon) 150 ms, local (delete list item) 200 ms, full-screen (open image) 350 ms. — [动效属性](https://developer.huawei.com/consumer/cn/doc/design-guides/animation-attributes-0000001797117229)
- Bezier curves: standard cubic-bezier(0.40, 0.00, 0.20, 1.00) for objects that stay in view (image zoom, tab switch, switch); also (0.20,0,0.10,1) and (0.33,0,0.67,1). Decelerate cubic-bezier(0,0,0.40,1) for objects entering view (dialog appears); also (0,0,0.20,1), (0,0,0,1). Accelerate curve for objects leaving; no value given. — [动效属性](https://developer.huawei.com/consumer/cn/doc/design-guides/animation-attributes-0000001797117229)
- Spring curves: interpolatingSpring (stiffness, damping, mass, initial velocity), example 128 / 12 / 1 / 0. springMotion/responsiveSpringMotion (response, dampingFraction, overlapDuration), example response 0.555, damping ratio 0.53; the two families are convertible. Spring properties: interruptible with automatic duration (new motion inherits previous velocity), velocity inheritance (same params, different input velocity gives different shape, "more 跟手"), natural duration. Suited to list scroll, home-screen paging, swipe-up-to-exit. — [动效属性](https://developer.huawei.com/consumer/cn/doc/design-guides/animation-attributes-0000001797117229)
- Gesture animation types listed: 抛滑 Fling (fast/slow variants; may or may not inherit hand speed), LongPress, Drag, Click. — [动效属性](https://developer.huawei.com/consumer/cn/doc/design-guides/animation-attributes-0000001797117229)
- Frame rate: interactive animation recommended 60 fps (30 fps visibly janky). Page notes micro-animations (multiple oscillations on small elements) should be combined with human-factors research. — [动效属性](https://developer.huawei.com/consumer/cn/doc/design-guides/animation-attributes-0000001797117229)
- API defaults relevant to 跟手: curves.responsiveSpringMotion defaults response 0.15 s, damping 0.86, overlap 0.25 s ("designed for follow-hand animation"); springMotion defaults response ~0.55 s, damping 0.825. (From a search-result summary of the API reference, not directly fetched.) — [ohos.curves API](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/js-apis-curve)
- Concrete production spring values in the smart-grip page: component move-in 'interpolatingSpring' velocity 0, mass 1, stiffness 200, damping 17; off-screen move in/out stiffness 170, damping 17. — [智感握姿](https://developer.huawei.com/consumer/cn/doc/design-guides/smart-reachability-0000002556657823)
- UX standard summary (secondary, sspai, quoting Huawei app UX standards): transition durations 200/250/300 ms by screen size; long-press default 500 ms (adjustable 400-650 ms); double-tap interval 70-400 ms. — [少数派](https://sspai.com/post/112882)
- Design concept page lists the visual philosophy as themes (fresh colours, soft rounded shapes, light skeuomorphism, spatial depth, HarmonyOS Sans variable font, cross-device cards, unified architecture) with celestial metaphors (origin, balance, eclipse, floating, attraction); it does not use the terms 和谐美学/灵动/沉浸 in the fetched text. — [设计理念](https://developer.huawei.com/consumer/cn/design/concept)

### Inferences
- Teaching point for the owner: the 3-phase model (follow, release-with-velocity, spring settle) is the same underlying idea as Apple's "direct manipulation + velocity-preserving springs" (WWDC "Designing Fluid Interfaces") and Material 3 Expressive springs. Huawei's twist is exposing response/dampingFraction (time-based) and stiffness/damping (physics-based) as interchangeable parameterisations. For Dapple: use a spring with inherited velocity whenever a slider/dial is released with fling, and damp (rubber-band) at min/max rather than hard stop.
- The "100-130 ms touch-down, feedback at touch-down" figure is a justification for showing a pressed/active state immediately, not a processing latency target. Do not quote it as a Huawei 跟手 latency SLA.
- Spring defaults for 跟手 (0.15 s response) imply a very tight follow, i.e. the object trails the finger only slightly; useful as a starting point for a value bubble that follows a thumb.

### Gaps
- No official ms target for touch-to-pixel latency (跟手性) found. The "100 ms response" figure appeared only in unattributed community posts.
- No official 抛滑 deceleration/friction coefficient found on design pages (API-level friction not fetched).
- Terms 和谐美学, 灵动, 沉浸, 自然 as named principles were not found on fetched official pages; the owner's framing may come from HDC talks I could not access.

---

## 2. Interaction basics: gestures, hot zones, touch targets, anti-mistouch, edge gestures

### Takeaway
Official design pages define the gesture vocabulary and system edge gestures precisely, and give an ergonomic response to one-handed reach (智感握姿 / smart grip: sense which hand holds and operates, and move only high-frequency, hard-to-reach controls). I could not find an official zone diagram with dimensions (手持热区); Huawei approaches the problem dynamically rather than by static zone maps.

### Cited Findings
- Gesture list: tap (cancellable by sliding off before lift), long press (previews/menus/reorder/select; poor discoverability so avoid for frequent actions), drag, swipe, double tap (zoom images, play/pause), pinch, rotate, knuckle tap (screenshot), air gestures. — [手势](https://developer.huawei.com/consumer/cn/doc/design-guides/hmi-touchscreen-0000001928273206)
- System navigation gestures on phones/foldables are edge gestures: bottom swipe up = home; swipe up and hold = multitasking; bottom left/right swipe = switch app; upper-left swipe down = notification centre; upper-right swipe down = control centre; edge swipe inward = back. On large screens (pad/PC) they become 3-finger non-precise gestures. — [手势](https://developer.huawei.com/consumer/cn/doc/design-guides/hmi-touchscreen-0000001928273206)
- Anti-mistouch guidance: do not use system navigation gestures for non-system functions; in immersive scenarios (games) the back gesture may cause accidental exit, so use a toast on first edge swipe and exit on second, or an explicit exit button; "press" gestures give no chance to undo so use only for quick reversible actions. — [手势](https://developer.huawei.com/consumer/cn/doc/design-guides/hmi-touchscreen-0000001928273206)
- Interaction events page: touch drag needs long press first, then movement; any menu opened by touch long-press must also work via right-click and touchpad two-finger tap; a feature must not use both single and double tap for the same action; wheel scroll default 64 vp per notch; scrolling continues decelerating after lift-off based on release speed. — [交互事件归一](https://developer.huawei.com/consumer/cn/doc/design-guides/hmi-interaction-events-0000001795531217)
- Drag: touch drag starts if movement after selection exceeds 10 vp; haptic vibration when long press floats the object; object scales slightly to avoid occluding; failure shows forbidden badge and returns object. — [拖拽](https://developer.huawei.com/consumer/cn/doc/design-guides/hmi-scenes-drag-0000001795410277)
- 智感握姿 (smart grip, HarmonyOS 6 era): background statement "the larger the screen, the smaller the stably reachable range; far end/top of a bar phone and the inner screen of a foldable make key actions hard to operate and increase mistouch". System recognises holding hand (left/right/both/none) and interacting hand (left/right). Principle: move only "高频且难触达" (frequent and hard to reach) components. Two ways: add a follower copy in the easy zone (original stays) or shift the component. Function, state and behaviour must stay identical; do not move frequently, do not move a target being pressed, do not interrupt an operation in progress. Floating widgets keep vertical position and equal side margins; grouped components (toolbar) move as a group keeping order and spacing. Do not apply to low-frequency, non-operational, ad/inducement buttons. — [智感握姿](https://developer.huawei.com/consumer/cn/doc/design-guides/smart-reachability-0000002556657823)
- Touch targets (secondary, sspai summary of Huawei app UX standards): recommended 48x48 vp, minimum 40x40 vp on phone/tablet/foldable; visible control may be smaller if hit area meets minimum; lift bottom controls/keyboards/floating buttons by 28 vp over the navigation bar; minimum font 8 vp on phone. — [少数派](https://sspai.com/post/112882)
- Tablet design page defers hot-zone sizes to the UX standards and gives no numbers. — [平板](https://developer.huawei.com/consumer/cn/doc/design-guides/pad-0000001823654157)
- Third-party (Tencent Cloud) article on thumb zones: portrait two-handed blind area in upper half, landscape top-middle; generic, not Huawei research. — [腾讯云开发者社区](https://cloud.tencent.com/developer/news/215517)

### Inferences
- Huawei's stance on reach differs from Apple HIG (which shows static layout guidance, bottom tab bars, 44x44 pt minimum) and Material 3 (48x48 dp target, bottom-anchored patterns): Huawei adds a runtime sensor (grip detection) to relocate controls. For a mobile RAW editor, the analogue is: keep the parameter dial/slider at the bottom by default, and consider mirroring the primary tool strip to the holding-hand side on the large inner screen of the Find N5. This is a design idea, not a Huawei recommendation; the OPPO device will not have HarmonyOS's grip API (we would need our own heuristics from touch history).
- The "do not move the control being pressed / do not interrupt an operation" rule is directly relevant: never rearrange the adjustment panel while the user is mid-drag on a slider.

### Gaps
- No official 手持热区/单手热区/双手热区 diagrams or dimensions found; no Huawei-cited thumb-reach study or ergonomic measurement numbers found (only the qualitative smart-grip background). Edge-swipe active-zone widths (mm/vp) not found.
- 48/40 vp touch-target figures come from a secondary summary; the primary UX-standard page fetched returned only a changelog index. Verify at [ux-guidelines overview](https://developer.huawei.com/consumer/cn/doc/design-guides/ux-guidelines-overview-0000001760867048).

---

## 3. 一多 responsive/adaptive layout: breakpoints, grid, techniques, near-square screens

### Takeaway
Breakpoints are by window width (xs/sm/md/lg/xl) AND a separate height breakpoint based on height-to-width ratio (sm <0.8, md 0.8-1.2, lg >=1.2), which is exactly the mechanism Huawei gives for near-square windows like Pura X outer screen and unfolded foldables. Layout decisions should be by window breakpoints, never by fold state or device model.

### Cited Findings
- Width breakpoints (vp): xs (0,320), sm [320,600), md [600,840), lg [840,1440), xl [1440,+inf). Height breakpoints = height/width ratio: sm (0,0.8), md [0.8,1.2), lg [1.2,+inf). Breakpoints apply to windows not devices; similar-width windows share the same layout. — [响应式布局](https://developer.huawei.com/consumer/cn/doc/best-practices/bpta-multi-device-responsive-layout)
- Grid (design-guide layout basics): width 0-<600 vp = 4 columns, 600-<840 = 8 columns, >=840 = 12 columns. Margin/gutter pairs: 16/8 (or 16/16) for 4 cols, 24/12 for 8 cols, 32/16 (or 32/20) for 12 cols (column-to-breakpoint mapping partly inferred by the page summariser). Max content width 2220 vp. Gutter generally not larger than margin. 8 vp base grid, 4 vp for small items like icons. — [布局基础](https://developer.huawei.com/consumer/cn/doc/design-guides/design-layout-basics-0000001795579413)
- Example device sizes (vp): Mate 60 374x826; Pura 80 359x789; Mate X5 unfolded 712x798 (ratio 1.12, i.e. near-square, md width, md height); MatePad 1280x800. — [布局基础](https://developer.huawei.com/consumer/cn/doc/design-guides/design-layout-basics-0000001795579413)
- Responsive techniques: 分栏 (split column: nav + content side by side when width >= 600 vp, e.g. settings); 重复 (repeat: same-property components side by side, suited to enlarging content beyond 150%); 挪移 (shift: top-bottom layout becomes left-right, suited to rotation or aspect change over 200%); 缩进 (indent: content indented on wide screens, 6- or 8-column indents on 8/12 grids). — [响应式布局](https://developer.huawei.com/consumer/cn/doc/best-practices/bpta-multi-device-responsive-layout)
- Near-square windows: judge by height breakpoint (sm = landscape-like, md = square-like) combined with width. Small square windows (width sm, height md) deserve a unique layout, e.g. 1:1 top-bottom split on Pura X outer screen. Width md alone cannot distinguish unfolded foldable, portrait tablet, landscape phone; use the height breakpoint too. — [响应式布局](https://developer.huawei.com/consumer/cn/doc/best-practices/bpta-multi-device-responsive-layout)
- Windows of similar width should use same layout; the guidance to add breakpoints is for horizontal or square-like windows. — [搜索摘要 of bpta pages](https://developer.huawei.com/consumer/cn/doc/best-practices/bpta-multi-device-responsive-layout)
- Layout integrity: components, images, video must not shift, clip, distort or blur on fold/rotate; mobile-first (design smallest, extend upward); avoid fixed px. Foldable: keep key information away from hinge. — [布局基础](https://developer.huawei.com/consumer/cn/doc/design-guides/design-layout-basics-0000001795579413)
- Developer rule: on fold/unfold listen only to window.on('windowSizeChange') to refresh size and breakpoints. Double-fold unfolded default split ratio 1:1. — [窗口模式](https://developer.huawei.com/consumer/cn/doc/best-practices/bpta-multi-device-window-mode)
- Large-screen app guidance: do not simply scale up phone UI; reflow by window width, orientation and page type; two-column/multi-column or primary/secondary; avoid wide empty margins; keep key actions visible. — [woshipm Pura X Max 适配](https://www.woshipm.com/share/6367374.html) (Huawei developer-community content republished; secondary)
- Foldable UX standard: unfolded state must not add pages or steps; mandatory "open/close continuity" (preserve page, scroll offset, input, image clarity, playback progress). — [折叠屏应用 UX 体验标准 4.1.1](https://developer.huawei.com/consumer/cn/doc/design-guides/ux-guidelines-foldable-screen-0000001807866557)
- Adaptive techniques (拉伸, 均分, 占比, 缩放, 延伸, 隐藏, 折行): not defined on the fetched pages; only secondary articles mention linear-layout stretch/zoom and justifyContent for even distribution. — [CSDN breakpoint article, secondary](https://harmonyosdev.csdn.net/69673c4a6554f1331aa1e9ed.html)

### Inferences
- For Dapple on Find N5 inner screen (~near-square, roughly 1.1-1.2 ratio when unfolded: same class as Mate X5 712x798 vp), the Huawei scheme would classify it as width md (maybe lg depending on dpi) with height md. Huawei's guidance implies a dedicated layout rather than stretched phone UI: split canvas and tool panel side by side (分栏) rather than the phone's top image / bottom panel. Whether to put the panel left or right is our call (consider holding hand).
- Using a height-to-width ratio breakpoint is a neat portable idea: define our own two-axis breakpoint (width class x aspect class) for Android and iOS rather than device/fold-state checks. Apple's size classes and Material 3 window size classes (compact/medium/expanded: 600/840 dp) are width-only; note Huawei's 600/840 are identical to Material 3's width breakpoints, so a shared implementation is feasible.
- 挪移 (shift top-bottom to left-right) is the technique matching a RAW editor: canvas over panel in portrait, canvas beside panel when width >= ~600 vp and aspect is landscape/square-like.

### Gaps
- The six "adaptive" techniques were not found defined on any official page I could load; harmonyos-guides responsive pages returned 503.
- Exact Find N5 vp dimensions were not researched (outside Huawei scope).

---

## 4. Foldable postures, hover (悬停态), Pura X / wide foldable

### Takeaway
Official rules for hover posture are concrete: lower half holds interactive controls, upper half holds information/content; keep upper content 16 vp (~3 mm) and lower content 40 vp (~7 mm) away from the crease midline. Mandatory for video, live, calls, meetings, and camera apps. Pura X is a "阔折叠" (wide foldable) with specific rules for short screen height. Correction for the brief: per Huawei's table Pura X outer screen is ~1:1 (326x326 vp) and its unfolded inner screen is 16:10 (440x707 vp), not a 16:10 outer screen.

### Cited Findings
- 4.1.3 悬停适配 (mandatory if applicable; long video, short video, live, calls, meetings, camera): interactive controls in the lower half; information and browsing content in the upper half; dialogs and half-modal in the lower half; context-following controls (menus) appear on the same side as their trigger. — [折叠屏应用 UX 体验标准](https://developer.huawei.com/consumer/cn/doc/design-guides/ux-guidelines-foldable-screen-0000001807866557)
- 4.1.4 折痕避让 (mandatory if applicable, same categories): "上半屏内容由中线向上避让 16 vp (3 毫米)、下半屏内容由中线向下避让 40 vp (7 毫米)". — [同上](https://developer.huawei.com/consumer/cn/doc/design-guides/ux-guidelines-foldable-screen-0000001807866557)
- Hover posture definition (best practice): intermediate state between fully open and folded, device rests stably; suited to video calls, playback, photography, music (no frequent touch); app must avoid the crease and adapt upper and lower interfaces separately; `display.getFoldStatus()` half-fold = hover. — [阔折叠/阔直板应用开发](https://developer.huawei.com/consumer/cn/doc/best-practices/bpta-purax-guide)
- Pura X series screen table (height:width ratio; vp floor): Pura X folded 1:1, 326x326 vp, 980x980 px, breakpoints sm x md; Pura X unfolded 16:10, 440x707 vp, 1320x2120 px, sm x lg; Pura X Max folded (outer) 14:10, 459x672 vp, sm x lg; Pura X Max unfolded 10:14, 939x664 vp, lg x sm; Pura X View (bar) 16:9.5, 440x744 vp. — [阔折叠/阔直板应用开发](https://developer.huawei.com/consumer/cn/doc/best-practices/bpta-purax-guide)
- Wide-fold experience items (strongly recommended, for small screen height): 4.2.1 reduce title font; 4.2.2 replace search box with search icon on the title row; 4.2.3 segmented index bar with press-and-slide; 4.2.4 switch fixed bottom tabs to floating tab bar. — [UX 标准 4.2.x](https://developer.huawei.com/consumer/cn/doc/design-guides/ux-guidelines-foldable-screen-0000001807866557)
- Wide-fold best practices: unfolded landscape (Pura X Max, lg width) use two/three-column, waterfall or multi-column grid, switch image-over-text to side-by-side; dialogs on short outer screens use adaptive layout or constraintSize to cap at 90% of parent height; centred dialog when width breakpoint sm, otherwise popover; half-modal POPUP style becomes bottom sheet when width < 600 vp; outer-screen browsing auto-hides nav bar and title bar, floating bottom tab (HdsTabs) with frosted material to enlarge visible area; video in adaptive immersive full screen avoiding stretch/crop; wide-bar phones have larger corner radii, use display.getRoundedCorner() to inset controls in landscape. — [阔折叠/阔直板应用开发](https://developer.huawei.com/consumer/cn/doc/best-practices/bpta-purax-guide)
- Camera on foldables: after fold/window/orientation change, re-check available cameras, do not select by fixed front/back rule, re-validate orientation, preview ratio and preview area on restart. — [woshipm Pura X Max 适配](https://www.woshipm.com/share/6367374.html)
- Gallery/image-browsing: only generic rules (treat rotation and fullscreen entry/exit as full window changes; preserve viewing state and control position). No specific gallery layout. — [woshipm](https://www.woshipm.com/share/6367374.html)
- Huawei revised its foldable design spec with outer-screen and multi-window chapters (2022 report): large outer screens can reuse phone layout; small outer screens need dedicated design. — [IT之家](https://www.ithome.com/0/603/761.htm) (search snippet only; page returned 403 when fetched)

### Inferences
- "Upper content / lower controls" is the hover recipe that maps onto a RAW editor in half-folded (laptop) posture on Find N5: image preview on the upper half, sliders/dials on the lower half. The numeric crease clearances (16 vp above, 40 vp below) are Huawei-specific; Huawei's asymmetry reflects that the lower half is touched, so more clearance avoids the crease dip under the thumb. Find N5's crease geometry differs, but the principle transfers.
- The 28 vp navigation-bar lift and 40 vp crease margin both come from "keep controls out of the system/hardware dead zone", a pattern shared with Apple's safe areas.
- Not found: official reasoning for 16 vs 40; the "why" above is my inference.

### Gaps
- No Huawei Gallery (图库) or Camera app layout description for Mate X5/X6 hover or unfolded states found in official sources.
- Mate X5/X6 postures beyond fold/unfold/hover not documented in fetched pages.

---

## 5. Continuous-value controls: Slider, rulers, haptics, Huawei editor details

### Takeaway
Huawei publishes no design-guide page for sliders, rulers/dials or vibration. Only component API defaults, one haptic mention (drag lift), and consumer support text exist. What can be said about Huawei Gallery's editor is thin and user-facing.

### Cited Findings
- ArkUI Slider defaults: horizontal slider default height 40 vp (vertical: width 40 vp); step-mark dot diameter default 4 vp; showSteps for tick values; styles OutSet/InSet. — [Slider API](https://developer.huawei.com/consumer/cn/doc/harmonyos-references/ts-basic-components-slider)
- Haptics: dragging on touch triggers a vibration when the object floats from long press. — [拖拽](https://developer.huawei.com/consumer/cn/doc/design-guides/hmi-scenes-drag-0000001795410277)
- Linear motor FAQ (older page): linear motors give finer feel and lower latency than ordinary motors; Haptics Engine presets cover camera, volume, picture and input scenes; custom effects target games. — [FAQ 线性马达能力](https://developer.huawei.com/consumer/cn/doc/development/system-Guides/faq-0000001051088140)
- Consumer support: Gallery adjust flow is double-tap picture, then Edit > Adjust, choose brightness/contrast/saturation etc. and slide; no mention of vibration or double-tap reset. — [华为消费者支持](https://consumer.huawei.com/cn/support/content/zh-cn16049900/)
- A developer article describes a HarmonyOS ruler component with one-decimal precision that snaps to the nearest tick after scrolling stops (third party, not official). — [infoq article](https://xie.infoq.cn/article/dff16007b09db34fccceb70a7)
- Tablet page: stylus feedback should be visual, haptic and audio. — [平板](https://developer.huawei.com/consumer/cn/doc/design-guides/pad-0000001823654157)
- Double-tap in the official gesture list is for zoom and play/pause; the interaction-events page forbids binding both tap and double-tap to the same action. — [手势](https://developer.huawei.com/consumer/cn/doc/design-guides/hmi-touchscreen-0000001928273206)

### Inferences
- The tick-snap-after-stop behaviour plus Huawei's spring settling and haptics-on-state-change would form a coherent dial design, but I found no Huawei source prescribing tick haptics. Double-tap-to-reset on a parameter label is a pattern from Lightroom/Apple Photos that I could not confirm in Huawei Gallery.

### Gaps
- No official vibration/haptic design spec page found (searched design-guides). No Huawei guidance on rulers/dials, number bubbles, or reset gestures. Huawei Gallery editor details beyond the support text were not found; direct device inspection would be needed.

---

## 6. Visual language (光感 / 沉浸) vs Apple Liquid Glass (brief)

### Takeaway
HarmonyOS 6 introduced "沉浸光感" (immersive light-sense visual effects): translucent material, interactive particles, "gaseous" motion, plus a floating bottom tab bar. It is system-level and opt-in per theme; developers get it through HDS components. Parallels to Liquid Glass are visible but I have no official comparative statement.

### Cited Findings
- HarmonyOS 6 immersive light-sense effect: three features (通透质感 translucent material, 灵动粒子 interactive particles, 气态动效 gaseous motion), shown with Mate 80; user toggle in settings; only some official themes support it. — [IT之家](https://www.ithome.com/0/913/506.htm) (via search summary)
- Floating bottom tab bar and immersive light-sense are the most visible HarmonyOS 6 UI changes; developer API: from API 23 (HarmonyOS 6.1.0) HDS components add `systemMaterialEffect` for bottom tabs; floating nav offers strong/balanced/weak transparency. — [jishuzhan](https://jishuzhan.net/article/2045296296320368641) (secondary developer article)
- Requires HarmonyOS 6.0.0.125+ on Mate 80 / Mate X7 initially. — [IT之家](https://www.ithome.com/0/913/506.htm)
- Pura X guidance recommends floating bottom tabs with frosted material to enlarge visible area on short screens. — [阔折叠/阔直板应用开发](https://developer.huawei.com/consumer/cn/doc/best-practices/bpta-purax-guide)

### Inferences
- Both Huawei's floating translucent tab bars and Apple's Liquid Glass let content run edge to edge beneath chrome; for a photo editor, the risk is the same: translucent chrome over the image corrupts colour judgement. Keep the edit canvas surround neutral and opaque (design choice, not Huawei guidance).

### Gaps
- No official HDS design-guide text for the material parameters fetched; no source for a Huawei-versus-Apple claim.

---

## 7. Comparison to Apple HIG and Material 3 (synthesis, mostly inference)

### Takeaway
Huawei's guidance is closer to Apple on motion philosophy (physics, velocity inheritance, direct manipulation) and closer to Material on layout mechanics (600/840 breakpoints, 4/8/12 columns, 8 vp grid, window-size-driven). Its unique contributions are the height-ratio breakpoint for near-square windows, quantified hover/crease numbers, and runtime grip sensing.

### Cited Findings
- Huawei width breakpoints 600/840 vp with 4/8/12 columns and 8 vp base grid — [布局基础](https://developer.huawei.com/consumer/cn/doc/design-guides/design-layout-basics-0000001795579413). (Material 3 window size classes compact <600, medium 600-839, expanded >=840 dp: from my background knowledge, not fetched this session.)
- Huawei hover/crease numbers and grip sensing — see sections 2 and 4.

### Inferences
- Material 3 and Apple HIG have no equivalent published crease-clearance numbers or hover-posture mandate in my knowledge (not verified this session); Huawei's are the most prescriptive. Apple HIG foldable guidance does not exist for iPhone yet (as of my knowledge), so for the later iPhone port Huawei's rules are only useful for the large-screen/iPad-style layout.

### Gaps
- No fresh fetch of Apple HIG or Material 3 pages in this task; comparison is from prior knowledge and should be checked by the other researchers' notes.
