// JNI glue for Kotlin's DappleVideoEdit (libdapple.so): the frames travel in direct ByteBuffers (half-float RGBA in, RGBA8 / I420 / P010 out).
#if defined(__ANDROID__)
#include <jni.h>

#include <string>

#include "dapple/dapple_c.h"

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

// Kotlin's availability check: the symbol exists only in a libdapple.so that has the edit API.
JNIEXPORT jboolean JNICALL Java_com_dapple_dapple_DappleVideoEdit_nativeProbe(JNIEnv*, jclass) { return JNI_TRUE; }

// Debug / tooling: the film pack path when no Flutter engine has installed the library in this process (DebugClipReceiver).
JNIEXPORT void JNICALL Java_com_dapple_dapple_DappleVideoEdit_nativeSetFilmPack(JNIEnv* env, jclass, jstring path) {
  const std::string p = jstr(env, path);
  dapple_film_set_pack_path(p.c_str());
}

JNIEXPORT jlong JNICALL Java_com_dapple_dapple_DappleVideoEdit_nativeOpen(JNIEnv* env, jclass, jstring params, jstring lut, jstring options) {
  const std::string p = jstr(env, params), l = jstr(env, lut), o = jstr(env, options);
  return reinterpret_cast<jlong>(dapple_video_edit_open(p.c_str(), l.empty() ? nullptr : l.c_str(), o.c_str()));
}

JNIEXPORT void JNICALL Java_com_dapple_dapple_DappleVideoEdit_nativeClose(JNIEnv*, jclass, jlong h) {
  dapple_video_edit_close(reinterpret_cast<DappleVideoEdit*>(h));
}

JNIEXPORT jint JNICALL Java_com_dapple_dapple_DappleVideoEdit_nativeSetReference(JNIEnv* env, jclass, jlong h, jobject src, jint w, jint hgt, jint kind,
                                                                               jint rot) {
  auto* p = static_cast<const uint16_t*>(env->GetDirectBufferAddress(src));
  return p ? dapple_video_edit_set_reference(reinterpret_cast<DappleVideoEdit*>(h), p, w, hgt, kind, rot) : DAPPLE_ERROR;
}

JNIEXPORT jintArray JNICALL Java_com_dapple_dapple_DappleVideoEdit_nativeOutputSize(JNIEnv* env, jclass, jlong h) {
  int32_t wh[2] = {0, 0};
  if (dapple_video_edit_output_size(reinterpret_cast<DappleVideoEdit*>(h), wh) != DAPPLE_OK) return nullptr;
  jintArray a = env->NewIntArray(2);
  env->SetIntArrayRegion(a, 0, 2, reinterpret_cast<const jint*>(wh));
  return a;
}

// {kind, primaries, transfer, matrix, range}
JNIEXPORT jintArray JNICALL Java_com_dapple_dapple_DappleVideoEdit_nativeOutputKind(JNIEnv* env, jclass, jlong h) {
  int32_t tags[4] = {1, 1, 1, 1};
  const int32_t k = dapple_video_edit_output_kind(reinterpret_cast<DappleVideoEdit*>(h), tags, nullptr);
  if (k < 0) return nullptr;
  jint v[5] = {k, tags[0], tags[1], tags[2], tags[3]};
  jintArray a = env->NewIntArray(5);
  env->SetIntArrayRegion(a, 0, 5, v);
  return a;
}

JNIEXPORT jstring JNICALL Java_com_dapple_dapple_DappleVideoEdit_nativeOutputNote(JNIEnv* env, jclass, jlong h) {
  const char* note = "";
  dapple_video_edit_output_kind(reinterpret_cast<DappleVideoEdit*>(h), nullptr, &note);
  return env->NewStringUTF(note ? note : "");
}

JNIEXPORT jlong JNICALL Java_com_dapple_dapple_DappleVideoEdit_nativeFrameBytes(JNIEnv*, jclass, jlong h, jint format) {
  return dapple_video_edit_frame_bytes(reinterpret_cast<DappleVideoEdit*>(h), format);
}

JNIEXPORT jint JNICALL Java_com_dapple_dapple_DappleVideoEdit_nativeRender(JNIEnv* env, jclass, jlong h, jobject src, jint w, jint hgt, jint kind, jint rot,
                                                                         jint frameIndex, jint format, jobject dst) {
  auto* s = static_cast<const uint16_t*>(env->GetDirectBufferAddress(src));
  auto* d = static_cast<uint8_t*>(env->GetDirectBufferAddress(dst));
  if (!s || !d) return DAPPLE_ERROR;
  return dapple_video_edit_render(reinterpret_cast<DappleVideoEdit*>(h), s, w, hgt, kind, rot, static_cast<uint32_t>(frameIndex), format, d,
                                  env->GetDirectBufferCapacity(dst));
}

JNIEXPORT jstring JNICALL Java_com_dapple_dapple_DappleVideoEdit_nativeLastError(JNIEnv* env, jclass) {
  return env->NewStringUTF(dapple_video_edit_last_error());
}

}  // extern "C"
#endif
