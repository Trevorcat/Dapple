add_library(dapple_core STATIC
  src/auto_adjust.cpp
  src/camera.cpp
  src/developer.cpp
  src/edit_params.cpp
  src/edit_params_json.cpp
  src/encode.cpp
  src/image.cpp
  src/hdr.cpp
  src/ingest.cpp
  src/look.cpp
  src/lut.cpp
  src/merge_io.cpp
  src/motion_photo.cpp
  src/pano.cpp
  src/renderer.cpp
  src/sequence.cpp
  src/spatial.cpp
  src/tone.cpp
  src/xmp_preset.cpp
  src/decoders/jpeg_decoder.cpp
  src/decoders/libraw_decoder.cpp
)
add_library(Dapple::core ALIAS dapple_core)

target_include_directories(dapple_core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(dapple_core PRIVATE dapple_libraw dapple_stb)
if(TARGET dapple_opencv)
  target_link_libraries(dapple_core PRIVATE dapple_opencv)
endif()

if(MSVC)
  target_compile_options(dapple_core PRIVATE /W4 /utf-8)
else()
  target_compile_options(dapple_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# ---------------------------------------------------------------------------
# Shared library for FFI (Flutter / dart:ffi). Exports only the C API from
# dapple_c.h; the C++ core is linked in statically. On Android this is
# libdapple.so.
# ---------------------------------------------------------------------------
add_library(dapple SHARED src/c_api.cpp)
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
  target_link_libraries(dapple PRIVATE log)
  # 16 KB page size support for Android 15+ devices
  target_link_options(dapple PRIVATE -Wl,-z,max-page-size=16384)
endif()
