# Old pipeline removal: the exact deletion list (engine-gaps engineer, wave 8, 2026-11-12)

Status: **nothing is deleted.** This is the list, the order, and what has to be re-pointed first. The owner signs off after device verification
(pipeline-architecture.md 21.3 is the earlier, shorter version of this list; this file supersedes it with a caller-by-caller check).
Method: `grep` over `core/ cli/ tests/` for every public `Renderer` method and every `Mode::Off` branch at HEAD 1088f2d plus the working tree of
wave 8, then each item classified as **delete**, **keep (the engine uses it)** or **re-point first**.

## 0. What "the old pipeline" is

`core/src/renderer.cpp` (1902 lines) is two things mixed:

* the **per-pixel kernels** the engine still runs: `Renderer::PointOp` (renderer.cpp:470), `makePointOp` (:636), `LutSampler`, the curves, `inputGain`,
  `whiteBalanceGain` (:242 / :251), the bake pieces (`bakeColourLattice`, `bakeTonePre`, `bakeToneTable`, `bakeAnalysis`, `bakeOverflow`,
  `latticeFilmOnly`), `pointKey` / `colourKey`, `framePlan`, `fastSize`, `sceneGain`, `filmActive`, `setToneProxy`, `setToneReach`. `FrameKernels`
  (`core/src/frame_kernels.h`, a friend of `Renderer`) wraps them as engine block fragments. **These stay**, and `renderer.cpp` has to be split so the
  kernels no longer live in a file called Renderer with a render loop (item 1).
* the **old render loops** that produce a whole frame without the planner: `render`, `renderFloat`, `renderBakedRgba`, `renderBakedFloat`,
  `renderBakedImpl`. These are the delete candidates. Their only legitimate remaining job is the golden `off` oracle.

## 1. Deletion list

| # | What | Where | Remaining callers at HEAD (all must be gone or re-pointed first) |
|---|---|---|---|
| 1a | `Renderer::render` | renderer.cpp:1463, renderer.h:`render` | `c_api.cpp:349, 352, 734` (all `Mode::Off`), `tests/core_tests.cpp` indirectly (via `renderFloat`) |
| 1b | `Renderer::renderFloat` | renderer.cpp:855 (+ the per-pixel loop it owns) | **real callers, not only Off:** `core/src/film_match.cpp:797, 830, 854, 868, 914`, `core/src/film_match_cache.cpp:106, 169, 198` (the film exposure match renders its 256 px proxy with it), `cli/film_cmds.cpp:363, 421`, `core/src/tone.cpp:22` (`Mode::Off` only), `cli/edit_bench.cpp`, tests (below) |
| 1c | `Renderer::renderBakedRgba`, `renderBakedFloat`, `renderBakedImpl` | renderer.cpp:1261 - 1462 | `c_api.cpp:842` (Off), `cli/edit_bench.cpp:253-353`, `cli/film_bench.cpp:295, 333`, `core/gpu/src/gpu_bench.cpp:212, 356`, `core/gpu/tests/gpu_tests.cpp` (11 sites), `tests/engine/exec_tests.cpp:431`, `tests/engine/film_gpu_tests.cpp:707`, `tests/engine/golden/golden_tests.cpp:630-637, 732`, `tests/core_tests.cpp:1363-1411`, `tests/film_tests.cpp` (about 15 sites) |
| 1d | the file split | `renderer.cpp` -> `point_kernels.cpp` (PointOp, makePointOp, Curve1D, LutSampler, gains, keys, bake pieces, framePlan, overflow) | n/a; `renderer.h` shrinks to the kernel class (rename `Renderer` -> `PointPipeline` is optional, large diff, not needed) |
| 2 | every `Mode::Off` branch in the C layer | `c_api.cpp`: `renderSessionParams` exact / baked fallbacks (:335-352), the frame path (:719-852), `DappleSession::bakes` + the `#f` / `#x` key lookup + `lastParams` bookkeeping, `Renderer::bake()`'s `#f` / `#x` keys | `engine::mode()` consumers: `c_api.cpp:348, 351, 731`, `tone.cpp:22`, `engine_session.cpp:49, 88, 1149` |
| 3 | `Mode::Off`, `DAPPLE_ENGINE=off|0`, and the Android system property that selects it | `engine_session.cpp:41-49`, `engine_session.h:27-28, 148`, `dapple_c.h:223, 241` (docs of the env var), `edit_bench.cpp:358` (prints it) | ctest `engine.golden_off`, `engine.reuse` (compares off vs engine), `engine.selfcheck` (the off half) |
| 4 | `GraphDetail::Legacy` and the `LegacyRender` node type | `core/engine/graph/graph.{h,cpp}`, `builder.cpp` (the table graph of the old `renderFrame`) | check with `grep LegacyRender`: still the cache type of the assembled bake |
| 5 | the two film seams | `engine_session.cpp:581, 810` (`[film: unknown stock / paper or failed table bake]` falls back to the old renderer) and `Renderer::filmActive()` in the error messages and `engine_session.cpp:322` | since 21.1 / section 22 the film nodes cover boost and enlarger diffusion; the seam only guards an unknown stock or a failed table bake, which should become an explicit error (a frame the planner cannot take = `engine: plan not supported`) |

