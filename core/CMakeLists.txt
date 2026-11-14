add_library(dapple_core STATIC
  src/auto_adjust.cpp
  src/camera.cpp
  src/developer.cpp
  src/edit_params.cpp
  src/edit_params_json.cpp
  src/local_adjust.cpp
  src/local_eval.cpp
  src/local_retouch.cpp
  src/reshape.cpp
  src/warp_field.cpp
  src/ai_masks.cpp
  src/ai_masks_build.cpp
  src/ai_masks_sky.cpp
  src/ai_masks_service.cpp
  src/encode.cpp
  src/export_stream.cpp
  src/exif_writer.cpp
  src/image.cpp
  src/hdr.cpp
  src/ingest.cpp
  src/look.cpp
  src/modern.cpp
  src/lut.cpp
  src/merge_io.cpp
  src/motion_photo.cpp
  src/pano.cpp
  src/renderer.cpp
  src/sequence.cpp
  src/spatial.cpp
  src/hdr_field.cpp
  src/tone.cpp
  src/ultrahdr.cpp
  src/xmp_preset.cpp
  src/decoders/jpeg_decoder.cpp
  src/decoders/jpeg_dc.cpp
  src/colour_space.cpp
  src/decoders/libraw_decoder.cpp
  src/decoders/dng_opcodes.cpp
  src/dng_thumbnail.cpp
  src/decoders/dng_jxl.cpp
  src/exposure_anchor.cpp
  src/film.cpp
  src/film_color.cpp
  src/film_model.cpp
  src/film_params.cpp
  src/film_match.cpp
  src/film_match_cache.cpp
  src/film_profile.cpp
  src/film_upsample.cpp
  src/film_kernels_base.cpp
  src/film_spatial.cpp
  src/film_spatial_render.cpp
  src/film_registry.cpp
  engine/graph/params.cpp
  engine/graph/graph.cpp
  engine/graph/registry.cpp
  engine/graph/builder.cpp
  engine/exec/engine_session.cpp
  engine/nodes/film/film_tables.cpp
  engine/nodes/film/film_cpu.cpp
  engine/nodes/film/film_frame.cpp
  engine/exec/cpu_executor.cpp
  engine/plan/planner.cpp
  engine/nodes/main/cpu_nodes.cpp
  engine/nodes/demosaic/demosaic_cpu.cpp
  engine/nodes/demosaic/demosaic_session.cpp
  engine/video/video_frame_session.cpp
)
add_library(Dapple::core ALIAS dapple_core)

