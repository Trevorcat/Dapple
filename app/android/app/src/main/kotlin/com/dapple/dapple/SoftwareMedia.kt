package com.dapple.dapple

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Gainmap
import android.graphics.Matrix
import android.media.Image
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaExtractor
import android.media.MediaFormat
import android.media.MediaMuxer
import android.net.Uri
import android.os.Build
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/**
 * Everything that turns decoded frames into files: filmstrip thumbnails, the Motion Photo still (Ultra HDR JPEG for
 * HDR sources, dithered 10-bit-derived SDR JPEG otherwise), and the 3 s clip (HEVC Main10 through MediaCodec when the
 * device has such an encoder, else 8-bit H.264; LPCM audio transcoded to AAC). Decoding itself is core/video
 * ([SwVideo]): hardware first, software fallback, one place for colour handling. Kotlin only encodes, muxes and writes.
 */
class SoftwareMedia(private val ctx: Context) {
    companion object {
        const val TAG = "DappleLive"
        const val CLIP_MAX_EDGE = 1920
        const val CLIP_MAX_FPS = 30.0
        const val TIMEOUT_MS = 180_000L
        private const val COLOR_FormatYUVP010 = 54
        private const val HEVC_MAIN10 = 2          // MediaCodecInfo.CodecProfileLevel.HEVCProfileMain10
        private const val AVC_HIGH = 8
        // FFmpeg AVCOL_* -> MediaFormat
        private fun standardOf(matrix: Int) = if (matrix == 9 || matrix == 10) MediaFormat.COLOR_STANDARD_BT2020 else MediaFormat.COLOR_STANDARD_BT709
        private fun transferOf(trc: Int) = when (trc) {
            18 -> MediaFormat.COLOR_TRANSFER_HLG
            16 -> MediaFormat.COLOR_TRANSFER_ST2084
            else -> MediaFormat.COLOR_TRANSFER_SDR_VIDEO
        }
    }

    private fun bitmapOf(buf: ByteBuffer, w: Int, h: Int, rotation: Int): Bitmap {
        buf.rewind()
        val b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        b.copyPixelsFromBuffer(buf)
        return rotate(b, rotation)
    }

    private fun rotate(b: Bitmap, rotation: Int): Bitmap {
        if (rotation == 0) return b
        val m = Matrix().apply { postRotate(rotation.toFloat()) }
        val r = Bitmap.createBitmap(b, 0, 0, b.width, b.height, m, true)
        if (r !== b) b.recycle()
        return r
    }

    private fun writeJpeg(b: Bitmap, dest: File, quality: Int, format: Bitmap.CompressFormat = Bitmap.CompressFormat.JPEG) {
        dest.parentFile?.mkdirs()
        FileOutputStream(dest).use { if (!b.compress(format, quality, it)) throw IllegalStateException("JPEG encode failed") }
    }

    /** One frame at [timeMs] as a bitmap (display orientation, 8-bit dithered from the 16-bit master). */
    private fun grab(s: SwVideo.Session, timeMs: Long, maxEdge: Int, fast: Boolean): Bitmap? {
        s.begin(timeMs * 1000L, timeMs * 1000L, maxEdge, 0.0, SwVideo.RGBA, fast)
        val buf = ByteBuffer.allocateDirect(s.bufferSize(SwVideo.RGBA))
        if (s.next(buf) < 0) return null
        return bitmapOf(buf, s.outW, s.outH, s.info.rotation)
    }

    fun thumbs(uri: String, dec: SwVideo.DecodeSel, source: Int, timesMs: List<Long>, maxEdge: Int, destDir: String, prefix: String): List<String?> {
        val t0 = System.nanoTime()
        SwVideo.open(ctx, uri, dec, source).use { s ->
            val out = timesMs.mapIndexed { i, t ->
                try {
                    val b = grab(s, t, maxEdge, true) ?: return@mapIndexed null
                    val f = File(destDir, "${prefix}_$i.jpg")
                    writeJpeg(b, f, 80)
                    b.recycle()
                    f.path
                } catch (e: Exception) {
                    Log.w(TAG, "thumb $t ms failed: $e")
                    null
                }
            }
            Log.i(TAG, "thumbs x${timesMs.size} @${maxEdge}px: ${(System.nanoTime() - t0) / 1_000_000} ms (${s.info})")
            return out
        }
    }

