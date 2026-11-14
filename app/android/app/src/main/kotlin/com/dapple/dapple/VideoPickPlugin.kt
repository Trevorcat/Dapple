package com.dapple.dapple

import android.app.Activity
import android.content.Intent
import android.media.MediaMetadataRetriever
import android.net.Uri
import android.provider.OpenableColumns
import android.util.Log
import io.flutter.plugin.common.MethodChannel
import java.util.concurrent.Executors

/**
 * Second source of the Motion Photo tool: one video file chosen in the system document picker (Storage Access Framework), for videos that
 * are not in the system library (a file manager, a cloud drive, a camera card). Called from the `dapple/live` handler of MainActivity:
 *   pickVideoFile -> a row shaped like a `listVideos` row {id, uri, name, durationMs, size, addedMs, width, height}, or null when cancelled.
 * The row's `uri` is the document's content:// URI; every other `dapple/live` call (info, probe, thumbs, frame, trim) and
 * MediaBridge.copyToFile already take a content URI. The read grant is made persistent where the provider allows it, so a Motion Photo
 * made from the file can be rebuilt later.
 */
class VideoPickPlugin(private val activity: Activity) {
    private var pending: MethodChannel.Result? = null
    private val io = Executors.newSingleThreadExecutor()

    fun pick(result: MethodChannel.Result) {
        if (pending != null) {
            result.error("busy", "picker already open", null)
            return
        }
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "video/*"
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION)
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

    fun onActivityResult(req: Int, res: Int, data: Intent?): Boolean {
        if (req != REQ) return false
        val cb = pending ?: return true
        pending = null
        val uri = if (res == Activity.RESULT_OK) data?.data else null
        if (uri == null) {
            cb.success(null)
            return true
        }
        try {
            activity.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
        } catch (e: Exception) {
            Log.i(TAG, "no persistable grant for $uri: ${e.message}")
        }
        io.execute {
            val row = try {
                describe(uri)
            } catch (e: Exception) {
                Log.w(TAG, "video file $uri unreadable: ${e.message}")
                null
            }
            activity.runOnUiThread { cb.success(row) }
        }
        return true
    }

    private fun describe(u: Uri): Map<String, Any?>? {
        var name: String? = null
        var size = 0L
        activity.contentResolver.query(u, arrayOf(OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE), null, null, null)?.use { c ->
            if (c.moveToFirst()) {
                name = c.getString(0)
                size = if (c.isNull(1)) 0L else c.getLong(1)
            }
        }
        val r = MediaMetadataRetriever()
        try {
            r.setDataSource(activity, u)
            fun s(k: Int) = r.extractMetadata(k)
            val dur = s(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toLongOrNull() ?: 0L
            if (dur <= 0L) return null // not a playable video
            // the picture size as shown (a rotated clip swaps the sides), like the MediaStore row
            val w = s(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH)?.toIntOrNull() ?: 0
            val h = s(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT)?.toIntOrNull() ?: 0
            val rot = s(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION)?.toIntOrNull() ?: 0
            val swap = rot == 90 || rot == 270
            return mapOf(
                "id" to "doc_${Math.abs(u.toString().hashCode())}",
                "uri" to u.toString(),
                "name" to (name ?: (u.lastPathSegment ?: "video")),
                "durationMs" to dur,
                "size" to size,
                "addedMs" to System.currentTimeMillis(),
                "width" to if (swap) h else w,
                "height" to if (swap) w else h,
            )
        } finally {
            r.release()
        }
    }

    companion object {
        const val REQ = 7434
        private const val TAG = "DappleLive"
    }
}
