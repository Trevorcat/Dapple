package com.dapple.dapple

import android.graphics.Bitmap
import java.io.File
import java.io.FileOutputStream

/**
 * The viewfinder frame of a RAW-only shot as a small JPEG. CameraX writes the DNG of a RAW-only capture without any preview, so the engine's
 * per-file exposure anchor (core/src/exposure_anchor.h, tier 1: the camera's own rendering of this frame) would have nothing to compare with.
 * Dart embeds this file into the DNG after the shot (dapple_dng_embed_thumbnail) and deletes it. The frame is the camera ISP's rendering of the
 * scene (same tone mapping as the phone's JPEG), upright as shown; the engine accepts either orientation.
 */
object CaptureThumb {
    const val LONG_EDGE = 720
    const val QUALITY = 90

    /** Long edge of [w] x [h] scaled down to at most [LONG_EDGE]; never up. Returns (width, height). */
    fun scaledSize(w: Int, h: Int): Pair<Int, Int> {
        val longest = maxOf(w, h)
        if (longest <= LONG_EDGE) return Pair(w, h)
        val s = LONG_EDGE.toDouble() / longest
        return Pair(maxOf(1, Math.round(w * s).toInt()), maxOf(1, Math.round(h * s).toInt()))
    }

    /** Writes [bmp] (null: no viewfinder bitmap available) scaled to [LONG_EDGE] as JPEG to [out]; null on any failure (the anchor then falls back). */
    fun save(bmp: Bitmap?, out: File): File? {
        if (bmp == null || bmp.width <= 0 || bmp.height <= 0) return null
        return try {
            val (w, h) = scaledSize(bmp.width, bmp.height)
            val scaled = if (w == bmp.width && h == bmp.height) bmp else Bitmap.createScaledBitmap(bmp, w, h, true)
            FileOutputStream(out).use { scaled.compress(Bitmap.CompressFormat.JPEG, QUALITY, it) }
            if (scaled !== bmp) scaled.recycle()
            if (out.length() > 0) out else null
        } catch (e: Exception) {
            null
        }
    }
}
