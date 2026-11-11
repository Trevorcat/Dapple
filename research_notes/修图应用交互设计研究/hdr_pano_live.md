# Dapple: HDR merge, panorama stitching, Live Photo / Motion Photo creation (research notes, as of 2026-11-11)

Method note: about 25 tool calls. Many primary pages returned HTTP 403 (helpx.adobe.com fetch, docs.opencv.org, opencv.org/license, ithome). Where I could only get search-snippet summaries, I say so. Anything without a URL is marked UNVERIFIED and is general knowledge, not research.

## 1. What does Sony actually offer for "Live Photo" from video?

### Takeaway
Sony's Creators' App can create "Motion Photos" (about 3 s, JPEG-based) from camera-recorded video. It does this either around a Shot Mark during import or at a position you pick during in-app playback. It also makes 15/30/60 s short videos around Shot Marks. The user's claim is basically right, but the feature is a Sony-style Motion Photo that Sony itself says may not be recognized as one on some phones or sharing services. The A7R V (ILCE-7RM5) is on the supported list.

### Cited Findings
- Creators' App can create 3-second Motion Photos from videos recorded on the camera. It can also create 15-, 30- or 60-second short videos centered on Shot Marks you already added. — [Sony support "What are Motion Photos?"](https://www.sony.ca/en/electronics/support/articles/CCCA99030) (page itself returned 403 to direct fetch; content from search-result extracts of this page and its regional copies, e.g. [sony-mea](https://www.sony-mea.com/en/electronics/support/e-mount-body-zv-e-series/zv-e10m2k/articles/CCCA99030))
- Three creation paths: (1) import with the "differential import" setting, (2) import without it (both use Shot Marks added at recording time), (3) during video playback, create a Motion Photo around a chosen position. Path 3 cannot make 15-60 s short videos. — same Sony page
- Motion Photos are JPEG format; short videos keep the original video format. Sony warns that, depending on the smartphone or sharing service, they may not appear as Motion Photos, and sharing may convert format. — same Sony page
- ILCE-7RM5 is marked as supporting Motion Photo in Sony's "Supported cameras" table. The ILCE-7RM5 needs camera system software 2.00 or later to connect to Creators' App. Footnote *16 (Motion Photo / short-video creation not supported when videos are imported) is attached to other models such as FX3, FX3A, FX30. — [Sony Supported cameras (ILCE-7RM5)](https://www.sony.ca/en/electronics/support/e-mount-body-ilce-7-series/ilce-7rm5/articles/CCCA03001), via search extract
- Sony's separate "Movie Edit add-on" (Imaging Edge) imports and edits phone video; its pages show no Live Photo feature. — [Sony Movie Edit add-on](https://support.d-imaging.sony.co.jp/app/movieedit/en/instruction/) (search extract only)

### Inferences
- Sony's output is likely a Google-style Motion Photo (JPEG + appended MP4), since Sony says "Motion Photos" in JPEG format and that Android/iOS may not recognize it. Sony does not state the XMP structure. UNVERIFIED: dump one Creators' App output with exiftool.
- Because Sony says recognition varies by phone, Dapple can differentiate by writing spec-correct output for both ecosystems.

### Gaps
- No Chinese-language Sony page found for 实况照片. Creators' App version history/release dates for the feature not found. Whether any in-camera Live-Photo feature exists on A7R V or X-T50 not found (nothing surfaced). Exact file structure of Sony's output unverified.

## 2. HDR merge: UX (Lightroom and others)

### Takeaway
Lightroom Classic's flow is: select 2+ (guide says 3 minimum in one source) frames, Photo > Photo Merge > HDR (Ctrl+H), preview dialog with Auto Align, Auto Settings/Tone, Deghost (None/Low/Med/High, overlay), Create Stack, output is a 16-bit DNG. Lightroom mobile does NOT merge existing photos (Adobe community reports); this is a feature gap Dapple can fill. Selection is manual multi-select in Adobe's products; automatic bracket grouping is not shown in the sources I found.

### Cited Findings
- Classic: Photo > Photo Merge > HDR or Ctrl+H; Alt+Shift+H (Win) skips the preview dialog using last settings. Auto Align recommended for handheld; may be unnecessary on tripod. Auto Tone (now "Auto Settings") gives an even starting point. Deghost Amount: None/Low/Medium/High; start with Low; pressing O shows the deghost overlay. Create Stack groups the sources with the merge on top. Result is a 16-bit DNG with an "HDR" suffix. — [Adobe HelpX HDR photo merge](https://helpx.adobe.com/lightroom/help/hdr-photo-merge.html) (listing via search; content summarized from search result, direct fetch 403), [jkost.com](https://jkost.com/blog/?p=23098)
- The HDR Panorama merge cannot deghost, so bracketed exposures must be HDR-merged first. — [jkost.com](https://jkost.com/blog/?p=19408), secondary source
- Lightroom mobile: HDR merge only works for the in-app raw HDR capture mode; Adobe community posts (2021 staff reply, 2024 Android post, 2025 iOS post) say no merge of imported photos. — [Adobe Community thread](https://community.adobe.com/t5/lightroom-ecosystem-cloud-based-ideas/p-enable-photo-merge-pano-amp-hdr-with-imported-images/idi-p/12232898) (forum evidence; may have changed after the posts; recheck the current app)
- Users point to Affinity Photo for iPad as having merge features that Lightroom for iPad lacks. — same forum thread
- Snapseed, Aurora HDR, Photomatix UX: NOT researched (budget). UNVERIFIED.

### Inferences
- A cheap, high-value UX win over Lightroom: auto-group candidates and pre-select them, instead of making users multi-select.
- A deghost overlay toggle (Adobe's "O") is a good, cheap preview pattern.

### Gaps
- Snapseed/Aurora/Photomatix flows; Lightroom Classic's progress behavior; current (late 2026) Lightroom mobile status.

## 3. HDR merge: algorithms, libraries, licenses, bracket detection

### Takeaway
For RAW, merge in linear space with exposure-normalized weighted averaging (no camera response curve needed because RAW is linear). Use a median-threshold-bitmap or tile alignment, plus a simple variance/difference-based deghost. Mertens fusion is the fallback for JPEG/tone-mapped output. OpenCV is Apache-2.0 (4.5+); LuminanceHDR and pfstools are GPL and unsuitable to link.

### Cited Findings
- The OpenCV repository LICENSE is Apache License 2.0. — [github.com/opencv/opencv LICENSE](https://github.com/opencv/opencv/blob/4.x/LICENSE) (fetched). Pre-4.5 headers carried 3-clause BSD. — [OpenCV 3.4.8 xfeatures2d.hpp copy](https://gemfury.com/foxeye/deb:opencv-dev/opencv-dev-3.4.8-amd64/content/usr/local/include/opencv2/xfeatures2d.hpp)
- SIFT and SURF were documented as patented/non-free (OPENCV_ENABLE_NONFREE, "use at your own risk"). SURF lives in xfeatures2d (opencv_contrib). — [OpenCV 4.2.0 docs](https://docs.opencv.org/4.2.0/d2/dca/group__xfeatures2d__nonfree.html). (SIFT patent expiry in 2020 and its move into main features2d in 4.4: from memory, UNVERIFIED. Prefer ORB/AKAZE regardless.)
- Google HDR+ (Hasinoff et al., SIGGRAPH Asia 2016): burst of constant, deliberately under-exposed raw frames, tile-based alignment to a reference, merge on raw with a robust (Wiener-style) filter. Notably not bracketing. — [Google Research](https://research.google/pubs/burst-photography-for-high-dynamic-range-and-low-light-imaging-on-mobile-cameras/), [paper preprint](https://swap.stanford.edu/was/20151211033644mp_/http://graphics.stanford.edu/papers/hdrp/hasinoff-hdrplus-sigasia16-preprint.pdf), [dataset](https://hdrplusdata.org/dataset.html)
- Survey of mobile computational photography. — [arXiv 2102.09000](https://arxiv.org/pdf/2102.09000)
- Sony A7R V-class bracket auto-detection: ExifTool decodes Sony MakerNote SequenceNumber / SequenceImageNumber / SequenceFileNumber / SequenceLength; a KDE bug report with an ILCE-7RM4A shows 5 exposure-bracketed ARWs with SequenceFileNumber 1-5 and SequenceLength 5, used by the author for grouping. — [KDE bug mail](https://mail-archive.com/kde-bugs-dist@kde.org/msg1179768.html); tag reference [exiftool.org Sony tags](https://exiftool.org/TagNames/Sony.html). Tag9400 contents not confirmed from the snippet. A7R V behavior is inferred from the 7RM4A (UNVERIFIED on A7R V).
- OpenCV photo module (AlignMTB, MergeDebevec, MergeRobertson, MergeMertens, CalibrateDebevec) and Debevec-Malik 1997, Mertens 2007, Ward 2003 MTB: docs fetch failed (403). Existence of these classes is from general knowledge, UNVERIFIED in this session; verify against the 4.x docs.
- LuminanceHDR (GPL) and pfstools (GPL/LGPL mix), Halide examples (MIT): UNVERIFIED in this session (not fetched).

### Inferences
- Linear RAW makes Debevec/Robertson response calibration unnecessary: scale each frame by 1/(exposure time x ISO gain), weight by a hat function (saturation and noise-floor rejection), average. That is easy to hand-write in the C++ core, no OpenCV dependency needed.
- Memory: 61MP x 4 bytes x 3 channels (float RGB) is about 730 MB per frame; 3-5 frames cannot be held at full res. Process in tiles/strips, or process on Bayer/demosaic-first at 16-bit half float (about 365 MB), or merge pre-demosaic raw. This is arithmetic, not sourced; needs a device test.
- A7R V RAW is 14-bit and has a very wide DR; brackets are most valuable for scenes beyond that, with 2-3 frames at +/-2 EV usually enough.
- Output: linear 16/32-bit float DNG (Adobe DNG spec, floating-point DNG supported) keeps editability in Dapple's pipeline; tone-mapped 16-bit TIFF/JPEG/HEIC for sharing.

### Gaps
- Exact A7R V AEB tag values (needs exiftool on a real file). Runtime/memory benchmarks on Find N5 (Snapdragon 8 Elite). Halide license and examples.

## 4. Panorama: UX, algorithms, libraries

### Takeaway
Lightroom offers Spherical (default), Cylindrical, Perspective projections plus Boundary Warp (0-100), Fill Edges, Auto Crop. OpenCV's Stitcher gives a complete, Apache-2.0 pipeline. Hugin/libpano13/enblend are GPL and should not be linked into the app.

### Cited Findings
- Projections: Spherical default, natural for wide panos; Cylindrical keeps verticals straight for wide panos; Perspective keeps straight lines straight (architecture). Some merges fail with certain projections, so try another. — [Adobe HelpX merge HDR/panorama](https://helpx.adobe.com/lightroom-cc/using/hdr-panorama.html), [Adobe help panorama](https://helpx.adobe.com/lightroom/help/panorama.html), [photographylife](https://photographylife.com/how-to-create-a-panorama-in-lightroom) (search extracts)
- Boundary Warp slider 0-100 stretches uneven edges to fill the frame, preserving content otherwise cropped. Fill Edges fills empty edges (one reviewer finds it weak over large open areas). Auto Crop crops transparent areas, may lose desired content. — [Adobe Boundary Warp](https://helpx.adobe.com/ro/lightroom/how-to/panorama-boundary-warp.html), [slrlounge](https://slrlounge.com/lightroom-panorama-tool-2), search extracts
- Lightroom mobile has no stitching of imported photos. — [Adobe Community](https://community.adobe.com/t5/lightroom-ecosystem-cloud-based-ideas/p-enable-photo-merge-pano-amp-hdr-with-imported-images/idi-p/12232898)
- OpenCV is Apache-2.0 (see section 3). The Stitcher module (feature matching, bundle adjustment, seam finding, multi-band blending) per OpenCV docs: not fetched here, UNVERIFIED in session. The nonfree-feature caveat applies: configure the Stitcher with ORB or AKAZE.
- PTGui, Hugin, Microsoft ICE UX; Hugin/enblend/libpano13 GPL; Brown & Lowe 2007: not researched in this session; licenses of Hugin/enblend/libpano13 as GPL are from general knowledge, UNVERIFIED.

### Inferences
- Auto-detect pano sequences by timestamp gaps under about 2-3 s plus consistent exposure plus overlapping features; Sony MakerNote sequence fields may help (UNVERIFIED for pano frames; Sony "Sweep Panorama" is a separate in-camera JPEG feature).
- 61MP frames: stitch at reduced resolution (about 2-4 MP) for alignment/bundle adjustment/seam estimation, then warp and blend at full res in tiles using the estimated homographies. Use the linear ProPhoto floats; blend in linear light, then apply the user's tone/edit stack afterward (consistent with HDR). Needs benchmarks.
- Constrain the tool: rotation-only camera model (tripod-less handheld panning) matches OpenCV's defaults.

### Gaps
- OpenCV Stitcher memory behavior on 61MP; real-time progress hooks; fisheye/lens-correction interplay; Microsoft ICE/PTGui/Hugin UX.

## 5. Apple Live Photo format and creation

### Takeaway
A Live Photo is a still image plus a MOV tied by the same UUID: MakerNote key 17 in the image, and `com.apple.quicktime.content.identifier` in the MOV, plus a timed `com.apple.quicktime.still-image-time` metadata track. Third-party creation: `PHAssetCreationRequest` with `.photo` and `.pairedVideo` resources. It is fragile; validate on-device with real iOS versions.

### Cited Findings
- Apple's AVFoundation sample uses PHAssetCreationRequest to add the photo and `.pairedVideo` resources (shouldMoveFile on the video options). — [Apple: Capturing and saving Live Photos](https://developer.apple.com/documentation/avfoundation/capturing-and-saving-live-photos.md)
- Shared UUID: still image MakerNote key 17; MOV `com.apple.quicktime.content.identifier`; MOV also needs a still-image-time timed-metadata track. — [edonv/LivePhoto](https://github.com/edonv/LivePhoto), [LimitPoint/LivePhoto](https://www.github.com/LimitPoint/LivePhoto/blob/master/LivePhoto.swift), [StackOverflow mirror](https://kaweah.freedombox.rocks/kiwix/content/stackoverflow.com_en_all_2023-11/a/35286486)
- Removing MakerNote key 17 makes the paired-video save fail. — [juniperphoton substack](https://juniperphoton.substack.com/p/decoding-some-hidden-magic-of-makerapple)
- Re-encoding the UIImage to JPEG strips the identifier and PhotoKit rejects the pair (error 3302). — [Live Photo PRO guide](https://scriptingapp.github.io/TestFlight/guide/Changelog/3.1.0/AVCaptureSession%20&%20Camera%20Control/Live%20Photo/)
- Reports of breakage: Live Photos from the PHLivePhoto API show "Motion not available" as wallpaper ([Apple forums 798044](https://developer.apple.com/forums/thread/798044)); JPEG+MOV combination fails to save on iOS 26.6.2 ([Apple forums 845589](https://developer.apple.com/forums/thread/845589)); similar threads [758764](https://developer.apple.com/forums/thread/758764), [776646](https://developer.apple.com/forums/thread/776646). Treat as current risks; contents of the threads not fully read.

### Inferences
- Write the still as HEIC or JPEG using an ImageIO path that preserves a MakerApple dictionary (kCGImagePropertyMakerAppleDictionary key "17"); verify with exiftool on a Camera-app sample. Do not round-trip through UIImage.
- Wallpaper (Live) eligibility likely needs extra metadata/duration/resolution; UNVERIFIED, test.

### Gaps
- Exact still-image-time encoding (a single int8 sample value 0 on a metadata track at the still frame's time, from general memory, UNVERIFIED). Video codec/size/duration requirements for Apple (typical Camera output: about 3 s, HEVC/H.264 1080p-ish, 30 fps): not sourced.

## 6. Android Motion Photo format and platform/social behavior

### Takeaway
Google's Motion Photo 1.0 is a still (JPEG/HEIC/AVIF) with an appended MP4/MOV, described by XMP. OPPO/ColorOS 实况照片 compatibility is NOT verified. WeChat added 实况照片 in Moments: iOS first (announced Sept 2024), Android announced April 2025 (per IT之家 text), but rollout is gray-scale and version/device dependent.

### Cited Findings
- Spec fields: `Camera:MotionPhoto`=1, `Camera:MotionPhotoVersion`=1, optional `Camera:MotionPhotoPresentationTimestampUs` (default: middle of the video if absent), namespace `http://ns.google.com/photos/1.0/camera/`. Container directory in `http://ns.google.com/photos/1.0/container/` with Items: first Primary (Mime image/jpeg|heic|avif, Semantic Primary), last MotionPhoto (Mime video/mp4 or video/quicktime, Length required). The video must be last in the file with no trailing bytes. MicroVideo* fields are deleted/ignored in v1.0 (legacy). JPEG: optional Padding. HEIC/AVIF: video wrapped in a top-level `mpvd` box, primary Padding must be 8. Filename pattern `*MP.jpg`. Video: required AVC/HEVC/AV1 primary track, resolution undefined, optional AAC 16-bit audio at 44.1/48/96 kHz, optional secondary higher-res track, optional metadata track (`mett`, `application/motionphoto-image-meta`) with frame scores and an `isStabilized` flag. Ultra HDR stills must also include a GainMap item before the video item. — [Android Motion Photo format 1.0](https://developer.android.com/media/platform/motion-photo-format) (fetched in full)
- WeChat announced Moments live photo support for iOS in Sept (2024) and Android in April (2025); Android needs recent system + WeChat and gray-scale eligibility; a reported 8.0.57 threshold and OPPO/OnePlus/vivo/Honor/realme "announcements" come from IT之家 and aggregator text. — [IT之家 0/894/205](https://www.ithome.com/0/894/205.htm) (via search extract; direct fetch 403). Years are inferred ("去年/今年" wording), only partly verified.
- WeChat Android 8.0.63 beta briefly allowed sending Live Photos in chat, adapted for Xiaomi/OPPO/vivo/Honor, then it was "cloud-withdrawn"; 8.0.64 re-enabled viewing only (Moments + chat). — [IT之家 0/894/205](https://www.ithome.com/0/894/205.htm) via search extract. The date of this report is not shown in my extract (one extract says "11月2日"; year unknown).
- OPPO said in December 2024 that the feature depended on third-party adaptation with no concrete plan; later, ColorOS reportedly supports sending/receiving Live Photos in WeChat chat (ColorOS 15+); article date unclear. — [IT之家 0/846/617](https://www.ithome.com/0/846/617.htm), [SMZDM](https://post.smzdm.com/p/a9kqd945/) (search extracts; low date reliability)
- A Toutiao marketing-style article says Find X8 Ultra/X8s can post "超清实况照片" to Moments. — [Toutiao](https://www.toutiao.com/zixun/7498505103687829504); the same article's claim of version 8.0.57 conflicts with the IT之家 8.0.57 beta report, which doesn't mention live photos (per search tool summary). Low reliability.
- Harmony WeChat reportedly supports send/view of 实况照片 in Moments since December (2024). — IT之家 via search extract. Unverified year.
- 小红书, 微博 behavior for live/motion photos: NOT found. Whether sharing preserves motion on Android vs iOS for Dapple-produced files: not verified.

### Inferences
- Producing a spec-correct Google Motion Photo (with the Primary/MotionPhoto Container XMP) is the best bet for Android gallery apps; ColorOS's own 实况照片 may use its own metadata, so test in OPPO Photos and WeChat on a Find N5.
- Provide an "Export as .mp4 + .jpg" fallback, since social apps vary.

### Gaps
- ColorOS 实况照片 on-disk format and Google-Motion-Photo interop; Find N5 specifically; 小红书/微博; 2026 status of WeChat Android rollout.

## 7. Video to live photo UX (reference apps)

### Takeaway
No sourced analysis of intoLive, Lively, Google Photos or 小米相册 was performed. The Sony flow above is the only sourced reference (choose position during playback; around a Shot Mark; about 3 s).

### Cited Findings
- Apps that convert Live Photo <-> video exist (e.g., "Live Photo Converter", "Export LivePhoto"); no UX detail captured. — [App Store Live Photo Converter](https://apps.apple.com/us/app/id1506460540?l=en-us)

### Inferences (design suggestions, not sourced)
- Flow: pick video, scrub to key frame (cover), a fixed 1.5-3 s window around it (Apple Camera's Live Photos are about 3 s: general knowledge, UNVERIFIED), mute toggle, optional stabilization, preview loop, export.

### Gaps
- Reference-app UX; encoding constraints per platform.

# Recommendations for Dapple

## HDR merge
(a) UX flow: Library auto-groups frames into bracket sets (same camera, timestamps within a few seconds, differing exposure; use Sony MakerNote Sequence* fields; UNVERIFIED on A7R V) and shows a "Merge HDR" badge on the stack. Tap it (or multi-select as fallback) opens a preview. Defaults: Auto align on, Deghost Low (None/Low/Med/High plus an overlay toggle), Auto tone on. Show a progress bar with stage names and a downsampled preview first, then the full-res result. Output: new linear float DNG (stacked with sources) so editing continues in Dapple's pipeline.
(b) Technical approach: own C++ linear-RAW merge (exposure-normalized hat-weighted average on LibRaw data, tile/strip processing), alignment via MTB or tile-based (HDR+-style) search implemented in-house or via OpenCV (Apache-2.0); Mertens fusion for JPEG/HEIC inputs. Avoid LuminanceHDR/pfstools (GPL). Output float DNG via Adobe DNG SDK (license: UNVERIFIED, check) or LibRaw-free writer; fall back to 16-bit TIFF.
(c) Risks: memory/time for 3-5 x 61MP on-device (needs benchmarks); A7R V AEB tag behavior; ghosting from moving subjects; DNG compatibility across viewers; white balance/exposure metadata normalization between frames.

## Panorama
(a) UX flow: auto-detect sequences (rapid timestamps, overlap), offer "Stitch Panorama" badge; preview with projection chooser (Spherical default, Cylindrical, Perspective), Boundary Warp slider, Auto Crop toggle, Fill Edges optional; low-res preview before full render; output float linear DNG or TIFF (keeps editing), plus JPEG/HEIC.
(b) Technical approach: OpenCV Stitcher/detail pipeline (Apache-2.0), with ORB/AKAZE (avoid SURF/nonfree), working on downscaled copies for estimation, full-res tiled warp and multi-band blend. Avoid Hugin/libpano13/enblend (GPL).
(c) Risks: OpenCV memory on 61MP, parallax artifacts, exposure variations, lens distortion/vignetting handling, runtime on phone, GPL contamination of any wrapped code.

## Live Photo / Motion Photo
(a) UX flow: pick a video clip (from Sony import or camera roll), scrub to cover frame, auto 1.5-3 s window around it (trim handles), mute toggle, optional stabilization, preview, export as: Android Motion Photo (JPEG or HEIC + MP4), iOS Live Photo (iOS later), or separate files. Sony-compatible: allow multi-frame Shot Mark seeds if present.
(b) Technical approach: Android: write JPEG/HEIC with Google Motion Photo XMP (Camera + Container directory, Length, Padding rules) then append H.264/HEVC MP4 (AAC optional) via MediaCodec/MediaMuxer or FFmpeg (LGPL if dynamically linked; check build config, UNVERIFIED); use Android Ultra HDR gain map only if present. iOS: PHAssetCreationRequest with .photo + .pairedVideo, identical UUID in MakerApple key 17 and the MOV content identifier, still-image-time track (AVAssetWriter). XMP via Adobe XMP toolkit (BSD-style, UNVERIFIED) or manual XML writing.
(c) Risks: ColorOS 实况照片 may not read Google format (test on Find N5 Photos app and gallery); WeChat/小红书/微博 may drop motion on share (test each, both OS); iOS PhotoKit save failures in iOS 26.x (see forum threads); encoder constraints (resolution, bit depth, HDR video from Sony S-Log/HLG); Sony says sharing may break motion recognition; no verified 2026 WeChat Android status.
