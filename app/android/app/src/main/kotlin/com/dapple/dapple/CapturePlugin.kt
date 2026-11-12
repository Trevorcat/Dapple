package com.dapple.dapple

import android.Manifest
import android.app.Activity
import android.content.ContentValues
import android.content.Intent
import android.content.pm.PackageManager
import android.provider.MediaStore
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.MethodChannel
import java.io.File
import java.util.concurrent.Executors

/**
 * dapple/capture (MethodChannel): capture -> {jpg, dng?, source} | null (cancelled); saveToGallery(path, name, mime).
 * The activity forwards onActivityResult here.
 */
class CapturePlugin(private val activity: Activity, messenger: BinaryMessenger) {
    private val io = Executors.newSingleThreadExecutor()
    private var pending: MethodChannel.Result? = null

    init {
        MethodChannel(messenger, "dapple/capture").setMethodCallHandler { call, result ->
            when (call.method) {
                "capture" -> {
                    if (pending != null) { result.error("busy", null, null); return@setMethodCallHandler }
                    if (activity.checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
                        result.error("permission", null, null); return@setMethodCallHandler
                    }
                    pending = result
                    activity.startActivityForResult(Intent(activity, CaptureActivity::class.java), REQ)
                }
                "sdkInt" -> result.success(android.os.Build.VERSION.SDK_INT)
                "saveToGallery" -> {
                    val src = File(call.argument<String>("path")!!)
                    val name = call.argument<String>("name")!!
                    val mime = call.argument<String>("mime") ?: "image/jpeg"
                    io.execute {
                        try {
                            val uri = save(src, name, mime)
                            activity.runOnUiThread { result.success(uri) }
                        } catch (e: Exception) {
                            activity.runOnUiThread { result.error("save", e.message, null) }
                        }
                    }
                }
                else -> result.notImplemented()
            }
        }
    }

    fun onActivityResult(req: Int, res: Int, data: Intent?): Boolean {
        if (req != REQ) return false
        val r = pending ?: return true
        pending = null
        when (res) {
            Activity.RESULT_OK -> {
                val m = HashMap<String, Any?>()
                m["jpg"] = data?.getStringExtra(CaptureActivity.EXTRA_JPG)
                m["dng"] = data?.getStringExtra(CaptureActivity.EXTRA_DNG)
                m["source"] = data?.getStringExtra(CaptureActivity.EXTRA_SOURCE) ?: "camerax"
                r.success(m)
            }
            Activity.RESULT_CANCELED -> r.success(null)
            else -> r.error("capture", data?.getStringExtra(CaptureActivity.EXTRA_ERROR), null)
        }
        return true
    }

    /** Pictures/Dapple, same place and conventions as the Motion Photos (the ingest scan skips this folder). */
    private fun save(src: File, name: String, mime: String): String {
        if (android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.Q) return saveLegacy(src, name, mime)
        val cr = activity.contentResolver
        val values = ContentValues().apply {
            put(MediaStore.Images.Media.DISPLAY_NAME, name)
            put(MediaStore.Images.Media.MIME_TYPE, mime)
            put(MediaStore.Images.Media.RELATIVE_PATH, "Pictures/Dapple")
            put(MediaStore.Images.Media.IS_PENDING, 1)
        }
        val uri = cr.insert(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, values)!!
        try {
            cr.openOutputStream(uri)!!.use { o -> src.inputStream().use { it.copyTo(o, 1 shl 20) } }
            values.clear()
            values.put(MediaStore.Images.Media.IS_PENDING, 0)
            cr.update(uri, values, null, null)
        } catch (e: Exception) {
            try { cr.delete(uri, null, null) } catch (_: Exception) {}
            throw e
        }
        return uri.toString()
    }

    /** API 24-28 has no RELATIVE_PATH / IS_PENDING: copy into the public Pictures/Dapple folder (WRITE_EXTERNAL_STORAGE, maxSdk 28) and index it. */
    private fun saveLegacy(src: File, name: String, mime: String): String {
        @Suppress("DEPRECATION")
        val pictures = android.os.Environment.getExternalStoragePublicDirectory(android.os.Environment.DIRECTORY_PICTURES)
        val target = CaptureFiles.legacyTarget(pictures, name)
        src.inputStream().use { i -> target.outputStream().use { o -> i.copyTo(o, 1 shl 20) } }
        android.media.MediaScannerConnection.scanFile(activity, arrayOf(target.absolutePath), arrayOf(mime), null)
        return target.absolutePath
    }

    companion object { const val REQ = 7421 }
}
