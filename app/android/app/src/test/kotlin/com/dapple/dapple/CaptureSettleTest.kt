package com.dapple.dapple

import org.junit.Assert.assertEquals
import org.junit.Test

class CaptureSettleTest {
    @Test
    fun shotRightAfterBindWaitsForTheRest() {
        assertEquals(700L, CaptureSettle.delayMs(1000, 1000))
        assertEquals(300L, CaptureSettle.delayMs(1400, 1000))
        assertEquals(1L, CaptureSettle.delayMs(1699, 1000))
    }

    @Test
    fun settledCameraShootsAtOnce() {
        assertEquals(0L, CaptureSettle.delayMs(1700, 1000))
        assertEquals(0L, CaptureSettle.delayMs(60_000, 1000))
    }

    @Test
    fun neverBoundOrClockGoingBackwardsIsSafe() {
        assertEquals(0L, CaptureSettle.delayMs(5, 0))
        assertEquals(700L, CaptureSettle.delayMs(900, 1000))  // age negative: at most one settle period
    }
}
