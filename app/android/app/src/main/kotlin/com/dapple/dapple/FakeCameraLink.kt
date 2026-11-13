package com.dapple.dapple

import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream

/**
 * A "camera" that is a folder of files, for the JVM tests and (debuggable builds only) the emulator, which has no USB host.
 * Layout: <root>/DCIM/<folder>/<files>. Control files in <root>:
 *   ATTACHED  present = the cable is plugged in (the emulator hub watches it); remove it to unplug, even in the middle of a copy
 *   SLOW      present = copies run at ~1 MB/s in 64 KB chunks (time to unplug mid-import)
 *   EVENTS    present = the fake sends ObjectAdded events for files that appear; absent = readEvent is unsupported (polling only)
 *   NOCARD    present = the camera lists no storage (empty or locked card)
 * Handles are assigned in the order files are first seen, so a file added later gets a higher handle, like a real body.
 * Drop a file into DCIM/100MSDCF to simulate a shot; write it in two steps (a short file, then the full one) to simulate a slow write.
 */
class FakeCameraLink(
    private val root: File,
    override val info: CamInfo = CamInfo("Fake", "Test Camera", "1.0", "FAKE0001", listOf(0x1001, 0x1004, 0x1007, 0x1008, 0x1009), listOf(PtpCode.EVENT_OBJECT_ADDED)),
    private val chunkDelayMs: Long = 0,
    private val useMarker: Boolean = false,
) : CameraLink {
    private val handles = LinkedHashMap<String, Int>() // relative path -> handle
    private val byHandle = HashMap<Int, File>()
    private var next = 1
    private val dcim get() = File(root, "DCIM")
    @Volatile private var closed = false
    @Volatile private var plugged = true

    /** Simulates pulling the cable in tests (the emulator uses the ATTACHED marker instead). */
    fun unplug() { plugged = false }

    private fun gone() = closed || !plugged || (useMarker && !File(root, "ATTACHED").exists())

    private fun check() { if (gone()) throw CameraGoneException("fake camera unplugged") }

    @Synchronized private fun scan() {
        check()
        val all = dcim.walkTopDown().filter { it.isFile }.sortedBy { it.relativeTo(root).path }.toList()
        // keep first-seen order: new files get new, higher handles
        for (f in all) {
            val rel = f.relativeTo(root).path.replace('\\', '/')
            if (rel !in handles) {
                val h = next++
                handles[rel] = h
                byHandle[h] = f
            }
        }
    }

    override fun storages(): List<CamStorage> {
        check()
        if (File(root, "NOCARD").exists()) return emptyList()
        return listOf(CamStorage(0x10001, "Slot 1", "FAKE", 64L shl 30, 32L shl 30))
    }

    override fun handles(storageId: Int): IntArray {
        scan()
        return synchronized(this) { handles.values.toIntArray() }
    }

    override fun objectInfo(handle: Int): CamObject? {
        check()
        val f = synchronized(this) { byHandle[handle] } ?: return null
        if (!f.exists()) return null
        return CamObject(handle, 0x10001, 0, if (CamNames.kind(f.name) == CamKind.JPEG) 0x3801 else 0x3000, f.name, f.length(), f.lastModified())
    }

    override fun thumbnail(handle: Int): ByteArray? {
        check()
        val f = synchronized(this) { byHandle[handle] } ?: return null
        return if (CamNames.kind(f.name) == CamKind.JPEG && f.length() < (3 shl 20)) f.readBytes() else null
    }

    override fun importTo(handle: Int, dest: File): Boolean {
        check()
        val src = synchronized(this) { byHandle[handle] } ?: return false
        val slow = File(root, "SLOW").exists()
        FileInputStream(src).use { i ->
            FileOutputStream(dest).use { o ->
                val buf = ByteArray(64 * 1024)
                while (true) {
                    check()
                    val n = i.read(buf)
                    if (n < 0) break
                    o.write(buf, 0, n)
                    o.flush()
                    if (slow) Thread.sleep(60) else if (chunkDelayMs > 0) Thread.sleep(chunkDelayMs)
                }
            }
        }
        return true
    }

    override fun readEvent(cancel: CancelFlag): CamEvent? {
        if (!File(root, "EVENTS").exists()) throw UnsupportedOperationException("fake without EVENTS marker")
        val known = HashSet<Int>().also { s -> synchronized(this) { s.addAll(handles.values) } }
        scan()
        synchronized(this) { known.addAll(handles.values) }
        while (!cancel.cancelled) {
            check()
            scan()
            val fresh = synchronized(this) { handles.values.firstOrNull { it !in known } }
            if (fresh != null) { known.add(fresh); return CamEvent(PtpCode.EVENT_OBJECT_ADDED, fresh) }
            Thread.sleep(100)
        }
        return null
    }

    override fun alive() = !gone()

    override fun close() { closed = true }
}
