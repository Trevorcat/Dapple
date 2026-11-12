package com.dapple.dapple

import android.content.Context
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.util.Log
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * One-time capability self-tests (always off the UI thread, always with a watchdog: a hardware encoder may accept input and
 * never answer). A declared capability is trusted only after it has produced a real result on this device and OS build.
 *
 *  - encode (10-bit): 8 frames of a synthetic 10-bit gradient -> output must arrive within the watchdog, the encoder's
 *    output KEY_PROFILE must really be Main10, and the stream decoded again must show more than 256 distinct luma levels.
 *  - encode (8-bit): the same with 8-bit input, output must arrive and decode.
 *  - decode (10-bit hardware): a bundled 3-frame Main10 clip is decoded to COLOR_FormatYUVP010 with the hardware decoder;
 *    the output format must really be P010 and the picture must show more than 256 distinct luma levels.
 */
class CodecSelfTest(private val ctx: Context, private val caps: List<CodecCaps>, private val ledger: TestLedger) {
    companion object {
        private const val TAG = "DappleCodec"
        const val W = 640
        const val H = 352
        const val FRAMES = 8
        private const val FPS = 30
        const val ASSET = "selftest_main10.mp4"

        /** Packed test frame: horizontal luma ramp (limited range) moving by 6 px per frame, neutral chroma. */
        fun gradientFrame(frame: Int, tenBit: Boolean): ByteBuffer {
            val bytes = if (tenBit) W * H * 3 else W * H * 3 / 2
            val b = ByteBuffer.allocateDirect(bytes).order(ByteOrder.LITTLE_ENDIAN)
            for (y in 0 until H) for (x in 0 until W) {
                val xx = (x + frame * 6) % W
                if (tenBit) b.putShort((y * W + x) * 2, ((64 + 876 * xx / (W - 1)) shl 6).toShort())
                else b.put(y * W + x, (16 + 219 * xx / (W - 1)).toByte())
            }
            if (tenBit) {
                val uv = W * H * 2
                for (i in 0 until W * H / 2) b.putShort(uv + i * 2, (512 shl 6).toShort())
            } else {
                for (i in 0 until W * H / 2) b.put(W * H + i, 128.toByte())
            }
            b.rewind()
            return b
        }
    }

    class Outcome(val pass: Boolean, val detail: String, val ms: Long)

    private class Sample(val data: ByteArray, val ptsUs: Long, val flags: Int)

    private fun done(t0: Long, pass: Boolean, detail: String): Outcome {
        val ms = (System.nanoTime() - t0) / 1_000_000
        Log.i(TAG, "self-test ${if (pass) "PASS" else "FAIL"} ($ms ms): $detail")
        return Outcome(pass, detail, ms)
    }

