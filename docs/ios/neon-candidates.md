# NEON: where a hand-written version would pay (measured on the M1, nothing written yet)

There is no NEON (or any intrinsic) in the core: `kBase` kernels (`core/src/film_kernels_base.cpp`, `film_spatial.cpp`) are portable scalar code and rely on the auto-vectoriser; the AVX2 variants
(`film_kernels_avx2.cpp`, compiled with `-mavx2 -mfma`, runtime-selected) only exist on x86, so every arm64 build (Android, iOS, Apple silicon) runs `kBase`.

## Measurements (M1, 8 cores, `kernel set: base`, Release, CPU only, public Sony A7 V ARW; desktop = i7-14700KF 28 threads, AVX2, docs/film/architecture.md and edit-bench-desktop.txt)

| | M1 base | i7 AVX2 (28 threads) |
|---|---|---|
| film 26 MP, no spatial effects | 160 ms | 60 ms |
| film 26 MP, all effects | 736 ms | ~280 - 330 ms |
| film 1600 px, all effects, cached bake | 38.1 ms | 18 - 19 ms |
| film 1600 px, preview mode | 22.5 ms | 9 - 10 ms |
| single-frame stage split (26 MP, all effects) | exposure->planar 76.7, scatter+halation 86.2, DIR 78.1 | 39 / 49 / 47 |
| edit-bench 1600 px "pointwise (tone / colour / curves)" | 84.7 ms | 48.7 ms |
| edit-bench 1600 px spatial (sharpen / chroma NR / luma NR / clarity / texture) | 5.2 / 9.1 / 5.9 / 7.3 / 6.4 ms | 17 - 20 / 25 / 20 / 22 / 17 ms (shared machine, 5-50 % noise) |

Per core the M1 `kBase` is already at parity with the desktop (160 ms x 8 cores = 1280 core-ms against 60 ms x 28 threads = 1680 thread-ms), the spatial passes are 3x faster than the recorded desktop numbers, but the
film point kernels and the "pointwise" stage are 1.7 - 2.6x slower than the AVX2 desktop at 3.5x fewer threads.

## Profile (macOS `sample`, 12 s of `dapple film-bench`, self time, 8 threads)

| rank | function | samples | share |
|---|---|---|---|
| 1 | `film::detail::k_base::exposure` (spectral upsampling table -> raw-LUT gather -> exposure) | 15333 | 25 % |
| 2 | `k_base::sp::grainRow` (pcg3d hash + inversion sampling per channel) | 13257 | 21 % |
| 3 | `k_base::printScan` (33^3 tetrahedral print + scan LUT) | 7845 | 13 % |
| 4 - 8 | `firRow` 3580, `firCol` 3329, `block` 3296, `logE` 3270, `memmove` 3065 | | 5 % each |
| 9 - 10 | `develop1` 2932, `develop2` 2623 | | 4 % each |

(the percentages use the sum of the 20 largest entries, ~62 000 samples, as 100 %). Object-code check of the autovectoriser: `film_kernels_base.o` has about 1330 `.4s` vector instructions and 96 scalar float multiplies, `film_spatial.o` 24 vector instructions:
the FIR and blur rows vectorise, the gather / LUT kernels do not.

## The three kernels worth a NEON version, with expected gain

1. **`exposure`** (25 % of the film frame): per pixel an 8-tap gather from a float4-padded 3D LUT plus a 3x3 matrix and `log10`. One tap = one 128-bit `vld1q_f32`, the 4 channels live in the lane axis, the weights and the matrix map to
   `vfmaq_laneq_f32`; the AVX2 variant does exactly this with 256-bit lanes. Expected 2 - 3x on this kernel (-15 % to -17 % of a 26 MP film frame, 1600 px: -2 ms). The exp/log polynomials need `vrndq`/`vcvtq` ports of the existing AVX2 polynomial.
2. **`grainRow`** (21 %, grain is on in most film looks): the generator is integer-only (`pcg3d`: 32-bit multiply, xor, shift), which NEON handles 4 lanes at a time (`vmulq_u32`, `veorq_u32`, `vshrq_n_u32`); the inversion sampler has a branch for `lambda < 4`
   that vectorises with `vbslq`. Expected 3 - 4x on the kernel (-15 % of the frame with grain; 26 MP "grain alone" costs +330 ms on the M1 against +75 ms recorded on the AVX2 desktop: the biggest relative gap).
3. **`printScan`** (13 %): tetrahedral interpolation in a 33^3 float4 LUT: index arithmetic and 4 gathers per pixel, the same structure as `exposure`; the AVX2 version exists, the port is mechanical. Expected 2x on the kernel (-6 %).

Beyond the top three: `logE` / `develop1` / `develop2` (4 % each) are piecewise polynomial and vectorise partially today (check with `-Rpass-missed=loop-vectorize`); the `edit-bench` "pointwise" stage (lattice bake with `PointOp::apply`, 85 ms at 1600 px) is
the largest single CPU item of the interactive pipeline on arm64 after the film kernels and has no SIMD version on any platform.
Gate for any of them: `dapple_golden selfcheck off full` (0 differences) plus the film kernel equivalence tests (`film.kernel_exp`, `film_nodes.equiv`: kBase vs the new set within the existing tolerances).
