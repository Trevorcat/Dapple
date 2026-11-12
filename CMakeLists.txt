cmake_minimum_required(VERSION 3.24)
project(Dapple VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
# Keep exe + dll together (Windows). Respect a value given by the caller, e.g.
# Android Gradle passes its own CMAKE_LIBRARY_OUTPUT_DIRECTORY.
if(NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
if(NOT DEFINED CMAKE_LIBRARY_OUTPUT_DIRECTORY)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()

# Self-contained Windows binaries (llvm-mingw): no libc++.dll / libunwind.dll next to the exe.
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND NOT MSVC)
  add_link_options(-static)
endif()

option(DAPPLE_BUILD_CLI "Build the dapple command line tool" ON)
option(DAPPLE_BUILD_TESTS "Build unit tests" ON)
# HDR alignment / panorama stitching need OpenCV (Apache-2.0, built from source, static).
option(DAPPLE_WITH_OPENCV "Build with OpenCV for HDR alignment and panorama stitching" ON)

include(cmake/Dependencies.cmake)
if(DAPPLE_WITH_OPENCV)
  include(cmake/OpenCV.cmake)
endif()

add_subdirectory(core)
include(core/video/host.cmake)  # desktop video core (only when tools/ffmpeg/build-host.sh has run)

# Software video decoding for Motion Photos (FFmpeg, LGPL, dynamically linked; docs/legal/ffmpeg-lgpl.md). Android only.
if(ANDROID)
  add_subdirectory(core/video)
endif()

# GPU compute backend (docs/engine/gpu-backend.md). Off by default; adds the `dapple gpu-bench` command.
option(DAPPLE_GPU "Build the GPU executor (Vulkan, + GLES 3.1 with DAPPLE_GLES) (core/gpu)" OFF)
# GLES 3.1 compute backend (docs/engine/pipeline-architecture.md 6.3): linked into dapple_gpu as the fallback behind Vulkan, plus
# dapple_gles_check and the ctest gles_* entries (skipped when ANGLE is absent).
option(DAPPLE_GLES "Build the GLES 3.1 HAL backend, its check tool and GLES ctest entries" OFF)
if(DAPPLE_GPU)
  add_subdirectory(core/gpu)
endif()

if(DAPPLE_BUILD_CLI)
  add_subdirectory(cli)
endif()

if(DAPPLE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(DAPPLE_GLES)
  include(core/engine/hal/gles/gles_gpu.cmake)
endif()
