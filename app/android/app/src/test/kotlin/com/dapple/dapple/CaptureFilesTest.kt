package com.dapple.dapple

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class CaptureFilesTest {
    @get:Rule val tmp = TemporaryFolder()

    private fun file(vararg head: Int, size: Int, tail: IntArray = intArrayOf()): java.io.File {
        val b = ByteArray(size)
        head.forEachIndexed { i, v -> b[i] = v.toByte() }
        tail.forEachIndexed { i, v -> b[size - tail.size + i] = v.toByte() }
        return tmp.newFile().apply { writeBytes(b) }
    }

    @Test fun completeJpegIsOk() {
        assertTrue(CaptureFiles.jpgOk(file(0xFF, 0xD8, size = 4096, tail = intArrayOf(0xFF, 0xD9))))
    }

    @Test fun truncatedOrEmptyJpegIsRejected() {
        assertFalse(CaptureFiles.jpgOk(file(0xFF, 0xD8, size = 4096)))
        assertFalse(CaptureFiles.jpgOk(file(size = 0)))
        assertFalse(CaptureFiles.jpgOk(file(0xFF, 0xD8, size = 100, tail = intArrayOf(0xFF, 0xD9))))
    }

    @Test fun dngIsATiffOfPlausibleSize() {
        val i = 'I'.code
        val m = 'M'.code
        assertTrue(CaptureFiles.isDng(file(i, i, 42, 0, size = 200_000)))
        assertTrue(CaptureFiles.isDng(file(m, m, 0, 42, size = 200_000)))
        assertFalse(CaptureFiles.isDng(file(i, i, 42, 0, size = 10)))
        assertFalse(CaptureFiles.isDng(file(0xFF, 0xD8, size = 200_000)))
    }

    @Test fun badgeNamesWhatWillBeSaved() {
        assertEquals("RAW + JPEG", CaptureFiles.badge(CaptureMode.RAW_JPEG, true))
        assertEquals("RAW + JPEG · 待确认", CaptureFiles.badge(CaptureMode.RAW_JPEG, false))
        assertEquals("RAW", CaptureFiles.badge(CaptureMode.RAW, true))
        assertEquals("RAW · 待确认", CaptureFiles.badge(CaptureMode.RAW, false))
        assertEquals("JPEG", CaptureFiles.badge(CaptureMode.JPEG, true))
        assertEquals("JPEG", CaptureFiles.badge(CaptureMode.JPEG, false))
    }

    @Test fun legacyGalleryTargetIsPicturesDappleAndNeverOverwrites() {
        val pictures = tmp.newFolder("Pictures")
        val a = CaptureFiles.legacyTarget(pictures, "DAP_1.jpg")
        assertEquals(java.io.File(java.io.File(pictures, "Dapple"), "DAP_1.jpg"), a)
        a.writeText("x")
        assertEquals("DAP_1_1.jpg", CaptureFiles.legacyTarget(pictures, "DAP_1.jpg").name)
        assertEquals("DAP_1.dng", CaptureFiles.legacyTarget(pictures, "DAP_1.dng").name)
    }
}
