package com.dapple.dapple

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.util.Collections

class UsbCamTest {
    @get:Rule val tmp = TemporaryFolder()
    private lateinit var card: File
    private lateinit var inbox: File
    private val events = Collections.synchronizedList(ArrayList<Pair<String, Map<String, Any?>>>())
    private val sink = CamSink { t, d -> events.add(t to d) }

    @Before fun setUp() {
        UsbDiag.printer = {}
        card = tmp.newFolder("card")
        File(card, "DCIM/100MSDCF").mkdirs()
        inbox = tmp.newFolder("inbox")
    }

    private fun shot(name: String, bytes: Int = 1000, sec: Long = 1_700_000_000L, dir: String = "100MSDCF"): File {
        val f = File(card, "DCIM/$dir/$name")
        f.parentFile!!.mkdirs()
        f.writeBytes(ByteArray(bytes) { (it % 251).toByte() })
        f.setLastModified(sec * 1000)
        return f
    }

    private fun session(link: CameraLink = FakeCameraLink(card), policy: WatchPolicy = WatchPolicy()) =
        CamSession(link, inbox, sink, policy, sleeper = {}, clock = { System.nanoTime() / 1_000_000 })

    private fun types() = synchronized(events) { events.map { it.first } }
    private fun of(t: String) = synchronized(events) { events.filter { it.first == t }.map { it.second } }

    @Suppress("UNCHECKED_CAST")
    private fun groups(page: Map<String, Any?>) = page["groups"] as List<Map<String, Any?>>

    @Suppress("UNCHECKED_CAST")
    private fun files(g: Map<String, Any?>) = g["files"] as List<Map<String, Any?>>

    // ---------------------------------------------------------------- names, identity, grouping

    @Test fun acceptedTypes() {
        for (n in listOf("A.ARW", "b.raf", "C.DNG", "d.JPG", "e.jpeg", "f.HIF", "g.heif", "h.HEIC")) assertTrue(n, CamNames.accepted(n))
        for (n in listOf("v.MP4", "v.mov", "x.xml", "noext", "THM.thm")) assertFalse(n, CamNames.accepted(n))
        assertEquals(CamKind.VIDEO, CamNames.kind("C0001.MP4"))
    }

    @Test fun identityIgnoresHandleStorageFolderAndCase() {
        val a = CamObject(5, 0x10001, 7, 0x3801, "DSC00012.JPG", 4096, 1_700_000_000_400)
        val b = CamObject(99, 0x20001, 8, 0x3801, "dsc00012.jpg", 4096, 1_700_000_000_900)
        assertEquals(CamIdentity.key(a), CamIdentity.key(b))
        assertEquals("usb:dsc00012.jpg|4096|1700000000", CamIdentity.key(a))
        assertNotEquals(CamIdentity.key(a), CamIdentity.key(a.copy(size = 4097)))
        assertNotEquals(CamIdentity.key(a), CamIdentity.key(a.copy(dateMs = 1_700_000_001_000)))
    }

    @Test fun ftpCopyIsRecognisedByNameAndSize() {
        assertTrue(CamIdentity.nameSizeMatches("DSC00012.ARW", 61_000_000, "dsc00012.arw", 61_000_000))
        assertFalse(CamIdentity.nameSizeMatches("DSC00012.ARW", 61_000_000, "DSC00012.ARW", 60_999_999))
    }

    @Test fun rawAndJpegWithTheSameStemAreOneGroupJpegFirst() {
        val o = listOf(
            CamObject(1, 1, 0, 0x3000, "DSC00001.ARW", 60, 1000),
            CamObject(2, 1, 0, 0x3801, "DSC00001.JPG", 10, 1000),
            CamObject(3, 1, 0, 0x3801, "DSC00002.JPG", 10, 2000),
        )
        val g = CamGrouping.group(o) { "100MSDCF" }
        assertEquals(2, g.size)
        assertEquals(listOf("DSC00001.JPG", "DSC00001.ARW"), g[0].files.map { it.name })
    }

