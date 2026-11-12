# RAW compatibility sweep (public samples)

Status: measured on HEAD `1088f2d` (engine-v2), desktop CPU build (no GPU), LibRaw 0.22.2 (first sweep; see the wave 8 update at the end). Samples are public CC0 files from raw.pixls.us (no private photos); the list with URLs, sizes and sha256 is `build/rawsamples/MANIFEST.md` (build/ is gitignored; regenerate with `scripts/samples/`). Files live under `build/rawsamples/<maker>/<model>/`.

## How it was run

| step | tool |
|---|---|
| download (sequential, generic client string, 45 files, 1.86 GB) | `scripts/samples/download.sh scripts/samples/list.tsv build/rawsamples` |
| build the CLI from a clean checkout of HEAD | `git worktree add build/rawsweep/wt HEAD`, `cmake -S build/rawsweep/wt -B build/rawsweep/b ... -DDAPPLE_GPU=OFF`, `-j4` |
| probe (one process per file) | `scripts/samples/build_probe.sh build/rawsweep/b build/rawsweep/wt`, then `python -I scripts/samples/sweep.py build/rawsamples <rawprobe.exe> build/rawsweep/out` |
| DNG tags and opcode payloads | `scripts/samples/dng_tags.py <file.dng>` (own TIFF parser, no exiftool on this PC) |
| contact sheets | `python -I scripts/samples/sheet.py build/rawsweep/out` (`sheet_1..4.jpg`) |

`rawprobe` (scripts/samples/rawprobe.cpp) links the core of the tree it was built from. For every file it runs `Developer::develop` (Preview quality = half-size decode as the app opens a file, auto adjust on, 1600 px long edge, JPEG q92), a separate Full-quality decode for timing, `extractEmbeddedPreview`, and reads LibRaw's black/white/WB fields. The comparison against the embedded JPEG preview is done at 320 px long edge in CIE Lab (sRGB assumed on both sides): `dE76` mean, `dE(ab)` (chroma only), `dL` (engine minus preview, mean L*), and the corner:centre luma ratio (four 12 % corner patches over the central 20 % patch; scene dependent, so only the difference between engine and preview on the same frame means anything).

The camera preview is the camera's own look (tone curve, saturation, lens correction), so dE76 of 7..25 is expected for a neutral-baseline engine; what matters is systematic dL, chroma shifts and vignetting. "Camera or DNG-profile CCT" is: the DNG-spec dual-illuminant estimate from ColorMatrix1/2 + AsShotNeutral for DNGs (`dngCct()` in rawprobe.cpp), or the nominal Kelvin of an in-camera WB preset when the as-shot multipliers equal that preset (Fuji "Fine weather"). It is a reference, not ground truth. For Sony/Canon/Fuji/Olympus/Pentax the engine value would come from LibRaw's Kelvin WB table when one exists, so no independent camera value is available there.

## Per-file table

Sensor: type as the engine reports it (`unknown` = no CFA reported: Canon sRAW; `none` = linear/demosaiced DNG). Black/white are LibRaw's (`black` is the common level; per-channel patterns live in cblack and are not shown). Times are CPU, one thread pool, this PC.

