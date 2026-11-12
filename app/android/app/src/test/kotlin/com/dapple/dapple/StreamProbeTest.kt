package com.dapple.dapple

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * SPS parsing used to decide between the hardware decoder and the FFmpeg fallback. The byte strings are the real
 * Annex-B SPS NAL units of the fixtures in tests/data/video (extracted with `-bsf:v h264_mp4toannexb` /
 * `hevc_mp4toannexb`), i.e. what MediaExtractor hands over as csd-0.
 */
class StreamProbeTest {
    private fun hex(s: String): ByteArray = ByteArray(s.length / 2) { s.substring(it * 2, it * 2 + 2).toInt(16).toByte() }

    // x264 High 4:2:2 Intra, yuv422p10le (what a Sony A7R V XAVC S-I clip looks like): profile_idc 122
    private val avc422 = hex("00000001677a100ab6cb88f602d40808050000030001000003003204")

    // x264 High, yuv420p (positive control: a plain stream every phone decodes in hardware)
    private val avc420 = hex("000000016764000aacd9447b016a040402800000030080000019078912cb")

    // x265 yuv422p10le (RExt), and x265 yuv420p10le (HLG fixture)
    private val hevc422 = hex("000000014201010408000003009d2800000300001eb020831365bab932bc05a81010082000000300200000030321")
    private val hevc420x10 = hex("000000014201010408000003009da800000300001ea020831365bab932bc05a81010482000000300200000030321")

    @Test fun avc422TenBitIsNotPlainAndNeedsHigh422() {
        val f = StreamProbe.parseAvcSps(avc422)
        assertNotNull(f)
        assertEquals(122, f!!.profileIdc)
        assertEquals(2, f.chromaFormatIdc)
        assertEquals(10, f.bitDepthLuma)
        assertEquals(10, f.bitDepthChroma)
        assertFalse(f.isPlain)
        assertEquals(0x20, f.requiredProfile) // AVCProfileHigh422 (0x20; 0x10 is High10)
        assertTrue(f.label.contains("4:2:2") && f.label.contains("10-bit"))
    }

    @Test fun avc420EightBitIsPlainAndNeedsNothing() {
        val f = StreamProbe.parseAvcSps(avc420)
        assertNotNull(f)
        assertEquals(100, f!!.profileIdc)
        assertEquals(1, f.chromaFormatIdc)
        assertEquals(8, f.bitDepthLuma)
        assertTrue(f.isPlain)
        assertEquals(0, f.requiredProfile)
    }

    @Test fun avcBareSpsWithoutStartCodeAndEmulationPrevention() {
        // same SPS, no start code (some devices deliver csd without it)
        val bare = avc422.copyOfRange(4, avc422.size)
        assertEquals(122, StreamProbe.parseAvcSps(bare)!!.profileIdc)
    }

    @Test fun avcWithoutSpsOrTruncatedGivesNull() {
        assertNull(StreamProbe.parseAvcSps(ByteArray(0)))
        assertNull(StreamProbe.parseAvcSps(hex("0000000168ce3c80"))) // PPS only
        assertNull(StreamProbe.parseAvcSps(hex("00000001677a10"))) // truncated SPS
    }

    @Test fun avcHigh10Needs0x10() {
        // profile 110 (High 10), 4:2:0, 10 bit: sps up to chroma_format_idc=1, bit depths = 8+2
        val f = AvcFormat(110, 1, 10, 10)
        assertEquals(0x10, f.requiredProfile)
        assertEquals(0x40, AvcFormat(244, 3, 8, 8).requiredProfile)
    }

    @Test fun hevc422TenBit() {
        val f = HevcProbe.parseSps(hevc422)
        assertNotNull(f)
        assertEquals(4, f!!.profileIdc) // RExt
        assertEquals(2, f.chromaFormatIdc)
        assertEquals(10, f.bitDepthLuma)
        assertFalse(f.isPlain)
        assertEquals(-1, f.requiredProfile) // Android has no 4:2:2 HEVC profile constant: software path
        assertTrue(f.label.contains("4:2:2"))
    }

    @Test fun hevc420TenBitNeedsMain10() {
        val f = HevcProbe.parseSps(hevc420x10)
        assertNotNull(f)
        assertEquals(1, f!!.chromaFormatIdc)
        assertEquals(10, f.bitDepthLuma)
        assertEquals(2, f.requiredProfile)
    }

    @Test fun hevcEightBitIsPlain() {
        assertTrue(HevcFormat(1, 1, 8, 8).isPlain)
        assertEquals(0, HevcFormat(1, 1, 8, 8).requiredProfile)
        assertEquals(-1, HevcFormat(4, 3, 12, 12).requiredProfile)
    }

    @Test fun hevcWithoutSpsOrTruncatedGivesNull() {
        assertNull(HevcProbe.parseSps(ByteArray(0)))
        assertNull(HevcProbe.parseSps(hex("000000014001")))
        assertNull(HevcProbe.parseSps(hex("0000000142010104")))
    }
}
