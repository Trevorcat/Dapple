package com.dapple.dapple

import android.Manifest
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.util.Log
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.TextView
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.AspectRatio
import androidx.camera.core.CameraSelector
import androidx.camera.core.ImageCapture
import androidx.camera.core.ImageCaptureException
import androidx.camera.core.Preview
import androidx.camera.core.resolutionselector.AspectRatioStrategy
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.view.PreviewView
import androidx.core.content.ContextCompat
import androidx.core.content.FileProvider
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Minimal in-app capture (testing convenience, docs/design/requirements.md 4.2.6): CameraX viewfinder, shutter,
 * lens switch, RAW/JPEG indicator. One shutter press returns to Dart with the files; Dart ingests and opens the editor.
 *
 * Output: [EXTRA_JPG] always, [EXTRA_DNG] when the lens supports ImageCapture RAW_JPEG. If CameraX cannot open a camera
 * the system camera (ACTION_IMAGE_CAPTURE into a FileProvider URI) takes the picture instead (JPEG only).
 */
class CaptureActivity : ComponentActivity() {
    companion object {
        const val EXTRA_JPG = "jpg"
        const val EXTRA_DNG = "dng"
        const val EXTRA_SOURCE = "source"
        const val EXTRA_ERROR = "error"
        private const val TAG = "DappleCapture"

        fun outDir(ctx: Context) = File(ctx.cacheDir, "capture").apply { mkdirs() }
    }

    private lateinit var preview: PreviewView
    private lateinit var shutter: ShutterView
    private lateinit var badge: TextView
    private var provider: ProcessCameraProvider? = null
    private var imageCapture: ImageCapture? = null
    private var back = true
    private var raw = false
    private var busy = false
    private var finished = false
    private var systemFile: File? = null

