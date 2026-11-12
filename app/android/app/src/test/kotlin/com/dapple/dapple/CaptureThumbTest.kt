package com.dapple.dapple

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class CaptureThumbTest {
    @Test
    fun smallFramesAreNotScaledUp() {
        assertEquals(Pair(640, 480), CaptureThumb.scaledSize(640, 480))
        assertEquals(Pair(720, 540), CaptureThumb.scaledSize(720, 540))
    }

    @Test
    fun longEdgeIsReducedAndAspectKept() {
        assertEquals(Pair(720, 540), CaptureThumb.scaledSize(2880, 2160))
        assertEquals(Pair(540, 720), CaptureThumb.scaledSize(1080, 1440))
        val (w, h) = CaptureThumb.scaledSize(4032, 3024)
        assertEquals(720, w)
        assertEquals(540, h)
    }

    @Test
    fun noBitmapGivesNoThumbnail() {
        assertNull(CaptureThumb.save(null, java.io.File("unused.jpg")))
    }
}
