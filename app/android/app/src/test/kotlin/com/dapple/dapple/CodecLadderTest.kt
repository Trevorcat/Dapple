package com.dapple.dapple

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/** Ladder ranking, demotion on a failed self-test, and cache invalidation across OS builds. No Android classes involved. */
class CodecLadderTest {
    private val P = ColorFmt.P010
    private val FLEX = ColorFmt.FLEXIBLE

    private fun hevcEnc(name: String, hw: Boolean, profiles: List<Int>, formats: List<Int>, maxW: Int = 4096, maxH: Int = 4096) =
        CodecCaps(name, Mime.HEVC, true, hw, profiles = profiles, colorFormats = formats, minW = 64, minH = 64, maxW = maxW, maxH = maxH)
    private fun avcEnc(name: String, hw: Boolean, profiles: List<Int> = listOf(Prof.AVC_HIGH)) =
        CodecCaps(name, Mime.AVC, true, hw, profiles = profiles, colorFormats = listOf(FLEX), minW = 64, minH = 64, maxW = 4096, maxH = 4096)
    private fun dec(name: String, mime: String, hw: Boolean, profiles: List<Int>, formats: List<Int> = listOf(FLEX)) =
        CodecCaps(name, mime, false, hw, profiles = profiles, colorFormats = formats)

    private val job = EncodeJob(1920, 1080, 30)

    // a phone like the Pixel 5 as declared: HW HEVC encoder lists Main + P010 (but not Main10), software HEVC Main only
    private val pixelLike = listOf(
        hevcEnc("hw.hevc", true, listOf(Prof.HEVC_MAIN), listOf(FLEX, P)),
        hevcEnc("sw.hevc", false, listOf(Prof.HEVC_MAIN), listOf(FLEX)),
        avcEnc("hw.avc", true), avcEnc("sw.avc", false),
    )
    // a flagship: Main10 + P010 in hardware
    private val flagship = listOf(
        hevcEnc("hw.hevc", true, listOf(Prof.HEVC_MAIN, Prof.HEVC_MAIN10, Prof.HEVC_MAIN10_HDR10), listOf(FLEX, P)),
        hevcEnc("sw.hevc", false, listOf(Prof.HEVC_MAIN), listOf(FLEX)),
        avcEnc("hw.avc", true),
    )

    @Test fun declaredMain10WithoutP010OrProfileIsNotOnTheTopRung() {
        val picks = Ladder.encode(pixelLike, job)
        assertEquals(listOf(EncodeRung.HEVC_MAIN, EncodeRung.HEVC_MAIN, EncodeRung.AVC_HIGH, EncodeRung.AVC_HIGH), picks.map { it.rung })
        assertEquals(listOf("hw.hevc", "sw.hevc", "hw.avc", "sw.avc"), picks.map { it.codec })
    }

    @Test fun main10HardwareWinsWhenDeclared() {
        val picks = Ladder.encode(flagship, job)
        assertEquals(EncodeRung.HEVC_MAIN10, picks[0].rung)
        assertEquals("hw.hevc", picks[0].codec)
        assertEquals(Prof.HEVC_MAIN10, picks[0].profile)
        assertEquals(listOf(EncodeRung.HEVC_MAIN10, EncodeRung.HEVC_MAIN, EncodeRung.HEVC_MAIN, EncodeRung.AVC_HIGH), picks.map { it.rung })
    }

    @Test fun compatAndFilmAlwaysUseEightBit() {
        val picks = Ladder.encode(flagship, EncodeJob(1920, 1080, 30, eightBitOnly = true))
        assertTrue(picks.none { it.rung.tenBit })
        assertEquals(EncodeRung.HEVC_MAIN, picks[0].rung)
    }

