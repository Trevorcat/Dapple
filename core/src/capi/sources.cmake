# The C API implementation (dapple/dapple_c.h), one list for the shared library and for every test that compiles it in
# (included from the top-level CMakeLists.txt before core/, so tests/ and core/engine/present see the variable).
set(_capi_dir ${CMAKE_CURRENT_LIST_DIR})
set(DAPPLE_CAPI_SOURCES
  ${_capi_dir}/ai.cpp
  ${_capi_dir}/config.cpp
  ${_capi_dir}/export.cpp
  ${_capi_dir}/film.cpp
  ${_capi_dir}/frame.cpp
  ${_capi_dir}/hdr.cpp
  ${_capi_dir}/info.cpp
  ${_capi_dir}/local.cpp
  ${_capi_dir}/memory.cpp
  ${_capi_dir}/merge.cpp
  ${_capi_dir}/motion.cpp
  ${_capi_dir}/open.cpp
  ${_capi_dir}/render.cpp
  ${_capi_dir}/session.cpp
  ${_capi_dir}/stream.cpp)