| file | result | sensor | size (decoded -> 1600 px out) | black / white | as-shot WB R,B | engine CCT K / duv | camera or DNG-profile CCT K | decode ms (full / half) | dE76 / dE(ab) / dL vs preview | corner:centre engine / preview | DNG opcodes |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Apple/iPhone-12-Pro/IMG_1361.DNG | ok | none | 3024x4032 -> 1200x1600 | 0/65535 | 2.227,1.849 | unknown | 5184 (DNG dual-matrix, duv -0.0013) | 507 / 517 | 12.72 / 6.83 / -9.0 | 0.797 / 0.685 |  |
| Apple/iPhone-XS/IMG_1105.dng | ok | Bayer | 4032x3024 -> 1600x1200 | 528/4095 | 2.306,1.938 | unknown | 5087 (DNG dual-matrix, duv -0.0021) | 649 / 157 | 28.36 / 7.86 / -26.0 | 0.106 / 0.75 | OL3:FixVignetteRadial |
| Canon/EOS-5D-Mark-IV/B13A0732.CR2 | ok | unknown | 3360x2240 -> 1600x1067 | 0/16383 | 1.988,1.474 | 5145 / +0.0019 | - | 304 / 292 | 24.48 / 7.29 / -22.4 | 0.307 / 0.496 |  |
| Canon/EOS-6D-Mark-II/6DII-RAW.CR2 | ok | Bayer | 6264x4180 -> 1600x1068 | 512/14558 | 2.036,1.774 | 4789 / +0.0025 | - | 1480 / 467 | 14.4 / 6.48 / -12.2 | 0.557 / 0.544 |  |
| Canon/EOS-6D-Mark-II/6DII-sRAW.CR2 | ok | unknown | 3120x2082 -> 1600x1068 | 0/16383 | 2.032,1.774 | 4782 / +0.0024 | - | 282 / 280 | 14.79 / 6.22 / -13.0 | 0.566 / 0.537 |  |
| Canon/EOS-R10/IMG_4361.CR3 | ok | Bayer | 4000x6000 -> 1067x1600 | 511/16383 | 1.741,1.897 | 4762 / +0.0072 | - | 1488 / 270 | 22.91 / 6.86 / -21.6 | 1.008 / 1.153 |  |
| Canon/EOS-R5/Canon_EOS_R5_RAW_ISO_100_nocrop_nodual.CR3 | ok | Bayer | 8190x5462 -> 1600x1067 | 510/16383 | 1.620,2.261 | 3994 / +0.0010 | - | 2283 / 617 | 10.18 / 6.7 / -6.6 | 0.194 / 0.219 |  |
| Canon/EOS-R6-Mark-II/canon-r6m2-img_0346-raw-normal-full.cr3 | ok | Bayer | 6022x4024 -> 1600x1069 | 511/16383 | 2.147,1.293 | 6510 / -0.0037 | - | 1231 / 270 | 21.49 / 5.57 / -19.6 | 2.445 / 1.45 |  |
| Fujifilm/X-A7/DSCF1595.RAF | ok | Bayer | 6016x4014 -> 1600x1068 | 0/16383 | 1.680,1.906 | 4675 / +0.0043 | 5200 (WB preset 'Fine weather') | 1158 / 142 | 24.84 / 16.17 / -15.3 | 0.895 / 1.199 |  |
| Fujifilm/X-S10/Fujifilm-X-S10-compressed.RAF | ok | X-Trans | 6246x4170 -> 1600x1068 | 0/16383 | 1.695,2.351 | 4097 / +0.0099 | - | 10835 / 1028 | 18.72 / 7.61 / -13.3 | 0.482 / 0.61 |  |
| Fujifilm/X-S10/Fujifilm-X-S10-lossless-compressed.RAF | ok | X-Trans | 6246x4170 -> 1600x1068 | 0/16383 | 1.695,2.351 | 4097 / +0.0099 | - | 11168 / 1014 | 18.74 / 7.61 / -13.3 | 0.482 / 0.61 |  |
| Fujifilm/X-T50/DSCF0118.RAF | ok | X-Trans | 7752x5178 -> 1600x1069 | 0/16383 | 1.867,1.874 | 5033 / -0.0019 | 5200 (WB preset 'Fine weather') | 16050 / 320 | 23.76 / 11.45 / -19.8 | 2.234 / 2.437 |  |
| Fujifilm/X-T50/DSCF0119.RAF | ok | X-Trans | 7752x5178 -> 1600x1069 | 0/16383 | 1.967,1.825 | 5328 / -0.0005 | - | 17081 / 1542 | 25.29 / 10.86 / -21.8 | 1.87 / 2.36 |  |
| Fujifilm/X-T50/DSCF0120.RAF | ok | X-Trans | 7752x5178 -> 1600x1069 | 0/16383 | 1.924,1.851 | 5188 / -0.0010 | - | 17159 / 1623 | 24.24 / 10.85 / -20.7 | 2.047 / 2.499 |  |
| Google/Pixel-6-Pro/PXL_20220910_093206982.dng | ok | Bayer | 4080x3072 -> 1600x1205 | 0/16368 | 2.216,1.476 | unknown | 4178 (DNG dual-matrix, duv -0.0056) | 847 / 186 | 30.62 / 19.79 / -21.9 | 0.134 / 0.803 | OL2:4xGainMap, OL3:WarpRectilinear |
| Google/Pixel-7-Pro/PXL_20221204_063131635.dng | ok | Bayer | 3882x2924 -> 1600x1205 | 0/16368 | 1.556,1.789 | unknown | 5634 (DNG dual-matrix, duv +0.0271) | 652 / 172 | 27.12 / 14.37 / -14.0 | 0.255 / 0.363 | OL2:4xGainMap, OL3:WarpRectilinear |
| Google/Pixel-8-Pro/PXL_20240415_103400204.RAW-02.ORIGINAL.dng | ok | Bayer | 8160x6144 -> 1600x1205 | 0/16368 | 1.000,1.000 | unknown | - | 2848 / 827 | 22.67 / 6.85 / -21.4 | 0.122 / 0.588 | OL2:4xGainMap, OL3:WarpRectilinear |
| Leica/M-Monochrom-246/M2462362.DNG | **FAIL** Unexpected LibRaw output layout | mono | - | 0/3750 | - | - | - | 427 / - | - | - |  |
| Leica/M10/f5381888.dng | ok | Bayer | 5984x3992 -> 1600x1067 | 0/15000 | 2.062,1.242 | 5320 / -0.0032 | 5180 (DNG dual-matrix, duv -0.0020) | 1382 / 319 | 7.43 / 4.84 / -2.1 | 2.197 / 2.429 |  |
| Leica/Q2/L1000750.DNG | ok | Bayer | 8392x5632 -> 1600x1074 | 0/16383 | 2.041,1.488 | 5371 / +0.0037 | 7235 (DNG dual-matrix, duv -0.0013) | 2362 / 504 | 12.21 / 4.04 / -10.8 | 0.352 / 0.434 | OL1:FixBadPixelsConstant, OL3:WarpRectilinear |
| Nikon/D850/Nikon-D850-14bit-compressed.NEF | ok | Bayer | 8288x5520 -> 1600x1066 | 400/16383 | 1.900,1.375 | 5291 / -0.0047 | - | 2585 / 636 | 7.59 / 4.84 / -1.5 | 2.814 / 5.116 |  |
| Nikon/D850/Nikon-D850-14bit-lossless-compressed.NEF | ok | Bayer | 8288x5520 -> 1600x1066 | 400/16383 | 1.900,1.375 | 5291 / -0.0047 | - | 2393 / 640 | 7.71 / 4.9 / -1.4 | 2.796 / 5.136 |  |
| Nikon/Z-8/Nikon_Z8_high_efficiency_low.NEF | **FAIL** LibRaw unpack failed: Unsupported file format or not RAW file | ? | - | 0/0 | - | - | - | 3 / - | - | - |  |
| Nikon/Z-8/Nikon_Z8_raw_14_bit_lossless_compression.NEF | ok | Bayer | 8280x5520 -> 1600x1067 | 1008/16383 | 1.908,1.537 | 5053 / +0.0050 | - | 2585 / 673 | 17.0 / 10.25 / -11.0 | 3.647 / 2.223 |  |
| OM-System/OM-1/OM_Systems_OM1_RAW_Image_20mpx_P1010025.ORF | ok | Bayer | 5220x3912 -> 1600x1199 | 254/4095 | 2.414,1.711 | 5871 / +0.0041 | - | 1286 / 464 | 20.71 / 6.23 / -18.9 | 1.35 / 1.039 |  |
| OM-System/OM-1/OM_Systems_OM1_RAW_image_HiRes_50mpx_P1010026.ORF | ok | Bayer | 8170x6132 -> 1600x1201 | 254/4095 | 2.414,1.711 | 5871 / +0.0041 | - | 3085 / 955 | 20.11 / 6.21 / -18.4 | 1.288 / 1.027 |  |
| Olympus/E-M1-Mark-III/_3160531.ORF | ok | Bayer | 5240x3912 -> 1600x1195 | 254/4095 | 1.703,2.367 | 4363 / +0.0034 | - | 1202 / 399 | 19.14 / 3.87 / -17.2 | 2.071 / 2.061 |  |
| OnePlus/A6003/IMG_20201019_093659.dng | ok (no embedded preview) | Bayer | 4656x3496 -> 1600x1201 | 0/1023 | 1.947,1.855 | unknown | 3896 (DNG dual-matrix, duv -0.0059) | 770 / 110 | - | - |  |
| Panasonic/DC-GH6/1-1.RW2 | ok | Bayer | 5792x4352 -> 1600x1202 | 2048/65535 | 2.762,1.984 | 4797 / +0.0020 | - | 1548 / 396 | 15.19 / 8.53 / -8.8 | 0.441 / 0.42 |  |
| Panasonic/DC-S5/P1047510.RW2 | ok | Bayer | 6024x4016 -> 1600x1067 | 512/16319 | 1.906,2.117 | 4286 / +0.0039 | - | 1143 / 160 | 18.11 / 3.62 / -17.0 | 0.783 / 0.823 |  |
| Panasonic/DC-S5/dc-s5_hr_96MP.RW2 | ok | Bayer | 12032x8018 -> 1600x1066 | 509/16319 | 1.996,1.922 | 4639 / +0.0020 | - | 4851 / 901 | 15.88 / 2.86 / -13.1 | 2.86 / 3.049 |  |
| Pentax/K-1-Mark-II/_IMG2390.DNG | ok | Bayer (4 frames in file) | 7376x4932 -> 1600x1070 | 0/16318 | 2.125,1.582 | 4999 / -0.0031 | 5026 (DNG dual-matrix, duv -0.0030) | 1922 / 436 | 7.71 / 2.81 / -5.3 | 2.55 / 2.959 |  |
| Pentax/K-1-Mark-II/_K1I9785.PEF | ok | Bayer | 4832x3220 -> 1600x1066 | 64/16383 | 2.059,1.508 | 5000 / -0.0075 | - | 807 / 180 | 18.82 / 5.98 / -17.2 | 2.869 / 2.905 |  |
| Pentax/K-3-Mark-III-Monochrome/K3III.DNG | **FAIL** Unexpected LibRaw output layout | mono | - | 64/16378 | - | - | - | 461 / - | - | - |  |
| Pentax/K-3-Mark-III/IMGP9854.PEF | ok | Bayer | 6224x4160 -> 1600x1069 | 256/16383 | 2.484,1.820 | 4803 / +0.0028 | - | 1682 / 311 | 14.91 / 9.87 / -9.4 | 0.562 / 0.438 |  |
| Realme/RMX1851/IMG_OCT_20191005_163311.dng | ok (no embedded preview) | Bayer | 4656x3496 -> 1600x1201 | 0/1023 | 2.004,1.872 | unknown | 3481 (DNG dual-matrix, duv -0.0042) | 955 / 140 | - | - | OL2:4xGainMap, OL3: |
| Samsung/Galaxy-S23-Ultra/20230817_120455.dng | ok | none | 3648x2736 -> 1600x1200 | 0/4095 | 2.012,1.784 | unknown | 4570 (DNG dual-matrix, duv +0.0003) | 440 / 447 | 7.76 / 5.73 / -2.5 | 1.343 / 1.444 | OL2:4xGainMap |
| Samsung/Galaxy-S23/20230715_154550.dng | ok | none | 4000x2252 -> 1600x901 | 0/4095 | 1.928,1.778 | unknown | 4858 (DNG dual-matrix, duv -0.0043) | 402 / 380 | 7.44 / 4.79 / -0.8 | 0.633 / 0.602 | OL2:4xGainMap |
| Samsung/SM-S901E/20250305_173647.dng | **FAIL** LibRaw unpack failed: Unsupported file format or not RAW file | ? | - | 0/0 | - | - | - | 1 / - | - | - | OL2:4xGainMap |
| Sony/ILCE-6700/DSC00002.ARW | ok | Bayer | 6240x4168 -> 1600x1069 | 512/16383 | 2.440,1.715 | 4891 / -0.0052 | - | 1799 / 307 | 14.3 / 10.29 / +5.0 | 14.092 / 23.299 |  |
| Sony/ILCE-7M4/ILCE-7M4_DSC06674_FullFrame-LossLess-Compressed-Large.ARW | ok | Bayer | 7028x4688 -> 1600x1067 | 512/16383 | 2.368,1.758 | 4895 / +0.0038 | - | 2521 / 466 | 10.02 / 6.7 / +5.3 | 2.102 / 2.551 |  |
| Sony/ILCE-7M5/full_compressed.ARW | **FAIL** LibRaw open failed: Unsupported file format or not RAW file | ? | - | None/None | - | - | - | 3 / - | - | - |  |
| Sony/ILCE-7RM4/DSC00396.ARW | ok | Bayer | 9568x6376 -> 1600x1066 | 512/16380 | 3.070,1.500 | 7097 / +0.0082 | - | 3285 / 507 | 11.98 / 4.38 / +10.6 | 3.688 / 6.598 |  |
| Xiaomi/M2010J19CG/IMG_20201224_141323.dng | ok (no embedded preview) | Bayer | 4000x3000 -> 1600x1200 | 0/1023 | 1.812,1.544 | unknown | 5263 (DNG dual-matrix, duv +0.0048) | 617 / 89 | - | - | OL2:4xGainMap, OL3:WarpRectilinear |
| Xiaomi/Mi-9-Lite/IMG_20200324_073611.dng | ok (no embedded preview) | Bayer | 3000x4000 -> 1200x1600 | 0/1023 | 1.835,1.718 | unknown | 4391 (DNG dual-matrix, duv -0.0060) | 723 / 108 | - | - | OL2:4xGainMap, OL3: |