    @Test fun failedSelfTestDemotesToNextRung() {
        val kv = MapKv()
        val cache = SelfTestCache(kv, "build-A")
        cache.record(SelfTestKind.ENC10, "hw.hevc", false, "no output", 3000)
        val picks = Ladder.encode(flagship, job, cache)
        assertFalse(picks.any { it.rung == EncodeRung.HEVC_MAIN10 })
        assertEquals(EncodeRung.HEVC_MAIN, picks[0].rung)
        assertEquals("hw.hevc", picks[0].codec)   // the Main 8-bit test is a different kind: not affected
        // and when that fails too the H.264 hardware encoder is next
        cache.record(SelfTestKind.ENC8_HEVC, "hw.hevc", false, "stall", 3000)
        cache.record(SelfTestKind.ENC8_HEVC, "sw.hevc", false, "stall", 3000)
        assertEquals("hw.avc", Ladder.encode(flagship, job, cache)[0].codec)
    }

    @Test fun testedSoftwareDoesNotBeatUntestedHardware_butTestedBeatsUntestedWithinHardware() {
        val two = listOf(
            hevcEnc("hwA", true, listOf(Prof.HEVC_MAIN), listOf(FLEX)),
            hevcEnc("hwB", true, listOf(Prof.HEVC_MAIN), listOf(FLEX)),
            hevcEnc("sw", false, listOf(Prof.HEVC_MAIN), listOf(FLEX)),
        )
        val cache = SelfTestCache(MapKv(), "b")
        cache.record(SelfTestKind.ENC8_HEVC, "sw", true, "ok", 10)
        assertEquals(listOf("hwA", "hwB", "sw"), Ladder.encode(two, job, cache).map { it.codec })
        cache.record(SelfTestKind.ENC8_HEVC, "hwB", true, "ok", 10)
        assertEquals(listOf("hwB", "hwA", "sw"), Ladder.encode(two, job, cache).map { it.codec })
    }

    @Test fun sizeAndFrameRateLimitsFilterEncoders() {
        val small = listOf(hevcEnc("tiny", true, listOf(Prof.HEVC_MAIN), listOf(FLEX), maxW = 1280, maxH = 720), avcEnc("avc", true))
        assertEquals(listOf("avc"), Ladder.encode(small, job).map { it.codec })
        val slow = listOf(CodecCaps("slow", Mime.HEVC, true, true, profiles = listOf(1), colorFormats = listOf(FLEX), maxW = 4096, maxH = 4096, maxFps1080 = 24))
        assertTrue(Ladder.encode(slow, job).isEmpty())
    }

    @Test fun av1AndVp9AreNeverChosen() {
        val caps = listOf(CodecCaps("av1.enc", Mime.AV1, true, true, colorFormats = listOf(FLEX, P)), CodecCaps("vp9.enc", Mime.VP9, true, true, colorFormats = listOf(FLEX)))
        assertTrue(Ladder.encode(caps, job).isEmpty())
    }

    @Test fun hdrSourcePicksHdr10ProfileOnlyWhenMain10IsNotDeclared() {
        val hdrOnly = listOf(hevcEnc("e", true, listOf(Prof.HEVC_MAIN10_HDR10), listOf(FLEX, P)))
        assertEquals(Prof.HEVC_MAIN10_HDR10, Ladder.encode(hdrOnly, EncodeJob(1920, 1080, 30, hdrSource = true))[0].profile)
        assertEquals(Prof.HEVC_MAIN10, Ladder.encode(flagship, EncodeJob(1920, 1080, 30, hdrSource = true))[0].profile)
    }

    // ---- decode ladder ---------------------------------------------------------------------------------------------------
    private val hevc10 = StreamSpec(Mime.HEVC, Prof.HEVC_MAIN10, 10, 1)
    private val decoders = listOf(
        dec("hw.hevc.dec", Mime.HEVC, true, listOf(Prof.HEVC_MAIN, Prof.HEVC_MAIN10, Prof.HEVC_MAIN10_HDR10), listOf(FLEX, P)),
        dec("sw.hevc.dec", Mime.HEVC, false, listOf(Prof.HEVC_MAIN)),
        dec("hw.avc.dec", Mime.AVC, true, listOf(Prof.AVC_HIGH)),
    )

