package com.dapple.dapple

import java.nio.ByteBuffer

/**
 * The still's edit as it travels to a Motion Photo clip (docs/engine/pipeline-architecture.md section 30): the EditParams JSON
 * the editor already produces, the look LUT, the still's shape and illuminant, and the time of the reference frame. The native
 * session meters the film once on the reference frame and holds the result for every frame of the clip.
 */
data class EditSpec(
    val paramsJson: String,
    val lutPath: String? = null,
    /** Clip time (ms, source stream time) of the reference frame: the cover frame of the Motion Photo; < 0 = the first frame of the window. */
    val refMs: Long = -1,
    /** width / height of the upright still; a clip of another shape is centre-cropped to it (0 = keep). */
    val stillAspect: Double = 0.0,
    /** Long edge of the still render: the film grain is scale-aware against it (0 = the clip's own scale). */
    val stillLongEdge: Int = 0,
    /** As-shot illuminant xy of the still (film white balance), 0 = unknown. */
    val sceneX: Double = 0.0,
    val sceneY: Double = 0.0,
    val cameraMake: String = "",
    val cameraModel: String = "",
) {
    /** The options object of dapple_video_edit_open (flat JSON, keys documented in dapple_c.h). */
    fun optionsJson(): String = buildString {
        append('{')
        append("\"cameraMake\":").append(quote(cameraMake))
        append(",\"cameraModel\":").append(quote(cameraModel))
        append(",\"sceneX\":").append(num(sceneX))
        append(",\"sceneY\":").append(num(sceneY))
        append(",\"stillAspect\":").append(num(stillAspect))
        append(",\"stillLongEdge\":").append(stillLongEdge)
        append('}')
    }

    companion object {
        /** From the method-channel map ("edit" argument of `trim`); null when the clip is not edited (pass-through, the existing path). */
        fun fromMap(m: Map<*, *>?): EditSpec? {
            if (m == null) return null
            val json = m["paramsJson"] as? String ?: return null
            fun d(k: String) = (m[k] as? Number)?.toDouble() ?: 0.0
            return EditSpec(
                paramsJson = json,
                lutPath = (m["lutPath"] as? String)?.takeIf { it.isNotEmpty() },
                refMs = (m["refMs"] as? Number)?.toLong() ?: -1L,
                stillAspect = d("stillAspect"),
                stillLongEdge = (m["stillLongEdge"] as? Number)?.toInt() ?: 0,
                sceneX = d("sceneX"), sceneY = d("sceneY"),
                cameraMake = m["cameraMake"] as? String ?: "",
                cameraModel = m["cameraModel"] as? String ?: "",
            )
        }

        internal fun quote(s: String): String {
            val b = StringBuilder("\"")
            for (c in s) when {
                c == '"' -> b.append("\\\"")
                c == '\\' -> b.append("\\\\")
                c < ' ' -> b.append(String.format("\\u%04x", c.code))
                else -> b.append(c)
            }
            return b.append('"').toString()
        }

        internal fun num(v: Double): String = if (v.isNaN() || v.isInfinite()) "0" else v.toString()
    }
}

/** What the encoder is fed with when a clip goes through the graph: the session packs P010 for a Main10 rung and I420 for the 8-bit rungs. */
object EditFormat {
    const val RGBA8 = 0
    const val I420 = 1
    const val P010 = 2
    /** P010 in the clip's output transfer: HLG / PQ on BT.2020 when the session decided on HDR, else exactly [P010]. */
    const val P010_HDR = 3
    /** Packed 10-bit RGB for an encoder input Surface (docs/engine/pipeline-architecture.md section 30.9), SDR / output transfer. */
    const val RGB10A2 = 4
    const val RGB10A2_HDR = 5
    fun forEncoder(tenBit: Boolean, hdr: Boolean = false) = if (tenBit) (if (hdr) P010_HDR else P010) else I420
}

/**
 * JNI face of core/engine/video (libdapple.so): a VideoFrameSession per clip. The frames arrive as half-float linear RGBA from [SwVideo]
 * (format [SwVideo.RGBAF16], so nothing is truncated before the graph); the result is packed for the encoder.
 */