Failures are clean errors (no crash, no hang). Full-decode time is the 45-file CPU path; the app uses the half-size decode first.

Contact sheets: `build/rawsweep/out/sheet_1.jpg` .. `sheet_4.jpg` (engine left, camera preview right). Per-file outputs: `build/rawsweep/out/<maker>__<model>__<file>.jpg` (and `_prev.jpg`), raw numbers: `build/rawsweep/out/results.jsonl`.

## Ranked defects (HEAD, with evidence)

1. **Non-Sony renders are systematically 13..20 L* darker than the camera's own render; Sony is 5..10 L* brighter.** Mean dL by maker: Apple -17.5, Canon -15.9, Fujifilm -17.4, Google -19.1, OM System -18.6, Olympus -17.2, Panasonic -13.0, Pentax -10.6, Leica -6.4, Nikon -4.7, Samsung -1.7, Sony +7.0. Mean luma (0..255) engine vs preview: X-T50 DSCF0119 60.7 vs 110.0, EOS R6 II 54.5 vs 100.8, EOS R10 110 vs 164, OM-1 100 vs 146. All of these resolve to camera profile `generic` or `fujifilm.generic` (only Sony and the X-T50 have data; the X-T50 profile `fujifilm.x-t50` is still about 20 L* low, so it is not calibrated either). Visible on sheets 1-2 (X-T50 sky is a dull grey-blue, trees nearly black). No colour casts and no magenta highlights were seen; the gap is exposure/tone, and auto exposure only acts on "uniformly dark" frames (+0.80 EV on EOS R5, D850).
2. **Phone DNG lens shading and geometry are not applied (OpcodeList2 GainMap, OpcodeList3 WarpRectilinear, FixVignetteRadial).** HEAD's `libraw_decoder.cpp` has no opcode support (an uncommitted WIP exists in the working tree, `core/src/decoders/dng_opcodes.*`, not evaluated here). Evidence: Pixel 6 Pro corners are near black (corner:centre 0.134 vs 0.803 in the preview; the file's GainMap says corner gain 4.2..4.7x), iPhone XS (FixVignetteRadial, corner:centre 0.106 vs 0.750, mean luma 55 vs 115), Mi 9 Lite and OnePlus A6003 show dark corners on sheets 3-4 (GainMap corner gain 3.5..4.3x). Pixel 8 Pro (corner gain 3.0..4.2x) and Pixel 7 Pro (1.4..1.57x) are the same failure at other strengths. Samsung S23 GainMaps are 1x1 identity (nothing to correct) and iPhone 12 Pro ProRAW carries no opcodes, so those are unaffected.
3. **Illuminant/CCT and tint are unknown (0) for all 11 phone/iPhone DNGs.** LibRaw leaves `color.cam_xyz` zero for these files, and `readMetadata` derives `asShotIlluminantXy` and `asShotCct` only from `cam_xyz`. The same DNG-spec estimate from `dng_color[].colormatrix` works (scripts/samples/rawprobe.cpp `dngCct`, 3481..5634 K on the phones). Pixel 8 Pro stores `AsShotWhiteXY` (0.32205, 0.32665; about 6020 K by McCamy) instead of `AsShotNeutral`, so LibRaw reports cam_mul = [0,1,0] and the engine metadata says as-shot WB = (1,1,1); the rendered pixels do look balanced (sheet 2), so only the metadata and anything that consumes it (film white balance, scene CCT label) is affected. How LibRaw balanced those pixels was not checked.
4. **Files that fail to decode** (clean errors; two messages are cryptic):
   - Sony ILCE-7M5 ARW: `LibRaw open failed: Unsupported file format or not RAW file` (LibRaw 0.22.2 predates the body). The embedded preview fails too, because `extractEmbeddedPreview` also goes through LibRaw, so the app cannot even show a placeholder.
   - Nikon Z8 High-Efficiency NEF (TicoRAW): `LibRaw unpack failed: Unsupported file format or not RAW file`.
   - Samsung SM-S901E (Galaxy S22) DNG: `LibRaw unpack failed: Unsupported file format or not RAW file`; the file is DNG 1.7 with JPEG XL compression (Compression tag 52546, LinearRaw 16 bit).
   - Monochrome sensors, Leica M Monochrom (Typ 246) DNG and Pentax K-3 Mark III Monochrome DNG: `Unexpected LibRaw output layout` (LibRaw gives 1 colour; the message should say "monochrome not supported").