    /**
     * The Motion Photo still at [timeMs]. HDR (HLG / PQ) sources on Android 14+ become an Ultra HDR JPEG (SDR base +
     * gain map); everything else is an SDR JPEG made from the 16-bit master with ordered dithering (no banding).
     * [compat] forces the plain SDR JPEG (the sharing-compatible version).
     */
    fun frame(uri: String, dec: SwVideo.DecodeSel, source: Int, timeMs: Long, dest: String, maxEdge: Int, quality: Int, compat: Boolean): Map<String, Any?> {
        val t0 = System.nanoTime()
        SwVideo.open(ctx, uri, dec, source).use { s ->
            val hdr = !compat && Build.VERSION.SDK_INT >= 34 && (s.info.colorKind == 2 || s.info.colorKind == 3)
            var kind = "sdr-jpeg"
            if (hdr) {
                try {
                    stillUltraHdr(s, timeMs, maxEdge, quality, File(dest))
                    kind = "ultra-hdr"
                } catch (e: Exception) {
                    Log.w(TAG, "Ultra HDR still failed ($e), SDR JPEG instead")
                    File(dest).delete()
                }
            }
            if (kind != "ultra-hdr") {
                val b = grab(s, timeMs, maxEdge, false) ?: throw IllegalStateException("no frame at $timeMs ms")
                writeJpeg(b, File(dest), quality)
                b.recycle()
            }
            val ms = (System.nanoTime() - t0) / 1_000_000
            Log.i(TAG, "still @${timeMs}ms maxEdge=$maxEdge $kind: $ms ms (${s.info})")
            afterDecode(dec, s)
            return mapOf("path" to dest, "kind" to kind, "decoder" to s.info.decoder, "hardware" to s.info.hardware,
                "colorKind" to s.info.colorKind, "elapsedMs" to ms)
        }
    }

    /**
     * The Ultra HDR cover of an edited Motion Photo (docs section 30.14). [sdrJpeg] is the exported still of the same edit; the cover frame at [timeMs] goes through the
     * engine session of [edit] (the same one the clip uses), its HDR rendition gives the gain map and [out] = [sdrJpeg] + gain map (or a plain copy of it when the
     * frame has no headroom, the source is SDR or the edit has film). Needs no Android 14 API: the container is written by the engine.
     */
    fun coverHdr(uri: String, dec: SwVideo.DecodeSel, source: Int, timeMs: Long, edit: EditSpec, sdrJpeg: String, out: String, forceHdr: Boolean): Map<String, Any?> {
        val t0 = System.nanoTime()
        SwVideo.open(ctx, uri, dec, source).use { s ->
            val us = timeMs * 1000L
            val run = EditRun.start(s, edit.copy(refMs = timeMs), us, us)
            try {
                val r = run.session.coverUhdr(run.src, s.outW, s.outH, run.kind, run.rotation, sdrJpeg, out, forceHdr)
                afterDecode(dec, s)
                val ms = (System.nanoTime() - t0) / 1_000_000
                Log.i(TAG, "cover @${timeMs}ms hdr=${r.written} reason=${r.reason} peak=${r.peak} gainMax=${r.gainMax} session=${run.session.note}: $ms ms")
                return mapOf("written" to r.written, "reason" to r.reason, "peak" to r.peak.toDouble(), "gainMax" to r.gainMax.toDouble(),
                    "colorKind" to s.info.colorKind, "elapsedMs" to ms, "path" to out)
            } finally {
                run.close()
            }
        }
    }

