include(FetchContent)

# ---------------------------------------------------------------------------
# LibRaw (dual-licensed LGPL-2.1 / CDDL-1.0; Dapple uses it under CDDL-1.0 so
# it can be statically linked into mobile apps).
# LibRaw ships no CMakeLists, so we only populate the sources (SOURCE_SUBDIR
# points at a directory that does not exist) and build the library ourselves.
# Offline builds: pass -DFETCHCONTENT_SOURCE_DIR_LIBRAW=<path to LibRaw-0.22.2>
# ---------------------------------------------------------------------------
FetchContent_Declare(libraw
  URL https://github.com/LibRaw/LibRaw/archive/refs/tags/0.22.2.tar.gz
  URL_HASH SHA256=627928088300ecde6ca91ffd202e189203f04ad61ad12f0fe9dc57b9a7a0fb3c
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR _no_cmake_)
FetchContent_MakeAvailable(libraw)

file(GLOB_RECURSE DAPPLE_LIBRAW_SOURCES CONFIGURE_DEPENDS "${libraw_SOURCE_DIR}/src/*.cpp")
# *_ph.cpp are placeholder units that are #included by other sources.
list(FILTER DAPPLE_LIBRAW_SOURCES EXCLUDE REGEX "_ph\\.cpp$")

add_library(dapple_libraw STATIC ${DAPPLE_LIBRAW_SOURCES})
target_include_directories(dapple_libraw SYSTEM PUBLIC "${libraw_SOURCE_DIR}")
target_compile_definitions(dapple_libraw PUBLIC LIBRAW_NODLL)
if(MSVC)
  target_compile_options(dapple_libraw PRIVATE /w)
else()
  target_compile_options(dapple_libraw PRIVATE -w)
endif()
if(WIN32)
  target_link_libraries(dapple_libraw PUBLIC ws2_32)
endif()

# ---------------------------------------------------------------------------
# stb_image / stb_image_write (public domain), vendored at a pinned commit.
# ---------------------------------------------------------------------------
add_library(dapple_stb INTERFACE)
target_include_directories(dapple_stb SYSTEM INTERFACE "${PROJECT_SOURCE_DIR}/third_party/stb")

# LibRaw relies on POSIX/GNU extras (M_PI etc.) that strict -std=c++23 hides.
set_target_properties(dapple_libraw PROPERTIES CXX_EXTENSIONS ON)
target_compile_definitions(dapple_libraw PRIVATE _USE_MATH_DEFINES)
