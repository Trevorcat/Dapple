package com.dapple.dapple

import android.content.Context
import android.net.Uri
import java.nio.ByteBuffer
import org.json.JSONObject

/**
 * Kotlin face of core/video (FFmpeg 9.x, LGPL, dynamically linked: libdapple_video.so + libav*.so). It is the only
 * video decoder of the app: hardware (MediaCodec through FFmpeg's h264_mediacodec / hevc_mediacodec) first when the
 * probe in [LiveBridge] says the device decodes the stream, software otherwise; one place for colour matrix, range
 * and transfer handling; >= 10-bit precision up to the final quantisation. Pull API:
 * open -> begin(range, size, format) -> next()* -> close.
 */
object SwVideo {
    const val RGBA = 0      // 8-bit sRGB, dithered from 16-bit
    const val I420 = 1      // 8-bit 4:2:0, BT.709 limited (H.264 fallback encoder)
    const val RGBA64 = 2    // 16-bit sRGB
    const val P010 = 3      // 10-bit 4:2:0 for the HEVC Main10 encoder
    const val RGBAF16 = 4   // half-float linear, 1.0 = SDR white, for Ultra HDR stills

    const val SRC_AUTO = 0
    const val SRC_STANDARD = 1
    const val SRC_SLOG3 = 2
    const val SRC_HLG = 3

    /** libdapple_video.so loaded (it is absent from builds that skip the FFmpeg step). */
    val available: Boolean by lazy {
        try { System.loadLibrary("dapple_video"); nativeVersion() != "stub" } catch (_: Throwable) { false }
    }

    @JvmStatic private external fun nativeOpen(url: String, hwPref: Int, source: Int, nativeDecoder: String?, handleOut: LongArray): String
    @JvmStatic private external fun nativeDecoderNow(handle: Long): String
    @JvmStatic private external fun nativeSetCodecReport(json: String)
    @JvmStatic private external fun nativeCodecReport(): String
    @JvmStatic private external fun nativeClose(handle: Long)
    @JvmStatic private external fun nativeBegin(handle: Long, startUs: Long, endUs: Long, maxEdge: Int, maxFps: Double, format: Int, fast: Int): IntArray
    @JvmStatic private external fun nativeNext(handle: Long, buf: ByteBuffer): Long
    @JvmStatic private external fun nativeTags(handle: Long): IntArray
    @JvmStatic private external fun nativeHdrInfo(handle: Long): ByteArray?
    @JvmStatic private external fun nativeAudioBegin(handle: Long, startUs: Long, endUs: Long): IntArray?
    @JvmStatic private external fun nativeAudioNext(handle: Long, buf: ByteBuffer, maxFrames: Int, ptsOut: LongArray): Int
    @JvmStatic private external fun nativeUltraHdr(hdr: ByteBuffer, w: Int, h: Int, sdr: ByteBuffer, gain: ByteBuffer, meta: FloatArray): IntArray?
    @JvmStatic private external fun nativeVersion(): String

    fun version(): String = if (available) nativeVersion() else "none"

    /** The codec capability report (JSON) kept in the core next to the decoder state (dv_set_codec_report / dv_codec_report). */
    fun setCodecReport(json: String) { if (available) nativeSetCodecReport(json) }
    fun codecReport(): String = if (available) nativeCodecReport() else ""

    /** Decoder decision for one source: hwPref 0 software, 1 FFmpeg's MediaCodec wrapper (8-bit), 2 NDK AMediaCodec with P010 output ([codec] by name). */
    data class DecodeSel(val hwPref: Int, val codec: String? = null) {
        companion object { val SOFTWARE = DecodeSel(0) }
    }

    class Info(j: JSONObject) {
        val width = j.getInt("width")
        val height = j.getInt("height")
        val rotation = j.getInt("rotation")
        val fps = j.getDouble("fps")
        val durationUs = j.getLong("durationUs")
        val bitDepth = j.getInt("bitDepth")
        val chroma = j.getInt("chroma")
        val primaries = j.getInt("primaries")
        val trc = j.getInt("trc")
        val matrix = j.getInt("matrix")
        val hdr = j.getInt("hdr")
        val codec = j.getString("codec")
        val hardware = j.getInt("hardware") != 0
        val decoder = j.getString("decoder")
        /** 0 standard, 1 BT.2020 SDR, 2 HLG, 3 PQ, 4 S-Log3 (as converted). */
        val colorKind = j.getInt("colorKind")
        /** 0 none, 1 Sony NRT metadata inside the MP4, 2 M01.XML sidecar. */
        val slogDetected = j.getInt("slogDetected")
        val captureGamma = j.getString("captureGamma")
        val capturePrimaries = j.getString("capturePrimaries")
        val audioCodec = j.getString("audioCodec")
        val audioRate = j.getInt("audioRate")
        val audioChannels = j.getInt("audioChannels")
        val audioDecodable = j.getInt("audioDecodable") != 0
        override fun toString() =
            "$codec ${width}x$height ${bitDepth}-bit chroma=$chroma hdr=$hdr kind=$colorKind rot=$rotation fps=$fps decoder=$decoder"
    }