    private fun stillUltraHdr(s: SwVideo.Session, timeMs: Long, maxEdge: Int, quality: Int, dest: File) {
        s.begin(timeMs * 1000L, timeMs * 1000L, maxEdge, 0.0, SwVideo.RGBAF16, false)
        val buf = ByteBuffer.allocateDirect(s.bufferSize(SwVideo.RGBAF16)).order(ByteOrder.nativeOrder())
        if (s.next(buf) < 0) throw IllegalStateException("no frame at $timeMs ms")
        val u = s.ultraHdr(buf)
        var base = Bitmap.createBitmap(s.outW, s.outH, Bitmap.Config.ARGB_8888)
        u.sdr.rewind(); base.copyPixelsFromBuffer(u.sdr)
        var gm = Bitmap.createBitmap(u.gw, u.gh, Bitmap.Config.ALPHA_8)
        u.gain.rewind(); gm.copyPixelsFromBuffer(u.gain)
        base = rotate(base, s.info.rotation)
        gm = rotate(gm, s.info.rotation)
        val g = Gainmap(gm)
        val m = u.meta
        g.setRatioMin(m[0], m[0], m[0])
        g.setRatioMax(m[1], m[1], m[1])
        g.setGamma(m[2], m[2], m[2])
        g.setEpsilonSdr(m[3], m[3], m[3])
        g.setEpsilonHdr(m[4], m[4], m[4])
        g.setMinDisplayRatioForHdrTransition(1.0f)
        g.setDisplayRatioForFullHdr(max(1.01f, m[1]))
        base.setGainmap(g)
        writeJpeg(base, dest, quality)  // a Bitmap with a gain map compresses to an Ultra HDR JPEG (Android 14+)
        base.recycle()
    }

    // ---- audio -----------------------------------------------------------------------------------------------------------
    private class Sample(val data: ByteArray, val ptsUs: Long, val flags: Int)
    private class AudioClip(val format: MediaFormat, val samples: List<Sample>, val how: String)