    @Test fun tenBitHardwareDecodeUsesNativeP010AndSoftwareIsLast() {
        val picks = Ladder.decode(decoders, hevc10)
        assertEquals(listOf(DecodeRoute.HW_NATIVE_P010, DecodeRoute.SOFTWARE), picks.map { it.route })
        assertEquals("hw.hevc.dec", picks[0].codec)
        assertEquals(2, picks[0].route.hwPref)
    }

    @Test fun decoderWithoutP010OrWithFailedSelfTestFallsToSoftware() {
        // no P010 in the colour list is no longer a reason to skip the hardware decoder: the self-test decides (see note10Plus tests below)
        val noP010 = decoders.map { if (it.name == "hw.hevc.dec") it.copy(colorFormats = listOf(FLEX)) else it }
        assertEquals(listOf(DecodeRoute.HW_NATIVE_P010, DecodeRoute.SOFTWARE), Ladder.decode(noP010, hevc10).map { it.route })
        val cache = SelfTestCache(MapKv(), "b")
        cache.record(SelfTestKind.DEC10, "hw.hevc.dec", false, "8-bit picture", 100)
        assertEquals(listOf(DecodeRoute.SOFTWARE), Ladder.decode(decoders, hevc10, cache).map { it.route })
    }

    @Test fun eightBitUsesFfmpegMediaCodecWrapper() {
        assertEquals(listOf(DecodeRoute.HW_FFMPEG, DecodeRoute.SOFTWARE), Ladder.decode(decoders, StreamSpec(Mime.HEVC, 0, 8, 1)).map { it.route })
        assertEquals(listOf(DecodeRoute.HW_FFMPEG, DecodeRoute.SOFTWARE), Ladder.decode(decoders, StreamSpec(Mime.AVC, 0, 8, 1)).map { it.route })
    }

    @Test fun sonyFourTwoTwoAndUndeclaredProfilesAreSoftware() {
        val sony = StreamSpec(Mime.AVC, Prof.AVC_HIGH422, 10, 2)
        assertEquals(listOf(DecodeRoute.SOFTWARE), Ladder.decode(decoders, sony).map { it.route })
        // a decoder that declares only High10 must not be taken for High 4:2:2
        val high10 = listOf(dec("avc10", Mime.AVC, true, listOf(Prof.AVC_HIGH, Prof.AVC_HIGH10)))
        assertEquals(listOf(DecodeRoute.SOFTWARE), Ladder.decode(high10, sony).map { it.route })
        // 10-bit 4:2:0 H.264 needs High10; P010 in the colour list is not required (the self-test decides), but a High10-less decoder is software
        assertEquals(listOf(DecodeRoute.HW_NATIVE_P010, DecodeRoute.SOFTWARE), Ladder.decode(high10, StreamSpec(Mime.AVC, Prof.AVC_HIGH10, 10, 1)).map { it.route })
        assertEquals(listOf(DecodeRoute.SOFTWARE), Ladder.decode(listOf(dec("avc8", Mime.AVC, true, listOf(Prof.AVC_HIGH))), StreamSpec(Mime.AVC, Prof.AVC_HIGH10, 10, 1)).map { it.route })
        val high10p = listOf(dec("avc10", Mime.AVC, true, listOf(Prof.AVC_HIGH10), listOf(FLEX, P)))
        assertEquals(DecodeRoute.HW_NATIVE_P010, Ladder.decode(high10p, StreamSpec(Mime.AVC, Prof.AVC_HIGH10, 10, 1))[0].route)
        // requiredProfile -1 (no Android constant): software
        assertEquals(listOf(DecodeRoute.SOFTWARE), Ladder.decode(decoders, StreamSpec(Mime.HEVC, -1, 12, 3)).map { it.route })
    }

    @Test fun softwareOnlyPlatformDecoderIsNotAHardwareRoute() {
        val onlySw = listOf(dec("c2.android.hevc.decoder", Mime.HEVC, false, listOf(Prof.HEVC_MAIN)))
        assertEquals(listOf(DecodeRoute.SOFTWARE), Ladder.decode(onlySw, StreamSpec(Mime.HEVC, 0, 8, 1)).map { it.route })
    }