    @Test fun sameStemInDifferentFoldersIsNotPaired() {
        val o = listOf(
            CamObject(1, 1, 10, 0x3000, "DSC00001.ARW", 60, 1000),
            CamObject(2, 1, 11, 0x3801, "DSC00001.JPG", 10, 5000),
        )
        assertEquals(2, CamGrouping.group(o) { if (it.parent == 10) "100MSDCF" else "101MSDCF" }.size)
    }

    // ---------------------------------------------------------------- paging

    @Test fun pagesAreNewestFirstAndStopAtTheEnd() {
        for (i in 1..25) shot("DSC%05d.JPG".format(i), sec = 1_700_000_000L + i)
        val s = session()
        s.open()
        val p1 = s.listPage(0, 10)
        assertEquals(10, groups(p1).size)
        assertEquals("dsc00025", groups(p1)[0]["stem"])
        assertEquals("dsc00016", groups(p1).last()["stem"])
        assertFalse(p1["done"] as Boolean)
        val p2 = s.listPage(p1["next"] as Int, 10)
        assertEquals("dsc00015", groups(p2)[0]["stem"])
        val p3 = s.listPage(p2["next"] as Int, 10)
        assertEquals(5, groups(p3).size)
        assertTrue(p3["done"] as Boolean)
        assertEquals(25, p3["total"])
    }

    @Test fun pagingReadsOnlyTheObjectsOfThePage() {
        for (i in 1..200) shot("DSC%05d.JPG".format(i), bytes = 10)
        val link = CountingLink(FakeCameraLink(card))
        val s = session(link)
        s.open()
        link.infoCalls = 0
        s.listPage(0, 20)
        assertTrue("info calls ${link.infoCalls}", link.infoCalls <= 30) // 20 files + at most a few folder lookups, not 200
    }

    @Test fun pageGroupsRawAndJpegAndSkipsVideosAndStrangers() {
        shot("DSC00001.ARW", 500, 1_700_000_001); shot("DSC00001.JPG", 50, 1_700_000_001)
        shot("C0001.MP4", 900, 1_700_000_002); shot("C0001M01.XML", 5, 1_700_000_002)
        val s = session(); s.open()
        val p = s.listPage(0, 50)
        assertEquals(1, groups(p).size)
        assertEquals(listOf("jpeg", "raw"), files(groups(p)[0]).map { it["kind"] })
        assertEquals(1, p["skippedVideos"])
    }

    @Test fun aNewerListCallStopsAnOlderOne() {
        for (i in 1..50) shot("DSC%05d.JPG".format(i), bytes = 10)
        val inner = FakeCameraLink(card)
        lateinit var s: CamSession
        var n = 0
        val link = object : CameraLink by inner {
            override fun objectInfo(handle: Int): CamObject? {
                if (++n == 5) s.cancelListing()
                return inner.objectInfo(handle)
            }
        }
        s = session(link); s.open()
        val p = s.listPage(0, 40)
        assertTrue(p["cancelled"] as Boolean)
        assertTrue(groups(p).size < 40)
    }

    @Test fun emptyCardListsNothingAndHasNoStorageWhenLocked() {
        val s = session(); s.open()
        assertEquals(0, groups(s.listPage(0, 10)).size)
        File(card, "NOCARD").writeText("x")
        assertTrue(FakeCameraLink(card).storages().isEmpty())
    }

    // ---------------------------------------------------------------- new shots: baseline, poll, events

    @Test fun baselineIsNeverReportedAsNew() {
        shot("DSC00001.JPG"); shot("DSC00002.JPG")
        val s = session(); s.open()
        assertTrue(s.pollOnce().isEmpty())
        assertTrue(of("newShots").isEmpty())
    }

    @Test fun pollFindsAShotTakenAfterConnectingAndPairsItsFiles() {
        shot("DSC00001.JPG")
        val s = session(); s.open()
        shot("DSC00002.ARW", 700); shot("DSC00002.JPG", 70)
        val found = s.pollOnce()
        assertEquals(2, found.size)
        val ev = of("newShots")
        assertEquals(1, ev.size)
        val g = groups(ev[0])
        assertEquals(1, g.size)
        assertEquals(listOf("DSC00002.JPG", "DSC00002.ARW"), files(g[0]).map { it["name"] })
        assertTrue(s.pollOnce().isEmpty()) // reported once
    }