object DappleVideoEdit {
    val available: Boolean by lazy {
        try { DappleViewNative.loaded && nativeProbe() } catch (_: Throwable) { false }
    }

    @JvmStatic private external fun nativeProbe(): Boolean
    @JvmStatic private external fun nativeSetFilmPack(path: String)
    @JvmStatic private external fun nativeOpen(params: String, lut: String?, options: String): Long
    @JvmStatic private external fun nativeClose(handle: Long)
    @JvmStatic private external fun nativeSetReference(handle: Long, src: ByteBuffer, w: Int, h: Int, kind: Int, rot: Int): Int
    @JvmStatic private external fun nativeOutputSize(handle: Long): IntArray?
    @JvmStatic private external fun nativeOutputKind(handle: Long): IntArray?
    @JvmStatic private external fun nativeOutputNote(handle: Long): String
    @JvmStatic private external fun nativeFrameBytes(handle: Long, format: Int): Long
    @JvmStatic private external fun nativeRender(handle: Long, src: ByteBuffer, w: Int, h: Int, kind: Int, rot: Int, frameIndex: Int, format: Int, dst: ByteBuffer): Int
    @JvmStatic private external fun nativeLastError(): String

    class Session internal constructor(private var handle: Long) : AutoCloseable {
        var outW = 0; private set
        var outH = 0; private set

        /** Freezes the clip: film exposure, tone analysis and solves are taken from this frame (a [SwVideo.RGBAF16] frame of [w] x [h]). */
        fun setReference(frame: ByteBuffer, w: Int, h: Int, colorKind: Int, rotation: Int) {
            if (nativeSetReference(handle, frame, w, h, colorKind, rotation) != 0) throw IllegalStateException("edit reference failed: ${nativeLastError()}")
            val s = nativeOutputSize(handle) ?: throw IllegalStateException("edit output size: ${nativeLastError()}")
            outW = s[0]; outH = s[1]
            readOutputKind()
        }

        fun frameBytes(format: Int): Int = nativeFrameBytes(handle, format).toInt()

        /** What the clip is written as (HDR policy, section 30.8): kind 0 SDR BT.709, 1 HLG, 2 PQ (BT.2020). Valid after [setReference]. */
        var outputKind = 0; private set
        /** FFmpeg AVCOL_* numbers: primaries, transfer, matrix, range. */
        var tags = intArrayOf(1, 1, 1, 1); private set
        var note = ""; private set
        val hdr get() = outputKind != 0

        internal fun readOutputKind() {
            val k = nativeOutputKind(handle) ?: throw IllegalStateException("edit output kind: ${nativeLastError()}")
            outputKind = k[0]; tags = intArrayOf(k[1], k[2], k[3], k[4]); note = nativeOutputNote(handle)
        }

        /** Frame [frameIndex] (>= 1 inside a clip: seeds the film grain) into [dst] (direct, at least [frameBytes]). */
        fun render(frame: ByteBuffer, w: Int, h: Int, colorKind: Int, rotation: Int, frameIndex: Int, format: Int, dst: ByteBuffer) {
            if (nativeRender(handle, frame, w, h, colorKind, rotation, frameIndex, format, dst) != 0) throw IllegalStateException("edit render failed: ${nativeLastError()}")
        }

        override fun close() {
            if (handle != 0L) { nativeClose(handle); handle = 0L }
        }
    }

    /** Tooling only (the app's Dart side installs the film library): installs the pack at [path] in a process that has no Flutter engine. */
    fun setFilmPack(path: String) { if (available) nativeSetFilmPack(path) }

    /** Opens the session for [spec]; throws when libdapple lacks the video API or the edit does not parse. */
    fun open(spec: EditSpec): Session {
        if (!available) throw IllegalStateException("the edit engine is not available")
        val h = nativeOpen(spec.paramsJson, spec.lutPath, spec.optionsJson())
        if (h == 0L) throw IllegalStateException("edit session: ${nativeLastError()}")
        return Session(h)
    }
}
