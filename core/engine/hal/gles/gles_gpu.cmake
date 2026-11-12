# GLES 3.1 backend inside the full tree build (include when -DDAPPLE_GLES=ON, after the tests directory):
#   * dapple_gles_check                  smoke / self-test tool (needs no Vulkan, no core)
#   * ctest gles_selftest_angle_vulkan / gles_selftest_angle_swiftshader   (skipped cleanly: exit 77 when ANGLE or a device is absent)
#   * dapple_gpu_gles + ctest gles_engine_<t>_angle_*   the UNMODIFIED GPU executor / nodes / scheduler tests with the GLES device in the
#     Vulkan slot (gles_as_vulkan.cpp). Needs -DDAPPLE_GPU=ON for the shader list / sources; requires TARGET dapple_core.
include("${CMAKE_CURRENT_LIST_DIR}/gles.cmake")

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

if(TARGET dapple_core AND TARGET dapple_gpu)
  set(_E "${PROJECT_SOURCE_DIR}/core/engine")
  add_library(dapple_gpu_gles STATIC
    ${_E}/hal/hal.cpp ${CMAKE_CURRENT_LIST_DIR}/gles_as_vulkan.cpp ${_E}/exec/gpu_scheduler.cpp ${_E}/exec/gpu_executor.cpp ${_E}/exec/gpu_frame.cpp
    ${_E}/nodes/gpu_common.cpp ${_E}/nodes/point/gpu.cpp ${_E}/nodes/nr/gpu.cpp ${_E}/nodes/dehaze/gpu.cpp ${_E}/nodes/detail/gpu.cpp
    ${_E}/nodes/resize/gpu.cpp ${_E}/nodes/gpu_nodes.cpp
    ${DAPPLE_FILM_GPU_SOURCES}
    ${PROJECT_SOURCE_DIR}/core/gpu/src/parity.cpp ${PROJECT_SOURCE_DIR}/core/gpu/src/hook.cpp)
  target_include_directories(dapple_gpu_gles PUBLIC ${PROJECT_SOURCE_DIR}/core/gpu/include
    PRIVATE "${PROJECT_SOURCE_DIR}/cli" "${PROJECT_SOURCE_DIR}/core" "${PROJECT_SOURCE_DIR}/core/gpu/src" "${PROJECT_SOURCE_DIR}/core/src")
  target_link_libraries(dapple_gpu_gles PUBLIC dapple_core dapple_hal_gles)
  if(NOT MSVC)
    target_compile_options(dapple_gpu_gles PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  endif()

  # the C API library variant that carries the GLES device behind the gpu hook (the original `dapple` carries Vulkan)
  add_library(dapple_gles SHARED ${PROJECT_SOURCE_DIR}/core/src/c_api.cpp ${PROJECT_SOURCE_DIR}/core/src/film_swatch.cpp)
  target_include_directories(dapple_gles PUBLIC ${PROJECT_SOURCE_DIR}/core/include)
  target_link_libraries(dapple_gles PRIVATE dapple_core dapple_gpu_gles)
  target_compile_definitions(dapple_gles PRIVATE DAPPLE_VERSION_STRING="${PROJECT_VERSION}" DAPPLE_HAVE_GPU=1)
  set_target_properties(dapple_gles PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
  add_executable(dapple_gles_gpu_tests ${PROJECT_SOURCE_DIR}/core/gpu/tests/gpu_tests.cpp)
  target_link_libraries(dapple_gles_gpu_tests PRIVATE dapple_gpu_gles dapple_core dapple_gles)
  target_include_directories(dapple_gles_gpu_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/gpu/src ${PROJECT_SOURCE_DIR}/cli)

  add_executable(dapple_gles_engine_tests ${PROJECT_SOURCE_DIR}/tests/engine/gpu/gpu_engine_tests.cpp)
  target_link_libraries(dapple_gles_engine_tests PRIVATE dapple_gpu_gles dapple_core)
  target_include_directories(dapple_gles_engine_tests PRIVATE ${PROJECT_SOURCE_DIR}/core ${PROJECT_SOURCE_DIR}/core/gpu/src ${PROJECT_SOURCE_DIR}/cli)
  if(_angle_ok)
    foreach(_b vulkan swiftshader)
      add_test(NAME gles_gpu_parity_angle_${_b} COMMAND dapple_gles_gpu_tests)
      set_tests_properties(gles_gpu_parity_angle_${_b} PROPERTIES TIMEOUT 3600 SKIP_RETURN_CODE 77
        ENVIRONMENT "DAPPLE_ANGLE_DIR=${DAPPLE_ANGLE_DIR};DAPPLE_ANGLE_BACKEND=${_b};DAPPLE_GPU_SOFTWARE=1")
      foreach(_t hal box sched)
        add_test(NAME gles_engine_${_t}_angle_${_b} COMMAND dapple_gles_engine_tests ${_t})
        set_tests_properties(gles_engine_${_t}_angle_${_b} PROPERTIES TIMEOUT 1800 SKIP_RETURN_CODE 77
          ENVIRONMENT "DAPPLE_ANGLE_DIR=${DAPPLE_ANGLE_DIR};DAPPLE_ANGLE_BACKEND=${_b};DAPPLE_GPU_SOFTWARE=1")
      endforeach()
    endforeach()
  endif()
endif()