    @Test fun videoShotIsSkippedAndCounted() {
        val s = session(); s.open()
        shot("C0002.MP4", 100)
        assertTrue(s.pollOnce().isEmpty())
        assertEquals(1, s.skippedVideos)
    }

    @Test fun halfWrittenFileIsAwaitedUntilItsSizeIsStable() {
        val inner = FakeCameraLink(card)
        var reads = 0
        val link = object : CameraLink by inner {
            override fun objectInfo(handle: Int): CamObject? {
                val o = inner.objectInfo(handle)
                return if (o != null && o.name == "DSC00003.JPG" && reads++ < 2) o.copy(size = reads * 10L) else o // grows twice, then settles
            }
        }
        val s = session(link); s.open()
        shot("DSC00003.JPG", 100)
        assertEquals(1, s.pollOnce().size)
        val sent = files(groups(of("newShots")[0])[0])[0]
        assertEquals(100L, sent["size"])
        assertTrue(reads >= 4)
    }

    @Test fun eventAnnouncesTheShotAndPollDoesNotRepeatIt() {
        val policy = WatchPolicy()
        val link = FakeCameraLink(card)
        val s = session(link, policy); s.open()
        shot("DSC00005.JPG", 100)
        val h = link.handles(0).last()
        s.onEvent(CamEvent(PtpCode.EVENT_OBJECT_ADDED, h))
        assertEquals(1, of("newShots").size)
        assertTrue(s.pollOnce().isEmpty())
        assertEquals(WatchPolicy.Mode.EVENTS, policy.mode)
    }

    @Test fun aPollBetweenTwoEventsOfOneShotDoesNotDistrustEvents() {
        val queued = ArrayList<Runnable>()
        val link = FakeCameraLink(card)
        val p = WatchPolicy()
        val s = CamSession(link, inbox, sink, p, sleeper = {}, async = { queued.add(it) })
        s.open()
        shot("DSC00010.ARW", 600); shot("DSC00010.JPG", 60)
        val hs = link.handles(0).takeLast(2)
        s.onEvent(CamEvent(PtpCode.EVENT_OBJECT_ADDED, hs[0]))
        s.onEvent(CamEvent(PtpCode.EVENT_OBJECT_ADDED, hs[1]))
        assertTrue(s.pollOnce().isEmpty()) // both are announced, their reports are only queued
        assertEquals(WatchPolicy.Mode.EVENTS, p.mode)
        queued.forEach { it.run() }
        assertEquals(2, of("newShots").sumOf { groups(it).sumOf { g -> files(g).size } })
    }

    // ---------------------------------------------------------------- event vs poll selection

    @Test fun startsPollingFastThenTrustsEventsWithASlowPoll() {
        val p = WatchPolicy(fastMs = 2000, slowMs = 20000)
        assertEquals(WatchPolicy.Mode.POLL, p.mode)
        assertEquals(2000, p.nextPollDelayMs(10))
        p.onObjectAdded()
        assertEquals(WatchPolicy.Mode.EVENTS, p.mode)
        assertEquals(20000, p.nextPollDelayMs(10))
    }

    @Test fun pollFindingWhatNoEventAnnouncedDistrustsEventsForTheSession() {
        val p = WatchPolicy()
        val link = FakeCameraLink(card)
        val s = session(link, p); s.open()
        shot("DSC00006.JPG", 100)
        val h = link.handles(0).last()
        s.onEvent(CamEvent(PtpCode.EVENT_OBJECT_ADDED, h))
        assertEquals(WatchPolicy.Mode.EVENTS, p.mode)
        shot("DSC00007.JPG", 100) // no event for this one
        assertEquals(1, s.pollOnce().size)
        assertEquals(WatchPolicy.Mode.POLL, p.mode)
        assertTrue(p.distrusted)
        p.onObjectAdded() // a later event does not bring the trust back
        assertEquals(WatchPolicy.Mode.POLL, p.mode)
    }

    @Test fun aLinkWithoutEventsIsPolledForGood() {
        val p = WatchPolicy()
        val s = session(policy = p); s.open()
        s.eventLoop() // fake without the EVENTS marker: unsupported
        assertTrue(p.eventsUnsupported)
        assertEquals(WatchPolicy.Mode.POLL, p.mode)
    }

