package com.dapple.dapple

import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.opengl.GLES30
import android.view.Surface
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Feeds an encoder's input Surface with finished frames (docs/engine/pipeline-architecture.md section 30.9): many hardware encoders take 10-bit video only through
 * a Surface (their ByteBuffer colour list has no P010). The engine's graph renders the frame on the CPU and packs it as RGB10A2 ([EditFormat.RGB10A2]); this class
 * uploads it as a GL_RGB10_A2 texture and draws one full-screen quad into an EGL window surface on the encoder's input Surface. The EGL config is chosen by what the
 * device offers (R10 G10 B10 A2 first, then RGBA8888 when [allowEightBit]): no device names. The encoder converts RGB to YUV with the colour standard / range in its
 * MediaFormat. [bits] tells which depth the config really has, so a 10-bit self-test can refuse an 8-bit fallback.
 */
class SurfaceFeeder(surface: Surface, private val w: Int, private val h: Int, wantTenBit: Boolean, allowEightBit: Boolean = false) : AutoCloseable {
    private var dpy: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var ctx: EGLContext = EGL14.EGL_NO_CONTEXT
    private var srf: EGLSurface = EGL14.EGL_NO_SURFACE
    private var prog = 0
    private var tex = 0
    /** 10 or 8: the colour depth of the EGL surface the encoder's Surface got. */
    val bits: Int
    val configDescription: String