    // ---- cache ----------------------------------------------------------------------------------------------------------
    @Test fun cacheRoundTripAndKeyedByFingerprintAndCodec() {
        val kv = MapKv()
        var now = 1000L
        val a = SelfTestCache(kv, "google/redfin/UP1A.231105", clock = { now })
        assertEquals(TestState.UNKNOWN, a.state(SelfTestKind.ENC10, "c2.qti.hevc.encoder"))
        a.record(SelfTestKind.ENC10, "c2.qti.hevc.encoder", false, "profile Main is not Main10\nsecond line", 3012)
        a.record(SelfTestKind.DEC10, "c2.qti.hevc.decoder", true, "ok | 640 levels", 120)
        val r = a.result(SelfTestKind.ENC10, "c2.qti.hevc.encoder")!!
        assertFalse(r.pass); assertEquals(3012, r.ms); assertEquals(1000, r.atMs)
        assertEquals("profile Main is not Main10 second line", r.detail)
        assertEquals("ok | 640 levels", a.result(SelfTestKind.DEC10, "c2.qti.hevc.decoder")!!.detail)
        assertEquals(TestState.UNKNOWN, a.state(SelfTestKind.ENC10, "c2.android.hevc.encoder"))
        assertEquals(TestState.UNKNOWN, a.state(SelfTestKind.ENC8_HEVC, "c2.qti.hevc.encoder"))
        assertEquals(2, a.all().size)
    }

    @Test fun osUpdateInvalidatesEveryResult() {
        val kv = MapKv()
        val before = SelfTestCache(kv, "google/redfin/UP1A.231105")
        before.record(SelfTestKind.ENC10, "enc", true, "ok", 10)
        before.record(SelfTestKind.DEC10, "dec", false, "bad", 10)
        // same codec names after an OS update: nothing is trusted, the ladders treat them as untested again
        val after = SelfTestCache(kv, "google/redfin/UP1A.240105")
        assertEquals(TestState.UNKNOWN, after.state(SelfTestKind.ENC10, "enc"))
        assertEquals(TestState.UNKNOWN, after.state(SelfTestKind.DEC10, "dec"))
        assertEquals(2, after.purgeStale())
        assertEquals(0, kv.keys().size)
        // a previously failed codec is offered (and tested) again
        val caps = listOf(dec("dec", Mime.HEVC, true, listOf(Prof.HEVC_MAIN10), listOf(P)))
        assertEquals(DecodeRoute.HW_NATIVE_P010, Ladder.decode(caps, hevc10, after)[0].route)
    }

    @Test fun purgeKeepsCurrentFingerprintAndForeignKeys() {
        val kv = MapKv()
        kv.put("unrelated", "x")
        val c = SelfTestCache(kv, "cur")
        c.record(SelfTestKind.ENC10, "e", true, "ok", 1)
        SelfTestCache(kv, "old").record(SelfTestKind.ENC10, "e", true, "ok", 1)
        assertEquals(1, c.purgeStale())
        assertEquals(TestState.PASS, c.state(SelfTestKind.ENC10, "e"))
        assertEquals("x", kv.get("unrelated"))
    }

    // ---- watchdog & layout ---------------------------------------------------------------------------------------------
    @Test fun watchdogFiresWhenEightFramesGetNoOutputFor3s() {
        val w = StallWatchdog()
        repeat(7) { w.onQueued() }
        assertFalse(w.stalled(0)); assertFalse(w.stalled(10_000))      // fewer than 8 pending: slow source, not a stall
        w.onQueued()
        assertFalse(w.stalled(10_000))                                   // armed now
        assertFalse(w.stalled(12_999))
        assertTrue(w.stalled(13_000))
    }

