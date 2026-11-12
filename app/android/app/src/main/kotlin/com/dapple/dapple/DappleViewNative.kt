package com.dapple.dapple

import android.view.Surface

/** JNI entry points of core/engine/present/view_jni.cpp (libdapple.so). The view pointer is created by Dart over FFI. */
internal object DappleViewNative {
    val loaded: Boolean = try {
        System.loadLibrary("dapple")
        true
    } catch (_: Throwable) {
        false
    }

    @JvmStatic external fun attach(view: Long, surface: Surface, width: Int, height: Int): Int
    @JvmStatic external fun detach(view: Long)
    @JvmStatic external fun trim(view: Long, level: Int)
    @JvmStatic external fun lastError(): String
}
