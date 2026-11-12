# GLES 3.1 backend inside the full tree build (include when -DDAPPLE_GLES=ON, after the tests directory):
#   * dapple_gles_check                  smoke / self-test tool (needs no Vulkan, no core)
#   * ctest gles_selftest_angle_vulkan / gles_selftest_angle_swiftshader   (skipped cleanly: exit 77 when ANGLE or a device is absent)
#   * ctest gles_gpu_parity_angle_* / gles_engine_<t>_angle_*   the production GPU executor (Plan -> gpuImplFor(...).recordPorts) and its
#     tests with the GLES device forced (DAPPLE_GPU_API=gles): dapple_gpu links dapple_hal_gles and hal/select.cpp picks it.
#     Needs -DDAPPLE_GPU=ON.
if(NOT TARGET dapple_hal_gles)
  include("${CMAKE_CURRENT_LIST_DIR}/gles.cmake")
endif()

set(DAPPLE_ANGLE_DIR "${PROJECT_SOURCE_DIR}/tools/android-sdk/emulator/lib64/gles_angle" CACHE PATH
    "Directory with ANGLE libEGL.dll/libGLESv2.dll (Android SDK emulator component) for desktop GLES tests")
set(_angle_ok FALSE)
if(EXISTS "${DAPPLE_ANGLE_DIR}/libEGL.dll" OR EXISTS "${DAPPLE_ANGLE_DIR}/libEGL.so")
  set(_angle_ok TRUE)
endif()

if(_angle_ok)
  foreach(_b vulkan swiftshader)
    add_test(NAME gles_selftest_angle_${_b} COMMAND dapple_gles_check --software --skip)
    set_tests_properties(gles_selftest_angle_${_b} PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      ENVIRONMENT "DAPPLE_ANGLE_DIR=${DAPPLE_ANGLE_DIR};DAPPLE_ANGLE_BACKEND=${_b}")
  endforeach()
else()
  message(STATUS "DAPPLE_GLES: ANGLE not found in ${DAPPLE_ANGLE_DIR}: GLES ctest entries not added (they would skip)")
endif()

if(TARGET dapple_gpu AND TARGET dapple_gpu_tests AND _angle_ok)
  foreach(_b vulkan swiftshader)
    set(_env "DAPPLE_ANGLE_DIR=${DAPPLE_ANGLE_DIR};DAPPLE_ANGLE_BACKEND=${_b};DAPPLE_GPU_SOFTWARE=1;DAPPLE_GPU_API=gles")
    add_test(NAME gles_gpu_parity_angle_${_b} COMMAND dapple_gpu_tests)
    set_tests_properties(gles_gpu_parity_angle_${_b} PROPERTIES TIMEOUT 3600 SKIP_RETURN_CODE 77 ENVIRONMENT "${_env}")
    foreach(_t hal box sched)
      add_test(NAME gles_engine_${_t}_angle_${_b} COMMAND dapple_gpu_engine_tests ${_t})
      set_tests_properties(gles_engine_${_t}_angle_${_b} PROPERTIES TIMEOUT 1800 SKIP_RETURN_CODE 77 ENVIRONMENT "${_env}")
    endforeach()
    # the 128-invocation workgroup shape (docs/engine/gles-shader-patches.md): what a GLES 3.1 device that only guarantees 128 runs
    add_test(NAME gles_gpu_parity_angle_${_b}_wg128 COMMAND dapple_gpu_tests --quick)
    set_tests_properties(gles_gpu_parity_angle_${_b}_wg128 PROPERTIES TIMEOUT 3600 SKIP_RETURN_CODE 77 ENVIRONMENT "${_env};DAPPLE_GPU_WG=128")
  endforeach()
endif()