    private val systemCamera = registerForActivityResult(ActivityResultContracts.TakePicture()) { ok ->
        val f = systemFile
        if (ok && f != null && f.length() > 0) deliver(f, null, "system") else cancel()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            fail("permission")
            return
        }
        buildUi()
        if (!packageManager.hasSystemFeature(PackageManager.FEATURE_CAMERA_ANY)) {
            launchSystemCamera()
            return
        }
        val f = ProcessCameraProvider.getInstance(this)
        f.addListener({
            try {
                provider = f.get()
                bind()
            } catch (e: Exception) {
                Log.w(TAG, "CameraX unavailable: $e")
                launchSystemCamera()
            }
        }, ContextCompat.getMainExecutor(this))
    }

    private fun dp(v: Float) = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, resources.displayMetrics)

    private fun buildUi() {
        val root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        preview = PreviewView(this).apply { scaleType = PreviewView.ScaleType.FIT_CENTER }
        root.addView(preview, FrameLayout.LayoutParams(-1, -1))

        val bar = FrameLayout(this)
        root.addView(bar, FrameLayout.LayoutParams(-1, dp(124f).toInt(), Gravity.BOTTOM))
        shutter = ShutterView(this).apply {
            contentDescription = "拍照"
            setOnClickListener { takePicture() }
        }
        val sz = dp(76f).toInt()
        bar.addView(shutter, FrameLayout.LayoutParams(sz, sz, Gravity.CENTER_HORIZONTAL or Gravity.TOP).apply {
            topMargin = dp(20f).toInt()
        })
        val flip = GlyphView(this, GlyphView.FLIP).apply {
            contentDescription = "切换镜头"
            setOnClickListener { if (!busy) { back = !back; bind() } }
        }
        val b = dp(48f).toInt()
        bar.addView(flip, FrameLayout.LayoutParams(b, b, Gravity.END or Gravity.TOP).apply {
            topMargin = dp(34f).toInt(); marginEnd = dp(32f).toInt()
        })

        val close = GlyphView(this, GlyphView.CLOSE).apply {
            contentDescription = "关闭"
            setOnClickListener { cancel() }
        }
        root.addView(close, FrameLayout.LayoutParams(b, b, Gravity.START or Gravity.TOP).apply {
            topMargin = dp(8f).toInt(); marginStart = dp(8f).toInt()
        })
        badge = TextView(this).apply {
            setTextColor(Color.WHITE)
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
            typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
            gravity = Gravity.CENTER
            letterSpacing = 0.04f
            setPadding(dp(10f).toInt(), dp(3f).toInt(), dp(10f).toInt(), dp(3f).toInt())
            background = GradientDrawable().apply { cornerRadius = dp(10f); setColor(0x66000000) }
        }
        root.addView(badge, FrameLayout.LayoutParams(-2, -2, Gravity.CENTER_HORIZONTAL or Gravity.TOP).apply {
            topMargin = dp(18f).toInt()
        })
        setContentView(root)
        ViewCompat.setOnApplyWindowInsetsListener(root) { _, insets ->
            val i = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            root.setPadding(i.left, i.top, i.right, i.bottom)
            insets
        }
    }

    private fun setBadge() {
        badge.text = if (raw) "RAW + JPEG" else "JPEG"
    }

    private fun selector(backLens: Boolean) = CameraSelector.Builder()
        .requireLensFacing(if (backLens) CameraSelector.LENS_FACING_BACK else CameraSelector.LENS_FACING_FRONT).build()

    private fun bind() {
        val p = provider ?: return
        var sel = selector(back)
        if (!p.hasCamera(sel)) {
            if (imageCapture != null) { back = !back; return } // no second lens: stay on the current one
            back = !back
            sel = selector(back)
            if (!p.hasCamera(sel)) { launchSystemCamera(); return }
        }
        val rawOk = sel.filter(p.availableCameraInfos).firstOrNull()?.let {
            ImageCapture.getImageCaptureCapabilities(it).supportedOutputFormats.contains(ImageCapture.OUTPUT_FORMAT_RAW_JPEG)
        } ?: false
        val rot = display?.rotation ?: 0
        for (wantRaw in if (rawOk) listOf(true, false) else listOf(false)) {
            try {
                p.unbindAll()
                // highest resolution the lens offers (including the sensor's high-resolution mode), 4:3
                val hi = ResolutionSelector.Builder()
                    .setAspectRatioStrategy(AspectRatioStrategy.RATIO_4_3_FALLBACK_AUTO_STRATEGY)
                    .setResolutionStrategy(ResolutionStrategy.HIGHEST_AVAILABLE_STRATEGY)
                    .setAllowedResolutionMode(ResolutionSelector.PREFER_HIGHER_RESOLUTION_OVER_CAPTURE_RATE)
                    .build()
                val ic = ImageCapture.Builder()
                    .setCaptureMode(ImageCapture.CAPTURE_MODE_MAXIMIZE_QUALITY)
                    .setResolutionSelector(hi)
                    .setJpegQuality(97)
                    .setTargetRotation(rot)
                    .apply { if (wantRaw) setOutputFormat(ImageCapture.OUTPUT_FORMAT_RAW_JPEG) }
                    .build()
                val pv = Preview.Builder()
                    .setResolutionSelector(
                        ResolutionSelector.Builder()
                            .setAspectRatioStrategy(AspectRatioStrategy(AspectRatio.RATIO_4_3, AspectRatioStrategy.FALLBACK_RULE_AUTO))
                            .build()
                    ).build()
                pv.surfaceProvider = preview.surfaceProvider
                p.bindToLifecycle(this, sel, pv, ic)
                imageCapture = ic
                raw = wantRaw
                setBadge()
                return
            } catch (e: Exception) {
                Log.w(TAG, "bind(raw=$wantRaw) failed: $e")
            }
        }
        if (imageCapture == null) launchSystemCamera()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        display?.rotation?.let { imageCapture?.targetRotation = it }
    }

    private fun takePicture() {
        val ic = imageCapture ?: return
        if (busy) return
        busy = true
        shutter.alpha = 0.4f
        val stamp = SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date())
        val dir = outDir(this)
        val jpg = File(dir, "DAP_$stamp.jpg")
        val dng = File(dir, "DAP_$stamp.dng")
        val ex = ContextCompat.getMainExecutor(this)
        var left = if (raw) 2 else 1
        val cb = object : ImageCapture.OnImageSavedCallback {
            override fun onImageSaved(r: ImageCapture.OutputFileResults) {
                if (--left == 0) finishCapture(jpg, if (raw) dng else null)
            }

            override fun onError(e: ImageCaptureException) {
                Log.w(TAG, "capture failed: $e")
                // a lost RAW still leaves the JPEG usable
                if (--left == 0 || (raw && jpg.length() > 0)) finishCapture(jpg, null)
                else if (jpg.length() == 0L && left <= 0) fail(e.message ?: "capture")
            }
        }
        try {
            if (raw) {
                ic.takePicture(
                    ImageCapture.OutputFileOptions.Builder(dng).build(),
                    ImageCapture.OutputFileOptions.Builder(jpg).build(), ex, cb
                )
            } else {
                ic.takePicture(ImageCapture.OutputFileOptions.Builder(jpg).build(), ex, cb)
            }
        } catch (e: Exception) {
            fail(e.message ?: "capture")
        }
    }

    private fun isJpeg(f: File): Boolean = try {
        f.inputStream().use { it.read() == 0xFF && it.read() == 0xD8 }
    } catch (_: Exception) { false }

    private fun finishCapture(jpg: File, dng: File?) {
        if (finished) return
        var d = dng
        // CameraX writes (raw, jpeg) in that order; verify by magic bytes instead of trusting it
        if (d != null && d.length() > 0 && jpg.length() > 0 && !isJpeg(jpg) && isJpeg(d)) {
            val t = File(jpg.parentFile, "swap.tmp")
            jpg.renameTo(t); d.renameTo(jpg); t.renameTo(d)
        }
        if (d != null && d.length() == 0L) d = null
        if (jpg.length() == 0L) { fail("empty"); return }
        deliver(jpg, d, "camerax")
    }

    private fun launchSystemCamera() {
        if (finished) return
        val f = File(outDir(this), "SYS_${System.currentTimeMillis()}.jpg")
        systemFile = f
        try {
            val uri = FileProvider.getUriForFile(this, "$packageName.captureprovider", f)
            systemCamera.launch(uri)
        } catch (e: Exception) {
            Log.w(TAG, "system camera unavailable: $e")
            fail("no camera")
        }
    }

    private fun deliver(jpg: File, dng: File?, source: String) {
        if (finished) return
        finished = true
        setResult(RESULT_OK, Intent().putExtra(EXTRA_JPG, jpg.absolutePath).putExtra(EXTRA_SOURCE, source).apply {
            if (dng != null) putExtra(EXTRA_DNG, dng.absolutePath)
        })
        finish()
    }

    private fun cancel() {
        if (finished) return
        finished = true
        setResult(RESULT_CANCELED)
        finish()
    }

    private fun fail(msg: String) {
        if (finished) return
        finished = true
        setResult(RESULT_FIRST_USER, Intent().putExtra(EXTRA_ERROR, msg))
        finish()
    }

    override fun onDestroy() {
        try { provider?.unbindAll() } catch (_: Exception) {}
        super.onDestroy()
    }
}

