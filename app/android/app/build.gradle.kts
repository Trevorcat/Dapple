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
                // GPU compute backend (core/gpu, docs/engine/gpu-backend.md): off unless built with -Pdapple.gpu=true.
                // Even then it only runs when the runtime flag is set (dapple_gpu_set_enabled / debug.dapple.gpu).
                if (project.hasProperty("dapple.gpu")) arguments("-DDAPPLE_GPU=ON")
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
    testImplementation("junit:junit:4.13.2")
}
