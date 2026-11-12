package com.dapple.dapple

import android.content.Context
import android.content.SharedPreferences
import android.os.Build
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject

private class PrefsKv(private val p: SharedPreferences) : KvStore {
    override fun get(key: String) = try { p.getString(key, null) } catch (_: Exception) { null }
    override fun put(key: String, value: String) { p.edit().putString(key, value).apply() }
    override fun remove(key: String) { p.edit().remove(key).apply() }
    override fun keys(): Set<String> = try { p.all.keys.toSet() } catch (_: Exception) { emptySet() }
}

/**
 * The device's codec capability layer: the declared capabilities ([CodecCatalog]), the ranked ladders ([Ladder]) and the
 * one-time self-tests ([CodecSelfTest]) whose results are cached per OS build + codec ([SelfTestCache]). Decode and encode both
 * ask this class which codec to use; a failed self-test demotes the codec to the next rung. Every job logs and returns the
 * path it took.
 */
class CodecManager private constructor(private val ctx: Context) {
    companion object {
        private const val TAG = "DappleCodec"
        @Volatile private var inst: CodecManager? = null
        fun get(ctx: Context): CodecManager = inst ?: synchronized(this) { inst ?: CodecManager(ctx.applicationContext).also { inst = it } }
    }

    val caps: List<CodecCaps> by lazy { CodecCatalog.query() }
    val cache = SelfTestCache(PrefsKv(ctx.getSharedPreferences("dapple_codec_selftest", Context.MODE_PRIVATE)), Build.FINGERPRINT).also {
        val n = it.purgeStale()
        if (n > 0) Log.i(TAG, "OS build changed: dropped $n cached self-test results, capabilities are verified again")
    }
    private val tester by lazy { CodecSelfTest(ctx, caps, cache) }
    private val lock = Any()
    private val jobs = ArrayDeque<String>()

    fun capsOf(name: String, encoder: Boolean) = caps.firstOrNull { it.name == name && it.encoder == encoder }

    // ---- encode ----------------------------------------------------------------------------------------------------
    fun encodeCandidates(job: EncodeJob): List<EncodePick> = Ladder.encode(caps, job, cache)

    /** True when [pick] passed its self-test (running it now when this device + OS build has not tested it yet). */
    fun verifyEncoder(pick: EncodePick): Boolean = synchronized(lock) {
        when (cache.state(pick.testKind, pick.codec)) {
            TestState.PASS -> true
            TestState.FAIL -> false
            TestState.UNKNOWN -> {
                val c = capsOf(pick.codec, true) ?: return false
                val o = tester.encode(c, pick)
                cache.record(pick.testKind, pick.codec, o.pass, o.detail, o.ms)
                publish()
                o.pass
            }
        }
    }

    /** A real encode showed the codec does not deliver what it declares (stall, downgraded profile): remember, demote. */
    fun markEncoderFailed(pick: EncodePick, why: String) = synchronized(lock) {
        cache.record(pick.testKind, pick.codec, false, "during a job: $why", 0)
        publish()
    }

    // ---- decode ----------------------------------------------------------------------------------------------------
    /** The decoder to use for [stream]: first rung of the decode ladder that is declared and verified. */
    fun decodeFor(stream: StreamSpec): DecodePick = synchronized(lock) {
        for (p in Ladder.decode(caps, stream, cache)) {
            if (p.route != DecodeRoute.HW_NATIVE_P010) return p
            val c = capsOf(p.codec!!, false) ?: continue
            when (cache.state(SelfTestKind.DEC10, c.name)) {
                TestState.PASS -> return p
                TestState.FAIL -> continue
                TestState.UNKNOWN -> {
                    val o = tester.decode10(c)
                    cache.record(SelfTestKind.DEC10, c.name, o.pass, o.detail, o.ms)
                    publish()
                    if (o.pass) return p
                }
            }
        }
        DecodePick(DecodeRoute.SOFTWARE, null)
    }

    /** The native decoder failed during a job: do not offer it again on this OS build. */
    fun markDecoderFailed(codec: String, why: String) = synchronized(lock) {
        cache.record(SelfTestKind.DEC10, codec, false, "during a job: $why", 0)
        publish()
    }

