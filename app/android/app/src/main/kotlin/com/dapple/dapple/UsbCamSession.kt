package com.dapple.dapple

import java.io.File

/** Where the session reports to (the plugin forwards to the EventChannel on the main thread). */
fun interface CamSink {
    fun emit(type: String, data: Map<String, Any?>)
}

/**
 * One open camera. All logic of the wired ingest lives here and is synchronous: the plugin (or a test) decides on which thread
 * each method runs. Safe to call from several threads (listing, polling, the event loop and an import run side by side).
 *
 * Which shots are "new": everything that is on the card when the session opens is the baseline (never imported by itself);
 * whatever appears afterwards is a new shot. Importing the baseline is an explicit act ([importHandles] from the picker).
 */
class CamSession(
    private val link: CameraLink,
    private val inbox: File,
    private val sink: CamSink,
    val policy: WatchPolicy = WatchPolicy(),
    private val sleeper: (Long) -> Unit = { Thread.sleep(it) },
    private val clock: () -> Long = { System.nanoTime() / 1_000_000 },
    /** Runs on each finished file before it is reported (HEIF to PNG conversion); returns the file to report. */
    private val postProcess: (File, CamObject) -> File = { f, _ -> f },
    private val minFreeBytes: Long = 64L shl 20,
    private val settleTries: Int = 6,
    private val settleWaitMs: Long = 500,
    /** Where the slow part of an event (waiting for a stable size) runs, so the event loop keeps reading; tests run it inline. */
    private val async: (Runnable) -> Unit = { it.run() },
) {
    private val lock = Any()
    private val seen = HashSet<Int>()          // every handle known (baseline + found + imported)
    private val announced = HashSet<Int>()     // handles an ObjectAdded event named, whether or not settled yet
    private val infoCache = HashMap<Int, CamObject>()
    private val dirCache = HashMap<Int, String>()
    private var order: IntArray? = null        // newest first, for the picker
    @Volatile private var listGen = 0
    @Volatile private var closed = false
    @Volatile var lastListingMs = 0L
        private set
    private val importCancel = CancelFlag()
    private val eventCancel = CancelFlag()
    @Volatile private var importing = false
    @Volatile var totalObjects = 0
        private set
    @Volatile var skippedVideos = 0
        private set

    private fun log(s: String) = UsbDiag.log(s)

    // ------------------------------------------------------------------ open / baseline

    /** Reads storages and the baseline. Returns the summary reported to the app. */
    fun open(): Map<String, Any?> {
        val t0 = clock()
        val st = link.storages()
        log("storages: ${st.size} ${st.joinToString { "id=0x${Integer.toHexString(it.id)} '${it.description}' vol='${it.volume}' cap=${it.capacity} free=${it.free}" }}")
        var total = 0
        for (s in st) {
            val t = clock()
            val h = link.handles(s.id)
            log("handles(storage 0x${Integer.toHexString(s.id)}) = ${h.size} in ${clock() - t} ms")
            synchronized(lock) { h.forEach { seen.add(it) } }
            total += h.size
        }
        lastListingMs = clock() - t0
        totalObjects = total
        log("baseline: $total objects, ${seen.size} handles, ${lastListingMs} ms")
        return summary(st)
    }

    private fun summary(st: List<CamStorage>): Map<String, Any?> = mapOf(
        "total" to totalObjects,
        "storages" to st.map { mapOf("id" to it.id, "description" to it.description, "capacity" to it.capacity, "free" to it.free) },
        "events" to link.info.events.map { "0x" + Integer.toHexString(it) },
    )

    // ------------------------------------------------------------------ reading objects

    private fun info(handle: Int): CamObject? {
        synchronized(lock) { infoCache[handle]?.let { return it } }
        val o = link.objectInfo(handle) ?: return null
        synchronized(lock) { infoCache[handle] = o }
        return o
    }

    private fun dirOf(o: CamObject): String {
        if (o.parent == 0 || o.parent == -1) return ""
        synchronized(lock) { dirCache[o.parent]?.let { return it } }
        val d = link.objectInfo(o.parent)?.name ?: ""
        synchronized(lock) { dirCache[o.parent] = d }
        return d
    }

    private fun itemMap(o: CamObject): Map<String, Any?> = mapOf(
        "handle" to o.handle,
        "name" to o.name,
        "size" to o.size,
        "dateMs" to o.dateMs,
        "key" to CamIdentity.key(o),
        "kind" to CamNames.kind(o.name).name.lowercase(),
    )

    private fun groupMap(g: CamGroup): Map<String, Any?> = mapOf(
        "gid" to "${g.dir.lowercase()}/${g.stem}",
        "stem" to g.stem,
        "dir" to g.dir,
        "dateMs" to g.dateMs,
        "files" to g.files.map { itemMap(it) },
    )

    /**
     * One page of the card for the picker, newest first. [cursor] indexes the handle list sorted by descending handle (cameras number
     * objects in the order they were written); within a page the groups are sorted by capture date. About [limit] files are examined
     * per call, only their object info is read (cheap), so thousands of files never block anything. A newer call (or [cancelListing])
     * stops an older one at the next object. A RAW and its JPEG can land on two pages: the receiver merges groups by "gid".
     */
    fun listPage(cursor: Int, limit: Int, refresh: Boolean = false): Map<String, Any?> {
        val gen = ++listGen
        val t0 = clock()
        var ord = synchronized(lock) { order }
        if (ord == null || refresh) {
            val all = ArrayList<Int>()
            for (s in link.storages()) link.handles(s.id).forEach { all.add(it) }
            ord = all.sortedDescending().toIntArray()
            synchronized(lock) { order = ord }
            totalObjects = ord.size
            log("list: ${ord.size} handles in ${clock() - t0} ms")
        }
        val files = ArrayList<CamObject>()
        var idx = cursor
        var examined = 0
        var cancelled = false
        while (idx < ord.size && files.size < limit && examined < limit * 4) {
            if (gen != listGen || closed) { cancelled = true; break }
            val o = info(ord[idx])
            idx++
            examined++
            if (o == null || o.format == PtpCode.FORMAT_ASSOCIATION) continue
            when {
                CamNames.accepted(o.name) -> files.add(o)
                CamNames.kind(o.name) == CamKind.VIDEO -> skippedVideos++
            }
        }
        val groups = CamGrouping.group(files, ::dirOf).sortedByDescending { it.dateMs }
        log("list page cursor=$cursor -> ${files.size} files in ${groups.size} groups, next=$idx/${ord.size}, ${clock() - t0} ms${if (cancelled) " (cancelled)" else ""}")
        return mapOf(
            "groups" to groups.map { groupMap(it) },
            "next" to idx,
            "done" to (idx >= ord.size),
            "total" to ord.size,
            "skippedVideos" to skippedVideos,
            "cancelled" to cancelled,
        )
    }

    fun cancelListing() { listGen++ }

    fun thumbnail(handle: Int): ByteArray? = try { link.thumbnail(handle) } catch (e: Exception) { log("thumbnail($handle) failed: ${e.message}"); null }

    // ------------------------------------------------------------------ watching for new shots

    /** Re-lists every storage and reports the objects that are not known yet. Returns what was reported. */
    fun pollOnce(): List<CamObject> {
        if (closed) return emptyList()
        val t0 = clock()
        val fresh = ArrayList<Int>()
        for (s in link.storages()) {
            for (h in link.handles(s.id)) {
                val isNew = synchronized(lock) { h !in seen && h !in announced }
                if (isNew) fresh.add(h)
            }
        }
        lastListingMs = clock() - t0
        if (fresh.isEmpty()) return emptyList()
        log("poll: ${fresh.size} new handles in $lastListingMs ms (mode=${policy.mode})")
        policy.onUnannouncedByPoll()
        return report(fresh.sorted())
    }

    /** Handles one camera event. Only ObjectAdded matters for importing; everything else is logged for the diagnostics. */
    fun onEvent(ev: CamEvent) {
        log("event 0x${Integer.toHexString(ev.code)} ${PtpCode.eventName(ev.code)} p1=${ev.p1} p2=${ev.p2} p3=${ev.p3} at ${clock()} ms")
        if (ev.code != PtpCode.EVENT_OBJECT_ADDED) return
        policy.onObjectAdded()
        val known = synchronized(lock) { !announced.add(ev.p1) || ev.p1 in seen }
        if (known) return
        async(Runnable { try { report(listOf(ev.p1)) } catch (e: Exception) { log("report of ${ev.p1} failed: ${e.javaClass.simpleName}: ${e.message}") } })
    }

    /** Blocks reading events until [cancelEvents] or the camera goes. Returns normally when events are unsupported. */
    fun eventLoop() {
        try {
            while (!closed && !eventCancel.cancelled) {
                val ev = link.readEvent(eventCancel) ?: break
                onEvent(ev)
            }
        } catch (e: UnsupportedOperationException) {
            log("events: not available on this link (${e.message}); polling only")
            policy.onEventsUnsupported()
        } catch (e: Exception) {
            if (!closed && !eventCancel.cancelled) {
                log("events: read failed (${e.javaClass.simpleName}: ${e.message}); polling only")
                policy.onEventsUnsupported()
            }
        }
    }

    fun cancelEvents() = eventCancel.cancel()

    /** Reads, waits until a (possibly still being written) object has a stable size, and announces the group(s). */
    private fun report(handles: List<Int>): List<CamObject> {
        val out = ArrayList<CamObject>()
        for (h in handles) {
            val o = settle(h)
            synchronized(lock) { seen.add(h) }
            if (o == null) { log("new handle $h: no object info (gone or busy), ignored"); continue }
            if (o.format == PtpCode.FORMAT_ASSOCIATION) continue
            if (CamNames.kind(o.name) == CamKind.VIDEO) { skippedVideos++; log("new video ${o.name} skipped (videos are not imported yet)"); continue }
            if (!CamNames.accepted(o.name)) { log("new object ${o.name} ignored (type not imported)"); continue }
            out.add(o)
        }
        if (out.isEmpty()) return out
        totalObjects += out.size
        val groups = CamGrouping.group(out, ::dirOf)
        log("new shots: ${out.joinToString { "${it.name} ${it.size} B" }}")
        sink.emit("newShots", mapOf("groups" to groups.map { groupMap(it) }, "total" to totalObjects))
        return out
    }

    /** Object info once its size stopped changing (and is not 0); the last reading if it never settles. */
    private fun settle(handle: Int): CamObject? {
        var last: CamObject? = null
        for (i in 0 until settleTries) {
            val o = try { link.objectInfo(handle) } catch (e: Exception) { null }
            if (o == null) { if (i + 1 < settleTries) sleeper(settleWaitMs); continue }
            if (last != null && o.size == last.size && o.size > 0) { synchronized(lock) { infoCache[handle] = o }; return o }
            last = o
            if (i + 1 < settleTries) sleeper(settleWaitMs)
        }
        if (last != null) { log("object $handle ${last.name}: size never settled (${last.size}), using it anyway"); synchronized(lock) { infoCache[handle] = last } }
        return last
    }

    // ------------------------------------------------------------------ import

    /**
     * Imports the files of [handles] one after another into [inbox]: written as "name.part", renamed when complete, so a partial file
     * is never visible and an unplug leaves nothing behind. A file already in the inbox with the same size is not copied again
     * (resume after a re-plug is "import the same list again"). Name clash with a different file: the whole exposure gets "-<time>".
     * Reports importStart / importProgress / importDone / importFailed per file and importQueueDone at the end.
     */
    fun importHandles(handles: List<Int>) {
        importing = true
        importCancel.reset()
        var done = 0
        var failed = 0
        var consecutiveFails = 0
        var cancelled = false
        var aborted: String? = null
        try {
            inbox.mkdirs()
            val objs = handles.mapNotNull { info(it) }
            val suffixed = collisionSuffixes(objs)
            val photoIndex = HashMap<String, Int>()
            for (o in objs) photoIndex.getOrPut(dirOf(o).lowercase() + "/" + CamNames.stem(o.name)) { photoIndex.size + 1 }
            val photoTotal = photoIndex.size
            sink.emit("importQueue", mapOf("files" to objs.size, "photos" to photoTotal))
            for (o in objs) {
                if (importCancel.cancelled || closed) { cancelled = true; break }
                val gk = dirOf(o).lowercase() + "/" + CamNames.stem(o.name)
                val base = mapOf("key" to CamIdentity.key(o), "name" to o.name, "size" to o.size, "index" to photoIndex[gk], "total" to photoTotal)
                val free = inbox.usableSpace
                if (free in 1 until (o.size + minFreeBytes)) {
                    log("import ${o.name}: not enough space ($free free, ${o.size} needed)")
                    sink.emit("importFailed", base + mapOf("reason" to "space"))
                    failed++
                    aborted = "space"
                    break
                }
                val outName = suffixed[gk]?.let { o.name.substringBeforeLast('.') + it + "." + o.name.substringAfterLast('.', "") } ?: o.name
                val dest = File(inbox, outName)
                if (dest.isFile && dest.length() == o.size) {
                    log("import ${o.name}: already in the inbox (${o.size} B), not copied again")
                    sink.emit("importDone", base + mapOf("path" to dest.absolutePath, "dateMs" to o.dateMs, "outName" to outName, "again" to true))
                    done++
                    consecutiveFails = 0
                    continue
                }
                sink.emit("importStart", base)
                val part = File(inbox, "$outName.part")
                part.delete()
                val t0 = clock()
                val ticker = progressTicker(part, o)
                val ok = try {
                    link.importTo(o.handle, part)
                } catch (e: CameraGoneException) {
                    part.delete()
                    log("import ${o.name}: camera gone mid-transfer at ${part.length()} B (${e.message})")
                    sink.emit("importFailed", base + mapOf("reason" to "gone"))
                    failed++
                    aborted = "gone"
                    ticker.interrupt()
                    break
                } catch (e: Exception) {
                    log("import ${o.name}: ${e.javaClass.simpleName}: ${e.message}")
                    false
                } finally {
                    ticker.interrupt()
                }
                val ms = clock() - t0
                if (ok && part.length() > 0 && part.renameTo(dest)) {
                    val final = try { postProcess(dest, o) } catch (e: Exception) { log("post-process ${o.name} failed: ${e.message}"); null }
                    if (final == null) {
                        dest.delete()
                        sink.emit("importFailed", base + mapOf("reason" to "convert"))
                        failed++
                        continue
                    }
                    log("import ${o.name}: ${o.size} B in $ms ms (${if (ms > 0) o.size * 1000 / ms / 1024 else 0} KB/s)")
                    sink.emit("importDone", base + mapOf("path" to final.absolutePath, "dateMs" to o.dateMs, "outName" to final.name, "again" to false, "ms" to ms))
                    done++
                    consecutiveFails = 0
                } else {
                    part.delete()
                    if (!link.alive()) {
                        log("import ${o.name}: failed and the camera is gone")
                        sink.emit("importFailed", base + mapOf("reason" to "gone"))
                        failed++
                        aborted = "gone"
                        break
                    }
                    log("import ${o.name}: failed after $ms ms (importFile returned false, ${part.length()} B written)")
                    sink.emit("importFailed", base + mapOf("reason" to "failed"))
                    failed++
                    if (++consecutiveFails >= 2) { aborted = "failed"; break }
                }
            }
        } finally {
            importing = false
            // a file that never got renamed must not stay behind
            inbox.listFiles { f -> f.name.endsWith(".part") }?.forEach { it.delete() }
            sink.emit("importQueueDone", mapOf("imported" to done, "failed" to failed, "cancelled" to cancelled, "aborted" to aborted))
            if (aborted == "gone") sink.emit("gone", emptyMap())
        }
    }

    /** Stops the queue after the file in flight (a blocking MTP transfer cannot be aborted halfway). */
    fun cancelImport() { importCancel.cancel() }

    private fun progressTicker(part: File, o: CamObject): Thread {
        val t = Thread {
            try {
                var last = -1L
                while (true) {
                    val len = part.length()
                    if (len != last) { last = len; sink.emit("importProgress", mapOf("name" to o.name, "bytes" to len, "size" to o.size)) }
                    Thread.sleep(200)
                }
            } catch (_: InterruptedException) {}
        }
        t.isDaemon = true
        t.name = "usbcam-progress"
        t.start()
        return t
    }

    private fun collisionSuffixes(objs: List<CamObject>): Map<String, String> {
        val out = HashMap<String, String>()
        for (o in objs) {
            val gk = dirOf(o).lowercase() + "/" + CamNames.stem(o.name)
            if (gk in out) continue
            val dest = File(inbox, o.name)
            if (dest.isFile && dest.length() != o.size) out[gk] = "-" + (o.dateMs / 1000)
        }
        return out
    }

    // ------------------------------------------------------------------ end

    fun isImporting() = importing

    /** The camera is gone (detach broadcast or a failing call): stop everything, remove partial files, close. */
    fun onUnplugged() {
        if (closed) return
        log("unplugged (importing=$importing)")
        closed = true
        importCancel.cancel()
        eventCancel.cancel()
        listGen++
        try { inbox.listFiles { f -> f.name.endsWith(".part") }?.forEach { it.delete() } } catch (_: Exception) {}
        try { link.close() } catch (_: Exception) {}
    }

    fun close() {
        if (closed) return
        closed = true
        importCancel.cancel()
        eventCancel.cancel()
        listGen++
        try { link.close() } catch (_: Exception) {}
    }

    val isClosed get() = closed
}
