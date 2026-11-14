# Publish presets: limits, sizes, open-app data (researched 2026-11-13)

Method: web search only (no logged-in platform consoles). "Confidence" is about the value in `app/lib/model/catalog.dart`. Anything marked 未核实 stays as the
previous assumption (requirements 4.11.2 [待验]); the UI does not say so.

| Preset | Count | Default / allowed ratios | Long edge | Confidence |
|---|---|---|---|---|
| 朋友圈 (wechat) | 9 | 原比例, 1:1 | 1440 | 未核实 (see below) |
| 小红书 (xhs) | 18 | 3:4, 原比例 | 2048 | 比例 medium, 张数 / 大小 未核实 |
| 微博 (weibo) | 18 | 原比例, 1:1 | settings default | 未核实 |
| 抖音图文 (douyin) | none | 3:4, 9:16 | 2048 | 未核实 |
| Instagram (ig) | 20 | 4:5, 1:1, 原比例 | 2048 | 张数 high, 比例 high, 边长 medium |
| 其他 App | none | 原比例 | settings default | n/a |

## Findings

- Instagram: feed photos keep full resolution when at least 1080 px wide and the aspect ratio lies between 1.91:1 and 4:5 (height 566 to 1350 at 1080 wide);
  other ratios are cropped to the nearest supported one. Source: Instagram Help Center article 1631821640426723 (https://help.instagram.com/1631821640426723; the page
  is script-rendered, so the text was read through search results, 2026-11-13). Carousel limit raised from 10 to 20 photos / videos in August 2024:
  https://petapixel.com/2024/08/09/instagram-users-can-now-share-20-photos-videos-in-a-post-carousel-photodump (2024-08-09), https://metricool.com/instagram-increases-carousel-content-limit/.
  Defaults 4:5 and the 20 limit are therefore kept (confirmed). 2048 px long edge is above the 1080 width Instagram stores; harmless, kept.
