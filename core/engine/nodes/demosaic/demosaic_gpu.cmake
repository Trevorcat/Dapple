# Demosaic GPU kernels: shader variants and sources, included by core/gpu/CMakeLists.txt (append to DAPPLE_GPU_SHADERS before the per-shader
# loop, add DAPPLE_DEMOSAIC_GPU_SOURCES to dapple_gpu) and by hal/gles/shader_list.cmake. Binding signature after the uniform block:
# I storage image, S sampled image, B storage buffer. docs/engine/pipeline-architecture.md section 32.
set(DAPPLE_DEMOSAIC_GPU_SHADERS
  "dm_scale|demosaic_scale.comp||BBB"
  "dm_bg|demosaic_bayer.comp|MODE=0|BB"
  "dm_bo32|demosaic_bayer.comp|MODE=1,OUT_FMT=rgba32f|BBI"
  "dm_bo16|demosaic_bayer.comp|MODE=1,OUT_FMT=rgba16f,OUT16=1|BBI"
  "dm_fg|demosaic_xfast.comp|MODE=0|BB"
  "dm_fc|demosaic_xfast.comp|MODE=1|BBB"
  "dm_fo32|demosaic_xfast.comp|MODE=2,OUT_FMT=rgba32f|BBI"
  "dm_fo16|demosaic_xfast.comp|MODE=2,OUT_FMT=rgba16f,OUT16=1|BBI"
  "dm_x0|demosaic_xtrans.comp|MODE=0|BBB"
  "dm_x1|demosaic_xtrans.comp|MODE=1|BBBB"
  "dm_x2|demosaic_xtrans.comp|MODE=2|BBBB"
  "dm_x5_32|demosaic_xtrans.comp|MODE=5,OUT_FMT=rgba32f|BBBBI"
  "dm_x5_16|demosaic_xtrans.comp|MODE=5,OUT_FMT=rgba16f,OUT16=1|BBBBI"
  "dm_h32|demosaic_half.comp|OUT_FMT=rgba32f|BBI"
  "dm_h16|demosaic_half.comp|OUT_FMT=rgba16f,OUT16=1|BBI")
set(DAPPLE_DEMOSAIC_GPU_SOURCES ${CMAKE_CURRENT_LIST_DIR}/demosaic_gpu.cpp)