    init {
        dpy = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        val ver = IntArray(2)
        check(dpy != EGL14.EGL_NO_DISPLAY && EGL14.eglInitialize(dpy, ver, 0, ver, 1)) { "EGL: no display" }
        var cfg: EGLConfig? = null
        var depth = 0
        fun choose(r: Int, a: Int): EGLConfig? {
            val attrs = intArrayOf(
                EGL14.EGL_RED_SIZE, r, EGL14.EGL_GREEN_SIZE, r, EGL14.EGL_BLUE_SIZE, r, EGL14.EGL_ALPHA_SIZE, a,
                EGL14.EGL_RENDERABLE_TYPE, EGLExt.EGL_OPENGL_ES3_BIT_KHR, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT,
                EGL_RECORDABLE_ANDROID, 1, EGL14.EGL_NONE,
            )
            val out = arrayOfNulls<EGLConfig>(1)
            val n = IntArray(1)
            return if (EGL14.eglChooseConfig(dpy, attrs, 0, out, 0, 1, n, 0) && n[0] > 0) out[0] else null
        }
        if (wantTenBit) { cfg = choose(10, 2); if (cfg != null) depth = 10 }
        if (cfg == null && (!wantTenBit || allowEightBit)) { cfg = choose(8, 8); if (cfg != null) depth = 8 }
        checkNotNull(cfg) { if (wantTenBit) "EGL: no R10G10B10A2 recordable config" else "EGL: no recordable config" }
        bits = depth
        val r = IntArray(1)
        EGL14.eglGetConfigAttrib(dpy, cfg, EGL14.EGL_RED_SIZE, r, 0)
        configDescription = "EGL ${ver[0]}.${ver[1]} config red=${r[0]}"
        ctx = EGL14.eglCreateContext(dpy, cfg, EGL14.EGL_NO_CONTEXT, intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 3, EGL14.EGL_NONE), 0)
        check(ctx != EGL14.EGL_NO_CONTEXT) { "EGL: no ES 3 context" }
        srf = EGL14.eglCreateWindowSurface(dpy, cfg, surface, intArrayOf(EGL14.EGL_NONE), 0)
        check(srf != EGL14.EGL_NO_SURFACE) { "EGL: window surface on the encoder input failed (0x${Integer.toHexString(EGL14.eglGetError())})" }
        check(EGL14.eglMakeCurrent(dpy, srf, srf, ctx)) { "EGL: makeCurrent failed" }
        prog = program(VS, FS)
        val t = IntArray(1)
        GLES30.glGenTextures(1, t, 0)
        tex = t[0]
        GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, tex)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MIN_FILTER, GLES30.GL_NEAREST)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_MAG_FILTER, GLES30.GL_NEAREST)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_S, GLES30.GL_CLAMP_TO_EDGE)
        GLES30.glTexParameteri(GLES30.GL_TEXTURE_2D, GLES30.GL_TEXTURE_WRAP_T, GLES30.GL_CLAMP_TO_EDGE)
        GLES30.glViewport(0, 0, w, h)
    }

    /**
     * Draws one frame. [pixels] = [w] x [h] packed words: RGB10A2 (little endian R | G << 10 | B << 20 | A << 30) when [bits] is 10, else RGBA8 bytes.
     * [ptsNs] is the presentation time handed to the encoder.
     */
    fun draw(pixels: ByteBuffer, ptsNs: Long) {
        pixels.order(ByteOrder.nativeOrder()).rewind()
        GLES30.glBindTexture(GLES30.GL_TEXTURE_2D, tex)
        GLES30.glPixelStorei(GLES30.GL_UNPACK_ALIGNMENT, 4)
        if (bits == 10) {
            GLES30.glTexImage2D(GLES30.GL_TEXTURE_2D, 0, GL_RGB10_A2, w, h, 0, GLES30.GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, pixels)
        } else {
            GLES30.glTexImage2D(GLES30.GL_TEXTURE_2D, 0, GLES30.GL_RGBA8, w, h, 0, GLES30.GL_RGBA, GLES30.GL_UNSIGNED_BYTE, pixels)
        }
        GLES30.glUseProgram(prog)
        GLES30.glDrawArrays(GLES30.GL_TRIANGLE_STRIP, 0, 4)
        EGLExt.eglPresentationTimeANDROID(dpy, srf, ptsNs)
        check(EGL14.eglSwapBuffers(dpy, srf)) { "EGL: swap failed (0x${Integer.toHexString(EGL14.eglGetError())})" }
    }

    override fun close() {
        if (dpy != EGL14.EGL_NO_DISPLAY) {
            EGL14.eglMakeCurrent(dpy, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            if (srf != EGL14.EGL_NO_SURFACE) EGL14.eglDestroySurface(dpy, srf)
            if (ctx != EGL14.EGL_NO_CONTEXT) EGL14.eglDestroyContext(dpy, ctx)
            EGL14.eglReleaseThread()
            EGL14.eglTerminate(dpy)
        }
        dpy = EGL14.EGL_NO_DISPLAY; ctx = EGL14.EGL_NO_CONTEXT; srf = EGL14.EGL_NO_SURFACE
    }

    private fun program(vs: String, fs: String): Int {
        fun sh(type: Int, src: String): Int {
            val s = GLES30.glCreateShader(type)
            GLES30.glShaderSource(s, src)
            GLES30.glCompileShader(s)
            val ok = IntArray(1)
            GLES30.glGetShaderiv(s, GLES30.GL_COMPILE_STATUS, ok, 0)
            check(ok[0] != 0) { "GL shader: ${GLES30.glGetShaderInfoLog(s)}" }
            return s
        }
        val p = GLES30.glCreateProgram()
        GLES30.glAttachShader(p, sh(GLES30.GL_VERTEX_SHADER, vs))
        GLES30.glAttachShader(p, sh(GLES30.GL_FRAGMENT_SHADER, fs))
        GLES30.glLinkProgram(p)
        val ok = IntArray(1)
        GLES30.glGetProgramiv(p, GLES30.GL_LINK_STATUS, ok, 0)
        check(ok[0] != 0) { "GL program: ${GLES30.glGetProgramInfoLog(p)}" }
        return p
    }

    private companion object {
        const val EGL_RECORDABLE_ANDROID = 0x3142
        const val GL_RGB10_A2 = 0x8059
        const val GL_UNSIGNED_INT_2_10_10_10_REV = 0x8368
        // full-screen strip generated from gl_VertexID; the texture row 0 is the top of the picture, so v is flipped
        const val VS = "#version 300 es\nout vec2 uv;\nvoid main() {\n  vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));\n  uv = vec2(p.x, 1.0 - p.y);\n  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n}\n"
        const val FS = "#version 300 es\nprecision highp float;\nuniform sampler2D t;\nin vec2 uv;\nout vec4 c;\nvoid main() { c = vec4(texture(t, uv).rgb, 1.0); }\n"
    }
}
