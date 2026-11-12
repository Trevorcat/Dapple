# Metal backend of the compute HAL (docs/ios/metal-backend.md). Included from core/gpu/CMakeLists.txt on Apple platforms, after
# `dapple_gpu` and the shader list DAPPLE_GPU_SHADERS exist. It
#   * removes the Vulkan-only sources from dapple_gpu (hal/vk/vk_device.cpp, volk.c),
#   * adds hal/metal/metal_device.mm (+ a Vulkan-slot alias, see the end of that file),
#   * generates the shaders: glslc -> SPIR-V -> dapple_shaderc --msl (SPIRV-Cross) -> `xcrun metal` -> one metallib embedded in the
#     library (no shader compilation on the device),
# Variables: DAPPLE_GLSLC (glslc), DAPPLE_SHADERC_EXE (prebuilt host dapple_shaderc, else built natively with ExternalProject),
#            DAPPLE_METAL_EMBED_SOURCE (ON = also embed the MSL text and allow compiling it at run time when the metallib is absent).
get_filename_component(_mt_dir "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(_root_dir "${_mt_dir}/../../../.." ABSOLUTE)

get_target_property(_srcs dapple_gpu SOURCES)
list(FILTER _srcs EXCLUDE REGEX "(vk_device\.cpp|volk\.c)$")
set_property(TARGET dapple_gpu PROPERTY SOURCES "${_srcs}")

# SDK name and minimum OS for `xcrun metal`
set(_sysroot "${CMAKE_OSX_SYSROOT}")
if(_sysroot MATCHES "[Ss]imulator")
  set(_mt_sdk iphonesimulator)
  set(_mt_min "${CMAKE_OSX_DEPLOYMENT_TARGET}")
  if(NOT _mt_min)
    set(_mt_min 16.0)
  endif()
  set(_mt_minflag "-mios-simulator-version-min=${_mt_min}")
elseif(_sysroot MATCHES "iphoneos|iPhoneOS")
  set(_mt_sdk iphoneos)
  set(_mt_min "${CMAKE_OSX_DEPLOYMENT_TARGET}")
  if(NOT _mt_min)
    set(_mt_min 16.0)
  endif()
  set(_mt_minflag "-mios-version-min=${_mt_min}")
else()
  set(_mt_sdk macosx)
  set(_mt_min "${CMAKE_OSX_DEPLOYMENT_TARGET}")
  if(NOT _mt_min)
    set(_mt_min 12.0)
  endif()
  set(_mt_minflag "-mmacosx-version-min=${_mt_min}")
endif()

if(_mt_sdk STREQUAL macosx)
  set(_mt_std macos-metal2.3)
else()
  set(_mt_std ios-metal2.3)
endif()

# ---- host tool (SPIR-V -> MSL), always a native macOS build ----
if(DAPPLE_SHADERC_EXE)
  set(_shaderc "${DAPPLE_SHADERC_EXE}")
  set(_shaderc_dep "")
else()
  if(NOT TARGET dapple_shaderc_host)
    include(ExternalProject)
    set(_hostdir "${CMAKE_BINARY_DIR}/dapple_shaderc_host")
    ExternalProject_Add(dapple_shaderc_host
      SOURCE_DIR "${_root_dir}/tools/shaderc"
      BINARY_DIR "${_hostdir}"
      CMAKE_GENERATOR "${CMAKE_GENERATOR}"
      CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_SYSROOT=macosx -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0
                 -DCMAKE_OSX_ARCHITECTURES=${CMAKE_HOST_SYSTEM_PROCESSOR}
      BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target dapple_shaderc
      INSTALL_COMMAND ""
      BUILD_ALWAYS 0
      BUILD_BYPRODUCTS "${_hostdir}/dapple_shaderc")
  endif()
  set(_shaderc "${CMAKE_BINARY_DIR}/dapple_shaderc_host/dapple_shaderc")
  set(_shaderc_dep dapple_shaderc_host)
endif()

# ---- per shader ----
set(_mgen "${CMAKE_CURRENT_BINARY_DIR}/metal_gen")
file(MAKE_DIRECTORY "${_mgen}")
set(_airs "")
set(_incs "")
set(_tab "")
set(_hash_src "")
foreach(_entry ${DAPPLE_GPU_SHADERS})
  string(REPLACE "|" ";" _p "${_entry}")
  list(GET _p 0 _name)
  list(GET _p 1 _src)
  list(GET _p 2 _defs)
  set(_dargs "")
  if(_defs)
    string(REPLACE "," ";" _dl "${_defs}")
    foreach(_d ${_dl})
      list(APPEND _dargs "-D${_d}")
    endforeach()
  endif()
  set(_spv "${_mgen}/${_name}.spv")
  set(_air "${_mgen}/${_name}.air")
  add_custom_command(OUTPUT "${_air}" "${_mgen}/${_name}_msl_entry.inc" "${_mgen}/${_name}_msl.h"
    COMMAND "${DAPPLE_GLSLC}" -O --target-env=vulkan1.0 -fshader-stage=compute ${_dargs}
            -I "${_SH}" "${_SH}/${_src}" -o "${_spv}"
    COMMAND "${_shaderc}" --name ${_name} --spv "${_spv}" --out-dir "${_mgen}" --msl
    COMMAND xcrun -sdk ${_mt_sdk} metal -std=${_mt_std} ${_mt_minflag} -fno-fast-math -O2 -c "${_mgen}/${_name}.metal" -o "${_air}"
    DEPENDS "${_SH}/${_src}" "${_SH}/common.glsl" ${_shaderc_dep} "${_shaderc}"
    COMMENT "MSL ${_name} (${_src})" VERBATIM)
  list(APPEND _airs "${_air}")
  string(APPEND _tab "    #include \"${_name}_msl_entry.inc\"\n")
  if(DAPPLE_METAL_EMBED_SOURCE)
    string(APPEND _incs "#include \"${_name}_msl.h\"\n")
  endif()
endforeach()

set(_metallib "${_mgen}/dapple.metallib")
add_custom_command(OUTPUT "${_metallib}"
  COMMAND xcrun -sdk ${_mt_sdk} metallib ${_airs} -o "${_metallib}"
  DEPENDS ${_airs} COMMENT "metallib" VERBATIM)
add_custom_command(OUTPUT "${_mgen}/dapple_metallib.h"
  COMMAND ${CMAKE_COMMAND} -DSPV=${_metallib} -DOUT=${_mgen}/dapple_metallib.h -DNAME=g_dapple_metallib
          -P "${_SH}/cmake/embed_spv.cmake"
  DEPENDS "${_metallib}" "${_SH}/cmake/embed_spv.cmake" COMMENT "embed metallib" VERBATIM)
if(DAPPLE_METAL_EMBED_SOURCE)
  set(_msl_macro "#define DAPPLE_MSL_SRC(n) g_##n##_msl")
else()
  set(_msl_macro "#define DAPPLE_MSL_SRC(n) nullptr")
endif()
file(GENERATE OUTPUT "${_mgen}/metal_shader_table.inc"
  CONTENT "#include \"dapple_metallib.h\"\n${_incs}\n${_msl_macro}\nconst unsigned char* const kMetallib = g_dapple_metallib;\nconst std::size_t kMetallibSize = sizeof(g_dapple_metallib);\nconst MslBin kShaders[] = {\n${_tab}};\nconst std::size_t kShaderCount = sizeof(kShaders) / sizeof(kShaders[0]);\n")

target_sources(dapple_gpu PRIVATE "${_mt_dir}/metal_device.mm" "${_mgen}/dapple_metallib.h")
set_source_files_properties("${_mt_dir}/metal_device.mm" PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-x;objective-c++;-fobjc-arc")
target_include_directories(dapple_gpu PRIVATE "${_mgen}")
target_compile_definitions(dapple_gpu PUBLIC DAPPLE_HAL_METAL=1)
target_link_libraries(dapple_gpu PUBLIC "-framework Metal" "-framework Foundation" "-framework QuartzCore")

if(DAPPLE_BUILD_TESTS AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  # ---- Metal specific tests + the registration of the Apple variants of the shared suites ----
  add_executable(dapple_metal_tests "${_mt_dir}/metal_tests.mm")
  set_source_files_properties("${_mt_dir}/metal_tests.mm" PROPERTIES LANGUAGE CXX COMPILE_OPTIONS "-x;objective-c++;-fobjc-arc")
  target_link_libraries(dapple_metal_tests PRIVATE dapple_gpu)
  target_include_directories(dapple_metal_tests PRIVATE "${PROJECT_SOURCE_DIR}/core")
  # tests/CMakeLists.txt is not Apple aware: an extra test include file adds metal.*; a deferred call (end of the root
  # CMakeLists) disables engine.gpu_external (it checks Vulkan swapchain handles; the Metal seam is metal.external).
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/metal_tests.cmake" CONTENT
  "add_test(metal.caps \"$<TARGET_FILE:dapple_metal_tests>\" caps)
  add_test(metal.external \"$<TARGET_FILE:dapple_metal_tests>\" external)
  set_tests_properties(metal.caps metal.external PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
  ")
  set_property(GLOBAL PROPERTY DAPPLE_METAL_TESTFILE "${CMAKE_CURRENT_BINARY_DIR}/metal_tests.cmake")
  function(dapple_metal_register_tests)
    get_property(_f GLOBAL PROPERTY DAPPLE_METAL_TESTFILE)
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}/tests" APPEND PROPERTY TEST_INCLUDE_FILES "${_f}")
    if(TARGET dapple_gpu_engine_tests)
      set_tests_properties(engine.gpu_external DIRECTORY "${PROJECT_SOURCE_DIR}/tests" PROPERTIES DISABLED TRUE)
    endif()
  endfunction()
  cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL dapple_metal_register_tests)
endif()
