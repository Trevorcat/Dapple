# GLES 3.1 backend of the compute HAL (docs/engine/pipeline-architecture.md 6.3, 7.1, 10.4 stage c).
#
#   include(engine/hal/gles/gles.cmake)      # from core/CMakeLists.txt or any consumer, after the GPU shader list is defined
#
# Provides the static library `dapple_hal_gles` (define DAPPLE_HAL_GLES for hal.cpp to dispatch Api::Gles31 to
# dapple::hal::gles::createGlesDevice). The ESSL sources are GENERATED at build time:
#   glslc (SPIR-V, as for Vulkan)  ->  dapple_shaderc (host tool over SPIRV-Cross)  ->  ESSL 310 + manifest + table entry
#   ->  lint: glslc -std=310es on the generated ESSL (catches ES rule violations a permissive desktop GL accepts).
#
# Inputs (variables):
#   DAPPLE_GPU_SHADERS   list of "name|source.comp|DEFINE=1,DEFINE2|binding signature" (same list the Vulkan backend embeds);
#                        when it is not set, gles/shader_list.cmake is used (kept identical by tests/engine/check_shader_lists)
#   DAPPLE_SHADER_DIR    directory with the .comp files and common.glsl (default core/engine/shaders)
#   DAPPLE_GLSLC         glslc executable (found in the NDK shader-tools like the Vulkan build)
#   DAPPLE_SHADERC_EXE   prebuilt host dapple_shaderc (otherwise built with ExternalProject for the BUILD machine)
#   DAPPLE_SHADERC_MSL   ON = also write MSL next to the ESSL (stage Metal)
#   DAPPLE_KHRONOS_GL    include dir with EGL/ GLES2/ GLES3/ KHR/ headers (default tools/_dl/khronos-gl/include)
get_filename_component(_gles_dir "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(_core_dir "${_gles_dir}/../../.." ABSOLUTE)
get_filename_component(_root_dir "${_core_dir}/.." ABSOLUTE)

if(NOT DAPPLE_SHADER_DIR)
  set(DAPPLE_SHADER_DIR "${_core_dir}/engine/shaders")
endif()
if(NOT DAPPLE_KHRONOS_GL)
  set(DAPPLE_KHRONOS_GL "${_root_dir}/tools/_dl/khronos-gl/include")
endif()
if(NOT DAPPLE_GPU_SHADERS)
  include("${_gles_dir}/shader_list.cmake")
endif()
if(NOT DAPPLE_GLSLC)
  file(GLOB _glslc_hint "${_root_dir}/tools/android-sdk/ndk/*/shader-tools/windows-x86_64"
                        "${_root_dir}/tools/android-sdk/ndk/*/shader-tools/linux-x86_64"
                        "${_root_dir}/tools/android-sdk/ndk/*/shader-tools/darwin-x86_64")
  find_program(DAPPLE_GLSLC glslc HINTS ${_glslc_hint} NO_CMAKE_FIND_ROOT_PATH)
endif()
if(NOT DAPPLE_GLSLC)
  message(FATAL_ERROR "dapple_hal_gles: glslc not found (set -DDAPPLE_GLSLC=...)")
endif()

# ---- host tool ----
if(DAPPLE_SHADERC_EXE)
  set(_shaderc "${DAPPLE_SHADERC_EXE}")
  set(_shaderc_dep "")
else()
  include(ExternalProject)
  set(_hostdir "${CMAKE_BINARY_DIR}/dapple_shaderc_host")
  set(_exe "${CMAKE_EXECUTABLE_SUFFIX}")
  if(CMAKE_HOST_WIN32)
    set(_exe ".exe")
  else()
    set(_exe "")
  endif()
  set(_host_args -DCMAKE_BUILD_TYPE=Release)
  if(DAPPLE_HOST_CXX_COMPILER)
    list(APPEND _host_args -DCMAKE_CXX_COMPILER=${DAPPLE_HOST_CXX_COMPILER})
  endif()
  if(CMAKE_GENERATOR)
    set(_host_gen "${CMAKE_GENERATOR}")
  endif()
  ExternalProject_Add(dapple_shaderc_host
    SOURCE_DIR "${_root_dir}/tools/shaderc"
    BINARY_DIR "${_hostdir}"
    CMAKE_GENERATOR "${_host_gen}"
    CMAKE_ARGS ${_host_args}
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target dapple_shaderc
    INSTALL_COMMAND ""
    BUILD_ALWAYS 0
    BUILD_BYPRODUCTS "${_hostdir}/dapple_shaderc${_exe}")
  set(_shaderc "${_hostdir}/dapple_shaderc${_exe}")
  set(_shaderc_dep dapple_shaderc_host)
endif()

# ---- per shader: glslc -> spv -> shaderc -> lint ----
set(_gen "${CMAKE_CURRENT_BINARY_DIR}/gles_gen")
file(MAKE_DIRECTORY "${_gen}")
set(_msl_flag "")
if(DAPPLE_SHADERC_MSL)
  set(_msl_flag --msl)
endif()
set(_stamps "")
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
  set(_spv "${_gen}/${_name}.spv")
  set(_stamp "${_gen}/${_name}.lint.stamp")
  set(_ppflag "")
  if(_defs MATCHES "ES_PINGPONG")
    set(_ppflag --pingpong)
  endif()
  add_custom_command(OUTPUT "${_stamp}" "${_gen}/${_name}_gles.h" "${_gen}/${_name}_entry.inc"
    COMMAND "${DAPPLE_GLSLC}" -O --target-env=vulkan1.0 -fshader-stage=compute ${_dargs}
            -I "${DAPPLE_SHADER_DIR}" "${DAPPLE_SHADER_DIR}/${_src}" -o "${_spv}"
    COMMAND "${_shaderc}" --name ${_name} --spv "${_spv}" --out-dir "${_gen}" ${_msl_flag} ${_ppflag}
    # ES rule lint: the generated ESSL must be accepted by glslang as #version 310 es
    COMMAND "${DAPPLE_GLSLC}" -std=310es --target-env=opengl -fshader-stage=compute -c "${_gen}/${_name}.essl" -o "${_gen}/${_name}.lint.spv"
    COMMAND ${CMAKE_COMMAND} -E touch "${_stamp}"
    DEPENDS "${DAPPLE_SHADER_DIR}/${_src}" "${DAPPLE_SHADER_DIR}/common.glsl" ${_shaderc_dep} "${_shaderc}"
    COMMENT "ESSL ${_name} (${_src})" VERBATIM)
  list(APPEND _stamps "${_stamp}")
  string(APPEND _incs "#include \"${_name}_gles.h\"\n")
  string(APPEND _tab "    #include \"${_name}_entry.inc\"\n")
  file(SHA256 "${DAPPLE_SHADER_DIR}/${_src}" _h)
  string(APPEND _hash_src "${_name}${_defs}${_h}")
endforeach()
file(SHA256 "${DAPPLE_SHADER_DIR}/common.glsl" _hc)
string(SHA256 _set_hash "${_hash_src}${_hc}")
string(SUBSTRING "${_set_hash}" 0 16 _set_hash)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${DAPPLE_SHADER_DIR}/common.glsl")
file(GENERATE OUTPUT "${_gen}/gles_shader_table.inc"
  CONTENT "${_incs}\nconst char* const kShaderSetHash = \"${_set_hash}\";\nconst GlesShaderBin kShaders[] = {\n${_tab}};\nconst std::size_t kShaderCount = sizeof(kShaders) / sizeof(kShaders[0]);\n")

add_custom_target(dapple_gles_shaders DEPENDS ${_stamps})
add_library(dapple_hal_gles STATIC "${_gles_dir}/gles_device.cpp" "${_gles_dir}/gles_selftest.cpp")
add_dependencies(dapple_hal_gles dapple_gles_shaders)
target_include_directories(dapple_hal_gles
  PUBLIC "${_core_dir}"
  PRIVATE "${_gen}" "${DAPPLE_KHRONOS_GL}")
target_compile_features(dapple_hal_gles PUBLIC cxx_std_17)
find_package(Threads)
if(Threads_FOUND)
  target_link_libraries(dapple_hal_gles PUBLIC Threads::Threads)
endif()
if(NOT WIN32)
  target_link_libraries(dapple_hal_gles PUBLIC ${CMAKE_DL_LIBS})
endif()
if(ANDROID)
  target_link_libraries(dapple_hal_gles PUBLIC log)
endif()
if(NOT MSVC)
  target_compile_options(dapple_hal_gles PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# backend independent selection logic (Vulkan -> GLES -> CPU); compiled here until the Vulkan side includes it too
target_sources(dapple_hal_gles PRIVATE "${_gles_dir}/../select.cpp")
target_compile_definitions(dapple_hal_gles PUBLIC DAPPLE_HAL_GLES=1)

# smoke / self-test tool; links hal.cpp itself and stubs the Vulkan factory (so it builds without the Vulkan backend)
add_executable(dapple_gles_check "${_gles_dir}/gles_check.cpp" "${_gles_dir}/../hal.cpp")
target_compile_definitions(dapple_gles_check PRIVATE DAPPLE_GLES_CHECK_STUB_VK=1)
target_link_libraries(dapple_gles_check PRIVATE dapple_hal_gles)