    @Test fun slowListingsStretchThePollInterval() {
        assertEquals(9000, WatchPolicy(fastMs = 2000).nextPollDelayMs(3000))
    }

    @Test fun eventLoopDeliversEventsFromTheFakeAndStopsWhenCancelled() {
        File(card, "EVENTS").writeText("1")
        val p = WatchPolicy()
        val s = session(policy = p); s.open()
        val t = Thread { s.eventLoop() }
        t.start()
        Thread.sleep(300)
        shot("DSC00008.JPG", 100)
        val until = System.currentTimeMillis() + 5000
        while (of("newShots").isEmpty() && System.currentTimeMillis() < until) Thread.sleep(50)
        assertEquals(1, of("newShots").size)
        assertEquals(WatchPolicy.Mode.EVENTS, p.mode)
        s.cancelEvents()
        t.join(3000)
        assertFalse(t.isAlive)
    }

    // ---------------------------------------------------------------- import

    @Test fun importCopiesBothFilesOfAShotAndReportsProgressOrder() {
        shot("DSC00001.ARW", 5000); shot("DSC00001.JPG", 500); shot("DSC00002.JPG", 300)
        val s = session(); s.open()
        val hs = FakeCameraLink(card).handles(0).toList()
        s.importHandles(hs)
        assertEquals(listOf("DSC00001.ARW", "DSC00001.JPG", "DSC00002.JPG").toSet(), inbox.list()!!.toSet())
        val done = of("importDone")
        assertEquals(3, done.size)
        assertEquals(2, (of("importQueue")[0]["photos"]))
        assertEquals(1, done[0]["index"]); assertEquals(1, done[1]["index"]); assertEquals(2, done[2]["index"])
        val end = of("importQueueDone")[0]
        assertEquals(3, end["imported"]); assertEquals(0, end["failed"])
        assertEquals(5000L, File(inbox, "DSC00001.ARW").length())
    }

    @Test fun importingTheSameListAgainDoesNotCopyAgain() {
        shot("DSC00001.JPG", 500)
        val s = session(); s.open()
        val hs = FakeCameraLink(card).handles(0).toList()
        s.importHandles(hs)
        val m = File(inbox, "DSC00001.JPG").lastModified()
        events.clear()
        s.importHandles(hs)
        assertTrue(of("importStart").isEmpty())
        assertEquals(true, of("importDone")[0]["again"])
        assertEquals(m, File(inbox, "DSC00001.JPG").lastModified())
    }

    @Test fun nameClashWithADifferentFileRenamesTheWholeShot() {
        File(inbox, "DSC00001.JPG").writeText("an older photo with the same name")
        shot("DSC00001.JPG", 500, sec = 1_700_000_123); shot("DSC00001.ARW", 900, sec = 1_700_000_123)
        val s = session(); s.open()
        s.importHandles(FakeCameraLink(card).handles(0).toList())
        assertTrue(File(inbox, "DSC00001-1700000123.JPG").isFile)
        assertTrue(File(inbox, "DSC00001-1700000123.ARW").isFile)
        assertEquals("an older photo with the same name", File(inbox, "DSC00001.JPG").readText())
    }

    @Test fun unplugMidImportRemovesThePartialFileAndAbortsTheQueue() {
        shot("DSC00001.ARW", 3 shl 20); shot("DSC00002.ARW", 3 shl 20)
        val link = FakeCameraLink(card, chunkDelayMs = 10)
        val s = session(link); s.open()
        val t = Thread { s.importHandles(link.handles(0).toList()) }
        t.start()
        val until = System.currentTimeMillis() + 5000
        while (of("importProgress").isEmpty() && System.currentTimeMillis() < until) Thread.sleep(10)
        link.unplug()
        t.join(10000)
        assertFalse(t.isAlive)
        assertEquals(listOf("importFailed"), of("importFailed").map { "importFailed" })
        assertEquals("gone", of("importFailed")[0]["reason"])
        assertEquals("gone", of("importQueueDone")[0]["aborted"])
        assertTrue(types().contains("gone"))
        assertEquals(0, inbox.listFiles()!!.size) // no .part, no half file, second file never started
        assertEquals(1, of("importStart").size)
    }

