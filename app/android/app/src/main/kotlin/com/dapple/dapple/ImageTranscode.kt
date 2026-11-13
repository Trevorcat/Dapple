package com.dapple.dapple

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.BitmapRegionDecoder
import android.graphics.Canvas
import android.graphics.ColorSpace
import android.graphics.ImageDecoder
import android.graphics.Rect
import android.media.ExifInterface
import android.os.Build
import android.util.Half
import android.util.Log
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * HEIC / HEIF / WebP -> what the engine reads, with the platform decoder (so the engine needs no HEIF library).
 * Android 9+: a 16-bit RGB PNG in the picture's own colour space ([toPng16]: Display P3 stays Display P3, no sRGB conversion, no 8-bit
 * step). Before API 28: the old 8-bit sRGB JPEG (quality 95, [toJpeg]). Capture date, camera, exposure and GPS are carried over
 * (PNG: eXIf chunk; JPEG: EXIF). PNG and JPEG themselves are not converted: the engine reads their profile / tags and depth.
 * Lost: HDR gain maps (the SDR base image is used), alpha (flattened to white).
 *
 * Memory ([toPng16]): the picture is read in full-width strips of about 6 MP with BitmapRegionDecoder (RGBA_F16 asked for, so 10-bit
 * HEIC keeps its bits) and streamed into the deflater: peak = one strip (24 MB 8-bit / 48 MB F16) + its raw bytes (~36 MB) + the decoder's
 * own state, whatever the picture size. Where the region decoder is not available the whole picture is decoded by ImageDecoder and cut
 * into the same strips: peak = the decoded bitmap (192 MB for 48 MP 8-bit, 384 MB F16) + about 60 MB.
 */
object ImageTranscode {
    private val keep = listOf(
        ExifInterface.TAG_DATETIME_ORIGINAL, ExifInterface.TAG_DATETIME_DIGITIZED, ExifInterface.TAG_DATETIME,
        ExifInterface.TAG_MAKE, ExifInterface.TAG_MODEL,
        ExifInterface.TAG_EXPOSURE_TIME, ExifInterface.TAG_F_NUMBER, ExifInterface.TAG_FOCAL_LENGTH,
        ExifInterface.TAG_GPS_LATITUDE, ExifInterface.TAG_GPS_LATITUDE_REF, ExifInterface.TAG_GPS_LONGITUDE, ExifInterface.TAG_GPS_LONGITUDE_REF,
        ExifInterface.TAG_GPS_ALTITUDE, ExifInterface.TAG_GPS_ALTITUDE_REF,
    )

    private val types = setOf("heic", "heif", "webp")
    private const val STRIP_PIXELS = 6_000_000

    /** True for the types that go through [convert] (the engine reads JPEG, PNG and the RAW formats itself). */
    fun needsTranscode(name: String) = name.substringAfterLast('.', "").lowercase() in types

    /** "IMG_1.HEIC" -> "IMG_1.png" (API 28+) or "IMG_1.jpg" (older). */
    fun outputName(name: String, sdk: Int = Build.VERSION.SDK_INT) =
        (if (name.contains('.')) name.substringBeforeLast('.') else name) + if (sdk >= 28) ".png" else ".jpg"

    /** Converts [src] to [dest] (a `.png` name; before API 28 the file written is the same name with `.jpg`). Returns the file written, null on failure. */
    fun convert(src: File, dest: File): File? {
        if (Build.VERSION.SDK_INT >= 28) return if (toPng16(src, dest)) dest else null
        val j = File(dest.parentFile, dest.nameWithoutExtension + ".jpg")
        return if (toJpeg(src, j)) j else null
    }

    private fun spaceOf(cs: ColorSpace?): PngSpace? = when (cs) {
        null -> PngSpace("sRGB", intArrayOf(1, 13))
        ColorSpace.get(ColorSpace.Named.SRGB), ColorSpace.get(ColorSpace.Named.EXTENDED_SRGB) -> PngSpace("sRGB", intArrayOf(1, 13))
        ColorSpace.get(ColorSpace.Named.DISPLAY_P3) -> PngSpace("Display P3", intArrayOf(12, 13))
        ColorSpace.get(ColorSpace.Named.BT2020) -> PngSpace("Rec.2020", intArrayOf(9, 14))
        ColorSpace.get(ColorSpace.Named.ADOBE_RGB) -> PngSpace("Adobe RGB", null, adobe = true)
        else -> null  // extended / linear / exotic: the caller asks the decoder for Display P3 instead
    }

