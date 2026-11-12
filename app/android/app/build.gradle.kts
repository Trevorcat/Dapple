plugins {
    id("com.android.application")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

android {
    namespace = "com.dapple.dapple"
    compileSdk = flutter.compileSdkVersion
    ndkVersion = flutter.ndkVersion

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    defaultConfig {
        applicationId = "com.dapple.dapple"
        // Side-by-side dev install: -Pdapple.idSuffix=.dev -> com.dapple.dapple.dev, label "Dapple Dev". Default: unchanged.
        val idSuffix = (project.findProperty("dapple.idSuffix") as String?) ?: ""
        if (idSuffix.isNotEmpty()) applicationIdSuffix = idSuffix
        manifestPlaceholders["appLabel"] = if (idSuffix.isNotEmpty()) "Dapple Dev" else "Dapple"
        minSdk = maxOf(29, flutter.minSdkVersion)
        targetSdk = flutter.targetSdkVersion
        versionCode = flutter.versionCode
        versionName = flutter.versionName

        ndk {
            abiFilters += listOf("arm64-v8a")
            // Emulator testing: flutter build apk --debug -Pdapple.x86=true
            if (project.hasProperty("dapple.x86")) abiFilters += listOf("x86_64")
        }
        externalNativeBuild {
            cmake {
                arguments(
                    "-DDAPPLE_BUILD_CLI=OFF",
                    "-DDAPPLE_BUILD_TESTS=OFF",
                    "-DANDROID_STL=c++_shared",
                    "-DFETCHCONTENT_SOURCE_DIR_LIBRAW=F:/projects/sony-render/tools/_dl/libraw/LibRaw-0.22.2",
                )
                // GPU executor (core/engine, docs/engine/pipeline-architecture.md 24): Vulkan + GLES 3.1 are compiled in by default and used when the
                // start-up self-test of the device passed (no user switch; developer kill switch: setprop debug.dapple.gpu 0).
                // -Pdapple.gpu=false builds a CPU-only app. The GLES shaders are translated by the host tool dapple_shaderc (tools/shaderc).
                if (project.findProperty("dapple.gpu") != "false") {
                    arguments(
                        "-DDAPPLE_GPU=ON",
                        "-DDAPPLE_GLES=ON",
                        "-DDAPPLE_SHADERC_EXE=F:/projects/sony-render/build/shaderc/dapple_shaderc.exe",
                    )
                }
                targets("dapple", "dapple_video")
            }
        }
    }

    externalNativeBuild {
        cmake {
            // Repo-root CMakeLists.txt (app/android/app -> repo root).
            path = file("../../../CMakeLists.txt")
            version = "3.31.6"
        }
    }

    sourceSets["main"].jniLibs.srcDir("../../../tools/ffmpeg/jniLibs")

    packaging {
        jniLibs {
            excludes += if (project.hasProperty("dapple.x86"))
                listOf("lib/armeabi-v7a/**", "lib/x86/**")
            else listOf("lib/armeabi-v7a/**", "lib/x86_64/**", "lib/x86/**")
        }
    }

    buildTypes {
        release {
            // Debug keys so `flutter run --release` works; replace for real distribution.
            signingConfig = signingConfigs.getByName("debug")
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget = org.jetbrains.kotlin.gradle.dsl.JvmTarget.JVM_17
    }
}

flutter {
    source = "../.."
}

dependencies {
    // In-app capture (CaptureActivity): CameraX, Apache-2.0. 1.5 adds ImageCapture RAW / RAW_JPEG output.
    val cameraX = "1.5.1"
    implementation("androidx.camera:camera-core:$cameraX")
    implementation("androidx.camera:camera-camera2:$cameraX")
    implementation("androidx.camera:camera-lifecycle:$cameraX")
    implementation("androidx.camera:camera-view:$cameraX")
    implementation("androidx.activity:activity:1.10.1")
    testImplementation("junit:junit:4.13.2")
}
