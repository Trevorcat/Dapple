package com.dapple.dapple

/*
 * Codec capability layer, pure part (no Android classes, JVM unit tested): what a codec declares, how the encode and decode
 * ladders rank the candidates for one job, and the cache of one-time self-test results. The Android side that fills
 * [CodecCaps] from MediaCodecList and runs the self-tests is CodecManager.kt / CodecSelfTest.kt.
 *
 * Owner principle: codecs adapt to what each device declares, verified by a one-time self-test, never hard-coded by model.
 */

object Mime {
    const val AVC = "video/avc"
    const val HEVC = "video/hevc"
    const val AV1 = "video/av01"
    const val VP9 = "video/x-vnd.on2.vp9"
}

object Prof {
    const val HEVC_MAIN = 1
    const val HEVC_MAIN10 = 2
    const val HEVC_MAIN10_HDR10 = 0x1000
    const val HEVC_MAIN10_HDR10_PLUS = 0x2000
    const val AVC_BASELINE = 1
    const val AVC_MAIN = 2
    const val AVC_HIGH = 8
    const val AVC_HIGH10 = 0x10
    const val AVC_HIGH422 = 0x20
    const val AVC_HIGH444 = 0x40

    fun isHevcMain10(p: Int) = p == HEVC_MAIN10 || p == HEVC_MAIN10_HDR10 || p == HEVC_MAIN10_HDR10_PLUS

    fun hevcName(p: Int) = when (p) { 1 -> "Main"; 2 -> "Main10"; 4 -> "MainStill"; 0x1000 -> "Main10HDR10"; 0x2000 -> "Main10HDR10Plus"; else -> "0x" + p.toString(16) }
}

object ColorFmt {
    const val NV12 = 21                 // COLOR_FormatYUV420SemiPlanar
    const val I420 = 19                 // COLOR_FormatYUV420Planar
    const val FLEXIBLE = 0x7F420888     // COLOR_FormatYUV420Flexible
    const val SURFACE = 0x7F000789      // COLOR_FormatSurface
    const val P010 = 54                 // COLOR_FormatYUVP010
}

/** One codec as MediaCodecList declares it (a snapshot, so the ladders can be tested without a device). */
data class CodecCaps(
    val name: String,
    val mime: String,
    val encoder: Boolean,
    val hardware: Boolean,
    val softwareOnly: Boolean = !hardware,
    val profiles: List<Int> = emptyList(),
    val colorFormats: List<Int> = emptyList(),
    val minW: Int = 0, val minH: Int = 0,
    val maxW: Int = 0, val maxH: Int = 0,
    /** Highest frame rate declared for 1080p (0 = unknown). */
    val maxFps1080: Int = 0,
    /** Number of declared performance points (API 29+). */
    val perfPoints: Int = 0,
    /** FEATURE_HdrEditing (API 33+). */
    val hdrEditing: Boolean = false,
) {
    fun declaresProfile(p: Int): Boolean = when (p) {
        0 -> true
        Prof.HEVC_MAIN10 -> profiles.any { Prof.isHevcMain10(it) }
        else -> profiles.contains(p)
    }
    val hasP010 get() = colorFormats.contains(ColorFmt.P010)
    val hasSurface get() = colorFormats.contains(ColorFmt.SURFACE)
    val has8BitInput get() = colorFormats.any { it == ColorFmt.FLEXIBLE || it == ColorFmt.NV12 || it == ColorFmt.I420 }

    fun supportsSize(w: Int, h: Int, fps: Int): Boolean {
        if (maxW > 0 && (w > maxW || w < minW)) return false
        if (maxH > 0 && (h > maxH || h < minH)) return false
        if (maxFps1080 > 0 && w * h <= 1920 * 1088 && fps > maxFps1080) return false
        return true
    }
}

/** What one job needs from the encoder. */
data class EncodeJob(
    val w: Int, val h: Int, val fps: Int,
    /** HLG / PQ source: 10-bit with the HDR tags if possible. */
    val hdrSource: Boolean = false,
    /** The 兼容版 export and the film-applied path always use 8-bit. */
    val eightBitOnly: Boolean = false,
    /** The frames come from the engine session (an edited clip), so they can be packed as RGB10A2 and fed through an encoder input Surface. */
    val surfaceOk: Boolean = false,
)

/** How the frames reach the encoder: ByteBuffers (P010 / I420) or the encoder's input Surface (RGB10A2 through EGL, SurfaceInput.kt). */
enum class InputMode { BUFFER, SURFACE }

