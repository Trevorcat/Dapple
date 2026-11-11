# WeChat Moments / Chinese social sharing from a third-party app (Dapple share step)

Research date: 2026-11-11. Budget-limited pass (~13 tool calls). Official WeChat docs were fetched only for the Android SDK image page; other items rely on dated community posts and are flagged. Several WebFetch/search results were summarized by a tool, not read verbatim.

## 1. WeChat Open SDK: can a third-party app share to Moments (WXSceneTimeline)?

### Takeaway
The official SDK shares ONE image per call to Moments (WXImageObject; no multi-image object is documented). Multi-image (up to 9) to Moments from a third-party app is only reported via the unofficial Android system-share Intent path, with unreliable, anecdotal reports of restrictions since WeChat 7.0. For a 9-photo workflow the dependable route is "save to the system album, then open WeChat and pick".

### Cited Findings
- Android SDK doc (WXImageObject, scene = WXSceneTimeline): `imageData` must not exceed 1MB; `imagePath` (local file) must not exceed 25MB; thumbnail `thumbData` must not exceed 32KB; all examples send a single image; no multi-image option is described; no image-specific Moments restrictions or review requirement listed (fetch was summarized by a tool, so verify wording). FileProvider is recommended for file paths on Android 7.0+. — [WeChat Open Platform Android share doc](https://developers.weixin.qq.com/doc/oplatform/Mobile_App/Share_and_Favorites/Android.html)
- The same doc says the optional mini program entrance on a shared image needs OpenSDK 6.8.30 and WeChat client 8.0.48+. — [same doc](https://developers.weixin.qq.com/doc/oplatform/Mobile_App/Share_and_Favorites/Android.html)
- The doc also lists signature fields (imgDataHash, thumbDataHash, msgSignature). Sharing through the SDK requires registering a mobile app AppID on the Open Platform (general requirement; this pass did not find the review specifics for image sharing). — [same doc](https://developers.weixin.qq.com/doc/oplatform/Mobile_App/Share_and_Favorites/Android.html)
- Community report: the official `imageData` limit is documented as 10MB but a developer found sharing failed above ~500KB; no official confirmation (the reply asked for logs and suggested imagePath). Use imagePath. (Anecdotal; the doc text above now says 1MB / 25MB, so the community post is probably from an older doc version.) — [WeChat community](https://developers.weixin.qq.com/community/develop/doc/0006225b1749c81b25bac22e15f400)
- Unofficial Android system-share path: `ComponentName("com.tencent.mm","com.tencent.mm.ui.tools.ShareToTimeLineUI")`, `ACTION_SEND_MULTIPLE`, `image/*`, `EXTRA_STREAM` URI list, caption via the obfuscated extra `Kdescription`; content:// (FileProvider) URIs needed on Android 7+. These are 2017–2023 blog posts; WeChat class names are obfuscated and may change. — [Aliyun dev article](https://developer.aliyun.com/article/676802), [Juejin 2023](https://juejin.cn/post/7232127712642891834)
- Anecdotal: one blog says WeChat 7.0 tightened multi-image share to Moments (single image only) and another says the "9 image" share was disabled, advising to save locally and let users share manually; the author calls it an inference. No official statement found. — [51cto blog](https://blog.51cto.com/u_13521/7541274), [CSDN](https://blog.csdn.net/okg0111/article/details/86498186)
- Anecdotal: with uni-app/DCloud, scene WXSceneTimeline sometimes opens the friends list instead of Moments, varying by device and version. — [DCloud Q&A](https://ask.dcloud.net.cn/question/134347)
- HarmonyOS: a forum answer says WeChat provides no native HarmonyOS SDK and recommends the system share capability; DCloud's docs say HBuilderX 4.87 and older fail on Harmony when sharing images over 20KB. A Share Kit article (2025) describes a unified share API with 20+ channels. All low-reliability, no official WeChat Harmony doc found. — [DCloud uni.share](https://doc.dcloud.net.cn/uni-app-x/api/share.html), [itying forum](https://bbs.itying.com/topic/68f97f4289629100479db6e3), [InfoQ](https://xie.infoq.cn/article/15ef588fbc3a43c1b4ca7c0ef)
- Live Photo: OPPO said ColorOS 15 supports Live Photo sharing to Weibo, Xiaohongshu, Douyin but not WeChat Moments; it depends on third-party adaptation with no concrete plan. (Date of the statement not verified; check the article.) — [ITHome](https://www.ithome.com/0/818/488.htm)

### Inferences
- Because the official path is single-image, a "post 9 photos to Moments in one tap" promise cannot be built on the documented SDK. Realistic options: (a) save all to a dedicated album and deep-link/launch WeChat (`weixin://`), (b) the unofficial ShareToTimeLineUI multi-image Intent on Android with a fallback, (c) system share sheet.
- Maintaining ShareToTimeLineUI risks breaking on WeChat updates; it should be feature-detected with fallback to option (a).

### Gaps
- Whether the SDK path still opens the Moments editor with one image on current WeChat (8.0.6x+), and whether multi-image Intent works in late 2026: untested, no dated 2025–2026 source found.
- iOS share extension / Universal Link specifics, AppID review requirements, and a 2024–2026 changelog for WeChat Open SDK: not found.
- Official HarmonyOS WeChat share docs: not found.

## 2. WeChat image compression (Moments & chat), export settings, color

### Takeaway
Moments got visibly sharper in WeChat 8.0.64 (Oct 2025); a user test (unofficial) puts the long edge cap at 2560px, up from ~1080. Tencent says clarity depends on original quality, screen and phone and that images are still compressed. Export sRGB; no WeChat-specific color-handling test was found.

### Cited Findings
- After WeChat 8.0.64, Moments photos and Live Photos look sharper but "not true originals"; a user measured the long edge cap rising from 1080 to 2560px. This is one user's test, not an official spec. Tencent support said clarity depends on original image quality, screen size and phone model, with compression to save data; a higher-clarity setting keeps closer to original but may use more memory. Article dated 2025-10-11. — [Digitaling](https://www.digitaling.com/articles/1411486.html)
- Chat "original image": Android WeChat 8.0.71+ reportedly supports sending/viewing up to 200MP originals in stickers/chat; iOS not yet at time of that report (year not confirmed). — [gamegeeker](https://gamegeeker.com/zh/news/wechat-sticker-supports-200mp-original-image-sharing)
- Metadata: a Chinese article states compressed Moments images drop GPS etc.; only originals keep it. WeChat "原图" may keep metadata (forum view, unconfirmed). — [i21st](https://www.i21st.cn/story/print_3803.html), [V2EX](https://www.v2ex.com/t/1075483)
- Color (not WeChat-specific): Display P3 files look dull or wrong in apps without color management; sRGB is the safest for social platforms. — [Sked Social help](https://help.skedsocial.com/en/articles/5879163-my-photos-look-less-vibrant-when-posting-to-instagram-about-p3-colour-profiles-and-web-friendly-formats), [Adobe community](https://community.adobe.com/t5/lightroom-ecosystem-cloud-based-discussions/lr-mobile-export-to-camera-roll-photos-ios-colors-are-washed/m-p/15234559/highlight/true)

### Inferences
- Export JPEG sRGB with long edge about 2560px, quality ~85–92 (my recommendation, not sourced). Going beyond 2560 gives no visible gain if the cap holds, and risks WeChat's re-encode. Mild output sharpening is a plausible mitigation, but no source tested it.
- HDR/Gain-map/P3 outputs should be tone-mapped to plain sRGB for the share path.

### Gaps
- No controlled test of Moments recompression (JPEG quality, chroma, sharpening effect) found; no confirmation of how WeChat treats embedded ICC/P3 profiles.
- The 2560px figure needs a real test on iOS vs Android; no official number.

## 3. Moments layout conventions, Xiaohongshu ratios

### Takeaway
Moments multi-image thumbnails are square-cropped; a single image preview keeps its ratio only within a limited ratio range (2016 test). Xiaohongshu recommends 3:4 portrait, and a note uses one ratio only.

### Cited Findings
- 2016 test: a single image keeps its ratio in the preview when width/height is 1–2.5; beyond ~2.5:1 the thumbnail ratio changes; thumbnails are produced by cropping to a square. Old, may have changed. — [CSDN 2016](https://blog.youkuaiyun.com/u011551941/article/details/50408927)
- Nine-grid posts are commonly made by cropping to 1:1 and splitting one picture into 9 tiles posted together. — [ifanr](https://www.ifanr.com/app/1128699), [51cto](https://blog.51cto.com/python3/2950897)
- Moments cover: 1080x1080 (1:1), the bottom-right can be covered by the avatar. (Canva, not official.) — [Canva](https://www.canva.cn/learn/wechat-circle-of-friends-cover-size/)
- Xiaohongshu: three picture ratios, 3:4 (recommended, bigger in feed), 1:1, 4:3; one ratio per note; 1200x1600 is a common 3:4 size (third-party, no official page found). — [Canva note cover](https://www.canva.cn/learn/xiaohongshu-note-cover/), [woshipm](https://www.woshipm.com/marketing/5572030.html), [CSDN](https://adg.csdn.net/6a31168e662f9a54cb7fcdad.html)

### Inferences
- Mixed-ratio 9-grids get center-cropped into squares in the grid, so offering a crop preview ("how it looks in the grid") or a square-safe composition hint helps.
- For Xiaohongshu, a batch must use one ratio; offer 3:4 default.

### Gaps
- Current (2025–2026) single-image display rules and long image behavior: no dated source found. Posting conventions of photographers (EXIF captions, watermarks, ordering of "cover first"): no source found.

## 4. "Export to album then pick in WeChat": ordering, album, privacy, naming

### Takeaway
Little citable material was found; it is mostly engineering inference.

### Cited Findings
- Found no source on WeChat picker ordering rules ("most recent first" etc.) — search returned nothing relevant.
- EXIF GPS can leak location; the practice is to strip GPS before sharing. — [i21st](https://www.i21st.cn/story/print_3803.html)

### Inferences
- Picker normally sorts by date added/taken, so write images in the intended reverse order or set the modification/taken time (to be tested on target devices); in a grid the selection order, not the album order, defines position. Unverified.
- Save into a dedicated album (e.g. Pictures/Dapple on Android via MediaStore RELATIVE_PATH) so WeChat's album switcher gives one-tap access. Strip GPS (and serial/lens info) by default in the share export; keep a toggle.

### Gaps
- WeChat picker sorting, album switching and sequence-number naming effects: untested.

## 5. OPPO / ColorOS share panel

### Takeaway
No ColorOS-specific documentation for Moments in the share panel was found. Only the Live Photo limitation was found.

### Cited Findings
- ColorOS 15: Live Photo sharing to WeChat Moments not supported (per OPPO statement). — [ITHome](https://www.ithome.com/0/818/488.htm) (also see [ITHome 815/040](https://www.ithome.com/0/815/040.htm), not verified)

### Gaps
- Behavior of Android Sharesheet / ColorOS panel for WeChat targets on Find N5 (ordering, "Moments" entry visibility, large-screen layout): not found; needs a device test.

## 6. UX requirements for Dapple's share step (derived)

Items marked [S] have a source above; [I] are inference/to verify.

1. Primary action "Save to album + open WeChat" must work for 1–9 photos, because the official SDK is single-image. [S: Android SDK doc]
2. Offer "Share to Moments" via SDK for single photo only; label multi-select as "9 photos: save then pick in WeChat". [S/I]
3. Treat the ShareToTimeLineUI multi-image Intent as an optional, feature-detected enhancement with automatic fallback to (1); never depend on it. [S: anecdotal blogs]
4. Pass images by FileProvider content:// URI/imagePath (≤25MB), never byte arrays (>1MB fails). [S]
5. Keep any SDK thumbnail ≤32KB. [S]
6. Default export: JPEG, sRGB, long edge ~2560px, quality ~85–92; no P3/HDR in share path. [S for 2560 (unofficial) and sRGB; I for quality]
7. Don't upscale small crops above 2560 and avoid heavy output sharpening; test mild sharpening against WeChat 8.0.64+ recompression. [I]
8. Write into a dedicated "Dapple" album via MediaStore (Android) / album (iOS) so it is one tap in WeChat's picker. [I]
9. Order the saved files so the intended grid order is easy: write in reverse order or set timestamps so the picker's newest-first sort shows photo 1 on top; verify on device. [I]
10. Strip GPS and device identifiers by default; surface a visible "location removed" indicator and a toggle. [S: privacy article]
11. Show a Moments grid preview that square-crops multi-image thumbnails; warn when ratios will be center-cropped, and flag ratios > 2.5:1 for single images. [S 2016, dated]
12. Offer a 3:4 (1200x1600+) preset for Xiaohongshu with one consistent ratio per batch; 1:1 / 4:3 alternatives. [S: third party]
13. Do not promise Live Photo/motion sharing to Moments on ColorOS. [S]
14. After saving, show a short toast with a deep link to open WeChat and the count of images saved ("9 saved, open WeChat > Moments > pick from Dapple album"). [I]
15. Provide a "send as original" hint: for chat, WeChat 原图 may preserve metadata, so Dapple's stripped export should be what the user picks. [S unconfirmed]
16. On HarmonyOS, plan on system share / album route; do not rely on a WeChat SDK. [S: forum]
17. Version-check WeChat and log failures; keep a remote-config kill switch for the Intent path. [I]
18. Include a first-run device test of share targets on OPPO Find N5 (foldable layouts) before shipping. [Gap]
