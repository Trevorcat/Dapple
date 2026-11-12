package com.dapple.dapple

import android.content.ComponentCallbacks2
import android.content.Context
import android.content.res.Configuration
import android.util.Log
import android.view.Surface
import io.flutter.plugin.common.BinaryMessenger
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.view.TextureRegistry

/**
 * Zero-copy display for the editor (docs/engine/present.md). One entry = one Flutter texture backed by a
 * SurfaceProducer; its Surface is handed to the native view (a Vulkan swapchain) so frames are drawn straight onto the
 * texture without the pixels visiting Dart.
 *
 * Channel "dapple/view":
 *   available               -> Boolean       libdapple.so with the view API is loadable
 *   create {w,h}            -> {id, ok}      texture entry (id for Texture(textureId:))
 *   bind {id, view}         -> {ok, error}   attach the entry's current Surface to the native view (address from FFI)
 *   resize {id,w,h}         -> {ok, error}   SurfaceProducer.setSize + re-attach the new Surface
 *   unbind {id}             -> null          detach (the native view keeps its last snapshot)
 *   dispose {id}            -> null
 * Surface lifecycle (SurfaceProducer.Callback: cleanup when Flutter reclaims the surface in the background, available
 * again on resume) and onTrimMemory reach the native view synchronously, on this (platform) thread.
 */
class DappleViewPlugin(private val appContext: Context, messenger: BinaryMessenger, private val textures: TextureRegistry) {
    private class Entry(val producer: TextureRegistry.SurfaceProducer) {
        var view: Long = 0
        var attached: Surface? = null
        var aw = 0
        var ah = 0
    }

    private val entries = HashMap<Long, Entry>()
    private val channel = MethodChannel(messenger, "dapple/view")

    private val trim = object : ComponentCallbacks2 {
        override fun onTrimMemory(level: Int) {
            // 0 transient buffers, 1 + swapchain, 2 + device (core/include/dapple/dapple_view.h)
            val l = when {
                level >= ComponentCallbacks2.TRIM_MEMORY_BACKGROUND -> 2
                level >= ComponentCallbacks2.TRIM_MEMORY_UI_HIDDEN -> 1
                level >= ComponentCallbacks2.TRIM_MEMORY_RUNNING_CRITICAL -> 1
                level >= ComponentCallbacks2.TRIM_MEMORY_RUNNING_LOW -> 0
                else -> return
            }
            for (e in entries.values) if (e.view != 0L) DappleViewNative.trim(e.view, l)
        }

        override fun onConfigurationChanged(newConfig: Configuration) {}

        override fun onLowMemory() {
            for (e in entries.values) if (e.view != 0L) DappleViewNative.trim(e.view, 2)
        }
    }

    init {
        channel.setMethodCallHandler(::onCall)
        appContext.registerComponentCallbacks(trim)
    }

    fun dispose() {
        appContext.unregisterComponentCallbacks(trim)
        channel.setMethodCallHandler(null)
        for (e in entries.values) release(e)
        entries.clear()
    }

    private fun release(e: Entry) {
        e.producer.setCallback(null)
        if (e.view != 0L) DappleViewNative.detach(e.view)
        e.view = 0
        e.attached = null
        e.producer.release()
    }

    /** Hands the producer's current Surface to the native view; skips when nothing changed. Returns an error text or null. */
    private fun attach(e: Entry, force: Boolean): String? {
        if (e.view == 0L) return null
        val s = e.producer.surface
        if (s == null || !s.isValid) return "surface not valid"
        val w = e.producer.width
        val h = e.producer.height
        if (!force && s === e.attached && w == e.aw && h == e.ah) return null
        val rc = DappleViewNative.attach(e.view, s, w, h)
        if (rc != 0) {
            val err = DappleViewNative.lastError()
            Log.w(TAG, "attach failed: $err")
            return err
        }
        e.attached = s
        e.aw = w
        e.ah = h
        return null
    }

    private fun entry(call: MethodCall): Entry? = entries[(call.argument<Number>("id") ?: -1).toLong()]

    private fun onCall(call: MethodCall, result: MethodChannel.Result) {
        try {
            when (call.method) {
                "available" -> result.success(DappleViewNative.loaded)
                "create" -> {
                    val w = (call.argument<Int>("w") ?: 1).coerceAtLeast(1)
                    val h = (call.argument<Int>("h") ?: 1).coerceAtLeast(1)
                    val p = textures.createSurfaceProducer()
                    p.setSize(w, h)
                    val e = Entry(p)
                    p.setCallback(object : TextureRegistry.SurfaceProducer.Callback {
                        override fun onSurfaceAvailable() {
                            // resume after a cleanup: a new Surface object
                            attach(e, true)
                        }

                        override fun onSurfaceCleanup() {
                            // about to become invalid: the native side must stop drawing now (synchronously)
                            if (e.view != 0L) DappleViewNative.detach(e.view)
                            e.attached = null
                        }
                    })
                    entries[p.id()] = e
                    result.success(mapOf("id" to p.id(), "ok" to true))
                }
                "bind" -> {
                    val e = entry(call)
                    if (e == null) {
                        result.error("id", "unknown texture", null)
                        return
                    }
                    e.view = (call.argument<Number>("view") ?: 0).toLong()
                    val err = attach(e, true)
                    result.success(mapOf("ok" to (err == null), "error" to (err ?: "")))
                }
                "resize" -> {
                    val e = entry(call)
                    if (e == null) {
                        result.error("id", "unknown texture", null)
                        return
                    }
                    val w = (call.argument<Int>("w") ?: 1).coerceAtLeast(1)
                    val h = (call.argument<Int>("h") ?: 1).coerceAtLeast(1)
                    var err: String? = null
                    if (w != e.producer.width || h != e.producer.height) {
                        e.producer.setSize(w, h)
                        err = attach(e, true)  // the producer hands out a new Surface for the new size
                    }
                    result.success(mapOf("ok" to (err == null), "error" to (err ?: "")))
                }
                "unbind" -> {
                    entry(call)?.let {
                        if (it.view != 0L) DappleViewNative.detach(it.view)
                        it.view = 0
                        it.attached = null
                    }
                    result.success(null)
                }
                "dispose" -> {
                    entries.remove((call.argument<Number>("id") ?: -1).toLong())?.let { release(it) }
                    result.success(null)
                }
                else -> result.notImplemented()
            }
        } catch (t: Throwable) {
            Log.e(TAG, "view channel ${call.method}", t)
            result.error("view", t.message, null)
        }
    }

    private companion object {
        const val TAG = "dapple_view"
    }
}