    // ---- encode ------------------------------------------------------------------------------------------------------
    fun encode(c: CodecCaps, pick: EncodePick): Outcome {
        if (pick.input == InputMode.SURFACE) return encodeSurface(c, pick)
        val t0 = System.nanoTime()
        val ten = pick.rung.tenBit
        var enc: MediaCodec? = null
        try {
            val fmt = MediaFormat.createVideoFormat(c.mime, W, H).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, if (ten) ColorFmt.P010 else ColorFmt.FLEXIBLE)
                setInteger(MediaFormat.KEY_BIT_RATE, 4_000_000)
                setInteger(MediaFormat.KEY_FRAME_RATE, FPS)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
                setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
                setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
                setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
                if (pick.profile != 0) setInteger(MediaFormat.KEY_PROFILE, pick.profile)
            }
            enc = MediaCodec.createByCodecName(c.name)
            enc.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            enc.start()
            val wd = StallWatchdog()
            val samples = ArrayList<Sample>()
            var outFmt: MediaFormat? = null
            val info = MediaCodec.BufferInfo()
            val row = ByteArray(W * 2)
            var queued = 0
            var eos = false
            var finished = false
            val limit = System.currentTimeMillis() + 20_000
            while (!finished) {
                val now = System.currentTimeMillis()
                if (now > limit) return done(t0, false, "timed out")
                if (wd.stalled(now)) return done(t0, false, "no output: ${wd.pending} frames accepted, none returned within 3 s")
                while (true) {
                    val oi = enc.dequeueOutputBuffer(info, 0)
                    if (oi == MediaCodec.INFO_TRY_AGAIN_LATER) break
                    if (oi == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) { outFmt = enc.outputFormat; continue }
                    if (oi < 0) continue
                    val ob = enc.getOutputBuffer(oi)!!
                    if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0 && info.size > 0) {
                        val d = ByteArray(info.size)
                        ob.position(info.offset); ob.limit(info.offset + info.size); ob.get(d)
                        samples.add(Sample(d, info.presentationTimeUs, info.flags))
                        wd.onOutput(System.currentTimeMillis())
                    }
                    val last = info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
                    enc.releaseOutputBuffer(oi, false)
                    if (last) { finished = true; break }
                }
                if (finished) break
                if (!eos) {
                    val ii = enc.dequeueInputBuffer(10_000)
                    if (ii >= 0) {
                        if (queued < FRAMES) {
                            val n = CodecInput.fill(enc, ii, gradientFrame(queued, ten), W, H, ten, row)
                            enc.queueInputBuffer(ii, 0, n, queued * 1_000_000L / FPS, 0)
                            queued++
                            wd.onQueued()
                        } else {
                            enc.queueInputBuffer(ii, 0, 0, queued * 1_000_000L / FPS, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            eos = true
                            wd.onEos()
                        }
                    }
                } else {
                    Thread.sleep(5)
                }
            }
            if (samples.size < FRAMES / 2) return done(t0, false, "only ${samples.size} of $FRAMES frames came back")
            val of = outFmt ?: return done(t0, false, "no output format")
            val prof = if (of.containsKey(MediaFormat.KEY_PROFILE)) of.getInteger(MediaFormat.KEY_PROFILE) else -1
            if (ten && !isMain10Profile(prof)) {
                return done(t0, false, if (prof < 0) "output format carries no profile" else "output profile ${CodecCatalog.profileName(c.mime, prof)} is not Main10 (silently downgraded)")
            }
            // decode it back
            val dec = decodeBack(c.mime, of, samples, ten)
            if (!dec.first) return done(t0, false, dec.second)
            return done(t0, true, "${samples.size} frames, profile ${if (prof < 0) "n/a" else CodecCatalog.profileName(c.mime, prof)}, ${dec.second}")
        } catch (e: Throwable) {
            return done(t0, false, "exception: $e")
        } finally {
            try { enc?.stop() } catch (_: Exception) {}
            try { enc?.release() } catch (_: Exception) {}
        }
    }

    /** RGB10A2 test frame: a horizontal grey ramp over the full 10-bit range, moving by 6 px per frame (what the encoder's RGB to YUV conversion turns into the luma ramp). */
    private fun rampRgb10(frame: Int): ByteBuffer {
        val b = ByteBuffer.allocateDirect(W * H * 4).order(ByteOrder.nativeOrder())
        for (x in 0 until W) {
            val v = ((x + frame * 6) % W) * 1023 / (W - 1)
            val word = v or (v shl 10) or (v shl 20) or (3 shl 30)
            for (y in 0 until H) b.putInt((y * W + x) * 4, word)
        }
        b.rewind()
        return b
    }

    /**
     * Main10 encode through the encoder's input Surface (docs/engine/pipeline-architecture.md section 30.9): the same checks as the buffer test (output
     * arrives, the output profile really is Main10, the stream decodes back with more than 256 luma levels) plus: the EGL surface must really be 10-bit.
     */
    private fun encodeSurface(c: CodecCaps, pick: EncodePick): Outcome {
        val t0 = System.nanoTime()
        var enc: MediaCodec? = null
        var feeder: SurfaceFeeder? = null
        try {
            val fmt = MediaFormat.createVideoFormat(c.mime, W, H).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, ColorFmt.SURFACE)
                setInteger(MediaFormat.KEY_BIT_RATE, 4_000_000)
                setInteger(MediaFormat.KEY_FRAME_RATE, FPS)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
                setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
                setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
                setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
                if (pick.profile != 0) setInteger(MediaFormat.KEY_PROFILE, pick.profile)
            }
            enc = MediaCodec.createByCodecName(c.name)
            enc.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            val surface = enc.createInputSurface()
            enc.start()
            feeder = SurfaceFeeder(surface, W, H, wantTenBit = pick.rung.tenBit)
            if (pick.rung.tenBit && feeder.bits != 10) return done(t0, false, "surface input: the device gave no 10-bit EGL surface (${feeder.configDescription})")
            val wd = StallWatchdog()
            val samples = ArrayList<Sample>()
            var outFmt: MediaFormat? = null
            val info = MediaCodec.BufferInfo()
            var queued = 0
            var finished = false
            var eosSent = false
            val limit = System.currentTimeMillis() + 20_000
            while (!finished) {
                val now = System.currentTimeMillis()
                if (now > limit) return done(t0, false, "timed out")
                if (wd.stalled(now)) return done(t0, false, "no output: ${wd.pending} frames accepted, none returned within 3 s")
                if (queued < FRAMES) {
                    feeder.draw(rampRgb10(queued), queued * 1_000_000_000L / FPS)
                    queued++
                    wd.onQueued()
                } else if (!eosSent) {
                    enc.signalEndOfInputStream()
                    eosSent = true
                    wd.onEos()
                }
                while (true) {
                    val oi = enc.dequeueOutputBuffer(info, if (eosSent) 10_000 else 0)
                    if (oi == MediaCodec.INFO_TRY_AGAIN_LATER) break
                    if (oi == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) { outFmt = enc.outputFormat; continue }
                    if (oi < 0) continue
                    val ob = enc.getOutputBuffer(oi)!!
                    if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG == 0 && info.size > 0) {
                        val d = ByteArray(info.size)
                        ob.position(info.offset); ob.limit(info.offset + info.size); ob.get(d)
                        samples.add(Sample(d, info.presentationTimeUs, info.flags))
                        wd.onOutput(System.currentTimeMillis())
                    }
                    val last = info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
                    enc.releaseOutputBuffer(oi, false)
                    if (last) { finished = true; break }
                }
            }
            if (samples.size < FRAMES / 2) return done(t0, false, "only ${samples.size} of $FRAMES frames came back")
            val of = outFmt ?: return done(t0, false, "no output format")
            val prof = if (of.containsKey(MediaFormat.KEY_PROFILE)) of.getInteger(MediaFormat.KEY_PROFILE) else -1
            if (pick.rung.tenBit && !isMain10Profile(prof)) {
                return done(t0, false, if (prof < 0) "output format carries no profile" else "output profile ${CodecCatalog.profileName(c.mime, prof)} is not Main10 (silently downgraded)")
            }
            val dec = decodeBack(c.mime, of, samples, pick.rung.tenBit)
            if (!dec.first) return done(t0, false, dec.second)
            return done(t0, true, "surface input, ${feeder.bits}-bit EGL, ${samples.size} frames, profile ${if (prof < 0) "n/a" else CodecCatalog.profileName(c.mime, prof)}, ${dec.second}")
        } catch (e: Throwable) {
            return done(t0, false, "surface input: exception $e")
        } finally {
            try { feeder?.close() } catch (_: Exception) {}
            try { enc?.stop() } catch (_: Exception) {}
            try { enc?.release() } catch (_: Exception) {}
        }
    }

    /** Decodes [samples] and checks the distinct luma levels. Returns (ok, detail). */
    private fun decodeBack(mime: String, of: MediaFormat, samples: List<Sample>, ten: Boolean): Pair<Boolean, String> {
        val candidates = if (ten)
            caps.filter { !it.encoder && it.mime == mime && it.declaresProfile(Prof.HEVC_MAIN10) && ledger.state(SelfTestKind.DEC10, it.name) != TestState.FAIL }
                .sortedBy { (if (it.hardware) 0 else 2) + (if (it.hasP010) 0 else 1) }
        else
            caps.filter { !it.encoder && it.mime == mime }.sortedBy { if (it.hardware) 0 else 1 }
        if (candidates.isEmpty()) return false to "no ${if (ten) "10-bit " else ""}decoder to verify the stream"
        var last = "no decoder produced a picture"
        for (d in candidates) {
            val r = decodeWith(d.name, mime, of, samples, ten)
            if (r.first) return r
            last = r.second
        }
        return false to last
    }

    private fun decodeWith(name: String, mime: String, of: MediaFormat, samples: List<Sample>, ten: Boolean): Pair<Boolean, String> {
        var dec: MediaCodec? = null
        try {
            val fmt = MediaFormat.createVideoFormat(mime, W, H)
            for (k in listOf("csd-0", "csd-1")) if (of.containsKey(k)) fmt.setByteBuffer(k, of.getByteBuffer(k)!!.duplicate())
            if (ten) fmt.setInteger(MediaFormat.KEY_COLOR_FORMAT, ColorFmt.P010)
            dec = MediaCodec.createByCodecName(name)
            dec.configure(fmt, null, null, 0)
            dec.start()
            val levels = HashSet<Int>()
            val info = MediaCodec.BufferInfo()
            var next = 0
            var eos = false
            var frames = 0
            var outColor = -1
            val start = System.currentTimeMillis()
            var lastProgress = start
            while (frames < 1 || (!eos)) {
                val now = System.currentTimeMillis()
                if (now - lastProgress > 3000 || now - start > 10_000) break
                if (!eos) {
                    val ii = dec.dequeueInputBuffer(10_000)
                    if (ii >= 0) {
                        if (next < samples.size) {
                            val s = samples[next++]
                            val ib = dec.getInputBuffer(ii)!!
                            ib.clear(); ib.put(s.data)
                            dec.queueInputBuffer(ii, 0, s.data.size, s.ptsUs, 0)
                            lastProgress = now
                        } else {
                            dec.queueInputBuffer(ii, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            eos = true
                        }
                    }
                }
                val oi = dec.dequeueOutputBuffer(info, 10_000)
                if (oi == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    outColor = dec.outputFormat.getInteger(MediaFormat.KEY_COLOR_FORMAT)
                } else if (oi >= 0) {
                    if (frames == 0 && info.size > 0) {
                        if (outColor < 0) outColor = dec.outputFormat.getInteger(MediaFormat.KEY_COLOR_FORMAT)
                        if (!ten || outColor == ColorFmt.P010) collectLevels(dec, oi, ten, levels)
                        frames++
                    }
                    lastProgress = now
                    val last = info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0
                    dec.releaseOutputBuffer(oi, false)
                    if (last) break
                    if (frames > 0) break
                }
            }
            if (frames == 0) return false to "$name: no decoded frame"
            if (ten && outColor != ColorFmt.P010) return false to "$name: output colour format $outColor, not P010"
            val min = if (ten) 256 else 64
            if (levels.size <= min) return false to "$name: only ${levels.size} distinct luma levels"
            return true to "decoded by $name, ${levels.size} distinct luma levels"
        } catch (e: Throwable) {
            return false to "$name: $e"
        } finally {
            try { dec?.stop() } catch (_: Exception) {}
            try { dec?.release() } catch (_: Exception) {}
        }
    }

    private fun collectLevels(dec: MediaCodec, index: Int, ten: Boolean, levels: HashSet<Int>) {
        val of = dec.outputFormat
        if (ten) {
            val ob = dec.getOutputBuffer(index)!!.order(ByteOrder.LITTLE_ENDIAN)
            val stride = if (of.containsKey("stride")) of.getInteger("stride") else of.getInteger(MediaFormat.KEY_WIDTH) * 2
            val w = of.getInteger(MediaFormat.KEY_WIDTH)
            val h = of.getInteger(MediaFormat.KEY_HEIGHT)
            for (y in intArrayOf(2, h / 2, h - 3)) for (x in 0 until w) levels.add((ob.getShort(y * stride + x * 2).toInt() and 0xffff) shr 6)
        } else {
            val img = dec.getOutputImage(index) ?: return
            val p = img.planes[0]
            val h = img.height
            for (y in intArrayOf(2, h / 2, h - 3)) for (x in 0 until img.width) levels.add(p.buffer.get(y * p.rowStride + x * p.pixelStride).toInt() and 0xff)
            img.close()
        }
    }

    // ---- hardware 10-bit decode ----------------------------------------------------------------------------------------
    fun decode10(c: CodecCaps): Outcome {
        val t0 = System.nanoTime()
        var dec: MediaCodec? = null
        val ex = MediaExtractor()
        try {
            ctx.assets.openFd(ASSET).use { ex.setDataSource(it.fileDescriptor, it.startOffset, it.length) }
            var tf: MediaFormat? = null
            for (i in 0 until ex.trackCount) {
                val f = ex.getTrackFormat(i)
                if ((f.getString(MediaFormat.KEY_MIME) ?: "").startsWith("video/")) { ex.selectTrack(i); tf = f; break }
            }
            if (tf == null) return done(t0, false, "test clip has no video")
            tf.setInteger(MediaFormat.KEY_COLOR_FORMAT, ColorFmt.P010)
            dec = MediaCodec.createByCodecName(c.name)
            dec.configure(tf, null, null, 0)
            dec.start()
            val levels = HashSet<Int>()
            val info = MediaCodec.BufferInfo()
            var inEos = false
            var got = 0
            var outColor = -1
            val start = System.currentTimeMillis()
            while (got < 1) {
                if (System.currentTimeMillis() - start > 3000) return done(t0, false, "${c.name}: no decoded frame within 3 s")
                if (!inEos) {
                    val ii = dec.dequeueInputBuffer(10_000)
                    if (ii >= 0) {
                        val ib = dec.getInputBuffer(ii)!!
                        val n = ex.readSampleData(ib, 0)
                        if (n < 0) { dec.queueInputBuffer(ii, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM); inEos = true }
                        else { dec.queueInputBuffer(ii, 0, n, ex.sampleTime, 0); ex.advance() }
                    }
                }
                val oi = dec.dequeueOutputBuffer(info, 10_000)
                if (oi == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    outColor = dec.outputFormat.getInteger(MediaFormat.KEY_COLOR_FORMAT)
                } else if (oi >= 0) {
                    if (info.size > 0) {
                        if (outColor < 0) outColor = dec.outputFormat.getInteger(MediaFormat.KEY_COLOR_FORMAT)
                        if (outColor == ColorFmt.P010) collectLevels(dec, oi, true, levels)
                        got++
                    }
                    dec.releaseOutputBuffer(oi, false)
                    if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
                }
            }
            if (got == 0) return done(t0, false, "${c.name}: no frame")
            val v = Decode10.verdict(c.name, outColor, levels.size, c.colorFormats)
            return done(t0, v.first, v.second)
        } catch (e: Throwable) {
            return done(t0, false, "${c.name}: exception $e")
        } finally {
            try { dec?.stop() } catch (_: Exception) {}
            try { dec?.release() } catch (_: Exception) {}
            ex.release()
        }
    }
}
