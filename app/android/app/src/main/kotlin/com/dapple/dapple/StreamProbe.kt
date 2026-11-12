package com.dapple.dapple

/** What an H.264 SPS says about the pixel format. */
data class AvcFormat(val profileIdc: Int, val chromaFormatIdc: Int, val bitDepthLuma: Int, val bitDepthChroma: Int) {
    /** 8-bit 4:2:0: what every platform decoder handles. */
    val isPlain get() = chromaFormatIdc == 1 && bitDepthLuma == 8 && bitDepthChroma == 8

    /** MediaCodecInfo.CodecProfileLevel.AVCProfile* a decoder must list to decode this stream (0 = any). */
    val requiredProfile: Int
        get() = when {
            profileIdc == 244 || chromaFormatIdc == 3 -> 0x40 // AVCProfileHigh444
            chromaFormatIdc == 2 -> 0x20 // AVCProfileHigh422
            bitDepthLuma > 8 || bitDepthChroma > 8 -> 0x10 // AVCProfileHigh10
            else -> 0
        }

    val label: String
        get() {
            val c = when (chromaFormatIdc) { 0 -> "4:0:0"; 1 -> "4:2:0"; 2 -> "4:2:2"; else -> "4:4:4" }
            return "H.264 profile $profileIdc, chroma $c, $bitDepthLuma-bit"
        }
}

/** Pure parsing helpers (no Android classes) so they can be unit tested on the JVM. */
object StreamProbe {
    private class Bits(private val b: ByteArray) {
        var pos = 0
        fun bit(): Int {
            val v = (b[pos shr 3].toInt() shr (7 - (pos and 7))) and 1
            pos++
            return v
        }
        fun bits(n: Int): Int { var v = 0; repeat(n) { v = (v shl 1) or bit() }; return v }
        fun ue(): Int {
            var z = 0
            while (bit() == 0 && z < 32) z++
            return if (z == 0) 0 else ((1 shl z) - 1 + bits(z))
        }
    }

    /** Strips emulation-prevention bytes (00 00 03 -> 00 00). */
    private fun unescape(src: ByteArray, from: Int): ByteArray {
        val out = java.io.ByteArrayOutputStream(src.size)
        var zeros = 0
        for (i in from until src.size) {
            val v = src[i].toInt() and 0xff
            if (zeros >= 2 && v == 3) { zeros = 0; continue }
            out.write(v)
            zeros = if (v == 0) zeros + 1 else 0
        }
        return out.toByteArray()
    }

    /**
     * Parses the first SPS in [csd] (Annex-B csd-0 as MediaExtractor delivers it: start code + NAL, or a bare NAL).
     * Returns null when none is found or it is truncated.
     */
    fun parseAvcSps(csd: ByteArray): AvcFormat? {
        var nal = -1
        var i = 0
        while (i + 3 < csd.size) {
            if (csd[i].toInt() == 0 && csd[i + 1].toInt() == 0 && csd[i + 2].toInt() == 1) {
                if ((csd[i + 3].toInt() and 0x1f) == 7) { nal = i + 3; break }
                i += 3
            } else i++
        }
        if (nal < 0 && csd.isNotEmpty() && (csd[0].toInt() and 0x1f) == 7) nal = 0
        if (nal < 0) return null
        return try {
            val r = Bits(unescape(csd, nal + 1))
            val profile = r.bits(8)
            r.bits(8); r.bits(8)
            r.ue()
            var chroma = 1
            var dl = 8
            var dc = 8
            if (profile in intArrayOf(100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135)) {
                chroma = r.ue()
                if (chroma == 3) r.bit()
                dl = 8 + r.ue()
                dc = 8 + r.ue()
            }
            AvcFormat(profile, chroma, dl, dc)
        } catch (_: Exception) { null }
    }
}

/** What an HEVC SPS says about the pixel format. */
data class HevcFormat(val profileIdc: Int, val chromaFormatIdc: Int, val bitDepthLuma: Int, val bitDepthChroma: Int) {
    val isPlain get() = chromaFormatIdc == 1 && bitDepthLuma == 8 && bitDepthChroma == 8

