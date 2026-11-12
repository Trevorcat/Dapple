// JNI glue for the Kotlin DappleViewPlugin: the Surface callbacks of the SurfaceProducer run on the platform thread and
// must reach the native view synchronously (a Surface can be gone as soon as onSurfaceCleanup returns).
#if defined(__ANDROID__)
#include <android/native_window_jni.h>
#include <jni.h>

#include <string>

#include "dapple/dapple_c.h"
#include "dapple/dapple_view.h"

namespace {
std::string jstr(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string r = c ? c : "";
  if (c) env->ReleaseStringUTFChars(s, c);
  return r;
}
}  // namespace

extern "C" {

JNIEXPORT jint JNICALL Java_com_dapple_dapple_DappleViewNative_attach(JNIEnv* env, jclass, jlong view, jobject surface, jint w, jint h) {
  if (!view || !surface) return DAPPLE_ERROR;
  ANativeWindow* win = ANativeWindow_fromSurface(env, surface);
  if (!win) return DAPPLE_ERROR;
  const int32_t rc = dapple_view_attach_window(reinterpret_cast<DappleView*>(view), win, w, h);
  ANativeWindow_release(win);  // the view holds its own reference
  return rc;
}

JNIEXPORT void JNICALL Java_com_dapple_dapple_DappleViewNative_detach(JNIEnv*, jclass, jlong view) {
  if (view) dapple_view_detach_window(reinterpret_cast<DappleView*>(view));
}

JNIEXPORT void JNICALL Java_com_dapple_dapple_DappleViewNative_trim(JNIEnv*, jclass, jlong view, jint level) {
  if (view) dapple_view_trim(reinterpret_cast<DappleView*>(view), level);
}

// App start (a background thread of MainActivity): where the GPU self-test verdicts live, then start the probe (returns at once).
JNIEXPORT void JNICALL Java_com_dapple_dapple_DappleViewNative_startGpu(JNIEnv* env, jclass, jstring dir, jstring appVersion, jstring fingerprint) {
  dapple_gpu_configure(jstr(env, dir).c_str(), jstr(env, appVersion).c_str(), jstr(env, fingerprint).c_str());
  dapple_gpu_warmup();
}

JNIEXPORT jstring JNICALL Java_com_dapple_dapple_DappleViewNative_lastError(JNIEnv* env, jclass) {
  return env->NewStringUTF(dapple_view_last_error());
}

}  // extern "C"
#endif
