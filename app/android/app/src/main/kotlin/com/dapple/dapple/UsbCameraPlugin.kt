package com.dapple.dapple

import android.app.Activity
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ApplicationInfo
import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel
import java.io.File
import java.util.concurrent.Executors

/**
 * Wired camera ingest: a camera plugged into the phone's USB-C port, read through android.mtp.MtpDevice (platform API, no third-party code).
 *
 * Channels (no camera names anywhere in the logic):
 *   dapple/usbcam (MethodChannel)
 *     start {inbox}                    register for plug / unplug, look for a camera already plugged in
 *     stop                             unregister, close the camera
 *     status                           the last "state" event again
 *     requestPermission                ask again after a refusal
 *     list {cursor, limit, refresh}    one page of the card, newest first -> {groups, next, done, total, skippedVideos}
 *     thumbnail {handle}               JPEG bytes or null
 *     import {handles: [int]}          copy to the inbox in this order; progress and results arrive as events
 *     cancelImport                     stop after the file in flight
 *     diagnostics                      text to copy and send after trying a camera
 *   dapple/usbcam_events (EventChannel), maps with "type":
 *     state {state: none|permission|opening|ready|unplugged|error, reason?, device?, card?}
 *     newShots {groups}  importQueue / importStart / importProgress / importDone / importFailed / importQueueDone
 *
 * Background decision: no foreground service in this step. The wired import is something the user is looking at (the app is open
 * when a camera is plugged in, the attach dialog launches it); a transfer in flight holds a partial wake lock (10 minute cap,
 * released at once when the queue ends). If the process is killed in the background the ".part" file is swept on the next start and
 * the next connect resumes (finished files are recognised by name and size). Step 2 (shoot with the phone in a pocket, screen off)
 * needs a foreground service of type connectedDevice (FOREGROUND_SERVICE_CONNECTED_DEVICE, granted once USB permission exists);
 * that is the point to add it, with a notification showing "n received".
 *
 * Emulator / tests: in a debuggable build, if <externalFilesDir>/fakecam exists, that folder is the camera (FakeCameraLink.kt).
 */
class UsbCameraPlugin(private val activity: Activity, messenger: BinaryMessenger) {
    private val usb = activity.getSystemService(Context.USB_SERVICE) as UsbManager
    private val main = Handler(Looper.getMainLooper())
    private val control = Executors.newSingleThreadExecutor { Thread(it, "usbcam-control").apply { isDaemon = true } }
    private val thumbs = Executors.newSingleThreadExecutor { Thread(it, "usbcam-thumb").apply { isDaemon = true } }
    private val reporter = Executors.newSingleThreadExecutor { Thread(it, "usbcam-report").apply { isDaemon = true } }
    private val importer = Executors.newSingleThreadExecutor { Thread(it, "usbcam-import").apply { isDaemon = true } }
    private var sink: EventChannel.EventSink? = null
    private var inbox: File? = null
    private var receiverOn = false

    @Volatile private var session: CamSession? = null
    @Volatile private var deviceName: String? = null
    @Volatile private var lastState: Map<String, Any?> = mapOf("type" to "state", "state" to "none")
    private val denied = HashSet<String>()
    private var watchers: List<Thread> = emptyList()
    @Volatile private var hubStop = false
    private var wake: PowerManager.WakeLock? = null

    private val debuggable get() = activity.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0
    private val fakeRoot: File? get() = if (debuggable) activity.getExternalFilesDir(null)?.let { File(it, "fakecam") }?.takeIf { it.isDirectory } else null