Not on the list although it mentions `Renderer`: `engine_session.cpp:1125, 1135` (`renderImage8/F` build a `Renderer` as the parameter resolver
for the plan), `gpu_context.cpp:202, 273` (GPU self-test builds the resolved parameter block), `video_frame_session.*` (same). They use the kernel class,
not a render loop.

## 2. What must be re-pointed first (in this order, because later steps depend on earlier ones)

1. **The film exposure match** (`film_match.cpp` x5, `film_match_cache.cpp` x3, `film_cmds.cpp` x2): proxy renders with `engine::renderImageF(params, camera, lut, proxy, /*spatial*/false, out)` (exists since wave 4). This is a performance-sensitive path (the match solve renders about 5 proxies, `engine.match_cache_seq` guards bit-identical settled frames): re-point, re-record only the golden lines that legitimately change (the proxy render through the engine is byte-identical to `renderFloat` according to `selfcheck`, so none should), run `engine.match_cache_seq` and the match timings of the device engineer before and after. `device-perf` owns `film_match_cache.cpp` this wave: do this after they finish.
2. **`tone.cpp:22`**: delete the `Mode::Off` branch, keep the engine call.
3. **Unit tests that use the old renderer as the implementation under test**: `tests/core_tests.cpp` (about 50 `Renderer(...).renderFloat(...)` sites: look, grading, geometry, crop, film look tests) and `tests/film_tests.cpp` (about 60: film exposure match, film_wb, grain determinism, off / dangling / bogus stock, `renderBakedFloat` bit tests): replace by a tiny helper `renderF(params, src, spatial)` = `engine::renderImageF`; the bit-equality assertions between two renders stay valid (both sides go through the engine), the ones that compare `renderFloat` against `renderBakedFloat` (bake accuracy, `core_tests.cpp:1357-1411`, `film_tests.cpp:1824-1979`) test the baked path, which is now the engine's `Full` frame: re-point them to `EngineSession::renderFrame` against `renderImageF` (the golden corpus already covers it bit for bit).
4. **Benches that exist to compare old vs new**: `cli/edit_bench.cpp` (`Renderer::render total`, `renderBakedRgba` rows, the lattice-size accuracy rows), `cli/film_bench.cpp`, `core/gpu/src/gpu_bench.cpp`, `tests/engine/exec_tests.cpp:392-431` (bench against `renderBakedRgba`): delete the old-renderer rows (the engine rows and the recorded baselines `docs/engine/baselines/edit-bench-desktop.txt`, which says "DAPPLE_ENGINE unset = off", stay as history) or re-point the accuracy rows to `renderImageF`.
5. **GPU parity tests whose reference is `renderBakedRgba`**: `core/gpu/tests/gpu_tests.cpp` (11 sites), `tests/engine/film_gpu_tests.cpp:707`, `tests/engine/gpu/gpu_engine_tests.cpp` (4 sites build a `Renderer` for parameters only: keep): re-point the reference to the CpuExecutor (`renderImageF` / the `Whole` plan), the same thing `gpu_context.cpp` already does for the self-test since wave 4. The parity budgets (mean dE2000 <= 0.1, p99 <= 1.0 for film) are unchanged.
6. **The golden harness** (`tests/engine/golden/golden_tests.cpp`): `runCoreApi` / the `off` mode and the `selfcheck` off half need the old renderer (:622-637, :723-732). Decision for the owner: **freeze the corpus and delete the `off` mode** (the 148 recorded hashes were produced by the old renderer and the `full` engine reproduces them bit for bit on desktop and on the Pixel-class arm64 devices; they remain the oracle as recorded hashes), or keep the old render loops compiled only into `dapple_golden` (about 700 lines in a test-only target) so `selfcheck` can still compare off vs full on a new architecture. The second keeps the architecture-independent check that the first loses: hashes recorded on x86_64 are not valid on arm64 NEON where float kernels differ at the last bit (the corpus has per-arch notes), which is why `selfcheck` exists. **Recommended: move `renderFloat` / `renderBakedRgba` and the loops they own into `tests/engine/golden/legacy_renderer.cpp` and delete them from `core`.** Then `core` has no old pipeline, production has no `Mode::Off`, and the oracle lives where only the test target pays for it.
7. Items 2 - 5 of the table (the C layer, `Mode::Off`, `LegacyRender`, the seams): last, after 1 - 6 are green, in one commit each, running `dapple_golden verify corpus.txt full`, `selfcheck`, `engine.*`, `film*`, `gpu*` after every step.