5. **X-Trans full-resolution decode is 5..10x slower than Bayer.** Full decode: X-T50 14.7..16.1 s (40 MP), X-S10 10.2..10.6 s (26 MP); Bayer 45 MP D850 2.4 s, 61 MP A7R IV 2.9 s. Half-size decode of the lossless-compressed RAFs takes 1.0..1.5 s against 0.27 s for the uncompressed X-T50 file (DSCF0118).
6. **Embedded preview ignores the DNG Orientation tag.** Pixel 7 Pro DNG has Orientation 3 (180 deg); the engine output is upright, `extractEmbeddedPreview` returns the preview rotated 180 deg (sheet 2). dE for that file (27) is therefore meaningless. (The other previews matched the engine's orientation.)
7. **Lower severity.**
   - Pentax K-1 Mark II pixel-shift DNG (`_IMG2390.DNG`, 4 raw frames, 161 MB): decodes frame 1 only as a normal 36 MP Bayer image, silently; no merge and no warning. Panasonic S5 96 MP and OM-1 50 MP "HiRes" files are camera-composed and decode as ordinary large Bayer files.
   - Canon sRAW CR2 (6D II sRAW, 5D IV small file) decode and look right, but the engine reports CFA `unknown`.
   - Sony A6700: corners are dark with a blue tint; the camera preview has the lens profile applied (no lens corrections from makernotes are used).
   - Galaxy S23 DNG: LibRaw prints `data corrupted at 4384328` on unpack; the decode still completes and looks correct.
   - Engine CCT versus the camera's WB preset: X-A7 (Fine weather, 5200 K) engine 4675 K = +21.6 mired; X-T50 DSCF0118 (same preset) 5033 K = +6.4 mired. DNG reference: M10 -5.1 mired, K-1 II +1.1 mired, Leica Q2 +48 mired (illuminants A / D50; the reference may be the unreliable side). Not enough points to call a bias.
   - No banding, no X-Trans worm artefacts and no wrong crop were seen at the 1600 px outputs (contact sheets and two full-size outputs); a 100 % pixel peep of X-Trans fine detail was not done.

## Hand-off: DNG opcodes and non-Sony illuminant

Reference code: `scripts/samples/dng_tags.py` (opcode parser, big-endian payloads) and `scripts/samples/rawprobe.cpp` (`dngCct`: DNG-spec iteration neutral -> xy with ColorMatrix1/2 interpolated in mired, calibration illuminant code -> CCT table).

| need | files (under `build/rawsamples/`) |
|---|---|
| GainMap, large grid, 4 maps per file (one per Bayer site, rowPitch=colPitch=2, area offsets (0,0),(0,1),(1,0),(1,1)) | `Google/Pixel-6-Pro/PXL_20220910_093206982.dng`, `Google/Pixel-7-Pro/PXL_20221204_063131635.dng`, `Google/Pixel-8-Pro/PXL_20240415_103400204.RAW-02.ORIGINAL.dng` (30x40 grid) |
| GainMap, 13x17 grid, flags=1 (optional), maps in IFD0 | `Xiaomi/M2010J19CG/IMG_20201224_141323.dng`, `Xiaomi/Mi-9-Lite/IMG_20200324_073611.dng`, `Realme/RMX1851/IMG_OCT_20191005_163311.dng` |
| GainMap 1x1 identity (must be a no-op) | `Samsung/Galaxy-S23/20230715_154550.dng`, `Samsung/Galaxy-S23-Ultra/20230817_120455.dng` (LinearRaw 12 bit, already demosaiced), `Samsung/SM-S901E/20250305_173647.dng` (JPEG XL, does not decode) |
| WarpRectilinear, 1 plane | Pixel 6/7/8 Pro; `Xiaomi/M2010J19CG/...` has identity coefficients and centre (0,0), a degenerate case to handle |
| WarpRectilinear, 3 planes (per-channel, lateral CA) plus FixBadPixelsConstant in OpcodeList1 | `Leica/Q2/L1000750.DNG` |
| FixVignetteRadial in OpcodeList3 | `Apple/iPhone-XS/IMG_1105.dng` |
| empty OpcodeList3 (tag present, zero opcodes) | `Realme/RMX1851/...`, `Xiaomi/Mi-9-Lite/...` |
| no opcodes, linear DNG with corrections already applied | `Apple/iPhone-12-Pro/IMG_1361.DNG` |
| no embedded preview (cannot compare, check by eye) | OnePlus A6003, both Xiaomi, Realme |
| dual-matrix DNGs for the illuminant estimate | all phone DNGs, Leica M10 / Q2, Pentax K-1 II `_IMG2390.DNG`; `AsShotWhiteXY` only: Pixel 8 Pro |
| non-DNG, non-Sony illuminant check (in-camera WB preset equals as-shot) | `Fujifilm/X-A7/DSCF1595.RAF`, `Fujifilm/X-T50/DSCF0118.RAF` (Fine weather) |

Payloads decoded from the files (WarpRectilinear is kr0..kr3, kt0, kt1 per plane, then the normalised centre cx, cy; GainMap grids are MapPointsV x MapPointsH with spacing 1/(points-1) and origin 0):

- **Pixel 8 Pro** (SubIFD0, active area 8160x6144, DefaultCrop 8144x6128 at (8,8)): OpcodeList2 = 4 GainMap, each 30x40, one plane, area = whole sensor, row/col pitch 2, starting at (0,0), (0,1), (1,0), (1,1); MapSpacing 0.034483 x 0.025641; first map gain range 1.004..4.219 (top-left 3.98, top-right 3.84, bottom-left 4.22, bottom-right 4.05, centre 1.014), other three sites peak 3.2..3.5. OpcodeList3 = WarpRectilinear flags=1 (optional), 1 plane, kr = [0.98970592, 0.02857298, -0.05583058, 0.03774713], kt = [0, 0], centre (0.50089, 0.498739).
- **Pixel 6 Pro** (SubIFD0, 4080x3072): 4 GainMap 30x40, gains 1.018..4.683 (corners 4.55 / 4.47 / 4.68 / 4.64, centre 1.02); WarpRectilinear flags=1, kr = [0.9735294, 0.07117444, -0.06845544, 0.0236339], centre (0.494874, 0.500959).
- Pixel 7 Pro (3882x2924): 4 GainMap 30x40, gains 1.001..1.567; WarpRectilinear kr = [0.98969603, 0.00994558, -0.00318311, 0.00303723], centre (0.499836, 0.491674).
- Leica Q2: WarpRectilinear flags=0 (mandatory), 3 planes, kr R = [0.99962163, -0.04787845, -0.09821651, 0.0554717], G = [0.99925111, -0.04720699, -0.09855991, 0.05532431], B = [0.999177, -0.04597954, -0.09942464, 0.05525776], kt = 0, centre (0.5, 0.5); OpcodeList1 FixBadPixelsConstant (8 bytes).
- iPhone XS: FixVignetteRadial k0..k4 = [1.51229043, 4.19174773, -9.2382389, 8.7175762, -2.75034023], centre (0.499763, 0.498417).
- Xiaomi M2010J19CG / Mi 9 Lite / Realme: GainMap 13x17, spacing 0.083333 x 0.0625, corner gains up to 3.88 / 4.29 / 2.78, centre about 1.0.

Notes for the implementer: the four Pixel GainMaps apply to the four Bayer sites, so they must run on the mosaic before demosaic (as the WIP in the working tree intends); OpcodeList3 acts on the demosaiced, white-balanced image and the DefaultCrop applies after it. The DNG Orientation tag (Pixel 7 Pro = 3) needs the same treatment in the embedded-preview path. Samsung LinearRaw DNGs carry identity GainMaps on an already demosaiced image.

For the non-Sony illuminant estimate: `asShotCct` currently equals the matrix-only value (within 1..40 K) on every non-Sony file (Canon 3994..6510 K, Fuji 4097..5328 K, Nikon 5053..5291 K, Panasonic 4286..4797 K, OM 4363..5871 K, Pentax 4803..5000 K, Leica M10/Q2 5320/5371 K), while LibRaw's `cam_xyz` is zero for every phone DNG. The only reference-like data in this set are the Fuji WB presets (X-A7, X-T50 DSCF0118) and the DNG dual-matrix estimate. Canon (15 points), Fuji (31), Olympus (12) and Pentax (17) also expose a WBCT table in LibRaw; the engine value for them is within 1..40 K of the matrix-only value, so check whether the table is actually being used for them.

## Not verified

- GPU path (this sweep is the CPU executor, DAPPLE_GPU=OFF); the app default is GPU.
- Absolute colour accuracy (only a comparison with each camera's embedded JPEG; no chart measurements); black/white level correctness against vendor specs (values are LibRaw's; Panasonic GH6 reports white 65535 with black 2048 and 16 bit, which looks high and was not investigated); per-channel black patterns.
- exiftool is not installed on this PC, so non-DNG tags were not cross-checked; the camera's own Kelvin value exists only where noted.
- The uncommitted DNG opcode WIP in the working tree, and files not in the list (Canon CR3 CRAW/dual-pixel variants, Nikon lossy NEF besides D850 14-bit compressed, Sony ILCE-1/9M3 and the other bodies on the site).
- Not downloaded: Fujifilm X-T5 / X-S20 / X-M5, Pixel 4a, Galaxy S22 Ultra (SM-S901E was taken instead), the Pentax K-3 III pixel-shift file (IMGP9855.PEF, 115 MB), Panasonic G9 II / GH6 hi-res files, Leica SL2 / M10-R, other iPhone ProRAW files, Sony ILCE-7M4 small/medium lossless. Fujifilm X-T50 uncompressed vs compressed was taken as the three files on the site (84 / 45 / 28 MB); which of them is compressed was not read from the RAF header (only DSCF0118, the 84 MB one, behaves as uncompressed by its 0.27 s half decode).


## Wave 8 update (engine-gaps engineer, 2026-11-13): what changed, HEAD plus the working tree of wave 8, LibRaw snapshot 202611

Sweep regression (`rawprobe` + `sweep.py` against `build/w8b`, the CLI of this tree, CPU): **44 of 45 files decode and develop** (HEAD: 40 of 45). Remaining failure: Samsung SM-S901E (Galaxy S22) DNG, JPEG XL compression (tag 52546). ctest 93 of 93 pass (desktop), the Android x86_64 emulator and arm64 builds are listed at the end.

| defect of the first sweep | state |
|---|---|
| 1. non-Sony renders 13 - 20 L* too dark | **fixed without any maker table**: per-file exposure anchor from the embedded preview (pipeline-architecture.md section 29). Mean absolute dL 8.1 -> 2.1 L*, X-T50 -19 .. -21 -> -1 .. -3. Table below |
| 2. phone DNG opcodes | **fixed**: GainMap, FixVignetteRadial, WarpRectilinear + DefaultCrop (section 27). Pixel 6 Pro corner:centre (display space) 0.43 -> 0.79 against the preview 0.88; the Pixel 6 Pro linear ratio 0.134 -> 0.54. `FixBadPixelsConstant` (Leica Q2, OpcodeList1, 8 bytes): **not applied** (reported as skipped): the constant is the value of the bad pixels in the DNG's linearised domain and LibRaw's own bad-pixel handling does not know it (no bad pixel check was made on the Leica Q2 frame) |
| 3. illuminant for phone DNGs | **fixed**: dual-matrix solve of the DNG specification for the 10 phones with AsShotNeutral, AsShotWhiteXY read from the file for the Pixel 8 Pro (exact xy, multipliers from the forward model, decode uses them; before the decode used an equal-energy white, 5450 K instead of 6020 K). All phone DNGs now report an illuminant. Not verifiable against ground truth (section 27.2) |
| 4a. Sony ILCE-7M5 (A7 V) ARW | **fixed by upgrading LibRaw** from the 0.22.2 release to upstream master snapshot 202611 (LibRaw 0.23.0 Snapshot202611, commit a9a14b19, 2026-11-04, no local patches, same CDDL / LGPL dual licence, used under CDDL-1.0 as before): decodes, renders, WBCT table used (8500 K), preview extracted |
| 4b. Nikon Z8 High-Efficiency NEF | **fixed by the same upgrade**: the snapshot's changelog lists "Nikon HE/HE* format (Z6-III, Z8, Z9, Zf)" (new `decoders/nikonhe.cpp`, about 8 bytes of intermediate buffer per pixel: about 400 MB peak for a 45 MP Z9 frame, single-threaded unless built with OpenMP). The Z8 HE sample decodes and looks right (2.1 s for the half-size decode on this PC) |
| 4c. Samsung S22 DNG with JPEG XL | **not fixed.** LibRaw's DNG 1.7 JPEG XL support needs the Adobe DNG SDK 1.7.x integration (`USE_DNGSDK`: the SDK sources under the Adobe SDK licence plus libjxl), neither is vendored. The decoder reports `LibRaw unpack failed: Unsupported file format or not RAW file`. The route: a small own JPEG XL tile decoder for DNG 1.7 (parse TIFF tile offsets, libjxl `JxlDecoder` per tile, feed the LinearRaw samples to the pipeline in place of LibRaw's), libjxl as a pinned BSD-3 dependency (size estimate, not measured: roughly 1 - 1.5 MB per ABI with the encoder left out; highway, brotli, skcms come with it; record in docs/legal). About 2 - 3 days including Android builds; not started in this wave |
| 4d. monochrome sensors | **decode fixed** (single-channel output replicated to three neutral channels; Leica M Monochrom and Pentax K-3 III Monochrome open, develop and film). **Level not right:** the monochrome frames render 5 / 19 L* brighter than the camera preview at 0 EV and the anchor cannot repair it (the previews fail the spread test, spread 2.1 / 0.9 EV); open, needs a look at the mono white level / scaling (data mean 0.52 of white in the K-3 III file) |
| 5. embedded preview ignores DNG Orientation | **fixed** for orientation 3 (180 degrees: Pixel 7 Pro); 90 degree cases were already handled |
| 6. X-Trans full decode slow (15 s for 40 MP) | **not done** (the time is in LibRaw's X-Trans demosaic, presumed, not profiled: the app uses LibRaw's default quality; a 1-pass setting would be faster at a visible cost; neither the profile nor the alternatives were measured this wave). The half-size decode the app opens with is 0.3 - 1.6 s |
| 7a. Pentax pixel-shift decodes frame 1 silently | **metadata fixed** (`raw format: multi-shot file (4 frames), frame 1 decoded, no composite`); a composite was not built: LibRaw offers none and the shift order of the Pentax frames would have to be verified against the file |
| 7b. Canon sRAW CFA `unknown` | **fixed**: reported as `none` (three-colour image, no mosaic) |
| 7c. Samsung S23 "data corrupted at 4384328" | **investigated, benign**: the message is LibRaw's lossless-JPEG decoder at file offset 4.38 MB; the decoded 4000 x 2252 frame has no zero rows, the largest row-to-row mean step is 11x the median step (a scene edge) and tile borders show no step; the pixels are complete. The warning is still printed by LibRaw on stderr |
| 7d. Panasonic GH6 white level 65535 | **not verified** (no tag dump tool for RW2 here; LibRaw's black 2048 / white 65535 with 16 bit is what the file reports; the render's mid-tones agree with the preview within +3.5 L* after the anchor, which a white level error of that kind would not allow) |
| 7e. Sony A6700 corners, lens corrections | **not done**: the ARW carries the lens profile as Sony maker data (distortion / vignetting / CA tables in the SR2 private IFD) which LibRaw does not decode; applying it needs a maker-data parser (the Sony tables are undocumented). The DNG opcodes cover phones and Leica only |