- 小红书: three note image ratios, 3:4 (900x1200), 1:1 (1080x1080), 4:3 (1200x900), one ratio per note (secondary sources: https://www.canva.cn/sizes/little-red-book/,
  https://www.digitaling.com/articles/395591.html, read 2026-11-13). Not found: official image count per note, file size cap. 18 images and 2048 px are
  unverified. 4:3 is not offered in the preset's ratio list; adding it needs an official source first.
- 朋友圈: iOS WeChat 8.0.18 (January 2022) lets a Moment pick up to 20 images, but more than 9 turn into a slideshow video; 9 photos stays the photo-post limit
  (https://www.ithome.com/0/600/690.htm, 2022-01). No official statement on recompression or long-image limits was found: 1440 px and 1:1 stay unverified.
- 微博: no usable source for image count (18), size cap or original-size option. Unverified.
- 抖音图文: no Douyin source found. TikTok (international) photo mode: up to 35 photos, 9:16 recommended, 1:1 and 4:5 accepted
  (https://postfa.st/sizes/tiktok/carousel); Douyin may differ. 3:4 is the Douyin Shop product-image ratio, not a post rule. Unverified, count left unlimited.

## Verification round 2 (2026-11-14): figure, source, official or not

No platform help page could be read as text (Instagram's help article renders by script and returned only topic headings; no official WeChat, Xiaohongshu, Weibo or
Douyin documentation was found). Every figure below is therefore third-party at best.

| Platform | Figure | Source | Official? | Status |
|---|---|---|---|---|
| Instagram | 1080 px wide kept, ratio 1.91:1 to 4:5 not cropped | https://help.instagram.com/1631821640426723 (via search text) | official (text not re-read) | confirmed earlier |
| Instagram | carousel 20 | https://petapixel.com/2024/08/09/instagram-users-can-now-share-20-photos-videos-in-a-post-carousel-photodump | third-party | confirmed (two sources) |
| Instagram | 3:4 (1080x1440) accepted in-app; file cap 30 MB | https://www.hopperhq.com/blog/correct-instagram-dimensions/, https://postfa.st/sizes/instagram/feed | third-party, sources disagree | unverified; 4:5 default kept, 3:4 not added |
| 小红书 | ratios 3:4, 1:1, 4:3, one per note | https://www.canva.cn/sizes/little-red-book/, https://www.woshipm.com/marketing/5572030.html, https://www.digitaling.com/articles/395591.html | third-party (three) | accepted; 1:1 added to the preset; 4:3 not in `cropRatios`, not added |
| 小红书 | 18 images, 750x1000 px, 20 MB per image | https://www.tixiaolu.com/v2/posts/v2-3f3331b3.html | third-party (one) | unverified, 18 and 2048 kept |
| 抖音 | 长图文 up to 30 inline images (a separate feature) | https://www.stdaily.com/web/gdxw/2025-12/26/content_453850.html | news report | not the plain 图文 post; count left unlimited |
| 抖音 | 3:4 / 9:16 ratios, 2048 px | none found | n/a | unverified, kept |
| 抖音 (TikTok photo mode, for reference) | 4 to 35 photos, 9:16, 1:1, 4:5 | https://postfa.st/sizes/tiktok/carousel | third-party | different product, not applied |
| 微博 | count 18, size cap, original-size option | none found (only a 2011 note on 9-image stitching) | n/a | unverified, kept |
| 朋友圈 | 9 photos; more than 9 become a slideshow video (iOS 8.0.18) | https://www.ithome.com/0/600/690.htm | news report | confirmed as photo limit |
| 朋友圈 | 1440 px long edge, recompression, 1:1 | none found | n/a | unverified, kept |

HDR and wide-gamut handling: not documented by any of the platforms in what was found; unconfirmed everywhere. Not changed: export behaviour, 抖音, 微博, 朋友圈 values.

## iOS URL schemes for the "open the app" fallback (data in `Preset.iosScheme`)

| App | Scheme | Source / confidence |
|---|---|---|
| 微信 | `weixin://` | registered by WeChat, used by its Open SDK (iOS apps must list it in LSApplicationQueriesSchemes to call canOpenURL); high |
| 小红书 | `xhsdiscover://` | scheme of the XHS open platform SDK; medium |
| 微博 | `sinaweibo://` | Weibo SDK scheme; high |
| 抖音 | `snssdk1128://` | Douyin open platform scheme; medium |
| Instagram | `instagram://` | Instagram's documented iOS URL scheme (`instagram://app`); high |

Whoever wires iOS must add these to `LSApplicationQueriesSchemes` in Info.plist (app/native_ios is not touched here). Not verified on a device.

## HEIC export: analysis only, no implementation

- Android: `androidx.heifwriter.HeifWriter` encodes HEIC from a Bitmap / YUV buffer on API 28+ with hardware HEVC where present (quality knob, optional Exif); software
  fallback varies by device. Exif and colour profile support through HeifWriter is thin (Exif via `addExifData`, no ICC control), so a Display P3 HEIC is awkward.
- iOS: ImageIO (`CGImageDestination` with `public.heic`, iOS 11+) writes Exif / GPS / ICC (Display P3) and 10-bit; Apple's HDR photos are HEIC with an ISO 21496-1 gain map.
- Platform acceptance (未核实, not tested against the live apps): iOS social apps generally take HEIC and re-encode to JPEG on upload; Android builds of the Chinese
  apps and the Moments picker are known to treat HEIC as unsupported or to fail on it on older systems. Instagram and TikTok convert HEIC server-side.
- Recommendation: keep JPEG for every publish preset (universal, our Ultra HDR JPEG already covers the Android HDR case). Offer HEIC later only as a "save to gallery"
  format on iOS (smaller files, P3 + 10-bit), never as the default for publishing; revisit when the wide-gamut export (see below) exists.

## Wide-gamut (Display P3) export status

The C API accepts `"colorSpace":"srgb"` only; `p3` returns an error. The P3 profile builder exists and is tested. The render pipeline compresses to sRGB inside
`Renderer` / the engine output stage (gamut map, sRGB encode, curves, LUT, vignette), so P3 needs changes outside the export functions (hunk list in the task report).
