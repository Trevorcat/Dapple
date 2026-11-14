# Dapple GPU compute backend (docs/engine/gpu-backend.md). Included from the top-level CMakeLists only with
# -DDAPPLE_GPU=ON (default OFF), so nothing else changes.
#
# Dependencies (permissive): Vulkan-Headers (Apache-2.0 / MIT), volk (MIT; loads vulkan-1.dll / libvulkan.so at
# run time, so there is no link-time Vulkan dependency and a device without Vulkan simply fails Backend::create()).
# Shaders: GLSL -> SPIR-V with glslc at build time (the Android NDK ships one: tools/android-sdk/ndk/*/shader-tools).
set(DAPPLE_VULKAN_DL "${PROJECT_SOURCE_DIR}/tools/_dl/vulkan" CACHE PATH "Vulkan-Headers + volk sources")
set(_hdr "${DAPPLE_VULKAN_DL}/Vulkan-Headers-1.4.309")
set(_volk "${DAPPLE_VULKAN_DL}/volk-1.4.304")
if(NOT EXISTS "${_hdr}/include/vulkan/vulkan.h")
  include(FetchContent)
  FetchContent_Declare(vkheaders
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.309.tar.gz
    URL_HASH SHA256=437925ada160d86ed763d29dcb9318c1bb0d024d7deaf77bc7c170b8eb6b6f10)
  FetchContent_Declare(volk
    URL https://github.com/zeux/volk/archive/refs/tags/1.4.304.tar.gz
    URL_HASH SHA256=ab3d4a8ccaeb32652259cdd008399504a41792675b0421d90b67729ee274746f)
  FetchContent_Populate(vkheaders)
  FetchContent_Populate(volk)
  set(_hdr "${vkheaders_SOURCE_DIR}")
  set(_volk "${volk_SOURCE_DIR}")
endif()

if(NOT DAPPLE_GLSLC)
  file(GLOB _glslc_hint "${PROJECT_SOURCE_DIR}/tools/android-sdk/ndk/*/shader-tools/windows-x86_64"
                        "${PROJECT_SOURCE_DIR}/tools/android-sdk/ndk/*/shader-tools/linux-x86_64"
                        "${PROJECT_SOURCE_DIR}/tools/android-sdk/ndk/*/shader-tools/darwin-x86_64")
  find_program(DAPPLE_GLSLC glslc HINTS ${_glslc_hint} NO_CMAKE_FIND_ROOT_PATH)
endif()
if(NOT DAPPLE_GLSLC)
  message(FATAL_ERROR "DAPPLE_GPU: glslc not found (set -DDAPPLE_GLSLC=...)")
endif()

