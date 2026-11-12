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

# OpenMP in LibRaw: the X-Trans demosaic (3-pass Markesteijn, the Fujifilm path: 17 s for a 40 MP frame single-threaded) and the other demosaics
# have OpenMP loops (LIBRAW_USE_OPENMP). Not available on Apple (the iOS / macOS toolchains ship no OpenMP): there the demosaic stays single
# threaded (the engine runs its own work in parallel elsewhere). Android links the NDK's libomp statically, Windows needs libomp.dll next to
# the executables (copied after the build).
option(DAPPLE_LIBRAW_OPENMP "Build LibRaw with OpenMP (parallel X-Trans / Bayer demosaic)" ON)
if(DAPPLE_LIBRAW_OPENMP AND NOT APPLE AND NOT MSVC)
  include(CheckCXXCompilerFlag)
  set(CMAKE_REQUIRED_LINK_OPTIONS "-fopenmp")
  check_cxx_compiler_flag("-fopenmp" DAPPLE_HAVE_FOPENMP)
  unset(CMAKE_REQUIRED_LINK_OPTIONS)
  if(DAPPLE_HAVE_FOPENMP)
    target_compile_options(dapple_libraw PRIVATE -fopenmp)
    target_compile_definitions(dapple_libraw PRIVATE LIBRAW_USE_OPENMP LIBRAW_FORCE_OPENMP)  # libraw_types.h #undefs it on mingw otherwise
    if(ANDROID)
      target_link_libraries(dapple_libraw PUBLIC -fopenmp -static-openmp)
    elseif(WIN32)
      # the toolchains build with -static, which hides the import library libomp.dll.a: link it by path
      get_filename_component(_cc_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
      file(GLOB _omp_lib "${_cc_dir}/../x86_64-w64-mingw32/lib/libomp.dll.a")
      if(_omp_lib)
        list(GET _omp_lib 0 _omp_lib0)
        target_link_libraries(dapple_libraw PUBLIC "${_omp_lib0}")
      endif()
    else()
      target_link_libraries(dapple_libraw PUBLIC -fopenmp)
    endif()
    if(WIN32)
      file(GLOB _omp_dll "${_cc_dir}/../x86_64-w64-mingw32/bin/libomp.dll" "${_cc_dir}/../aarch64-w64-mingw32/bin/libomp.dll")
      if(_omp_dll AND CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        list(GET _omp_dll 0 _omp_dll0)
        add_custom_command(TARGET dapple_libraw POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_omp_dll0}" "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
      endif()
    endif()
    message(STATUS "LibRaw: OpenMP on")
  endif()
endif()

# ---------------------------------------------------------------------------
# stb_image / stb_image_write (public domain), vendored at a pinned commit.
# ---------------------------------------------------------------------------
add_library(dapple_stb INTERFACE)
target_include_directories(dapple_stb SYSTEM INTERFACE "${PROJECT_SOURCE_DIR}/third_party/stb")

# LibRaw relies on POSIX/GNU extras (M_PI etc.) that strict -std=c++23 hides.
set_target_properties(dapple_libraw PROPERTIES CXX_EXTENSIONS ON)
target_compile_definitions(dapple_libraw PRIVATE _USE_MATH_DEFINES)
