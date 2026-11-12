package com.dapple.dapple

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ClipEditTest {
    @Test
    fun noEditMapMeansPassThrough() {
        assertNull(EditSpec.fromMap(null))
        assertNull(EditSpec.fromMap(mapOf("refMs" to 100)))   // no params: not an edit
    }

    @Test
    fun specComesFromTheMethodChannelMap() {
        val s = EditSpec.fromMap(
            mapOf(
                "paramsJson" to "{\"film_stock\":\"kodak_portra_400\"}", "lutPath" to "", "refMs" to 1500L, "stillAspect" to 1.3333, "stillLongEdge" to 6000,
                "sceneX" to 0.3457, "sceneY" to 0.3585, "cameraMake" to "SONY", "cameraModel" to "ILCE-7RM5",
            )
        )
        assertNotNull(s)
        s!!
        assertEquals(1500L, s.refMs)
        assertNull(s.lutPath)                      // an empty path is no LUT
        assertEquals(6000, s.stillLongEdge)
        assertEquals("{\"film_stock\":\"kodak_portra_400\"}", s.paramsJson)
    }

    @Test
    fun optionsJsonIsTheFlatObjectTheNativeSessionReads() {
        val j = EditSpec("{}", cameraMake = "A\"B", cameraModel = "C\\D", stillAspect = 1.5, stillLongEdge = 4000, sceneX = 0.31, sceneY = 0.33).optionsJson()
        assertEquals(
            "{\"cameraMake\":\"A\\\"B\",\"cameraModel\":\"C\\\\D\",\"sceneX\":0.31,\"sceneY\":0.33,\"stillAspect\":1.5,\"stillLongEdge\":4000}", j
        )
        assertTrue(EditSpec("{}", stillAspect = Double.NaN).optionsJson().contains("\"stillAspect\":0"))
    }

    @Test
    fun encoderInputFollowsTheRungDepth() {
        assertEquals(EditFormat.P010, EditFormat.forEncoder(true))
        assertEquals(EditFormat.I420, EditFormat.forEncoder(false))
    }
}
