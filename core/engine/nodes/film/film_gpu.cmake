# Film GPU nodes: shader variants and sources, included by core/gpu/CMakeLists.txt (append to DAPPLE_GPU_SHADERS before the
# per-shader loop, add DAPPLE_FILM_GPU_SOURCES to dapple_gpu). Binding signature: I storage image, S sampled image, B storage buffer.
set(DAPPLE_FILM_GPU_SHADERS
  "film_point|film_point.comp||SSSBI"
  "film_pd|film_point.comp|FUSED_POST=1|SSSBISB"
  "film_split|film_mix.comp|MODE=0|SI"
  "film_merge|film_mix.comp|MODE=1|SSSI"
  "film_down|film_mix.comp|MODE=2|SI"
  "film_base|film_mix.comp|MODE=3|SSI"
  "film_firh|film_mix.comp|MODE=4|SI"
  "film_firv|film_mix.comp|MODE=5|SI"
  "film_grain|film_grain.comp||SBI"
  "film_dec|film_post.comp|MODE=0|SBI"
  "film_gnoise|film_post.comp|MODE=1|I"
  "film_gadd|film_post.comp|MODE=2|SSI"
  "film_f4h|film_post.comp|MODE=3|SI"
  "film_f4v|film_post.comp|MODE=4|SI"
  "film_enc|film_post.comp|MODE=5|SSI"
  "film_f4ve|film_post.comp|MODE=6|SSI"
  "film_d3|film_mix3.comp|MODE=0|SI"
  "film_h3|film_mix3.comp|MODE=1|SIIIIB"
  "film_v3|film_mix3.comp|MODE=2|SSSSSSIB"
  "film_b3|film_mix3.comp|MODE=3|SSI"
  "film_src|film_src.comp||SI"
  "film_fin|film_fin.comp||SII")
set(DAPPLE_FILM_GPU_SOURCES ${CMAKE_CURRENT_LIST_DIR}/film_gpu.cpp)
