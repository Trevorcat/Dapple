# Apple HIG and WWDC Design Guidance for Dapple (light-themed mobile photo app)

Method note: the HIG pages are JS-rendered, so I read them through Apple's page data endpoint (`developer.apple.com/tutorials/data/design/human-interface-guidelines/<page>.json`). The human-readable URLs cited below are the same pages. Quotes are as returned by that fetch (the fetch tool summarizes, so a few quotes may be lightly trimmed). Fetched in Nov 2026. Where Apple's text had no explicit statement, this is said, and my own inference goes under "Inferences". The OPPO Find N5 and foldable-specific issues are outside HIG; HIG has no foldable guidance.

## 1. Foundations: color, light/dark, contrast, typography, layout, icons, motion, accessibility

### Takeaway
Light mode is fully supported. Use semantic and system colors with light, dark and increased-contrast variants. Keep chrome neutral so the photo carries the color. Minimum contrast is 4.5:1 (7:1 is recommended for custom small text). The iOS default text size is 17 pt with an 11 pt minimum. The default hit target is 44x44 pt. Layout should key off size classes, not device type.

### Cited Findings
Color and appearance
- System colors vary subtly by appearance, and Increase Contrast makes the differences "far more apparent". For custom colors: "make sure to supply light and dark variants, and an increased contrast option for each variant". "Even if your app ships in a single appearance mode, provide both light and dark colors to support Liquid Glass adaptivity" — [Color](https://developer.apple.com/design/human-interface-guidelines/color)
- "Avoid hard-coding system color values … The actual color values may fluctuate from release to release." "Avoid redefining the semantic meanings of dynamic system colors." — [Color](https://developer.apple.com/design/human-interface-guidelines/color)
- Liquid Glass color: "By default, Liquid Glass has no inherent color, and instead takes on colors from the content directly behind it." "Apply color sparingly … reserve it for elements that truly benefit from emphasis, such as status indicators or primary actions … apply color to the background rather than to symbols or text … Refrain from adding color to the background of multiple controls." — [Color](https://developer.apple.com/design/human-interface-guidelines/color)
- "Avoid relying solely on color to differentiate between objects, indicate interactivity, or communicate essential information … use text labels or glyph shapes." — [Color](https://developer.apple.com/design/human-interface-guidelines/color); also [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility) ("Convey information with more than color alone … red-green and blue-orange").
- Color management: "Apply color profiles to your images." "Photos and videos that use wide color are more lifelike." "Use the Display P3 color profile at 16 bits per pixel (per channel) and export images in PNG format"; P3 images "appear fine on sRGB displays". The Color page has no photo-specific chrome-vs-content section (the fetch read only the first 100k of 182k characters, so the remainder is unread) — [Color](https://developer.apple.com/design/human-interface-guidelines/color)
- Dark Mode page: "people often choose Dark Mode as their default interface style, and they generally expect all apps and games to respect their preference." "Ensure that your app looks good in both appearance modes" (Auto switches during the day, possibly while the app runs). "In rare cases, consider using only a dark appearance … an app that supports immersive media viewing [may use] a permanently dark appearance that lets the UI recede and helps people focus on the media." "Avoid offering an app-specific appearance setting … they may think your app is broken." — [Dark Mode](https://developer.apple.com/design/human-interface-guidelines/dark-mode)
- Dark Mode: "Soften the color of white backgrounds. If you display a content image that includes a white background, consider slightly darkening the image" and "Make sure full-color images and icons look good in both appearances." — [Dark Mode](https://developer.apple.com/design/human-interface-guidelines/dark-mode)
- Contrast: "no lower than 4.5:1. For custom foreground and background colors, strive for a contrast ratio of 7:1, especially in small text." — [Dark Mode](https://developer.apple.com/design/human-interface-guidelines/dark-mode)
- Accessibility contrast table (WCAG AA): up to 17 pt, any weight: 4.5:1; 18 pt: 3:1; bold: 3:1. "ensure it at least provides a higher contrast color scheme when … Increase Contrast is turned on"; check both light and dark — [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility)

Typography
- iOS default 17 pt, minimum 11 pt (visionOS min 12). "Follow the recommended default and minimum text sizes … for both custom and system fonts" — [Typography](https://developer.apple.com/design/human-interface-guidelines/typography); same table in [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility)
- iOS text styles at the Large (default) size, in pt as size/leading: Large Title 34/41, Title1 28/34, Title2 22/28, Title3 20/25, Headline 17/22 (semibold), Body 17/22, Callout 16/21, Subhead 15/20, Footnote 13/18, Caption1 12/16, Caption2 11/13 — [Typography](https://developer.apple.com/design/human-interface-guidelines/typography)
- "prefer Regular, Medium, Semibold, or Bold font weights, and avoid Ultralight, Thin, and Light." "Make sure your app's layout adapts to all font sizes." "Keep text truncation to a minimum as font size increases." "Increase the size of meaningful interface icons as font size increases." "Maintain a consistent information hierarchy regardless of the current font size." "don't embed system fonts" — [Typography](https://developer.apple.com/design/human-interface-guidelines/typography)
- "Ideally, give people the option to enlarge text by at least 200 percent." — [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility)
- WWDC25: typography "now bolder and left-aligned to improve readability in key moments like alerts and onboarding." — [WWDC25-356](https://developer.apple.com/videos/play/wwdc2025/356/)

Layout
- "Respecting the safe area is essential to make sure system UI and hardware features like the Dynamic Island don't obstruct content and controls." — [Layout](https://developer.apple.com/design/human-interface-guidelines/layout)
- "Determine layout based on size classes, not device type or orientation." "Consider all possible combinations of size classes." "Keep functionality the same as size classes change … Consider taking advantage of larger spaces to switch from a tab bar to a sidebar." — [Layout](https://developer.apple.com/design/human-interface-guidelines/layout)
- "Instead of applying a solid or semi-opaque background color beneath controls, use a scroll edge effect." "For full-screen background content, be sure to extend it underneath sidebars, toolbars, and tab bars." A `backgroundExtensionEffect()` mirrors and blurs an image beneath adjacent components — [Layout](https://developer.apple.com/design/human-interface-guidelines/layout)
- The Layout page has no iPhone 17 Pro Max-specific layout guidance (only a Sept 9, 2025 changelog entry adding device specs) and no foldable guidance — [Layout](https://developer.apple.com/design/human-interface-guidelines/layout)
- WWDC25 concentricity: "Concentric shapes calculate their radius by subtracting padding from the parent's." — [WWDC25-356](https://developer.apple.com/videos/play/wwdc2025/356/)

Hit targets and spacing
- Default control size iOS/iPadOS 44x44 pt, minimum 28x28 pt. "about 12 points of padding around elements that include a bezel … about 24 points of padding" for elements without a bezel — [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility). Buttons need "a hit region of at least 44x44 pt" — [Buttons](https://developer.apple.com/design/human-interface-guidelines/buttons)

Icons / SF Symbols
- 9 weights matched to SF font weights, 3 scales (small/medium/large), 4 rendering modes (monochrome, hierarchical, palette, multicolor). Custom symbols should be "Simple, Recognizable, Inclusive, Directly related to the action or content" and must carry accessibility descriptions. "using system-provided colors ensures that symbols automatically adapt to accessibility accommodations" — [SF Symbols](https://developer.apple.com/design/human-interface-guidelines/sf-symbols)
- WWDC25: "Use the same symbols across devices to preserve meaning and build familiarity through repetition." — [WWDC25-356](https://developer.apple.com/videos/play/wwdc2025/356/)

Motion
- "Add motion purposefully, supporting the experience without overshadowing it." "Make motion optional … supplement visual feedback by also using alternatives like haptics and audio." "generally avoid adding motion to UI interactions that occur frequently." "Aim for brevity and precision." "Let people cancel motion … don't make people wait for an animation to complete" — [Motion](https://developer.apple.com/design/human-interface-guidelines/motion)
- Reduce Motion: reduce "automatic and repetitive animations, including zooming, scaling, and peripheral motion"; tighten springs; track gestures directly; "Avoid animating depth changes in z-axis layers"; replace x/y/z transitions with fades; "Avoid animating into and out of blurs" — [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility)
- VoiceOver: "Describe your app's interface and content for VoiceOver." — [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility)

### Inferences
- A permanently light app is allowed by HIG but runs against its default advice ("respect system appearance"; "avoid app-specific appearance setting"). The only explicit exception Apple names for a fixed dark look is immersive media viewing. The safest design is to be light by default and still shipped with a dark variant. A neutral-gray photo canvas, dimmed chrome or a "viewing" mode for the full-screen photo can serve the "UI recedes" goal inside a light UI. Apple does not say this.
- Because WCAG says up to 17 pt needs 4.5:1, small slider-value labels (11-13 pt) over photos need either a solid or glass backing or 4.5:1 or better.
- No HIG text covers soft-proofing or color-accurate editing environments. Treat "keep chrome neutral so photo color is judged correctly" as a derived requirement. The color-profile and P3 guidance is the only HIG anchor.

### Gaps
- HIG text on "color for photo content vs chrome" is not explicit. The unread second half of the Color page (offset 100000) was not checked.
- No official HIG numbers for iPhone 17 Pro Max margins. No foldable guidance in HIG.

## 2. Materials and Liquid Glass

### Takeaway
Liquid Glass is a functional layer for controls and navigation floating over content. It is not for the content layer. Use it sparingly, use clear glass only over rich media, and never stack glass on glass. Tint only primary actions. Photo and canvas colors should do the "color" work.

### Cited Findings
- "Liquid Glass forms a distinct functional layer for controls and navigation elements — like tab bars and sidebars — that floats above the content layer." "Don't use Liquid Glass in the content layer … use standard materials for elements in the content layer, such as app backgrounds. An exception … controls in the content layer with a transient interactive element like sliders and toggles; … the element takes on a Liquid Glass appearance … when a person activates it." — [Materials](https://developer.apple.com/design/human-interface-guidelines/materials)
- "Use Liquid Glass effects sparingly … Limit these effects to the most important functional elements in your app." — [Materials](https://developer.apple.com/design/human-interface-guidelines/materials)
- Regular vs clear: "The regular variant blurs and adjusts the luminosity of background content to maintain legibility." "The clear variant is highly translucent … Use this variant for components that float above media backgrounds — such as photos and videos." Dimming: "If the underlying content is bright, consider adding a dark dimming layer of 35% opacity." Clear variant should be used only over visually rich backgrounds — [Materials](https://developer.apple.com/design/human-interface-guidelines/materials)
- Accessibility settings (Reduce Transparency, Increase Contrast) change the appearance of both glass variants — [Materials](https://developer.apple.com/design/human-interface-guidelines/materials)
- Standard materials on iOS: ultra-thin, thin, regular (default), thick; "avoid using quaternary on top of the thin and ultraThin materials, because the contrast is too low." — [Materials](https://developer.apple.com/design/human-interface-guidelines/materials)
- Changelog: Liquid Glass added June 9, 2025, updated Sept 9, 2025 — [Materials](https://developer.apple.com/design/human-interface-guidelines/materials); Toolbars and Buttons updated Dec 16, 2025 — [Toolbars](https://developer.apple.com/design/human-interface-guidelines/toolbars); Sheets "button placement" updated Mar 24, 2026 — [Sheets](https://developer.apple.com/design/human-interface-guidelines/sheets)
- WWDC25 Meet Liquid Glass: "Liquid Glass is best reserved for the navigation layer that floats above the content of your app." "always avoid glass on glass." "Tinting should only be used to bring emphasis to primary elements and actions … When every element is tinted, nothing stands out … If you want to imbue color into your app, do it in the content layer instead." "Scroll edge effects work in concert with Liquid Glass to maintain that crucial separation between the UI and content layers." — [WWDC25-219](https://developer.apple.com/videos/play/wwdc2025/219/)
- WWDC25 design system: "Liquid Glass defines a new functional layer in the UI, floating above your content to bring structure and clarity, without ever stealing focus." "scroll edge effects are not decorative. They don't block or darken like overlays." "Soft is the default and the one you'll use in most cases, especially on iOS." "Apply one scroll edge effect per view." "Instead of relying on decoration, hierarchy should be expressed through layout and grouping." — [WWDC25-356](https://developer.apple.com/videos/play/wwdc2025/356/)
- Image views: "Compositing text on top of images can decrease both the clarity of the image and the legibility of the text … consider … a text shadow or background layer." — [Image views](https://developer.apple.com/design/human-interface-guidelines/image-views)

### Inferences
- In an editor, the photo is the content layer. Only the nav/tools should use glass. The 35% dim rule lets a bottom adjustment bar float over a bright photo. Over a light UI, regular glass is the safer choice and clear glass should be reserved for overlays on the photo.
- Selection counts, badges (e.g. RAW, rating) and "Auto" markers drawn on thumbnails are content-layer overlays and need a solid or standard-material backing, not glass.

### Gaps
- The "Adopting Liquid Glass" technology overview page did not render and was not read. The tab bar minimize behavior is not covered in the WWDC25-219 transcript as fetched.

## 3. Navigation and components: tab bars, toolbars, sheets, buttons, sliders, segmented controls, menus, toggles, collections

### Takeaway
Use a small tab bar (5 or fewer; "fewer is easier"), one prominent action per toolbar, a resizable sheet with a grabber for tool panels, a slider with live feedback, and at most 5 segments on iPhone. Context menus supplement and never replace visible controls.

### Cited Findings
Tab bars
- "Its items rest on a Liquid Glass background that allows content beneath to peek through." Tabs: "It's generally easier to navigate among fewer tabs." "Avoid overflow tabs." "Don't disable or hide tab bar buttons, even when their content is unavailable." Customizable default "five or fewer." "Include tab labels … Use single words whenever possible." "Prefer filled symbols." "Reserve badges for critical information." — [Tab bars](https://developer.apple.com/design/human-interface-guidelines/tab-bars)
- Minimize on scroll applies to tab bars with an attached accessory: "you can choose to minimize the tab bar and move the accessory inline with it when a person scrolls down. A person can exit the minimized state by tapping a tab or scrolling to the top." API: `TabBarMinimizeBehavior` / `UITabBarController.MinimizeBehavior` — [Tab bars](https://developer.apple.com/design/human-interface-guidelines/tab-bars)
- "Avoid applying a similar color to tab labels and content layer backgrounds. If your app already has bright, colorful content … prefer a monochromatic appearance for tab bars." — [Tab bars](https://developer.apple.com/design/human-interface-guidelines/tab-bars)
- "Avoid placing screen-specific actions here — a checkout button, for example, belongs with the content it supports." — [WWDC25-356](https://developer.apple.com/videos/play/wwdc2025/356/)
- A tab bar can include "a dedicated search tab at the trailing end" — [Tab bars](https://developer.apple.com/design/human-interface-guidelines/tab-bars)

Toolbars
- "Reduce the use of toolbar backgrounds and tinted controls … use the content layer to inform the color and appearance of the toolbar." Prefer standard components with concentric corner radii. "Group toolbar items logically by function and frequency of use." "aim for a maximum of three [groups]." "Use the `.prominent` style for key actions such as Done or Submit … Only specify one primary action, and put it on the trailing side." "Prioritize only the most important items" on iOS; add a More menu only if needed. "Prefer simple, recognizable symbols … except for actions like edit." "Consider temporarily hiding toolbars for a distraction-free experience … offer ways to reliably restore hidden interface elements." — [Toolbars](https://developer.apple.com/design/human-interface-guidelines/toolbars)
- "Just be sure to not group symbols with text, like 'Select' and the share icon … it could be perceived as a single button." "move secondary actions into a more menu" — [WWDC25-356](https://developer.apple.com/videos/play/wwdc2025/356/)

Sheets
- Detents: "large … fully expanded" and "medium … about half." "Include a grabber in a resizable sheet … tap it to cycle through the detents … works with VoiceOver." In iOS/iPadOS a sheet can be nonmodal: "people use its functionality to affect the parent view without dismissing the sheet." "Support swiping to dismiss a sheet … If people have unsaved changes … use an action sheet." Cancel on the leading edge and Done on the trailing edge. "Avoid showing all three buttons — Cancel, Done, and Back." — [Sheets](https://developer.apple.com/design/human-interface-guidelines/sheets). The Sheets page contains no Liquid Glass-specific text.
- Modality: "Aim to keep modal tasks simple, short, and streamlined." "Always give people an obvious way to dismiss a modal view." — [Modality](https://developer.apple.com/design/human-interface-guidelines/modality)

Buttons
- "Keep the number of prominent buttons to one or two per view." "Use style — not size — to visually distinguish the preferred choice." "Don't assign the primary role to a button that performs a destructive action." "Avoid applying a similar color to button labels and content layer backgrounds." Labels start with a verb. "Configure a button to display an activity indicator when you need to provide feedback about an action that doesn't instantly complete." — [Buttons](https://developer.apple.com/design/human-interface-guidelines/buttons)

Sliders
- Optional min/max icons; "give live feedback as the value of a slider changes"; consider exact value display and text entry for wide ranges; min on the leading side, max on the trailing side; tick marks for accuracy. The page has no iOS 26 or accessibility text — [Sliders](https://developer.apple.com/design/human-interface-guidelines/sliders)

Segmented controls
- "no more than about five segments on iPhone." Equal widths. "Prefer using either text or images — not a mix of both." — [Segmented controls](https://developer.apple.com/design/human-interface-guidelines/segmented-controls)

Context menus
- "Always make context menu items available in the main interface, too." Hidden by default, so "people might not know it's there." "Aim for a small number of menu items"; about three groups at most; destructive items last and marked destructive; "Provide either a context menu or an edit menu for an item, but not both." — [Context menus](https://developer.apple.com/design/human-interface-guidelines/context-menus)

Toggles
- iOS: "Use the switch toggle style only in a list row … Outside of a list, use a button that behaves like a toggle." "Avoid relying solely on different colors to communicate state." — [Toggles](https://developer.apple.com/design/human-interface-guidelines/toggles)

Collections
- "Generally speaking, collections are ideal for showing image-based content." "Use the standard row or grid layout whenever possible." "By default, people can tap to select, touch and hold to edit, and swipe to scroll." "Use caution when making dynamic layout changes … avoid changing the layout while people are viewing and interacting with it, unless it's in response to an explicit action." "Consider using animations to provide feedback when people insert, delete, or reorder items." No multi-select mechanics or spacing numbers on this page — [Collections](https://developer.apple.com/design/human-interface-guidelines/collections)

### Inferences
- A Dapple flow of Library (grid), Edit, Share maps to 3 tabs or one screen with a persistent bottom tool strip. The shared "Select" toggle plus a share/"Post" prominent action fit the toolbar rules: one prominent action, on the trailing side.
- HIG offers no dedicated Photos-style multi-select pattern. Photos' "Select" mode with a checkmark overlay and a bottom share bar is Apple's de facto convention, but this research did not verify it in the HIG text.

### Gaps
- No HIG text on how the Liquid Glass tab bar or floating toolbar behaves over a full-screen editing canvas. The sheet Liquid Glass changes (inset, translucent iOS 26 sheets) are not in the HIG Sheets page.
- Multi-selection pattern page: HIG `multi-selection`/`editing` URLs returned 404, so no citation.

## 4. Patterns: launching, onboarding, loading and progress, feedback, undo, sharing, settings, drag and drop

### Takeaway
Launch instantly and restore state, with no launch-time onboarding. Show something immediately and use determinate progress where possible. Offer Cancel and multi-level undo. Share through the system activity view, and finish long work in the app.

### Cited Findings
- "Launch instantly … sometimes they don't want to wait more than a couple of seconds." "Restore the previous state when your app restarts … scroll the view to people's most recent position." Launch screen: nearly identical to the first screen, no text, "Don't advertise." — [Launching](https://developer.apple.com/design/human-interface-guidelines/launching)
- Onboarding: "fast, fun, and optional." "Teach through interactivity." "Consider providing a collection of context-specific tips" (TipKit). "Postpone nonessential setup flows or customization steps. Provide reasonable default settings." Request permission in onboarding if needed to function; otherwise "when people first access the specific function." "Prefer letting people experience your app … before prompting them for ratings." — [Onboarding](https://developer.apple.com/design/human-interface-guidelines/onboarding)
- Loading: "Show something as soon as possible … placeholder text, graphics" and "Let people do other things in your app … while they wait." — [Loading](https://developer.apple.com/design/human-interface-guidelines/loading)
- Progress: "When possible, use a determinate progress indicator." "switch a progress bar from indeterminate to determinate" when duration becomes known. "Prefer an activity indicator (spinner) to communicate the status of a background operation or when space is constrained." "let people halt processing … include a Cancel button"; provide Pause too if interruption has side effects. "Avoid vague terms like loading." "Showing 90 percent completion in five seconds and the last 10 percent in 5 minutes can make people wonder if your app is still working and can even feel deceptive." "Display a progress indicator in a consistent location." — [Progress indicators](https://developer.apple.com/design/human-interface-guidelines/progress-indicators)
- Feedback: "Consider integrating status feedback into your interface" near the items it describes. "Make sure all feedback is accessible" (color, text, sound, haptics). Confirm completion only for significant tasks ("people typically expect their action or task to succeed, they only need to know when it doesn't"). Warn only for "unexpected and irreversible" data loss. "Show people when a command can't be carried out and help them understand why." — [Feedback](https://developer.apple.com/design/human-interface-guidelines/feedback)
- Undo and redo: "Let people undo multiple times." "Consider giving people the option to revert multiple changes at once … undo all the changes they made since opening a document." "Show the results of an undo or redo." "Provide undo and redo buttons only when necessary … use the standard system-provided symbols and put the buttons in a toolbar." iOS: "Avoid redefining standard gestures for undo and redo" (three-finger swipe, shake). — [Undo and redo](https://developer.apple.com/design/human-interface-guidelines/undo-and-redo)
- Activity views: "Use the Share button to display an activity view … Avoid confusing people by providing an alternative way to do the same thing." Custom actions: succinct verb titles, an SF Symbol or a roughly 70x70 px custom icon; can exclude non-applicable system activities. Extensions: "Streamline and limit interaction." "Use your main app to denote the progress of a lengthy operation … don't notify them simply because the task completes." Photos' own app-specific actions: Copy Photo, Add to Album, Adjust Location. — [Activity views](https://developer.apple.com/design/human-interface-guidelines/activity-views)
- Settings: "Minimize the number of settings you offer." "Aim to provide default settings that give the best experience to the largest number of people." "prefer letting people modify task-specific options without going to your settings area." — [Settings](https://developer.apple.com/design/human-interface-guidelines/settings)
- Drag and drop: "Offer alternative ways to accomplish drag-and-drop actions"; "Support multi-item drag and drop"; "Display a drag image as soon as people drag a selection about three points"; "Prefer letting people undo a drag-and-drop operation." — [Drag and drop](https://developer.apple.com/design/human-interface-guidelines/drag-and-drop)

### Inferences
- "Shoot fast, edit fast, post fast": the batch develop job is a determinate, cancelable background task with a persistent, consistently placed indicator, while each thumbnail shows a placeholder, then an automatically developed preview. Sharing to WeChat Moments goes through the system share sheet or a documented WeChat SDK. HIG gives no guidance on third-party SDK sharing, and the Share-button rule says not to build a competing alternative.
- Because export is long-running with per-photo completion, status feedback should be in-app and quiet, and a notification should be used only for failures (activity views page).

### Gaps
- HIG pages for "managing notifications", "file management" and "multi-selection" were not retrieved. The editing page returned 404.

## 5. Inputs, gestures, haptics

### Takeaway
Use standard gestures. Offer alternatives to every custom gesture. Haptics should complement and never replace visual feedback, and must be optional. Press-and-hold to compare original is a Photos convention, not a documented HIG rule.

### Cited Findings
- Standard gestures: Tap, Swipe, Drag, "Touch (or pinch) and hold — Reveal additional controls or functionality", Double tap ("Zoom in; zoom out if already zoomed in"), Zoom, Rotate. "Give people more than one way to interact with your app." "Use shortcut gestures to supplement standard gestures, not replace them." "Add custom gestures only when necessary" (discoverable, straightforward, distinct, not the only way). — [Gestures](https://developer.apple.com/design/human-interface-guidelines/gestures)
- Haptics: "Use system-provided haptic patterns according to their documented meanings." "Use haptics consistently." "Prefer using haptics to complement other feedback." "Avoid overusing haptics … the best haptic experience is one that people may not be conscious of, but miss when it's turned off." "prefer playing short haptics that complement discrete events." "Make haptics optional." Selection haptics "provide feedback while the values of a UI element are changing"; notification haptics: success, warning, error; impact: light, medium, heavy, rigid, soft. Standard sliders, toggles and pickers play system haptics by default. "Ensure that haptic vibrations don't disrupt experiences involving device features like the camera, gyroscope" — [Playing haptics](https://developer.apple.com/design/human-interface-guidelines/playing-haptics)
- WWDC18 Fluid Interfaces: "Everything needs to respond instantly." "The button should highlight immediately when I touch down on it" yet "we shouldn't confirm the tap until my touch goes up." "allow for constant redirection and interruption." "One-to-one tracking is extremely important. When swiping or dragging, the contents should stay attached to the gesture." Hysteresis "usually 10 points in iOS." "we recommend starting with 100% damping, or no overshoot" for elastic behaviors (80% damping for a swipe-dismiss in Music). — [WWDC18-803](https://developer.apple.com/videos/play/wwdc2018/803/)

### Inferences
- A slider drag should tick selection haptics at the neutral (0) point and at extremes, per "selection feedback while values change." Do not tick on every value.

### Gaps
- HIG gestures page does not list specific "press and hold to compare original". It is only the generic touch-and-hold row.

## 6. Apple Photos editing UX (iOS 18/26) as reference

### Takeaway
Photos' model is Auto first, then manual adjustment through a tool strip and a dial, with before/after compare, and always reversible. Sources on the compare gesture disagree.

### Cited Findings
- Edit, then the Adjust icon opens a horizontal tool strip. Auto is the first tool and lights up a dial; "Swipe the dial to change Auto's intensity. To return to the auto-tuned level, move it back to the white dot." The selected tool's icon displays a numeric value ring. Tool list: Exposure, Brilliance, Highlights, Shadows, Contrast, Brightness, Black Point, Saturation, Vibrance, Warmth, Tint, Sharpness, Definition, Noise Reduction, Vignette. "Tap Done to apply and save" — [MacRumors how-to (secondary source, iOS 13-era UI)](https://www.macrumors.com/how-to/how-to-use-the-editing-tools-in-photos/)
- Compare gesture: one secondary source says "Tap the edited image to see the original. Tapping again returns you to the edited version" — [iPhone Life](https://iphonelife.com/blog/32671/tip-day-press-and-hold-photo-while-editing-temporarily-see-original); contradicted by search-snippet summaries of other guides claiming press and hold — [search results via Gadget Hacks et al.](https://ios.gadgethacks.com/how-to/toggle-adjustments-your-iphones-photos-editor-compare-effects-before-after-0215285). This is unresolved, and Apple's own support page did not render.
- In iOS 26.5, one walkthrough reports Auto also moves other sliders such as Brilliance, which shows a ring (single secondary snippet, unverified) — [Tom's Guide](https://www.tomsguide.com/phones/iphones/im-not-a-photo-editor-but-with-the-photos-app-in-ios-26-and-these-6-tips-i-look-like-a-photoshop-pro)

### Inferences
- Support both tap-and-hold and a visible Before/After button in Dapple, which is consistent with the "more than one way" gesture rule and the "not the only way" context-menu rule.

### Gaps
- Apple's own iPhone User Guide editing page could not be read, so Photos behavior comes from secondary sources and is lower confidence. Not verified: iOS 26 Photos redesign details.

## 7. ML and generative AI guidance (automatic results, correction, privacy)

### Takeaway
Apple wants automatic results that are correctable, undoable and honest about limits. Corrections should be familiar and immediately rewarding, and you should never rely on corrections to fix low quality.

### Cited Findings
- "Request explicit feedback only when necessary." "Always make providing explicit feedback a voluntary task." "Act immediately when you receive explicit feedback and persist the resulting changes." — [Machine learning](https://developer.apple.com/design/human-interface-guidelines/machine-learning)
- Mistakes: "Make it easy for people to correct frequent or predictable mistakes." "When possible, address mistakes without complicating the UI." Corrections: "Give people familiar, easy ways to make corrections." "Provide immediate value when people make a correction." "Let people correct their corrections." "Never rely on corrections to make up for low-quality results." "When possible, use guided corrections instead of freeform corrections." — [Machine learning](https://developer.apple.com/design/human-interface-guidelines/machine-learning)
- Multiple options: "Prefer diverse options." "avoid providing too many options." "List the most likely option first." Confidence: "avoid showing results when confidence is low." Limitations: "Help people establish realistic expectations … Demonstrate how to get the best results." Privacy: "Always secure people's information"; "Regardless of the sensitivity of the data, all apps must protect user privacy at all times." — [Machine learning](https://developer.apple.com/design/human-interface-guidelines/machine-learning)
- Generative AI: "Keep people in control … revert or retry content transformations." "surfacing controls like Edit, Undo, Retry, or Adjust near generated content." "acknowledge when their corrections take effect." "Consider offering alternate versions of results." "Avoid automating destructive actions, like deleting photos." "Ensure a great experience even when generative features aren't available … non-AI fallback." "Communicate where your app uses AI." — [Generative AI](https://developer.apple.com/design/human-interface-guidelines/generative-ai)
- Essential Design Principles (WWDC17): "Every action taken in your app should provide some form of confirmation feedback." "Providing the ability for people to directly manipulate an object is more straightforward, it's more intuitive, and precise." "Too much feedback is annoying. Too much visibility is distracting. Too much progressive disclosure can make work flows inefficient." "Trying to get people to change their mental model of how your app works is risky." — [WWDC17-802](https://developer.apple.com/videos/play/wwdc2017/802/)

### Inferences
- The auto-develop (preset + per-photo correction) is a "proactive feature". HIG says to be especially careful about mistakes there. Show it as a non-destructive starting point (Auto level adjustable and removable) and never overwrite the original RAW.
- "Prefer diverse options, most likely first" fits a small number of candidate looks per photo, with the best auto result first.
- The ML page says nothing about on-device vs cloud processing choices. Privacy for RAW upload is a policy question outside HIG.

### Gaps
- WWDC25 "Make apps for everyone" and other accessibility session transcripts were not read. The principle names "deference, clarity, depth" (WWDC13/iOS 7 and 2017-era HIG) were not re-verified in a fetched Apple source in this research, so they are not quoted.

---

## Actionable design requirements (derived from HIG; "Inference" noted where it is my synthesis)

1. Use SwiftUI/UIKit standard components so Liquid Glass applies automatically; custom glass only on the few most important controls. — [Materials](https://developer.apple.com/design/human-interface-guidelines/materials)
2. Keep the photo (grid and editing canvas) in the content layer with no glass; reserve glass for the floating tab bar, toolbar and tool panel. — [Materials](https://developer.apple.com/design/human-interface-guidelines/materials), [WWDC25-219](https://developer.apple.com/videos/play/wwdc2025/219/)
3. Never stack glass on glass (e.g. a glass chip inside a glass toolbar). — [WWDC25-219](https://developer.apple.com/videos/play/wwdc2025/219/)
4. Use regular glass for text-heavy chrome; use clear glass only for controls over the photo, and add a ~35% dark dim layer when the photo underneath is bright. — [Materials](https://developer.apple.com/design/human-interface-guidelines/materials)
5. Replace solid toolbar background fills with a soft scroll edge effect, one per view. — [Layout](https://developer.apple.com/design/human-interface-guidelines/layout), [WWDC25-356](https://developer.apple.com/videos/play/wwdc2025/356/)
6. Extend the photo or grid beneath bars to the screen edge (background extension for letterboxed images). — [Layout](https://developer.apple.com/design/human-interface-guidelines/layout)
7. Keep tab bar and toolbar tint monochrome by default; one accent color, used only for the primary action (e.g. "Share/Post" or "Done"). Color belongs in the photo. — [Color](https://developer.apple.com/design/human-interface-guidelines/color), [Tab bars](https://developer.apple.com/design/human-interface-guidelines/tab-bars), [WWDC25-219](https://developer.apple.com/videos/play/wwdc2025/219/)
8. Tab bar with 5 or fewer single-word, filled-symbol tabs, none hidden or disabled; no screen-specific actions in it. — [Tab bars](https://developer.apple.com/design/human-interface-guidelines/tab-bars), [WWDC25-356](https://developer.apple.com/videos/play/wwdc2025/356/)
9. At most one prominent button (two max) per view; place it at the trailing edge; style, not size, shows preference. — [Buttons](https://developer.apple.com/design/human-interface-guidelines/buttons), [Toolbars](https://developer.apple.com/design/human-interface-guidelines/toolbars)
10. Group toolbar items by function and frequency, max 3 groups; never group a text button next to an icon button. — [Toolbars](https://developer.apple.com/design/human-interface-guidelines/toolbars), [WWDC25-356](https://developer.apple.com/videos/play/wwdc2025/356/)
11. Default to the light appearance via semantic colors, but supply dark and increased-contrast variants for every custom color. Do not add an in-app appearance toggle (Inference: if the design is deliberately light-only, document it as a deviation from "respect system appearance"). — [Dark Mode](https://developer.apple.com/design/human-interface-guidelines/dark-mode), [Color](https://developer.apple.com/design/human-interface-guidelines/color)
12. Meet 4.5:1 contrast for text up to 17 pt (3:1 at 18 pt or bold), target 7:1 for custom small text; check over glass and photos in both appearances. — [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility), [Dark Mode](https://developer.apple.com/design/human-interface-guidelines/dark-mode)
13. Never encode state by color alone (selected, auto-applied, error); add a checkmark, shape or label. — [Color](https://developer.apple.com/design/human-interface-guidelines/color), [Toggles](https://developer.apple.com/design/human-interface-guidelines/toggles)
14. Use text styles from the system scale (Body 17, Footnote 13, Caption2 11 minimum); avoid Light/Thin weights; test with enlarged Dynamic Type including 200%. — [Typography](https://developer.apple.com/design/human-interface-guidelines/typography), [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility)
15. Every tappable control at least 44x44 pt, with about 12 pt padding around bezeled controls and about 24 pt for unbezeled ones. — [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility)
16. Layout by size class, not device. Compact phone screens use a tab bar. Wider windows (inner Find N5 display, iPad) should offer a sidebar or split view with the same functionality. (Foldable specifics are Inference; HIG covers only size classes.) — [Layout](https://developer.apple.com/design/human-interface-guidelines/layout)
17. Use a standard grid for the library, with no layout changes while the user is interacting unless triggered by an explicit action. — [Collections](https://developer.apple.com/design/human-interface-guidelines/collections)
18. Multi-select: provide an explicit Select mode with a non-color selection indicator and a persistent count; the share and save actions live in the toolbar. (Inference from Apple conventions; no HIG multi-select page was retrieved.) — [Collections](https://developer.apple.com/design/human-interface-guidelines/collections), [Toolbars](https://developer.apple.com/design/human-interface-guidelines/toolbars)
19. Present the adjustment panel as a resizable sheet (medium/large) with a grabber. Prefer a nonmodal sheet so the photo stays live, and support swipe-to-dismiss with confirmation if edits are unsaved. — [Sheets](https://developer.apple.com/design/human-interface-guidelines/sheets)
20. Keep edit tasks short and modal flows single-path with an obvious dismiss. — [Modality](https://developer.apple.com/design/human-interface-guidelines/modality)
21. Sliders: min on the leading side, max on the trailing side; live preview as the thumb moves; show the numeric value with a way to reset; an endpoint icon helps for ambiguous parameters; add tick marks or a labeled neutral where useful. — [Sliders](https://developer.apple.com/design/human-interface-guidelines/sliders)
22. Do not exceed about 5 segments on iPhone (e.g. looks, or Before/After); use all text or all icons within one control. — [Segmented controls](https://developer.apple.com/design/human-interface-guidelines/segmented-controls)
23. Outside a list row, toggles (e.g. "Auto on/off") are state-changing buttons with visible fill changes, not switches; switches are for Settings rows only. — [Toggles](https://developer.apple.com/design/human-interface-guidelines/toggles)
24. Every context-menu action (e.g. on a long press of a thumbnail) must also exist in visible UI; a small menu, about 3 groups at most; destructive items last. — [Context menus](https://developer.apple.com/design/human-interface-guidelines/context-menus)
25. Press-and-hold to compare with the original may be added as a shortcut, but always pair it with a visible Before/After control. (Photos' own behavior is disputed in sources, see section 6.) — [Gestures](https://developer.apple.com/design/human-interface-guidelines/gestures)
26. Non-destructive, multi-level undo/redo plus "Revert to original/auto" for everything; show what was undone; place any undo/redo buttons in the toolbar with the standard symbols; do not hijack shake or three-finger gestures. — [Undo and redo](https://developer.apple.com/design/human-interface-guidelines/undo-and-redo)
27. Treat the automatic correction as an adjustable starting point: expose its strength, let people remove it, and acknowledge when a correction takes effect. Never rely on user corrections to hide poor auto results. — [Machine learning](https://developer.apple.com/design/human-interface-guidelines/machine-learning), [Generative AI](https://developer.apple.com/design/human-interface-guidelines/generative-ai)
28. Offer a small set of ordered alternatives (most likely first) instead of many options; use guided corrections (sliders, presets) over freeform. — [Machine learning](https://developer.apple.com/design/human-interface-guidelines/machine-learning)
29. Never auto-delete or overwrite originals (RAW/JPEG pairs); confirm before irreversible actions. — [Generative AI](https://developer.apple.com/design/human-interface-guidelines/generative-ai)
30. Disclose AI-based processing where used and explain limitations (e.g. when auto correction may be poor), with voluntary, low-friction feedback only. — [Generative AI](https://developer.apple.com/design/human-interface-guidelines/generative-ai), [Machine learning](https://developer.apple.com/design/human-interface-guidelines/machine-learning)
31. Batch processing: determinate progress ("12 of 40") in a consistent location; switch from indeterminate to determinate as soon as the count is known; paced evenly; Cancel available; no vague "Loading". — [Progress indicators](https://developer.apple.com/design/human-interface-guidelines/progress-indicators)
32. Show thumbnails immediately with placeholders and let users browse and edit while the rest develops in the background. — [Loading](https://developer.apple.com/design/human-interface-guidelines/loading)
33. Use a spinner for per-thumbnail or per-button background work, and a progress activity indicator inside a button for non-instant actions such as "Saving…". — [Progress indicators](https://developer.apple.com/design/human-interface-guidelines/progress-indicators), [Buttons](https://developer.apple.com/design/human-interface-guidelines/buttons)
34. Launch instantly, restore the last position and selection, a launch screen matching the first screen with no text or branding, and no splash or tutorial before use. — [Launching](https://developer.apple.com/design/human-interface-guidelines/launching)
35. Onboarding fast and optional: interactive tips (TipKit) in context instead of a tutorial; request camera-transfer, local network and photo-library permissions only when first needed, with a rationale; ship a good default look so no setup is required. — [Onboarding](https://developer.apple.com/design/human-interface-guidelines/onboarding), [Settings](https://developer.apple.com/design/human-interface-guidelines/settings)
36. Sharing: use the system Share button and activity view; add app-specific actions (e.g. "Post to Moments") as custom activities with short verb titles; a single path, not a competing share UI. — [Activity views](https://developer.apple.com/design/human-interface-guidelines/activity-views)
37. Show export status in-app; notify only for failures, not for completion. — [Activity views](https://developer.apple.com/design/human-interface-guidelines/activity-views), [Feedback](https://developer.apple.com/design/human-interface-guidelines/feedback)
38. Keep settings minimal; put task-specific options (look, strength) inside the editing screen, with only rare options in Settings. — [Settings](https://developer.apple.com/design/human-interface-guidelines/settings)
39. Use motion sparingly and briefly; never block input on an animation; honor Reduce Motion by replacing movement with fades and avoiding blur-in/out animations. — [Motion](https://developer.apple.com/design/human-interface-guidelines/motion), [Accessibility](https://developer.apple.com/design/human-interface-guidelines/accessibility)
40. Gestures: the photo must follow the finger one-to-one; respond on touch-down; allow interruption and redirection; use the standard pinch, double-tap zoom and swipe. Provide alternatives to every custom gesture and make haptics short, consistent and optional (selection haptic while dragging values, success haptic when a batch export completes). — [WWDC18-803](https://developer.apple.com/videos/play/wwdc2018/803/), [Gestures](https://developer.apple.com/design/human-interface-guidelines/gestures), [Playing haptics](https://developer.apple.com/design/human-interface-guidelines/playing-haptics)