    @Test fun replugResumesWithoutCopyingFinishedFilesAgain() {
        shot("DSC00001.JPG", 400); shot("DSC00002.ARW", 3 shl 20); shot("DSC00003.JPG", 300)
        val link1 = FakeCameraLink(card, chunkDelayMs = 10)
        val s1 = session(link1); s1.open()
        val hs1 = link1.handles(0).toList()
        val t = Thread { s1.importHandles(hs1) }
        t.start()
        val until = System.currentTimeMillis() + 5000
        while (of("importStart").size < 2 && System.currentTimeMillis() < until) Thread.sleep(5)
        link1.unplug()
        t.join(10000)
        s1.onUnplugged()
        assertTrue(File(inbox, "DSC00001.JPG").isFile)
        assertFalse(File(inbox, "DSC00002.ARW").exists())
        // plug in again: a new link and session, same card
        events.clear()
        val s2 = session(); s2.open()
        s2.importHandles(FakeCameraLink(card).handles(0).toList())
        val again = of("importDone").filter { it["again"] == true }.map { it["name"] }
        assertEquals(listOf("DSC00001.JPG"), again)
        assertEquals(3, of("importDone").size)
        assertEquals(3, inbox.listFiles()!!.size)
    }

    @Test fun cancelStopsAfterTheFileInFlight() {
        for (i in 1..4) shot("DSC0000$i.ARW", 1 shl 20)
        val link = FakeCameraLink(card, chunkDelayMs = 5)
        val s = session(link); s.open()
        val t = Thread { s.importHandles(link.handles(0).toList()) }
        t.start()
        while (of("importStart").isEmpty()) Thread.sleep(5)
        s.cancelImport()
        t.join(10000)
        val end = of("importQueueDone")[0]
        assertEquals(true, end["cancelled"])
        assertTrue((end["imported"] as Int) < 4)
        assertTrue(inbox.listFiles()!!.none { it.name.endsWith(".part") })
    }

    @Test fun twoFailuresInARowAbortTheQueue() {
        for (i in 1..4) shot("DSC0000$i.JPG", 100)
        val inner = FakeCameraLink(card)
        val link = object : CameraLink by inner {
            override fun importTo(handle: Int, dest: File) = false
        }
        val s = session(link); s.open()
        s.importHandles(inner.handles(0).toList())
        assertEquals(2, of("importFailed").size)
        assertEquals("failed", of("importQueueDone")[0]["aborted"])
    }

    @Test fun convertFailureDropsTheFileAndContinues() {
        shot("DSC00001.HIF", 100); shot("DSC00002.JPG", 100)
        val s = CamSession(FakeCameraLink(card), inbox, sink, WatchPolicy(), sleeper = {}, postProcess = { f, o ->
            if (CamNames.kind(o.name) == CamKind.HEIF) throw IllegalStateException("no decoder") else f
        })
        s.open()
        s.importHandles(FakeCameraLink(card).handles(0).toList())
        assertEquals("convert", of("importFailed")[0]["reason"])
        assertEquals(1, of("importDone").size)
        assertFalse(File(inbox, "DSC00001.HIF").exists())
    }

    @Test fun unpluggedSessionIgnoresFurtherPolls() {
        val s = session(); s.open()
        s.onUnplugged()
        assertTrue(s.isClosed)
        shot("DSC00009.JPG", 10)
        assertTrue(s.pollOnce().isEmpty())
    }

    @Test fun deviceGoneWhileListingSurfacesAsCameraGone() {
        val link = FakeCameraLink(card)
        val s = session(link); s.open()
        link.unplug()
        var gone = false
        try { s.listPage(0, 10, refresh = true) } catch (e: CameraGoneException) { gone = true }
        assertTrue(gone)
    }

    // ---------------------------------------------------------------- helper

    private class CountingLink(private val d: CameraLink) : CameraLink by d {
        var infoCalls = 0
        override fun objectInfo(handle: Int): CamObject? { infoCalls++; return d.objectInfo(handle) }
    }
}
