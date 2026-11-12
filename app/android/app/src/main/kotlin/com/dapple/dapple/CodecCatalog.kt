package com.dapple.dapple

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.util.Log
import java.nio.ByteBuffer

/** Reads what the device's codecs declare (MediaCodecList) into [CodecCaps]. */
object CodecCatalog {
    private const val TAG = "DappleCodec"
    private val MIMES = listOf(Mime.AVC, Mime.HEVC, Mime.AV1, Mime.VP9)

    fun query(): List<CodecCaps> {
        val out = ArrayList<CodecCaps>()
        val infos = try { MediaCodecList(MediaCodecList.ALL_CODECS).codecInfos } catch (e: Exception) { Log.w(TAG, "codec list: $e"); return out }
        for (ci in infos) {
            if (Build.VERSION.SDK_INT >= 29 && ci.isAlias) continue
            // secure (DRM) decoders only work with a protected pipeline: never a candidate for ordinary video
            if (ci.name.endsWith(".secure")) continue
            for (t in ci.supportedTypes) {
                val mime = MIMES.firstOrNull { it.equals(t, true) } ?: continue
                try {
                    val c = ci.getCapabilitiesForType(t)
                    val vc = c.videoCapabilities
                    val hw = if (Build.VERSION.SDK_INT >= 29) ci.isHardwareAccelerated
                    else !(ci.name.startsWith("OMX.google.") || ci.name.startsWith("c2.android.") || ci.name.startsWith("c2.google."))
                    val swOnly = if (Build.VERSION.SDK_INT >= 29) ci.isSoftwareOnly else !hw
                    var fps1080 = 0
                    try { fps1080 = vc.getSupportedFrameRatesFor(1920, 1080).upper.toInt() } catch (_: Exception) {}
                    var pp = 0
                    if (Build.VERSION.SDK_INT >= 29) try { pp = vc.supportedPerformancePoints?.size ?: 0 } catch (_: Exception) {}
                    var hdrEdit = false
                    if (Build.VERSION.SDK_INT >= 33) try { hdrEdit = c.isFeatureSupported("hdr-editing") } catch (_: Exception) {}
                    out.add(
                        CodecCaps(
                            name = ci.name, mime = mime, encoder = ci.isEncoder, hardware = hw, softwareOnly = swOnly,
                            profiles = c.profileLevels.map { it.profile }.distinct(),
                            colorFormats = c.colorFormats.toList(),
                            minW = vc.supportedWidths.lower, minH = vc.supportedHeights.lower,
                            maxW = vc.supportedWidths.upper, maxH = vc.supportedHeights.upper,
                            maxFps1080 = fps1080, perfPoints = pp, hdrEditing = hdrEdit,
                        )
                    )
                } catch (e: Exception) {
                    Log.w(TAG, "caps of ${ci.name} $t: $e")
                }
            }
        }
        return out
    }

    fun profileName(mime: String, p: Int): String = when (mime) {
        Mime.HEVC -> when (p) { 1 -> "Main"; 2 -> "Main10"; 4 -> "MainStill"; 0x1000 -> "Main10HDR10"; 0x2000 -> "Main10HDR10Plus"; else -> "0x" + p.toString(16) }
        Mime.AVC -> when (p) { 1 -> "Baseline"; 2 -> "Main"; 4 -> "Extended"; 8 -> "High"; 0x10 -> "High10"; 0x20 -> "High422"; 0x40 -> "High444"; else -> "0x" + p.toString(16) }
        else -> "0x" + p.toString(16)
    }

    fun colorName(f: Int): String = when (f) {
        ColorFmt.P010 -> "P010"; ColorFmt.NV12 -> "NV12"; ColorFmt.I420 -> "I420"; ColorFmt.FLEXIBLE -> "Flexible"; ColorFmt.SURFACE -> "Surface"
        else -> f.toString()
    }
}

