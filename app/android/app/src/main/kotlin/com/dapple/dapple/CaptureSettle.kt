package com.dapple.dapple

/**
 * Auto exposure restarts every time a camera is bound (start of the screen, lens switch, mode fallback). The first frames after the bind are
 * exposed with the HAL's initial guess; a RAW captured then can be several stops off the converged exposure (the DNG is the raw sensor data and
 * keeps whatever exposure the shot had, no later ISP stage rescues it). The shutter therefore waits until [SETTLE_MS] have passed since the bind.
 * CameraX's own precapture sequence (flash / AF) is unchanged. Not verified on a phone: the lead's device check lists the measurement.
 */
object CaptureSettle {
    const val SETTLE_MS = 700L

    /** Milliseconds the shot has to wait: 0 when the camera has been bound for at least [SETTLE_MS] (or was never bound: [boundAtMs] <= 0). */
    fun delayMs(nowMs: Long, boundAtMs: Long): Long {
        if (boundAtMs <= 0L) return 0L
        val age = nowMs - boundAtMs
        return if (age >= SETTLE_MS) 0L else (SETTLE_MS - maxOf(age, 0L))
    }
}