    // ---- job log -------------------------------------------------------------------------------------------------------
    fun noteJob(summary: String) {
        Log.i(TAG, "job: $summary")
        synchronized(jobs) { jobs.addLast(summary); while (jobs.size > 30) jobs.removeFirst() }
        publish()
    }

    // ---- diagnostics ---------------------------------------------------------------------------------------------------
    /** Runs every self-test this device could need (1080p30 encode ladder + hardware 10-bit decoders). [force] repeats finished ones. */
    fun runAll(force: Boolean) = synchronized(lock) {
        val probe = EncodeJob(1920, 1080, 30)
        for (p in Ladder.encode(caps, probe, TestLedger.NONE)) {
            if (!force && cache.state(p.testKind, p.codec) != TestState.UNKNOWN) continue
            val c = capsOf(p.codec, true) ?: continue
            val o = tester.encode(c, p)
            cache.record(p.testKind, p.codec, o.pass, o.detail, o.ms)
        }
        for (c in caps.filter { !it.encoder && it.hardware && it.mime == Mime.HEVC && it.declaresProfile(Prof.HEVC_MAIN10) && it.hasP010 }) {
            if (!force && cache.state(SelfTestKind.DEC10, c.name) != TestState.UNKNOWN) continue
            val o = tester.decode10(c)
            cache.record(SelfTestKind.DEC10, c.name, o.pass, o.detail, o.ms)
        }
        publish()
    }

    fun report(): JSONObject {
        val j = JSONObject()
        j.put("fingerprint", Build.FINGERPRINT)
        j.put("sdk", Build.VERSION.SDK_INT)
        j.put("abi", Build.SUPPORTED_ABIS.firstOrNull() ?: "")
        val codecs = JSONArray()
        for (c in caps) {
            codecs.put(JSONObject().apply {
                put("name", c.name); put("mime", c.mime); put("type", if (c.encoder) "encoder" else "decoder")
                put("hardware", c.hardware); put("softwareOnly", c.softwareOnly)
                put("profiles", JSONArray(c.profiles.map { CodecCatalog.profileName(c.mime, it) }))
                put("colorFormats", JSONArray(c.colorFormats.map { CodecCatalog.colorName(it) }))
                put("maxSize", "${c.maxW}x${c.maxH}")
                put("maxFps1080", c.maxFps1080)
                put("performancePoints", c.perfPoints)
                put("hdrEditing", c.hdrEditing)
            })
        }
        j.put("codecs", codecs)
        val tests = JSONArray()
        for ((kind, codec, r) in cache.all()) {
            tests.put(JSONObject().apply { put("kind", kind); put("codec", codec); put("pass", r.pass); put("detail", r.detail); put("ms", r.ms); put("at", r.atMs) })
        }
        j.put("selfTests", tests)
        fun ladder(job: EncodeJob) = JSONArray(Ladder.encode(caps, job, cache).map { "${it.rung.label} · ${it.codec}${if (it.hardware) "" else " (sw)"}" })
        fun dec(spec: StreamSpec) = JSONArray(Ladder.decode(caps, spec, cache).map { "${it.route.label}${it.codec?.let { c -> " · $c" } ?: ""}" })
        j.put("encodeLadder1080p30", ladder(EncodeJob(1920, 1080, 30)))
        j.put("encodeLadder1080p30Hdr", ladder(EncodeJob(1920, 1080, 30, hdrSource = true)))
        j.put("encodeLadder8Bit", ladder(EncodeJob(1920, 1080, 30, eightBitOnly = true)))
        j.put("decodeLadderHevc10", dec(StreamSpec(Mime.HEVC, Prof.HEVC_MAIN10, 10, 1)))
        j.put("decodeLadderHevc8", dec(StreamSpec(Mime.HEVC, 0, 8, 1)))
        j.put("decodeLadderAvc8", dec(StreamSpec(Mime.AVC, 0, 8, 1)))
        j.put("decodeLadderAvc422", dec(StreamSpec(Mime.AVC, Prof.AVC_HIGH422, 10, 2)))
        j.put("jobs", JSONArray(synchronized(jobs) { jobs.toList() }))
        return j
    }

    /** Hands the report to the core (C API) so it sits next to the decoder state. */
    private fun publish() {
        try { if (SwVideo.available) SwVideo.setCodecReport(report().toString()) } catch (_: Throwable) {}
    }
}
