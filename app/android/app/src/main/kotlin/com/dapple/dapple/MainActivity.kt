package com.dapple.dapple

import android.content.ContentResolver
import android.content.ContentUris
import android.content.ContentValues
import android.content.Intent
import android.database.ContentObserver
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.MediaStore
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel
import java.io.File
import java.util.concurrent.Executors

/**
 * Bridges MediaStore to Dart:
 *  - dapple/media (MethodChannel): queryNew, copyToFile, saveToGallery
 *  - dapple/media_events (EventChannel): ping whenever MediaStore changes
 */
class MainActivity : FlutterActivity() {
    private val io = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())
    private var observer: ContentObserver? = null
    private val liveIo = Executors.newFixedThreadPool(2)
    private val live by lazy { LiveBridge(applicationContext) }
    private var viewPlugin: DappleViewPlugin? = null
    private var capturePlugin: CapturePlugin? = null
    private var pickPlugin: PhotoPickPlugin? = null
    private var videoPick: VideoPickPlugin? = null
    private var usbPlugin: UsbCameraPlugin? = null
    private var memoryPlugin: MemoryPlugin? = null

    private val extensions = listOf("arw", "raf", "dng", "jpg", "jpeg")

    // Only camera-ish locations (DCIM / Pictures) are ingested; chat apps and screenshots are skipped.
    private val excluded = listOf("%/Screenshots/%", "%/WeiXin/%", "%/WeChat/%", "%/Tencent/%", "%/MicroMsg/%", "%/Download/%")

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        if (capturePlugin?.onActivityResult(requestCode, resultCode, data) == true) return
        if (pickPlugin?.onActivityResult(requestCode, resultCode, data) == true) return
        if (videoPick?.onActivityResult(requestCode, resultCode, data) == true) return
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
    }

    override fun onDestroy() {
        usbPlugin?.dispose()
        memoryPlugin?.dispose()
        super.onDestroy()
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        val messenger = flutterEngine.dartExecutor.binaryMessenger

        // GPU executor: self-test on a background thread, on by default when it passes (GpuProbe.kt); frames are CPU frames until then
        GpuProbe.start(applicationContext)

        // Zero-copy display (stage d): SurfaceProducer textures drawn by the native view (DappleViewPlugin.kt)
        viewPlugin = DappleViewPlugin(applicationContext, messenger, flutterEngine.renderer)

        // In-app camera capture (CaptureActivity.kt): testing convenience
        capturePlugin = CapturePlugin(this, messenger)

        // "从相册选择" / "从文件选择": system photo picker and document picker (PhotoPickPlugin.kt), same channel and result rows as iOS
        pickPlugin = PhotoPickPlugin(this, messenger)
        // a video file from the document picker, second source of the Motion Photo tool (VideoPickPlugin.kt, call dapple/live pickVideoFile)
        videoPick = VideoPickPlugin(this)

        // Camera on the USB-C cable: android.mtp.MtpDevice (UsbCameraPlugin.kt), channels dapple/usbcam + dapple/usbcam_events
        usbPlugin = UsbCameraPlugin(this, messenger)

        // AI masks: the bundled model files are copied to app storage on first use (AiModelsPlugin.kt), channel dapple/ai
        AiModelsPlugin(applicationContext, messenger)

        // Memory class of the device + onTrimMemory for the engine's caches (MemoryPlugin.kt), channel dapple/memory
        memoryPlugin?.dispose()
        memoryPlugin = MemoryPlugin(applicationContext, messenger)

        // Editor landmark haptics (requirements 4.15.9): View.performHapticFeedback with the
        // finest constant the OS offers. Numeric values so this compiles against any compileSdk.
        MethodChannel(messenger, "dapple/haptics").setMethodCallHandler { call, result ->
            if (call.method != "perform") {
                result.notImplemented()
                return@setMethodCallHandler
            }
            val sdk = Build.VERSION.SDK_INT
            val constant = when (call.arguments as? String) {
                "autoSnap" -> if (sdk >= 34) 27 /* SEGMENT_TICK */ else 4 /* CLOCK_TICK */
                "detent" -> if (sdk >= 34) 26 /* SEGMENT_FREQUENT_TICK */ else 4
                "endStop" -> if (sdk >= 30) 16 /* CONFIRM */ else 0 /* LONG_PRESS */
                else -> -1
            }
            if (constant >= 0) window.decorView.performHapticFeedback(constant)
            result.success(null)
        }

        MethodChannel(messenger, "dapple/media").setMethodCallHandler { call, result ->
            when (call.method) {
                "queryNew" -> {
                    val limit = call.argument<Int>("limit") ?: 100
                    val sinceSec = (call.argument<Number>("sinceSec") ?: 0).toLong()
                    io.execute {
                        try {
                            val rows = queryFiles(limit, sinceSec)
                            main.post { result.success(rows) }
                        } catch (e: Exception) {
                            main.post { result.error("query", e.message, null) }
                        }
                    }
                }
                "copyToFile" -> {
                    val uri = Uri.parse(call.argument<String>("uri")!!)
                    val dest = File(call.argument<String>("dest")!!)
                    io.execute {
                        try {
                            dest.parentFile?.mkdirs()
                            contentResolver.openInputStream(uri)!!.use { i ->
                                dest.outputStream().use { o -> i.copyTo(o, 1 shl 20) }
                            }
                            main.post { result.success(dest.absolutePath) }
                        } catch (e: Exception) {
                            dest.delete()
                            main.post { result.error("copy", e.message, null) }
                        }
                    }
                }
                "saveToGallery" -> {
                    val src = File(call.argument<String>("path")!!)
                    val name = call.argument<String>("name")!!
                    io.execute {
                        try {
                            if (call.argument<Boolean>("replace") == true) removeOwnGalleryItems(name)
                            val uri = saveJpegToGallery(src, name)
                            main.post { result.success(uri) }
                        } catch (e: Exception) {
                            main.post { result.error("save", e.message, null) }
                        }
                    }
                }
                "toPng16" -> {
                    // HEIC / WebP file -> 16-bit PNG in its own colour space (ImageTranscode.kt; an sRGB JPEG before API 28), for files that did not come through the pick channel
                    val src = File(call.argument<String>("src")!!)
                    val dest = File(call.argument<String>("dest")!!)
                    io.execute {
                        val written = ImageTranscode.convert(src, dest)
                        main.post { if (written != null) result.success(written.absolutePath) else result.error("convert", "cannot convert ${src.name}", null) }
                    }
                }
                "openApp" -> {
                    val pkg = call.argument<String>("package")
                    val intent = if (pkg == null) null else packageManager.getLaunchIntentForPackage(pkg)
                    if (intent == null) {
                        result.success(false)
                    } else {
                        try {
                            intent.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
                            startActivity(intent)
                            result.success(true)
                        } catch (e: Exception) {
                            result.success(false)
                        }
                    }
                }
                else -> result.notImplemented()
            }
        }

        // one-line ingest outcomes from Dart into logcat (tag DappleIngest), also in release builds
        MethodChannel(messenger, "dapple/log").setMethodCallHandler { call, result ->
            if (call.method == "line") android.util.Log.i("DappleIngest", call.arguments as? String ?: "")
            result.success(null)
        }

        MethodChannel(messenger, "dapple/live").setMethodCallHandler { call, result ->
            fun <T> run(f: () -> T) = liveIo.execute {
                try {
                    val v = f()
                    main.post { result.success(v) }
                } catch (e: Exception) {
                    main.post { result.error(if (e is UnsupportedVideo) "unsupported" else "live", e.message ?: e.toString(), null) }
                }
            }
            when (call.method) {
                "pickVideoFile" -> videoPick!!.pick(result)
                "listVideos" -> { val n = call.argument<Int>("limit") ?: 50; run { live.listVideos(n) } }
                "info" -> { val u = call.argument<String>("uri")!!; run { live.info(u) } }
                "probe" -> { val u = call.argument<String>("uri")!!; run { live.probe(u) } }
                "capabilities" -> run { live.capabilities() }
                "codecReport" -> { val r = call.argument<Boolean>("run") ?: false; val f = call.argument<Boolean>("force") ?: false; run { live.codecReport(r, f) } }
                "findVideo" -> { val n = call.argument<String>("stem")!!; run { live.findVideo(n) } }
                "probeEmbedded" -> { val u = call.argument<String>("path")!!; run { live.probeEmbedded(u) } }
                "thumbs" -> {
                    val u = call.argument<String>("uri")!!
                    val t = call.argument<List<Number>>("timesMs")!!.map { it.toLong() }
                    val edge = call.argument<Int>("maxEdge") ?: 160
                    val dir = call.argument<String>("destDir")!!
                    val prefix = call.argument<String>("prefix") ?: "t"
                    val src = call.argument<Int>("source") ?: 0
                    run { live.thumbs(u, t, edge, dir, prefix, src) }
                }
                "frame" -> {
                    val u = call.argument<String>("uri")!!
                    val t = (call.argument<Number>("timeMs") ?: 0).toLong()
                    val d = call.argument<String>("dest")!!
                    val edge = call.argument<Int>("maxEdge") ?: 0
                    val q = call.argument<Int>("quality") ?: 92
                    val src = call.argument<Int>("source") ?: 0
                    val compat = call.argument<Boolean>("compat") ?: false
                    run { live.frameToFile(u, t, d, edge, q, src, compat) }
                }
                "coverHdr" -> {
                    val u = call.argument<String>("uri")!!
                    val t = (call.argument<Number>("timeMs") ?: 0).toLong()
                    val src = call.argument<Int>("source") ?: 0
                    val edit = EditSpec.fromMap(call.argument<Map<String, Any?>>("edit")) ?: throw IllegalArgumentException("edit missing")
                    val sdr = call.argument<String>("sdr")!!
                    val out = call.argument<String>("out")!!
                    val force = call.argument<Boolean>("force") ?: false
                    run { live.coverHdr(u, t, src, edit, sdr, out, force) }
                }
                "trim" -> {
                    val u = call.argument<String>("uri")!!
                    val s0 = (call.argument<Number>("startMs") ?: 0).toLong()
                    val dur = (call.argument<Number>("durationMs") ?: 3000).toLong()
                    val d = call.argument<String>("dest")!!
                    val mute = call.argument<Boolean>("mute") ?: false
                    val src = call.argument<Int>("source") ?: 0
                    val compat = call.argument<Boolean>("compat") ?: false
                    val film = call.argument<Boolean>("film") ?: false
                    val edit = EditSpec.fromMap(call.argument<Map<String, Any?>>("edit"))
                    run { live.trim(u, s0, dur, d, mute, src, compat, film, edit) }
                }
                else -> result.notImplemented()
            }
        }

        MethodChannel(messenger, "dapple/ftp").setMethodCallHandler { call, result ->
            when (call.method) {
                "start" -> {
                    val i = Intent(this, FtpService::class.java)
                        .putExtra("port", call.argument<Int>("port") ?: 2121)
                        .putExtra("user", call.argument<String>("user") ?: "dapple")
                        .putExtra("pass", call.argument<String>("pass") ?: "")
                        .putExtra("root", ftpRoot().path)
                    try {
                        ftpRoot().mkdirs()
                        startForegroundService(i)
                        result.success(true)
                    } catch (e: Exception) {
                        result.error("ftp", e.message ?: e.toString(), null)
                    }
                }
                "stop" -> {
                    stopService(Intent(this, FtpService::class.java))
                    result.success(true)
                }
                "status" -> result.success(
                    mapOf(
                        "running" to FtpBus.running, "clients" to FtpBus.clients, "port" to FtpBus.port,
                        "error" to FtpBus.lastError, "root" to ftpRoot().path, "addresses" to FtpBus.addresses(),
                    )
                )
                "listInbox" -> {
                    val files = (ftpRoot().walkTopDown().filter { it.isFile && !it.name.startsWith(".") }.toList())
                        .sortedBy { it.lastModified() }
                        .map { mapOf("name" to it.name, "path" to it.path, "size" to it.length(), "modifiedMs" to it.lastModified()) }
                    result.success(files)
                }
                else -> result.notImplemented()
            }
        }

        EventChannel(messenger, "dapple/ftp_events").setStreamHandler(object : EventChannel.StreamHandler {
            override fun onListen(args: Any?, sink: EventChannel.EventSink) {
                FtpBus.sink = object : FtpBus.Sink { override fun send(event: Map<String, Any?>) { sink.success(event) } }
            }
            override fun onCancel(args: Any?) { FtpBus.sink = null }
        })

        EventChannel(messenger, "dapple/media_events").setStreamHandler(object : EventChannel.StreamHandler {
            override fun onListen(args: Any?, sink: EventChannel.EventSink) {
                val obs = object : ContentObserver(main) {
                    override fun onChange(selfChange: Boolean, uri: Uri?) {
                        sink.success(uri?.toString() ?: "")
                    }
                }
                observer = obs
                contentResolver.registerContentObserver(MediaStore.Files.getContentUri("external"), true, obs)
                contentResolver.registerContentObserver(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, true, obs)
            }

            override fun onCancel(args: Any?) {
                observer?.let { contentResolver.unregisterContentObserver(it) }
                observer = null
            }
        })
    }

    private fun ftpRoot() = File(filesDir, "ftp_inbox")

    private fun queryFiles(limit: Int, sinceSec: Long): List<Map<String, Any?>> {
        val base = MediaStore.Files.getContentUri("external")
        @Suppress("DEPRECATION")
        val dataCol = MediaStore.MediaColumns.DATA
        val proj = arrayOf(
            MediaStore.MediaColumns._ID, MediaStore.MediaColumns.DISPLAY_NAME, MediaStore.MediaColumns.SIZE,
            MediaStore.MediaColumns.DATE_ADDED, MediaStore.MediaColumns.MIME_TYPE, dataCol,
        )
        val like = extensions.joinToString(" OR ") { "${MediaStore.MediaColumns.DISPLAY_NAME} LIKE ?" }
        val sel = "($like) AND ${MediaStore.MediaColumns.SIZE} > 0 AND $dataCol NOT LIKE ? " +
            "AND ${MediaStore.MediaColumns.DATE_ADDED} >= ? " +
            "AND ($dataCol LIKE ? OR $dataCol LIKE ?) " +
            excluded.joinToString(" ") { "AND $dataCol NOT LIKE ?" }
        val args = (extensions.map { "%.$it" } + listOf("%/Pictures/Dapple/%", sinceSec.toString(), "%/DCIM/%", "%/Pictures/%") +
            excluded).toTypedArray()
        val q = Bundle().apply {
            putString(ContentResolver.QUERY_ARG_SQL_SELECTION, sel)
            putStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS, args)
            putString(ContentResolver.QUERY_ARG_SQL_SORT_ORDER, "${MediaStore.MediaColumns.DATE_ADDED} ASC")
            putInt(ContentResolver.QUERY_ARG_LIMIT, limit)
        }
        val out = ArrayList<Map<String, Any?>>()
        contentResolver.query(base, proj, q, null)?.use { c ->
            while (c.moveToNext()) {
                val id = c.getLong(0)
                out.add(
                    mapOf(
                        "id" to id,
                        "uri" to ContentUris.withAppendedId(base, id).toString(),
                        "name" to c.getString(1),
                        "size" to c.getLong(2),
                        "addedMs" to c.getLong(3) * 1000L,
                        "mime" to c.getString(4),
                        "path" to c.getString(5),
                    )
                )
            }
        }
        return out
    }

    /** Deletes this app's earlier Pictures/Dapple items called [name] (a regenerated Motion Photo replaces them). */
    private fun removeOwnGalleryItems(name: String) {
        try {
            val n = contentResolver.delete(
                MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
                "${MediaStore.Images.Media.DISPLAY_NAME} = ? AND ${MediaStore.Images.Media.RELATIVE_PATH} LIKE ?",
                arrayOf(name, "Pictures/Dapple%"),
            )
            android.util.Log.i("DappleLive", "replaced $n gallery item(s) named $name")
        } catch (e: Exception) {
            android.util.Log.w("DappleLive", "could not remove old gallery item $name: $e")
        }
    }

    private fun saveJpegToGallery(src: File, name: String): String {
        // minSdk is 29, so scoped storage (MediaStore insert) is always available.
        val values = ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, name)
            put(MediaStore.Images.Media.MIME_TYPE, "image/jpeg")
            put(MediaStore.Images.Media.RELATIVE_PATH, "Pictures/Dapple")
            put(MediaStore.Images.Media.IS_PENDING, 1)
        }
        val uri = contentResolver.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)!!
        try {
            contentResolver.openOutputStream(uri)!!.use { o -> src.inputStream().use { it.copyTo(o, 1 shl 20) } }
            values.clear()
            values.put(MediaStore.Images.Media.IS_PENDING, 0)
            contentResolver.update(uri, values, null, null)
        } catch (e: Exception) {
            try { contentResolver.delete(uri, null, null) } catch (_: Exception) {}
            throw e
        }
        return uri.toString()
    }
}
