# Standalone build of the GLES 3.1 backend + dapple_gles_check (no Vulkan, no core): used for desktop/ANGLE and Android x86_64 emulator runs.
#   cmake -G Ninja -S core/engine/hal/gles/standalone -B build/gles
cmake_minimum_required(VERSION 3.22)
project(dapple_gles_standalone CXX)
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  add_link_options(-static)  # llvm-mingw: no libunwind.dll next to the exe
endif()
enable_testing()
include("${CMAKE_CURRENT_LIST_DIR}/../gles.cmake")
set(DAPPLE_ANGLE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../../tools/android-sdk/emulator/lib64/gles_angle" CACHE PATH "ANGLE dir")
get_filename_component(DAPPLE_ANGLE_DIR "${DAPPLE_ANGLE_DIR}" ABSOLUTE)
if(EXISTS "${DAPPLE_ANGLE_DIR}/libEGL.dll")
  foreach(_b vulkan swiftshader)
    add_test(NAME gles_selftest_angle_${_b} COMMAND dapple_gles_check --software --skip)
    set_tests_properties(gles_selftest_angle_${_b} PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      ENVIRONMENT "DAPPLE_ANGLE_DIR=${DAPPLE_ANGLE_DIR};DAPPLE_ANGLE_BACKEND=${_b}")
  endforeach()
endif()
