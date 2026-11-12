package com.dapple.dapple

import java.io.File
import java.io.RandomAccessFile

/** Pure helpers of the in-app capture: file validation and the honest badge text (unit-tested on the JVM). */
object CaptureFiles {
    fun isJpeg(f: File): Boolean = try {
        f.inputStream().use { it.read() == 0xFF && it.read() == 0xD8 }
    } catch (_: Exception) { false }

    /** A complete JPEG: starts with FFD8 and ends with FFD9. */
    fun jpgOk(f: File): Boolean = try {
        f.length() > 1024 && isJpeg(f) && RandomAccessFile(f, "r").use { r ->
            r.seek(r.length() - 2); r.read() == 0xFF && r.read() == 0xD9
        }
    } catch (_: Exception) { false }

    /** The badge text for a mode; RAW is never shown as certain before a DNG was verified. */
    fun badge(mode: CaptureMode, verified: Boolean): String {
        val base = when (mode) {
            CaptureMode.RAW_JPEG -> "RAW + JPEG"
            CaptureMode.RAW -> "RAW"
            CaptureMode.JPEG -> "JPEG"
        }
        return if (mode != CaptureMode.JPEG && !verified) "$base · 待确认" else base
    }

    /** API 24-28 gallery copy: `<Pictures>/Dapple/<name>`, a numeric suffix instead of overwriting an existing file. */
    fun legacyTarget(pictures: File, name: String): File {
        val dir = File(pictures, "Dapple").apply { mkdirs() }
        var f = File(dir, name)
        val dot = name.lastIndexOf('.')
        val stem = if (dot > 0) name.substring(0, dot) else name
        val ext = if (dot > 0) name.substring(dot) else ""
        var n = 1
        while (f.exists()) f = File(dir, "${stem}_${n++}$ext")
        return f
    }

    /** A DNG is a TIFF (II*\0 or MM\0*) of a plausible size. */
    fun isDng(f: File): Boolean = try {
        f.length() > 100_000 && f.inputStream().use { i ->
            val h = ByteArray(4); i.read(h)
            (h[0] == 'I'.code.toByte() && h[1] == 'I'.code.toByte() && h[2] == 42.toByte() && h[3] == 0.toByte()) ||
                (h[0] == 'M'.code.toByte() && h[1] == 'M'.code.toByte() && h[2] == 0.toByte() && h[3] == 42.toByte())
        }
    } catch (_: Exception) { false }
}

/** What one shutter press writes. */
enum class CaptureMode { RAW_JPEG, RAW, JPEG }

/** Persistence of the chosen mode (SharedPreferences on the device, a map in the JVM tests). */
interface StrategyStore {
    fun get(key: String): String?
    fun put(key: String, value: String)
}

/**
 * Which capture mode a lens uses, decided by what actually works (no device names): RAW+JPEG is tried first; when its DNG does not
 * arrive the lens goes to RAW-only (Dapple develops the DNG itself, the camera JPEG is not needed), and only when RAW-only fails too
 * to JPEG-only. The result is remembered per camera id and build fingerprint ([key]); a mode that was verified (a DNG was read back)
 * is only demoted for the running process after a failure, an unverified one is demoted for good.
 */
class CaptureStrategy(
    private val store: StrategyStore,
    val key: String,
    private val rawJpegAvailable: Boolean,
    private val rawAvailable: Boolean,
) {
    var mode: CaptureMode = CaptureMode.JPEG
        private set
    var verified = false
        private set

    init {
        val saved = store.get(key)
        val m = saved?.removeSuffix("+")?.let { s -> CaptureMode.values().firstOrNull { it.name == s } }
        mode = clamp(m ?: best())
        verified = saved != null && saved.endsWith("+") && m == mode
    }

    private fun best() = when {
        rawJpegAvailable -> CaptureMode.RAW_JPEG
        rawAvailable -> CaptureMode.RAW
        else -> CaptureMode.JPEG
    }

    /** A remembered mode the lens no longer offers drops to the next one. */
    private fun clamp(m: CaptureMode): CaptureMode = when {
        m == CaptureMode.RAW_JPEG && !rawJpegAvailable -> clamp(CaptureMode.RAW)
        m == CaptureMode.RAW && !rawAvailable -> CaptureMode.JPEG
        else -> m
    }

    /** The current mode produced a real DNG (or, for JPEG, a JPEG): remember it as working. */
    fun markVerified() {
        verified = true
        store.put(key, mode.name + "+")
    }

    /** The current mode failed or stalled: step down (RAW+JPEG -> RAW -> JPEG). Returns the new mode. */
    fun failed(): CaptureMode {
        val wasVerified = verified
        mode = when (mode) {
            CaptureMode.RAW_JPEG -> clamp(CaptureMode.RAW)
            CaptureMode.RAW -> CaptureMode.JPEG
            CaptureMode.JPEG -> CaptureMode.JPEG
        }
        verified = mode == CaptureMode.JPEG
        if (!wasVerified) store.put(key, mode.name + if (verified) "+" else "")
        return mode
    }

    /** What the next shutter press will really write. A mode that carries RAW and was never verified says so. */
    fun badge(): String = CaptureFiles.badge(mode, verified)
}
