# Dapple on-device inference (dapple_ai): runtime-agnostic Model/run interface + task wrappers.
# NOT part of the main build yet. Hook it in with one line in core/CMakeLists.txt (or the root CMakeLists.txt):
#     add_subdirectory(ai)
# Standalone (tests, phone benchmark):  cmake -S core/ai -B build/ai -DDAPPLE_AI_TESTS=ON ...
#
# Backends (both optional, each adds one shared library per ABI to the app):
#   DAPPLE_AI_LITERT  LiteRT 1.4 TFLite C API (libtensorflowlite_jni.so, 4.3 MB stripped arm64): MediaPipe .tflite models
#   DAPPLE_AI_ORT     ONNX Runtime custom minimal build (libonnxruntime.so, 2.8 MB stripped arm64): Depth Anything, MobileSAM
cmake_minimum_required(VERSION 3.20)
if(NOT PROJECT_NAME)
  project(dapple_ai CXX)
  set(CMAKE_CXX_STANDARD 23)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

set(_ai_rt ${CMAKE_CURRENT_LIST_DIR}/../../tools/ai-rt)
option(DAPPLE_AI_LITERT "LiteRT backend" ON)
option(DAPPLE_AI_ORT "ONNX Runtime backend" ON)
option(DAPPLE_AI_TESTS "build dapple_ai_tests (and the phone benchmarks)" OFF)
set(DAPPLE_AI_LITERT_ROOT "${_ai_rt}/dl/litert_1_4" CACHE PATH "unpacked LiteRT AAR: headers/ and jni/<abi>/libtensorflowlite_jni.so")
set(DAPPLE_AI_ORT_INCLUDE "${_ai_rt}/dl/ort_android/headers" CACHE PATH "dir with onnxruntime_c_api.h")
set(DAPPLE_AI_ORT_LIB "${_ai_rt}/ort-build-min2/MinSizeRel/libonnxruntime.so" CACHE FILEPATH "libonnxruntime (custom minimal build for this ABI)")

add_library(dapple_ai STATIC src/ai.cpp src/imageops.cpp src/tasks.cpp src/backend_litert.cpp src/backend_ort.cpp)
target_include_directories(dapple_ai PUBLIC include PRIVATE src ${CMAKE_CURRENT_LIST_DIR}/../src)  # ../src: sysperf.h (header only, not modified)
target_compile_features(dapple_ai PUBLIC cxx_std_20)

if(DAPPLE_AI_LITERT)
  set(_abi ${ANDROID_ABI})
  if(NOT _abi)
    set(_abi arm64-v8a)
  endif()
  add_library(dapple_ai_litert SHARED IMPORTED)
  set_target_properties(dapple_ai_litert PROPERTIES IMPORTED_LOCATION ${DAPPLE_AI_LITERT_ROOT}/jni/${_abi}/libtensorflowlite_jni.so
    INTERFACE_INCLUDE_DIRECTORIES "${DAPPLE_AI_LITERT_ROOT}/headers;${DAPPLE_AI_LITERT_ROOT}/headers/external/org_tensorflow")
  if(WIN32)  # desktop tests: an import library (.lib) of the LiteRT C API DLL sits beside the .so name
    set_target_properties(dapple_ai_litert PROPERTIES IMPORTED_IMPLIB ${DAPPLE_AI_LITERT_ROOT}/jni/${_abi}/libtensorflowlite_jni.lib)
  endif()
  target_link_libraries(dapple_ai PUBLIC dapple_ai_litert)
  target_compile_definitions(dapple_ai PUBLIC DAPPLE_AI_LITERT=1)
endif()
if(DAPPLE_AI_ORT)
  add_library(dapple_ai_ort SHARED IMPORTED)
  set_target_properties(dapple_ai_ort PROPERTIES IMPORTED_LOCATION ${DAPPLE_AI_ORT_LIB} INTERFACE_INCLUDE_DIRECTORIES "${DAPPLE_AI_ORT_INCLUDE}")
  if(WIN32)  # desktop tests: DAPPLE_AI_ORT_LIB is the import library (onnxruntime.lib) of the official onnxruntime-win-x64 package
    set_target_properties(dapple_ai_ort PROPERTIES IMPORTED_IMPLIB ${DAPPLE_AI_ORT_LIB})
  endif()
  target_link_libraries(dapple_ai PUBLIC dapple_ai_ort)
  target_compile_definitions(dapple_ai PUBLIC DAPPLE_AI_ORT=1)
endif()

if(DAPPLE_AI_TESTS)
  add_executable(dapple_ai_tests tests/test_ai.cpp)
  target_link_libraries(dapple_ai_tests PRIVATE dapple_ai)
  target_include_directories(dapple_ai_tests PRIVATE src)
  if(ANDROID)
    target_link_libraries(dapple_ai_tests PRIVATE log)
    foreach(_b litert ort)
      add_executable(dapple_ai_bench_${_b} bench/${_b}_bench.cpp)
      target_include_directories(dapple_ai_bench_${_b} PRIVATE bench)
    endforeach()
    target_link_libraries(dapple_ai_bench_litert PRIVATE dapple_ai_litert ${_ai_rt}/dl/litert_gpu_1_4/jni/${_abi}/libtensorflowlite_gpu_jni.so log)
    target_include_directories(dapple_ai_bench_litert PRIVATE ${_ai_rt}/dl/litert_gpu_1_4/headers)
    target_link_libraries(dapple_ai_bench_ort PRIVATE dapple_ai_ort log)
  endif()
endif()
