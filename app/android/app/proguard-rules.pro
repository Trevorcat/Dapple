# R8 keep rules for the release build (app/android/app/build.gradle.kts, buildTypes.release).
# Flutter's own rules come from the Flutter Gradle plugin and the AndroidX / CameraX consumer rules ship inside their AARs.

# JNI: native symbols are bound by name (Java_com_dapple_dapple_<Class>_<method>), so every class that declares a native method keeps its name and
# those methods keep theirs. Today: DappleViewNative, DappleVideoEdit (ClipEdit.kt), SwVideo. Nothing in the engine calls back into Java by
# reflection (no FindClass / GetMethodID in core/), so no member of a Java class needs a callback keep rule.
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}

# Anything in the app package that Android instantiates by name from the manifest, or that Flutter registers as a plugin (R8 reads the manifest;
# this is a belt-and-braces rule for the plugin classes registered in MainActivity).
-keep class com.dapple.dapple.*Plugin { *; }
-keep class com.dapple.dapple.MainActivity { *; }

# AI runtimes are loaded as native libraries only (libtensorflowlite_jni.so, libonnxruntime.so; the engine uses their C APIs). If a Java wrapper
# of either is ever added to the dependencies, its native-method rule above already applies; these two keep their public packages intact.
-keep class org.tensorflow.lite.** { *; }
-keep class ai.onnxruntime.** { *; }
-dontwarn org.tensorflow.lite.**
-dontwarn ai.onnxruntime.**

# Play Core deferred components are referenced by the Flutter embedding but not used.
-dontwarn com.google.android.play.core.**
