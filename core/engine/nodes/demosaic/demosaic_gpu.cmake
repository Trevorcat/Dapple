# Demosaic GPU kernels: shader variants and sources, included by core/gpu/CMakeLists.txt (append to DAPPLE_GPU_SHADERS before the per-shader
# loop, add DAPPLE_DEMOSAIC_GPU_SOURCES to dapple_gpu) and by hal/gles/shader_list.cmake. Binding signature after the uniform block:
# I storage image, S sampled image, B storage buffer. docs/engine/pipeline-architecture.md section 32.
set(DAPPLE_DEMOSAIC_GPU_SHADERS
  "dm_scale|demosaic_scale.comp||BBB"
  "dm_bg|demosaic_bayer.comp|MODE=0|BB"
  "dm_bo32|demosaic_bayer.comp|MODE=1,OUT_FMT=rgba32f|BBI"
  "dm_bo16|demosaic_bayer.comp|MODE=1,OUT_FMT=rgba16f,OUT16=1|BBI"
  "dm_xg|demosaic_xtrans.comp|MODE=0|BB"
  "dm_xc|demosaic_xtrans.comp|MODE=1|BBB"
  "dm_xo32|demosaic_xtrans.comp|MODE=2,OUT_FMT=rgba32f|BBI"
  "dm_xo16|demosaic_xtrans.comp|MODE=2,OUT_FMT=rgba16f,OUT16=1|BBI")
set(DAPPLE_DEMOSAIC_GPU_SOURCES ${CMAKE_CURRENT_LIST_DIR}/demosaic_gpu.cpp)
