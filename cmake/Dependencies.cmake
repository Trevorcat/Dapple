include(FetchContent)

# ---------------------------------------------------------------------------
# LibRaw (dual-licensed LGPL-2.1 / CDDL-1.0; Dapple uses it under CDDL-1.0 so
# it can be statically linked into mobile apps).
# LibRaw ships no CMakeLists, so we only populate the sources (SOURCE_SUBDIR
# points at a directory that does not exist) and build the library ourselves.
# Offline builds: pass -DFETCHCONTENT_SOURCE_DIR_LIBRAW=<path to the unpacked snapshot below>
# Version: the upstream master snapshot 202611 (LibRaw 0.23.0 Snapshot202611, commit a9a14b19, 2026-11-04), not the 0.22.2 release: the release
# cannot open the Sony ILCE-7M5 (A7 V) and cannot decode Nikon HE / HE* (TicoRAW) NEF of the Z6 III / Z8 / Z9 / Z f; the snapshot does both
# (docs/engine/raw-compat.md). No local patches to LibRaw. Switch back to a tagged release as soon as upstream publishes one with these bodies.
# ---------------------------------------------------------------------------
FetchContent_Declare(libraw
  URL https://github.com/LibRaw/LibRaw/archive/a9a14b1970d188c25dade81f8326ff00a6e33649.tar.gz
  URL_HASH SHA256=453b1311b4825cc1946f114b5a0bcf0d4ad62a52c667dd5cc8b2b70e28342020
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