    /** 16-bit PNG in the source's own colour space. */
    fun toPng16(src: File, dest: File): Boolean {
        dest.parentFile?.mkdirs()
        try {
            if (regionPath(src, dest)) return dest.length() > 0
        } catch (e: Throwable) {
            Log.w("DappleIngest", "region decode of ${src.name} failed (${e.javaClass.simpleName}: ${e.message}), decoding whole")
        }
        dest.delete()
        return try {
            fullPath(src, dest) && dest.length() > 0
        } catch (e: Throwable) {
            Log.w("DappleIngest", "whole decode of ${src.name} failed: ${e.javaClass.simpleName}: ${e.message}")
            dest.delete()
            false
        }
    }

    private fun exifBlock(src: File, orientation: Boolean): ByteArray? {
        val tmp = File.createTempFile("exif", ".jpg", src.parentFile)
        try {
            val one = Bitmap.createBitmap(1, 1, Bitmap.Config.ARGB_8888)
            tmp.outputStream().use { one.compress(Bitmap.CompressFormat.JPEG, 50, it) }
            one.recycle()
            val a = ExifInterface(src.path)
            val b = ExifInterface(tmp.path)
            for (t in keep) a.getAttribute(t)?.let { b.setAttribute(t, it) }
            if (orientation) a.getAttribute(ExifInterface.TAG_ORIENTATION)?.let { b.setAttribute(ExifInterface.TAG_ORIENTATION, it) }
            b.saveAttributes()
            return Png16Writer.exifOfJpeg(tmp.readBytes())
        } catch (_: Exception) {
            return null
        } finally {
            tmp.delete()
        }
    }

    /** Tiled: BitmapRegionDecoder delivers the pixels as stored (no orientation applied: the orientation tag goes into the eXIf chunk). */
    @Suppress("DEPRECATION")
    private fun regionPath(src: File, dest: File): Boolean {
        val dec = (if (Build.VERSION.SDK_INT >= 31) BitmapRegionDecoder.newInstance(src.path) else BitmapRegionDecoder.newInstance(src.path, false))
            ?: return false
        try {
            val w = dec.width
            val h = dec.height
            val rows = maxOf(1, minOf(h, STRIP_PIXELS / w))
            val opts = BitmapFactory.Options().apply { inPreferredConfig = Bitmap.Config.RGBA_F16 }
            fun strip(y0: Int) = dec.decodeRegion(Rect(0, y0, w, minOf(h, y0 + rows)), opts)
            var bmp = strip(0) ?: return false
            val space = spaceOf(bmp.colorSpace) ?: return false
            Log.i("DappleIngest", "deep ${src.name}: ${space.label}, ${bmp.config}, ${w}x$h, region strips of $rows rows")
            Png16Writer(dest, w, h, space, exifBlock(src, true)).use { png ->
                var y0 = 0
                val buf = ByteArray(rows * (1 + w * 6))
                while (y0 < h) {
                    val b = bmp
                    if (b.width != w) return false
                    png.write(buf, packRows(b, buf))
                    y0 += b.height
                    b.recycle()
                    if (y0 < h) bmp = strip(y0) ?: return false
                }
            }
            return true
        } finally {
            dec.recycle()
        }
    }

    /** Whole-picture fallback: ImageDecoder (orientation applied, so no tag), cut into the same strips. */
    private fun fullPath(src: File, dest: File): Boolean {
        fun decode(target: ColorSpace?) = ImageDecoder.decodeBitmap(ImageDecoder.createSource(src)) { d, _, _ ->
            d.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
            if (target != null) d.setTargetColorSpace(target)
        }
        var bm = decode(null)
        var space = spaceOf(bm.colorSpace)
        if (space == null) {
            bm.recycle()
            bm = decode(ColorSpace.get(ColorSpace.Named.DISPLAY_P3))
            space = spaceOf(bm.colorSpace) ?: PngSpace("Display P3", intArrayOf(12, 13))
        }
        Log.i("DappleIngest", "deep ${src.name}: ${space.label}, ${bm.config}, ${bm.width}x${bm.height}, whole-picture decode")
        val w = bm.width
        val h = bm.height
        val rows = maxOf(1, minOf(h, STRIP_PIXELS / w))
        val buf = ByteArray(rows * (1 + w * 6))
        Png16Writer(dest, w, h, space, exifBlock(src, false)).use { png ->
            var y0 = 0
            while (y0 < h) {
                val n = minOf(rows, h - y0)
                val s = Bitmap.createBitmap(bm, 0, y0, w, n)
                png.write(buf, packRows(s, buf))
                if (s !== bm) s.recycle()
                y0 += n
            }
        }
        bm.recycle()
        return true
    }

