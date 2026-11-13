package com.dapple.dapple

import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.ColorSpace
import android.graphics.ImageDecoder
import android.media.ExifInterface
import android.os.Build
import java.io.File

/**
 * HEIC / HEIF / PNG / WebP -> 8-bit sRGB JPEG (quality 95) with the platform decoder, so the engine needs no HEIF library.
 * ImageDecoder (API 28+) applies the EXIF orientation and converts to sRGB (a Display P3 HEIC is converted, not read as sRGB);
 * before API 28 BitmapFactory reads PNG / WebP only (HEIC returns false). Capture date, camera, exposure and GPS are copied into the JPEG.
 * Lost: colours outside sRGB (clipped), bit depth beyond 8, HDR gain maps (the SDR base image is used), alpha (flattened to white).
 */
object ImageTranscode {
    private val keep = listOf(
        ExifInterface.TAG_DATETIME_ORIGINAL, ExifInterface.TAG_DATETIME_DIGITIZED, ExifInterface.TAG_DATETIME,
        ExifInterface.TAG_MAKE, ExifInterface.TAG_MODEL,
        ExifInterface.TAG_EXPOSURE_TIME, ExifInterface.TAG_F_NUMBER, ExifInterface.TAG_FOCAL_LENGTH,
        ExifInterface.TAG_GPS_LATITUDE, ExifInterface.TAG_GPS_LATITUDE_REF, ExifInterface.TAG_GPS_LONGITUDE, ExifInterface.TAG_GPS_LONGITUDE_REF,
        ExifInterface.TAG_GPS_ALTITUDE, ExifInterface.TAG_GPS_ALTITUDE_REF,
    )

    private val types = setOf("heic", "heif", "png", "webp")

    /** True for the types that go through [toJpeg] (the engine reads JPEG and the RAW formats itself). */
    fun needsTranscode(name: String) = name.substringAfterLast('.', "").lowercase() in types

    /** "IMG_1.HEIC" -> "IMG_1.jpg". */
    fun jpegName(name: String) = (if (name.contains('.')) name.substringBeforeLast('.') else name) + ".jpg"

    fun toJpeg(src: File, dest: File, quality: Int = 95): Boolean {
        var bmp: Bitmap? = null
        try {
            bmp = decode(src) ?: return false
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

    private fun decode(src: File): Bitmap? {
        if (Build.VERSION.SDK_INT >= 28) {
            val bm = ImageDecoder.decodeBitmap(ImageDecoder.createSource(src)) { d, _, _ ->
                d.allocator = ImageDecoder.ALLOCATOR_SOFTWARE
                d.setTargetColorSpace(ColorSpace.get(ColorSpace.Named.SRGB))
            }
            return if (bm.config == Bitmap.Config.ARGB_8888) bm else bm.copy(Bitmap.Config.ARGB_8888, false).also { bm.recycle() }
        }
        val o = BitmapFactory.Options().apply {
            inPreferredConfig = Bitmap.Config.ARGB_8888
            if (Build.VERSION.SDK_INT >= 26) inPreferredColorSpace = ColorSpace.get(ColorSpace.Named.SRGB)
        }
        return BitmapFactory.decodeFile(src.path, o)
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