enum class EncodeRung(val label: String, val mime: String, val tenBit: Boolean) {
    HEVC_MAIN10("HEVC Main10", Mime.HEVC, true),
    HEVC_MAIN("HEVC Main 8-bit", Mime.HEVC, false),
    AVC_HIGH("H.264 High 8-bit", Mime.AVC, false),
}

data class EncodePick(val rung: EncodeRung, val codec: String, val hardware: Boolean, val profile: Int, val input: InputMode = InputMode.BUFFER) {
    /** Self-test kind recorded for this pick (10-bit: one per input mode, so the diagnostics say which one passed). */
    val testKind get() = if (rung.tenBit) (if (input == InputMode.SURFACE) SelfTestKind.ENC10_SURFACE else SelfTestKind.ENC10) else if (rung == EncodeRung.HEVC_MAIN) SelfTestKind.ENC8_HEVC else SelfTestKind.ENC8_AVC
}

/** What the stream to decode needs (from its SPS). requiredProfile 0 = any, -1 = no platform profile exists for it. */
data class StreamSpec(val mime: String, val requiredProfile: Int, val bitDepth: Int, val chroma: Int)

/** SwVideo hwPref values. */
enum class DecodeRoute(val hwPref: Int, val label: String) {
    /** NDK AMediaCodec, COLOR_FormatYUVP010 output (10-bit 4:2:0). */
    HW_NATIVE_P010(2, "hardware 10-bit (P010)"),
    /** FFmpeg's MediaCodec wrapper (8-bit 4:2:0 only). */
    HW_FFMPEG(1, "hardware"),
    /** FFmpeg software decoder (any depth / chroma). */
    SOFTWARE(0, "software"),
}

data class DecodePick(val route: DecodeRoute, val codec: String?)

object SelfTestKind {
    const val ENC10 = "enc10"                  // HEVC Main10 through ByteBuffer P010 input
    const val ENC10_SURFACE = "enc10s"         // HEVC Main10 through an input Surface (RGB10A2 via EGL)
    const val ENC8_HEVC = "enc8hevc"
    const val ENC8_AVC = "enc8avc"
    const val DEC10 = "dec10"
}

enum class TestState { UNKNOWN, PASS, FAIL }

/** Read side of the self-test cache, as the ladders need it. */
fun interface TestLedger {
    fun state(kind: String, codec: String): TestState

    companion object { val NONE = TestLedger { _, _ -> TestState.UNKNOWN } }
}

object Ladder {
    private fun order(c: CodecCaps, ledger: TestLedger, kind: String): Int =
        (if (c.hardware) 0 else 10) + (if (ledger.state(kind, c.name) == TestState.PASS) 0 else 1)

