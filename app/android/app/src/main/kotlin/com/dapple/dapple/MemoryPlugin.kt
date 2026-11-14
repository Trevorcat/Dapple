package com.dapple.dapple

import android.app.ActivityManager
import android.content.ComponentCallbacks2
import android.content.Context
import android.content.res.Configuration
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.MethodChannel

/**
 * Memory of the device and the system's memory-pressure callbacks for the engine (docs/engine/pipeline-architecture.md 42.9).
 *  - dapple/memory "device": {totalBytes, memoryClassMb, largeMemoryClassMb, lowRam} (ActivityManager), passed to dapple_memory_configure
 *  - dapple/memory "trim" (Kotlin -> Dart): onTrimMemory(level); Dart maps it to dapple_memory_trim + the Flutter image cache
 * Nothing here runs periodically: the callbacks come from the system only.
 */
class MemoryPlugin(private val context: Context, messenger: BinaryMessenger) : ComponentCallbacks2 {
    private val channel = MethodChannel(messenger, "dapple/memory")

    init {
        channel.setMethodCallHandler { call, result ->
            when (call.method) {
                "device" -> {
                    val am = context.getSystemService(Context.ACTIVITY_SERVICE) as ActivityManager
                    val mi = ActivityManager.MemoryInfo()
                    am.getMemoryInfo(mi)
                    result.success(
                        mapOf(
                            "totalBytes" to mi.totalMem,
                            "memoryClassMb" to am.memoryClass,
                            "largeMemoryClassMb" to am.largeMemoryClass,
                            "lowRam" to am.isLowRamDevice,
                        )
                    )
                }
                else -> result.notImplemented()
            }
        }
        context.registerComponentCallbacks(this)
    }

    fun dispose() {
        context.unregisterComponentCallbacks(this)
        channel.setMethodCallHandler(null)
    }

    override fun onTrimMemory(level: Int) {
        channel.invokeMethod("trim", level)
    }

    @Deprecated("Deprecated in Java")
    override fun onLowMemory() {
        channel.invokeMethod("trim", ComponentCallbacks2.TRIM_MEMORY_COMPLETE)
    }

    override fun onConfigurationChanged(newConfig: Configuration) {}
}