# Film run kernels: a second build of the same source with AVX2 + FMA, selected at runtime.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Mm][Dd]64|x86_64|x64|X86" AND NOT MSVC)
  target_sources(dapple_core PRIVATE src/film_kernels_avx2.cpp)
  set_source_files_properties(src/film_kernels_avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
  target_compile_definitions(dapple_core PRIVATE DAPPLE_FILM_HAVE_AVX2=1)
  set(DAPPLE_FILM_AVX2 ON CACHE INTERNAL "")
endif()

target_include_directories(dapple_core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
          ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(dapple_core PRIVATE dapple_libraw dapple_stb)
if(APPLE)
  target_sources(dapple_core PRIVATE src/omp_apple.cpp)
  if(DAPPLE_LIBRAW_OPENMP AND DAPPLE_LIBOMP_DIR)
    set_source_files_properties(src/omp_apple.cpp PROPERTIES COMPILE_OPTIONS "-Xclang;-fopenmp;-I${DAPPLE_LIBOMP_DIR}/include" COMPILE_DEFINITIONS DAPPLE_LIBOMP=1)
  endif()
endif()
if(TARGET dapple_jxl)
  target_link_libraries(dapple_core PRIVATE dapple_jxl)
  target_compile_definitions(dapple_core PRIVATE DAPPLE_HAVE_JXL=1)
endif()
if(TARGET dapple_opencv)
  target_link_libraries(dapple_core PRIVATE dapple_opencv)
endif()

# ---------------------------------------------------------------------------
# AI masks (core/ai, core/src/ai_masks.cpp; docs/engine/pipeline-architecture.md section 41).
#   DAPPLE_AI = ON | OFF | AUTO. AUTO: ON for Android arm64-v8a / x86_64 when the runtime libraries are staged under tools/ai-rt, OFF on desktop and on Apple (iOS / macOS: pass -DDAPPLE_AI=ON, scripts/ios/build-engine.sh does).
#   Desktop ON needs the official onnxruntime-win-x64 package (tools/ai-rt/dl/ort_win_x64): only depth / MobileSAM tap run there (no LiteRT desktop
#   library exists); with DAPPLE_AI OFF the C API functions exist and report "unavailable".
# Runtime libraries per ABI (tools/ai-rt/jniLibs/<abi>/, the same files Gradle packages): arm64-v8a LiteRT 1.4 + the minimal ONNX Runtime build
# (2.8 MB); x86_64 (emulator) LiteRT 1.4 + the official full ONNX Runtime AAR library (45 MB, debug APKs for the emulator only).
# ---------------------------------------------------------------------------
set(DAPPLE_AI "AUTO" CACHE STRING "AI masks: ON, OFF or AUTO (AUTO = Android arm64-v8a / x86_64 when the runtimes are present)")
set(_ai_rt ${CMAKE_CURRENT_SOURCE_DIR}/../tools/ai-rt)
set(_ai_enable OFF)
if(DAPPLE_AI STREQUAL "AUTO")
  if(ANDROID AND ANDROID_ABI MATCHES "arm64-v8a|x86_64" AND EXISTS ${_ai_rt}/jniLibs/${ANDROID_ABI}/libonnxruntime.so
     AND EXISTS ${_ai_rt}/jniLibs/${ANDROID_ABI}/libtensorflowlite_jni.so)
    set(_ai_enable ON)
  endif()
elseif(DAPPLE_AI)
  set(_ai_enable ON)
endif()
if(_ai_enable)
  if(ANDROID)
    set(DAPPLE_AI_LITERT ON CACHE BOOL "" FORCE)
    set(DAPPLE_AI_ORT ON CACHE BOOL "" FORCE)
    set(DAPPLE_AI_LITERT_ROOT ${_ai_rt}/dl/litert_1_4 CACHE PATH "" FORCE)
    set(DAPPLE_AI_ORT_INCLUDE ${_ai_rt}/dl/ort_android/headers CACHE PATH "" FORCE)
    set(DAPPLE_AI_ORT_LIB ${_ai_rt}/jniLibs/${ANDROID_ABI}/libonnxruntime.so CACHE FILEPATH "" FORCE)
  elseif(APPLE)
    # Apple: official prebuilts linked statically (library paths and slices are chosen in core/ai/CMakeLists.txt, docs/research/ai-runtime.md "iOS and macOS").
    #   CPU / XNNPACK only: no CoreML, Metal or GPU delegate is created anywhere.
    set(DAPPLE_AI_ORT ON CACHE BOOL "" FORCE)
    set(DAPPLE_AI_LITERT ON CACHE BOOL "" FORCE)  # core/ai switches it off with a warning when the TFLite C library of the platform is not staged
  else()
    set(DAPPLE_AI_LITERT OFF CACHE BOOL "" FORCE)  # no LiteRT desktop C library
    set(DAPPLE_AI_ORT ON CACHE BOOL "" FORCE)
    set(_ort_win ${_ai_rt}/dl/ort_win_x64/onnxruntime-win-x64-1.32.0)
    set(DAPPLE_AI_ORT_INCLUDE ${_ort_win}/include CACHE PATH "" FORCE)
    set(DAPPLE_AI_ORT_LIB ${_ort_win}/lib/onnxruntime.lib CACHE FILEPATH "" FORCE)
    if(NOT EXISTS ${DAPPLE_AI_ORT_LIB})
      message(FATAL_ERROR "DAPPLE_AI=ON on desktop needs tools/ai-rt/dl/ort_win_x64 (official onnxruntime-win-x64 1.32.0)")
    endif()
    file(COPY ${_ort_win}/lib/onnxruntime.dll DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
  endif()
  set(DAPPLE_AI_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory(ai)
  target_link_libraries(dapple_core PRIVATE dapple_ai)
  target_compile_definitions(dapple_core PUBLIC DAPPLE_AI=1)
  message(STATUS "DAPPLE_AI: ON (LiteRT=${DAPPLE_AI_LITERT} ORT=${DAPPLE_AI_ORT})")
else()
  message(STATUS "DAPPLE_AI: OFF (C API reports AI masks unavailable)")
endif()

if(MSVC)
  target_compile_options(dapple_core PRIVATE /W4 /utf-8)
else()
  target_compile_options(dapple_core PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-math-errno)
endif()

# ---------------------------------------------------------------------------
# Shared library for FFI (Flutter / dart:ffi). Exports only the C API from
# dapple_c.h; the C++ core is linked in statically. On Android this is
# libdapple.so.
# ---------------------------------------------------------------------------
# film_swatch.cpp (a C API function of its own) is compiled into the shared library itself: as a member of the static core nothing
# references it, so the linker would drop it from libdapple.
add_library(dapple SHARED src/c_api.cpp src/film_swatch.cpp engine/video/video_c_api.cpp engine/video/video_jni.cpp)
target_include_directories(dapple PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(dapple PRIVATE dapple_core)
target_compile_definitions(dapple PRIVATE DAPPLE_VERSION_STRING="${PROJECT_VERSION}")
set_target_properties(dapple PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
if(NOT WIN32 AND NOT APPLE)
  # Keep statically linked dependencies' symbols out of the dynamic table.
  target_link_options(dapple PRIVATE -Wl,--exclude-libs,ALL -Wl,--gc-sections -Wl,--icf=safe)
endif()
if(MSVC)
  target_compile_options(dapple PRIVATE /W4 /utf-8)
else()
  target_compile_options(dapple PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
if(ANDROID)
  target_link_libraries(dapple_core PUBLIC log)  # engine_session.cpp logs via __android_log_print (static consumers: tests, cli)
  target_link_libraries(dapple PRIVATE log)
  # 16 KB page size support for Android 15+ devices
  target_link_options(dapple PRIVATE -Wl,-z,max-page-size=16384)
endif()

# Stage (d): zero-copy display layer (view API, render loop, presenters) compiled into libdapple.
include(engine/present/present.cmake)