    @Test fun watchdogResetsOnOutputAndOnEos() {
        val w = StallWatchdog()
        repeat(8) { w.onQueued() }
        assertFalse(w.stalled(0))
        w.onOutput(2_000)                                                // progress: pending 7
        assertFalse(w.stalled(100_000))
        w.onQueued()                                                     // pending 8 again
        assertFalse(w.stalled(100_000))                                  // armed here
        assertFalse(w.stalled(102_999))
        assertTrue(w.stalled(103_000))
        // end of stream with frames still pending counts too (the self-test queues 8 frames then EOS)
        val e = StallWatchdog()
        repeat(3) { e.onQueued() }
        e.onEos()
        assertFalse(e.stalled(5_000)); assertTrue(e.stalled(8_000))
        repeat(3) { e.onOutput(8_100) }
        assertFalse(e.stalled(50_000))
    }

    @Test fun inputBytesFollowStrideAndSliceHeightNotWidthTimesHeight() {
        // Pixel 5 HEVC encoder, P010 1920x1080: stride 3840 bytes, slice height 1088
        val n = inputFrameBytes(3840, 1088, 7_000_000)
        assertEquals(3840 * 1088 * 3 / 2, n)
        assertTrue(n != 1920 * 1080 * 3)
        assertEquals(1_000_000, inputFrameBytes(3840, 1088, 1_000_000))   // never more than the buffer holds
    }

    @Test fun main10ProfileValues() {
        assertTrue(isMain10Profile(2)); assertTrue(isMain10Profile(0x1000)); assertTrue(isMain10Profile(0x2000))
        assertFalse(isMain10Profile(1)); assertFalse(isMain10Profile(-1))
    }

    @Test fun unknownCodecHasNoStateAndNoneLedgerIsEmpty() {
        assertEquals(TestState.UNKNOWN, TestLedger.NONE.state("x", "y"))
        assertNull(SelfTestCache(MapKv(), "f").result("a", "b"))
    }

    // ---- Samsung Galaxy Note10+ (Android 12, Snapdragon 855) as declared, docs/engine/device-results/note10plus.md section 5 ----------------------
    private val QCOM_VENDOR = 0x7fa30c04  // a Qualcomm vendor colour format (not P010)
    private val note10Plus = listOf(
        // HW HEVC encoder: Main + Main10HDR10, no P010 in the colour list (Surface, Flexible, SemiPlanar, vendor)
        hevcEnc("OMX.qcom.video.encoder.hevc", true, listOf(Prof.HEVC_MAIN, Prof.HEVC_MAIN10_HDR10), listOf(ColorFmt.SURFACE, FLEX, ColorFmt.NV12, QCOM_VENDOR), 4096, 2304),
        hevcEnc("c2.android.hevc.encoder", false, listOf(Prof.HEVC_MAIN, 4), listOf(FLEX), 512, 512),
        avcEnc("OMX.qcom.video.encoder.avc", true), avcEnc("c2.android.avc.encoder", false),
        dec("OMX.qcom.video.decoder.hevc", Mime.HEVC, true, listOf(Prof.HEVC_MAIN, Prof.HEVC_MAIN10, Prof.HEVC_MAIN10_HDR10), listOf(FLEX, QCOM_VENDOR, ColorFmt.SURFACE)),
        dec("OMX.qcom.video.decoder.avc", Mime.AVC, true, listOf(1, 2, Prof.AVC_HIGH)),
        dec("c2.android.hevc.decoder", Mime.HEVC, false, listOf(Prof.HEVC_MAIN, Prof.HEVC_MAIN10)),
    )

    @Test fun note10PlusHardwareMain10DecoderIsTriedEvenWithoutP010InItsList() {
        val picks = Ladder.decode(note10Plus, hevc10)
        assertEquals(listOf(DecodeRoute.HW_NATIVE_P010, DecodeRoute.SOFTWARE), picks.map { it.route })
        assertEquals("OMX.qcom.video.decoder.hevc", picks[0].codec)
    }