# name | source | defines (comma separated) | descriptor bindings after the uniform block (I storage image,
# S sampled image + sampler, B storage buffer). The binding string is checked against the shader by the self-test.
set(_SH "${PROJECT_SOURCE_DIR}/core/engine/shaders")
set(DAPPLE_GPU_SHADERS
  "point|point.comp||SBSSIIB"
  "point_local|point.comp|LOCAL=1|SBSSIIBBBII"
  "local_detail|local_detail.comp||SSSSSSII"
  "local_film_in|local_film_in.comp||SIBBII"
  "local_heal|local_heal.comp||SBII"
  "local_rt_down|local_rt_down.comp||SSSII"
  "local_retouch|local_retouch.comp||SSSSSSSII"
  "sep_d2y|sepconv.comp|IN_FMT=rgba16f,OUT_FMT=r32f,IN_MODE=1,SCALAR=1|SIB"
  "sep_d2y_v|sepconv.comp|BIGV=1,IN_FMT=rgba16f,OUT_FMT=r32f,IN_MODE=1,SCALAR=1|SIB"
  "sep_d2y_s|sepconv.comp|SMALL=1,IN_FMT=rgba16f,OUT_FMT=r32f,IN_MODE=1,SCALAR=1|SIB"
  "sep_c2y|sepconv.comp|IN_FMT=rgba32f,OUT_FMT=r32f,IN_MODE=2,SCALAR=1|SIB"
  "sep_c2y_v|sepconv.comp|BIGV=1,IN_FMT=rgba32f,OUT_FMT=r32f,IN_MODE=2,SCALAR=1|SIB"
  "sep_c2y_s|sepconv.comp|SMALL=1,IN_FMT=rgba32f,OUT_FMT=r32f,IN_MODE=2,SCALAR=1|SIB"
  "sep_c|sepconv.comp|IN_FMT=rgba32f,OUT_FMT=rgba32f,IN_MODE=0|SIB"
  "sep_c_s|sepconv.comp|SMALL=1,IN_FMT=rgba32f,OUT_FMT=rgba32f,IN_MODE=0|SIB"
  "sep_y|sepconv.comp|IN_FMT=r32f,OUT_FMT=r32f,IN_MODE=0,SCALAR=1|SIB"
  "sep_y_v|sepconv.comp|BIGV=1,IN_FMT=r32f,OUT_FMT=r32f,IN_MODE=0,SCALAR=1|SIB"
  "sep_y_s|sepconv.comp|SMALL=1,IN_FMT=r32f,OUT_FMT=r32f,IN_MODE=0,SCALAR=1|SIB"
  "down|down.comp||SI"
  "nr_luma|nr_luma.comp||ISI"
  "nr_chroma|nr_chroma.comp||ISI"
  "dehaze_apply|dehaze_apply.comp||ISBI"
  "reduce|reduce.comp||SSBB"
  "gf_ab|gf_ab.comp||SI"
  "detail|detail.comp||ISSSI"
  "resize|resize.comp||SI"
  "convert|convert.comp||BI"
  "bx_d2y|boxscan.comp|IN_MODE=1|SI"
  "bx_y|boxscan.comp|IN_MODE=0|SI"
  "bx_y_v|boxscan.comp|VERT=1,IN_MODE=0|SI")

# film nodes (engineer: film): shader variants + sources
include("${PROJECT_SOURCE_DIR}/core/engine/nodes/film/film_gpu.cmake")
list(APPEND DAPPLE_GPU_SHADERS ${DAPPLE_FILM_GPU_SHADERS})
# demosaic node (pipeline-architecture.md section 32)
include("${PROJECT_SOURCE_DIR}/core/engine/nodes/demosaic/demosaic_gpu.cmake")
list(APPEND DAPPLE_GPU_SHADERS ${DAPPLE_DEMOSAIC_GPU_SHADERS})

set(_gen "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${_gen}")
set(_spv_headers "")
set(_inc_headers "")
set(_inc_table "")
foreach(_entry ${DAPPLE_GPU_SHADERS})
  string(REPLACE "|" ";" _p "${_entry}")
  list(GET _p 0 _name)
  list(GET _p 1 _src)
  list(GET _p 2 _defs)
  list(GET _p 3 _bind)
  set(_dargs "")
  if(_defs)
    string(REPLACE "," ";" _dl "${_defs}")
    foreach(_d ${_dl})
      list(APPEND _dargs "-D${_d}")
    endforeach()
  endif()
  set(_spv "${_gen}/${_name}.spv")
  set(_h "${_gen}/${_name}_spv.h")
  add_custom_command(OUTPUT "${_h}"
    COMMAND "${DAPPLE_GLSLC}" -O --target-env=vulkan1.0 -fshader-stage=compute ${_dargs}
            -I "${_SH}"
            "${_SH}/${_src}" -o "${_spv}"
    COMMAND ${CMAKE_COMMAND} -DSPV=${_spv} -DOUT=${_h} -DNAME=g_${_name}_spv
            -P "${_SH}/cmake/embed_spv.cmake"
    DEPENDS "${_SH}/${_src}" "${_SH}/common.glsl" "${_SH}/local.glsl" "${_SH}/warp.glsl" "${_SH}/warp_lanczos.glsl"
            "${_SH}/cmake/embed_spv.cmake"
    COMMENT "glslc ${_name} (${_src})")
  list(APPEND _spv_headers "${_h}")
  string(APPEND _inc_headers "#include \"${_name}_spv.h\"\n")
  string(APPEND _inc_table "    {\"${_name}\", g_${_name}_spv, sizeof(g_${_name}_spv), \"${_bind}\"},\n")
