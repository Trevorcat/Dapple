package com.dapple.dapple

import android.content.ContentUris
import android.content.Context
import android.graphics.Bitmap
import android.media.Image
import android.media.MediaCodec
import android.media.MediaCodecList
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaMetadataRetriever
import android.media.MediaMuxer
import android.net.Uri
import android.os.Build
import android.provider.MediaStore
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer

/** The platform has no decoder that can decode this stream correctly; it would hand back false-colour frames. */
class UnsupportedVideo(msg: String) : Exception(msg)

/** Platform side of "video -> Motion Photo": MediaStore listing, the decoder decision (probe), and the calls into core/video + the encoders ([SoftwareMedia]). */
class LiveBridge(private val ctx: Context) {
    private companion object { const val TAG = "DappleLive" }
    private val sw by lazy { SoftwareMedia(ctx) }


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

    /** The newest MediaStore video whose file name is [stem].<ext>, same shape as a listVideos row; null when gone. */
    fun findVideo(stem: String): Map<String, Any?>? {
        val base = MediaStore.Video.Media.EXTERNAL_CONTENT_URI
        val proj = arrayOf(
            MediaStore.Video.Media._ID, MediaStore.Video.Media.DISPLAY_NAME, MediaStore.Video.Media.DURATION,
            MediaStore.Video.Media.SIZE, MediaStore.Video.Media.DATE_ADDED, MediaStore.Video.Media.WIDTH, MediaStore.Video.Media.HEIGHT,
        )
        val esc = stem.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_")
        ctx.contentResolver.query(base, proj, "${MediaStore.Video.Media.DISPLAY_NAME} LIKE ? ESCAPE '\\' AND ${MediaStore.Video.Media.SIZE} > 0",
            arrayOf("$esc.%"), "${MediaStore.Video.Media.DATE_ADDED} DESC")?.use { c ->
            if (c.moveToFirst()) {
                val id = c.getLong(0)
                return mapOf(
                    "id" to id, "uri" to ContentUris.withAppendedId(base, id).toString(), "name" to c.getString(1),
                    "durationMs" to c.getLong(2), "size" to c.getLong(3), "addedMs" to c.getLong(4) * 1000L,
                    "width" to c.getInt(5), "height" to c.getInt(6),
                )
            }
        }
        return null
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
                "decodable" to (probe(uri)["decodable"] == true),
                "software" to (probe(uri)["software"] == true),
                "format" to probe(uri)["format"],
            ) + coreInfo(uri)
        } finally {
            r.release()
        }
    }


    /** What the core finds in the file: bit depth, colour classification, Sony S-Log3 metadata, audio. Empty when it cannot open it. */
    private fun coreInfo(uri: String): Map<String, Any?> {
        if (!SwVideo.available || probe(uri)["decodable"] != true) return emptyMap()
        return try {
            SwVideo.open(ctx, uri, decodeSel(uri), SwVideo.SRC_AUTO).use { s ->
                val i = s.info
                mapOf(
                    "bitDepth" to i.bitDepth, "chroma" to i.chroma, "colorKind" to i.colorKind, "hdr" to i.hdr,
                    "slogDetected" to i.slogDetected, "captureGamma" to i.captureGamma, "capturePrimaries" to i.capturePrimaries,
                    "decoder" to i.decoder, "hardwareDecode" to i.hardware, "audioCodec" to i.audioCodec,
                )
            }
        } catch (_: Exception) { emptyMap() }
    }

    private val probeCache = HashMap<String, Map<String, Any?>>()

    /**
     * Can the platform decode the first video track of [uri] correctly? Platform decoders (the emulator's in
     * particular) can accept a 4:2:2 / 10-bit H.264 stream and still return false-colour frames instead of failing,
     * so the stream is checked against the decoders' advertised profiles before any frame is trusted.
     */
    fun probe(uri: String): Map<String, Any?> = synchronized(probeCache) {
        probeCache.getOrPut(uri) { doProbe(uri) }
    }

    /**
     * Probes the video embedded in a Motion Photo JPEG (located through the Container XMP item length; the MP4 is the
     * file tail). Used to find items written before the software path existed, whose clip is an undecodable
     * 10-bit / 4:2:2 stream copy. Same map as [probe]; "decodable" is true when the file has no clip.
     */
    fun probeEmbedded(path: String): Map<String, Any?> {
        try {
            val file = File(path)
            val head = ByteArray(minOf(file.length(), 262144L).toInt())
            file.inputStream().use { it.read(head) }
            val xmp = String(head, Charsets.ISO_8859_1)
            val m = Regex("Item:Mime=\"video/mp4\"[^>]*?Item:Length=\"(\\d+)\"|Item:Length=\"(\\d+)\"[^>]*?Item:Mime=\"video/mp4\"").find(xmp)
            val len = (m?.groupValues?.get(1)?.toLongOrNull() ?: m?.groupValues?.get(2)?.toLongOrNull()) ?: 0L
            if (len <= 0 || len >= file.length()) return mapOf("decodable" to true, "software" to false, "format" to "no clip")
            return doProbe(path, file.length() - len, len)
        } catch (e: Exception) {
            return mapOf("decodable" to true, "software" to false, "format" to "unknown")
        }
    }

    private fun doProbe(uri: String, offset: Long = -1, length: Long = -1): Map<String, Any?> {
        val ex = MediaExtractor()
        try {
            if (offset >= 0) {
                java.io.FileInputStream(uri).use { ex.setDataSource(it.fd, offset, length) }
            } else if (uri.startsWith("/")) ex.setDataSource(uri) else ex.setDataSource(ctx, Uri.parse(uri), null)
            var f: MediaFormat? = null
            for (i in 0 until ex.trackCount) {
                val tf = ex.getTrackFormat(i)
                if ((tf.getString(MediaFormat.KEY_MIME) ?: "").startsWith("video/")) { f = tf; break }
            }
            if (f == null) return mapOf("decodable" to false, "reason" to "no video track", "format" to "none")
            val mime = f.getString(MediaFormat.KEY_MIME)!!
            var need = 0
            var label = mime
            var bitDepth = 8
            var chroma = 1
            if (f.containsKey("csd-0") && (mime == "video/avc" || mime == "video/hevc")) {
                val bb = f.getByteBuffer("csd-0")!!.duplicate()
                val arr = ByteArray(bb.remaining()); bb.get(arr)
                if (mime == "video/avc") {
                    val fmt = StreamProbe.parseAvcSps(arr)
                    if (fmt != null) { label = fmt.label; need = fmt.requiredProfile; bitDepth = fmt.bitDepthLuma; chroma = fmt.chromaFormatIdc }
                } else {
                    val fmt = HevcProbe.parseSps(arr)
                    if (fmt != null) { label = fmt.label; need = fmt.requiredProfile; bitDepth = fmt.bitDepthLuma; chroma = fmt.chromaFormatIdc }
                }
            } else if (Build.VERSION.SDK_INT >= 33 && f.containsKey(MediaFormat.KEY_PROFILE)) {
                need = f.getInteger(MediaFormat.KEY_PROFILE)
                label = "$mime profile $need"
            }
            val hw = need >= 0 && try {
                MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos.any { ci ->
                    if (ci.isEncoder || !ci.supportedTypes.any { it.equals(mime, true) }) return@any false
                    // plain 8-bit 4:2:0 needs no particular profile; anything else must be advertised
                    need == 0 || ci.getCapabilitiesForType(mime).profileLevels.any {
                        it.profile == need || (mime == "video/hevc" && need == 2 && (it.profile == 0x1000 || it.profile == 0x2000))
                    }
                }
            } catch (_: Exception) { true }
            // No platform decoder: fall back to the FFmpeg software decoder (H.264 / HEVC only).
            val sw = !hw && (mime == "video/avc" || mime == "video/hevc") && SwVideo.available
            // the codec ladder decides which decoder the core uses: declared profile + output format, and for 10-bit hardware a
            // one-time self-test (CodecManager); software FFmpeg is always the last rung
            val pick = if (hw && SwVideo.available) CodecManager.get(ctx).decodeFor(StreamSpec(mime, need, bitDepth, chroma)) else DecodePick(DecodeRoute.SOFTWARE, null)
            Log.i(TAG, "probe $label: ${if (hw) "declared by the platform" else if (sw) "software (FFmpeg)" else "unsupported"}; decode route ${pick.route} ${pick.codec ?: ""}")
            return mapOf(
                "decodable" to (hw || sw), "hardware" to (pick.route != DecodeRoute.SOFTWARE), "software" to (sw || pick.route == DecodeRoute.SOFTWARE),
                "route" to pick.route.name, "hwPref" to pick.route.hwPref, "hwCodec" to pick.codec,
                "reason" to (if (hw || sw) "" else label), "format" to label,
            )
        } catch (e: Exception) {
            // unreadable source: let the normal paths report their own error
            return mapOf("decodable" to true, "reason" to "", "format" to "unknown")
        } finally {
            ex.release()
        }
    }

    private fun requireDecodable(uri: String) {
        val p = probe(uri)
        if (p["decodable"] != true) throw UnsupportedVideo("unsupported video: ${p["reason"]}")
    }

    private fun decodeSel(uri: String): SwVideo.DecodeSel {
        val p = probe(uri)
        return SwVideo.DecodeSel((p["hwPref"] as? Int) ?: if (p["hardware"] == true) 1 else 0, p["hwCodec"] as? String)
    }

    /**
     * What the clip encoder can do on this device: whether an HEVC Main10 (P010) encoder exists for 1080p30 and which one.
     * The probe's verdict on [uri] is the decoder decision input; this is its encoder counterpart.
     */
    fun capabilities(): Map<String, Any?> {
        val mgr = CodecManager.get(ctx)
        val first = mgr.encodeCandidates(EncodeJob(1920, 1080, 30)).firstOrNull()
        return mapOf(
            "main10Encoder" to first?.takeIf { it.rung.tenBit }?.codec, "ultraHdr" to (Build.VERSION.SDK_INT >= 34),
            "videoLib" to SwVideo.version(),
        )
    }

    /** The codec capability report (declared capabilities, ladders, self-test results) as JSON; [run] first runs every missing self-test, [force] repeats all. */
    fun codecReport(run: Boolean, force: Boolean): String {
        val mgr = CodecManager.get(ctx)
        if (run || force) mgr.runAll(force)
        return mgr.report().toString()
    }

    /** One JPEG per entry of [timesMs] (filmstrip). Missing frames are null. */
    fun thumbs(uri: String, timesMs: List<Long>, maxEdge: Int, destDir: String, prefix: String, source: Int): List<String?> {
        if (probe(uri)["decodable"] != true) return timesMs.map { null }
        return sw.thumbs(uri, decodeSel(uri), source, timesMs, maxEdge, destDir, prefix)
    }

    /** The exact frame at [timeMs] as a JPEG (the Motion Photo still): Ultra HDR for HDR sources, dithered SDR otherwise. */
    fun frameToFile(uri: String, timeMs: Long, dest: String, maxEdge: Int, quality: Int, source: Int, compat: Boolean): Map<String, Any?> {
        requireDecodable(uri)
        return sw.frame(uri, decodeSel(uri), source, timeMs, dest, maxEdge, quality, compat)
    }

    /**
     * Decodes (core/video) and re-encodes [startMs, startMs + durationMs] into [dest] (MP4): HEVC Main10 when the device has an
     * encoder for it, else 8-bit H.264 ([compat] forces the 8-bit H.264 + SDR sharing version); LPCM audio becomes AAC. [edit] = the still's edit
     * (film included): every frame goes through the engine's node graph before it is encoded (docs/engine/pipeline-architecture.md section 27).
     */
    fun trim(uri: String, startMs: Long, durationMs: Long, dest: String, mute: Boolean, source: Int, compat: Boolean, film: Boolean = false, edit: EditSpec? = null): Map<String, Any?> {
        requireDecodable(uri)
        if (edit != null && !DappleVideoEdit.available) throw UnsupportedVideo("the edit engine is not available in this build")
        return sw.clip(uri, decodeSel(uri), source, startMs, durationMs, dest, mute, compat, film, edit)
    }
}