    @Test fun note10PlusDecoderThatDeliversAVendorFormatFailsWithThatFormatInTheReasonAndSoftwareTakesOver() {
        val v = Decode10.verdict("OMX.qcom.video.decoder.hevc", QCOM_VENDOR, 700, listOf(FLEX, QCOM_VENDOR, ColorFmt.SURFACE))
        assertEquals(false, v.first)
        assertTrue(v.second, v.second.contains("0x7fa30c04") && v.second.contains("only P010"))
        val cache = SelfTestCache(MapKv(), "n10")
        cache.record(SelfTestKind.DEC10, "OMX.qcom.video.decoder.hevc", v.first, v.second, 40)
        assertEquals(listOf(DecodeRoute.SOFTWARE), Ladder.decode(note10Plus, hevc10, cache).map { it.route })
    }

    @Test fun decode10VerdictAcceptsP010WithMoreThan256LevelsOnly() {
        assertEquals(true, Decode10.verdict("c", ColorFmt.P010, 640, listOf(ColorFmt.P010)).first)
        assertEquals(false, Decode10.verdict("c", ColorFmt.P010, 220, listOf(ColorFmt.P010)).first)   // truncated to 8 bit
        assertEquals(false, Decode10.verdict("c", FLEX, 640, listOf(FLEX)).first)                       // 8-bit flexible for a 10-bit stream
        assertEquals(false, Decode10.verdict("c", -1, 0, emptyList()).first)
    }

    @Test fun note10PlusMain10HdrEncoderIsOfferedAsMain10CandidateAfterCodecsListingP010_andAskedForItsDeclaredProfile() {
        val picks = Ladder.encode(note10Plus, job)
        assertEquals(EncodeRung.HEVC_MAIN10, picks[0].rung)
        assertEquals("OMX.qcom.video.encoder.hevc", picks[0].codec)
        assertEquals(Prof.HEVC_MAIN10_HDR10, picks[0].profile)      // only Main10HDR10 declared: ask for that, not Main10
        // a codec that lists P010 outranks one that does not
        val withP010 = note10Plus + hevcEnc("other.hevc", true, listOf(Prof.HEVC_MAIN10), listOf(FLEX, P))
        assertEquals("other.hevc", Ladder.encode(withP010, job)[0].codec)
        // after a failed self-test the phone is back to 8-bit HEVC
        val cache = SelfTestCache(MapKv(), "n10")
        cache.record(SelfTestKind.ENC10, "OMX.qcom.video.encoder.hevc", false, "configure failed", 20)
        assertEquals(EncodeRung.HEVC_MAIN, Ladder.encode(note10Plus, job, cache)[0].rung)
    }

    @Test fun main10EncodeRowAlwaysExistsWithStateAndReason() {
        val cache = SelfTestCache(MapKv(), "n10")
        fun rows() = Diagnostics.main10EncodeRows(note10Plus) { k, c -> cache.result(k, c) }
        // before any test: one row per HEVC encoder
        var r = rows()
        assertEquals(listOf("OMX.qcom.video.encoder.hevc", "c2.android.hevc.encoder"), r.map { it.codec })
        assertEquals(Diagnostics.UNTESTED, r[0].state)
        assertTrue(r[0].reason, r[0].reason.contains("Main10HDR10") && r[0].reason.contains("Surface"))
        assertEquals(Diagnostics.NOT_DECLARED, r[1].state)
        cache.record(SelfTestKind.ENC10, "OMX.qcom.video.encoder.hevc", false, "configure failed: -10000 (Unsupported eColorFormat)", 12)
        r = rows()
        assertEquals(Diagnostics.UNTESTED, rows()[0].state)           // the Surface mode of this codec is still untested
        cache.record(SelfTestKind.ENC10_SURFACE, "OMX.qcom.video.encoder.hevc", false, "no 10-bit EGL surface", 5)
        r = rows()
        assertEquals(Diagnostics.FAILED, r[0].state)
        assertTrue(r[0].reason, r[0].reason.contains("Unsupported eColorFormat") && r[0].reason.contains("no 10-bit EGL surface"))
        cache.record(SelfTestKind.ENC10_SURFACE, "OMX.qcom.video.encoder.hevc", true, "surface input, 10-bit EGL, 8 frames, profile Main10HDR10, 640 levels", 300)
        r = rows()
        assertEquals(Diagnostics.PASSED, r[0].state)
        assertTrue(r[0].reason, r[0].reason.startsWith("input mode passed: surface"))
        cache.record(SelfTestKind.ENC10, "OMX.qcom.video.encoder.hevc", true, "8 frames", 300)
        assertTrue(rows()[0].reason.startsWith("input mode passed: buffer P010 + surface"))
        // a device with no HEVC encoder at all still has the row
        val none = Diagnostics.main10EncodeRows(listOf(avcEnc("a", true))) { _, _ -> null }
        assertEquals(1, none.size)
        assertEquals(Diagnostics.NOT_DECLARED, none[0].state)
    }

