package com.dapple.dapple

import java.io.File
import java.io.IOException

/**
 * Wired camera ingest, platform-independent half (no android.* here, so the JVM tests run all of it).
 * The only thing that talks to hardware is [CameraLink]; MtpCameraLink.kt is the real one (android.mtp.MtpDevice),
 * FakeCameraLink.kt is a folder of files (JVM tests, and the emulator behind the debuggable-build flag).
 */

/** PTP / MTP numbers worth naming in the diagnostics (ISO 15740 event and format codes). */
object PtpCode {
    const val EVENT_OBJECT_ADDED = 0x4002
    const val EVENT_OBJECT_REMOVED = 0x4003
    const val EVENT_STORE_ADDED = 0x4004
    const val EVENT_STORE_REMOVED = 0x4005
    const val EVENT_DEVICE_PROP_CHANGED = 0x4006
    const val EVENT_OBJECT_INFO_CHANGED = 0x4007
    const val EVENT_STORAGE_INFO_CHANGED = 0x400C
    const val EVENT_CAPTURE_COMPLETE = 0x400D
    const val FORMAT_ASSOCIATION = 0x3001 // a folder

    fun eventName(code: Int): String = when (code) {
        EVENT_OBJECT_ADDED -> "ObjectAdded"
        EVENT_OBJECT_REMOVED -> "ObjectRemoved"
        EVENT_STORE_ADDED -> "StoreAdded"
        EVENT_STORE_REMOVED -> "StoreRemoved"
        EVENT_DEVICE_PROP_CHANGED -> "DevicePropChanged"
        EVENT_OBJECT_INFO_CHANGED -> "ObjectInfoChanged"
        EVENT_STORAGE_INFO_CHANGED -> "StorageInfoChanged"
        EVENT_CAPTURE_COMPLETE -> "CaptureComplete"
        else -> "event"
    }
}

/** The camera went away (unplugged, powered off, sleeping) while an operation was running. */
class CameraGoneException(msg: String, cause: Throwable? = null) : IOException(msg, cause)

data class CamStorage(val id: Int, val description: String, val volume: String, val capacity: Long, val free: Long)

data class CamInfo(
    val manufacturer: String,
    val model: String,
    val version: String,
    val serial: String,
    val operations: List<Int>,
    val events: List<Int>,
)

data class CamObject(
    val handle: Int,
    val storageId: Int,
    val parent: Int,
    val format: Int,
    val name: String,
    val size: Long,
    val dateMs: Long,
)

data class CamEvent(val code: Int, val p1: Int = 0, val p2: Int = 0, val p3: Int = 0)

/** Cooperative cancel for a blocking call: [hook] lets the owner abort the native wait (CancellationSignal.cancel). */
class CancelFlag {
    @Volatile var cancelled = false
        private set
    @Volatile var hook: (() -> Unit)? = null

    fun reset() { cancelled = false }

    fun cancel() {
        cancelled = true
        try { hook?.invoke() } catch (_: Exception) {}
    }
}

/** What the session needs from a camera. All calls block; callers keep them off the main thread. */
interface CameraLink {
    val info: CamInfo
    fun storages(): List<CamStorage>

    /** Every object handle on the storage (folders included). Empty when the card cannot be listed. */
    fun handles(storageId: Int): IntArray

    /** Null when the object cannot be read (gone, camera busy). */
    fun objectInfo(handle: Int): CamObject?
    fun thumbnail(handle: Int): ByteArray?

    /** Copies the object to [dest]; false on failure, [CameraGoneException] when the camera vanished. */
    fun importTo(handle: Int, dest: File): Boolean

    /** Blocks until the camera sends an event; null when cancelled. [UnsupportedOperationException] = this link has no events. */
    fun readEvent(cancel: CancelFlag): CamEvent?

    /** False once the device is gone. */
    fun alive(): Boolean
    fun close()
}

enum class CamKind { RAW, JPEG, HEIF, VIDEO, OTHER }

object CamNames {
    private val raw = setOf("arw", "raf", "dng")
    private val jpeg = setOf("jpg", "jpeg")
    private val heif = setOf("heif", "heic", "hif")
    private val video = setOf("mp4", "mov", "mxf", "avi", "mts", "m2ts", "mkv", "lrv")

    fun ext(name: String) = name.substringAfterLast('.', "").lowercase()
    fun stem(name: String) = name.substringBeforeLast('.').lowercase()

