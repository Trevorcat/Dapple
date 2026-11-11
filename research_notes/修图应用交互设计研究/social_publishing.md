# Multi-platform social publishing for Dapple (platform-agnostic publish step)

Research date: 2026-11-11. Limitation: web search was weak on official Chinese platform docs (open.xiaohongshu.com, open.douyin.com, open.weibo.com, Weibo/Kuaishou/Bilibili/QQ空间/Lofter/图虫 help centres were not retrievable). Most platform specs below come from third-party guides and are marked [3P]. Anything unmarked and uncited is not in the notes. Items I could not source are in Gaps. Everything flagged "verify" must be tested on device before it is hard-coded.

## 1. Per-platform image specs and conventions

### Takeaway
Only a few limits are confirmed: Instagram and Threads carousels hold up to 20 items, X holds 4 images, and 小红书 offers 3:4, 1:1 and 4:3 with one ratio per note (per secondary sources). Everything else (pixels, byte limits, compression behaviour) is anecdotal or missing. Design the preset table as data (remote-updatable), not hard-coded logic, and use 3:4 as the cross-platform default master.

### Cited Findings
- 小红书: pure image notes recommend three ratios, 3:4 portrait, 1:1 square and 4:3 landscape. One note supports only one ratio, and mixing causes cropping. 3:4 is most recommended for feed real estate. [3P, aggregated by search tool, original woshipm article] — [Woshipm](https://www.woshipm.com/marketing/5572030.html), [Canva CN](https://www.canva.cn/learn/xiaohongshu-note-cover/)
- 小红书: a blog claims max 18 images and 750x1000 px suggested. I found no official confirmation; flagged anecdotal. — [Canva CN](https://www.canva.cn/learn/xiaohongshu-note-cover/) (search summary; claim source unclear)
- 小红书: one user test (少数派) says the largest static image size XHS supports is 1280x1706, that files over roughly 10 MB get compressed, and that animated webp only worked at 720x960 or 800x800. Personal test, not official. — [少数派](https://sspai.com/post/82686)
- 微博: the "超9图" feature (up to 18 images) launched Oct 2019 for the full-feature client, initially only for accounts with more than 100k followers. Current limits and 原图 rules were not found; this source is old. — [36Kr](https://www.36kr.com/p/1724495757313)
- Instagram carousel: app allows up to 20 photos/videos per carousel; all slides should share one shape. Scheduling tools and ads often cap at 10. [3P] — [Flowgent](https://flowgent.ai/blog/instagram-post-size), [AdMake](https://admakeai.com/blog/instagram-post-size)
- Instagram grid: the profile grid moved to 3:4 previews in 2025 (credited to Adam Mosseri). Guides disagree on the best upload ratio. Some recommend 1080x1440 (3:4) to fill the grid edge to edge; others recommend 1080x1350 (4:5) and say the grid crops the sides. [3P] — [Neal Schaffer](https://nealschaffer.com/instagram-post-size/), [Inmagic](https://inmagic.ai/instagram-post-size/), [Oktopost](https://www.oktopost.com/blog/instagram-changed-grid-layout-heres-everything-know-far/)
- Instagram API: a vendor (RecurPost, updated Sep 2026) says the publishing API accepts a ratio range of 4:5 to 1.91:1, so a native 3:4 (0.75) upload is rejected via the API, while manual in-app upload of 3:4 works. Vendor claim, not Meta docs; relevant only if Dapple ever publishes via the Graph API. — [RecurPost](https://recurpost.com/?p=162471)
- Threads: Meta docs say a carousel holds 2 to 20 images, videos or a mix; the 20 limit was announced in 2024. 100 MB per image is a third-party figure, unverified. — [Meta Threads docs](https://developers.facebook.com/documentation/threads/posts.md), [Social Media Today](https://www.socialmediatoday.com/news/threads-users-can-now-add-up-to-20-photos-andor-videos-per-post/727452/)
- X: max 4 images per post; 5 MB per image on mobile, 15 MB on web [3P]; four-image layout is a 2x2 grid with 2:1 cells and crops images; 1200x675 (16:9) is the usual recommended single-image size. Images over 5 MB may be recompressed; Premium accounts may see less compression (vendor claim). No official X doc retrieved. — [Postfast](https://postfa.st/sizes/x/posts), [Ayrshare](https://ayrshare.com/docs/media-guidelines/x_twitter), [Moda](https://moda.app/resources/sizes/twitter-post), [Metricool](https://metricool.com/twitter-image-size/)
- Douyin 图文: the creator long-text editor reportedly caps at 10 images (beta-era report, may be stale). Image-note ratio and count for the standard 图文 post not found. — [Search summary of 凤凰科技/woshipm items](https://tech.ifeng.com/c/8mQIhUWKMlP)
- WeChat (for contrast, covered elsewhere): SDK image data up to 1 MB, image file up to 25 MB. — [WeChat docs](https://developers.weixin.qq.com/doc/oplatform/Mobile_App/Share_and_Favorites/Android.html)

### Inferences
- Common denominator: 3:4 portrait works natively on 小红书, the Instagram app (and fits its grid), and Threads/Weibo/Douyin as a normal portrait. Landscape-heavy platforms are X (2:1 / 16:9 crops), Facebook and the photographer-community sites.
- Export JPEG sRGB, long edge about 2048 for general social, since I found no evidence any platform preserves more than that on mobile. Treat this as a hypothesis to A/B, not a sourced spec.
- Since byte caps are around 5 to 10 MB on X and XHS, a default quality of about 85-90 JPEG under 5 MB per image avoids server recompression on the strictest common platform.

### Gaps
- No official source found for: 小红书 max image count and pixel limit, 微博 current count/原图/long-image rules, 抖音 图文 ratio/count, 快手, B站 动态, QQ空间, Facebook, Flickr, 500px, Lofter, 图虫. Which platforms compress most heavily was not established with test evidence.

## 2. Cropping implications

### Takeaway
No sourced evidence on cropping UX patterns was gathered; the factual constraints are the ones above (XHS one ratio per note, Instagram carousel one shape, Instagram grid 3:4, X 2:1 crop in grid).

### Cited Findings
- XHS: whole note must use one ratio; mixed ratios are cropped. [3P] — [Woshipm](https://www.woshipm.com/marketing/5572030.html)
- Instagram: all carousel slides should share one shape, and the first slide drives the grid thumbnail. [3P] — [Flowgent](https://flowgent.ai/blog/instagram-post-size)
- X: multi-image posts crop to 2x2 cells. [3P] — [Postfast](https://postfa.st/sizes/x/posts)

### Inferences
- Model: one master edit per photo plus a per-destination crop record (ratio + crop rect), applied at export time only. Default destination ratio set: 3:4 (XHS/Instagram/Threads/Douyin/Weibo), 4:5 (Instagram-safe, API-safe), 1:1, 4:3, 16:9/2:1 (X). Group a batch by destination ratio so all photos in one post share a ratio.
- Auto-crop with subject-aware suggestion is a design choice, not sourced; keep original uncropped on preview as a fallback.

### Gaps
- No competitor UX sourced (Lightroom, VSCO, Snapseed, Apple Photos), no research on subject-aware crop quality.

## 3. Technical sharing paths

### Takeaway
Android's Sharesheet supports multi-image ACTION_SEND_MULTIPLE, but whether each social app accepts multiple images into a new post is app-specific, and Instagram on Android has been reported as single-image only. Official Chinese SDK docs (小红书, 抖音, 微博) could not be retrieved, so the SDK column is largely unverified.

### Cited Findings
- Android: multi-image share uses `ACTION_SEND_MULTIPLE` with `EXTRA_STREAM` as an ArrayList of URIs, MIME `image/*` (or specific); receiver needs URI permission via FileProvider per-URI grants or MediaStore; Android 14+ lets the sender add custom actions; you can learn which target was chosen via an IntentSender (`EXTRA_CHOOSER_RESULT`). The docs say it is up to the receiving app to parse data. — [developer.android.com](https://developer.android.com/training/sharing/send)
- Instagram Sharing to Feed (Meta doc): Android uses `ACTION_SEND` with one `EXTRA_STREAM`, JPEG/GIF/PNG; the page does not cover multiple images. iOS uses Document Interaction with a `.ig` file (UTI `com.instagram.photo`), JPEG preferred, 640x640 recommended and larger resized automatically; the page lists only single-file flow. — [Meta Instagram Sharing to Feed](https://developers.facebook.com/docs/instagram-platform/sharing-to-feed)
- Instagram Android multi-image: a OneUp help article says Instagram permits only one image when another app opens it; Stack Overflow reports captions via EXTRA_TEXT are ignored. Third-party/anecdotal. — [OneUp](https://help.oneupapp.io/en-us/article/multi-image-instagram-posts-on-android-1s90bpx/), [SO thread](https://kiwix.lema.org/stackoverflow.com_en_all_2023-11/a/24663170)
- Meta's own Reels share example builds a single-URI intent, not a multi-item one. — [Meta Android sharing to Reels](https://developers.secure.facebook.com/docs/android/sharing-to-reels-instagram)
- 小红书: a third-party uni-app plugin wrapping the Xiaohongshu share SDK supports single image, multi-image, base64 and video on Android and iOS; needs an AppKey from the open platform and an iOS Universal Link. The SDK owner is listed as 行吟信息科技, with docs possibly at agora.xiaohongshu.com. Image-count limit unknown. — [DCloud plugin](https://ext.dcloud.net.cn/plugin?id=26910), [Kimi SDK disclosure](https://www.kimi.com/user/agreement/SDKSharing)
- 抖音: the open platform has a mobile SDK with share and "投稿发布" (publish) capabilities, requires a ClientKey and permission application with review; image sharing passes via the photo album (needs album access) per a plugin example. Image-count limit unknown. — [Douyin Open iOS access doc](https://partner.open-douyin.com/docs/resource/zh-CN/dop/develop/sdk/mobile-app/access/ios), [DCloud plugin](https://ext.dcloud.net.cn/plugin?id=21794)
- Threads API (Meta): publishing carousels of up to 20 items through the Graph API exists, but that requires OAuth and a hosted image URL, not the share intent. — [Meta Threads docs](https://developers.facebook.com/documentation/threads/posts.md)
- X API: v1.1 media-upload endpoints deprecated June 9, 2025, v2 required (via Emplifi, reporting). — [Emplifi](https://docs.emplifi.io/platform/latest/home/twitter-publishing)

### Inferences
- Treat each platform as one of: (A) system share sheet multi-image, (B) SDK/intent with single image only, (C) official SDK with registration/review (XHS, Douyin, Weibo), (D) API-only. Dapple should capability-probe at runtime and fall back to "save to gallery and open app" with a clear hint.
- A robust universal fallback is: export to MediaStore/Photos album (named album), then open target app's composer or tell user to pick the images in order. This matches how many tools handle Instagram's single-image limit (inferred, not sourced).
- iOS UIActivityViewController was not researched here (no Apple doc fetched); see Gaps.

### Gaps
- No Apple UIActivityViewController doc retrieved; no verification of which iOS apps accept multi-image shares.
- No official doc for XHS, Douyin, Weibo, Kuaishou, Bilibili SDK image limits; no 2024-2026 change log for them.
- Whether X, Threads, Facebook accept ACTION_SEND_MULTIPLE not verified.
- Instagram single-image claim is third party and may be outdated.

## 4. Patterns in other apps

### Takeaway
Not researched with sources in this pass. Only the Android Sharesheet facilities relevant to a generic publish step are cited.

### Cited Findings
- Android Sharesheet guidance: do not build your own list of share targets; use the system Sharesheet for consistency and ranking; custom actions (Android 14+) and up to two initial intents are possible but reduce system suggestions. — [developer.android.com](https://developer.android.com/training/sharing/send)

### Inferences
- A custom per-platform tile row is still justified for Dapple because ratio/preset selection must happen before the handoff; the Sharesheet should remain the "more" path.

### Gaps
- Lightroom, VSCO, Snapseed export dialogs, "export for Instagram/小红书" apps, watermark/signature conventions: not researched.

## 5. Metadata and privacy

### Takeaway
Most major platforms (Instagram, X) are reported to strip EXIF/GPS from displayed/downloaded images, but this is a recompression side effect, evidence is blog-level, and Chinese platforms were not covered. Strip location by default regardless.

### Cited Findings
- Blog sources say Instagram removes GPS and most EXIF on upload, and X removes EXIF in web/mobile apps; sources conflict on some other platforms and are partly undated; the original may remain on the server. Weibo and Xiaohongshu: no results. — [Fast.io](https://fast.io/resources/social-media-photo-metadata-platforms-strip/), [Alphr](https://www.alphr.com/instagram-remove-exif-data-images), [Flavio Copes](https://flaviocopes.com/exif-metadata-privacy/)

### Inferences
- Default: strip GPS, serial numbers and maker notes, keep copyright/artist and minimal camera metadata optionally; offer a "keep metadata" toggle for Flickr/500px-type destinations. Do not rely on platforms to strip.

### Gaps
- No primary-source statement from any platform; 小红书/微博/抖音/快手 behaviour unknown.

## 6. HDR / wide colour

### Takeaway
Instagram and Threads on Android added Ultra HDR photo support in 2024, initially on selected devices (Pixel, Samsung named); Instagram on iPhone reportedly displays HDR photos too. Nothing found for Chinese platforms. Default to SDR sRGB; offer HDR as opt-in for Instagram/Threads only.

### Cited Findings
- Threads Android got Ultra HDR images and 10-bit HDR video on Pixel in March 2024; I found no broader Threads support. — [9to5Google](https://9to5google.com/2024/03/08/threads-ultra-hdr-10-bit-hdr-pixel/)
- Meta said in March 2024 that Instagram and Threads support HDR photos, partnering with Google and Samsung. — [Meta Engineering](https://engineering.fb.com/2024/03/26/android/instagram-threads-hdr-photos/)
- Instagram's early Ultra HDR adoption story on Android (Aug 2024): users can post, view, edit and filter Ultra HDR photos. — [Android Developers Blog](https://android-developers.googleblog.com/2024/08/instagrams-early-adoption-of-ultra-hdr-transforms-user-experience.html)
- Instagram on iPhone reportedly showed HDR photos from the native camera; IG filters make the result SDR; Instagram is strict about HDR file formats; web upload loses HDR. 2024-era reports, may have changed. — [inkl](https://www.inkl.com/news/instagram-hdr-support-expands-to-iphone-and-pixel-devices), [Greg Benz](https://gregbenzphotography.com/hdr-photos/instagram-now-supports-hdr-photos)
- Apple and Google use different gain-map formats, making cross-platform uploads unreliable. — [Greg Benz](https://gregbenzphotography.com/hdr-photos/instagram-now-supports-hdr-photos)
- Instagram's in-app camera adding Ultra HDR on Android (date unclear). — [Engadget](https://engadget.com/2170145/instagrams-in-camera-app-will-support-ultra-hdr-and-night-sight-on-android)

### Inferences
- OPPO Find N5 is not named in any HDR support list; do not assume HDR display on OPPO.
- Export SDR sRGB JPEG by default (embedded sRGB profile); optional "HDR (Ultra HDR JPEG)" only for Instagram/Threads, with a warning.

### Gaps
- Support for 小红书, 微博, 抖音 HDR photos not found; current (2026) Instagram iOS still-photo HDR status not confirmed.

## Spec table (unverified entries marked ?)

| Platform | Max images | Recommended ratio(s) | Export size / quality | Share path |
|---|---|---|---|---|
| 小红书 | 18? (anecdotal) | 3:4 default, 1:1, 4:3; one ratio per note | 3:4 up to ~1280x1706 per one test; JPEG under ~10 MB; suggest 1440x1920 sRGB | Open-platform share SDK (needs AppKey, 3P plugin says multi-image works); fallback album + open app |
| 微博 | up to 18 (2019, followers>100k at launch; ? now) | any; 九宫格 reads best at 1:1 or 3:4 | ? ; JPEG sRGB, ≤ ~2048 long edge | open.weibo.com SDK ? ; Sharesheet ? |
| 抖音 图文 | ? (10 in long-text editor, beta) | 3:4 / 9:16 ? | ? | Douyin open SDK share/投稿 (ClientKey + review); album-based |
| 快手 / B站 / QQ空间 / Lofter / 图虫 | ? | ? | ? | not researched |
| Instagram | 20 (app); 10 in many tools/ads | 4:5 (API-safe), 3:4 (grid, manual only), 1:1, 1.91:1 | 1080 wide; 1080x1350 or 1080x1440; sRGB JPEG | Android ACTION_SEND single image; iOS .ig Document Interaction single image; multi via Graph API only |
| Threads | 20 | 4:5 / 3:4 / 1:1 | ~1080-1440 wide | API (hosted URLs); share intent unverified |
| X | 4 | 16:9 single; 2:1 in 4-grid | 1200x675 single; ≤5 MB mobile | Sharesheet ? (unverified); API v2 |
| Facebook / Flickr / 500px | ? | any | full-res likely accepted (unverified) | not researched |

## UX requirements for Dapple generic publish step

1. Publish step = select photos (ordered set) -> pick destination preset -> review crops -> hand off. Presets are data-driven (JSON, remotely updatable) because limits change and are unverified. Sources: section 1 gaps.
2. Keep one master edit per photo; store per-destination crop + export settings as non-destructive metadata. [Inference]
3. Enforce one ratio per post for destinations that need it (XHS, Instagram carousel) and group the batch by ratio. [Woshipm](https://www.woshipm.com/marketing/5572030.html), [Flowgent](https://flowgent.ai/blog/instagram-post-size)
4. Default ratio 3:4; offer 4:5 as the "Instagram-safe" variant, with a note that 3:4 may fail via API-based schedulers. [RecurPost](https://recurpost.com/?p=162471)
5. Auto-crop with subject-aware suggestion, one-tap accept, drag to adjust; always show the first-slide crop for grid-thumbnail platforms. [Flowgent](https://flowgent.ai/blog/instagram-post-size)
6. Warn when selection exceeds a destination's max (X 4, Instagram/Threads 20, XHS 18?) and offer splitting. [Postfast](https://postfa.st/sizes/x/posts), [Meta Threads](https://developers.facebook.com/documentation/threads/posts.md)
7. Default export: JPEG, sRGB, long edge about 2048 (XHS/Weibo/Douyin/IG), file under 5 MB for X. [Postfast](https://postfa.st/sizes/x/posts)
8. Strip GPS and device identifiers by default; allow per-destination override; never depend on platform stripping. [Fast.io](https://fast.io/resources/social-media-photo-metadata-platforms-strip/)
9. SDR sRGB by default; HDR/Ultra HDR as an explicit opt-in for Instagram/Threads only. [Greg Benz](https://gregbenzphotography.com/hdr-photos/instagram-now-supports-hdr-photos)
10. Use FileProvider per-URI grants and ClipData when sending multiple images; never expose raw file paths. [developer.android.com](https://developer.android.com/training/sharing/send)
11. Use the system Sharesheet for the generic "more" path; do not build your own full target list; supply a ChooserAction (Android 14+) for "Export for..." presets. [developer.android.com](https://developer.android.com/training/sharing/send)
12. Capability ladder per destination: multi-image share -> single-image share -> save to named album and open app with instructions. Do not promise one-tap multi-image for Instagram. [OneUp](https://help.oneupapp.io/en-us/article/multi-image-instagram-posts-on-android-1s90bpx/)
13. For Instagram on iOS use the Document Interaction `.ig` flow for single image, else photo-library fallback. [Meta](https://developers.facebook.com/docs/instagram-platform/sharing-to-feed)
14. Treat the 小红书 and 抖音 SDKs as optional integrations requiring developer registration and review; ship the album fallback first. [Douyin Open](https://partner.open-douyin.com/docs/resource/zh-CN/dop/develop/sdk/mobile-app/access/ios), [DCloud](https://ext.dcloud.net.cn/plugin?id=26910)
15. Track the chosen share target via Sharesheet callback to default the next publish to the user's usual destinations. [developer.android.com](https://developer.android.com/training/sharing/send)
16. Offer an optional signature/watermark (off by default, per-destination, saved) and copyright metadata. [Inference; competitor patterns not sourced]
17. Keep a "Original quality" preset (full-res JPEG, metadata optional) for Flickr/500px/Lofter/图虫 style destinations. [Inference]
18. Render a preview of the platform crop (grid cell for X, feed vs grid for Instagram) before handoff. [Postfast](https://postfa.st/sizes/x/posts), [Neal Schaffer](https://nealschaffer.com/instagram-post-size/)
19. Do not bake WeChat Moments rules here; keep as a separate preset (SDK image limit 1 MB data / 25 MB file). [WeChat docs](https://developers.weixin.qq.com/doc/oplatform/Mobile_App/Share_and_Favorites/Android.html)
20. Before launch, run device tests on OPPO Find N5 and iPhone for each target app version, because share-intent behaviour and limits are app-version-specific and mostly undocumented. [Inference]
