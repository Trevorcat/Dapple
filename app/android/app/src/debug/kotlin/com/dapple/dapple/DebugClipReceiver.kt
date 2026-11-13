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
                if (i.getBooleanExtra("inspect", false)) {
                    // what the platform sees: --es uri <file> [--el off <video offset> --el len <video length> for a Motion Photo JPEG]. Video track format
                    // (profile / colour keys) through MediaExtractor, and for a JPEG BitmapFactory's gain map verdict (API 34+).
                    val off = i.getLongExtra("off", -1L)
                    val file = java.io.File(uri)
                    val ex = android.media.MediaExtractor()
                    if (off < 0 && uri.endsWith(".jpg")) { /* a plain JPEG has no video track */ } else {
                    if (off >= 0) java.io.FileInputStream(file).use { ex.setDataSource(it.fd, off, i.getLongExtra("len", file.length() - off)) } else ex.setDataSource(uri)
                    for (t in 0 until ex.trackCount) {
                        val f = ex.getTrackFormat(t)
                        if ((f.getString(android.media.MediaFormat.KEY_MIME) ?: "").startsWith("video/")) {
                            fun k(n: String) = if (f.containsKey(n)) f.getInteger(n) else null
                            Log.i(tag, "INSPECT video mime=${f.getString(android.media.MediaFormat.KEY_MIME)} ${f.getInteger(android.media.MediaFormat.KEY_WIDTH)}x${f.getInteger(android.media.MediaFormat.KEY_HEIGHT)} " +
                                "profile=${k("profile")} color-standard=${k("color-standard")} color-transfer=${k("color-transfer")} color-range=${k("color-range")} hdr-static-info=${f.containsKey("hdr-static-info")}")
                        }
                    }
                    }
                    ex.release()
                    if (off >= 0 || uri.endsWith(".jpg")) {
                        val bmp = android.graphics.BitmapFactory.decodeFile(uri)
                        if (bmp != null) {
                            val gm = if (android.os.Build.VERSION.SDK_INT >= 34 && bmp.hasGainmap()) bmp.gainmap else null
                            Log.i(tag, "INSPECT jpeg ${bmp.width}x${bmp.height} sdk=${android.os.Build.VERSION.SDK_INT} hasGainmap=${gm != null}" +
                                (gm?.let { " ratioMax=${it.ratioMax[0]} ratioMin=${it.ratioMin[0]} gamma=${it.gamma[0]} gainmapContents=${it.gainmapContents.width}x${it.gainmapContents.height} displayRatioForFullHdr=${it.displayRatioForFullHdr}" } ?: ""))
                        } else Log.i(tag, "INSPECT jpeg: decode failed")
                    }
                    return@Thread
                }
                if (i.getBooleanExtra("cover", false)) {
                    // the Ultra HDR cover of a Motion Photo (docs section 30.14): --es sdr <the still of the same edit> --es out <cover.jpg> --el refMs <cover time> [--ez force true]
                    val p = i.getStringExtra("params") ?: i.getStringExtra("paramsFile")?.let { java.io.File(it).readText() } ?: "{}"
                    i.getStringExtra("pack")?.let { DappleVideoEdit.setFilmPack(it) }
                    val spec = EditSpec(paramsJson = p, refMs = i.getLongExtra("refMs", 0L), stillLongEdge = i.getIntExtra("stillLongEdge", 0))
                    val m = LiveBridge(app).coverHdr(uri, i.getLongExtra("refMs", 0L), 0, spec, i.getStringExtra("sdr") ?: error("sdr missing"),
                        i.getStringExtra("out") ?: error("out missing"), i.getBooleanExtra("force", false))
                    Log.i(tag, "COVER OK: $m")
                    return@Thread
                }
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
