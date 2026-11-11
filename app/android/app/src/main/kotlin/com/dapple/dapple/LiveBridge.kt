package com.dapple.dapple

import android.content.ContentUris
import android.content.Context
import android.graphics.Bitmap
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaMetadataRetriever
import android.media.MediaMuxer
import android.net.Uri
import android.provider.MediaStore
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer

/** Platform side of "video -> Motion Photo": listing, filmstrip / cover frames, stream-copy trimming. */
class LiveBridge(private val ctx: Context) {

    fun listVideos(limit: Int): List<Map<String, Any?>> {
        val base = MediaStore.Video.Media.EXTERNAL_CONTENT_URI
        val proj = arrayOf(
            MediaStore.Video.Media._ID, MediaStore.Video.Media.DISPLAY_NAME, MediaStore.Video.Media.DURATION,
            MediaStore.Video.Media.SIZE, MediaStore.Video.Media.DATE_ADDED, MediaStore.Video.Media.WIDTH,
            MediaStore.Video.Media.HEIGHT, MediaStore.Video.Media.DATE_TAKEN,
        )
        val out = ArrayList<Map<String, Any?>>()
        ctx.contentResolver.query(base, proj, "${MediaStore.Video.Media.DURATION} > 0 AND ${MediaStore.Video.Media.SIZE} > 0", null,
            "${MediaStore.Video.Media.DATE_ADDED} DESC")?.use { c ->
            while (c.moveToNext() && out.size < limit) {
                val id = c.getLong(0)
                out.add(
                    mapOf(
                        "id" to id,
                        "uri" to ContentUris.withAppendedId(base, id).toString(),
                        "name" to c.getString(1),
                        "durationMs" to c.getLong(2),
                        "size" to c.getLong(3),
                        "addedMs" to c.getLong(4) * 1000L,
                        "width" to c.getInt(5),
                        "height" to c.getInt(6),
                    )
                )
            }
        }
        return out
    }

    private fun retriever(uri: String): MediaMetadataRetriever {
        val r = MediaMetadataRetriever()
        if (uri.startsWith("/")) r.setDataSource(uri) else r.setDataSource(ctx, Uri.parse(uri))
        return r
    }

