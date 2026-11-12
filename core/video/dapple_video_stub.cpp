// Stand-in libdapple_video.so for ABIs without an FFmpeg build (tools/ffmpeg/build.sh not run for them): the app then
// reports "no video decoder" instead of failing to build or load.
#include <jni.h>

extern "C" {
JNIEXPORT jstring JNICALL Java_com_dapple_dapple_SwVideo_nativeOpen(JNIEnv* e, jclass, jstring, jint, jint, jstring, jlongArray h) {
  jlong z = 0;
  e->SetLongArrayRegion(h, 0, 1, &z);
  return e->NewStringUTF("{\"error\":\"FFmpeg not built for this ABI\"}");
}
JNIEXPORT void JNICALL Java_com_dapple_dapple_SwVideo_nativeClose(JNIEnv*, jclass, jlong) {}
JNIEXPORT jintArray JNICALL Java_com_dapple_dapple_SwVideo_nativeBegin(JNIEnv* e, jclass, jlong, jlong, jlong, jint, jdouble, jint, jint) {
  return e->NewIntArray(3);
}
JNIEXPORT jlong JNICALL Java_com_dapple_dapple_SwVideo_nativeNext(JNIEnv*, jclass, jlong, jobject) { return -2; }
JNIEXPORT jintArray JNICALL Java_com_dapple_dapple_SwVideo_nativeTags(JNIEnv* e, jclass, jlong) { return e->NewIntArray(4); }
JNIEXPORT jbyteArray JNICALL Java_com_dapple_dapple_SwVideo_nativeHdrInfo(JNIEnv*, jclass, jlong) { return nullptr; }
JNIEXPORT jintArray JNICALL Java_com_dapple_dapple_SwVideo_nativeAudioBegin(JNIEnv*, jclass, jlong, jlong, jlong) { return nullptr; }
JNIEXPORT jint JNICALL Java_com_dapple_dapple_SwVideo_nativeAudioNext(JNIEnv*, jclass, jlong, jobject, jint, jlongArray) { return -2; }
JNIEXPORT jintArray JNICALL Java_com_dapple_dapple_SwVideo_nativeUltraHdr(JNIEnv*, jclass, jobject, jint, jint, jobject, jobject, jfloatArray) {
  return nullptr;
}
JNIEXPORT void JNICALL Java_com_dapple_dapple_SwVideo_nativeSetCodecReport(JNIEnv*, jclass, jstring) {}
JNIEXPORT jstring JNICALL Java_com_dapple_dapple_SwVideo_nativeCodecReport(JNIEnv* e, jclass) { return e->NewStringUTF(""); }
JNIEXPORT jstring JNICALL Java_com_dapple_dapple_SwVideo_nativeDecoderNow(JNIEnv* e, jclass, jlong) { return e->NewStringUTF(""); }
JNIEXPORT jstring JNICALL Java_com_dapple_dapple_SwVideo_nativeVersion(JNIEnv* e, jclass) { return e->NewStringUTF("stub"); }
}