    fun kind(name: String): CamKind {
        val e = ext(name)
        return when {
            e in raw -> CamKind.RAW
            e in jpeg -> CamKind.JPEG
            e in heif -> CamKind.HEIF
            e in video -> CamKind.VIDEO
            else -> CamKind.OTHER
        }
    }

    fun accepted(name: String) = kind(name).let { it == CamKind.RAW || it == CamKind.JPEG || it == CamKind.HEIF }
}

/**
 * Identity of a camera file. Rule: lower-case file name + "|" + size in bytes + "|" + capture time in whole seconds, prefixed "usb:".
 * Not in the key on purpose: the object handle and storage id (reassigned between sessions and when a card moves slots),
 * the folder (100MSDCF vs 101MSDCF holds the same shot after a card copy), the camera serial (not always readable, and the
 * triple is already unique within a body's 4-digit counter cycle: a rolled-over counter differs in time).
 * [nameSizeMatches] is how a photo that arrived over FTP (no capture time there) is recognised as the same one.
 */
object CamIdentity {
    fun key(o: CamObject) = key(o.name, o.size, o.dateMs)
    fun key(name: String, size: Long, dateMs: Long) = "usb:${name.lowercase()}|$size|${dateMs / 1000}"
    fun nameSizeMatches(a: String, sizeA: Long, b: String, sizeB: Long) = a.equals(b, ignoreCase = true) && sizeA == sizeB
}

/** One exposure on the card: the RAW and the JPEG (or HEIF) with the same name in the same folder. */
data class CamGroup(val dir: String, val stem: String, val files: List<CamObject>) {
    val dateMs: Long get() = files.maxOf { it.dateMs }
}

object CamGrouping {
    /** Groups [objs] by (folder, name without extension); a group is ordered JPEG/HEIF first (instant preview), then RAW. */
    fun group(objs: List<CamObject>, dirOf: (CamObject) -> String): List<CamGroup> {
        val m = LinkedHashMap<String, MutableList<CamObject>>()
        for (o in objs) m.getOrPut(dirOf(o).lowercase() + "/" + CamNames.stem(o.name)) { ArrayList() }.add(o)
        return m.values.map { l ->
            val sorted = l.sortedBy { if (CamNames.kind(it.name) == CamKind.RAW) 1 else 0 }
            CamGroup(dirOf(sorted[0]), CamNames.stem(sorted[0].name), sorted)
        }
    }
}

/**
 * Picks between camera events and polling while the camera stays connected, with no knowledge of any camera model.
 * Start polling fast (nothing proves events work until one arrives). The first ObjectAdded event switches to events plus a slow
 * poll as safety net. If that poll then finds a new object no event announced, events are not trustworthy on this camera:
 * back to fast polling for the rest of the session. A link without events goes to polling for good.
 * The poll interval also grows with how long a listing takes (cards with many files list slowly on some cameras).
 */
class WatchPolicy(private val fastMs: Long = 2000, private val slowMs: Long = 20000) {
    enum class Mode { POLL, EVENTS }

    @Volatile var mode = Mode.POLL
        private set
    @Volatile var eventsUnsupported = false
        private set
    @Volatile var distrusted = false
        private set
    @Volatile var eventsSeen = 0
        private set

    @Synchronized fun onObjectAdded() {
        eventsSeen++
        if (!eventsUnsupported && !distrusted) mode = Mode.EVENTS
    }

    @Synchronized fun onUnannouncedByPoll() {
        if (mode == Mode.EVENTS) {
            distrusted = true
            mode = Mode.POLL
        }
    }

    @Synchronized fun onEventsUnsupported() {
        eventsUnsupported = true
        mode = Mode.POLL
    }

    fun nextPollDelayMs(lastListingMs: Long): Long = maxOf(if (mode == Mode.EVENTS) slowMs else fastMs, lastListingMs * 3)
}

/** Diagnostics: one tag, a ring buffer the "copy diagnostics" action returns. */
object UsbDiag {
    const val TAG = "DappleUsb"
    private val lines = ArrayDeque<String>()
    var printer: (String) -> Unit = { s -> try { android.util.Log.i(TAG, s) } catch (_: Throwable) {} }
    var clock: () -> Long = { System.currentTimeMillis() }
    private val stamp = java.text.SimpleDateFormat("HH:mm:ss.SSS", java.util.Locale.US)

    @Synchronized fun log(s: String) {
        printer(s)
        lines.addLast(synchronized(stamp) { stamp.format(java.util.Date(clock())) } + " " + s)
        while (lines.size > 800) lines.removeFirst()
    }

    @Synchronized fun dump(): String = lines.joinToString("\n")
    @Synchronized fun clear() = lines.clear()
}