    /** Raw pixels of [b] in its own encoding (copyPixelsToBuffer: no colour conversion) -> PNG scanlines of 16-bit RGB, alpha flattened to white. */
    fun packRows(b: Bitmap, out: ByteArray): Int {
        val bb = ByteBuffer.allocate(b.byteCount).order(ByteOrder.nativeOrder())
        b.copyPixelsToBuffer(bb)
        val rb = b.rowBytes
        val w = b.width
        var o = 0
        for (y in 0 until b.height) {
            out[o++] = 0
            val row = y * rb
            for (x in 0 until w) {
                when (b.config) {
                    Bitmap.Config.RGBA_F16 -> {
                        val p = row + x * 8
                        val a = Half.toFloat(bb.getShort(p + 6)).coerceIn(0f, 1f)
                        for (c in 0 until 3) {
                            val v = (Half.toFloat(bb.getShort(p + 2 * c)).coerceIn(0f, 1f) + (1f - a)).coerceIn(0f, 1f)
                            val u = (v * 65535f + 0.5f).toInt()
                            out[o++] = (u shr 8).toByte(); out[o++] = u.toByte()
                        }
                    }
                    Bitmap.Config.RGBA_1010102 -> {
                        val v = bb.getInt(row + x * 4)
                        for (c in 0 until 3) {
                            val u = (((v ushr (10 * c)) and 1023) * 65535 + 511) / 1023
                            out[o++] = (u shr 8).toByte(); out[o++] = u.toByte()
                        }
                    }
                    else -> {  // ARGB_8888 (R, G, B, A bytes, premultiplied)
                        val p = row + x * 4
                        val a = bb.get(p + 3).toInt() and 0xFF
                        for (c in 0 until 3) {
                            val u = minOf(255, (bb.get(p + c).toInt() and 0xFF) + (255 - a)) * 257
                            out[o++] = (u shr 8).toByte(); out[o++] = u.toByte()
                        }
                    }
                }
            }
        }
        return o
    }

    /** Before API 28: HEIC / WebP -> 8-bit sRGB JPEG (BitmapFactory reads WebP only there). */
    fun toJpeg(src: File, dest: File, quality: Int = 95): Boolean {
        var bmp: Bitmap? = null
        try {
            val o = BitmapFactory.Options().apply {
                inPreferredConfig = Bitmap.Config.ARGB_8888
                if (Build.VERSION.SDK_INT >= 26) inPreferredColorSpace = ColorSpace.get(ColorSpace.Named.SRGB)
            }
            bmp = BitmapFactory.decodeFile(src.path, o) ?: return false
            val flat = Bitmap.createBitmap(bmp.width, bmp.height, Bitmap.Config.ARGB_8888)
            Canvas(flat).apply { drawColor(-1); drawBitmap(bmp, 0f, 0f, null) }
            dest.parentFile?.mkdirs()
            dest.outputStream().use { if (!flat.compress(Bitmap.CompressFormat.JPEG, quality, it)) return false }
            flat.recycle()
            copyExif(src, dest)
            return dest.length() > 0
        } catch (e: Exception) {
            dest.delete()
            return false
        } finally {
            bmp?.recycle()
        }
    }

    private fun copyExif(src: File, dest: File) {
        try {
            val a = ExifInterface(src.path)
            val b = ExifInterface(dest.path)
            var any = false
            for (t in keep) a.getAttribute(t)?.let { b.setAttribute(t, it); any = true }
            if (any) b.saveAttributes()  // the pixels are already upright: no orientation tag
        } catch (_: Exception) {
        }
    }
}
