package com.dapple.dapple

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class CaptureStrategyTest {
    private class MemStore : StrategyStore {
        val m = HashMap<String, String>()
        override fun get(key: String) = m[key]
        override fun put(key: String, value: String) { m[key] = value }
    }

    private fun strat(s: MemStore, key: String = "0|fp", rawJpeg: Boolean = true, raw: Boolean = true) =
        CaptureStrategy(s, key, rawJpeg, raw)

    @Test fun startsWithTheBestModeTheLensOffers() {
        assertEquals(CaptureMode.RAW_JPEG, strat(MemStore()).mode)
        assertEquals(CaptureMode.RAW, strat(MemStore(), rawJpeg = false).mode)
        assertEquals(CaptureMode.JPEG, strat(MemStore(), rawJpeg = false, raw = false).mode)
        assertFalse(strat(MemStore()).verified)
    }

    @Test fun rawJpegStallGoesToRawOnlyNotJpeg() {
        val st = MemStore()
        val s = strat(st)
        assertEquals(CaptureMode.RAW, s.failed())
        assertEquals("RAW · 待确认", s.badge())
        assertEquals("RAW", st.m["0|fp"])
    }

    @Test fun rawOnlyFailureFallsBackToJpegAndStaysThere() {
        val s = strat(MemStore())
        s.failed()
        assertEquals(CaptureMode.JPEG, s.failed())
        assertEquals(CaptureMode.JPEG, s.failed())
        assertEquals("JPEG", s.badge())
    }

    @Test fun lensWithoutRawJpegSkipsToRaw() {
        val s = strat(MemStore(), rawJpeg = false)
        assertEquals(CaptureMode.RAW, s.mode)
        assertEquals(CaptureMode.JPEG, s.failed())
    }

    @Test fun choiceIsRememberedPerCameraAndFingerprint() {
        val st = MemStore()
        val a = strat(st, "0|fp")
        a.failed()
        a.markVerified()
        val again = strat(st, "0|fp")
        assertEquals(CaptureMode.RAW, again.mode)
        assertTrue(again.verified)
        assertEquals("RAW", again.badge())
        // another camera or another build starts over
        assertEquals(CaptureMode.RAW_JPEG, strat(st, "1|fp").mode)
        assertEquals(CaptureMode.RAW_JPEG, strat(st, "0|fp2").mode)
    }

    @Test fun verifiedModeIsOnlyDemotedForTheProcessNotForGood() {
        val st = MemStore()
        val s = strat(st)
        s.markVerified()
        assertEquals(CaptureMode.RAW, s.failed())
        assertEquals("RAW_JPEG+", st.m["0|fp"])
        assertEquals(CaptureMode.RAW_JPEG, strat(st).mode)
    }

    @Test fun rememberedModeTheLensNoLongerOffersDrops() {
        val st = MemStore()
        st.m["0|fp"] = "RAW_JPEG+"
        assertEquals(CaptureMode.RAW, strat(st, rawJpeg = false).mode)
        assertEquals(CaptureMode.JPEG, strat(st, rawJpeg = false, raw = false).mode)
    }
}