    /**
     * Encoders for [job] in the order to try them: HEVC Main10 (P010 input) -> HEVC Main 8-bit -> H.264 High 8-bit. Within a
     * rung hardware comes first, and a codec that already passed its self-test before an untested one of the same kind. A codec
     * whose self-test failed is left out (demoted). AV1 / VP9 are never chosen (Motion Photo needs the compatible codecs).
     */
    fun encode(caps: List<CodecCaps>, job: EncodeJob, ledger: TestLedger = TestLedger.NONE): List<EncodePick> {
        val out = ArrayList<EncodePick>()
        for (rung in EncodeRung.values()) {
            if (rung.tenBit && job.eightBitOnly) continue
            val kind = EncodePick(rung, "", false, 0).testKind
            val kindS = SelfTestKind.ENC10_SURFACE
            caps.filter { it.encoder && it.mime == rung.mime && it.supportsSize(job.w, job.h, job.fps) }
                .filter { c ->
                    when (rung) {
                        // a declared Main10 (or Main10HDR10) profile is enough: whether the codec really takes P010 input although its colour list does not say so
                        // (OMX.qcom.video.encoder.hevc of the Note10+) is for the one-time self-test to decide, so such codecs come after the ones that list P010
                        EncodeRung.HEVC_MAIN10 -> c.profiles.any { Prof.isHevcMain10(it) }
                        EncodeRung.HEVC_MAIN -> c.has8BitInput && (c.profiles.isEmpty() || c.profiles.contains(Prof.HEVC_MAIN))
                        EncodeRung.AVC_HIGH -> c.has8BitInput
                    }
                }
                .filter { rung == EncodeRung.HEVC_MAIN10 || ledger.state(kind, it.name) != TestState.FAIL }
                .sortedBy {
                    if (rung != EncodeRung.HEVC_MAIN10) order(it, ledger, kind) * 2
                    else {
                        val pass = ledger.state(kind, it.name) == TestState.PASS || ledger.state(kindS, it.name) == TestState.PASS
                        (if (it.hardware) 0 else 10) + (if (pass) 0 else 2) + (if (it.hasP010) 0 else 1)
                    }
                }
                .forEach { c ->
                    val profile = when (rung) {
                        EncodeRung.HEVC_MAIN10 ->
                            // a codec that declares only Main10HDR10 gets asked for exactly that (asking for Main10 makes it reset the profile silently)
                            if (c.profiles.contains(Prof.HEVC_MAIN10_HDR10) && !c.profiles.contains(Prof.HEVC_MAIN10)) Prof.HEVC_MAIN10_HDR10 else Prof.HEVC_MAIN10
                        EncodeRung.HEVC_MAIN -> Prof.HEVC_MAIN
                        EncodeRung.AVC_HIGH -> if (c.profiles.contains(Prof.AVC_HIGH)) Prof.AVC_HIGH else 0
                    }
                    if (rung != EncodeRung.HEVC_MAIN10) {
                        out.add(EncodePick(rung, c.name, c.hardware, profile))
                    } else {
                        // Main10 input modes of this codec, best first. The colour list says what the codec declares; the self-test of each mode decides:
                        //  buffer P010: a codec that does not list P010 is still tried once (it may accept it), a failed test removes the mode;
                        //  surface: only when the codec declares Surface input and the job can feed one; a failed test removes the mode.
                        val bufferState = ledger.state(kind, c.name)
                        val surfaceState = ledger.state(kindS, c.name)
                        val modes = ArrayList<InputMode>()
                        if (bufferState != TestState.FAIL) modes.add(InputMode.BUFFER)
                        if (job.surfaceOk && c.hasSurface && surfaceState != TestState.FAIL) modes.add(InputMode.SURFACE)
                        val surfaceFirst = surfaceState == TestState.PASS && bufferState != TestState.PASS ||
                            (bufferState != TestState.PASS && surfaceState != TestState.PASS && !c.hasP010 && c.hasSurface)
                        if (surfaceFirst) modes.sortBy { if (it == InputMode.SURFACE) 0 else 1 }
                        modes.forEach { out.add(EncodePick(rung, c.name, c.hardware, profile, it)) }
                    }
                }
        }
        return out
    }

    /**
     * Decoders for [stream] in the order to try them. 8-bit 4:2:0: a hardware decoder that declares the profile -> FFmpeg's
     * MediaCodec wrapper. 10-bit 4:2:0: a hardware decoder that declares the profile AND lists P010 output (and has not failed
     * its self-test) -> NDK AMediaCodec with P010 output. Everything else, and the last rung always: FFmpeg software.
     */
    fun decode(caps: List<CodecCaps>, stream: StreamSpec, ledger: TestLedger = TestLedger.NONE): List<DecodePick> {
        val out = ArrayList<DecodePick>()
        val hwEligible = stream.requiredProfile >= 0 && stream.chroma == 1 && (stream.mime == Mime.AVC || stream.mime == Mime.HEVC)
        if (hwEligible) {
            val dec = caps.filter { !it.encoder && it.mime == stream.mime && it.hardware && it.declaresProfile(stream.requiredProfile) }
            if (stream.bitDepth <= 8) {
                dec.firstOrNull()?.let { out.add(DecodePick(DecodeRoute.HW_FFMPEG, it.name)) }
            } else if (stream.bitDepth == 10) {
                // Every hardware decoder that declares the profile is tried; P010 in its colour list is NOT required (OMX.qcom.video.decoder.hevc declares Main10
                // and no P010). The self-test asks for P010 and records the format the decoder really delivers; only P010 is accepted (Decode10.verdict), so a
                // decoder that offers a vendor 10-bit layout fails with that format in the reason and software takes over. Codecs listing P010 come first.
                dec.filter { ledger.state(SelfTestKind.DEC10, it.name) != TestState.FAIL }
                    .sortedBy { (if (ledger.state(SelfTestKind.DEC10, it.name) == TestState.PASS) 0 else 2) + (if (it.hasP010) 0 else 1) }
                    .forEach { out.add(DecodePick(DecodeRoute.HW_NATIVE_P010, it.name)) }
            }
        }
        out.add(DecodePick(DecodeRoute.SOFTWARE, null))
        return out
    }
}

