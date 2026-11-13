# Fallback shader list for the GLES build when the Vulkan build has not defined DAPPLE_GPU_SHADERS (same format).
# Keep identical to the list used by the Vulkan backend; the authoritative one is the GPU engineer's.
set(DAPPLE_GPU_SHADERS
  "point|point.comp||SBSSII"
  "point_local|point.comp|LOCAL=1|SBSSIIBBBII"
  "local_detail|local_detail.comp||SSSSSSII"
  "local_film_in|local_film_in.comp||SIBBII"
  "local_heal|local_heal.comp||SBII"
  "local_rt_down|local_rt_down.comp||SSSII"
  "local_retouch|local_retouch.comp||SSSSSSII"
  "sep_d2y|sepconv.comp|IN_FMT=rgba16f,OUT_FMT=r32f,IN_MODE=1,SCALAR=1|SIB"
  "sep_d2y_v|sepconv.comp|BIGV=1,IN_FMT=rgba16f,OUT_FMT=r32f,IN_MODE=1,SCALAR=1|SIB"
  "sep_d2y_s|sepconv.comp|SMALL=1,IN_FMT=rgba16f,OUT_FMT=r32f,IN_MODE=1,SCALAR=1|SIB"
  "sep_c2y|sepconv.comp|IN_FMT=rgba32f,OUT_FMT=r32f,IN_MODE=2,SCALAR=1|SIB"
  "sep_c2y_v|sepconv.comp|BIGV=1,IN_FMT=rgba32f,OUT_FMT=r32f,IN_MODE=2,SCALAR=1|SIB"
  "sep_c2y_s|sepconv.comp|SMALL=1,IN_FMT=rgba32f,OUT_FMT=r32f,IN_MODE=2,SCALAR=1|SIB"
  "sep_c|sepconv.comp|IN_FMT=rgba32f,OUT_FMT=rgba32f,IN_MODE=0|SIB"
  "sep_c_s|sepconv.comp|SMALL=1,IN_FMT=rgba32f,OUT_FMT=rgba32f,IN_MODE=0|SIB"
  "sep_y|sepconv.comp|IN_FMT=r32f,OUT_FMT=r32f,IN_MODE=0,SCALAR=1|SIB"
  "sep_y_v|sepconv.comp|BIGV=1,IN_FMT=r32f,OUT_FMT=r32f,IN_MODE=0,SCALAR=1|SIB"
  "sep_y_s|sepconv.comp|SMALL=1,IN_FMT=r32f,OUT_FMT=r32f,IN_MODE=0,SCALAR=1|SIB"
  "down|down.comp||SI"
  "nr_luma|nr_luma.comp|ES_PINGPONG=1|ISIS"
  "nr_chroma|nr_chroma.comp|ES_PINGPONG=1|ISIS"
  "dehaze_apply|dehaze_apply.comp|ES_PINGPONG=1|ISBIS"
  "reduce|reduce.comp||SSBB"
  "gf_ab|gf_ab.comp||SI"
  "detail|detail.comp|ES_PINGPONG=1|ISSSIS"
  "resize|resize.comp||SI"
  "convert|convert.comp||BI"
  "bx_d2y|boxscan.comp|IN_MODE=1|SI"
  "bx_y|boxscan.comp|IN_MODE=0|SI"
  "bx_y_v|boxscan.comp|VERT=1,IN_MODE=0|SI")

# film nodes (film engineer): appended like core/gpu/CMakeLists.txt does for the Vulkan list
include("${CMAKE_CURRENT_LIST_DIR}/../../nodes/film/film_gpu.cmake")
list(APPEND DAPPLE_GPU_SHADERS ${DAPPLE_FILM_GPU_SHADERS})

# film nodes (owned by the film engineer): their list is appended when it exists, exactly as core/gpu/CMakeLists.txt does
get_filename_component(_gles_film_cmake "${CMAKE_CURRENT_LIST_DIR}/../../nodes/film/film_gpu.cmake" ABSOLUTE)
if(EXISTS "${_gles_film_cmake}")
  include("${_gles_film_cmake}")
  list(APPEND DAPPLE_GPU_SHADERS ${DAPPLE_FILM_GPU_SHADERS})
endif()

# demosaic node (pipeline-architecture.md section 32)
include("${CMAKE_CURRENT_LIST_DIR}/../../nodes/demosaic/demosaic_gpu.cmake")
list(APPEND DAPPLE_GPU_SHADERS ${DAPPLE_DEMOSAIC_GPU_SHADERS})