/** White ring with an inner disc (iOS-style shutter). */
private class ShutterView(c: Context) : View(c) {
    private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; color = Color.WHITE }
    private val disc = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE }
    init { isClickable = true; isFocusable = true }
    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        ring.strokeWidth = w * 0.05f
        canvas.drawCircle(w / 2, w / 2, w / 2 - ring.strokeWidth / 2, ring)
        disc.alpha = if (isPressed) 180 else 255
        canvas.drawCircle(w / 2, w / 2, w / 2 - w * 0.13f, disc)
    }
    override fun setPressed(pressed: Boolean) { super.setPressed(pressed); invalidate() }
    override fun setAlpha(alpha: Float) { super.setAlpha(alpha); isEnabled = alpha >= 1f }
}

/** Line glyph buttons: close (x) and lens flip (two arcs with arrowheads). */
private class GlyphView(c: Context, private val kind: Int) : View(c) {
    companion object { const val CLOSE = 0; const val FLIP = 1 }
    private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE; color = Color.WHITE; strokeCap = Paint.Cap.ROUND; strokeJoin = Paint.Join.ROUND
    }
    init { isClickable = true; isFocusable = true }
    override fun onDraw(canvas: Canvas) {
        val w = width.toFloat()
        line.strokeWidth = w * 0.045f
        val u = w / 24f
        if (kind == CLOSE) {
            canvas.drawLine(8 * u, 8 * u, 16 * u, 16 * u, line)
            canvas.drawLine(16 * u, 8 * u, 8 * u, 16 * u, line)
        } else {
            val r = RectF(5 * u, 5 * u, 19 * u, 19 * u)
            canvas.drawArc(r, 200f, 120f, false, line)
            canvas.drawArc(r, 20f, 120f, false, line)
            val a = Path().apply { moveTo(15.5f * u, 3.5f * u); lineTo(18.6f * u, 5.6f * u); lineTo(15.2f * u, 8.2f * u) }
            val b = Path().apply { moveTo(8.5f * u, 20.5f * u); lineTo(5.4f * u, 18.4f * u); lineTo(8.8f * u, 15.8f * u) }
            canvas.drawPath(a, line); canvas.drawPath(b, line)
        }
    }
}