    /**
     * MediaCodecInfo.CodecProfileLevel.HEVCProfile* a decoder must list: 0 = any, 2 = Main10 (or its HDR10 variants),
     * -1 = Android has no profile constant for it (4:2:2, 4:4:4, more than 10 bits): treat as not hardware decodable.
     */
    val requiredProfile: Int
        get() = when {
            chromaFormatIdc != 1 || bitDepthLuma > 10 || bitDepthChroma > 10 -> -1
            bitDepthLuma > 8 || bitDepthChroma > 8 -> 2
            else -> 0
        }

    val label: String
        get() {
            val c = when (chromaFormatIdc) { 0 -> "4:0:0"; 1 -> "4:2:0"; 2 -> "4:2:2"; else -> "4:4:4" }
            return "HEVC profile $profileIdc, chroma $c, $bitDepthLuma-bit"
        }
}

object HevcProbe {
    private class Bits(private val b: ByteArray) {
        var pos = 0
        fun bit(): Int {
            val v = (b[pos shr 3].toInt() shr (7 - (pos and 7))) and 1
            pos++
            return v
        }
        fun bits(n: Int): Int { var v = 0; repeat(n) { v = (v shl 1) or bit() }; return v }
        fun skip(n: Int) { pos += n; if ((pos shr 3) >= b.size) throw IndexOutOfBoundsException() }
        fun ue(): Int {
            var z = 0
            while (bit() == 0 && z < 32) z++
            return if (z == 0) 0 else ((1 shl z) - 1 + bits(z))
        }
    }

    private fun unescape(src: ByteArray, from: Int): ByteArray {
        val out = java.io.ByteArrayOutputStream(src.size)
        var zeros = 0
        for (i in from until src.size) {
            val v = src[i].toInt() and 0xff
            if (zeros >= 2 && v == 3) { zeros = 0; continue }
            out.write(v)
            zeros = if (v == 0) zeros + 1 else 0
        }
        return out.toByteArray()
    }

    /** Parses the first SPS (NAL type 33) in [csd] (Annex-B csd-0: VPS, SPS, PPS with start codes, or a bare SPS). */
    fun parseSps(csd: ByteArray): HevcFormat? {
        var nal = -1
        var i = 0
        while (i + 4 < csd.size) {
            if (csd[i].toInt() == 0 && csd[i + 1].toInt() == 0 && csd[i + 2].toInt() == 1) {
                if (((csd[i + 3].toInt() shr 1) and 0x3f) == 33) { nal = i + 3; break }
                i += 3
            } else i++
        }
        if (nal < 0 && csd.size > 2 && ((csd[0].toInt() shr 1) and 0x3f) == 33) nal = 0
        if (nal < 0) return null
        return try {
            val r = Bits(unescape(csd, nal + 2)) // skip the 2-byte NAL header
            r.skip(4) // sps_video_parameter_set_id
            val maxSub = r.bits(3)
            r.skip(1) // temporal_id_nesting
            // profile_tier_level(1, maxSub)
            r.skip(2 + 1) // general_profile_space, general_tier_flag
            val profile = r.bits(5)
            r.skip(32 + 4 + 43 + 1) // compatibility flags, progressive/interlaced/non-packed/frame-only, 43 reserved, 1
            r.skip(8) // general_level_idc
            val profPresent = BooleanArray(8)
            val levelPresent = BooleanArray(8)
            for (k in 0 until maxSub) { profPresent[k] = r.bit() == 1; levelPresent[k] = r.bit() == 1 }
            if (maxSub > 0) for (k in maxSub until 8) r.skip(2)
            for (k in 0 until maxSub) {
                if (profPresent[k]) r.skip(88)
                if (levelPresent[k]) r.skip(8)
            }
            r.ue() // sps_seq_parameter_set_id
            val chroma = r.ue()
            if (chroma == 3) r.skip(1)
            r.ue(); r.ue() // width, height
            if (r.bit() == 1) { r.ue(); r.ue(); r.ue(); r.ue() } // conformance window
            val dl = 8 + r.ue()
            val dc = 8 + r.ue()
            HevcFormat(profile, chroma, dl, dc)
        } catch (_: Exception) { null }
    }
}
