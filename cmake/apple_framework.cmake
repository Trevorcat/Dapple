# -DCMAKE_PROJECT_INCLUDE=cmake/apple_framework.cmake (iOS builds, docs/ios/README.md): turns the `dapple` shared library into
# dapple.framework (the unit Xcode embeds and signs) without touching core/CMakeLists.txt: the properties are set at the end of
# the root directory, when every target exists.
# Highway (libjxl, DNG 1.7 JPEG XL) dispatches to SVE targets on arm64; Apple clang cannot compile SVE on Darwin (backend crash: "Scalable
# vectors ... not yet supported on Darwin") and no Apple CPU has SVE.
if(APPLE)
  add_compile_definitions(HWY_DISABLED_TARGETS=25952256)  # HWY_SVE2_128 | HWY_SVE_256 | HWY_SVE2 | HWY_SVE (1<<18, 19, 23, 24 in highway 1.2.0 detect_targets.h)
endif()

if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
  function(_dapple_ios_framework)
    if(NOT TARGET dapple)
      return()
    endif()
    set_target_properties(dapple PROPERTIES
      FRAMEWORK TRUE
      OUTPUT_NAME dapple
      MACOSX_FRAMEWORK_IDENTIFIER org.dapple.engine
      MACOSX_FRAMEWORK_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/apple_framework_info.plist.in"
      MACOSX_FRAMEWORK_BUNDLE_VERSION 1
      MACOSX_FRAMEWORK_SHORT_VERSION_STRING "${PROJECT_VERSION}"
      XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO
      BUILD_WITH_INSTALL_RPATH TRUE
      INSTALL_NAME_DIR "@rpath")
  endfunction()
  cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _dapple_ios_framework)
endif()