endforeach()
# the table is included inside namespace dapple::gpu::vk by vk_core.cpp
file(GENERATE OUTPUT "${_gen}/shader_table.inc"
  CONTENT "${_inc_headers}\nconst ShaderBin kShaders[] = {\n${_inc_table}};\nconst std::size_t kShaderCount = sizeof(kShaders) / sizeof(kShaders[0]);\n")

set(_E "${PROJECT_SOURCE_DIR}/core/engine")
# GLES 3.1 backend (hal/gles): a fallback device behind Vulkan when -DDAPPLE_GLES=ON (selected by hal/select.cpp, 5.9 / 6.6)
# (in a function: gles.cmake sets variables of its own, e.g. _gen, that this file still uses)
function(dapple_add_gles_backend)
  unset(DAPPLE_GPU_SHADERS)  # the GLES list (hal/gles/shader_list.cmake) has the ES_PINGPONG variants; the Vulkan list is already embedded
  include("${_E}/hal/gles/gles.cmake")
endfunction()
if(DAPPLE_GLES)
  dapple_add_gles_backend()
endif()
add_library(dapple_gpu STATIC
  ${_E}/hal/hal.cpp ${_E}/hal/select.cpp ${_E}/hal/vk/vk_device.cpp ${_E}/exec/gpu_scheduler.cpp ${_E}/exec/gpu_executor.cpp ${_E}/exec/gpu_context.cpp
  ${_E}/nodes/gpu_common.cpp ${_E}/nodes/point/gpu.cpp ${_E}/nodes/nr/gpu.cpp ${_E}/nodes/dehaze/gpu.cpp ${_E}/nodes/detail/gpu.cpp ${_E}/nodes/local/gpu.cpp
  ${_E}/nodes/resize/gpu.cpp ${_E}/nodes/gpu_nodes.cpp ${DAPPLE_FILM_GPU_SOURCES} ${DAPPLE_DEMOSAIC_GPU_SOURCES}
  src/parity.cpp src/gpu_bench.cpp ${_spv_headers})
if(TARGET dapple_hal_gles)
  target_link_libraries(dapple_gpu PUBLIC dapple_hal_gles)
endif()
target_include_directories(dapple_gpu
  PRIVATE "${_hdr}/include" "${_volk}" "${_gen}" "${PROJECT_SOURCE_DIR}/cli" "${PROJECT_SOURCE_DIR}/core" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(dapple_gpu PRIVATE "${PROJECT_SOURCE_DIR}/core/src")  # film nodes read the film tables
target_compile_definitions(dapple_gpu PUBLIC DAPPLE_HAVE_GPU=1 PRIVATE VK_NO_PROTOTYPES)
target_sources(dapple_gpu PRIVATE "${_volk}/volk.c")
set_source_files_properties("${_volk}/volk.c" PROPERTIES LANGUAGE C)
if(APPLE)
  # Apple GPUs: Metal backend instead of Vulkan (core/engine/hal/metal/metal.cmake removes the Vulkan sources and adds the Metal ones)
  include("${_E}/hal/metal/metal.cmake")
endif()
target_link_libraries(dapple_gpu PUBLIC dapple_core)
if(NOT WIN32)
  target_link_libraries(dapple_gpu PUBLIC ${CMAKE_DL_LIBS})
endif()
if(ANDROID)
  target_link_libraries(dapple_gpu PUBLIC log)
endif()
find_package(Threads)
if(Threads_FOUND)
  target_link_libraries(dapple_gpu PUBLIC Threads::Threads)
endif()
if(NOT MSVC)
  target_compile_options(dapple_gpu PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

# The C API library (libdapple.so / dapple.dll) carries the backend behind the runtime flag (default off), see
# core/engine/exec/gpu_runtime.h (frames and exports go through EngineSession). A build without DAPPLE_GPU does not contain any of it.
if(TARGET dapple)
  target_link_libraries(dapple PRIVATE dapple_gpu)
endif()