/** Moves one raw 4:2:0 frame into a MediaCodec input buffer using the codec's own layout (getInputImage / stride * slice-height). */
object CodecInput {
    private fun copyPlane(src: ByteBuffer, off: Int, w: Int, h: Int, plane: android.media.Image.Plane, row: ByteArray) {
        val dst = plane.buffer
        val rs = plane.rowStride
        val ps = plane.pixelStride
        for (y in 0 until h) {
            src.position(off + y * w)
            src.get(row, 0, w)
            if (ps == 1) {
                dst.position(y * rs)
                dst.put(row, 0, w)
            } else {
                val base = y * rs
                for (x in 0 until w) dst.put(base + x * ps, row[x])
            }
        }
    }

    /**
     * [src] holds the tightly packed frame: I420 (Y, U, V) or P010 (Y words, then interleaved UV words). Fills input buffer
     * [index] of [enc] and returns the number of bytes to queue (never w*h*3: stride * slice-height from the codec's input
     * format, capped at the buffer capacity).
     */
    fun fill(enc: MediaCodec, index: Int, src: ByteBuffer, w: Int, h: Int, tenBit: Boolean, row: ByteArray): Int {
        val inFmt = try { enc.inputFormat } catch (_: Exception) { null }
        val ib = enc.getInputBuffer(index) ?: throw IllegalStateException("no input buffer")
        val img = enc.getInputImage(index)
        val bps = if (tenBit) 2 else 1
        if (img != null) {
            val planes = img.planes
            if (tenBit) {
                val yp = planes[0]
                for (y in 0 until h) {
                    src.position(y * w * 2)
                    src.get(row, 0, w * 2)
                    yp.buffer.position(y * yp.rowStride)
                    yp.buffer.put(row, 0, w * 2)
                }
                val up = planes[1]; val vp = planes[2]
                val uvOff = w * h * 2
                for (cy in 0 until h / 2) {
                    src.position(uvOff + cy * w * 2)
                    src.get(row, 0, w * 2)
                    val ub0 = cy * up.rowStride
                    val vb0 = cy * vp.rowStride
                    for (cx in 0 until w / 2) {
                        val k = cx * 4
                        val uo = ub0 + cx * up.pixelStride
                        val vo = vb0 + cx * vp.pixelStride
                        up.buffer.put(uo, row[k]); up.buffer.put(uo + 1, row[k + 1])
                        vp.buffer.put(vo, row[k + 2]); vp.buffer.put(vo + 1, row[k + 3])
                    }
                }
            } else {
                copyPlane(src, 0, w, h, planes[0], row)
                copyPlane(src, w * h, w / 2, h / 2, planes[1], row)
                copyPlane(src, w * h + (w / 2) * (h / 2), w / 2, h / 2, planes[2], row)
            }
            val stride = inFmt?.takeIf { it.containsKey("stride") }?.getInteger("stride") ?: planes[0].rowStride
            val slice = inFmt?.takeIf { it.containsKey("slice-height") }?.getInteger("slice-height") ?: h
            return inputFrameBytes(stride, slice, ib.capacity())
        }
        // no Image view: semi-planar buffer, P010 only (the layout the core delivers)
        if (!tenBit) throw IllegalStateException("encoder gives no input image for 8-bit input")
        val stride = inFmt?.takeIf { it.containsKey("stride") }?.getInteger("stride") ?: (w * bps)
        val slice = inFmt?.takeIf { it.containsKey("slice-height") }?.getInteger("slice-height") ?: h
        ib.clear()
        for (y in 0 until h) {
            src.position(y * w * 2); src.get(row, 0, w * 2)
            ib.position(y * stride); ib.put(row, 0, w * 2)
        }
        val uvBase = stride * slice
        for (cy in 0 until h / 2) {
            src.position(w * h * 2 + cy * w * 2); src.get(row, 0, w * 2)
            ib.position(uvBase + cy * stride); ib.put(row, 0, w * 2)
        }
        return inputFrameBytes(stride, slice, ib.capacity())
    }
}