Fuji DR modes: `raw format` now carries `DR<n>, exposure mid-point shift <x>` from the maker notes.

### Exposure anchor: per-file results (tier, EV, confidence, mean L* difference to the camera JPEG)

`scripts/samples/anchor_table.py`, engine auto render vs embedded preview at 400 px. dL off = profile baseline only (`DAPPLE_ANCHOR=off`), dL anchor = as shipped, last column = profile forced to `generic` (`DAPPLE_PROFILE=generic`). Files: the 45 public samples (those that decode) and 22 of the owner's A7R V frames (private, not listed in the manifest).

| file | camera | tier | EV | conf | JPEG mean L* | dL off | dL anchor | dL anchor, generic profile |
|---|---|---|---|---|---|---|---|---|
| IMG_1361.DNG | Apple iPhone13,3 back camera | 1 | +0.54 | 0.49 | 52.5 | -8.5 | -0.5 | -0.5 |
| IMG_1105.dng | Apple iPhone11,2 back camera | 1 | +1.23 | 0.70 | 48.9 | -18.0 | -0.8 | -0.8 |
| B13A0732.CR2 | Canon EOS 5D Mark IV | 1 | +1.72 | 0.73 | 53.9 | -22.3 | +0.6 | +0.6 |
| 6DII-RAW.CR2 | Canon EOS 6D Mark II | 1 | +0.95 | 0.74 | 39.4 | -11.8 | +0.6 | +0.6 |
| 6DII-sRAW.CR2 | Canon EOS 6D Mark II | 1 | +1.00 | 0.77 | 40.6 | -12.6 | +0.5 | +0.5 |
| IMG_4361.CR3 | Canon EOS R10 | 1 | +1.42 | 0.70 | 69.5 | -21.5 | -1.1 | -1.1 |
| Canon_EOS_R5_RAW_ISO_100_nocrop_nodual.CR3 | Canon EOS R5 | 1 | +1.58 | 0.77 | 29.5 | -6.5 | +2.6 | +2.6 |
| canon-r6m2-img_0346-raw-normal-full.cr3 | Canon EOS R6 Mark II | 1 | +1.87 | 0.72 | 42.1 | -19.4 | +2.6 | +2.6 |
| DSCF1595.RAF | Fujifilm X-A7 | 1 | +0.90 | 0.64 | 57.8 | -15.2 | -1.3 | -1.3 |
| Fujifilm-X-S10-compressed.RAF | Fujifilm X-S10 | 1 | +1.14 | 0.69 | 44.3 | -13.2 | +1.7 | +1.7 |
| Fujifilm-X-S10-lossless-compressed.RAF | Fujifilm X-S10 | 1 | +1.14 | 0.69 | 44.4 | -13.2 | +1.7 | +1.7 |
| DSCF0118.RAF | Fujifilm X-T50 | 1 | +1.44 | 0.67 | 44.4 | -19.3 | -1.2 | -1.2 |
| DSCF0119.RAF | Fujifilm X-T50 | 1 | +1.38 | 0.71 | 47.4 | -21.4 | -3.3 | -3.3 |
| DSCF0120.RAF | Fujifilm X-T50 | 1 | +1.40 | 0.67 | 44.7 | -20.3 | -2.4 | -2.4 |
| PXL_20220910_093206982.dng | Google Pixel 6 Pro | 1 | +0.80 | 0.56 | 46.0 | -12.1 | -0.8 | -0.8 |
| PXL_20221204_063131635.dng | Google Pixel 7 Pro | 1 | +0.85 | 0.74 | 43.5 | -11.1 | +0.2 | +0.2 |
| PXL_20240415_103400204.RAW-02.ORIGINAL.dng | Google Pixel 8 Pro | 1 | +1.00 | 0.64 | 44.8 | -12.9 | +0.7 | +0.7 |
| f5381888.dng | Leica M10 | 1 | +0.03 | 0.77 | 52.1 | -1.7 | -1.4 | -1.4 |
| M2462362.DNG | Leica M MONOCHROM (Typ 246) | 3 | +1.17 | 0.20 | 23.1 | +5.0 | +20.5 | +20.5 |
| L1000750.DNG | Leica Q2 | 1 | +0.78 | 0.95 | 46.6 | -9.2 | +1.4 | +1.4 |
| Nikon-D850-14bit-compressed.NEF | Nikon D850 | 3 | +1.66 | 0.20 | 26.7 | -1.3 | +9.5 | +9.5 |
| Nikon-D850-14bit-lossless-compressed.NEF | Nikon D850 | 3 | +1.66 | 0.20 | 26.6 | -1.2 | +9.6 | +9.6 |
| Nikon_Z8_high_efficiency_low.NEF | Nikon Z 8 | - | - | - | 39.9 | - | - | - |
| Nikon_Z8_raw_14_bit_lossless_compression.NEF | Nikon Z 8 | 1 | +1.01 | 0.51 | 40.4 | -10.2 | +1.4 | +1.4 |
| _3160531.ORF | Olympus E-M1 Mark III | 1 | +1.23 | 0.86 | 49.8 | -17.1 | +1.0 | +1.0 |
| OM_Systems_OM1_RAW_Image_20mpx_P1010025.ORF | Olympus OM-1 | 1 | +1.27 | 0.64 | 61.3 | -18.4 | +0.6 | +0.6 |
| OM_Systems_OM1_RAW_image_HiRes_50mpx_P1010026.ORF | Olympus OM-1 | 1 | +1.27 | 0.70 | 60.5 | -18.0 | +1.2 | +1.2 |
| 1-1.RW2 | Panasonic DC-GH6 | 1 | +0.85 | 0.69 | 37.8 | -8.2 | +3.5 | +3.5 |
| dc-s5_hr_96MP.RW2 | Panasonic DC-S5 | 1 | +0.98 | 0.70 | 60.8 | -12.7 | +2.7 | +2.7 |
| P1047510.RW2 | Panasonic DC-S5 | 1 | +1.15 | 0.77 | 63.7 | -16.9 | +2.1 | +2.1 |
| _IMG2390.DNG | Pentax K-1 Mark II | 1 | +0.47 | 0.71 | 45.4 | -5.0 | +1.6 | +1.6 |
| _K1I9785.PEF | Pentax K-1 Mark II | 1 | +1.26 | 0.65 | 40.7 | -16.4 | -0.5 | -0.5 |
| IMGP9854.PEF | Pentax K-3 Mark III | 1 | +0.95 | 0.77 | 38.3 | -9.1 | +1.1 | +1.1 |
| K3III.DNG | Pentax K-3 Mark III Monochrome | 2 | -0.51 | 0.50 | 56.1 | +19.0 | +11.3 | +11.3 |
| 20230715_154550.dng | Samsung Galaxy S23 | 1 | +0.23 | 0.75 | 45.3 | -0.7 | +2.7 | +2.7 |
| 20230817_120455.dng | Samsung Galaxy S23 Ultra | 1 | +0.17 | 0.71 | 47.0 | -2.2 | +0.4 | +0.4 |
| 20250305_173647.dng | Samsung SM-S901E | - | - | - | 68.3 | - | - | - |
| DSC00002.ARW | Sony ILCE-6700 | 1 | +1.07 | 0.63 | 53.1 | +5.8 | +0.7 | -0.7 |
| ILCE-7M4_DSC06674_FullFrame-LossLess-Compressed-Large.ARW | Sony ILCE-7M4 | 1 | +1.00 | 0.65 | 47.5 | +6.2 | +0.3 | +0.3 |
| DSC00396.ARW | Sony ILCE-7RM4 | 3 | +0.55 | 0.20 | 38.7 | +10.8 | +2.2 | +2.2 |
| DSC00113.ARW | Sony ILCE-7RM5 | 1 | +1.33 | 0.73 | 42.6 | -0.2 | -1.0 | -0.2 |
| DSC00138.ARW | Sony ILCE-7RM5 | 1 | +1.27 | 0.73 | 52.3 | +2.9 | +0.7 | +0.8 |
| DSC00163.ARW | Sony ILCE-7RM5 | 1 | +1.47 | 0.77 | 39.0 | -0.8 | +0.2 | +0.6 |
| DSC00188.ARW | Sony ILCE-7RM5 | 1 | +1.79 | 0.79 | 52.6 | -2.4 | +3.1 | +2.5 |
| DSC00213.ARW | Sony ILCE-7RM5 | 1 | +1.54 | 0.75 | 49.4 | -1.3 | +0.6 | +0.8 |
| DSC00238.ARW | Sony ILCE-7RM5 | 1 | +1.52 | 0.76 | 55.7 | +0.8 | +2.3 | +2.1 |
| DSC00263.ARW | Sony ILCE-7RM5 | 1 | +1.60 | 0.81 | 51.0 | -1.9 | +1.1 | +0.8 |
| DSC00288.ARW | Sony ILCE-7RM5 | 1 | +1.34 | 0.75 | 34.3 | +2.7 | +0.9 | +2.0 |
| DSC00313.ARW | Sony ILCE-7RM5 | 1 | +1.38 | 0.74 | 50.2 | +0.6 | +0.4 | +0.3 |
| DSC00338.ARW | Sony ILCE-7RM5 | 1 | +1.43 | 0.64 | 42.3 | +0.7 | +1.2 | +1.0 |
| DSC00363.ARW | Sony ILCE-7RM5 | 1 | +1.79 | 0.82 | 50.2 | -4.0 | +1.6 | +1.3 |
| DSC00388.ARW | Sony ILCE-7RM5 | 1 | +1.37 | 0.70 | 47.6 | -0.2 | -0.7 | +0.1 |
| DSC00413.ARW | Sony ILCE-7RM5 | 1 | +1.28 | 0.82 | 50.2 | +2.2 | +0.3 | +0.0 |
| DSC00438.ARW | Sony ILCE-7RM5 | 1 | +1.37 | 0.70 | 44.6 | +1.0 | +0.5 | +0.8 |
| DSC00463.ARW | Sony ILCE-7RM5 | 1 | +1.68 | 0.74 | 55.6 | -1.9 | +2.1 | +1.6 |
| DSC00488.ARW | Sony ILCE-7RM5 | 1 | +1.59 | 0.73 | 42.7 | -1.4 | +1.3 | +0.9 |
| DSC00513.ARW | Sony ILCE-7RM5 | 1 | +1.51 | 0.86 | 46.6 | +0.1 | +1.8 | +1.5 |
| DSC00538.ARW | Sony ILCE-7RM5 | 1 | +1.49 | 0.79 | 50.0 | -0.4 | +1.1 | +1.0 |
| DSC00563.ARW | Sony ILCE-7RM5 | 1 | +1.46 | 0.69 | 50.3 | +0.5 | +1.4 | +1.1 |
| DSC00588.ARW | Sony ILCE-7RM5 | 1 | +1.60 | 0.68 | 45.1 | -1.7 | +1.1 | +1.1 |
| DSC00613.ARW | Sony ILCE-7RM5 | 1 | +1.45 | 0.76 | 52.2 | -0.6 | +0.0 | +0.1 |
| DSC00638.ARW | Sony ILCE-7RM5 | 1 | +1.61 | 0.95 | 65.1 | -1.6 | +1.9 | +0.9 |