// ---- self-test cache ------------------------------------------------------------------------------------------------

/** Key / value storage behind the cache (SharedPreferences on Android, a map in tests). */
interface KvStore {
    fun get(key: String): String?
    fun put(key: String, value: String)
    fun remove(key: String)
    fun keys(): Set<String>
}

class MapKv : KvStore {
    val map = LinkedHashMap<String, String>()
    override fun get(key: String) = map[key]
    override fun put(key: String, value: String) { map[key] = value }
    override fun remove(key: String) { map.remove(key) }
    override fun keys(): Set<String> = map.keys.toSet()
}

data class TestResult(val pass: Boolean, val detail: String, val ms: Long, val atMs: Long)

/**
 * Self-test results keyed by Build.FINGERPRINT + codec name + test kind. After an OS update the fingerprint differs, so no old
 * result is found (and [purgeStale] drops them): every capability is verified again before its first use.
 */
class SelfTestCache(private val kv: KvStore, val fingerprint: String, private val clock: () -> Long = { System.currentTimeMillis() }) : TestLedger {
    private companion object { const val P = "st|" }

    private fun key(kind: String, codec: String) = "$P$fingerprint|$codec|$kind"

    fun result(kind: String, codec: String): TestResult? {
        val raw = kv.get(key(kind, codec)) ?: return null
        val f = raw.split('|', limit = 4)
        if (f.size < 4) return null
        return TestResult(f[0] == "1", f[3], f[1].toLongOrNull() ?: 0, f[2].toLongOrNull() ?: 0)
    }

    override fun state(kind: String, codec: String): TestState = result(kind, codec)?.let { if (it.pass) TestState.PASS else TestState.FAIL } ?: TestState.UNKNOWN

    fun record(kind: String, codec: String, pass: Boolean, detail: String, ms: Long) {
        kv.put(key(kind, codec), "${if (pass) 1 else 0}|$ms|${clock()}|${detail.replace('\n', ' ')}")
    }

    /** Removes results recorded under another fingerprint; returns how many. */
    fun purgeStale(): Int {
        var n = 0
        for (k in kv.keys()) if (k.startsWith(P) && !k.startsWith("$P$fingerprint|")) { kv.remove(k); n++ }
        return n
    }

    /** (kind, codec, result) for the current fingerprint. */
    fun all(): List<Triple<String, String, TestResult>> = kv.keys().filter { it.startsWith("$P$fingerprint|") }.sorted().mapNotNull { k ->
        val rest = k.removePrefix("$P$fingerprint|")
        val i = rest.lastIndexOf('|')
        if (i < 0) return@mapNotNull null
        val codec = rest.substring(0, i)
        val kind = rest.substring(i + 1)
        result(kind, codec)?.let { Triple(kind, codec, it) }
    }
}

// ---- watchdog -------------------------------------------------------------------------------------------------------

/**
 * Detects an encoder that accepts input and silently never produces output (the Pixel 5 hardware HEVC encoder does this
 * instead of erroring). Stalled = 8 or more frames queued without output (or the stream ended with frames still pending)
 * and no output for [stallMs]. Poll [stalled] from the encode loop; feed [onQueued] / [onOutput] / [onEos].
 */
class StallWatchdog(private val maxPending: Int = 8, private val stallMs: Long = 3000) {
    private var queued = 0
    private var output = 0
    private var eos = false
    private var lastOutputAt = -1L
    private var armedAt = -1L

    val pending get() = queued - output
    val outputs get() = output

    fun onQueued() { queued++ }
    fun onOutput(now: Long) { output++; lastOutputAt = now; armedAt = -1 }
    fun onEos() { eos = true }

    fun stalled(now: Long): Boolean {
        val cond = pending >= maxPending || (eos && pending > 0)
        if (!cond) { armedAt = -1; return false }
        if (armedAt < 0) armedAt = now
        return now - maxOf(armedAt, lastOutputAt) >= stallMs
    }
}

/** HEVC output profiles that really are 10-bit. */
fun isMain10Profile(p: Int) = Prof.isHevcMain10(p)

/** Bytes to queue for one raw 4:2:0 input frame given the codec's input layout (stride * slice-height, never w*h*3). */
fun inputFrameBytes(stride: Int, sliceHeight: Int, capacity: Int): Int = minOf(capacity, stride * sliceHeight * 3 / 2)

