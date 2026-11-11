# Reference photo apps: interaction patterns for Dapple (automation-first photo app)

Research date: 2026-11-11. Method: ~17 web searches; the two direct page fetches (Adobe help, DPReview) returned HTTP 403, so most findings rest on search-result summaries, not full-page reads. Anything marked [UNVERIFIED] or "from my background knowledge" has no URL and should not be cited as fact. Facts and impressions are labeled.

## 1. How is the automatic result presented, and how is strength adjusted or undone?

### Takeaway
Auto is shown as the first or default tool in most editors, with an intensity control and an easy return to the "auto" point (Apple dial centre; Google tap-again to remove). Lightroom's newer AI features (Adaptive presets, Quick actions) create editable masks. No reference app fully "develops on arrival with a chosen look". That is Dapple's differentiator, so there is no direct pattern to copy.

### Cited Findings
- Apple Photos: Auto is the first tool in the Adjust strip; its dial raises or lowers the effect, and returning to the middle white dot restores the auto-tuned level. This is from an iOS 13-era guide; the iOS 26 layout is reported to be largely unchanged from iOS 18. (Fact, older source.) — [MacRumors editing how-to](https://www.macrumors.com/how-to/how-to-use-the-editing-tools-in-photos/); [AppleInsider iOS 26 vs 18](https://appleinsider.com/inside/ios-26/vs/photos-ios-26-vs-ios-18-small-changes-huge-impact)
- Apple Photos iOS 26: tabs are Adjust, Filters, Crop, Markup plus AI tools; slider animation is reported as smoother with real-time updates. — [AppleInsider](https://appleinsider.com/inside/ios-26/tips/inside-photos-in-ios-26-macos-26----refinements-in-apples-image-and-video-management-tool)
- Apple Photos known issue: a support thread reports that the yellow/default line on sliders such as exposure can be misleading in iOS 26 (unresolved in the result). — [Apple Support thread](https://discussions.apple.com/thread/256182797)
- Google Photos (May-June 2025 redesign): editor opens to an "Auto" category replacing "Suggestions"; AI Enhance generates multiple results; tapping an applied suggestion again removes it. Rollout to Android June 2025, iOS "later this year". — [TechCrunch 2025-05-28](https://techcrunch.com/2025/05/28/google-photos-debuts-redesigned-editor-with-new-ai-tools); [AlternativeTo](https://alternativeto.net/news/2025/5/google-photos-revamps-editor-with-ai-suggestions-device-exclusives-and-sharing-upgrades); [Google help](https://support.google.com/photos/answer/6128850)
- Google Photos "Help me edit" is a conversational (describe-the-edit) option via Gemini, expanded to all US Android users (Sept 2025). Sources are news, not Google docs. — [9to5Google](https://9to5google.com/2025/09/23/google-photos-help-me-edit/)
- Lightroom mobile Quick actions: AI analyzes the photo and suggests targeted enhancements; presets applied this way create masks that remain editable in the Masking panel; now on iPad. — [Adobe preset help (regional page, search summary)](https://helpx.adobe.com/lightroom-cc/using/presets-lightroom-ios.html); [Quick actions help](https://helpx.adobe.com/id_en/lightroom/mobile/apply-quick-actions/apply-presets-to-photos-using-quick-actions.html)
- Lightroom Adaptive presets: content-aware, under Premium (Landscape, Portrait, Subject, Sky); apply to a specific area of the scene; later seasonal Landscape options and 8 new Adaptive Landscape presets. — [Adobe: use presets in Lightroom mobile](https://www.adobe.com/learn/lightroom-cc/web/use-presets-in-lightroom-mobile); [Release notes (updated Sept 2026)](https://helpx.adobe.com/ee/lightroom/mobile/whats-new/adobe-lightroom-on-mobile-release-notes.html)
- Lightroom "Recommended presets": analyzes exposure, colour and content and suggests edit settings used on similar photos shared in Community; release notes say the section became "Trending" (exact release not confirmed). — same Adobe sources as above.
- VSCO: tapping a preset again opens its strength control; Pro presets add contrast/colour/tone sliders. VSCO presets for Lightroom now natively support the strength slider (Feb 2026). — [VSCO getting started](https://www.vsco.co/learn/getting-started-vsco); [VSCO Pro Presets](https://www.vsco.co/features/photo-filters/pro-presets); [PetaPixel 2026-02-12](https://petapixel.com/2026/02/12/vsco-brings-back-beloved-film-inspired-lightroom-presets)
- VSCO Capture (iOS, 2025): 50+ live presets with adjustable bloom/halation/grain; video recording added Dec 2025. — [PetaPixel 2025-12-31](https://petapixel.com/2025/12/31/vsco-wraps-up-2025-by-adding-video-recording-to-its-iphone-camera-app)
- 美图秀秀 pushes "美图配方" and "一键美容" to cut user operation time; 醒图 markets "一键美颜" and templates (set a tone template, then tweak). Based on a search summary of competitive-analysis pieces. — [人人都是产品经理 评测](https://www.woshipm.com/evaluating/5183886.html); [竞品分析 美图 vs 醒图](https://www.woshipm.com/evaluating/4438372.html); [醒图 App Store CN](https://apps.apple.com/cn/app/%E9%86%92%E5%9B%BE-%E6%8B%8D%E7%85%A7-%E4%BF%AE%E5%9B%BE-%E4%BF%AElive%E7%A5%9E%E5%99%A8/id1500526240)

### Inferences
- Two undo idioms recur: "dial returns to a visible auto centre" (Apple) and "tap the active item again to remove" (Google, VSCO). Both need zero extra buttons.
- Lightroom's "AI result becomes ordinary editable masks/sliders" is the model for keeping auto results tweakable without a separate mode.
- Dapple's default-applied look plus per-photo correction should still expose a single "auto amount" slider and a one-tap revert to camera original; no reference app shows both at once.

### Gaps
- Exact Lightroom default behaviour on import (is Auto applied by default?) not verified; Adobe help pages returned 403.
- No source found on whether Lightroom's Auto strength is adjustable as a single slider.
- Xingtu/Meitu "一键" flows (tap counts, whether strength is shown) not verified beyond store/summary text.

## 2. Preset browsing: thumbnails, count, favorites, categories

### Takeaway
Lightroom uses categorized preset groups (Premium/Adaptive, Trending, user presets) and VSCO a preset strip with tap-again strength. Specific facts on thumbnail type and visible count were not retrievable.

### Cited Findings
- Lightroom: presets are grouped; Premium group is refreshed with every release; Adaptive sub-groups are Landscape, Portrait, Subject, Sky. — [Adobe learn page](https://www.adobe.com/learn/lightroom-cc/web/use-presets-in-lightroom-mobile)
- Fuji recipe culture: recipes are shared as parameter lists (film sim, grain, colour chrome, WB, tone) via sites and apps; the Fuji X Weekly app can send recipes to a camera but with limits (ISO and typical exposure compensation not transferred, video recipes excluded, errors possible, and it is marked as needing a camera protocol). — [Fuji X Weekly 2026-08-20](https://fujixweekly.com/2026/08/20/new-send-recipes-to-your-camera-directly-from-the-fuji-x-weekly-app/); [Notebookcheck](https://www.notebookcheck.net/Developer-makes-one-tap-Fuji-recipe-transfers-from-phone-to-camera-possible.1040911.0.html)
- Third-party recipe apps: one App Store reviewer called a $15 film.recipes app mostly a web list without auto-loading (impression). — [App Store id6741566252](https://apps.apple.com/app/id6741566252)

### Inferences
- Fuji photographers already think in "recipe names" (e.g., Reggie's Superia on Fuji X Weekly); showing a look as a name + parameter summary on the photographer's own frame is consistent with that culture.

### Gaps
- Whether Lightroom/VSCO/Xingtu thumbnails are generated from the user's own photo, visible count per screen, favorites mechanism: not verified (from background knowledge Lightroom mobile shows the open photo in preset thumbnails and has Favorites, but no source retrieved, treat as [UNVERIFIED]).
- Fujifilm film-simulation presentation inside XApp: no source found.

## 3. Before/after: hold vs split vs toggle

### Takeaway
Press-and-hold is the dominant phone idiom (Snapseed, Google Photos); Apple uses tap on image/toggle.

### Cited Findings
- Snapseed: tap and hold within a Tool or Filter shows the before image; a compare icon at top right does the same when held. — [Snapseed help](https://support.google.com/snapseed/answer/6157827)
- Google Photos: tap and hold the image preview to compare with original. — [Google Photos help](https://support.google.com/photos/answer/6128850)
- Apple Photos: tapping the image (or the adjustment button) shows the edit against the original; general behavior, not iOS 26-specific. — [Gadget Hacks](https://ios.gadgethacks.com/how-to/toggle-adjustments-your-iphones-photos-editor-compare-effects-before-after-0215285)

### Inferences
- For multi-select grids, hold-to-compare on the thumbnail itself would be a natural extension (no source; design inference).

### Gaps
- Lightroom mobile's compare gesture and VSCO's not verified (background: Lightroom uses a toggle/hold on the image; unsourced).

## 4. Control placement, one-handed use, foldable/tablet layouts

### Takeaway
Snapseed's vertical-select/horizontal-adjust gesture lets the whole image act as the slider so thumbs never leave the lower screen; sources on foldable layouts were not found.

### Cited Findings
- Snapseed: swipe vertically anywhere on the image to select a parameter, horizontally to change its value; one reviewer notes it is hard to hit exact values with horizontal swipes. — [Snapseed guides via search: PhotoWorkout](https://www.photoworkout.com/?p=132476); [Mobiography tutorial](https://www.mobiography.net/tutorials/snapseed-app-tutorial/); [DPReview Snapseed review](https://www.dpreview.com/reviews/app-review-snapseed-android)
- Apple Photos uses a horizontal dial under the image for both Auto/adjustments and filter intensity. — [MacRumors](https://www.macrumors.com/how-to/adjust-filters-photos-ios/)

### Inferences
- A bottom-thumb dial or swipe-anywhere gesture suits one-handed use on a 7.82" unfolded or 6.4" cover screen; unfolded layout could place grid left, preview+dials right (design inference, no source).

### Gaps
- No source on Lightroom/Photos/Phocus Mobile foldable or tablet layouts. Phocus Mobile 2 is iPad/iPhone with a new Android version (2026) but layouts not described. — [PetaPixel 2026-07-16](https://petapixel.com/2026/07/16/hasselblad-phocus-mobile-finally-arrives-on-android/)

## 5. Batch: copy/paste settings, queue visibility, progress

### Takeaway
Lightroom mobile's batch is long-press to enter selection, then Copy/Paste Settings with group choice. It is functional but multi-step and has documented slowness and gaps.

### Cited Findings
- Copy: open edited photo, three-dot menu, Copy Settings, choose setting groups (chevron to select individual settings; "All" option). Paste to many: in grid, long-press an image to enter selection mode, tap others, then Paste Settings. — [Adobe: Copy and paste edits](https://helpx.adobe.com/lightroom/mobile/share-save-and-export/copy-and-paste-edits-to-photos.html); [bwillcreative](https://www.bwillcreative.com/?p=69432)
- Limits: settings copy only for photos in the catalog, not Device view; Auto Settings reportedly cannot be batch-copied on Android (workaround: auto one photo, copy, paste); on iPad a user reported pasted Denoise not actually processed. (Community reports.) — [Adobe community: batch auto settings](https://community.adobe.com/t5/lightroom-ecosystem-cloud-based-discussions/mobile-app-batch-auto-settings/td-p/13927918); [iPad denoise thread](https://community.adobe.com/questions-680/ipad-denoise-is-not-actually-applied-when-pasting-settings-to-multiple-photos-1637515)
- Speed: July 2025 post says batch edit of 5 photos on iPad Pro M4 took almost a minute to sync, faster on Android. — [Adobe community](https://community.adobe.com/t5/lightroom-ecosystem-cloud-based-discussions/extremely-slow-batch-edit-on-ipad-compared-to-android/m-p/15479406)
- Bug thread (July 2025): with sync active, applying Adaptive presets is slow and the app unresponsive (up to a minute lag; "auto" tone showed same lag); lag disappears with sync off. — [Adobe community bug report](https://community.adobe.com/bug-reports-674/p-with-sync-active-adaptive-presets-are-slow-to-apply-and-become-unresponsive-664349)
- Sony Creators' App v2.6.0 (Feb 2025): "differential" transfer sends only unsent files on new connections; direct upload to Google Drive or Lightroom. — [PetaPixel 2025-02-04](https://petapixel.com/2025/02/04/sony-cameras-can-now-directly-transfer-images-to-google-drive-and-adobe-lightroom)
- Phocus Mobile 2: one user reports RAW batch processing works on iPad (absent in desktop Phocus); single forum comment. — [Fred Miranda](https://www.fredmiranda.com/forum/topic/1915956/)

### Inferences
- Users hit sync-engine and cloud-roundtrip cost; a local-first pipeline with visible per-photo status would avoid the cited failure mode.

### Gaps
- No source describing queue/progress UI in any app.
- No source on ShutterSnitch/Cascable batch UI.

## 6. Export / share (including multiple photos to WeChat Moments)

### Takeaway
Little app-specific data was found. WeChat constraints are the key design input.

### Cited Findings
- WeChat iOS 8.0.18 (Jan 2022) allows >9 photos in a Moments post (up to 20); more than 9 are combined into a slideshow-style video. — [ITHome](https://www.ithome.com/0/600/690.htm) (as summarized by search; verify against the page)
- WeChat 8.0.27 raised chat sending to 99 images and shows original-image size when sending. — same ITHome result (search summary; low confidence).
- Claims that WeChat compresses images twice, or numbers like "15%-40% size" without original: unverified personal/unreliable (pconline) — do not use.

### Inferences
- Because WeChat recompresses, exporting to the system share sheet/album at a fixed sensible size (long edge and quality chosen in advance) and enabling "original" toggle in WeChat is likely the practical path (design inference, needs testing).

### Gaps
- Actual WeChat Moments image limits for Android 2025-26, recompression behavior, and the step count for multi-photo share from Lightroom/Xingtu/Meitu: not found. Xingtu/Meitu direct "share to WeChat Moments" button conventions: not verified.
- Lightroom export size/quality options: not retrieved (Adobe 403).

## 7. Camera-companion apps and pain points

### Takeaway
Transfer reliability and setup friction are the dominant complaints; Sony's app is rated mixed. Evidence is mostly app-store and forum anecdotes.

### Cited Findings
- Sony Creators' App: reviewers report wireless transfer stalling or very slow, Bluetooth pairing failures on iPhone 15 Pro Max, incompatibility messages on some bodies, and bounced support emails. Sony's own support page for the A9 III acknowledges unstable connection and slow transfer and suggests switching the camera to 5 GHz Wi-Fi. Many complaints are from the older Imaging Edge Mobile era. — [Sony support](https://www.sony.ua/electronics/support/e-mount-body-ilce-9-series/ilce-9m3/articles/CCCA99015); [App Store reviews mirror](https://justuseapp.com/en/app/1616622856/creators-app/reviews); [Sony community thread](https://community.sony.de/t5/cyber-shot-compact-cameras/imaging-edge-app/m-p/3967775)
- Aggregator marlvel.ai reports 4.16/5 over ~10,000 reviews with connection stability the top complaint (unverified methodology). — [marlvel.ai](https://marlvel.ai/apps/sony-creators-app/reviews)
- Creators' App supports α7R V per Newsshooter list; v4.3 later added 3-second Motion Photo transfers. — [Newsshooter](https://www.newsshooter.com/?p=262983); [Digital Camera World](https://www.digitalcameraworld.com/tech/firmware/now-you-can-create-ultra-short-form-videos-on-your-sony-camera-as-3-second-motion-photos-come-to-creators-app)
- Fujifilm XApp: JPEG-only transfer was the early complaint; RAW transfer added in v2.1.0, X-H2S and GFX100 II first; later guide says X-T50, X100VI, X-S20 supported. A forum user reports full-size auto transfer of JPEG+RAW failing as the connection drops, working only with smartphone-resize on (undated). Early review: Bluetooth did not reconnect in background. — [App Store](https://apps.apple.com/au/app/fujifilm-xapp/id1586089681); [jmpeltier guide](https://www.jmpeltier.com/fujifilm-xapp-transfer-raw-files/); [Fuji X Forum](https://www.fuji-x-forum.com/topic/17343-fuji-remote-app-image-transfer-failure); [CineD 2023](https://cined.com/?p=293666)
- ShutterSnitch: after each shot is saved, it is sent to the app and shown full screen; App Store listing warns most of the setup lies outside the app; subscription $2.99/month or $29.99/year; reviews generally positive. Cascable Pro: RAW+JPEG download, Wi-Fi/USB import, $4.99/month or $29.99/year. — [CreativePro](https://creativepro.com/?p=123606); [ShutterSnitch App Store](https://apps.apple.com/us/app/id364176211); [Cascable App Store](https://apps.apple.com/gb/app/cascable/id974193500)
- Phocus Mobile 2: remote control + wireless tethering + RAW transfer on iOS, HDR display, Android version launched 2026 without phone tethering. A Fstoppers comment found Phocus too complex vs Leica workflow (single anecdote). — [Hasselblad](https://www.hasselblad.com/phocus-mobile); [Hasselblad press 2026](https://master.hasselblad.com/press/press-releases/2026/hasselblad-brings-phocus-mobile-to-android); [Fstoppers](https://fstoppers.com/gear/wireless-tethering-hasselblad-x2d-and-phocus-mobile-2-711600)
- Lightroom mobile pain: sync slow or stuck (Aug-Oct 2025 threads), crop tool not smooth, slow iPad uploads. — [Adobe community sync stuck](https://community.adobe.com/t5/lightroom-ecosystem-cloud-based-discussions/sync-stuck-and-not-working-ios-app-stuck-too/m-p/15549538); [crop not smooth](https://community.adobe.com/t5/lightroom-ecosystem-cloud-based-discussions/crop-tool-is-not-smooth/m-p/15279486/highlight/true)

### Inferences
- Wireless transfer stability on the A7R V (large RAW, ~60MP) and X-T50 is the real risk to the "shoot fast" promise; Dapple may need transfer of a JPEG/preview first, RAW later (design inference supported by the Fuji resize workaround).

### Gaps
- Leica FOTOS: no data found.
- No Reddit, 小红书 or 少数派 user-complaint content was retrieved (the search tool returned none); Chinese user sentiment is a gap.
- No A7R V-specific Creators' App review from DPReview/PetaPixel found.
- No 2025-26 Fujifilm XApp complaint dates verified.

## (a) Patterns x apps (Y = sourced above; ~ = partially/unclear; ? = not verified; blank = not applicable)

| Pattern | Lightroom mobile | Apple Photos | Google Photos | VSCO | Snapseed | Xingtu / Meitu | Sony Creators' / XApp / SS / Cascable / Phocus |
|---|---|---|---|---|---|---|---|
| Auto as first/default tool | ? | Y (Auto first) | Y (Auto tab) | ? | ? | ~ (一键 features) | no editing automation found |
| AI/adaptive suggestions | Y (Adaptive, Quick actions) | ~ | Y (AI Enhance, Help me edit) | no | no | ~ | Phocus: HDR display only |
| Strength control | ? | Y (dial) | ? | Y (tap preset again) | n/a | ? | n/a |
| Undo by tap-again | ? | dial centre | Y | Y | n/a | ? | n/a |
| Press-and-hold compare | ? | tap | Y | ? | Y | ? | n/a |
| Vertical select / horizontal adjust gesture | no | dial only | no | no | Y | no | n/a |
| Batch copy/paste | Y (multi-step) | ? | ? | ? | no | ? | Phocus iPad RAW batch (1 comment) |
| Local-first / fast | complaints (sync) | n/a | n/a | n/a | n/a | n/a | transfer-reliability complaints |
| Auto transfer from camera | Sony->Lightroom direct upload | n/a | n/a | n/a | n/a | n/a | Y (Creators' differential transfer, XApp auto transfer, ShutterSnitch auto display) |

## (b) Steal this / avoid this

Evidence tags: [S] sourced fact; [I] inference from sources.

1. STEAL: tap-and-hold the image to see the original. Used by Snapseed and Google Photos; no UI chrome. [S] [Snapseed](https://support.google.com/snapseed/answer/6157827), [Google](https://support.google.com/photos/answer/6128850)
2. STEAL: tap an applied item again to remove it (Google suggestions, VSCO strength). [S] [Google](https://support.google.com/photos/answer/6128850), [VSCO](https://www.vsco.co/learn/getting-started-vsco)
3. STEAL: a dial/slider with a visible neutral centre for "auto amount", as in Apple's Auto dial. [S] [MacRumors](https://www.macrumors.com/how-to/how-to-use-the-editing-tools-in-photos/)
4. STEAL: vertical-select, horizontal-adjust on the image itself for the few sliders (Snapseed), but add snap points/detents because exact values are hard. [S + I] [DPReview](https://www.dpreview.com/reviews/app-review-snapseed-android)
5. STEAL: AI results that resolve into ordinary editable adjustments/masks (Lightroom Quick actions). [S] [Adobe](https://helpx.adobe.com/id_en/lightroom/mobile/apply-quick-actions/apply-presets-to-photos-using-quick-actions.html)
6. STEAL: differential transfer ("only unsent files") so reconnecting never re-downloads. [S] [PetaPixel](https://petapixel.com/2025/02/04/sony-cameras-can-now-directly-transfer-images-to-google-drive-and-adobe-lightroom)
7. STEAL: ShutterSnitch's "show each new shot full screen on arrival" as the review moment before editing. [S] [CreativePro](https://creativepro.com/?p=123606)
8. STEAL: let the user choose a look once and show its name with a parameter summary, matching Fuji recipe culture. [I] [Fuji X Weekly](https://fujixweekly.com/2026/08/20/new-send-recipes-to-your-camera-directly-from-the-fuji-x-weekly-app/)
9. STEAL: Google's single "Auto" entry point with several candidate results to cycle through rather than many menus. [S + I] [TechCrunch](https://techcrunch.com/2025/05/28/google-photos-debuts-redesigned-editor-with-new-ai-tools)
10. STEAL: long-press a thumbnail to enter multi-select, then tap others (Lightroom grid). [S] [Adobe](https://helpx.adobe.com/lightroom/mobile/share-save-and-export/copy-and-paste-edits-to-photos.html)
11. STEAL: "copy settings" with group choice, but expose it as one "sync look/tweaks to selected" action. [S + I] same source
12. AVOID: cloud-roundtrip sync in the edit loop; documented minute-long lag applying presets and stuck "Waiting for settings". [S] [Adobe bug report](https://community.adobe.com/bug-reports-674/p-with-sync-active-adaptive-presets-are-slow-to-apply-and-become-unresponsive-664349), [stuck sync](https://community.adobe.com/t5/lightroom-ecosystem-cloud-based-discussions/sync-stuck-and-not-working-ios-app-stuck-too/m-p/15549538)
13. AVOID: auto settings that cannot be batch-applied (Lightroom Android workaround). Dapple's core is batch auto. [S] [Adobe community](https://community.adobe.com/t5/lightroom-ecosystem-cloud-based-discussions/mobile-app-batch-auto-settings/td-p/13927918)
14. AVOID: silent partial paste (iPad Denoise pasted but not processed); show per-photo status. [S] [Adobe community](https://community.adobe.com/questions-680/ipad-denoise-is-not-actually-applied-when-pasting-settings-to-multiple-photos-1637515)
15. AVOID: fragile always-on Bluetooth/Wi-Fi sessions with no recovery; show connection state, auto-retry, fall back to lower-res preview (XApp resize workaround). [S + I] [Fuji X Forum](https://www.fuji-x-forum.com/topic/17343-fuji-remote-app-image-transfer-failure), [Sony support](https://www.sony.ua/electronics/support/e-mount-body-ilce-9-series/ilce-9m3/articles/CCCA99015)
16. AVOID: transferring RAW+JPEG at full size by default over Wi-Fi; transfer JPEG/embedded preview first, RAW later. [I from the same Fuji thread]
17. AVOID: setup that lives outside the app (ShutterSnitch listing admits it); guide pairing in-app. [S] [ShutterSnitch App Store](https://apps.apple.com/us/app/id364176211)
18. AVOID: tying an app to a specific camera model list that lags (XApp RAW support rolled out per model). [S] [App Store](https://apps.apple.com/au/app/fujifilm-xapp/id1586089681)
19. AVOID: misleading default markers on sliders (iOS 26 exposure line report); make the "auto value" mark accurate. [S, single thread] [Apple Support](https://discussions.apple.com/thread/256182797)
20. AVOID: paid-subscription gating of core looks as the main preset experience (VSCO Pro sliders, Lightroom Premium). Impression, not sourced as a complaint; treat as a business-model question. [VSCO](https://www.vsco.co/features/photo-filters/pro-presets)
21. STEAL (WeChat): design the share step around WeChat's limits (up to 20 photos, >9 become a slideshow video on iOS; unconfirmed for Android) and pre-select export size; needs verification. [S, low confidence] [ITHome](https://www.ithome.com/0/600/690.htm)
22. STEAL: a small "look strength" tap-again control and a "Trending/Recommended" row only if it's quick; Lightroom renamed Recommended to Trending, suggesting social-based recommendations are not the same as per-photo fit. [S + I] [Adobe release notes](https://helpx.adobe.com/ee/lightroom/mobile/whats-new/adobe-lightroom-on-mobile-release-notes.html)

Items needing further research before design lock: Lightroom/VSCO/Xingtu preset-thumbnail and favorites behaviour; WeChat Moments Android limits and recompression; Chinese-user sentiment (小红书/少数派); foldable layouts; Lightroom export size/quality options.