| maker | n | mean dL off | mean dL anchor | mean abs dL anchor | max abs dL anchor | mean dL anchor generic | max abs generic - anchor |
|---|---|---|---|---|---|---|---|
| Apple | 2 | -13.2 | -0.7 | 0.7 | 0.8 | -0.7 | 0.00 |
| Canon | 6 | -15.7 | +1.0 | 1.3 | 2.6 | +1.0 | 0.00 |
| Fujifilm | 6 | -17.1 | -0.8 | 1.9 | 3.3 | -0.8 | 0.00 |
| Google | 3 | -12.0 | +0.0 | 0.6 | 0.8 | +0.0 | 0.00 |
| Leica | 3 | -2.0 | +6.8 | 7.8 | 20.5 | +6.8 | 0.00 |
| Nikon | 3 | -4.2 | +6.8 | 6.8 | 9.6 | +6.8 | 0.00 |
| Olympus | 3 | -17.8 | +0.9 | 0.9 | 1.2 | +0.9 | 0.00 |
| Panasonic | 3 | -12.6 | +2.8 | 2.8 | 3.5 | +2.8 | 0.00 |
| Pentax | 4 | -2.9 | +3.4 | 3.6 | 11.3 | +3.4 | 0.00 |
| Samsung | 2 | -1.4 | +1.5 | 1.5 | 2.7 | +1.5 | 0.00 |
| Sony | 25 | +0.6 | +1.0 | 1.1 | 3.1 | +0.9 | 1.39 |