    init {
        MethodChannel(messenger, "dapple/usbcam").setMethodCallHandler { call, result ->
            when (call.method) {
                "start" -> {
                    inbox = File(call.argument<String>("inbox") ?: activity.cacheDir.absolutePath).also { it.mkdirs(); sweepParts(it) }
                    registerReceiver()
                    control.execute { scan(userAsked = false) }
                    result.success(true)
                }
                "stop" -> { control.execute { closeSession("stop") }; unregisterReceiver(); hubStop = true; result.success(true) }
                "status" -> result.success(lastState)
                "requestPermission" -> { control.execute { denied.clear(); scan(userAsked = true) }; result.success(true) }
                "list" -> {
                    val s = session
                    if (s == null) { result.error("no_camera", "no camera", null); return@setMethodCallHandler }
                    val cursor = call.argument<Int>("cursor") ?: 0
                    val limit = call.argument<Int>("limit") ?: 60
                    val refresh = call.argument<Boolean>("refresh") ?: false
                    control.execute {
                        try {
                            val page = s.listPage(cursor, limit, refresh)
                            main.post { result.success(page) }
                        } catch (e: CameraGoneException) {
                            onGone(s)
                            main.post { result.error("gone", e.message, null) }
                        } catch (e: Exception) {
                            UsbDiag.log("list failed: ${e.javaClass.simpleName}: ${e.message}")
                            main.post { result.error("list", e.message, null) }
                        }
                    }
                }
                "thumbnail" -> {
                    val s = session
                    val h = call.argument<Int>("handle") ?: 0
                    if (s == null) { result.success(null); return@setMethodCallHandler }
                    thumbs.execute { val b = s.thumbnail(h); main.post { result.success(b) } }
                }
                "import" -> {
                    val s = session
                    val hs = call.argument<List<Int>>("handles") ?: emptyList()
                    if (s == null) { result.error("no_camera", "no camera", null); return@setMethodCallHandler }
                    result.success(true)
                    importer.execute {
                        acquireWake()
                        try { s.importHandles(hs) } catch (e: Exception) { UsbDiag.log("import queue crashed: ${e.javaClass.simpleName}: ${e.message}") } finally { releaseWake() }
                    }
                }
                "cancelImport" -> { session?.cancelImport(); result.success(true) }
                "diagnostics" -> result.success(diagnostics())
                else -> result.notImplemented()
            }
        }
        EventChannel(messenger, "dapple/usbcam_events").setStreamHandler(object : EventChannel.StreamHandler {
            override fun onListen(args: Any?, events: EventChannel.EventSink) { sink = events }
            override fun onCancel(args: Any?) { sink = null }
        })
    }

    fun dispose() {
        unregisterReceiver()
        hubStop = true
        session?.close()
        releaseWake()
    }

    // ------------------------------------------------------------------ events out

    private fun emit(type: String, data: Map<String, Any?>) {
        val m = HashMap<String, Any?>(data)
        m["type"] = type
        if (type == "state") lastState = m
        main.post { sink?.success(m) }
    }

    private fun state(state: String, reason: String? = null, extra: Map<String, Any?> = emptyMap()) =
        emit("state", mapOf("state" to state, "reason" to reason) + extra)

