# JPEG XL decoder for DNG 1.7 (compression 52546: Samsung Galaxy S22 and newer, Adobe DNG Converter). Decoder only: libjxl v0.11.1 (BSD-3-Clause)
# with Highway 1.2.0 (Apache-2.0 / BSD-3-Clause). The encoder, the CMS, boxes (brotli), JPEG recompression and the thread runner are left out:
# the DNG tiles are plain codestreams and each tile is decoded on its own thread by the caller. docs/legal/libjxl.md has the notices.
# Optional: -DDAPPLE_JXL=OFF builds without it (JPEG XL DNGs then fail with a precise error); defaults ON, also on Apple and Android.
option(DAPPLE_JXL "JPEG XL decoder (libjxl) for DNG 1.7 JPEG XL RAW files" ON)
if(NOT DAPPLE_JXL)
  return()
endif()

FetchContent_Declare(libjxl
  URL https://github.com/libjxl/libjxl/archive/refs/tags/v0.11.1.tar.gz
  URL_HASH SHA256=1492dfef8dd6c3036446ac3b340005d92ab92f7d48ee3271b5dac1d36945d3d9
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR _no_cmake_)
FetchContent_Declare(highway
  URL https://github.com/google/highway/archive/refs/tags/1.2.0.tar.gz
  URL_HASH SHA256=7e0be78b8318e8bdbf6fa545d2ecb4c90f947df03f7aadc42c1967f019e63343
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(HWY_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_CONTRIB OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(HWY_SYSTEM_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libjxl highway)

# the decoder source list of libjxl (paths relative to <libjxl>/lib)
set(_jxl_lib "${libjxl_SOURCE_DIR}/lib")
include("${_jxl_lib}/jxl_lists.cmake")
set(_jxl_srcs "")
foreach(f IN LISTS JPEGXL_INTERNAL_DEC_SOURCES)
  if(f MATCHES "\\.cc$")
    list(APPEND _jxl_srcs "${_jxl_lib}/${f}")
  endif()
endforeach()

# generated headers: version.h (from version.h.in) and jxl_export.h (static build: nothing to export)
set(_jxl_gen "${CMAKE_CURRENT_BINARY_DIR}/jxl_generated/include/jxl")
set(JPEGXL_MAJOR_VERSION 0)
set(JPEGXL_MINOR_VERSION 11)
set(JPEGXL_PATCH_VERSION 1)
configure_file("${_jxl_lib}/jxl/version.h.in" "${_jxl_gen}/version.h")
file(WRITE "${_jxl_gen}/jxl_export.h" "#ifndef JXL_EXPORT_H\n#define JXL_EXPORT_H\n#define JXL_EXPORT\n#define JXL_NO_EXPORT\n#ifndef JXL_DEPRECATED\n#define JXL_DEPRECATED __attribute__ ((__deprecated__))\n#endif\n#endif\n")
file(WRITE "${_jxl_gen}/jxl_threads_export.h" "#ifndef JXL_THREADS_EXPORT_H\n#define JXL_THREADS_EXPORT_H\n#define JXL_THREADS_EXPORT\n#endif\n")

add_library(dapple_jxl STATIC ${_jxl_srcs})
target_include_directories(dapple_jxl PRIVATE "${libjxl_SOURCE_DIR}" "${_jxl_lib}/include")
target_include_directories(dapple_jxl SYSTEM PUBLIC "${_jxl_lib}/include" "${CMAKE_CURRENT_BINARY_DIR}/jxl_generated/include")
target_compile_definitions(dapple_jxl PRIVATE JXL_INTERNAL_LIBRARY_BUILD JPEGXL_ENABLE_BOXES=0 JPEGXL_ENABLE_TRANSCODE_JPEG=0 JPEGXL_ENABLE_SKCMS=0)
target_compile_options(dapple_jxl PRIVATE -w -fno-rtti)
target_link_libraries(dapple_jxl PUBLIC hwy)
set_target_properties(dapple_jxl PROPERTIES CXX_EXTENSIONS ON POSITION_INDEPENDENT_CODE ON)
