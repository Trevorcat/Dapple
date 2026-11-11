# ---------------------------------------------------------------------------
# OpenCV 4.12.0 (Apache-2.0) -- optional, used for HDR alignment and panorama
# stitching only. Built as static libs from source with a minimal module list;
# no contrib / non-free / GPU code (features: ORB and AKAZE, never SIFT/SURF).
#
# Sources are cached in tools/_dl/opencv. If tools/_dl/opencv/opencv-4.12.0 is
# present it is used directly (offline); otherwise the pinned tarball is
# downloaded into tools/_dl/opencv (FetchContent DOWNLOAD_DIR) and extracted.
# Override with -DFETCHCONTENT_SOURCE_DIR_OPENCV=<path>.
# ---------------------------------------------------------------------------
include(FetchContent)

set(DAPPLE_OPENCV_DL "${PROJECT_SOURCE_DIR}/tools/_dl/opencv")
if(NOT DEFINED FETCHCONTENT_SOURCE_DIR_OPENCV AND
   EXISTS "${DAPPLE_OPENCV_DL}/opencv-4.12.0/CMakeLists.txt")
  set(FETCHCONTENT_SOURCE_DIR_OPENCV "${DAPPLE_OPENCV_DL}/opencv-4.12.0")
endif()

FetchContent_Declare(opencv
  URL https://github.com/opencv/opencv/archive/refs/tags/4.12.0.tar.gz
  URL_HASH SHA256=44c106d5bb47efec04e531fd93008b3fcd1d27138985c5baf4eafac0e1ec9e9d
  DOWNLOAD_DIR "${DAPPLE_OPENCV_DL}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SOURCE_SUBDIR _no_cmake_)
FetchContent_MakeAvailable(opencv)

set(DAPPLE_OPENCV_MODULES core imgproc features2d calib3d flann stitching photo video)
string(REPLACE ";" "," _ocv_list "${DAPPLE_OPENCV_MODULES}")

macro(_ocv_set var val)
  set(${var} ${val} CACHE BOOL "" FORCE)
endmacro()
set(BUILD_LIST "${_ocv_list}" CACHE STRING "" FORCE)
_ocv_set(BUILD_SHARED_LIBS OFF)
foreach(_o BUILD_TESTS BUILD_PERF_TESTS BUILD_EXAMPLES BUILD_DOCS BUILD_JAVA BUILD_opencv_apps
           BUILD_opencv_python2 BUILD_opencv_python3 BUILD_ANDROID_PROJECTS BUILD_ANDROID_EXAMPLES
           BUILD_PACKAGE BUILD_OPENEXR BUILD_PROTOBUF BUILD_JPEG BUILD_PNG BUILD_TIFF BUILD_WEBP
           BUILD_JASPER BUILD_OPENJPEG BUILD_ITT BUILD_USE_SYMLINKS
           WITH_IPP WITH_ITT WITH_OPENCL WITH_CUDA WITH_OPENMP WITH_TBB WITH_EIGEN WITH_LAPACK WITH_PROTOBUF
           WITH_FFMPEG WITH_GSTREAMER WITH_V4L WITH_1394 WITH_JPEG WITH_PNG WITH_TIFF WITH_WEBP WITH_JASPER
           WITH_OPENJPEG WITH_OPENEXR WITH_GTK WITH_WIN32UI WITH_VTK WITH_ADE WITH_QUIRC WITH_OPENCLAMDBLAS
           WITH_OPENCLAMDFFT WITH_DSHOW WITH_MSMF WITH_AVIF WITH_IMGCODEC_HDR WITH_IMGCODEC_SUNRASTER
           WITH_IMGCODEC_PXM WITH_IMGCODEC_PFM WITH_CAP_IOS WITH_OBSENSOR WITH_FLATBUFFERS
           OPENCV_GENERATE_PKGCONFIG ENABLE_PRECOMPILED_HEADERS INSTALL_CREATE_DISTRIB CV_TRACE)
  _ocv_set(${_o} OFF)
endforeach()
_ocv_set(BUILD_ZLIB ON)
_ocv_set(WITH_PTHREADS_PF ON)
_ocv_set(OPENCV_FORCE_3RDPARTY_BUILD ON)

# Keep OpenCV's artefacts out of our bin/ directory and out of "all".
set(_saved_runtime "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
set(_saved_library "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
set(_saved_vis_cxx "${CMAKE_CXX_VISIBILITY_PRESET}")
unset(CMAKE_CXX_VISIBILITY_PRESET)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/opencv/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/opencv/lib")
add_subdirectory("${opencv_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/opencv" EXCLUDE_FROM_ALL)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${_saved_runtime}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${_saved_library}")
set(CMAKE_CXX_VISIBILITY_PRESET "${_saved_vis_cxx}")

# Interface target carrying everything dapple_core needs.
add_library(dapple_opencv INTERFACE)
foreach(_m ${DAPPLE_OPENCV_MODULES})
  target_link_libraries(dapple_opencv INTERFACE opencv_${_m})
  target_include_directories(dapple_opencv SYSTEM INTERFACE "${opencv_SOURCE_DIR}/modules/${_m}/include")
endforeach()
target_include_directories(dapple_opencv SYSTEM INTERFACE "${CMAKE_BINARY_DIR}" "${CMAKE_BINARY_DIR}/opencv")
target_compile_definitions(dapple_opencv INTERFACE DAPPLE_HAVE_OPENCV=1)
