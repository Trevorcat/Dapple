add_executable(dapple_tests core_tests.cpp)
target_link_libraries(dapple_tests PRIVATE dapple_core dapple)
target_include_directories(dapple_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src ${PROJECT_SOURCE_DIR}/cli)
if(MSVC)
  target_compile_options(dapple_tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(dapple_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
add_test(NAME core_tests COMMAND dapple_tests)
set_tests_properties(core_tests PROPERTIES TIMEOUT 300)

# Merge (HDR / panorama / Motion Photo) tests. Set DAPPLE_PERF=1 for the 26 MP timing runs.
add_executable(dapple_merge_tests merge_tests.cpp)
target_link_libraries(dapple_merge_tests PRIVATE dapple_core dapple dapple_stb)
if(TARGET dapple_opencv)
  target_link_libraries(dapple_merge_tests PRIVATE dapple_opencv)
endif()
target_include_directories(dapple_merge_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
if(MSVC)
  target_compile_options(dapple_merge_tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(dapple_merge_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
add_test(NAME merge_tests COMMAND dapple_merge_tests WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(merge_tests PROPERTIES TIMEOUT 900)

# DNG opcode lists (GainMap / FixVignetteRadial / WarpRectilinear), default crop and orientation, on synthetic DNG fixtures.
add_executable(dapple_dng_tests dng_opcode_tests.cpp)
target_link_libraries(dapple_dng_tests PRIVATE dapple_core)
target_include_directories(dapple_dng_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src)
if(MSVC)
  target_compile_options(dapple_dng_tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(dapple_dng_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
foreach(id parse gainmap vignette warp e2e illuminant anchor jxl badpixels)
  add_test(NAME dng.${id} COMMAND dapple_dng_tests ${id} WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(dng.${id} PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
endforeach()

# Spectral film engine tests (profiles, film pack, grey neutrality, baked-vs-direct dE2000, hooks).
# The C API under test is compiled into the executable (not the dapple DLL): the registry and the thread pool are
# process singletons, so linking both libdapple_core.a and the DLL would test two separate copies.
add_executable(dapple_film_tests film_tests.cpp ${PROJECT_SOURCE_DIR}/core/src/c_api.cpp ${PROJECT_SOURCE_DIR}/core/src/film_swatch.cpp)
target_link_libraries(dapple_film_tests PRIVATE dapple_core)
target_include_directories(dapple_film_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/include)
target_include_directories(dapple_film_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src ${PROJECT_SOURCE_DIR}/cli)
target_compile_definitions(dapple_film_tests PRIVATE DAPPLE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                           $<$<BOOL:${DAPPLE_FILM_AVX2}>:DAPPLE_FILM_HAVE_AVX2=1>)
if(MSVC)
  target_compile_options(dapple_film_tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(dapple_film_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
# One ctest entry per test (`dapple_film_tests --list` prints the ids; an unknown id fails with exit code 2, so this
# list cannot silently drift from the executable).
set(DAPPLE_FILM_TEST_IDS profiles colorimetry upsample grey spec_vectors baked_vs_direct dir_params render_paths
    smoke_all gauss_mixture spatial_plans grain_stats spatial_render glare_enlarger spatial_extras editparams_json
    develop_film film_match c_api validation strong_guarantee robust_render render_rows pack_format cache_keys concurrency
    pool_exceptions kernel_exp library_errors)
foreach(id IN LISTS DAPPLE_FILM_TEST_IDS)
  add_test(NAME film.${id} COMMAND dapple_film_tests ${id} WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(film.${id} PROPERTIES TIMEOUT 300)
endforeach()

# GPU compute backend (core/gpu): per-stage parity with the CPU reference, tiled export seams. Only with -DDAPPLE_GPU=ON.
# Exit code 77 = no Vulkan device (skipped). The SwiftShader run (the emulator's software Vulkan) needs its ICD json.
if(TARGET dapple_gpu)
  add_executable(dapple_gpu_tests ${PROJECT_SOURCE_DIR}/core/gpu/tests/gpu_tests.cpp)
  target_link_libraries(dapple_gpu_tests PRIVATE dapple_gpu dapple_core dapple)
  target_include_directories(dapple_gpu_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/gpu/src ${PROJECT_SOURCE_DIR}/cli)
  target_compile_definitions(dapple_gpu_tests PRIVATE DAPPLE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  if(NOT MSVC)
    target_compile_options(dapple_gpu_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
  add_test(NAME gpu_parity COMMAND dapple_gpu_tests)
  set_tests_properties(gpu_parity PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77)
  # The emulator's software Vulkan: its ICD manifest names the library relative to itself, which the Windows loader
  # refuses, so a manifest with the absolute path is generated here.
  set(DAPPLE_SWIFTSHADER_LIB "${PROJECT_SOURCE_DIR}/tools/android-sdk/emulator/lib64/vulkan/vk_swiftshader.dll"
      CACHE FILEPATH "SwiftShader (software Vulkan) library for the gpu_parity_swiftshader test")
  if(EXISTS "${DAPPLE_SWIFTSHADER_LIB}")
    set(_ss_icd "${CMAKE_BINARY_DIR}/swiftshader_icd.json")
    file(WRITE "${_ss_icd}" "{\"file_format_version\":\"1.0.0\",\"ICD\":{\"library_path\":\"${DAPPLE_SWIFTSHADER_LIB}\",\"api_version\":\"1.0.5\"}}")
    add_test(NAME gpu_parity_swiftshader COMMAND dapple_gpu_tests)
    set_tests_properties(gpu_parity_swiftshader PROPERTIES TIMEOUT 1800 SKIP_RETURN_CODE 77
      ENVIRONMENT "VK_DRIVER_FILES=${_ss_icd};VK_ICD_FILENAMES=${_ss_icd};DAPPLE_GPU_SOFTWARE=1")
  endif()

  # GPU engine tests (core/engine/hal, exec/gpu_*, nodes/*/gpu.cpp): HAL, scheduler, O(1) box, splitting, selection.
  add_executable(dapple_gpu_engine_tests ${PROJECT_SOURCE_DIR}/tests/engine/gpu/gpu_engine_tests.cpp)
  target_link_libraries(dapple_gpu_engine_tests PRIVATE dapple_gpu dapple_core dapple)
  target_include_directories(dapple_gpu_engine_tests PRIVATE ${PROJECT_SOURCE_DIR}/core ${PROJECT_SOURCE_DIR}/core/gpu/src ${PROJECT_SOURCE_DIR}/cli)
  if(NOT MSVC)
    target_compile_options(dapple_gpu_engine_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
  foreach(_t hal box sched select external probe)
    add_test(NAME engine.gpu_${_t} COMMAND dapple_gpu_engine_tests ${_t})
    set_tests_properties(engine.gpu_${_t} PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
  endforeach()
  if(EXISTS "${DAPPLE_SWIFTSHADER_LIB}")
    foreach(_t hal box sched select external probe)
      add_test(NAME engine.gpu_${_t}_swiftshader COMMAND dapple_gpu_engine_tests ${_t})
      set_tests_properties(engine.gpu_${_t}_swiftshader PROPERTIES TIMEOUT 1800 SKIP_RETURN_CODE 77
        ENVIRONMENT "VK_DRIVER_FILES=${_ss_icd};VK_ICD_FILENAMES=${_ss_icd};DAPPLE_GPU_SOFTWARE=1")
    endforeach()
  endif()
endif()

# ---- engine migration (docs/engine/pipeline-architecture.md section 10, stage a) ---------------------------------------
# Golden corpus + hash harness (step a0): hashes recorded from the unchanged renderer; every engine mode must reproduce
# them byte for byte (gate B). The C API is compiled into the executable (process singletons, see dapple_film_tests).
add_executable(dapple_golden engine/golden/golden_tests.cpp)
target_link_libraries(dapple_golden PRIVATE dapple_core)
target_include_directories(dapple_golden PRIVATE ${PROJECT_SOURCE_DIR}/core/src ${PROJECT_SOURCE_DIR}/core/include)
target_compile_definitions(dapple_golden PRIVATE DAPPLE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(MSVC)
  target_compile_options(dapple_golden PRIVATE /W4 /utf-8)
else()
  target_compile_options(dapple_golden PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
foreach(mode off full)
  add_test(NAME engine.golden_${mode}
           COMMAND dapple_golden verify ${PROJECT_SOURCE_DIR}/tests/engine/golden/corpus.txt ${mode}
           WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(engine.golden_${mode} PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
endforeach()
# off (old Renderer) vs full (engine) on this machine, any architecture; also built for Android (arm64 NEON / x86_64 emulator)
add_test(NAME engine.selfcheck COMMAND dapple_golden selfcheck WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
# cached film exposure match against the uncached one: every settled frame bit identical (pipeline-architecture.md 26.2)
add_test(NAME engine.match_cache_seq COMMAND dapple_golden seqcheck WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(engine.match_cache_seq PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
set_tests_properties(engine.selfcheck PROPERTIES TIMEOUT 600)

# Engine graph / table nodes / cache / overhead tests (steps a1-a3). One ctest entry per id (`--list` prints them).
add_executable(dapple_engine_tests engine/engine_tests.cpp)
target_link_libraries(dapple_engine_tests PRIVATE dapple_core)
target_include_directories(dapple_engine_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src ${PROJECT_SOURCE_DIR}/core/include)
target_compile_definitions(dapple_engine_tests PRIVATE DAPPLE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(MSVC)
  target_compile_options(dapple_engine_tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(dapple_engine_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
set(DAPPLE_ENGINE_TEST_IDS hash registry validate snapshots keys conformance cache reuse overhead)
foreach(id IN LISTS DAPPLE_ENGINE_TEST_IDS)
  add_test(NAME engine.${id} COMMAND dapple_engine_tests ${id} WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(engine.${id} PROPERTIES TIMEOUT 900)
endforeach()

# Film nodes (step b4): byte-identity with FilmSimulator::render, table re-bake granularity, declaration conformance of the
# film nodes, scale-aware grain statistics, frame-index grain, timings. One ctest entry per id (`--list` prints them).
add_executable(dapple_film_nodes_tests engine/film_nodes_tests.cpp)
target_link_libraries(dapple_film_nodes_tests PRIVATE dapple_core)
target_include_directories(dapple_film_nodes_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src ${PROJECT_SOURCE_DIR}/core/include ${PROJECT_SOURCE_DIR}/core)
target_compile_definitions(dapple_film_nodes_tests PRIVATE DAPPLE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(MSVC)
  target_compile_options(dapple_film_nodes_tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(dapple_film_nodes_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
foreach(id equiv rebake keys graph concurrent grain_scale grain_frame fx)
  add_test(NAME film_nodes.${id} COMMAND dapple_film_nodes_tests ${id} WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(film_nodes.${id} PROPERTIES TIMEOUT 900)
endforeach()

# Film tables on disk ("warm from disk"): round trip, corruption, version (salt) mismatch, size cap, concurrent processes (it runs itself as child processes).
add_executable(dapple_film_disk_tests engine/film_disk_tests.cpp)
target_link_libraries(dapple_film_disk_tests PRIVATE dapple_core)
target_include_directories(dapple_film_disk_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src ${PROJECT_SOURCE_DIR}/core/include ${PROJECT_SOURCE_DIR}/core)
target_compile_definitions(dapple_film_disk_tests PRIVATE DAPPLE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(MSVC)
  target_compile_options(dapple_film_disk_tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(dapple_film_disk_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
add_test(NAME film_disk.tables COMMAND dapple_film_disk_tests WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(film_disk.tables PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77)

# CPU planner / executor tests (steps a5 - a10): byte identity of the Whole plan, tile invariance of TiledExport, plan structure,
# cancellation, the tonal-analysis key fix. One ctest entry per id.
add_executable(dapple_exec_tests engine/exec_tests.cpp)
target_link_libraries(dapple_exec_tests PRIVATE dapple_core)
target_include_directories(dapple_exec_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src ${PROJECT_SOURCE_DIR}/core/include ${PROJECT_SOURCE_DIR}/cli)
if(MSVC)
  target_compile_options(dapple_exec_tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(dapple_exec_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
foreach(id whole tiles plan cancel analysis)
  add_test(NAME engine.exec_${id} COMMAND dapple_exec_tests ${id} WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(engine.exec_${id} PROPERTIES TIMEOUT 900)
endforeach()

# Single-image HDR (EditParams::hdr): identity, tile invariance, monotonic effect, halo, noise gain; with the GPU executor parity and cost.
add_executable(dapple_hdr_tests engine/hdr_tests.cpp)
target_link_libraries(dapple_hdr_tests PRIVATE dapple_core)
target_include_directories(dapple_hdr_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src ${PROJECT_SOURCE_DIR}/core/include ${PROJECT_SOURCE_DIR}/cli)
if(MSVC)
  target_compile_options(dapple_hdr_tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(dapple_hdr_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
set(_hdr_ids identity lowrange anchors midband tiles monotonic halo noise)
if(TARGET dapple_gpu)
  target_link_libraries(dapple_hdr_tests PRIVATE dapple_gpu)
  target_include_directories(dapple_hdr_tests PRIVATE ${PROJECT_SOURCE_DIR}/core ${PROJECT_SOURCE_DIR}/core/gpu/src)
  target_compile_definitions(dapple_hdr_tests PRIVATE DAPPLE_HDR_GPU=1)
  set(_hdr_ids ${_hdr_ids} parity)
endif()
foreach(id ${_hdr_ids})
  add_test(NAME engine.hdr_${id} COMMAND dapple_hdr_tests ${id} WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(engine.hdr_${id} PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
endforeach()

# Film nodes on the GPU executor (step b4 (2)): per-node parity with the CPU nodes. Only with -DDAPPLE_GPU=ON; exit 77 = no Vulkan.
if(TARGET dapple_gpu)
  add_executable(dapple_film_gpu_tests engine/film_gpu_tests.cpp)
  target_link_libraries(dapple_film_gpu_tests PRIVATE dapple_gpu dapple_core)
  target_include_directories(dapple_film_gpu_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/gpu/src ${PROJECT_SOURCE_DIR}/core/src ${PROJECT_SOURCE_DIR}/core/include
                             ${PROJECT_SOURCE_DIR}/core ${PROJECT_SOURCE_DIR}/cli)
  target_compile_definitions(dapple_film_gpu_tests PRIVATE DAPPLE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
  if(NOT MSVC)
    target_compile_options(dapple_film_gpu_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
  endif()
  add_test(NAME film_gpu.parity COMMAND dapple_film_gpu_tests WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(film_gpu.parity PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77)
  # whole film frames through EngineSession + GpuExecutor against the CPU engine (section 20)
  add_test(NAME film_gpu.frames COMMAND dapple_film_gpu_tests frames WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(film_gpu.frames PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77)
endif()

# Slider-drag timing through the C API (docs/engine/pipeline-architecture.md section 26)
add_executable(dapple_drag_bench engine/drag_bench.cpp)
target_link_libraries(dapple_drag_bench PRIVATE dapple)
target_include_directories(dapple_drag_bench PRIVATE ${PROJECT_SOURCE_DIR}/core/include)

# The editor decode stages through the C API (open, 2x detail, 1:1 regions, export), for device timing without the app
add_executable(dapple_stage_bench engine/stage_bench.cpp)
target_link_libraries(dapple_stage_bench PRIVATE dapple)
target_include_directories(dapple_stage_bench PRIVATE ${PROJECT_SOURCE_DIR}/core/include)

# Cached film exposure match: exact equals the uncached solve, interpolation during a print drag, lean key (section 26.2)
add_executable(dapple_match_cache_tests engine/match_cache_tests.cpp)
target_link_libraries(dapple_match_cache_tests PRIVATE dapple_core)
target_include_directories(dapple_match_cache_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/include ${PROJECT_SOURCE_DIR}/core/src)
target_compile_definitions(dapple_match_cache_tests PRIVATE DAPPLE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_test(NAME engine.match_cache COMMAND dapple_match_cache_tests WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
set_tests_properties(engine.match_cache PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)

# VideoFrameSession (wave 8, motion-photo): a Motion Photo clip through the node graph, docs/engine/pipeline-architecture.md section 27.
# depth10 / e2e / verify need the desktop FFmpeg (tools/ffmpeg/build-host.sh); the others run on synthetic frames.
add_executable(dapple_video_session_tests video/video_session_tests.cpp)
target_link_libraries(dapple_video_session_tests PRIVATE dapple_core)
target_include_directories(dapple_video_session_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src ${PROJECT_SOURCE_DIR}/core/include ${PROJECT_SOURCE_DIR}/core)
target_compile_definitions(dapple_video_session_tests PRIVATE DAPPLE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
if(DAPPLE_HAVE_VIDEO_HOST)
  target_link_libraries(dapple_video_session_tests PRIVATE dapple_video_host)
  target_include_directories(dapple_video_session_tests PRIVATE "${DAPPLE_FFMPEG_HOST_DIR}/include")
  target_compile_definitions(dapple_video_session_tests PRIVATE DAPPLE_HAVE_VIDEO_HOST=1)
  if(DAPPLE_FFMPEG_HOST_DLLS)
    add_custom_command(TARGET dapple_video_session_tests POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DAPPLE_FFMPEG_HOST_DLLS} $<TARGET_FILE_DIR:dapple_video_session_tests>)
  endif()
endif()
if(NOT MSVC)
  target_compile_options(dapple_video_session_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
foreach(id convert oracle hold depth10 flicker grain perf e2e hdr anchor)
  add_test(NAME video_session.${id} COMMAND dapple_video_session_tests ${id} WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(video_session.${id} PROPERTIES TIMEOUT 900 SKIP_RETURN_CODE 77)
endforeach()
if(DAPPLE_HAVE_VIDEO_HOST)
  # the CLI end to end: decode a 10-bit HEVC clip, film through the graph, FFV1 10-bit out; then decode the output again and count the code values
  add_test(NAME video_session.cli_motion_render
           COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:dapple_cli> -DTESTER=$<TARGET_FILE:dapple_video_session_tests> -DSRC=${PROJECT_SOURCE_DIR}
                   -DOUT=${CMAKE_CURRENT_BINARY_DIR}/motion_render_test.mkv -P ${CMAKE_CURRENT_SOURCE_DIR}/video/cli_motion_render.cmake
           WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(video_session.cli_motion_render PROPERTIES TIMEOUT 600)
endif()

# Demosaic node (core/engine/nodes/demosaic): tests and evaluation tool
include(${PROJECT_SOURCE_DIR}/tests/engine/demosaic/demosaic.cmake)

# Ultra HDR (gain-map JPEG) export: container, gain-map maths, session export (docs/engine/pipeline-architecture.md section 34). Compiles the C API in,
# like the film tests; the session test skips (exit 77) without build/rawsamples.
add_executable(dapple_uhdr_tests uhdr_tests.cpp ${PROJECT_SOURCE_DIR}/core/src/c_api.cpp ${PROJECT_SOURCE_DIR}/core/src/film_swatch.cpp)
target_link_libraries(dapple_uhdr_tests PRIVATE dapple_core dapple_stb)
target_include_directories(dapple_uhdr_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/include ${PROJECT_SOURCE_DIR}/core/src ${PROJECT_SOURCE_DIR}/cli)
target_compile_definitions(dapple_uhdr_tests PRIVATE DAPPLE_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
                           $<$<BOOL:${DAPPLE_FILM_AVX2}>:DAPPLE_FILM_HAVE_AVX2=1>)
if(MSVC)
  target_compile_options(dapple_uhdr_tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(dapple_uhdr_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
foreach(id container reconstruct rule jpeg_source session)
  add_test(NAME uhdr.${id} COMMAND dapple_uhdr_tests ${id} WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  set_tests_properties(uhdr.${id} PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
endforeach()

# Wide-gamut / 16-bit display-referred sources (colour space tags, ICC matrix profiles, banding).
add_executable(dapple_wide_tests wide_gamut_tests.cpp)
target_link_libraries(dapple_wide_tests PRIVATE dapple_core)
target_include_directories(dapple_wide_tests PRIVATE ${PROJECT_SOURCE_DIR}/core/src ${PROJECT_SOURCE_DIR}/core/include)
if(MSVC)
  target_compile_options(dapple_wide_tests PRIVATE /W4 /utf-8)
else()
  target_compile_options(dapple_wide_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
add_test(NAME wide_gamut COMMAND dapple_wide_tests)
set_tests_properties(wide_gamut PROPERTIES TIMEOUT 300)
