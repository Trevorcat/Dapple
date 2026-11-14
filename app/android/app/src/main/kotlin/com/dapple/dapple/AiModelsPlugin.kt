package com.dapple.dapple

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.MethodChannel
import java.io.File
import java.util.concurrent.Executors

/**
 * dapple/ai (MethodChannel): `prepare` -> absolute path of the directory that holds the AI model files (person_parts.tflite, face_detector.tflite,
 * face_landmarks.tflite, pose_detector.tflite, pose_landmarks.tflite, tap_encoder.tflite, tap_decoder.tflite, depth.ort), or null when the APK carries none (built with -Pdapple.ai=false).
 *
 * The models are bundled as assets under ai-models/ (no server, no Play services: docs/legal/ai-models.md). The engine reads them with ordinary file
 * I/O, so they are copied once to filesDir/ai-models; a file is copied again only when its size differs from the asset (an app update with a new
 * model), and a half-copied file is never visible (written to .tmp, then renamed). Runs on a background thread (about 150 MB the first time).
 */
class AiModelsPlugin(private val context: Context, messenger: BinaryMessenger) {
    private val io = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())

    init {
        MethodChannel(messenger, "dapple/ai").setMethodCallHandler { call, result ->
            if (call.method != "prepare") {
                result.notImplemented()
                return@setMethodCallHandler
            }
            io.execute {
                val dir = try {
                    prepare()
                } catch (t: Throwable) {
                    Log.w(TAG, "model copy failed: $t")
                    null
                }
                main.post { result.success(dir) }
            }
        }
    }

    private fun prepare(): String? {
        val names = context.assets.list(ASSET_DIR) ?: return null
        if (names.isEmpty()) return null
        val dir = File(context.filesDir, "ai-models")
        dir.mkdirs()
        for (name in names) {
            val dst = File(dir, name)
            val size = context.assets.openFd("$ASSET_DIR/$name").use { it.length }  // assets are stored uncompressed (noCompress in build.gradle.kts)
            if (dst.isFile && dst.length() == size) continue
            val tmp = File(dir, "$name.tmp")
            context.assets.open("$ASSET_DIR/$name").use { input -> tmp.outputStream().use { input.copyTo(it, 1 shl 20) } }
            if (!tmp.renameTo(dst)) throw java.io.IOException("rename $name")
            Log.i(TAG, "model $name copied ($size bytes)")
        }
        // files no longer in the APK are removed
        dir.listFiles()?.forEach { if (it.name !in names && !it.name.endsWith(".tmp")) it.delete() }
        return dir.absolutePath
    }

    companion object {
        private const val TAG = "DappleAi"
        private const val ASSET_DIR = "ai-models"
    }
}