    class UltraHdr(val sdr: ByteBuffer, val gain: ByteBuffer, val gw: Int, val gh: Int, val meta: FloatArray)

    class Session internal constructor(private var handle: Long, val info: Info) : AutoCloseable {
        /** Name of the decoder in use right now ("mc:<codec>" NDK MediaCodec, "h264_mediacodec", "hevc" ...); it changes when the core falls back to software. */
        fun decoderNow(): String = nativeDecoderNow(handle)

        var outW = 0; private set
        var outH = 0; private set

        /**
         * Starts a pass over [startUs, endUs] (stream time). maxEdge > 0 scales early, maxFps > 0 drops frames,
         * fast = thumbnail quality. startUs == endUs yields the single frame nearest to that time.
         */
        fun begin(startUs: Long, endUs: Long, maxEdge: Int, maxFps: Double, format: Int, fast: Boolean) {
            val r = nativeBegin(handle, startUs, endUs, maxEdge, maxFps, format, if (fast) 1 else 0)
            if (r[0] != 0) throw IllegalStateException("video decoder seek failed (${r[0]})")
            outW = r[1]; outH = r[2]
        }

        fun bufferSize(format: Int): Int = when (format) {
            RGBA -> outW * outH * 4
            I420 -> outW * outH * 3 / 2
            P010 -> outW * outH * 3
            else -> outW * outH * 8
        }

        /** Next frame into [buf] (direct, at least [bufferSize]); its time in us, -1 at the end. */
        fun next(buf: ByteBuffer): Long {
            val t = nativeNext(handle, buf)
            if (t == -2L) throw IllegalStateException("video decoder failed")
            return t
        }

        /** Colour tags the YUV output carries: primaries, transfer, matrix, range (FFmpeg AVCOL_* values). */
        fun tags(): IntArray = nativeTags(handle)

        /** CTA-861.3 static HDR metadata (25 bytes) when the stream has any. */
        fun hdrStaticInfo(): ByteArray? = nativeHdrInfo(handle)

        /** Starts decoding the audio of [startUs, endUs] to 16-bit PCM; returns (rate, channels) or null when none / undecodable. */
        fun audioBegin(startUs: Long, endUs: Long): IntArray? = nativeAudioBegin(handle, startUs, endUs)

        /** Up to [maxFrames] sample frames of interleaved PCM; returns the count (0 at the end) and the first sample's time in [ptsOut]. */
        fun audioNext(buf: ByteBuffer, maxFrames: Int, ptsOut: LongArray): Int {
            val n = nativeAudioNext(handle, buf, maxFrames, ptsOut)
            if (n < 0) throw IllegalStateException("audio decoder failed")
            return n
        }

        /** SDR base + gain map from a [RGBAF16] frame in [hdr] (Ultra HDR still). */
        fun ultraHdr(hdr: ByteBuffer): UltraHdr {
            val sdr = ByteBuffer.allocateDirect(outW * outH * 4)
            val gain = ByteBuffer.allocateDirect(((outW + 1) / 2) * ((outH + 1) / 2))
            val meta = FloatArray(5)
            val g = nativeUltraHdr(hdr, outW, outH, sdr, gain, meta) ?: throw IllegalStateException("gain map failed")
            return UltraHdr(sdr, gain, g[0], g[1], meta)
        }

        override fun close() {
            if (handle != 0L) { nativeClose(handle); handle = 0L }
        }
    }

    /**
     * Opens [uri] (an absolute path or a content:// URI). [dec] = the codec ladder's decision for this stream (see
     * [CodecManager.decodeFor]); the core falls back to software on any hardware failure. Throws UnsupportedVideo when FFmpeg
     * cannot decode it.
     */
    fun open(ctx: Context, uri: String, dec: DecodeSel = DecodeSel.SOFTWARE, source: Int = SRC_AUTO): Session {
        if (!available) throw UnsupportedVideo("video decoder not available")
        val h = LongArray(1)
        val url: String
        if (uri.startsWith("/")) {
            url = uri
        } else {
            val pfd = ctx.contentResolver.openFileDescriptor(Uri.parse(uri), "r") ?: throw IllegalStateException("cannot open $uri")
            url = "fd:" + pfd.detachFd() // FFmpeg's fd protocol owns (and closes) it
        }
        val j = JSONObject(nativeOpen(url, dec.hwPref, source, dec.codec, h))
        if (h[0] == 0L) {
            throw UnsupportedVideo(j.optString("error", "video decoder failed"))
        }
        return Session(h[0], Info(j))
    }
}