    /** AAC-LC from the core's PCM (Sony LPCM) with the platform AAC encoder. */
    private fun audioFromCore(s: SwVideo.Session, startUs: Long, endUs: Long): AudioClip? {
        val pcm = s.audioBegin(startUs, endUs)
        if (pcm == null) { Log.w(TAG, "no decodable audio in the source (${s.info.audioCodec})"); return null }
        val rate = pcm[0]
        val ch = pcm[1]
        val fmt = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, rate, ch).apply {
            setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
            setInteger(MediaFormat.KEY_BIT_RATE, 96_000 * ch)
            setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16384)
        }
        val enc = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC)
        try {
            enc.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            enc.start()
            val src = ByteBuffer.allocateDirect(1024 * ch * 2).order(ByteOrder.nativeOrder())
            val chunk = ByteArray(1024 * ch * 2)
            val ptsOut = LongArray(1)
            val out = ArrayList<Sample>()
            var outFormat: MediaFormat? = null
            val info = MediaCodec.BufferInfo()
            var inDone = false
            var outDone = false
            var firstPts = -1L
            var written = 0L
            val deadline = System.currentTimeMillis() + 60_000
            while (!outDone) {
                if (System.currentTimeMillis() > deadline) throw IllegalStateException("audio encode timed out")
                if (!inDone) {
                    val ii = enc.dequeueInputBuffer(5_000)
                    if (ii >= 0) {
                        src.clear()
                        val n = s.audioNext(src, 1024, ptsOut)
                        if (n <= 0) {
                            enc.queueInputBuffer(ii, 0, 0, if (firstPts < 0) 0 else firstPts + written * 1_000_000L / rate, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            inDone = true
                        } else {
                            if (firstPts < 0) firstPts = ptsOut[0]
                            val bytes = n * ch * 2
                            src.position(0); src.get(chunk, 0, bytes)
                            val ib = enc.getInputBuffer(ii)!!
                            ib.clear(); ib.put(chunk, 0, bytes)
                            enc.queueInputBuffer(ii, 0, bytes, firstPts + written * 1_000_000L / rate, 0)
                            written += n
                        }
                    }
                }
                val oi = enc.dequeueOutputBuffer(info, 5_000)
                if (oi == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    outFormat = enc.outputFormat
                } else if (oi >= 0) {
                    val ob = enc.getOutputBuffer(oi)!!
                    if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0 && info.size > 0) {
                        val d = ByteArray(info.size)
                        ob.position(info.offset); ob.limit(info.offset + info.size); ob.get(d)
                        out.add(Sample(d, max(0L, info.presentationTimeUs - startUs), info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME))
                    }
                    if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) outDone = true
                    enc.releaseOutputBuffer(oi, false)
                }
            }
            if (out.isEmpty() || outFormat == null) return null
            return AudioClip(outFormat, out, "aac (${s.info.audioCodec} ${rate} Hz ${ch} ch transcoded)")
        } finally {
            try { enc.stop() } catch (_: Exception) {}
            try { enc.release() } catch (_: Exception) {}
        }
    }

    /** AAC already in the source: copied as is. */
    private fun audioCopy(uri: String, startUs: Long, endUs: Long): AudioClip? {
        val ex = MediaExtractor()
        try {
            if (uri.startsWith("/")) ex.setDataSource(uri) else ex.setDataSource(ctx, Uri.parse(uri), null)
            var track = -1
            var fmt: MediaFormat? = null
            for (i in 0 until ex.trackCount) {
                val tf = ex.getTrackFormat(i)
                if (tf.getString(MediaFormat.KEY_MIME) == MediaFormat.MIMETYPE_AUDIO_AAC) { track = i; fmt = tf; break }
            }
            if (track < 0 || fmt == null) return null
            ex.selectTrack(track)
            ex.seekTo(startUs, MediaExtractor.SEEK_TO_PREVIOUS_SYNC)
            val ab = ByteBuffer.allocateDirect(1 shl 20)
            val out = ArrayList<Sample>()
            while (true) {
                val pts = ex.sampleTime
                if (pts < 0 || pts > endUs) break
                if (pts >= startUs) {
                    ab.clear()
                    val n = ex.readSampleData(ab, 0)
                    if (n < 0) break
                    val d = ByteArray(n); ab.position(0); ab.get(d, 0, n)
                    out.add(Sample(d, pts - startUs, if (ex.sampleFlags and MediaExtractor.SAMPLE_FLAG_SYNC != 0) MediaCodec.BUFFER_FLAG_KEY_FRAME else 0))
                }
                if (!ex.advance()) break
            }
            return if (out.isEmpty()) null else AudioClip(fmt, out, "aac (copied)")
        } catch (_: Exception) {
            return null
        } finally {
            ex.release()
        }
    }

    // ---- the clip --------------------------------------------------------------------------------------------------------
    /** The encoder accepted the job but does not deliver what it declares (stall, downgraded profile): demote it, try the next rung. */
    class EncoderUnreliable(msg: String) : Exception(msg)

    /**
     * Decodes [startMs, startMs + durationMs] (scaled to at most 1920 px and 30 fps while decoding) and encodes it with
     * MediaCodec along the encode ladder ([Ladder.encode]): HEVC Main10 (10-bit 4:2:0, BT.709 SDR or BT.2020 HLG / PQ with HDR
     * metadata) -> HEVC Main 8-bit -> H.264 High 8-bit. Each rung is used only after its one-time self-test passed on this
     * device; a failed self-test, a stalled encoder or a downgraded profile moves on to the next rung. [compat] (the
     * sharing-compatible version) and [film] (the legacy flag: film applied outside the graph) always take the 8-bit rungs. Audio (Sony LPCM)
     * is transcoded to AAC; AAC is copied; [mute] drops it. The result carries the chosen path ("codecSummary").
     *
     * [edit] != null: the clip follows the still's edit. Every decoded frame (half-float linear RGBA, no truncation) goes through the engine's node graph
     * ([DappleVideoEdit]: film exposure, tone analysis and solves held from the reference frame, grain per frame) and comes out as P010 for a Main10 rung
     * or I420 for an 8-bit rung; the encoder size is the edit's output size (crop / rotate applied, the container rotation is baked in). [edit] == null is
     * the pass-through path, unchanged.
     */
    fun clip(uri: String, dec: SwVideo.DecodeSel, source: Int, startMs: Long, durationMs: Long, dest: String, mute: Boolean, compat: Boolean, film: Boolean = false,
             edit: EditSpec? = null): Map<String, Any?> {
        val tStart = System.nanoTime()
        val s = SwVideo.open(ctx, uri, dec, source)
        var ed: EditRun? = null
        val destFile = File(dest)
        destFile.parentFile?.mkdirs()
        destFile.delete()
        val mgr = CodecManager.get(ctx)
        try {
            val startUs = startMs * 1000L
            val endUs = (startMs + durationMs) * 1000L
            val outFps = min(CLIP_MAX_FPS, max(1.0, s.info.fps)).roundToInt()
            // audio first (small): the muxer needs its format before it starts
            var audio: AudioClip? = null
            Log.i(TAG, "clip mute=$mute audioStream=${s.info.audioCodec} decodable=${s.info.audioDecodable}")
            if (!mute) {
                audio = audioCopy(uri, startUs, endUs)
                if (audio == null) audio = try { audioFromCore(s, startUs, endUs) } catch (e: Exception) { Log.w(TAG, "audio failed: $e"); null }
            }
            // output size is known after begin() (with an edit: after the reference frame went through the graph)
            if (edit != null) {
                ed = EditRun.start(s, edit, startUs, endUs)
                s.begin(startUs, endUs, CLIP_MAX_EDGE, CLIP_MAX_FPS, SwVideo.RGBAF16, false)
            } else {
                s.begin(startUs, endUs, CLIP_MAX_EDGE, CLIP_MAX_FPS, SwVideo.I420, false)
            }
            val job = EncodeJob(ed?.session?.outW ?: s.outW, ed?.session?.outH ?: s.outH, outFps, surfaceOk = ed != null,
                hdrSource = if (ed != null) ed.session.hdr else (s.info.colorKind == 2 || s.info.colorKind == 3), eightBitOnly = compat || (film && edit == null))
            val picks = mgr.encodeCandidates(job)
            var result: Map<String, Any?>? = null
            var chosen: EncodePick? = null
            val attempts = ArrayList<String>()
            for (pick in picks) {
                if (!mgr.verifyEncoder(pick)) { attempts.add("${pick.rung.label} ${pick.codec}: self-test failed"); continue }
                try {
                    result = encodeVideo(s, startUs, endUs, outFps, dest, audio, pick, ed)
                    chosen = pick
                    break
                } catch (e: EncoderUnreliable) {
                    Log.w(TAG, "${pick.rung.label} on ${pick.codec}: ${e.message}, next rung")
                    attempts.add("${pick.rung.label} ${pick.codec}: ${e.message}")
                    mgr.markEncoderFailed(pick, e.message ?: "unreliable")
                    destFile.delete()
                } catch (e: Exception) {
                    Log.w(TAG, "${pick.rung.label} on ${pick.codec} failed ($e), next rung")
                    attempts.add("${pick.rung.label} ${pick.codec}: $e")
                    destFile.delete()
                }
            }
            if (result == null) {
                // nothing on the ladder (the device declares no usable encoder, or all failed): the platform default H.264 encoder as the last resort
                val last = EncodePick(EncodeRung.AVC_HIGH, "", false, 0)
                try {
                    result = encodeVideo(s, startUs, endUs, outFps, dest, audio, last, ed)
                    chosen = last
                } catch (e: Exception) {
                    destFile.delete()
                    throw IllegalStateException("no encoder produced the clip: ${attempts.joinToString("; ")}; default H.264: $e")
                }
            }
            val ms = (System.nanoTime() - tStart) / 1_000_000
            val full = HashMap(result)
            val c = chosen!!
            full["audioHow"] = audio?.how ?: if (mute) "muted" else "none"
            full["audio"] = audio != null
            full["decoder"] = s.info.decoder
            full["hardwareDecode"] = s.info.hardware
            full["decoderNow"] = s.decoderNow()
            full["colorKind"] = s.info.colorKind
            full["slogDetected"] = s.info.slogDetected
            full["captureGamma"] = s.info.captureGamma
            full["compat"] = compat
            full["film"] = film
            full["edited"] = edit != null
            full["rung"] = c.rung.label
            full["encoderHardware"] = if (c.codec.isEmpty()) false else c.hardware
            full["demoted"] = attempts.joinToString("; ")
            full["main10Failure"] = attempts.firstOrNull { it.startsWith(EncodeRung.HEVC_MAIN10.label) }
            full["elapsedMs"] = ms
            full["actualStartMs"] = startMs
            full["path"] = dest
            val summary = "${c.rung.label} · ${full["encoder"]} (${if (full["encoderHardware"] == true) "hardware" else "software"})" +
                " | decode ${s.decoderNow()} (${if (s.info.hardware) "hardware" else "software"})" +
                (if (attempts.isEmpty()) "" else " | demoted: ${attempts.joinToString("; ")}")
            full["codecSummary"] = summary
            afterDecode(dec, s)
            mgr.noteJob("clip $summary, $ms ms")
            Log.i(TAG, "clip ${s.info} -> $full")
            return full
        } catch (e: Exception) {
            destFile.delete()
            throw e
        } finally {
            ed?.close()
            s.close()
        }
    }

    /**
     * The graph side of an edited clip: the native session (frozen on the reference frame) and the buffer of one decoded frame. [start] decodes the
     * reference frame (the cover frame of the Motion Photo when [EditSpec.refMs] says so, else the first frame of the window), freezes the session on it
     * and thereby sizes the encoder input.
     */
    class EditRun private constructor(val session: DappleVideoEdit.Session, val kind: Int, val rotation: Int) : AutoCloseable {
        lateinit var src: ByteBuffer      // one decoded RGBAF16 frame
        var frameNo = 0
        companion object {
            fun start(s: SwVideo.Session, spec: EditSpec, startUs: Long, endUs: Long): EditRun {
                val sess = DappleVideoEdit.open(spec)
                try {
                    val refUs = if (spec.refMs >= 0) (spec.refMs * 1000L).coerceIn(startUs, max(startUs, endUs)) else startUs
                    s.begin(refUs, refUs, CLIP_MAX_EDGE, 0.0, SwVideo.RGBAF16, false)
                    val buf = ByteBuffer.allocateDirect(s.bufferSize(SwVideo.RGBAF16)).order(ByteOrder.nativeOrder())
                    if (s.next(buf) < 0) throw IllegalStateException("no reference frame for the edit")
                    val run = EditRun(sess, s.info.colorKind, s.info.rotation)
                    sess.setReference(buf, s.outW, s.outH, run.kind, run.rotation)
                    run.src = buf
                    return run
                } catch (e: Exception) {
                    sess.close()
                    throw e
                }
            }
        }

        /** Frame index n >= 1 seeds the grain; the cover frame (the still) is index 0 and is not rendered here. */
        fun render(decodedW: Int, decodedH: Int, format: Int, dst: ByteBuffer) {
            frameNo++
            session.render(src, decodedW, decodedH, kind, rotation, frameNo, format, dst)
        }

        override fun close() = session.close()
    }

    /** The native 10-bit decoder was chosen but the core ended up in software (open or mid-pass): demote it for this OS build. */
    private fun afterDecode(dec: SwVideo.DecodeSel, s: SwVideo.Session) {
        if (dec.hwPref == 2 && dec.codec != null && !s.decoderNow().startsWith("mc:")) {
            CodecManager.get(ctx).markDecoderFailed(dec.codec, "fell back to software while decoding a real clip")
        }
    }

    private fun encodeVideo(s: SwVideo.Session, startUs: Long, endUs: Long, outFps: Int, dest: String, audio: AudioClip?, pick: EncodePick, ed: EditRun? = null): Map<String, Any?> {
        val tenBit = pick.rung.tenBit
        val format = if (tenBit) SwVideo.P010 else SwVideo.I420
        // an edited clip is decoded to half-float RGBA and packed by the graph session (BT.709 SDR: the film and the edit's tone are display referred)
        s.begin(startUs, endUs, CLIP_MAX_EDGE, CLIP_MAX_FPS, if (ed != null) SwVideo.RGBAF16 else format, false)
        val decW = s.outW
        val decH = s.outH
        val w = ed?.session?.outW ?: decW
        val h = ed?.session?.outH ?: decH
        // an edited clip carries what the session decided (section 30.8): BT.709 SDR, or HLG / PQ on BT.2020 for an HDR source edited without film
        val tags = if (ed != null) (if (tenBit) ed.session.tags else intArrayOf(1, 1, 1, 1)) else s.tags()
        val hdrOut = ed != null && tenBit && ed.session.hdr
        val hdrInfo = if (tenBit && (ed == null || hdrOut)) s.hdrStaticInfo() else null
        val mime = pick.rung.mime
        // input Surface (docs/engine/pipeline-architecture.md section 30.9): the edited frame is packed as RGB10A2 and drawn into the encoder's surface through EGL
        val surfaceMode = pick.input == InputMode.SURFACE && ed != null && tenBit
        val editFormat = if (surfaceMode) (if (hdrOut) EditFormat.RGB10A2_HDR else EditFormat.RGB10A2) else EditFormat.forEncoder(tenBit, hdrOut)
        var feeder: SurfaceFeeder? = null
        val bitrate = (w.toLong() * h * outFps * (if (tenBit) 0.085 else 0.1)).toLong().coerceIn(3_000_000L, 20_000_000L).toInt()
        val fmt = MediaFormat.createVideoFormat(mime, w, h).apply {
            setInteger(MediaFormat.KEY_COLOR_FORMAT, if (surfaceMode) ColorFmt.SURFACE else if (tenBit) COLOR_FormatYUVP010 else MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible)
            setInteger(MediaFormat.KEY_BIT_RATE, bitrate)
            setInteger(MediaFormat.KEY_FRAME_RATE, outFps)
            setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            setInteger(MediaFormat.KEY_COLOR_STANDARD, standardOf(tags[2]))
            setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
            setInteger(MediaFormat.KEY_COLOR_TRANSFER, transferOf(tags[1]))
            if (pick.profile != 0) setInteger(MediaFormat.KEY_PROFILE, pick.profile)
            if (hdrInfo != null) setByteBuffer(MediaFormat.KEY_HDR_STATIC_INFO, ByteBuffer.wrap(hdrInfo))
        }
        val enc = if (pick.codec.isNotEmpty()) MediaCodec.createByCodecName(pick.codec) else MediaCodec.createEncoderByType(mime)
        var muxer: MediaMuxer? = null
        var muxerStarted = false
        try {
            enc.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            val inSurface = if (surfaceMode) enc.createInputSurface() else null
            enc.start()
            if (inSurface != null) {
                feeder = SurfaceFeeder(inSurface, w, h, wantTenBit = true)
                if (feeder.bits != 10) throw EncoderUnreliable("input surface: no 10-bit EGL surface (${feeder.configDescription})")
            }
            File(dest).delete()
            muxer = MediaMuxer(dest, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
            muxer.setOrientationHint(if (ed != null) 0 else s.info.rotation)  // the graph session rotates edited frames upright

            val frame = ByteBuffer.allocateDirect(if (ed != null) ed.session.frameBytes(editFormat) else s.bufferSize(format)).order(ByteOrder.nativeOrder())
            val srcF16 = ed?.src ?: frame
            ed?.frameNo = 0
            val row = ByteArray(w * 2)
            val info = MediaCodec.BufferInfo()
            val wd = StallWatchdog()
            var vOut = -1
            var aOut = -1
            var sourceDone = false
            var eosQueued = false
            var outDone = false
            var pendingPts = -1L
            var frames = 0
            var lastRelUs = -1L
            val deadline = System.currentTimeMillis() + TIMEOUT_MS
            var outputFormat: MediaFormat? = null
            var outProfile = -1
            while (!outDone) {
                val nowMs = System.currentTimeMillis()
                if (nowMs > deadline) throw IllegalStateException("clip encode timed out")
                // an encoder that takes input and never answers (Pixel 5 hardware HEVC): give up on it, try the next rung
                if (wd.stalled(nowMs)) throw EncoderUnreliable("no output after ${wd.pending} queued frames")
                var drained = false
                while (true) {
                    val oi = enc.dequeueOutputBuffer(info, 0)
                    if (oi == MediaCodec.INFO_TRY_AGAIN_LATER) break
                    drained = true
                    if (oi == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        outputFormat = enc.outputFormat
                        outProfile = if (outputFormat.containsKey(MediaFormat.KEY_PROFILE)) outputFormat.getInteger(MediaFormat.KEY_PROFILE) else -1
                        // the encoder may accept a Main10 request and silently produce Main: never mux that as 10-bit
                        if (tenBit && !isMain10Profile(outProfile)) {
                            throw EncoderUnreliable(if (outProfile < 0) "output format has no profile" else "output profile ${CodecCatalog.profileName(mime, outProfile)} is not Main10")
                        }
                        vOut = muxer.addTrack(outputFormat)
                        if (audio != null) aOut = try { muxer.addTrack(audio.format) } catch (_: Exception) { -1 }
                        muxer.start()
                        muxerStarted = true
                    } else if (oi >= 0) {
                        val ob = enc.getOutputBuffer(oi)!!
                        if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) info.size = 0
                        if (info.size > 0 && muxerStarted) {
                            ob.position(info.offset); ob.limit(info.offset + info.size)
                            muxer.writeSampleData(vOut, ob, info)
                            frames++
                            wd.onOutput(System.currentTimeMillis())
                        }
                        val eos = info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
                        enc.releaseOutputBuffer(oi, false)
                        if (eos) { outDone = true; break }
                    }
                }
                if (outDone) break
                if (!eosQueued) {
                    if (pendingPts < 0 && !sourceDone) {
                        val t = s.next(srcF16)
                        if (t < 0) sourceDone = true else {
                            pendingPts = t
                            ed?.render(decW, decH, editFormat, frame)
                        }
                    }
                    if (feeder != null) {
                        if (pendingPts >= 0) {
                            var rel = max(0L, pendingPts - startUs)
                            if (rel <= lastRelUs) rel = lastRelUs + 1
                            lastRelUs = rel
                            feeder.draw(frame, rel * 1000L)
                            wd.onQueued()
                            pendingPts = -1
                        } else if (sourceDone) {
                            enc.signalEndOfInputStream()
                            eosQueued = true
                            wd.onEos()
                        }
                    }
                    val ii = if (feeder != null) -1 else enc.dequeueInputBuffer(if (drained) 0 else 5_000)
                    if (ii >= 0) {
                        if (pendingPts >= 0) {
                            val n = CodecInput.fill(enc, ii, frame, w, h, tenBit, row)
                            var rel = max(0L, pendingPts - startUs)
                            if (rel <= lastRelUs) rel = lastRelUs + 1
                            lastRelUs = rel
                            enc.queueInputBuffer(ii, 0, n, rel, 0)
                            wd.onQueued()
                            pendingPts = -1
                        } else if (sourceDone) {
                            enc.queueInputBuffer(ii, 0, 0, lastRelUs + 1, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            eosQueued = true
                            wd.onEos()
                        }
                    }
                } else {
                    Thread.sleep(2)
                }
            }
            if (!muxerStarted || frames == 0) throw IllegalStateException("clip produced no frames")
            if (aOut >= 0 && audio != null) {
                val ai = MediaCodec.BufferInfo()
                for (smp in audio.samples) {
                    ai.set(0, smp.data.size, smp.ptsUs, smp.flags)
                    muxer.writeSampleData(aOut, ByteBuffer.wrap(smp.data), ai)
                }
            }
            muxer.stop()
            val of = outputFormat
            return mapOf(
                "videoCodec" to (if (mime == MediaFormat.MIMETYPE_VIDEO_HEVC) "hevc" else "h264"),
                "videoProfile" to (if (outProfile >= 0) CodecCatalog.profileName(mime, outProfile) else if (tenBit) "Main10" else if (mime == MediaFormat.MIMETYPE_VIDEO_HEVC) "Main" else "High"),
                "bitDepth" to (if (tenBit) 10 else 8),
                "encoder" to enc.name,
                "inputMode" to (if (surfaceMode) "surface" else "buffer"),
                "colorStandard" to standardOf(tags[2]),
                "colorTransfer" to transferOf(tags[1]),
                "hdrStaticInfo" to (hdrInfo != null),
                "frames" to frames,
                "durationMs" to lastRelUs / 1000L,
                "width" to w, "height" to h,
                "encodedFormat" to (of?.toString() ?: ""),
            )
        } finally {
            try { feeder?.close() } catch (_: Exception) {}
            try { enc.stop() } catch (_: Exception) {}
            try { enc.release() } catch (_: Exception) {}
            try { muxer?.release() } catch (_: Exception) {}
        }
    }
}
