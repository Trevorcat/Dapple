# Stage (d) display layer (core/engine/present): compiled straight into libdapple (a static archive would lose its exports to
# --exclude-libs). Vulkan is dlopen'ed at run time; nothing links against it.
set(_p "${CMAKE_CURRENT_LIST_DIR}")
target_sources(dapple PRIVATE
  ${_p}/render_loop.cpp
  ${_p}/null_presenter.cpp
  ${_p}/vk_presenter.cpp
  ${_p}/view.cpp
  ${_p}/view_jni.cpp)
target_include_directories(dapple PRIVATE ${_p})
if(ANDROID)
  target_link_libraries(dapple PRIVATE android)
endif()

# Native tests (loop semantics + view lifecycle on the null presenter). c_api.cpp is compiled in, not the DLL, like film_tests.
if(DAPPLE_BUILD_TESTS AND NOT ANDROID)
  add_executable(dapple_present_tests ${_p}/present_tests.cpp ${_p}/render_loop.cpp ${_p}/null_presenter.cpp ${_p}/view.cpp
                 ${PROJECT_SOURCE_DIR}/core/src/c_api.cpp)
  target_link_libraries(dapple_present_tests PRIVATE dapple_core)
  target_include_directories(dapple_present_tests PRIVATE ${_p} ${PROJECT_SOURCE_DIR}/core/include ${PROJECT_SOURCE_DIR}/core
                             ${PROJECT_SOURCE_DIR}/core/src)
  target_compile_definitions(dapple_present_tests PRIVATE DAPPLE_VERSION_STRING="test"
                             $<$<BOOL:${DAPPLE_FILM_AVX2}>:DAPPLE_FILM_HAVE_AVX2=1>)
  if(NOT MSVC)
    target_compile_options(dapple_present_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
  enable_testing()
  add_test(NAME present.loop_view COMMAND dapple_present_tests)
  set_tests_properties(present.loop_view PROPERTIES TIMEOUT 120)
endif()
