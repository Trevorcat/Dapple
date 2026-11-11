# Reference photo apps: gap-fill pass (queue/progress, foldable layouts, Chinese flows, preset browsing, sentiment)

Research date: 2026-11-11. Method: ~19 tool calls (searches plus 7 page fetches). Builds on `reference_apps.md` and does not repeat it. Adobe learn page, Apple support page, ifeng and MacRumors fetched OK. Reddit, DPReview, AndroidAuthority and the Redlib mirror were blocked (403) or never returned by search. No real Reddit thread content was retrieved, so Q5 rests on forums and blogs. Tags: [F] sourced fact, [I] inference, [U] unverified. Search-summary-only claims are noted.

## 1. Background queue and progress UI patterns

### Takeaway
Mainstream apps converge on three layers: a per-item icon (queued, uploading ring, done check), one global status line with a count of remaining items, and a colored health indicator for paused/blocked states with a reason. No camera-companion app's progress UI could be sourced. Sony documents only background notifications and resumable/differential transfer.

### Cited Findings
- Google Photos phases are "No backup, Preparing backup, Backing up, Backup complete, Backup paused". Progress is counted as items remaining. Per-item: queued or paused items show a "will eventually" icon, a slashed cloud means not backed up and not pending. (Search summary of the help page.) — [Google Photos help](https://support.google.com/photos/answer/6193313)
- Google Photos per-item icon: cloud with up arrow = waiting to upload, then a circular progress ring that fills, then a checked cloud. (Third-party description, not Google.) — [Guiding Tech](https://www.guidingtech.com/what-is-cloud-symbol-in-google-photos/)
- Apple Photos shows iCloud status at the profile screen below name and photo count (iOS 18+), or at the bottom of All Photos on iOS 17 and earlier. A green/yellow/red dot sits beside the message. Messages give the reason for pause: Low Data Mode, Low Power Mode, Optimizing Battery Power, Poor Network Connection, Low Battery, "Needs to Cool Down", storage full. Where offered, a "Sync Now" action resumes. The page does not say which colour maps to which message. [F] — [Apple Support 101559](https://support.apple.com/en-mz/101559)
- Apple "Updating" can persist for hours or days per user threads, i.e. no ETA is shown. (Anecdotal.) — [Apple discussions](https://discussions.apple.com/thread/255918610)
- Lightroom mobile: tap the cloud icon in the top bar to see progress under an "Importing From Camera Roll" heading. Imported photos go into an upload queue, originals upload afterward. Signing out mid-import can drop queued in-app-camera captures. Expired trial shows a yellow warning on the cloud icon, "Syncing is disabled". Uploads can get stuck with a progress bar that stops advancing (fix: force-quit). [F] — [Adobe: upload or download stuck](https://helpx.adobe.com/lightroom/mobile/troubleshoot/app-launch-and-update-issues/upload-or-download-stuck.html); [Adobe: sign-out message](https://helpx.adobe.com/lightroom-classic/kb/lightroom-mobile-sign-out-message.html)
- Lightroom Classic users asked in an Ideas thread for a list of which images are syncing in each direction, i.e. count-only status is felt as insufficient. (2024 thread, title only seen.) — [Adobe Ideas](https://community.adobe.com/t5/lightroom-classic-ideas/p-provide-list-of-images-that-are-being-synced-in-each-direction-in-lrc-itself/idc-p/14651040)
- Sony Creators' App: on Android the Bluetooth location-linkage and "transfer notification" features keep running in the background and can be turned off under Others > Transfer notification settings. Sony says transfers can resume after interruption and can send only not-yet-transferred files. Whether the transfer screen shows thumbnails per file was not documented in the results. [F] — [Sony support CCCA99018](https://www.sony.co.th/electronics/support/articles/CCCA99018)
- Fujifilm XApp: one App Store reviewer says transfers only occasionally happen automatically and that you cannot keep shooting while transferring; common fix is re-selecting the phone in the camera's transfer mode. (Review summaries.) — [App Store XApp](https://apps.apple.com/au/app/fujifilm-xapp/id1586089681); [jmpeltier](https://www.jmpeltier.com/fujifilm-cam-remote-app-not-transferring-photos/)
- Cascable: v6.7.1 note says RAW and JPEG of a RAW+JPEG shot are now viewed separately rather than as one. A reviewer says that on import it previews its own rendering of the RAW. Dropped connections beyond ~5 m on an A7 III. (Store listing summaries, older.) — [Cascable App Store](https://apps.apple.com/gb/app/cascable/id974193500)
- ShutterSnitch: no progress-UI detail found beyond the earlier note.

### Inferences
- Dapple needs a four-state per-photo badge: arriving (transfer ring), developing (spinner), ready (nothing or tiny check), failed/needs attention (dot). Google's ring-to-check is the closest proven idiom [I].
- A single global line "N arriving, M developing" plus a tappable reason when paused (Apple-style reason text and Sync Now) is better than Lightroom's cloud menu that hides inside a top-bar icon [I].
- RAW+JPEG as a single stack with a RAW-pending sub-state avoids Cascable's separated pairs [I].

### Gaps
- No source found on Fujifilm XApp, ShutterSnitch or Cascable per-item progress visuals, or Sony's actual progress notification content.
- Android foreground-service notification patterns for camera apps not retrieved.
- Lightroom mobile per-thumbnail badges not verified.

## 2. Foldable and large-screen layouts

### Takeaway
Lightroom on Galaxy Z Fold is repeatedly reported (2022-era threads, some newer) as opening in a tablet layout whose panel is too big, with users asking for a phone mode and a flex-mode split (photo on top half, sliders on bottom half). No source describes OPPO's own gallery editor on Find N. Google Photos' large-screen editor split (preview one side, tools other side) was reported as a 2023 leak only.

### Cited Findings
- Adobe community: users report Lightroom defaults to tablet UI on Z Fold 3, with a side panel so large photos look tiny. Workaround of launching on the cover screen then unfolding reverts to tablet mode. [F, user reports, 2021-2022 threads, status of fix unknown] — [Adobe: tablet UI on Fold3](https://community.adobe.com/questions-680/lightroom-mobile-is-defaulting-to-tablet-ui-on-samsung-fold3-922587); [phone mode request](https://community.adobe.com/feature-requests-681/p-open-lightroom-in-phone-mode-instead-of-tablet-mode-on-samsung-z-fold-661483)
- Adobe community feature request: Flex mode with sliders on the bottom half and the photo on the top half. Status unknown. [F request exists] — [Adobe: flex mode request](https://community.adobe.com/feature-requests-681/p-give-us-a-flex-mode-option-for-the-fold-661707)
- Another request asks for the editing panel along the horizontal bottom edge. — [Adobe request](https://community.adobe.com/feature-requests-681/p-editing-panel-at-the-horizontal-bottom-edge-661374)
- July 2026 community post: HDR editing shows "HDR Unavailable" on unfolded Z Fold7, an officially supported device. — [Adobe community](https://community.adobe.com/questions-680/hdr-editing-unavailable-on-galaxy-z-fold7-officially-supported-device-same-issue-on-both-play-store-and-galaxy-store-builds-1634241)
- Samsung developer guidance recommends 50:50 multi-pane on Z Fold and triple-pane on landscape tablets. (Guidelines, not Lightroom behaviour.) — [Samsung developers](https://developer.samsung.com/one-ui/foldable-and-largescreen/intro.html)
- Google Photos editor for large screens: May 2023 reports (Android Central) of a layout dividing the screen into preview and tools side by side, replacing a stretched phone layout; rollout was unconfirmed. Not tied to tabletop. — [Android Central](https://androidcentral.com/apps-software/google-photos-editing-interface-tablets-large-screens)
- Pixel Fold reviewer says editing in Lightroom on the inner screen is "just as good as I imagined" (2023 blog, opinion). — [Kowatek](https://blog.kowatek.com/2023/07/14/editing-photos-on-googles-pixel-fold-has-totally-sold-me-on-the-form-factor/)
- Find N5: inner 8.12" 2480x2248 120 Hz, outer 6.62". Several split-screen entry methods (two-finger swipe down, smart sidebar drag, "..." menu). (Official spec page; split methods from a user-contributed pconline answer.) — [OPPO CN specs](https://www.oppo.com/cn/smartphones/series-find-n/find-n5/specs/); [pconline](https://www.pconline.com.cn/ask/16659.html)
- sspai review notes OPPO's "全景虚拟屏" since Find N3 and that N5 is smoother; no gallery-editor detail seen in the snippet. — [sspai 97162](https://sspai.com/post/97162)
- Nothing sourced on Snapseed or VSCO foldable layouts, or Apple iPad Photos two-pane.

### Inferences
- Inner screen near 1:1 aspect (2480x2248) means a landscape-style two-pane works only with a square-ish preview; a bottom-docked panel (as users requested) keeps the image big [I].
- The 50:50 Samsung guidance suggests grid left / preview right on Find N5 unfolded, and tabletop (half-fold) with image top and sliders bottom, which matches the Adobe flex-mode request [I].
- Lightroom's tablet-layout complaint suggests don't just scale a tablet layout: test the near-square inner display separately [I].

### Gaps
- OPPO Gallery (ColorOS 相册) editor layout on Find N5: no source.
- Snapseed, VSCO, Google Photos current (2025-26) foldable layouts: no source.
- Whether Adobe shipped a flex mode: unknown.

## 3. Chinese apps' concrete flows

### Takeaway
Little step-by-step flow detail could be retrieved. Evidence shows 醒图 moving towards photographer batch workflows (batch presets, RAW), and ColorOS investing in AI gallery tools and system-level share to 小红书/微博/抖音 for Live Photos. Tap counts to share to WeChat/小红书/抖音 were not found for any app.

### Cited Findings
- 醒图专业版 (desktop), debuted at ChinaJoy 2026 per ifeng (6 Aug 2026): save a retouch setting and apply it to a whole set in one step, RAW import and high-res export, "当天拍完、当天返图" same-day delivery, one-click removal of passersby, 1,200 layerable effects. Desktop focus; photographers reportedly handle larger batches on desktop than phone. [F] — [ifeng](https://tech.ifeng.com/c/8vNDKoqCLom)
- 醒图 App Store updates mention AI 造型室, AI 写真, 一键出片 effect and a 收藏滤镜 (favorite filters) feature; template sharing generates an image or link so others can import the same retouch look. (Store text; search summary.) — [醒图 App Store CN](https://apps.apple.com/cn/app/%E9%86%92%E5%9B%BE-%E6%8B%8D%E7%85%A7-%E4%BF%AE%E5%9B%BE-%E4%BF%AElive%E7%A5%9E%E5%99%A8/id1500526240)
- 醒图 free export reportedly adds a watermark and the guide recommends 1080p for social (low-reliability tool guide). — [sj.qq.com](https://sj.qq.com/blog/1774608857908)
- ColorOS 15 Live Photo sharing to 小红书, 微博, 抖音 (system sharing; 小红书 and 美图秀秀 listed, 抖音 adaptation in progress at one point), per ITHome. [F] — [ITHome 818488](https://www.ithome.com/0/818/488.htm); [ITHome 867848](https://www.ithome.com/0/867/848.htm)
- ColorOS 15 gallery AI: AI Clarity Enhancer, Reflection Remover, Eraser, Unblur, Perfect Shot (swap faces across shots), Group Photo Composer. ColorOS 16 adds more AI to the Photos app. [F] — [ChannelNews](https://www.channelnews.com.au/oppo-rolls-out-global-coloros-15-update-with-new-ai-upgrades-and-camera-tools/); [OPPO newsroom](https://www.oppo.com/in/newsroom/stories/how-coloros16-brings-smarter-more-adaptive-experiences-to-oppo-smartphones/)
- Polarr/泼辣修图: filters are fully editable and custom filters can be saved to favorites (older review; app version unspecified). Team interview from ifanr exists but was not read. — [ifanr interview](https://www.ifanr.com/app/1228879)
- Nothing found for 黄油相机, Lightroom China usage, "一键成片" in ColorOS 相册 (the term did not surface), or WeChat share button steps.

### Inferences
- 醒图 Pro's "apply one saved look to the whole set, deliver same day" is the closest Chinese competitor positioning to Dapple's auto-develop, but on desktop; a phone-first, camera-arrival flow remains open [I].
- System share sheet (ColorOS) already covers 小红书/抖音/微博 targets, so Dapple may rely on Android share intents instead of per-platform SDKs for v1 [I].

### Gaps
- Step counts export-to-WeChat/小红书/抖音 in 醒图, 美图秀秀, ColorOS 相册; 黄油相机 flows; "智能推荐滤镜" in ColorOS 相册; Lightroom usage in China.

## 4. Preset/look browsing details

### Takeaway
Lightroom mobile's panel is three tabs with the user's own photo analyzed to generate thumbnails; VSCO uses a grid with Favorites/Recent/For This Photo organization (older source). Apple's filter strip is a horizontal swipe with a dial below the selected filter; thumbnail type and count were not verifiable.

### Cited Findings
- Lightroom mobile preset panel (opened from the overlapping-circles icon in Edit): tabs Recommended, Premium, Yours. Recommended suggests settings from similar photos shared in the Community and is grouped (Subtle, Strong, B&W); tap the three dots on a thumbnail to "Save to your presets". Premium has Adaptive presets (Portrait, Subject, Blur Background) at the top. Yours has "User Presets" and "Saved from Community" sections; create from the three-dot menu. Thumbnails are generated by analyzing your image, which can take longer for mask-based presets (so thumbnails appear to be the user's photo; page implies, does not state outright). Tapping the applied thumbnail shows the Amount slider (range not stated here; a search summary of another Adobe page said up to 200). Favorites and visible count not stated. [F] — [Adobe learn](https://www.adobe.com/learn/lightroom-cc/web/use-presets-in-lightroom-mobile)
- VSCO preset view: grid with up to three previews per line; categories Favorites, Recent, For This Photo, Warm, Cool, Vibrant, B&W (older review, version unknown). 200+ presets, full library needs membership. [F, dated] — [Gridfiti via search](https://gridfiti.com/?p=15500); [VSCO App Store](https://apps.apple.com/us/app/vsco-photo-editor-presets/id588013838)
- VSCO Recipes: saved combination of edits and their amounts; crop and straighten are excluded; saved by tapping "+" after editing; managed in Organizer; each recipe shown with a photo thumbnail. [F, from search summaries] — [PhoneArena](https://www.phonearena.com/news/Photo-editing-app-VSCO-now-lets-users-save-their-own-recipes_id99692); [Android Authority (403 on fetch)](https://www.androidauthority.com/save-favorite-photo-editing-recipes-vsco-813828)
- Apple Photos filters (2019 source, mechanism assumed unchanged): swipe along the strip, tap previews on the photo, a dial under the selected filter adjusts intensity. Screenshots unreadable, so thumbnail type/count not confirmed. iOS 18 reportedly condensed filters into a single menu (reviewer remark, not detailed). — [MacRumors 2019](https://www.macrumors.com/how-to/adjust-filters-photos-ios/); search summary of iOS 18 coverage at [MacRumors iOS 18 guide](https://macrumors.com/guide/ios-18-photos)
- 醒图 has a 收藏滤镜 (favorites) feature per store text (see Q3).

### Inferences
- VSCO's "For This Photo" and Lightroom's analysis-based Recommended both pre-rank looks per photo; Dapple's per-photo auto correction could expose "closest alternatives to my look" the same way [I].
- Recipes that exclude crop/straighten is the right boundary for a "look" that auto-applies to arrivals [I].

### Gaps
- Visible thumbnail count in any app; whether Apple thumbnails use the user's photo (background knowledge says yes, unsourced [U]); VSCO's current 2025-26 strip layout and favorites gesture.

## 5. User sentiment on speed and friction

### Takeaway
Complaints cluster on: slow/unreliable wireless camera-to-phone transfer, dropped connections and re-pairing, quality loss on the way to Instagram, and Lightroom sync slowness. Many users give up on apps for a card reader or cable. No Reddit threads were retrieved.

### Cited Findings
- Alpha Shooters forum (Sony A7 IV owner): Creators' App documentation is a mess, update "purgatory"; one user abandoned Sony software for a small card reader to the phone, "reliable and very fast". Phone may say it doesn't support the device and offer to format. [F, forum, undated] — [Alpha Shooters thread](https://www.alphashooters.com/community/threads/the-hell-that-is-creators-app-update-purgatory-perhaps.7925/post-49460)
- Lightroom mobile to Instagram: quality loss reports; slow sync (1,500 photos took a day, one user); "Copy to Instagram" option reported removed. (Old threads, around 2017-2020; dated, use with caution.) — [Adobe community](https://community.adobe.com/t5/lightroom-classic-discussions/camera-to-lightroom-mobile-to-instagram-degraded-quality/m-p/9050386); [Lightroom Killer Tips](https://lightroomkillertips.com/lightroom-mobile-instagram-best-way-well-right-way-anyway/)
- Fuji XApp: connecting is the biggest issue per a Fuji X Weekly writer; transfers block shooting; for Instagram, enable Resize to Small since Instagram downsizes anyway. [F, dated 2022-2024] — [Fuji X Weekly tag](https://fujixweekly.com/tag/cam-remote-app/); [jmpeltier](https://www.jmpeltier.com/fujifilm-xapp-transfer-raw-files/)
- Canon forum threads ask for "fast wireless transfer to a social media account" on EOS R5 (titles only seen). — [Canon community](https://community.usa.canon.com/t5/EOS-DSLR-Mirrorless-Cameras/EOS-R5-Fast-Wireless-File-Transfer-to-a-Social-Media-Account/m-p/515138)
- A guide warns people delete the card after "importing" only to find the phone saved a low-res preview. — [Discover Cameras](https://discovercameras.com/how-to-transfer-photos-from-camera-to-phone/)
- Cable is called fastest/most reliable, Bluetooth only for handshake/GPS tagging (vendor blogs; one blog's view). — [Gadget Hacks](https://android.gadgethacks.com/how-to/fastest-way-transfer-photos-videos-from-your-dslr-your-android-0177420)

### Inferences
- The "preview vs original" trap means Dapple must state clearly what has arrived (preview vs full RAW) so users don't format cards prematurely [I].
- Because users see Instagram recompress anyway, offering export presets by platform with pre-sized output saves a manual resize step [I].

### Gaps
- Real Reddit threads (r/AdobeLightroom, r/SonyAlpha, r/fujifilm, r/photography) not retrievable; Chinese user sentiment (知乎, 小红书) not retrieved.

## Steal this / avoid this (new, not in earlier note)

1. STEAL: Per-photo icon sequence of waiting, filling progress ring, then check, as Google Photos does. [F + I] — [Guiding Tech](https://www.guidingtech.com/what-is-cloud-symbol-in-google-photos/)
2. STEAL: One global status line that counts remaining items plus phases (preparing, working, complete, paused). [F] — [Google help](https://support.google.com/photos/answer/6193313)
3. STEAL: When blocked, state the reason in words with a Sync Now style action (Apple: low battery, needs to cool down, poor network). [F] — [Apple Support](https://support.apple.com/en-mz/101559)
4. AVOID: Apple's colour dot with no documented colour meaning and open-ended "Updating" with no ETA; show counts and a clear "stuck" state. [F + I] — [Apple Support](https://support.apple.com/en-mz/101559); [Apple thread](https://discussions.apple.com/thread/255918610)
5. AVOID: Hiding queue state inside a top-bar cloud icon menu, and queue loss on sign-out/close (Lightroom). Keep the queue persistent and survive restarts. [F + I] — [Adobe sign-out](https://helpx.adobe.com/lightroom-classic/kb/lightroom-mobile-sign-out-message.html)
6. STEAL: Silence-able background transfer notification with an explicit setting (Sony Others > Transfer notification settings), plus resumable and differential transfers. [F] — [Sony support](https://www.sony.co.th/electronics/support/articles/CCCA99018)
7. AVOID: Transfers that block shooting or only sometimes auto-start (XApp review). Make arrival fully background and show it started. [F, review] — [App Store XApp](https://apps.apple.com/au/app/fujifilm-xapp/id1586089681)
8. AVOID: Splitting a RAW+JPEG shot into two separate items (Cascable v6.7.1). Keep one stack with RAW-pending state. [F + I] — [Cascable](https://apps.apple.com/gb/app/cascable/id974193500)
9. STEAL (foldable): Support a half-folded layout with image on top and controls on the bottom, which Lightroom users explicitly requested, and a phone/compact option so the inner screen does not get a tablet-sized sidebar. [F requests + I] — [Adobe flex-mode request](https://community.adobe.com/feature-requests-681/p-give-us-a-flex-mode-option-for-the-fold-661707); [tablet UI on Fold3](https://community.adobe.com/questions-680/lightroom-mobile-is-defaulting-to-tablet-ui-on-samsung-fold3-922587)
10. STEAL: Use 50:50 pane split guidance on unfolded (grid left, preview right) and test the near-square 8.12" panel. [F guideline + I] — [Samsung](https://developer.samsung.com/one-ui/foldable-and-largescreen/intro.html); [OPPO specs](https://www.oppo.com/cn/smartphones/series-find-n/find-n5/specs/)
11. STEAL: Look saved as a recipe that includes amounts but excludes crop/straighten, shown with a thumbnail of the photo it came from (VSCO). [F, via summaries] — [PhoneArena](https://www.phonearena.com/news/Photo-editing-app-VSCO-now-lets-users-save-their-own-recipes_id99692)
12. STEAL: Lightroom-style tabs separating "Yours" (saved looks) from suggested ones, with "save to your presets" in an item menu; and a "For This Photo" ranking row as in VSCO. [F + I] — [Adobe learn](https://www.adobe.com/learn/lightroom-cc/web/use-presets-in-lightroom-mobile)
13. STEAL: 醒图 Pro's positioning of one saved look batch-applied to a set for same-day delivery, as the proven Chinese demand signal; and shareable looks as an image or link (醒图 template sharing). [F] — [ifeng](https://tech.ifeng.com/c/8vNDKoqCLom); [醒图 App Store](https://apps.apple.com/cn/app/%E9%86%92%E5%9B%BE-%E6%8B%8D%E7%85%A7-%E4%BF%AE%E5%9B%BE-%E4%BF%AElive%E7%A5%9E%E5%99%A8/id1500526240)
14. STEAL: Rely on the Android system share sheet for 小红书/抖音/微博 (ColorOS already integrates them for Live Photos) and export with platform-specific presets; Instagram and WeChat recompress anyway. [F + I] — [ITHome](https://www.ithome.com/0/818/488.htm); [jmpeltier](https://www.jmpeltier.com/fujifilm-xapp-transfer-raw-files/)
15. AVOID: Letting users believe an "imported" low-res preview is the original; label preview vs original on each photo before they clear the card. [F + I] — [Discover Cameras](https://discovercameras.com/how-to-transfer-photos-from-camera-to-phone/)

Open items before design lock: real Reddit sentiment; OPPO Gallery editor on Find N5 (hands-on); WeChat/小红书/抖音 share step counts; Apple and VSCO strip thumbnail details (hands-on check on a device is faster than further searching).
