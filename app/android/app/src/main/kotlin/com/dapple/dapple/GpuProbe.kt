package com.dapple.dapple

import android.content.Context
import android.os.Build
import java.io.File

/**
 * Starts the native GPU probe at app start (docs/engine/pipeline-architecture.md 24): the engine creates a Vulkan device (else GLES 3.1), runs
 * its self-test against the CPU reference and enables the GPU executor only when it passed. No user switch; the developer kill switch is
 * `setprop debug.dapple.gpu 0`. Runs on its own low-priority thread, never on the UI thread, and never blocks the first frame: frames are CPU frames until
 * the probe is done. Verdicts are cached in noBackupFilesDir/gpu keyed by Build.FINGERPRINT + app version + driver version; a "self-test in progress"
 * marker next to them marks an API as failed when the process died or hung inside the test.
 */
internal object GpuProbe {
    private var started = false

    /** Cache key part for the app build: versionName+versionCode, so a new build re-runs the self-test. */
    fun appVersionString(name: String?, code: Long): String = "${name ?: "?"}+$code"

    @Synchronized
    fun start(ctx: Context) {
        if (started) return
        started = true
        val app = ctx.applicationContext
        Thread({
            try {
                if (!DappleViewNative.loaded) return@Thread
                val info = app.packageManager.getPackageInfo(app.packageName, 0)
                @Suppress("DEPRECATION")
                val code = if (Build.VERSION.SDK_INT >= 28) info.longVersionCode else info.versionCode.toLong()
                val dir = File(app.noBackupFilesDir, "gpu").absolutePath
                DappleViewNative.startGpu(dir, appVersionString(info.versionName, code), Build.FINGERPRINT)
            } catch (_: Throwable) {
                // the app keeps working on the CPU
            }
        }, "dapple-gpu-probe").apply {
            isDaemon = true
            priority = Thread.MIN_PRIORITY
            start()
        }
    }
}
