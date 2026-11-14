import java.util.Properties

plugins {
    id("com.android.application")
    // The Flutter Gradle Plugin must be applied after the Android and Kotlin Gradle plugins.
    id("dev.flutter.flutter-gradle-plugin")
}

// AI masks: runtime libraries (tools/ai-rt/jniLibs/<abi>) and model files (tools/ai-rt/assets/ai-models, ~150 MB) go into the APK. -Pdapple.ai=false: neither.
// The depth model (99 MB of the 149 MB) is left out by default: depth masks then report unavailable. -Pdapple.aiDepth=true bundles it.
val aiRt = file("../../../tools/ai-rt")
val aiEnabled = project.findProperty("dapple.ai") != "false" && file("$aiRt/jniLibs").isDirectory
val aiDepth = project.findProperty("dapple.aiDepth") == "true"

// Release signing (docs/release/android.md): app/android/key.properties (git-ignored, never committed) holds storeFile, storePassword, keyAlias,
// keyPassword. Absent or incomplete: the release build falls back to the debug key and says so loudly (such an APK cannot be published).
val keyProps = Properties().apply {
    val f = rootProject.file("key.properties")
    if (f.isFile) f.inputStream().use { load(it) }
}
val haveReleaseKey = listOf("storeFile", "storePassword", "keyAlias", "keyPassword").all { !keyProps.getProperty(it).isNullOrBlank() } &&
    rootProject.file(keyProps.getProperty("storeFile", "")).isFile
// ABIs in the package. Default arm64-v8a only. Emulator: -Pdapple.x86=true adds x86_64. Explicit list: -Pdapple.abis=arm64-v8a,x86_64
val packAbis: List<String> = (project.findProperty("dapple.abis") as String?)?.split(",")?.map { it.trim() }?.filter { it.isNotEmpty() }
    ?: if (project.hasProperty("dapple.x86")) listOf("arm64-v8a", "x86_64") else listOf("arm64-v8a")

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
            abiFilters += packAbis
            // AAB: native debug symbols (symbol table) go into the bundle so the Play Console can symbolise native crashes.
            debugSymbolLevel = "SYMBOL_TABLE"
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
                // AI masks (core/ai: LiteRT + ONNX Runtime, docs/engine/pipeline-architecture.md section 41): on by default when the runtime libraries are
                // staged under tools/ai-rt/jniLibs (CMake option DAPPLE_AI=AUTO); -Pdapple.ai=false builds without them (smaller APK, C API says unavailable).
                if (aiEnabled) arguments("-DDAPPLE_AI=AUTO") else arguments("-DDAPPLE_AI=OFF")
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
    if (aiEnabled) {
        sourceSets["main"].jniLibs.srcDir("../../../tools/ai-rt/jniLibs")  // libtensorflowlite_jni.so + libonnxruntime.so (x86_64 only ships in -Pdapple.x86 builds)
        if (file("$aiRt/assets/ai-models").isDirectory) sourceSets["main"].assets.srcDir("../../../tools/ai-rt/assets")
    }
    // model files are stored as they are: copied once to app storage at first use (AiModelsPlugin.kt); deflate would save about 7 % and cost start-up time
    androidResources {
        noCompress += listOf("tflite", "ort")
        // packaging.resources.excludes does not apply to assets; the aapt ignore-assets pattern does (merged assets of every variant)
        if (!aiDepth) ignoreAssetsPatterns += "!depth.ort"
    }

    packaging {
        jniLibs {
            excludes += listOf("lib/armeabi-v7a/**", "lib/x86/**", "lib/arm64-v8a/**", "lib/x86_64/**").filter { e -> packAbis.none { e.startsWith("lib/$it/") } }
        }
    }

    signingConfigs {
        create("release") {
            if (haveReleaseKey) {
                storeFile = rootProject.file(keyProps.getProperty("storeFile"))
                storePassword = keyProps.getProperty("storePassword")
                keyAlias = keyProps.getProperty("keyAlias")
                keyPassword = keyProps.getProperty("keyPassword")
            }
        }
    }

    buildTypes {
        debug {
            // Native engine is always optimised (AGP would otherwise pass CMAKE_BUILD_TYPE=Debug: -O0, unusable speed). Same flags as release;
            // -g (added below for every variant) keeps symbols in the unstripped libs for crash symbolisation.
            externalNativeBuild { cmake { arguments("-DCMAKE_BUILD_TYPE=Release") } }
        }
        release {
            if (haveReleaseKey) {
                signingConfig = signingConfigs.getByName("release")
            } else {
                // Debug keys so `flutter run --release` and local release tests work. NOT publishable.
                signingConfig = signingConfigs.getByName("debug")
                logger.warn("!!!!!!!! WARNING: app/android/key.properties missing or incomplete: the RELEASE build is signed with the DEBUG key. Do not publish it. See docs/release/android.md !!!!!!!!")
            }
            // R8 code shrinking + resource shrinking. Keep rules for JNI and the native plugins: proguard-rules.pro.
            isMinifyEnabled = true
            isShrinkResources = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"), "proguard-rules.pro")
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