## 3. Verification already done for this list

* Every public `Renderer` method was grepped over `core cli tests` (counts above); `render`, `renderFloat`, `renderBakedRgba`, `renderBakedFloat`, `renderBakedImpl` are the only ones with no engine caller. The bake pieces, `framePlan`, `pointKey`, `colourKey`, `inputGain`, `whiteBalanceGain`, `sceneGain`, `setToneProxy`, `setToneReach` are used by `cpu_nodes.cpp`, `engine_session.cpp`, `gpu_executor.cpp`, `film_match*.cpp`, `video_frame_session.cpp` and stay.
* `dapple_golden selfcheck` passes at this tree: desktop 148 frames off vs full 0 mismatches, 21 one-shot comparisons identical (`SELFCHECK OK (0)`); on the Android x86_64 emulator (avx2 = 0 kernels) also `SELFCHECK OK (0)`. It is now a named step of `tools/check-all.sh` (`golden-selfcheck`; `--emu` adds `emu-golden-selfcheck`; the by-hand one-liner is in the script header).
* Not verified: anything on a phone, `DAPPLE_ENGINE=off` after the film exposure match is re-pointed (that is the point of step 1), the arm64 `selfcheck` at this tree (last run on the N5 before wave 8).

## 4. Order of work and size

Steps 1 - 5 are mechanical re-points, 1 - 2 days with the golden gates; step 6 is a decision plus a file move (half a day); step 7 deletes roughly 2,500 lines (`renderer.cpp` loops about 1,100, `c_api.cpp` Off branches about 300, `GraphDetail::Legacy` / `LegacyRender` about 250, tests and benches about 800). Nothing here may start before the owner's device sign-off (the Off path is the A/B switch used on phones).

## 5. Status of step 3 (round 4, 2026-11-13): what was re-pointed, nothing deleted

* `tests/render_f.h` (`renderF(params, camera, lut, source, spatial)` = `engine::renderImageF`, aborts with the engine's message on failure).
* `tests/core_tests.cpp` (30 call sites) and `tests/film_tests.cpp` (40 call sites): every `Renderer(params, cam, lut).renderFloat(source[, spatial])` expression was rewritten mechanically (a balanced-parenthesis rewriter script, not kept: the pattern is `Renderer(A, B, C).renderFloat(D[, E])` -> `renderF(A, B, C, D[, E])`). Behaviour identical: `core_tests` and all 30 `film.*` ctest tests pass unchanged (the bit-equality assertions compare two engine renders now, the others compare an engine render with its expectations).
* Left in the tests on purpose: the sites that use `renderBakedFloat` / `renderBakedRgba` / `bake` (baked-path accuracy tests: `core_tests.cpp` 1357 - 1411, `film_tests.cpp` 1824 - 1979, `exec_tests.cpp`, `film_gpu_tests.cpp`, `gpu_tests.cpp`, `golden_tests.cpp`): they test the baked variant or serve as the oracle; they move into the golden target together with the old loops (step 6), together with the 4 `r.renderFloat` calls that sit next to them (`core_tests.cpp` 305, 337, 1360, 1383).
* `cli/film_cmds.cpp`: the two proxy renders (`target` and `keyL0`) go through `engine::renderImageF`; the film command's output for the A7 IV test frame is bit-identical to the previous binary (`dapple film-compare` dE2000 0.000, same exposure line `+1.76`, print `-0.47`).
* Not done: `cli/edit_bench.cpp`, `cli/film_bench.cpp`, `core/gpu/src/gpu_bench.cpp` (they are old-vs-new comparisons by design: their old rows go away together with the old loops), `tone.cpp`'s `Mode::Off` branch and the move of the old loops (`renderFloat`, `renderBakedRgba`, `renderBakedImpl` and what they own, about 1,100 lines) into `tests/engine/golden/legacy_renderer.cpp`: a mechanical move whose risk is the golden hashes, to be done with the owner's go (nothing in `core` was deleted).