/** The pure judgement of the hardware 10-bit decode self-test (what the decoder delivered), unit tested without a device. */
object Decode10 {
    /** Output layouts the engine converts: only P010 for now (the native decoder in core/video refuses anything else rather than truncating). */
    val CONVERTIBLE = setOf(ColorFmt.P010)

    /** (pass, reason). [outColor] = KEY_COLOR_FORMAT of the output format (-1 none), [levels] = distinct luma levels read from the first frame. */
    fun verdict(codec: String, outColor: Int, levels: Int, declaredFormats: List<Int>): Pair<Boolean, String> {
        if (outColor < 0) return false to "$codec: no output format reported"
        if (outColor !in CONVERTIBLE)
            return false to "$codec: delivers colour format ${colorLabel(outColor)} for a 10-bit stream; the engine converts only P010 " +
                "(declared: ${declaredFormats.joinToString(",") { colorLabel(it) }})"
        if (levels <= 256) return false to "$codec: only $levels distinct luma levels (8-bit picture)"
        return true to "$codec: P010 output, $levels distinct luma levels"
    }

    fun colorLabel(f: Int): String = when (f) {
        ColorFmt.P010 -> "P010"; ColorFmt.NV12 -> "NV12"; ColorFmt.I420 -> "I420"; ColorFmt.FLEXIBLE -> "Flexible"; ColorFmt.SURFACE -> "Surface"
        else -> "0x" + f.toString(16)
    }
}

/** One line of the diagnostics page: HEVC Main10 encode, per encoder (always at least one line). */
data class Main10Row(val codec: String, val hardware: Boolean, val state: String, val reason: String)

object Diagnostics {
    const val NOT_DECLARED = "not declared"
    const val UNTESTED = "declared, not tested yet"
    const val PASSED = "passed"
    const val FAILED = "failed"

    /**
     * The "HEVC Main10 encode" rows: one per HEVC encoder (declared Main10 / Main10HDR10 or not), or one row saying that no HEVC encoder exists. [result]
     * gives the cached self-test result of (kind, codec). Never empty, so the page always has the row.
     */
    fun main10EncodeRows(caps: List<CodecCaps>, result: (String, String) -> TestResult?): List<Main10Row> {
        val enc = caps.filter { it.encoder && it.mime == Mime.HEVC }
        if (enc.isEmpty()) return listOf(Main10Row("(no HEVC encoder)", false, NOT_DECLARED, "the device lists no HEVC encoder"))
        fun profs(c: CodecCaps, only10: Boolean) = c.profiles.filter { !only10 || Prof.isHevcMain10(it) }.joinToString(",") { Prof.hevcName(it) }
        return enc.map { c ->
            val inputs = ((if (c.hasP010) "P010 " else "") + (if (c.hasSurface) "Surface " else "") + (if (c.has8BitInput) "8-bit" else "")).trim()
            if (c.profiles.none { Prof.isHevcMain10(it) }) {
                Main10Row(c.name, c.hardware, Diagnostics.NOT_DECLARED, "profiles ${profs(c, false)}; inputs $inputs")
            } else {
                // two input modes, each with its own self-test: ByteBuffer P010 and an input Surface (RGB10A2 through EGL); the row says which one passed
                val rb = result(SelfTestKind.ENC10, c.name)
                val rs = if (c.hasSurface) result(SelfTestKind.ENC10_SURFACE, c.name) else null
                fun part(label: String, r: TestResult?, available: Boolean) =
                    if (!available) "$label: not offered by the codec" else if (r == null) "$label: not tested yet" else if (r.pass) "$label: passed (${r.detail})" else "$label: failed (${r.detail})"
                val detail = part("buffer P010", rb, true) + "; " + part("surface", rs, c.hasSurface)
                val passedModes = listOfNotNull(if (rb?.pass == true) "buffer P010" else null, if (rs?.pass == true) "surface" else null)
                val untested = rb == null || (c.hasSurface && rs == null)
                when {
                    passedModes.isNotEmpty() -> Main10Row(c.name, c.hardware, Diagnostics.PASSED, "input mode passed: ${passedModes.joinToString(" + ")}. $detail")
                    untested -> Main10Row(c.name, c.hardware, Diagnostics.UNTESTED, "declares ${profs(c, true)}; inputs $inputs. $detail")
                    else -> Main10Row(c.name, c.hardware, Diagnostics.FAILED, detail)
                }
            }
        }
    }
}
