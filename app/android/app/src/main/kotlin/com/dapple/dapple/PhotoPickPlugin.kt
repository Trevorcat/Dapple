package com.dapple.dapple

import android.app.Activity
import android.content.Intent
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.provider.OpenableColumns
import android.util.Log
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.MethodChannel
import java.io.File
import java.util.concurrent.Executors

/**
 * dapple/photos (MethodChannel), same method name and result rows as iOS (DappleEnginePlugin.swift):
 *   pick {dest, known: [keys], source: "photos"|"files"} -> [{key, name, dup, path?, size?, error?}] | []  (cancelled)
 * "photos" = the system photo picker (no permission; the contract falls back to the document picker where it does not exist),
 * "files" = the document picker (RAW types the photo picker does not list). Originals are copied into `dest`.
 * Rows with dup=true are already in the library (key in [known]): nothing is copied.
 */
class PhotoPickPlugin(private val activity: Activity, messenger: BinaryMessenger) {
    private var pending: MethodChannel.Result? = null
    private var dest: File? = null
    private var known: Set<String> = emptySet()
    private val io = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())

    init {
        MethodChannel(messenger, "dapple/photos").setMethodCallHandler { call, result ->
            if (call.method != "pick") {
                result.notImplemented()
                return@setMethodCallHandler
            }
            if (pending != null) {
                result.error("busy", "picker already open", null)
                return@setMethodCallHandler
            }
            dest = File(call.argument<String>("dest") ?: activity.cacheDir.absolutePath)
            known = (call.argument<List<String>>("known") ?: emptyList()).toSet()
            val intent = if (call.argument<String>("source") == "files") {
                Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
                    addCategory(Intent.CATEGORY_OPENABLE)
                    type = "*/*"
                    putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("image/*", "image/x-adobe-dng", "image/x-sony-arw", "image/x-fuji-raf", "application/octet-stream"))
                    putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true)
                }
            } else {
                ActivityResultContracts.PickMultipleVisualMedia()
                    .createIntent(activity, PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly))
            }
            pending = result
            try {
                @Suppress("DEPRECATION")
                activity.startActivityForResult(intent, REQ)
            } catch (e: Exception) {
                pending = null
                result.error("launch", e.message, null)
            }
        }
    }

    fun onActivityResult(req: Int, res: Int, data: Intent?): Boolean {
        if (req != REQ) return false
        val cb = pending ?: return true
        pending = null
        val uris = if (res == Activity.RESULT_OK && data != null) urisOf(data) else emptyList()
        if (uris.isEmpty()) {
            cb.success(emptyList<Map<String, Any?>>())
            return true
        }
        val out = dest!!
        val knownKeys = known
        io.execute {
            val rows = ArrayList<Map<String, Any?>>()
            out.mkdirs()
            val now = System.currentTimeMillis()
            for ((n, u) in uris.withIndex()) {
                val (name, size) = describe(u)
                val key = keyFor(u.toString(), name, size)
                if (key in knownKeys) {
                    rows.add(mapOf("key" to key, "name" to name, "dup" to true))
                    continue
                }
                if (!supported(name)) {
                    rows.add(mapOf("key" to key, "name" to name, "dup" to false, "error" to "unsupported"))
                    continue
                }
                val convert = ImageTranscode.needsTranscode(name)
                val f = File(out, "${now + n}_${if (convert) ImageTranscode.outputName(name) else name}")
                val raw = if (convert) File(activity.cacheDir, "pick_${now + n}_$name") else f
                try {
                    activity.contentResolver.openInputStream(u)!!.use { i -> raw.outputStream().use { o -> i.copyTo(o, 1 shl 20) } }
                    var written = f
                    if (convert) {
                        val w = ImageTranscode.convert(raw, f)
                        raw.delete()
                        if (w == null) throw IllegalStateException("cannot decode $name")
                        written = w
                    }
                    Log.i("DappleIngest", "picked $name: ${written.length()} bytes${if (convert) ", converted to ${written.extension}" else ""}")
                    rows.add(mapOf("key" to key, "name" to written.name.substringAfter('_'), "dup" to false, "path" to written.absolutePath, "size" to written.length()))
                } catch (e: Exception) {
                    f.delete()
                    raw.delete()
                    Log.w("DappleIngest", "pick of $name failed: ${e.message}")
                    rows.add(mapOf("key" to key, "name" to name, "dup" to false, "error" to "copy"))
                }
            }
            main.post { cb.success(rows) }
        }
        return true
    }

    private fun urisOf(data: Intent): List<Uri> {
        val out = ArrayList<Uri>()
        data.clipData?.let { c -> for (i in 0 until c.itemCount) c.getItemAt(i).uri?.let { out.add(it) } }
        if (out.isEmpty()) data.data?.let { out.add(it) }
        return out
    }

    private fun describe(u: Uri): Pair<String, Long> {
        var name: String? = null
        var size = 0L
        try {
            activity.contentResolver.query(u, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { c ->
                if (c.moveToFirst()) {
                    name = c.getString(0)
                    size = if (c.isNull(1)) 0L else c.getLong(1)
                }
            }
        } catch (_: Exception) {
        }
        return Pair(name ?: (u.lastPathSegment ?: "photo"), size)
    }

    companion object {
        const val REQ = 7433
        private val exts = listOf("arw", "raf", "dng", "jpg", "jpeg", "heic", "heif", "png", "webp")

        fun supported(name: String) = name.substringAfterLast('.', "").lowercase() in exts

        /**
         * Stable identity of a picked file. A MediaStore item (photo picker, or the media document provider) is "ms:<_ID>", the very id
         * the automatic scan gives it, so the same photo is never added twice; anything else is "imp:<name>|<size>".
         */
        fun keyFor(uri: String, name: String, size: Long): String {
            val last = uri.substringBefore('?').substringAfterLast('/').substringAfterLast(':')
            val isMedia = uri.contains("/media/picker") || uri.contains("com.android.providers.media") || uri.startsWith("content://media/")
            if (isMedia && last.isNotEmpty() && last.all { it.isDigit() }) return "ms:$last"
            return "imp:$name|$size"
        }
    }
}
