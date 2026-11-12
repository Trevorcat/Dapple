# libdapple_video.so: JNI + software decoder on top of the shared FFmpeg built by tools/ffmpeg/build.sh.
# FFmpeg 9.x stays separate .so files (LGPL: the user can replace them); Gradle packages them from tools/ffmpeg/jniLibs.
# ABIs without an FFmpeg build get a stub library (the app then reports "no software decoder").
set(DAPPLE_FFMPEG_DIR "${CMAKE_SOURCE_DIR}/tools/ffmpeg/install/${ANDROID_ABI}")
if(EXISTS "${DAPPLE_FFMPEG_DIR}/lib/libavcodec.so")
  set(_ff_libs avutil avcodec avformat swscale swresample)
  foreach(l ${_ff_libs})
    add_library(ff_${l} SHARED IMPORTED)
    set_target_properties(ff_${l} PROPERTIES IMPORTED_LOCATION "${DAPPLE_FFMPEG_DIR}/lib/lib${l}.so")
  endforeach()
  add_library(dapple_video SHARED dapple_video.cpp dapple_video_jni.cpp)
  target_include_directories(dapple_video PRIVATE "${DAPPLE_FFMPEG_DIR}/include")
  target_link_libraries(dapple_video PRIVATE ff_avformat ff_avcodec ff_swscale ff_swresample ff_avutil mediandk log)
else()
  message(STATUS "dapple_video: no FFmpeg build for ${ANDROID_ABI}, building the stub")
  add_library(dapple_video SHARED dapple_video_stub.cpp)
endif()