    // ---- Surface input (Main10 only through an input Surface, like many hardware encoders) ----------------------------------------------------------
    private val surfaceOnly = listOf(hevcEnc("hw.hevc.surface", true, listOf(Prof.HEVC_MAIN, Prof.HEVC_MAIN10), listOf(ColorFmt.SURFACE, FLEX)), avcEnc("hw.avc", true))

    @Test fun surfaceModeIsOfferedOnlyToJobsThatCanFeedASurface() {
        val bufferJob = Ladder.encode(surfaceOnly, EncodeJob(1920, 1080, 30))
        assertEquals(listOf(InputMode.BUFFER), bufferJob.filter { it.rung == EncodeRung.HEVC_MAIN10 }.map { it.input })
        val edited = Ladder.encode(surfaceOnly, EncodeJob(1920, 1080, 30, surfaceOk = true)).filter { it.rung == EncodeRung.HEVC_MAIN10 }
        // a codec that does not list P010 tries its Surface input first
        assertEquals(listOf(InputMode.SURFACE, InputMode.BUFFER), edited.map { it.input })
        assertEquals(listOf(SelfTestKind.ENC10_SURFACE, SelfTestKind.ENC10), edited.map { it.testKind })
    }

    @Test fun eachInputModeHasItsOwnVerdictAndDemotion() {
        val cache = SelfTestCache(MapKv(), "dev")
        cache.record(SelfTestKind.ENC10_SURFACE, "hw.hevc.surface", false, "no 10-bit EGL surface", 4)
        val picks = Ladder.encode(surfaceOnly, EncodeJob(1920, 1080, 30, surfaceOk = true), cache).filter { it.rung == EncodeRung.HEVC_MAIN10 }
        assertEquals(listOf(InputMode.BUFFER), picks.map { it.input })        // surface failed: only the buffer mode is left
        cache.record(SelfTestKind.ENC10, "hw.hevc.surface", false, "configure failed", 4)
        assertEquals(EncodeRung.HEVC_MAIN, Ladder.encode(surfaceOnly, EncodeJob(1920, 1080, 30, surfaceOk = true), cache)[0].rung)  // both failed: 8-bit HEVC
        val passed = SelfTestCache(MapKv(), "dev2")
        passed.record(SelfTestKind.ENC10, "hw.hevc.surface", true, "ok", 4)
        passed.record(SelfTestKind.ENC10_SURFACE, "hw.hevc.surface", true, "ok", 4)
        assertEquals(InputMode.BUFFER, Ladder.encode(surfaceOnly, EncodeJob(1920, 1080, 30, surfaceOk = true), passed)[0].input)  // both passed: the buffer path first
    }

    @Test fun editFormatFollowsRungDepthAndHdr() {
        assertEquals(EditFormat.P010, EditFormat.forEncoder(true, false))
        assertEquals(EditFormat.P010_HDR, EditFormat.forEncoder(true, true))
        assertEquals(EditFormat.I420, EditFormat.forEncoder(false, true))     // an 8-bit rung is always SDR
    }
}
