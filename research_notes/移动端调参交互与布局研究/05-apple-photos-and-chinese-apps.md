# Apple Photos editing controls, Chinese photo editors, and HarmonyOS/ColorOS hot-zone guidance

Research date: 2026-11-12. The WebFetch/WebSearch tools return summarised text, not verbatim pages, so quotes below are the tool's paraphrases of the pages. Apple Support pages often came back truncated. Coverage is partial. Several items the brief asked for were not found and are listed under Gaps.

## (A) Apple Photos edit controls on iPhone (iOS 13 to 18, iOS 26), Camera zoom wheel, Photographic Styles

### Takeaway
Apple's own User Guide confirms the core flow: Edit, swipe left under the photo to a row of effect buttons, tap a button, then drag a slider or dial. The button outline shows the direction and amount of change, and tapping the button or the photo compares with the original. Third-party reviews indicate iOS 18 and iOS 26 changed the entry point and visuals (three-slider Edit icon, Liquid Glass) but not the editing model. Dial details such as tick marks, relative drag, a numeric readout and haptics at zero were NOT confirmed by any source I could fetch.

### Cited Findings
- iPhone User Guide, "Edit photos and videos on iPhone": tap Edit, swipe left under the photo to see the buttons for each effect (Exposure, Brilliance, Highlights and so on), tap a button, then drag the slider. The outline around the button shows whether the effect was increased or decreased. Tap the effect button to see before/after, or tap the photo to toggle between edited and original. Done saves, and Cancel then Discard Changes discards. Edit then Revert then Revert to Original returns to the unedited photo. — [Apple iPhone User Guide (14.0, en-kz)](https://support.apple.com/en-kz/guide/iphone/iphb08064d57/14.0/ios); a search summary quotes the same swipe-and-drag wording from [Apple iPhone User Guide](https://support.apple.com/guide/iphone/iphb08064d57/ios). The guide has been available in versions 14, 15 and 16 at the same ID, per the search results listing.
- The Enhance (magic wand) button applies automatic edits. iMore describes sliding a dial at the bottom to set the intensity of Auto-Enhance, and the lighting controls as a dial running left to right for a weaker or stronger effect. iMore did not mention a numeric readout or an on/off toggle. — [iMore, how to use adjust light and color](https://www.imore.com/how-use-smart-adjustments-photos-iphone-ipad) (undated in my fetch; likely iOS 13/14 era)
- iOS 18: the top-right Edit button was replaced by a three-slider icon that opens the editing view. One guide says the editing interface did not change fundamentally in iOS 18. — [search results citing iGeeksBlog / MacRumors](https://macrumors.com/guide/ios-18-photos) and [iGeeksBlog](https://www.igeeksblog.com/how-to-edit-photos-on-iphone/) (secondary; I could not fetch them directly)
- iOS 26: reviewers say Photos brings back tabs, uses Liquid Glass mainly for controls (the photo stays the focus), and adds no brand-new ways to edit versus iOS 18. A bottom-bar Edit button (three sliders) is reported. — [AppleInsider, Photos iOS 26 vs iOS 18](https://appleinsider.com/inside/ios-26/vs/photos-ios-26-vs-ios-18-small-changes-huge-impact) (via search summary only; direct fetch returned 403) and [AppleInsider, Photos in iOS 26 brings back tabs](https://appleinsider.com/articles/25/06/12/photos-in-ios-26-brings-back-tabs-and-generates-spatial-photos)
- Camera zoom wheel: press and hold the 1x (or 0.5x/2x, model dependent) button, then a zoom wheel/slider appears and you slide side to side. — [iMore, camera zoom features](https://www.imore.com/how-use-camera-zoom-features-iphone-and-ipad), [Gadget Hacks](https://ios.gadgethacks.com/how-to/theres-better-way-zoom-with-your-iphones-camera-0312986/), [Apple Community thread](https://discussions.apple.com/thread/256072602). One source says the wheel is limited to 0.5x to 3x on iPhone 11 (the Pro models go to 6x); that is an unverified single-source claim.
- Camera Control (the physical button) is the source of the haptic clicks when a zoom switches lenses, per a forum post. The sources found do NOT describe a haptic at 0.5x/2x on the on-screen wheel. — [MacRumors Camera Control guide](https://www.macrumors.com/how-to/iphone-using-camera-control-button/) and [forum post](https://forums.macrumors.com/threads/iphone-18-said-to-use-simpler-camera-control-button-design.2469969/page-3)
- Photographic Styles (iPhone 16 and later): Apple's guide says to drag on the pad to adjust tone and color simultaneously, and to drag the slider below it to adjust intensity (called Palette). Axis mapping is reported inconsistently: MacRumors says X is color and Y is tone, TapSmart says up/down is Tone and left/right is Color, and onoff.gr says the same with up meaning brighter. — [Apple, Use latest generation Photographic Styles](https://support.apple.com/en-is/guide/iphone/iph629d2cd37/ios) (title/nav only fetched; wording via search summary), [MacRumors guide](https://www.macrumors.com/guide/iphone-16-photographic-styles/), [TapSmart](https://www.tapsmart.com/tips-and-tricks/photographic-styles-guide), [onoff.gr](https://www.onoff.gr/blog/en/iphone/photographic-styles-iphone-find-your-perfect-look/). Styles can be edited after capture in Photos on newer models ([MacRumors](https://www.macrumors.com/how-to/iphone-16-edit-photographic-styles-after-shooting)).
- iOS 26.2 added a Liquid Glass slider for the Lock Screen clock; this is not part of the Photos editor. — [MacRumors, 2025-11-04](https://www.macrumors.com/2025/11/04/ios-26-2-liquid-glass-slider/)

### Inferences
- The model is "dial or slider under the photo, button row above it, outline as the value indicator". The Apple guide documents the outline as the indicator, so a numeric bubble is not part of the documented design. My memory says the iOS 13+ editor shows a number inside the ring and a tick-mark ruler with a haptic at zero, but no source I fetched confirms it, so treat it as unverified.
- The press-and-hold-to-open wheel is a mode switch from a quick tap (tap jumps, hold scrubs). This is inferred from the descriptions.

### Gaps
- No fetched source confirmed: tick-mark ruler design, relative (non-absolute) drag behaviour, numeric readout in the ring, haptic at default/zero, Auto strength details, or the iOS 13 redesign specifics. Apple Support pages returned truncated content to the fetcher.
- No MacStories/9to5Mac/Verge piece on editing-control interaction was fetched. The AppleInsider iOS 26 page returned 403.
- WWDC 2018 "Designing Fluid Interfaces" and Apple Design Award rationale were not searched or retrieved within the call budget.
- Details of what changed in the iOS 26 editor beyond the Edit button location and Liquid Glass styling are not established.

## (B) Chinese mobile photo editors (醒图, 美图秀秀, 黄油相机, ColorOS, Xiaomi/Huawei gallery)

### Takeaway
I found no citable source describing slider placement, number bubbles, double-tap reset or the compare button for these apps. Searches returned app-store pages and generic tutorials only. Everything in this section is a gap except the one fact below.

### Cited Findings
- 醒图 App Store release notes mention a new "暗角" (vignette) effect in the 调节 (Adjust) panel and a one-tap "一键出片" feature, but nothing about slider gestures. — [醒图 App Store (CN)](https://apps.apple.com/cn/app/id1500526240)
- A 美图秀秀 search yielded no tutorial covering long-press compare or slider drag. The only relevant hit was a PC-version tutorial that sets filter strength between 30% and 100%. — [IT之家](https://www.ithome.com/0/394/965.htm) (PC version, old, low relevance)

### Inferences
- None that I can support from sources.

### Gaps
- Slider type (on-photo scrubber vs bottom slider), number bubble, panel layout, double-tap reset and compare button for 醒图, 美图秀秀, 黄油相机, ColorOS 相册, Xiaomi and Huawei galleries: not found. sspai.com, woshipm.com and zhihu.com were not successfully retrieved for this topic. Suggested next step: fetch the apps directly or use screen recordings, or search 小红书 tutorials.

## (B2) HarmonyOS and ColorOS hot-zone / foldable guidance

### Takeaway
I could not retrieve developer.huawei.com's design guidelines or open.oppomobile.com's specifications. Only secondary sources are available: a 40x40 vp minimum touch target (secondary), window-width breakpoints, a system one-handed mode, and a grip-detection API.

### Cited Findings
- A Chinese article summarising HarmonyOS app-review guidance lists a 40 x 40 vp minimum touch target for phones, tablets and foldables. The touch area may exceed the visible control (the responseRegion approach). Secondary source, so verify against official docs. — [search-result summary citing jishuzhan / DEV Community](https://dev.to/harmonyos/make-small-icons-easy-to-tap-enlarging-image-hit-areas-with-responseregion-or-wrapper-padding-obn)
- HarmonyOS breakpoints (window width) per community articles: xs 0-320 vp, sm 320-600 vp (phone portrait), md 600-840 vp (phone landscape or foldable), lg 840 vp and above (tablet/2in1). — [InfoQ article](https://xie.infoq.cn/article/51c6d7280b2527a42740e35fb). A HarmonyOS 7 article instead cites 440/600/840 vp (hence a conflict on the first boundary) — [besthub](https://www.besthub.dev/articles/harmonyos-7-floating-tabs-window-width-driven-layouts-for-foldables-tablets-392fb49b2fd6). Both are non-official.
- FoldSplitContainer (API 12+) supports two- or three-pane layouts across expanded, hover and folded states. — [51CTO](https://ost.51cto.com/posts/35699)
- HarmonyOS 智感握姿 (grip awareness) detects left hand, right hand, both hands or no grip. It needs the DETECT_GESTURE permission and can be used to adapt the UI. — [CSDN HarmonyOS dev](https://harmonyosdev.csdn.net/693c02be0800f3458b81c949.html)
- Huawei's system one-handed mode shrinks the screen and docks it to a side; HarmonyOS 5+ adds arc-swipe-to-corner and bottom-edge swipe entry gestures. — [Huawei consumer support](https://consumer.huawei.com/cn/support/content/zh-cn15819650/)
- A woshipm article on 阔折叠 (wide-fold) and foldable UI adaptation is qualitative only (prioritise breakpoints over models, scrollable containers, multi-column on large screens) with no numbers or hot-zone sizes. — [woshipm](https://www.woshipm.com/?p=6453984)
- ColorOS 13.2: reports say 30+ first-party apps were reworked for large screens and the responsive framework 2.0 adapts layout to screen size. — [MyDrivers](https://news.mydrivers.com/1/941/941012.htm). No official open.oppomobile.com spec was found.

### Inferences
- A 40 vp touch target is below Apple's 44 pt convention (from memory, not fetched here), so a safe cross-platform minimum would be 44 or more.

### Gaps
- No official Huawei "手持热区/操作热区" figures or thumb-reach zone diagrams; no official 折叠屏 hover-state guidance numbers; no OPPO/OnePlus large-screen specification numbers.
