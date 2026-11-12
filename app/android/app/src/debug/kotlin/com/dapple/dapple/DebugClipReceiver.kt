package com.dapple.dapple

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.util.Log

/**
 * Debug builds only (src/debug): runs [LiveBridge.trim] without the UI, so the edited-clip path (docs/engine/pipeline-architecture.md section 30) can be
 * exercised on an emulator or a device from adb:
 *   adb -s <serial> shell am broadcast -n com.dapple.dapple/.DebugClipReceiver -a com.dapple.dapple.DEBUG_CLIP \
 *     --es uri /sdcard/Download/clip.mp4 --es dest /sdcard/Download/out.mp4 --el startMs 0 --el durMs 1500 \
 *     [--es paramsFile /sdcard/Download/p.json (EditParams JSON, e.g. {"film":"kodak_portra_400"}; --es params takes it inline) --es pack /sdcard/Download/dapple.dfp] [--ez compat true] [--ef aspect 1.7778]
 * `--ez probe true` instead runs the codec self-tests and the Surface-input mechanism probe (8-bit encoders) and logs them.
 * The result (or the failure) is logged under the tag DappleDebugClip.
 */
class DebugClipReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, i: Intent) {
        // jobs longer than the broadcast window (a 3 s film clip on an emulator CPU) must not hold the broadcast: it is finished at once, the thread keeps working
        goAsync().finish()
        val app = ctx.applicationContext
        Thread {
            val tag = "DappleDebugClip"
            try {
                if (i.getBooleanExtra("probe", false)) {
                    // codec diagnostics without the UI: all self-tests, the Main10 encode rows, and the Surface-input mechanism on every 8-bit encoder
                    val mgr = CodecManager.get(app)
                    mgr.runAll(true)
                    val rep = mgr.report()
                    Log.i(tag, "main10Encode=" + rep.getJSONArray("main10Encode"))
                    Log.i(tag, "selfTests=" + rep.getJSONArray("selfTests"))
                    for (c in mgr.caps.filter { it.encoder && it.mime == Mime.AVC && it.hasSurface }) {
                        val o = mgr.probeEncode(EncodePick(EncodeRung.AVC_HIGH, c.name, c.hardware, 0, InputMode.SURFACE))
                        Log.i(tag, "surface probe ${c.name}: ${if (o?.pass == true) "PASS" else "FAIL"} ${o?.detail}")
                    }
                    return@Thread
                }
                val uri = i.getStringExtra("uri") ?: error("uri missing")
                val dest = i.getStringExtra("dest") ?: error("dest missing")
                val params = i.getStringExtra("params") ?: i.getStringExtra("paramsFile")?.let { java.io.File(it).readText() }
                i.getStringExtra("pack")?.let { DappleVideoEdit.setFilmPack(it) }
                val spec = if (params == null) null else EditSpec(
                    paramsJson = params,
                    refMs = i.getLongExtra("refMs", -1L),
                    stillAspect = i.getFloatExtra("aspect", 0f).toDouble(),
                    stillLongEdge = i.getIntExtra("stillLongEdge", 0),
                )
                val t0 = System.nanoTime()
                val m = LiveBridge(app).trim(uri, i.getLongExtra("startMs", 0L), i.getLongExtra("durMs", 1500L), dest, true, 0, i.getBooleanExtra("compat", false), false, spec)
                Log.i(tag, "OK edited=${spec != null} in ${(System.nanoTime() - t0) / 1_000_000} ms: $m")
            } catch (e: Throwable) {
                Log.e(tag, "FAILED: $e", e)
            }
        }.start()
    }
}