    fun info(uri: String): Map<String, Any?> {
        val r = retriever(uri)
        try {
            fun s(k: Int) = r.extractMetadata(k)
            return mapOf(
                "durationMs" to (s(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toLongOrNull() ?: 0L),
                "width" to (s(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH)?.toIntOrNull() ?: 0),
                "height" to (s(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT)?.toIntOrNull() ?: 0),
                "rotation" to (s(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION)?.toIntOrNull() ?: 0),
                "hasAudio" to (s(MediaMetadataRetriever.METADATA_KEY_HAS_AUDIO) == "yes"),
                "mime" to s(MediaMetadataRetriever.METADATA_KEY_MIMETYPE),
            )
        } finally {
            r.release()
        }
    }

    private fun scaleDown(b: Bitmap, maxEdge: Int): Bitmap {
        if (maxEdge <= 0 || maxOf(b.width, b.height) <= maxEdge) return b
        val k = maxEdge.toFloat() / maxOf(b.width, b.height)
        val s = Bitmap.createScaledBitmap(b, maxOf(1, (b.width * k).toInt()), maxOf(1, (b.height * k).toInt()), true)
        if (s !== b) b.recycle()
        return s
    }

    private fun frame(r: MediaMetadataRetriever, timeMs: Long, maxEdge: Int, exact: Boolean): Bitmap? {
        val opt = if (exact) MediaMetadataRetriever.OPTION_CLOSEST else MediaMetadataRetriever.OPTION_CLOSEST_SYNC
        val t = timeMs * 1000L
        if (maxEdge > 0) {
            try {
                val w = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH)?.toIntOrNull() ?: 0
                val h = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT)?.toIntOrNull() ?: 0
                val rot = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION)?.toIntOrNull() ?: 0
                val uw = if (rot == 90 || rot == 270) h else w
                val uh = if (rot == 90 || rot == 270) w else h
                if (uw > 0 && uh > 0) {
                    val k = maxEdge.toFloat() / maxOf(uw, uh)
                    if (k < 1f) return r.getScaledFrameAtTime(t, opt, maxOf(1, (uw * k).toInt()), maxOf(1, (uh * k).toInt()))
                }
            } catch (_: Exception) {}
        }
        return r.getFrameAtTime(t, opt)?.let { scaleDown(it, maxEdge) }
    }

    private fun writeJpeg(b: Bitmap, dest: File, quality: Int) {
        dest.parentFile?.mkdirs()
        FileOutputStream(dest).use { b.compress(Bitmap.CompressFormat.JPEG, quality, it) }
    }

    /** One JPEG per entry of [timesMs] (filmstrip). Missing frames are null. */
    fun thumbs(uri: String, timesMs: List<Long>, maxEdge: Int, destDir: String, prefix: String): List<String?> {
        val r = retriever(uri)
        try {
            val dir = File(destDir)
            return timesMs.mapIndexed { i, t ->
                try {
                    val b = frame(r, t, maxEdge, false) ?: return@mapIndexed null
                    val f = File(dir, "${prefix}_$i.jpg")
                    writeJpeg(b, f, 80)
                    b.recycle()
                    f.path
                } catch (_: Exception) { null }
            }
        } finally {
            r.release()
        }
    }

    /** The exact frame at [timeMs] as a JPEG (the Motion Photo still). */
    fun frameToFile(uri: String, timeMs: Long, dest: String, maxEdge: Int, quality: Int): String {
        val r = retriever(uri)
        try {
            val b = frame(r, timeMs, maxEdge, true) ?: throw IllegalStateException("no frame at $timeMs ms")
            writeJpeg(b, File(dest), quality)
            b.recycle()
            return dest
        } finally {
            r.release()
        }
    }

    /**
     * Stream-copies [startMs, startMs + durationMs] of the video into [dest] (MP4, no re-encode). The cut starts at
     * the preceding key frame, so the clip may begin up to one GOP earlier than asked; the real start is returned.
     * Audio that MP4 cannot carry (PCM) is left out.
     */
    fun trim(uri: String, startMs: Long, durationMs: Long, dest: String, mute: Boolean): Map<String, Any?> {
        val ex = MediaExtractor()
        if (uri.startsWith("/")) ex.setDataSource(uri) else ex.setDataSource(ctx, Uri.parse(uri), null)
        var vTrack = -1
        var aTrack = -1
        for (i in 0 until ex.trackCount) {
            val mime = ex.getTrackFormat(i).getString(MediaFormat.KEY_MIME) ?: continue
            if (mime.startsWith("video/") && vTrack < 0) vTrack = i
            else if (mime.startsWith("audio/") && aTrack < 0) aTrack = i
        }
        if (vTrack < 0) { ex.release(); throw IllegalStateException("no video track") }
        var rotation = 0
        try {
            val r = retriever(uri)
            rotation = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION)?.toIntOrNull() ?: 0
            r.release()
        } catch (_: Exception) {}
        val destFile = File(dest)
        destFile.parentFile?.mkdirs()
        destFile.delete()
        val muxer = MediaMuxer(dest, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        try {
            muxer.setOrientationHint(rotation)
            val vOut = muxer.addTrack(ex.getTrackFormat(vTrack))
            var aOut = -1
            if (!mute && aTrack >= 0) {
                try { aOut = muxer.addTrack(ex.getTrackFormat(aTrack)) } catch (_: Exception) { aOut = -1 }
            }
            val startUs = startMs * 1000L
            val endUs = (startMs + durationMs) * 1000L
            ex.selectTrack(vTrack)
            ex.seekTo(startUs, MediaExtractor.SEEK_TO_PREVIOUS_SYNC)
            val baseUs = maxOf(0L, ex.sampleTime)
            if (aOut >= 0) {
                ex.selectTrack(aTrack)
                ex.seekTo(baseUs, MediaExtractor.SEEK_TO_PREVIOUS_SYNC)
            }
            var maxIn = 1 shl 20
            for (t in listOf(vTrack, aTrack)) if (t >= 0) {
                val f = ex.getTrackFormat(t)
                if (f.containsKey(MediaFormat.KEY_MAX_INPUT_SIZE)) maxIn = maxOf(maxIn, f.getInteger(MediaFormat.KEY_MAX_INPUT_SIZE))
            }
            var buf = ByteBuffer.allocateDirect(maxOf(maxIn, 4 shl 20))
            val info = MediaCodec.BufferInfo()
            muxer.start()
            var frames = 0
            var lastUs = baseUs
            while (true) {
                val track = ex.sampleTrackIndex
                if (track < 0) break
                val pts = ex.sampleTime
                if (track == vTrack && pts > endUs) break
                if (track == aTrack && pts > endUs) { ex.advance(); continue }
                if (track == aTrack && pts < baseUs) { ex.advance(); continue }
                val size = ex.sampleSize.toInt()
                if (size > buf.capacity()) buf = ByteBuffer.allocateDirect(size)
                buf.clear()
                val n = ex.readSampleData(buf, 0)
                if (n < 0) break
                info.set(0, n, pts - baseUs, 0)
                info.flags = if (ex.sampleFlags and MediaExtractor.SAMPLE_FLAG_SYNC != 0) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0
                if (track == vTrack) { muxer.writeSampleData(vOut, buf, info); frames++; lastUs = maxOf(lastUs, pts) }
                else if (aOut >= 0 && track == aTrack) muxer.writeSampleData(aOut, buf, info)
                ex.advance()
            }
            muxer.stop()
            return mapOf(
                "actualStartMs" to baseUs / 1000L,
                "durationMs" to (lastUs - baseUs) / 1000L,
                "frames" to frames,
                "audio" to (aOut >= 0),
                "path" to dest,
            )
        } catch (e: Exception) {
            try { muxer.release() } catch (_: Exception) {}
            destFile.delete()
            throw e
        } finally {
            try { muxer.release() } catch (_: Exception) {}
            ex.release()
        }
    }
}
