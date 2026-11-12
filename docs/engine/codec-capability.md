# Codec capability layer (video decode and encode)

Principle (owner): codecs adapt to what each device declares, verified by a one-time self-test; nothing is hard-coded by device model.

Code: `app/android/.../CodecLadder.kt` (pure: declared capabilities, ladders, self-test cache, stall watchdog; JVM tests in
`CodecLadderTest.kt`), `CodecCatalog.kt` (MediaCodecList query, input-buffer layout), `CodecSelfTest.kt` (the tests),
`CodecManager.kt` (glue, JSON report), `SoftwareMedia.kt` / `LiveBridge.kt` (use it), `core/video` (native 10-bit hardware decode,
report storage in the C API).

## 1. Query

`CodecCatalog.query()` lists every AVC / HEVC / AV1 / VP9 codec of `MediaCodecList(ALL_CODECS)` (aliases skipped on API 29+):
profiles, colour formats (P010 = 54, Surface, Flexible, NV12, I420), supported size range, 1080p frame-rate ceiling, number of
performance points (API 29+), `hdr-editing` feature (API 33+), hardware (`isHardwareAccelerated`) vs software-only
(`isSoftwareOnly`; before API 29 judged by the `OMX.google.` / `c2.android.` prefixes). AV1 and VP9 are reported but never chosen
(Motion Photo needs the compatible codecs).

The report is JSON (`CodecManager.report()`): channel `dapple/live` method `codecReport` (`run`: run missing self-tests, `force`:
repeat all); in the core `dv_set_codec_report(json)` / `dv_codec_report(buf, len)` (`core/video/dapple_video.h`).

## 2. Ladders

Encode (`Ladder.encode`), per job (size, fps, HDR source, 8-bit-only):

1. HEVC Main10: encoder declares a Main10 profile (Main10, Main10HDR10 or Main10HDR10Plus) **and** P010 input; HDR sources get
   BT.2020 + HLG/PQ tags and `KEY_HDR_STATIC_INFO`. Skipped for the 兼容版 export and the film-applied path (`eightBitOnly`).
   Surface-only 10-bit input is not used (the core delivers P010 buffers).
2. HEVC Main 8-bit.
3. H.264 High 8-bit (High requested only if declared).

Within a rung hardware first, a codec that already passed its self-test before an untested one. A failed self-test (or a failure
during a real job: stall, downgraded profile) removes the codec from that rung.

Decode (`Ladder.decode`), per stream (mime, required profile, bit depth, chroma):

1. 8-bit 4:2:0: a hardware decoder declaring the profile -> FFmpeg's MediaCodec wrapper (`hwPref` 1).
2. 10-bit 4:2:0: a hardware decoder declaring the profile **and** P010 output, not failed in its self-test -> NDK `AMediaCodec`
   directly with `COLOR_FormatYUVP010` output (`hwPref` 2, `core/video` `openNative`); FFmpeg's wrapper only returns 8-bit.
3. FFmpeg software (any depth / chroma, e.g. Sony H.264 4:2:2 10-bit). Always the last rung.

(`StreamProbe` had the AVC profile constants wrong: 4:2:2 asked for 0x10 = High10, 10-bit 4:2:0 for 0x08 = High. Now High10 0x10,
High422 0x20, High444 0x40.)

## 3. Self-tests (off the UI thread, before first use of a declared capability)

| kind | what | pass condition |
|---|---|---|
| `enc10` | encode 8 frames of a synthetic 640x352 10-bit gradient (P010), decode back | output within the watchdog (8 frames accepted without output for 3 s = fail); output `KEY_PROFILE` is 2 / 0x1000 / 0x2000; decoded luma has > 256 distinct levels (hardware 10-bit decoder, P010 output) |
| `enc8hevc`, `enc8avc` | same with 8-bit input | output within the watchdog, decodes, > 64 distinct luma levels |
| `dec10` | decode the bundled 3-frame Main10 clip (`assets/selftest_main10.mp4`) with the hardware decoder to P010 | output format is really P010 and > 256 distinct luma levels |

Results are cached in SharedPreferences keyed by `Build.FINGERPRINT` + codec name + kind (`SelfTestCache`); keys of another
fingerprint are purged at start, so an OS update tests everything again. A real job that finds the codec unreliable (stall,
downgraded profile, native decoder falling back to software) records a FAIL for that OS build.

`StallWatchdog` also runs in the real encode loop. `CodecInput.fill` writes the input through `getInputImage` planes and queues
`min(capacity, stride * slice-height * 3 / 2)` bytes (stride / slice-height from the input format), never `w*h*3`. After
`INFO_OUTPUT_FORMAT_CHANGED` a Main10 job checks `KEY_PROFILE` and falls to the next rung instead of muxing a downgraded stream.

Each job logs `DappleCodec: job: <rung> · <encoder> (hardware|software) | decode <decoder> | demoted: ...` and returns it as
`codecSummary`, which the app stores in the item's `liveInfo`.

## 4. Diagnostics

关于 > long-press 版本 opens a read-only page (declared codecs, ladders, self-test results, last jobs; buttons 运行自检 / 全部重测).
There are no user-facing switches.

## 5. Tests

* JVM: `CodecLadderTest` (ladder ranking, demotion, size/fps filters, AV1 excluded, decode rules, cache round trip, OS update
  invalidation, watchdog, input size), `StreamProbeTest` (profile constants).
* Native on the emulator (`tools/ffmpeg/test-android.sh`): the NDK decoder path against FFmpeg software on 8-bit streams (the
  emulator has no Main10 decoder: NV12 output exercises Annex B conversion, csd, output layout, pts, flush/seek, EOS, release),
  the 10-bit request falling back to software with identical pixels, the 10-bit asset (628 distinct luma levels in a row).
* Flutter: diagnostics page and the long-press route, clip summary, swatches.

## 6. Edited clips (Motion Photo follows the still's edit)

An edited clip (`SoftwareMedia.clip(edit = EditSpec)`) changes what feeds the encoder, not how the ladder is chosen: the decoder is picked as before (hardware first, software fallback), but frames are requested as `RGBAF16` (half-float linear, no truncation) and go through the engine's node graph (`VideoFrameSession`, pipeline-architecture.md section 30); the session packs **P010** for a Main10 rung and **I420** for the 8-bit rungs, at the edit's output size (crop / rotate applied, container rotation baked in, `setOrientationHint(0)`). The encoder is tagged BT.709 / SDR with no HDR static info (an edited clip is display referred). `compat` still forces the 8-bit rungs; the legacy `film` flag only forces them when no `edit` is given. Without `edit` nothing changes. The graph runs on the CPU executor for P010 (the GPU executor writes 8-bit only).

## 7. Declared profile is the candidate, the self-test decides (wave 8)

A hardware 10-bit decoder / Main10 encoder is a candidate when it declares the profile (Main10 or Main10HDR10); P010 in its colour list only ranks it first. The one-time self-test asks for P010 and judges what the codec really delivers (`Decode10.verdict`: P010 and more than 256 distinct luma levels, otherwise the failure names the delivered format). The diagnostics report always carries `main10Encode`: one row per HEVC encoder with `not declared` / `declared, not tested yet` / `passed` / `failed` and the reason. Surface-input encode is not implemented. See pipeline-architecture.md section 30.7.