    // ------------------------------------------------------------------ plug / unplug

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            val dev: UsbDevice? = if (Build.VERSION.SDK_INT >= 33) i.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice::class.java)
            else @Suppress("DEPRECATION") i.getParcelableExtra(UsbManager.EXTRA_DEVICE)
            UsbDiag.log("broadcast ${i.action} device=${dev?.deviceName}")
            when (i.action) {
                UsbManager.ACTION_USB_DEVICE_ATTACHED -> control.execute { denied.remove(dev?.deviceName); scan(userAsked = false) }
                UsbManager.ACTION_USB_DEVICE_DETACHED -> if (dev?.deviceName == deviceName) session?.let { onGone(it) }
                ACTION_PERMISSION -> {
                    val granted = i.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
                    UsbDiag.log("permission result for ${dev?.deviceName}: granted=$granted")
                    if (dev == null) return
                    if (granted) control.execute { openAndStart(dev) }
                    else { denied.add(dev.deviceName); state("error", "permission_denied") }
                }
            }
        }
    }

    private fun registerReceiver() {
        if (receiverOn) return
        val f = IntentFilter().apply {
            addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
            addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
            addAction(ACTION_PERMISSION)
        }
        if (Build.VERSION.SDK_INT >= 33) activity.registerReceiver(receiver, f, Context.RECEIVER_NOT_EXPORTED)
        else @Suppress("UnspecifiedRegisterReceiverFlag") activity.registerReceiver(receiver, f)
        receiverOn = true
        hubStop = false
        fakeRoot?.let { startFakeHub(it) }
    }

    private fun unregisterReceiver() {
        if (!receiverOn) return
        try { activity.unregisterReceiver(receiver) } catch (_: Exception) {}
        receiverOn = false
    }

    private fun isCandidate(d: UsbDevice): Boolean {
        for (n in 0 until d.interfaceCount) {
            val i = d.getInterface(n)
            if (i.interfaceClass == UsbConstants.USB_CLASS_STILL_IMAGE) return true
            if (i.interfaceClass == UsbConstants.USB_CLASS_VENDOR_SPEC && (i.name ?: "").contains("MTP", ignoreCase = true)) return true
        }
        return false
    }

    private fun describe(d: UsbDevice) {
        UsbDiag.log("usb device ${d.deviceName} vid=${d.vendorId} (0x${Integer.toHexString(d.vendorId)}) pid=${d.productId} (0x${Integer.toHexString(d.productId)}) " +
            "class=${d.deviceClass} sub=${d.deviceSubclass} proto=${d.deviceProtocol} ifaces=${d.interfaceCount} candidate=${isCandidate(d)}")
        for (n in 0 until d.interfaceCount) {
            val i = d.getInterface(n)
            UsbDiag.log("  interface #${i.id} class=${i.interfaceClass} sub=${i.interfaceSubclass} proto=${i.interfaceProtocol} name='${i.name}' " +
                "endpoints=${(0 until i.endpointCount).joinToString { e -> i.getEndpoint(e).let { "0x${Integer.toHexString(it.address)}/type${it.type}/max${it.maxPacketSize}" } }}")
        }
    }

    /** Looks at every attached USB device; the first still-image one is the camera. Runs on the control thread. */
    private fun scan(userAsked: Boolean) {
        if (fakeRoot != null) return // the fake hub drives the attach/detach in debug emulation
        if (session != null) return
        val devs = usb.deviceList.values.toList()
        UsbDiag.log("scan: ${devs.size} usb device(s)")
        devs.forEach { describe(it) }
        val cam = devs.firstOrNull { isCandidate(it) }
        if (cam == null) {
            // after an unplug the card keeps saying so; "none" only when nothing was ever reported or the state was something else
            if (lastState["state"] != "unplugged") state("none")
            return
        }
        deviceName = cam.deviceName
        if (usb.hasPermission(cam)) { openAndStart(cam); return }
        if (cam.deviceName in denied && !userAsked) { state("error", "permission_denied"); return }
        state("permission")
        val pi = PendingIntent.getBroadcast(activity, 0, Intent(ACTION_PERMISSION).setPackage(activity.packageName),
            PendingIntent.FLAG_MUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        UsbDiag.log("requesting usb permission for ${cam.deviceName}")
        usb.requestPermission(cam, pi)
    }

    private fun openAndStart(dev: UsbDevice) {
        if (session != null) return
        val dir = inbox ?: return
        deviceName = dev.deviceName
        state("opening")
        var reason = "open_failed"
        val link = MtpCameraLink.open(usb, dev) { reason = it }
        if (link == null) { state("error", reason); return }
        beginSession(link, dir)
    }

    private fun beginSession(link: CameraLink, dir: File) {
        val i = link.info
        UsbDiag.log("device info: manufacturer='${i.manufacturer}' model='${i.model}' version='${i.version}' serial='${i.serial}'")
        UsbDiag.log("operations supported: ${i.operations.joinToString { "0x" + Integer.toHexString(it) }}")
        UsbDiag.log("events supported: ${i.events.joinToString { "0x${Integer.toHexString(it)} ${PtpCode.eventName(it)}" }}")
        val s = CamSession(link, dir, { t, d -> if (t == "gone") session?.let { g -> onGone(g) } else emit(t, d) }, postProcess = ::convertHeif, async = { reporter.execute(it) })
        val card = try {
            s.open()
        } catch (e: CameraGoneException) {
            UsbDiag.log("camera vanished while opening: ${e.message}")
            s.close(); state("unplugged"); return
        } catch (e: Exception) {
            UsbDiag.log("opening the card failed: ${e.javaClass.simpleName}: ${e.message}")
            s.close(); state("error", "card_unreadable"); return
        }
        if ((card["storages"] as List<*>).isEmpty()) {
            UsbDiag.log("camera reports no storage (no card, or locked)")
            s.close(); state("error", "no_storage"); return
        }
        session = s
        state("ready", null, mapOf(
            "device" to mapOf("manufacturer" to i.manufacturer, "model" to i.model, "serial" to i.serial),
            "card" to card,
        ))
        startWatching(s)
    }

    private fun startWatching(s: CamSession) {
        val events = Thread({ s.eventLoop() }, "usbcam-events").apply { isDaemon = true }
        val poll = Thread({
            try {
                while (!s.isClosed) {
                    Thread.sleep(s.policy.nextPollDelayMs(lastListing(s)))
                    if (s.isClosed) break
                    try { s.pollOnce() } catch (e: CameraGoneException) { onGone(s); break }
                    catch (e: Exception) { UsbDiag.log("poll failed: ${e.javaClass.simpleName}: ${e.message}") }
                }
            } catch (_: InterruptedException) {}
        }, "usbcam-poll").apply { isDaemon = true }
        watchers = listOf(events, poll)
        events.start()
        poll.start()
    }

    private fun lastListing(s: CamSession) = s.lastListingMs

    private fun onGone(s: CamSession) {
        if (session !== s) return
        session = null
        deviceName = null
        s.onUnplugged()
        watchers.forEach { it.interrupt() }
        watchers = emptyList()
        state("unplugged")
        // another camera may already be on the bus
        control.execute { scan(userAsked = false) }
    }

    private fun closeSession(why: String) {
        val s = session ?: return
        session = null
        UsbDiag.log("session closed: $why")
        s.close()
        watchers.forEach { it.interrupt() }
        watchers = emptyList()
    }

    // ------------------------------------------------------------------ emulator hub

    private fun startFakeHub(root: File) {
        UsbDiag.log("debug build with ${root.absolutePath}: emulating a camera from that folder (ATTACHED marker = plugged in)")
        Thread({
            var attached = false
            while (!hubStop) {
                val now = File(root, "ATTACHED").exists()
                if (now && !attached) {
                    attached = true
                    UsbDiag.log("fake camera plugged in")
                    control.execute {
                        val dir = inbox ?: return@execute
                        beginSession(FakeCameraLink(root, useMarker = true), dir)
                    }
                } else if (!now && attached) {
                    attached = false
                    UsbDiag.log("fake camera unplugged")
                    session?.let { onGone(it) } // already reported when the transfer thread saw it first
                }
                try { Thread.sleep(400) } catch (_: InterruptedException) { return@Thread }
            }
        }, "usbcam-fakehub").apply { isDaemon = true }.start()
    }

    // ------------------------------------------------------------------ helpers

    private fun convertHeif(f: File, o: CamObject): File {
        if (CamNames.kind(o.name) != CamKind.HEIF) return f
        val out = File(f.parentFile, ImageTranscode.outputName(f.name.substringBeforeLast('.') + ".heic"))
        val written = ImageTranscode.convert(f, out) ?: throw IllegalStateException("cannot decode ${o.name}")
        f.delete()
        return written
    }

    private fun sweepParts(dir: File) { dir.listFiles { x -> x.name.endsWith(".part") }?.forEach { it.delete() } }

    private fun acquireWake() {
        try {
            val pm = activity.getSystemService(Context.POWER_SERVICE) as PowerManager
            wake = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "dapple:usbimport").apply { setReferenceCounted(false); acquire(10 * 60 * 1000L) }
        } catch (e: Exception) { UsbDiag.log("wake lock: ${e.message}") }
    }

    private fun releaseWake() { try { wake?.takeIf { it.isHeld }?.release() } catch (_: Exception) {}; wake = null }

    private fun diagnostics(): String {
        val h = StringBuilder()
        h.append("Dapple usb diagnostics\n")
        h.append("android=${Build.VERSION.RELEASE} sdk=${Build.VERSION.SDK_INT} phone=${Build.MANUFACTURER} ${Build.MODEL}\n")
        h.append("state=${lastState["state"]} reason=${lastState["reason"]}\n")
        session?.let { h.append("watch mode=${it.policy.mode} eventsSeen=${it.policy.eventsSeen} eventsUnsupported=${it.policy.eventsUnsupported} distrusted=${it.policy.distrusted} objects=${it.totalObjects} skippedVideos=${it.skippedVideos}\n") }
        // a fresh look at the bus so a camera that never reached the app still shows up
        try { usb.deviceList.values.forEach { describe(it) } } catch (_: Exception) {}
        h.append("----\n").append(UsbDiag.dump())
        return h.toString()
    }

    companion object {
        const val ACTION_PERMISSION = "com.dapple.dapple.USB_PERMISSION"
    }
}
