package com.dapple.dapple

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Test

class GpuProbeTest {
    @Test
    fun appVersionKeyChangesWithVersionNameAndCode() {
        assertEquals("1.2.3+45", GpuProbe.appVersionString("1.2.3", 45))
        assertNotEquals(GpuProbe.appVersionString("1.2.3", 45), GpuProbe.appVersionString("1.2.3", 46))
        assertNotEquals(GpuProbe.appVersionString("1.2.3", 45), GpuProbe.appVersionString("1.2.4", 45))
        assertEquals("?+1", GpuProbe.appVersionString(null, 1))
    }
}
