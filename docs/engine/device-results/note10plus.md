# Samsung Galaxy Note10+ (SM-N9760) device results, 2026-11-12

## 2026-11-16 (print-table easing): phone measurement not done

The change (section 60b: the interactive coarse print table is eased, 26 of 71 steps on the exposure pad were table-generation-only) was verified on the desktop only (`engine.film_print_ease`). The Note10+ measurement was not run: a public sample pushed to a new directory does not appear in the app's photo grid without a media scan (the grid still listed 40 photos), and the other ways in (the system file picker, `cmd media`, `content query`) are outside the privacy rules of the task. The installed build is the previous one (15:10); the stock selected there was not checked either, so the N400C / N200B mix-up of the previous run is still open. Cleanup: the pushed file and directory removed (`ls` reports the directory absent), the debug property reset to empty, no screenshots kept.

## 2026-11-16 (film drag flicker: match easing)

Release APKs of the working tree before / after the fix (`install -r`), Note10+ RFCMA05Q5ZX, taps and swipes only (resumed-activity check before every input), scripts and logs in `build/perf-20261116i/` (`drag.sh`, `map.sh`, `an.py`, `abs.py`, `ft.py`). `debug.dapple.frame_profile` 2 for the traces (the view reads the picture back), 1 for the frame times; reset to empty. Public sample `T1116i_a.ARW` (A6700). Drags: 3 s `input swipe`, 5 per gesture. **Before** the stock was N400C (puck in the centre), **after** N200B (the app kept the earlier selection; same pad coordinates): the two sets are comparable in kind, not frame for frame.

Per-drag count of frame-to-frame jumps of the mean luma above 0.01 (absolute; 0..1) and the largest jump, excluding the first two frames of a drag:
| gesture | before (steps per drag; max jump; frames) | after |
|---|---|---|
| 成像 pad (exposure) | 14 18 18 19 20; 0.037; 712 | 4 4 1 3 2; 0.075 (cold first drag, first frames); 565 |
| 颜色 pad with film | 3 1 1 1 1; 0.175 (first drag); 349 | 5 1 0 1 0; 0.085 (first drag); 347 |
| film map (3 s swipe; stock changes included) | 6 6 3 3 3; 0.21; 44 | 8 22 3 3 3; 0.22; 135 |
Per-frame jump p90 on the exposure pad 0.0111 before, 0.0044 after. Match landings: every 20-25 ms (exposure pad), 200 ms (colour pad); after the fix the used EV changes each frame (eased), so the "landing interval" is no longer visible in the trace. The film map jumps are real stock changes (2 EV) and not eased when the stock has a remembered match; the larger counts after come from more frames logged (more repaints), not larger steps. Not enough reps to call the map unchanged or better.

Frame times, `frame_profile` 1, cpu_ms of interactive film frames (first / rest median, rest p90, max): exposure pad before 3.4 / 1.4, p90 9.3, max 47.6 (590 frames); after 4.1 / 1.7, p90 14.5, max 85.8 (586); colour pad before 34.5 / 20.4, p90 30.4, max 64.1; after 37.6 / 20.9, p90 29.3, max 66.9. The exposure pad rest p90 and max are higher after (extra repaint frames while the values converge); not investigated further.

Cleanup: `rm /sdcard/Pictures/DappleT1116i/T1116i_*`, `rmdir /sdcard/Pictures/DappleT1116i`, `ls` of that directory reports it absent; property reset (reads back empty); screenshots deleted (they showed only Dapple's UI); app force-stopped. The app library keeps its entry for the photo.

## 2026-11-16 (5a57516) film exports on the GPU stream

Release APK of HEAD 5a57516 (built 14:17, `install -r` 14:17), Note10+ RFCMA05Q5ZX, taps only (every input preceded by a resumed-activity check), scripts / logs / pulled exports in `build/perf-20261116h/`. State: **Battery Saver ON (`low_power` 1, unchanged)**, battery 100 % on USB, 28.8 C at start, 31.6 C at the end, `Thermal Status 0`, no reboot. `debug.dapple.frame_profile` set to 1, reset to empty. Files: public samples as `T1116h_{6700.ARW,6700b.ARW,7rm4.ARW,xs10.RAF,xt50.RAF}`, scanned with `MEDIA_SCANNER_SCAN_FILE`. **Film: N400C at 100 % for every run** (picked from the 成像 panel). No crash, ANR, `exec=cpu`, `reason=`, `FilmTiledLocal`, `vkAllocateMemory` or GPU error line in any log. Every film export ran as `export_stream GPU ... ok`.

**Method note:** an export of an unchanged edit state is served from the app's export cache (no stream line, file copied in about 1 s), so reps 2 and 3 of the first batch were cache hits and are not counted. Fresh reps used the small exposure swipe before the export (as in the earlier sections): **3 fresh reps for A6700, 2 for X-S10, X-T50 and A7RM4** (the time budget did not allow a third).

### Full-size JPEG export with N400C (tap to the `export_stream` line; RSS sampled every 0.25 s; HWM from /proc)
| | total ms (fresh reps) | stage split of rep 1 (ms) | MP demosaiced | RSS max / HWM MB |
|---|---|---|---|---|
| A6700 26 MP | 7253 / 7260 / 7259 | analysis 413 (demosaic 144), render 6681 = source windows 693 + upload 200 + film walks 2144 + record 419 + submit 93 + wait for previous tile 2846 + sink 234 + other 51; encoder finish 58, write 24 | 26.3 in analysis + 10.9 in 5 windows | 808-827 / 970-975 |
| A6700 + 局部 circle (亮 30) | 7696 / 7682 / 7744 | analysis 451, film walks 2425, wait 2940, record 436 | same | 809 / 967-969 |
| X-S10 26 MP | 8652 / 8776 | analysis 2348 (CPU X-Trans demosaic 2038), render 6178 = windows 624 + upload 186 + film walks 1996 + record 394 + submit 99 + wait 2599 + sink 238; finish 40, write 24 | 26.0 (analysis only, 0 windows) | 800 / 970-972 |
| X-T50 40 MP | 15082 / 14876 | analysis 3491 (demosaic 3028), render 11395 = windows 976 + upload 318 + film walks 3771 + record 626 + submit 146 + wait 5105 + sink 376; finish 38, write 71 | 40.1 (analysis only) | 1030 / 1055-1082 |
| A7RM4 61 MP | 18733 / 18945 | analysis 851 (demosaic 367), render 17616 = windows 1723 (demosaic 1042) + upload 640 + film walks 5575 + record 931 + submit 209 + wait 7940 + sink 535; finish 150, write 54 | 64.1 in analysis + **124.2 in 28 windows (4328 rows demosaiced twice or more, 2048 from the cache)** | 891-912 / 920-935 |

The stage timers add up to the total (render + encoder finish + file write + other); "GPU busy 0 ms" is logged in all (counter not filled). Before (from the brief, not re-measured): A6700 film + 局部 on the CPU stream 13.6 s, RSS 1.10 GB; now 7.7 s and 0.97 GB HWM. I have no in-app before numbers for X-S10, X-T50, A7RM4 with film. The largest stage everywhere is "wait for the previous tile" (about 35-45 % of the total), then the film walks (about 30 %).

### Correctness (A6700 6240x4168, X-S10 6246x4170, X-T50 7752x5178, A7RM4 9568x6376; one export per file pulled, orientation tag 1)
Looked at the whole picture plus a 900x600 100 % crop across a 640 multiple per file (A7RM4 on flat sky at x = 3840; A6700 also with the 局部 circle at x = 3200): grain continuous, no grid, no band, no colour step, no black or missing tile, bottom and right edges normal (last 4 columns / rows within 1-3 levels of the 8 before them, no pixel below 2). Mean absolute difference of adjacent columns at x = 640k against all other columns (grey): A6700 8.59 vs 8.51 (rows 8.40 vs 8.59), X-S10 7.64 vs 7.45 (7.28 vs 7.47), X-T50 9.35 vs 9.51 (8.80 vs 9.43), A7RM4 5.18 vs 4.98 (5.47 vs 4.98, scene edges). A phase scan (mean local ratio per position mod 640) shows no spike at phase 639 in any file; the best phase is at an arbitrary offset in each (ratio 1.07-1.19, over 8-14 samples). Flat A7RM4 sky strip: 5.18 at the seams, other phases 4.78-5.31. Mean colour step across a seam (16 px left against right): median 0.14 (A7RM4 sky), 0.24-1.3 elsewhere against 0.11-0.8 at an offset of 320. A6700 + 局部: 6.56 vs 6.28 in the circle band, the brightening continues over the tile borders. Not a pixel-for-pixel comparison with a CPU render.

### Determinism
A6700 with N400C and no other edit, exported in the first app session and again from a second copy of the file (`T1116h_6700b.ARW`, separate process, 7312 ms): the two JPEGs are byte identical, md5 `da5921f635e79363357c712a4e34bb25` both (20667845 bytes). An unchanged re-export of the same photo is served from the cache, so the second copy was used to force a second computation.

### No film, same build
A6700 whole frame (`export_jpeg`): 1.94 s tap to the log line (open 391, render 802, encode+write 591; RSS 851 MB, HWM 919 MB). A7RM4 61 MP stream: 4178 ms (analysis 813, render 3188, finish 149; 41.4 MP in 10 windows, no film walks; RSS 788 MB, HWM 929 MB), against 4.34-4.46 s at f78c661. No regression.

### Interactive drags with N400C selected (X-S10, 5 each, 700 ms swipe, interactive frames, exec=vulkan, film=on; ms first / rest median)
| drag | frame total | tables | upload | rungs |
|---|---|---|---|---|
| exposure | 30.2 [16.1-98.1] / 10.1 | 17.9 / 8.8 | 0.5 / 0.0 | 810 and 1080 |
| colour pad | 32.2 [28.5-82.0] / 14.2 | 30.3 / 12.7 | 0.4 / 0.0 | 810 and 1080 |

No first frame over 100 ms, waitMs 0. Earlier film colour pad (f78c661, 20 drags): 28.9 / 20.3, so no sign of a slowdown.

### Drag during an A7RM4 film export (exposure swipes back and forth for 18.6 s, starting 1.3 s after the tap)
Export: **33.0 s** against 18.7-18.9 s alone, still on the GPU stream; the CPU demosaic of the windows took 14.0 s against 1.04 s (source windows 14720 ms), turn 1182 ms, peak footprint 1281 MB (892 alone), encode 1129. Drag: 512 interactive frames, `exec=vulkan`, film on, rungs 810 (387) and 1080 (125); frame total median 15.9 / p90 34.9 / max 201 ms; tables median 14.4; **waitMs median 0.0, max 90.9**; view latency **median 28.9 / p90 58.9 / max 287.6 ms, 18 of 512 over 100 ms**; 8 frames over 100 ms in frame ms. Drag alone (X-S10 film, rest frames): view latency median 24-27 ms. The film walks do not hold the GPU for the drag, but the drag costs the export about 14 s (CPU demosaic contention, as in the earlier no-film section) and about 400 MB more memory.

### Slower or worse
Nothing slower than the CPU stream. X-S10 and X-T50 run the CPU X-Trans demosaic inside the analysis pass (2.0 s, 3.0 s) before the GPU render. A7RM4 film exports demosaic the windows twice over (124 MP for a 61 MP picture, 1.0 s CPU) because the film windows and the low-plane walks re-read rows.

### Not measured
Third fresh rep for X-S10, X-T50 and A7RM4; an in-app "before" for film exports other than the 13.6 s A6700 figure; film + local detail operation (`FilmTiledLocal` CPU path); GLES; other stocks; CPU frequency.

### Cleanup
`adb -s RFCMA05Q5ZX shell rm /sdcard/Pictures/DappleT1116h/T1116h_*`, `rmdir /sdcard/Pictures/DappleT1116h`, `rm /sdcard/Pictures/Dapple/T1116h_*`; the two permitted `ls` forms report both absent. Prop reset (empty). Screenshots deleted (only crops of the public samples remain in `build/perf-20261116h/`). The app library keeps its entries for the test photos (no `pm clear`).

## 2026-11-16 (f78c661) 61 MP export, lattice 53

Release APK of HEAD f78c661 (built 11:48, `install -r` 11:49), Note10+ RFCMA05Q5ZX, taps only (every input preceded by a resumed-activity check), scripts / logs in `build/perf-20261116g/`. State: **Battery Saver ON (`low_power` 1, unchanged)**, battery 100 % on USB, 26.3 C at start, 29.5 C at the end, `Thermal Status 0`, no reboot. `debug.dapple.frame_profile` set to 1, reset to empty. Files: public samples as `T1116g_{6700.ARW,7rm4.ARW,xs10.RAF,xt50.RAF}`, scanned with `MEDIA_SCANNER_SCAN_FILE`. No crash, ANR, `exec=cpu`, `reason=` or GPU error line in any log (12 + 2 exports, 60 + 20 drags, 12 opens).

### No-film full-size export (exposure swipe before the export as before; tap to the export log line; 3 reps; RSS sampled every 0.25 s)
| | before (63244fc / earlier) | after (f78c661) |
|---|---|---|
| A7RM4 61 MP (stream, CPU demosaic) | 6.04 - 6.12 s (analyse 0.83, render 0.82, encode 0.66) | **4.34 / 4.42 / 4.46 s to log, `export_stream` 4112 / 4068 / 4133 ms**; analyses 803-829 ms (demosaic 329-350), render pass 3856-3928 (source windows 524-551, upload 294-310, record 647-688, wait for previous tile 1029-1071, sink 417-426), encoder finish 175-188, file write 15-20, other 9-10; RSS max 808-812 MB, HWM 916-931 MB |
| A6700 26 MP | 2.08 s | 1.89 / 1.95 / 2.00 s (demosaic 210-224, render 783-800, encode+write 526-620); RSS 877-934 MB |
| X-S10 26 MP | 3.5-3.65 s | 3.48 / 3.49 / 3.61 s (demosaic 1960-2093 CPU, render 760-771, encode 341-372); RSS 867-881 MB |
| X-T50 40 MP | 5.8-6.1 s | 5.51 / 5.39 / 5.20 s (demosaic 2961-3201 CPU, render 1083-1141, encode 751-820); RSS 1111-1119 MB |
The stage timers add up to the total (render + encoder finish + file write + other = `export_stream` ms). Path: A7RM4 `stream`, whole-frame peak 1542 MB against budget 1536 MB; the other three whole-frame, X-Trans on the CPU node.
Correctness: A7RM4 9568x6376 and A6700 6240x4168 pulled and viewed (whole picture, 600 px centre crop of the A7RM4): orientation 1, colours plausible, no seam, band or missing row seen; no constant row or column, largest row-mean / column-mean step 0.9 / 0.7 levels (A7RM4), 1.5 / 1.5 (A6700, scene edges). md5: two further A7RM4 exports with no edit between them are identical (`0c4df305c0d2dc259ad26a58eb6d1ba6` both); the third swipe export has the same md5, the first two swipe exports differ (`91d6763a...`, `e9e38450...`: different swipe values). The two no-edit exports wrote no export log line (log window), the files exist.

### Colour pad and tone / exposure drags (A6700, 2.3 s pause, 700 ms swipe; ms, first / rest median; exec=vulkan)
| drag | before | after |
|---|---|---|
| colour pad, no film, 20 | 45.7 / 33.1, tables 41.8 / 28.9 (rung 1080) | **43.8 [28.6-129.6] / 30.0**, tables 38.5 / 25.9, upload 1.4 / 1.3, exec 0.4 / 0.5; rung 810; 1 first frame over 100 ms |
| colour pad, film, 20 | | 28.9 [20.7-107.0] / 20.3, tables 25.5 / 16.8, upload 0.3 / 0.0; rungs 810 and 1080 (a film picked from the panel, `film=on`) |
| exposure, no film, 10 | 15.4 / 8.9 | 14.5 / 8.9, tables 13.3 / 6.3 |
| tone, no film, 10 | 14.0 / 9.4 | 13.7 / 9.6, tables 10.7 / 6.7 |
The colour rung was 810 here, 1080 before, so that row is not like for like.

### Colour during a drag against after release (held touch via `input motionevent`, same pad position, screenshots of the photo area)
Last frame before the mid-drag screenshot was interactive (810x541), the settled frame (1080x721, full) came 2 s after release. Mean / p99 absolute difference per channel (R, G, B, 0-255): full resolution 8.5 / 9.4 / 7.5 and 52 / 58 / 57 (detail and edges: the rungs differ in size); low-passed to 1/8: mean 2.0 / 1.9 / 1.8, p99 10 / 9 / 11; mean colour of the area 112.7 / 116.1 / 91.8 during against 111.0 / 114.7 / 90.5 after. The picture does not jump in colour, a shift of about 1.5 levels in the mean. One run, screenshot timing about 1 s after the last move (the held touch makes it reliable; a swipe cannot be timed).

### Open to first frame (tap to first frame line; 3 reps)
A6700 700 / 744 / 803 ms (before 739), X-S10 1130 / 1110 / 1077 (1124), A7RM4 1050 / 1028 / 1043 (1016), X-T50 947 / 936 / 940 (no earlier number). No regression.

### Not measured
GLES, the first X-Trans export of a fresh install, thermal / frequency during exports, film drags at the 1080 rung only.

### Cleanup
`adb -s RFCMA05Q5ZX shell rm /sdcard/Pictures/DappleT1116g/T1116g_*`, `rmdir /sdcard/Pictures/DappleT1116g`, `rm /sdcard/Pictures/Dapple/T1116g_*`; the two permitted `ls` forms report both absent. Prop reset. Screenshots (Dapple UI only) deleted. The app library keeps its entries for the test photos (no delete action; no `pm clear`).

## 2026-11-16 (57) Smaller colour lattice for interactive frames that rebuild it (pipeline-architecture.md section 57)

Native `dapple_drag_bench a6700.ARW dapple.dfp 810 60 'pads colour'` (DAPPLE_GPU=1, interaction flag set as the app does, bake 57 requested, analysis on, tone strength 60), arm64 release build of the working tree, RFCMA05Q5ZX in `/data/local/tmp/lattice53` (removed afterwards), 3 runs alternating before / after with a 20 s pause. Before = `DAPPLE_INTERACTIVE_LATTICE=0` (57^3 as the app asks), after = the engine default 53^3, same binary. Tables ms and frame ms (wall of `render_frame`), median / p90 of the 59 frames after the first, per run:

| colour pad | tables median (p90) before | tables after | frame median (p90) before | frame after |
|---|---|---|---|---|
| no film | 28.8 (29.9) / 27.5 (28.8) / 27.9 (29.3) | **24.9 (26.7) / 25.5 (27.2) / 24.4 (26.3)** | 41.4 (44.2) / 40.1 (42.4) / 41.3 (43.7) | **38.2 (40.3) / 38.7 (40.8) / 37.9 (39.2)** |
| film (Portra 400, full strength) | 15.0 (20.0) / 15.2 (21.5) / 15.4 (22.0) | 14.8 (20.8) / 15.3 (21.8) / 16.0 (20.4) | 38.6 (44.8) / 39.2 (45.4) / 39.6 (44.6) | 38.8 (47.3) / 39.1 (44.7) / 39.8 (45.9) |
The gain is the 20 % of the lattice bake the size removes: about 3 ms of tables and 3 ms of frame (-11 % / -8 %) without film; with a full-strength film the colour lattice is a stub, so nothing changes (and nothing is lost). First frames (cold process) 610 - 650 ms without film in both. No frame over 100 ms. Not measured: the app, the 1080 rung, GLES, the settled frame on the phone (it is the same 65^3 bake as before).

## 2026-11-16 (56) In-app X-Trans node choice and drag start, measured and fixed (pipeline-architecture.md section 56)

Release APKs of the working tree on 63244fc+, `install -r` only, Note10+ RFCMA05Q5ZX, taps only; scripts, logs, `an_*.py` in `build/perf-20261116f/`. Builds: `before.apk` (diagnostics only), `a1`/`a2` (placement fix; a2 + piggyback fix), `a3` (small first keep-warm units), `a4` (no CPU waits for uploads), `a5` (+ film LUT size change asynchronous; final). Native binaries `build/sched-arm64` under `/data/local/tmp/sch` (removed). State: **Battery Saver ON (`low_power` 1, unchanged)**, battery 100 % on USB, 28.2-28.8 C, `Thermal Status 0`, no reboot. Props `debug.dapple.frame_profile`, `debug.dapple.diag_sched`, `debug.dapple.diag_upload` set during runs and reset to empty. Files: public samples as `T1116f_{6700.ARW,xs10.RAF,xt50.RAF}`, scanned with `MEDIA_SCANNER_SCAN_FILE`.

### Thread placement (`dapple_sched` / probe lines; nice, allowed-cpu mask)
| | app before | native | app after |
|---|---|---|---|
| verdict probe thread | **nice 10, mask f (cpus 0-3)**: the idle region warm-up thread (`dapple_thread_background`) | nice 0, mask df | before the probe nice 10 / f, **during it nice 0, mask 6f-ff** |
| verdict (frame) | GPU 332 vs **CPU 1024** ms -> GPU (half path GPU 113 vs CPU 238) | GPU 323 vs CPU 221 -> CPU | GPU 334 vs **CPU 224** -> **CPU** (half GPU 97 vs CPU 52) |
| frame pool workers | **nice 10** (inherited from a thumbnail thread), mask f0 | nice 0 | **nice 0**, mask f0 |
| background pool workers | nice -10 (inherited from a Flutter thread) | 0 | 0 |
| render thread | nice -10, mask f0 (30-70 while core control isolates cores) | | unchanged |
| fixed workload (256 chunks) | pool 27-55 ms, threads spawned from the frame thread 38-64 ms, single big core 9-13 ms per 32 chunks | spawned from the probe thread 26 ms | pool 27-36 ms; probe thread during the probe 25-26 ms |
cpusets: `top-app` 0-7, `foreground` 0-2,4-7, `background` 0-3; every app thread was in `/top-app`. The masks of pinned threads lacked 1-3 big cores at times (Qualcomm core control isolates idle cores: 30 / 50 / 6f). The native probe saw the same (df). One first measurement after the placement fix was refused because the mask then lacked cpus 4 and 7. That check was relaxed (section 56) and the next build cached its first probe.

### X-Trans export (exp.sh, one exposure swipe, tap to the `export_jpeg` line; 3 reps; no `gpu node` line = CPU node)
| | before (63244fc section) | after (a2) | after (a5) |
|---|---|---|---|
| X-S10 26 MP | 6.42-6.51 s, demosaic 4.91-4.96 s GPU | 3.51 / 3.67 / 3.55 s, demosaic 2.03-2.14 s **CPU** | **3.65 / 3.50 / 3.52 s**, demosaic 2.06-2.09 s CPU |
| X-T50 40 MP | 10.29-10.39 s, demosaic 7.56-7.62 s GPU | 5.71 / 5.77 / 5.86 s, demosaic 3.06-3.11 s CPU | **6.08 / 5.84 / 5.87 s**, demosaic 3.14-3.35 s CPU |
RSS max 871-879 MB (X-S10) and 1108-1119 MB (X-T50), as before. The a5 X-S10 and X-T50 exports were checked by eye, whole picture and a 512 px centre crop: no artefacts.

### First frame of a drag (A6700, drag.sh: 2.3 s pause, 700 ms swipe; ms, first frame median [min-max] / median of the other frames; view latency first / rest)
| drag | before (63244fc) | after |
|---|---|---|
| exposure, no film, 20 | 64.8 / 15.3; latency 79.9 / 29.1 | **15.4 [7.1-31.1] / 8.9**; tables 14.6 / 6.3, upload 0.3 / 0.3, exec 0.5 / 0.5; latency 25.8 / 22.3 (a4) |
| colour pad, no film, 20 | 131.1 / 49.5; tables 40.5 / 31.2 | **45.7 [35.4-61.6] / 33.1**; tables **41.8 / 28.9**, upload 1.5 / 1.3; latency 59.4 / 46.8 (a4, rung 1080) |
| tone, no film, 20 | 68.8 / 13.3 (5 drags) | **14.0 [7.4-31.7] / 9.4**; tables 12.8 / 6.9; latency 27.7 / 23.3 (a4) |
| exposure + film, 20 | 97.7 / 23.0 (10 drags) | **30.5 [16.4-111.1] / 14.0**; tables 26.4 / 13.1, upload 0.4 / 0.0; latency 39.7 / 27.7 (a5; one first frame over 100 ms) |
The parts of the first frame (`diag_upload`, 8 drags per build, exposure): before, `sub` was 26.8-31.1 ms. Recording took 0.1 ms and `vkQueueSubmit` 0.1 ms; the rest was the keep-warm piggyback's third unit waiting in `begin()` for the frame's own slot. The tone-table upload waited 20-44 ms for its fence. No resized-source rebuilds after the first drag of the photo (`rsz 0`) and no new pool images (`poolNew 0`). The keep-warm windows show the first 256 KB-4 MB unit at 37-545 MB/s after the pause, against `lowMBps` 4.5-5.7 GB/s. After the piggyback fix the first frame was 44.1 ms. With small first units it was 34.5 ms. Without the CPU upload wait it was 15.4 ms. Film: the first drag frame's `ensureFilm` took 28-38 ms (the print LUT changed size from 33^3 to 17^3, then the synchronous update and `waitAll`). With a5 the upload is 0.4 ms. The no-film rows were measured with a4. a5 only changes the film path. The rungs were 1080 (and 810 / 640), where 63244fc used 810: the preview size controller picks a larger rung now that frames are faster.
Colour-pad tables: 28.9 ms per drag frame at the 1080 rung, 31.2 ms before at 810. The thread placement fix did not change it (pool workload 27-36 ms vs 27-55 ms).

### Not measured / open
GLES in the app. The first X-Trans export of a fresh install without the region warm-up. The probe always ran from the warm-up here. A probe refused under load and measured again on the export was not observed in the app (it is the desktop test `demosaic.gpu_verdict`). The CPU table stage after a pause is still about 2x a later frame. That remains the gap in the first frame.

### Cleanup
`adb -s RFCMA05Q5ZX shell rm /sdcard/Pictures/DappleT1116f/T1116f_*`, `rmdir /sdcard/Pictures/DappleT1116f`, `rm /sdcard/Pictures/Dapple/T1116f_*`, `rm -r /data/local/tmp/sch`. The two permitted `ls` forms report both paths absent. Props reset (read back empty). Screenshots of Dapple's UI deleted after reading. One screenshot showed the phone dialer and was deleted at once. During an `install -r` the app was not in front and four taps landed in the dialer: it showed `++++`; no call was placed and nothing else was opened. The app's library still lists the test photos.

## 2026-11-16 (63244fc) X-Trans export, drag start, session growth

Release APK of HEAD 63244fc (built 05:25, `install -r` at 05:26), Note10+ RFCMA05Q5ZX, taps only, scripts / logs / `an_*.py` in `build/perf-20261116e/`. State: **Battery Saver ON (`low_power` 1, unchanged)**, battery 100 % on USB, 25.7 C at start, 30.6 C at the end, `Thermal Status 0`, all 8 cores online, no reboot, `debug.dapple.frame_profile` set to 1 and reset to empty. Files: public samples as `T1116e_{xs10.RAF,xt50.RAF,6700.ARW,7rm4.ARW}` (no-film export, one small exposure swipe before the export as in the earlier runs unless noted). No crash, ANR, `exec=cpu`, `reason=` or GPU error line in any log (13 exports, 45 + 10 drags, 2236 growth frames).

### Finding: the app exports X-Trans on the GPU node, not the CPU node
Every X-S10 and X-T50 export logged `gpu node WxH: 117 tiles of 512` (whole frame, one node). The cached in-app verdict (read at open, `dapple_demosaic verdict ... (cached)`): **frame GPU 341 ms vs CPU 1022 ms, region GPU 76 vs CPU 200** (half path: GPU 104 vs CPU 198, 33 vs 47). The native probe on the same phone gave CPU 213 ms vs GPU 326 ms (section 55 below). The in-app CPU probe is 4.8x slower than the native one; the verdict is written once and then reused, so the 2x faster CPU demosaic is not used in the app on this phone. Cause not tested (probe ran in the app during the first photo opens, Battery Saver on; I cannot delete the cached verdict, no `pm clear`). **The one-time probe of the "first X-Trans export after install" was not captured**: I opened X-Trans photos (grid slot identification) before logcat was cleared, so the verdict was already cached for every export below; the first X-S10 export (6.44 s) is therefore not a probe-including number.

### No-film full-size export (tap to the `export_jpeg` / `export_stream` log line, so ~0.1 s less than "done"; 3 reps each, VmRSS sampled every 0.25 s / VmHWM)
| | before (earlier sections) | after (63244fc) |
|---|---|---|
| X-S10 26 MP | 6.5 s (5.0 s GPU demosaic) | first 6.44; reps **6.42 / 6.47 / 6.51**; open 5.15-5.20 (demosaic 4.91-4.96 **GPU**), render 0.75-0.79, encode 0.33-0.37; RSS 863-870 MB, HWM 894-898 MB |
| X-T50 40 MP | not measured in-app | **10.29 / 10.35 / 10.39**; open 7.82-7.90 (demosaic 7.56-7.62 **GPU**), render 1.51-1.53, encode 0.78-0.82; RSS 1092-1156 MB, HWM 1181-1184 MB |
| A6700 26 MP | 2.08 s | **2.08 / 2.08 / 2.09**; open 0.57-0.60 (demosaic 0.39-0.40), render 0.79-0.82, encode 0.52-0.57; RSS 867-872, HWM 1073-1076 MB |
| A7RM4 61 MP (stream) | 6.1 s | **6.04 / 6.09 / 6.12**; analyse 0.83, render 0.82, encode 0.66; RSS 785-825 MB, HWM 868-869 MB |
Unchanged from before within noise (X-S10 6.5 s -> 6.4-6.5 s) because the GPU node is still used. md5 of X-S10 exports: two runs **without any edit are identical** (`afeab522c8be2cd718b9bdc6b9ff08d5` both). The four exports with the exposure swipe have four different md5 (`e711...`, `75e9...`, `0a08...`, `afea...` = the no-edit one; swipes land on different values, so they are different edits, not a nondeterminism test; mean abs difference between two of them 0.46 / 255, max 10, against the no-edit file 0.03 - 0.46).

### Correctness (X-S10 no-edit export, X-T50 swipe export, pulled to the scratch dir, 100 % crops 512x512 viewed at 2x)
Sizes 6246x4170 (X-S10) and 7752x5178 (X-T50), EXIF orientation 1, landscape, colours plausible (chart: neutral greys, saturated patches in order; X-T50: foliage, sky, red roofs). No maze / zipper pattern, no false colour on the chart's fine print or on the foliage; one faint green fringe on a dark/light chart edge (X-S10 crop 2, left edge), not seen on the X-T50 crop. Seams: gradient at rows / columns that are multiples of 512 vs all others 1.44 / 1.36 vs 1.14 / 1.12 on X-S10, but multiples of 8 / 16 / 128 give the same 1.45 (chart structure, not a tile seam); X-T50 6.92 / 7.00 vs 6.74 / 6.91. **Against the previous X-S10 export** (`build/perf-20261116c/ex_xs10.jpg`, GPU, an unknown exposure swipe): mean abs 0.23 - 0.43 / 255 over the whole frame depending on which of my exports (no-edit 0.43, max 9; centre 1024x1024 0.41); that includes the edit difference, so it says nothing about CPU vs GPU demosaic parity (both GPU).

### First frame of a drag (A6700 copy, 2.3 s pause before every drag, 700 ms swipe, rung 810x541, `exec=vulkan`; ms, first frame median [min-max] / median of the following frames)
| drag | total | tablesMs | uploadMs | execMs | waitMs | view latency first / rest |
|---|---|---|---|---|---|---|
| exposure, no film, 20 drags | 64.8 [11.6-116.9] / 15.3 | 12.8 / 6.9 | 20.4 [0.5-78] / 5.3 | 27.5 / 0.5 | 0 / 0 | 79.9 (max 120) / 29.1 |
| colour pad, no film, 20 drags | **131.1** [59.5-155.6] / 49.5 | 40.5 / 31.2 | 60.2 [12-75] / 14.6 | 30.9 / 2.9 | 0 / 0 | 144.3 (max 160) / 66.9 |
| tone (光线 pad vertical), no film, 5 drags | 68.8 [17.8-74.7] / 13.3 | 8.8 / 7.1 | 21.6 / 4.7 | 21.4 / 0.7 | 0 / 0 | 81.8 / 27.2 |
| exposure + film, 10 drags (rungs 810 and 640) | **97.7** [44-114] / 23.0 | 23.2 / 18.4 | 32.9 [10-68] / 0 | 36.7 / 3.7 | 0 / 0 | 113.4 (max 158) / 39.0 |
First frames over 100 ms: 1 of 20 (exposure), 19 of 20 (colour pad), 0 of 5 (tone), 5 of 10 (film exposure). The first frame is still 2 - 8x a later frame: the cost is `execMs` (27 - 37 ms against 0.5 - 3.7) and `uploadMs` (20 - 60 ms against 0 - 15), i.e. the GPU / upload side after the idle pause, not the table stage (tables 1.2 - 1.9x). Before (b95e771, film): first frame 105 - 158 ms, tables 35 - 61, upload 7 - 69; now film 97.7 median with tables 23. No-film first frames were not measured before.

### Session growth (film exposure drag then colour-pad drag alternating, 40 pairs, 1.1 s swipes, 2236 interactive frames, all `film=on exec=vulkan`; rungs 96x64 to 640x428)
| tablesMs median (p90 / max) | first 200 | middle 200 | last 200 |
|---|---|---|---|
| all frames | **17.9** (24.1 / 51.3) | **17.5** (24.2 / 45.9) | **18.1** (25.3 / 42.0) |
| exposure only (1099 frames) / colour only (1137) | 17.4 / 18.6 | 17.6 / 17.8 | 18.7 / 18.6 |
No growth (before: 12.5 -> 30 - 35 ms over a session), no frame with tables over 100 ms. The session had already seen ~55 drags before these frames.

### Tone drag
The 光线 pad is two-axis: horizontal = 暗 / 亮 (exposure, readout "亮 30"), vertical = 高 / 低 (the half-circle icons, contrast / tone). `input swipe 697 1500 697 1900 900` (x on the pad, vertical) drives it: 35 frame lines per drag, table above. The earlier "no frame lines" was therefore a missed pad position, not an `input swipe` limit (pad is about x 246-969, y 1368-1989 in a 1080x2280 screen, 光线 tab at 170, 2147).

### Not measured
CPU demosaic in the app (cannot force the node in a release app; the cached verdict is GPU); the probe-including first X-Trans export; tone drag with film; thermal / CPU frequency during the exports (status 0 and 30.6 C only); GLES.

### Cleanup
`adb -s RFCMA05Q5ZX shell rm /sdcard/Pictures/DappleT1116e/T1116e_*`, `rmdir /sdcard/Pictures/DappleT1116e`, `rm /sdcard/Pictures/Dapple/T1116e_*`; the two permitted `ls` forms then report both paths absent. Prop reset (`setprop debug.dapple.frame_profile ""`, reads back empty). Screenshots (Dapple UI and, once, the launcher) deleted. The app's library still lists the test photos (no delete action).

## 2026-11-16 (55) X-Trans on one node, first drag frame, bounded cache (native binaries, pipeline-architecture.md section 55)

Native binaries only, under `/data/local/tmp/xtp` (removed afterwards): `before` = HEAD b95e771, `after` = this change, arm64 release builds in `build/xtp-before-arm64` / `build/xtp-after-arm64`. Battery Saver ON (`low_power` 1, unchanged), no reboot, no app. Files: public samples `Fujifilm-X-S10-lossless-compressed.RAF` (26 MP), X-T50 `DSCF0118.RAF` (40 MP), A6700 `DSC00002.ARW` (drags), pack `build/film/dapple_new.dfp`.

### X-Trans export (`dapple_open_bench <raw> export`, editor session first, then the full open + export; ms)
| | X-S10 before | X-S10 after | X-T50 before | X-T50 after |
|---|---|---|---|---|
| default (verdict cached) | 3492 (open 2723 + render/encode 737), CPU/GPU row split | **2596 - 2600** (open 1853, render/encode 709 - 716), CPU node | 5350 (3885 + 1424) | **4147 - 4180** (2780 + 1319 - 1361) |
| CPU node forced | 4312 (3572 + 708) | 2600 (1854 + 716) | 6848 (5493 + 1316) | 4161 (2800 + 1321) |
| GPU node forced | 4483 (3739 + 712) | 4101 (3345 + 721) | 6636 (5427 + 1169) | 6196 (4823 + 1329) |
| first default export of a fresh cache (probe measured) | | 5329 (once per device + format + app version) | | |

Demosaic alone (`dapple_demosaic_eval --no-ref --gpu 512 --no-readback`, all cores, ms): X-S10 CPU 3570 -> **1843** (1750 - 1816 in other runs), GPU 2957 -> 2600 (tap-list variant 2257, not kept: 1.5x slower on the Adreno 710); X-T50 CPU 5428 -> **2714**, GPU 4549 -> 3449. GPU passes before (X-S10, `DAPPLE_GPU_PROFILE`): scale 36, green 74, chroma 1531, derivatives 609, selection 504; workgroups of 64 / 128 and tiles of 256 - 2048 did not help. Verdict (fresh probe, after): frame GPU 326 vs CPU 213 ms, region 81 vs 67: CPU. Before, the probe gave GPU 372 vs CPU 509 and a 37.5 % CPU row share here; the app's cached probe had given 12.5 % (896-row GPU nodes).
Pixels: CPU-node export JPEG md5 before = after for both files (`90d219b2...`, `10721bde...`), default after = CPU before, two default runs identical. `dapple_demosaic_tests gpu_parity gpu_tiles tiles threads` (Vulkan): X-Trans GPU vs CPU fp32 max abs 5.4e-07, RGBA16F mean dE2000 0.016 p99 0.061, tiles 0 differing values.

### First frame of a drag, table time over a session (`dapple_drag_bench a6700 pack 810 20 'pads exposure (film)'`, GPU, `DAPPLE_BENCH_SETTLE_EDGE=1080`: settled frame after each drag, pause, touch hint 80 ms + 60 ms, every drag on new values; new bench with the before / after library)
| | before | after |
|---|---|---|
| 10 drags, 2 s pauses: first frame median / max, other frames median | 50.0 / 77.5, 48.0 (first-frame tables 17 - 32 ms) | **35.7 / 48.4, 36.2** (tables 10 - 24) |
| 60 drags, 0.3 s pauses: first frame median / max, other frames median | 48.7 / 65.5, 37.2 | **37.7 / 49.5, 35.3** |
| same 1200 frames: table ms median per 12 drags | 14.7 14.9 16.3 16.6 16.9 | **13.8 14.6 14.6 13.7 13.8** |
The app's 105 - 158 ms first frames were not reproduced natively (no `uploadMs` spike: 0.6 - 1.8 ms here, 7 - 69 ms in the app log).

### Not measured
The app (no install); GLES; the X-Trans stage lines (logcat not read); thermal state not logged (no change in the run-to-run spread).

## 2026-11-16 (b95e771) stalls, spikes, rungs, export profile

Build: release APK of HEAD b95e771 (built 03:38, `install -r`), Note10+ RFCMA05Q5ZX, taps only, scripts / logs / analysis in `build/perf-20261116d/` (`xdrag.sh`, `fdrag.sh`, `rdrag.sh`, `exp1.sh`, `an*.py`; the timing split is read from the `frame_profile` line: tablesMs = tables + prep, uploadMs = upload, waitMs = turn, execMs = exec - prep - upload - turn). State: **Battery Saver ON (`low_power` 1, unchanged)**, battery 100 % on USB, 29.8 - 30.1 C, `Thermal Status 0`, no reboot, `debug.dapple.frame_profile` set to 1 and reset to empty. **CPU: all 8 cores online (`0-7`).** `scaling_max_freq` is readable only for cores 4-7: **2016000 kHz on all four, while `cpuinfo_max_freq` is 2419200 (4-6) / 2841600 (core 7)**, i.e. 83 % / 71 % of the hardware maximum (cause not tested; Battery Saver is the likely one, not proven). Cores 0-3: `scaling_max` not readable, `cpuinfo_max` 1785600, current 1.56-1.79 GHz. Current frequency during the exports: A7RM4 CPU phase (2 s in) cores 0-3 at 1.79 GHz, core 4 at 1.40-1.61 GHz, core 7 at 2.02 GHz (the cap); during the X-S10 GPU phase cores 4-6 sat at 710 MHz. Files: the three public samples as `T1116d_6700.ARW`, `T1116d_7rm4.ARW`, `T1116d_xs10.RAF`, scanned in with `MEDIA_SCANNER_SCAN_FILE` broadcasts (no media query).

### Before / after
| | before (earlier sections) | after (b95e771) |
|---|---|---|
| exposure drag during the 61 MP export, view latency med / p90 / max | 31.5 / 60 / 358 ms, one 315-358 ms frame in 4 of 5 runs | **25.0 / 38.0 / 112.5** (5 runs, 998 shown frames; per-run max 108 / 103 / 113 / 103 / 94); idle in the same session 27.1 / 38.5 / 99.0 (373 frames) |
| frames > 100 ms / > 300 ms (view latency) | 4 of 5 runs had one frame > 300 | 4 / 0 (one 102-113 ms frame in runs 1-4); engine frame (cpu_ms) max 84.6, none > 100 |
| `waitMs` (turn wait) under the export | not measured | 0.0 on every frame (191-203 per run) |
| film global drag frames with tablesMs > 100 | 18 of 521 (3.5 %), 0.3-1.5 s | **0 of 2129** (exposure 764 + 381, colour pad 614 + 370; 40 drags), max tablesMs 68.0, all `film=on exec=vulkan`, no `reason=` |
| first 5 frames of each new drag after a 2 s pause (200 frames) | the spikes were here | tablesMs median 19-34, max 68.0, none > 100; the first frame of each rep is 105-158 ms in total (tables 20-68 + exec 30-62 + upload 20-69), 31 such frames, the rest of the drag is normal |
| view latency over all 2294 film frames | | > 100 ms: 145, > 300: 0, max 265 |
| exposure rung (no film, X-S10 copy, 3 drags) | 810, then 640, 640 | **810x541 on all three** (284 frames) |
| colour pad rung (no film) | 640 | **810** on all three |
| film exposure / colour pad rung | 640 / 640 | **810 / 810** (A6700 copy) |

Rungs per drag (frame = engine cpu_ms median; split = tablesMs / execMs / uploadMs medians; waitMs max 0.0-0.1 everywhere; the rung never changed inside a drag; no-film on the X-S10 copy, film on the A6700 copy):
| drag | drags | rung | frame ms (per drag) | tablesMs / execMs / uploadMs |
|---|---|---|---|---|
| exposure, no film | 3 | 810x541 x3 | 8.8 / 14.8 / 13.8 | 6.2-8.2 / 0.3-0.4 / 1.0-5.8 |
| colour pad, no film | 3 | 810x541 x3 | 42.3 / 41.5 / 53.6 | 29.5-31.4 / 0.6-2.7 / 11-19 |
| 局部 circle, 光线 pad x, no film | 3 | 810x541 x3 | 9.2 / 9.8 / 10.1 | 0.3-0.4 / 0.4-0.7 / 7.8-8.4 |
| exposure + film | 3 | 810x541 x3 | 32.3 / 37.8 / 40.6 | 30.5-35.2 / 0.9-1.9 / 0 |
| colour pad + film | 3 | 810x541 x3 | 41.4 / 41.0 / 40.8 | 35.2-37.2 / 1.8-2.9 / 0 |

Film table time grew over the session on the same drag (exposure + film tablesMs median 12.5, then 25.0, then 30-35 ms in the three exposure runs; temperature flat at 30 C, status 0); not diagnosed.

### Export profile, no film, once per file (in-app, `T1116d_*`)
| stage | A6700 26 MP | X-S10 26 MP | A7RM4 61 MP |
|---|---|---|---|
| path and why | whole frame: peak 841 MB (GPU 370) vs budget 1536 MB (available x 0.5 - 96 = 3385, ceiling 1536, room 3579) | whole frame: 842 MB (GPU 370) vs 1536 (3373 / 1536 / 3579) | **stream**: whole-frame peak 1542 MB (GPU 436) vs 1536 (3346 / 1536 / 3511) |
| decode / full open | 547 ms (mosaic 30, demosaic 373 CPU, shadow chroma 23) | 5238 ms (mosaic 49, **demosaic + crop 5000**, chroma 50) | lazy open 115 ms |
| demosaic where | CPU | GPU nodes 4 x 6246x896 (26 tiles each), 1159-1166 ms each (submit/wait 1121-1130, window build 25-31, read-back hand-off 7-9); the CPU row share is not logged | CPU, 1441 ms over 23 windows (102.4 MP, row cache 224 MB) |
| analysis pass | in the open | in the open | 1 pass 1465 ms (demosaic 739) |
| render | 811 | 762 | 823 |
| encode + write | 567 | 349 | 659 |
| total (log) | tap to `export_jpeg` line 2.08 s | 6.5 s | `export_stream` 5887 ms (peak 824 MB); tap to line 6.1 s |

Readback and wait are not separate lines in the export logs.
Redmi (2026-11-16 native bench, after; not the same harness): A6700 FULL open 416 (demosaic 352), render 1112, encode 285, total 1848; X-S10 open 2356 (demosaic 2279), render 1072, encode 220, total 3663; A7RM4 analysis 1424, window demosaic 616, total 4853.
**What is slower here:** X-S10 demosaic 5000 vs 2279 ms (**2.2x**, +2.7 s of the gap; the four GPU nodes alone take 4.65 s, so the Adreno 640 rows are the bottleneck); A7RM4 window demosaic 1441 vs 616 ms (**2.3x**, +0.8 s; CPU, big cores capped at 2.02 GHz); encode 567 vs 285 ms (A6700, **2.0x**) and 349 vs 220 (X-S10, 1.6x). The analysis pass is the same (1465 vs 1424) and the A6700 demosaic is near equal (373 vs 352). Render is faster here (762 vs 1072, 811 vs 1112). So the 61 MP and X-S10 exports are limited by demosaic (CPU windows / GPU nodes) and the JPEG encode on slower cores, not by the path choice.
**Export with the drag** (5 runs, drag started 0.6 s after the tap): export_stream 10.5-10.8 s (alone 5.9), analysis pass 6.16-6.40 s (alone 1.47; demosaic 5.4 s vs 0.74), render 0.99 vs 0.82, encode 0.98 vs 0.66; a 4096 detail session of the same photo decoded at the same time (4192 ms). The stalls are gone but the export takes about 4.7 s longer under a drag: the cost moved from the frames to the export's CPU phase.

### Anything broken
No crash (crash buffer empty), no ANR line, no `exec=cpu` or `reason=` in any drag, rung or export log of this run, no `VkResult`, no GPU error. The interactive tier rose to 1080x720 in some frames of 4 of the 5 drag-under-export runs (not looked into).

### Not measured
Tone drag (a vertical swipe on the 光线 pad produced no frame lines in two attempts, so no number); no-film runs used the X-S10 photo and film runs the A6700 (film could not be removed again without many undo taps); 局部 pad without film only; CPU row share of the X-S10 demosaic (not logged); per-stage readback; the Redmi comparison is native bench vs in-app; `scaling_max_freq` of cores 0-3; one export per file, so no spread.

### Cleanup
`adb -s RFCMA05Q5ZX shell rm /sdcard/Pictures/DappleT1116d/T1116d_*`, `rmdir /sdcard/Pictures/DappleT1116d`, `rm /sdcard/Pictures/Dapple/T1116d_*`; the two permitted `ls` forms then report both paths absent. `debug.dapple.frame_profile` reset (reads back empty). Screenshots (Dapple UI only) deleted. The app's library still lists the test photos (no delete action); the app was force-stopped.

## 2026-11-16 open and export after the decode work

Build: release APK of HEAD (built 02:27, `install -r`), Note10+ RFCMA05Q5ZX, taps only, same method as the 2026-11-15 sections (logs and scripts in `build/perf-20261116c/`). State: **Battery Saver ON (`low_power` 1, unchanged)**, battery 100 % on USB, 25.4 C at start, 28.0 C at the end, `Thermal Status 0`, no reboot, `debug.dapple.frame_profile` set to 1 and reset to empty. Files: the three public samples under new names (`T6700_26mp.ARW`, `T7RM4_61mp.ARW`, `TXS10_26mp.RAF`) in a new folder, removed afterwards. Photos identified from the `session_open` names.

### Before / after (median [min-max])
| | before | after |
|---|---|---|
| open, tap to first engine frame, A6700 (5 reps) | 944 | **739** [676-774]; session_open 466 (mosaic 282, block reduction 108, anchor 79), starts 23 ms after the tap |
| open, X-S10 (5 reps) | 1304 | **1124** [1080-1137]; session_open 843 (unpack 526, mosaic 707 incl. unpack, reduction 75, anchor 58) |
| open, A7RM4 (5 reps) | 1574 | **1016** [992-1035]; session_open 753 (unpack 193, mosaic 373, reduction 241, anchor 132) |
| no-film export, tap to done, X-S10 (3 reps) | 6.9 s | **6.5 s** (6.57 / 6.48 / 6.54); render 0.78, encode+write 0.33-0.37 |
| no-film export, A7RM4 (3 reps) | 6.8 s | **6.1 s** (6.09 / 6.18 / 6.13); `export_stream` 5.79-5.92 s: analyse 0.83, render 0.82, encode 0.66 |
| no-film export, A6700 (3 reps) | not measured | 2.0 s (2.05 / 1.99 / 2.03); render 0.79-0.81, encode+write 0.51-0.54 |
| peak footprint (log) / peak sampled VmRSS / VmHWM, X-S10 | RSS 927-939 | 875 MB / 864-869 / 965-970 MB |
| same, A7RM4 | RSS 850-887 | 824-827 MB / 817-826 / 925-938 MB |
| same, A6700 | not measured | 933-934 MB / 877-878 / 961-981 MB |
| zoom to first detail frame, X-S10 / A7RM4 / A6700 (1 rep each) | 926 / 1028 / 911 ms | 910 / 990 / 970 ms (`exec=vulkan`, half-resolution session) |
| RSS after 5 opens, same photo mostly (A7RM4 x4, A6700, X-S10; grid order shifted), then grid | 696-800 MB, HWM 1005, grid 277 | 845 / 836 / 825 / 826 / 827 MB, HWM 1014, grid 275 |
| RSS after 5 opens, X-S10, A7RM4, A6700, X-S10, A7RM4 (not like for like) | n/a | 699 / 967 / 887 / 905 / 949 MB, HWM 1127, grid 281 |

Open is **unimodal**: all 15 opens within one cluster, spread 6-12 % per file (the Redmi bimodality did not appear). The time to first frame improved 19 % (A6700), 14 % (X-S10), 35 % (A7RM4); the 61 MP and X-S10 opens are limited by `unpack` (X-S10 526 ms) and block reduction. Memory after the five-open series is about 50-100 MB above the earlier 680-800 MB range; HWM is unchanged (1014 vs 1005 MB) in the same-photo series. The mixed series (two 61 MP opens plus detail sessions) peaks at 1127 MB. Not isolated whether the +50-100 MB is the pinned decode threads, the retained buffers or the different photo order.

### Export paths (from the log)
* A6700 and X-S10: **whole frame** (`whole-frame peak 842 MB (GPU 370) vs budget 1536 MB`), then `export_jpeg`. X-S10: GPU demosaic in 4 nodes of 6246x896 (26 tiles each, 1.16-1.17 s each, `submit/wait` 1.12 s), `full ...: demosaic + crop/orientation` 4.97-5.00 s. 4 x 896 = 3584 rows; the other 586 rows would be the CPU share, but **the log has no CPU/GPU row split line**, so that number is inferred, not logged. A6700: CPU demosaic 0.39 s (`full`: mosaic 24, demosaic 390, shadow chroma 24).
* A7RM4 61 MP: `whole-frame peak 1542 MB vs budget 1536 MB` -> **stream** (`lazy` session), `export_stream GPU ... ok (23 windows, 102.4 MP, demosaic 1427-1450 ms CPU)`, analyse 1 pass 1.43-1.47 s (demosaic 0.74 s), row cache 224 MB (2048 rows). Not a banded slide in the sense of per-tile GPU demosaic: Bayer demosaic is CPU, tonemap/colour on GPU.
* No fallback: no `exec=cpu reason=` in any export, open or zoom log (three `exec=cpu reason=no source store (host frame)` frames at app start are the known warm-up probes), no `VkResult`, no crash, no ANR, no fatal line in logcat.

### Correctness of the exports (viewed)
Pulled one export per file (`ex_xs10.jpg` 6246x4170, `ex_6700.jpg` 6240x4168, `ex_7rm4.jpg` 9568x6376; downscaled views `*_small.png`, crops `ex_xs10_crop_3584.png`, `ex_7rm4_crop.png`). Dimensions and orientation match the sources, last rows present (no blank or black bottom rows), no garbled tiles, no visible seams or colour shifts between bands; the colour chart's patches and text are clean at 100 % across the 3584-row boundary.
Seam numbers (`an_seam.py`, row means of RGB, adjacent-row difference summed over channels):
* X-S10: median 0.48, p99 6.5, max 17.4. At the candidate GPU/CPU boundary (row 3584) the adjacent-row difference is 0.20-0.52 over rows 3576-3592 (median row 0.48): **no step**. The 32-row-above vs 32-row-below mean step at 3584 is 26.7 against a median of 2.8 over the image and a 99th percentile of 109, i.e. image content (the chart's grey bar ends there), not a seam. Row texture (mean |dx| of luma): 1.22 in the last 586 rows vs 1.22 in the 586 before, so the CPU-share rows have the same sharpness as the GPU rows. The largest jumps (rows 158 and 3176-3178) are chart edges spread over 3 rows with 57-65 % same-sign columns, not full-width single-row steps.
* A6700 (max 4.7 at row 1643) and A7RM4 (max 2.6 at row 2403): no spikes at 896-row multiples; the biggest row-to-row jumps are content edges.
Not compared pixel for pixel with a CPU render; the exports carry the earlier 光线 edit of the test copies for A6700 (亮 30) and none for the others.

### Drag preview sizes (A6700 copy, no film, `film=off exec=vulkan` on all 400 frames, 3 reps each)
| drag | rung | engine cpu med / p90 / max | tables | view latency med |
|---|---|---|---|---|
| exposure (光线 pad x) | 810x541 for rep 1 (100 frames), 640x428 for reps 2 and 3 (170 frames) | 15.0 / 24.0 / 98.7 | 7.5 | 29.2 |
| colour pad (diagonal) | 640x428 (130 of 130) | 43.2 / 69.8 / 188 | 33.2 | 59.4 |
Colour pad holds at 640. Exposure holds at 810 only in the first drag, then **the rung stays at 640 for the next two** (the earlier run had 810 for all 297 frames). Frame times are higher than the earlier A6700 run (15.0 vs 9.6 ms median) on a photo that already had an exposure edit; I did not find which of the two causes the carry-over (not diagnosed).

### Not measured
Zoom and drag were single / 3 reps; no per-band seam analysis for the GPU/CPU boundary other than at rows 896 x n and 3584 (the true boundary is not logged); no film exports (not requested); tombstones not readable.

### Cleanup
Removed by exact name: `/sdcard/Pictures/DappleTest1116c/{T6700_26mp.ARW,T7RM4_61mp.ARW,TXS10_26mp.RAF}` (folder removed) and the nine exports `{T6700_26mp,T7RM4_61mp,TXS10_26mp}_dapple.jpg`, `... (1).jpg`, `... (2).jpg` in `/sdcard/Pictures/Dapple/`, MediaStore rows deleted per name; a query by those names finds nothing. The app library still lists the three test photos (no delete action). Screenshots (Dapple UI only) deleted from the scratch dir.

## 2026-11-16 (b) adaptive preview size from the pixel-dependent part (app only, Dart)

Build: release APK of the working tree (app change only, `install -r`), Note10+ RFCMA05Q5ZX, Battery Saver ON (unchanged), 100 %, 27.9 -> 29.5 C, 60 Hz. Same method / `an_drag.py` as the section below (logs and scripts in `build/perf-20261116b/`). Files: `S6700_26mp.ARW`, `S6700b_26mp.ARW` (public A6700 sample, new folder, removed). Local runs on the fresh copy, global runs on the other.

**Cause of the collapse.** `AdaptiveQuality.record(cycleMs)` compared the whole render time (tables included) with one refresh (16.7 ms) and stepped down after 2 frames over; the way back up predicted `cycleMs * pixelRatio^2 < 0.6 * budget` (10 ms), i.e. it scaled the table time with the pixels too, so a 12 ms table-heavy frame could never climb and a 7 ms 局部 frame could not either; the rung is also carried over from drag to drag (one controller per editor). Not vsync: `renderMs` is the native call only (the view's `present` is 0.4 ms).
**What the app receives per frame**: `renderMs` (whole `dapple_session_render_frame`, tables + exec + upload), `presentMs`, `latencyMs`. It does not separate tables from GPU time (the split is only in the `frame_profile` log line). The controller therefore models the split: pixel part = ms/Mpx x Mpx of the rung (prior: the warm-up `filmMsPerMpx`, else 12), overhead = smoothed frame time - pixel part. **Field that would make this exact:** `tablesMs` (or `execMs`) in the frame event next to `renderMs`.
**Rule.** allowance = budget - overhead, held within 0.5..1 x budget (overhead over budget: shrinking buys nothing, the pixel part may still use half a refresh); target = largest rung whose pixel part fits (display and thermal caps stay floors); down after 2 consecutive frames over budget only if the target is smaller; up after 12 frames whose target at 0.85 x allowance is larger; kept: rung fixed during a drag (a size step mid-drag is a visible jump), the move is applied at drag end straight to the target (was: one rung per drag).

| drag (A6700) | before cpu ms med / tables, rung | after cpu med / p90, tables, rung | frames > 16.7 ms (before -> after) |
|---|---|---|---|
| exposure | 12.2 / 6.9, 360 (143 of 279) | 9.6 / 22.2, 6.3, 810 (297 of 297) | 120/279 -> 105/297 |
| tone | 13.9 / 7.6, 360 | 11.8 / 24.3, 7.1, 810 | 124/289 -> 111/296 |
| colour pad | 43.7 / 34.2, 360 | 52.1 / 73.3, 34.6, 640 | 117/126 -> 114/124 |
| film exposure | 25.4 / 20.5, 360 | 22.8 / 35.0, 19.7, 640 | 132/148 -> 130/154 |
| film colour pad | 27.0 / 22.4, 360 | 26.1 / 54.5, 21.9, 640 (3 frames 96x64) | 124/129 -> 118/126 |
| 局部 pad (光线) | 7.4 / 0.1, 360 | 8.3 / 13.4, 0.0, 810 | 24/381 -> 24/368 |
| 局部 area (move circle) | 0.4 / 0.0, 810..360 (121 of 241 at 360) | 7.4 / 13.2, 0.1, 810 | 7/241 -> 12/244 |

Frame time vs the pixel-dependent part: tables are unchanged (6.3-7.1 / 34.6 / 19.7-21.9 vs 6.9-7.6 / 34.2 / 20.5-22.4), engine `exec` median 3.0 vs 4.1 (exposure), 3.3 vs 6.2 (tone), 12.7 vs 12.2 (colour), so the pixel part of the larger rung is small; the colour-pad median of 52 vs 44 is inside the earlier per-rep spread (41.8 / 47.0 / 56.0) with exec +0.5 ms; not isolated further. **局部 area**: 0.4 -> 7.4 ms is a real pixel-dependent cost (the old figure was mostly 360x240 frames; exec 0.3 -> 7.1 at 810), still under the 16.7 ms budget. Dropped / late frames are not logged by the app; the table counts frames whose engine `cpu_ms` is over one refresh (p90 and max did not get worse beyond noise, except max in the film runs: 452 / 409 ms spikes as in the earlier run, 339 / 1540 before).
**局部 at 360 (finding):** not vsync and not wall-clock latency. The rung carried over from the previous drag (a table-heavy drag drops it) and could not climb: the 0.4-7 ms frames at 360 predicted 7 x 1.78 = 12.5 ms > 10 ms. Same mechanism, fixed by the same rule; a fresh drag of cheap frames now returns to the target at its end.
Not verified: the pixel prior on other phones (it falls back to the film warm-up value, which over-states non-film pixel cost, so those phones may stay a rung lower than necessary); thermal rule only unit tested; no film-on 局部 runs this round.

## 2026-11-16 after the table work

Build: release APK of e26ef6b (built 00:32, `install -r`), Note10+ RFCMA05Q5ZX, taps only, same method as the 2026-11-15 sections (3 reps of a 1.3 s swipe out and back, interactive frames pooled; scripts, logs and `an_drag.py` in `build/perf-20261116a/`). State: **Battery Saver ON (`low_power` 1, unchanged)**, battery 100 % on USB, 25.4 C at start, 29.2 C at the end, `Thermal Status 0`, no reboot. Files: the three public samples under new names (`R6700_26mp.ARW`, `R7RM4_61mp.ARW`, `RXS10_26mp.RAF`, plus a second copy of the A6700 `R6700b_26mp.ARW` so the local no-film run starts from a fresh edit state), new throw-away folder, removed afterwards. The grid order changes with edit state, so photos were identified from the log (`session_open` names).

**No film is really no film:** every frame of the no-film runs logs `film=off` (965 global frames, 634 local frames) and every film run logs `film=on` (548 + 446). The no-film local run used a fresh second copy; film was selected afterwards on the same photo (circle kept). The global runs (exposure, tone, 冷暖, colour) were on one photo with film added after the no-film runs (edits kept). All frames `exec=vulkan`, no `reason=` line, no fallback.

### C1 Drag frames (A6700, interactive frames, engine `cpu_ms` median / p90 / max; tables = CPU table ms in it; [per-rep medians])
"before" = the 2026-11-15 values (bb1cf0f / 02495b4); no before exists for tone, 冷暖 and the no-film local runs.
| drag | film | before cpu / tables | after cpu med / p90 / max | after tables | tier settled | frames > 100 ms |
|---|---|---|---|---|---|---|
| exposure (光线 pad x) | off | 24.6 / 12.4 | **12.2** / 23.0 / 116 [11.1 10.5 17.5] | 6.9 | 810 -> 360x240 (143 of 279), 480 43, 640 42, 810 51 | 2 of 279 |
| tone (光线 pad y) | off | n/a | 13.9 / 25.6 / 76 | 7.6 | 360x240 (289 of 289) | 0 |
| 冷暖 (颜色 pad x only) | off | n/a | 13.2 / 29.8 / 159 | 8.1 | 360x240 | 1 of 246 |
| colour pad (diagonal) | off | 54.3 / 39.4 | **43.7** / 76.0 / 163 [41.8 47.0 56.0] | 34.2 | 360x240 (126 of 126) | 8 of 126 |
| exposure | on | 32.0 / 26.0 | **25.4** / 37.9 / 339 [24.9 25.6 26.1] | 20.5 | 360x240 (148 of 148) | 4 of 148 |
| tone | on | n/a | 27.3 / 48.8 / 397 | 21.6 | 360x240 | 2 of 120 |
| 冷暖 | on | n/a | 26.4 / 46.4 / 558 | 20.6 | 360x240 (one 96x64) | 5 of 124 |
| colour pad | on | 33.6 / 26.5 | **27.0** / 55.0 / 1540 [26.8 27.8 26.2] | 22.4 | 360x240 | 7 of 129 |
| 局部 area (move circle) | off | n/a | 0.4 / 6.9 / 93 | 0.0 | 810 35, 640 42, 480 43, 360 121 | 0 of 241 |
| 局部 pad (光线 of the circle) | off | n/a | 7.4 / 11.2 / 198 [7.5 7.4 7.2] | 0.1 | 360x240 (381 of 381) | 1 of 381 |
| 局部 area | on | 7.1 / 12.1 / 24 (A6700 film) | 7.6 / 12.4 / 112 [7.1 7.9 8.9] | 0.1 | 360x240 (159 of 159) | 1 of 159 |
| 局部 pad | on | 7.8 / 15.0 / 235 | 5.9 / 14.0 / 80 [7.0 5.4 6.4] | 0.1 | 360x240 (263 of 263) | 0 of 263 |

The table work shows up as expected on the Note10+, but less than on the Redmi: exposure tables 12.4 -> 6.9 ms (frame 24.6 -> 12.2), colour pad 39.4 -> 34.2 ms only (frame 54 -> 44), film tables 26-27 -> 20.5-22.4 ms (frame 32-34 -> 25-27). **The tier did not recover from 360x240** in any global drag (it also settles there for no-film exposure; the first drag starts at 810 and shrinks within the drag). The local runs show a one-off no-film start at 810 and then 360, and in the film 局部 runs the tier is 360x240 for all frames (the 2026-11-15 run had 480-810 for the film 局部 pad).
Spikes: film frames show isolated table times of 0.3 - 1.5 s (`pre` / `tone` stage inside `tables`, e.g. `tables 1518 pre 1515`, view render up to 2.2 s) at the start of a rep or after the idle pause; 18 frames over 100 ms in 521 film global frames (3.5 %), 11 in 940 no-film global. Not diagnosed (CPU frequency / background work after the idle gap are possibilities, not measured).

### C2 Drag while the 61 MP export runs (A7RM4, 光线 pad, 5 reps; view latency, ms)
Each rep: cold open, idle drag (same swipe), then export tap, drag 1.5 s later (inside the analyse / GPU-tile phase). Latency = `dapple_view` latency of frames shown in the drag window.
| | frames | median | p90 | max |
|---|---|---|---|---|
| idle drag | 488 | 24.8 | 36.0 | 153 |
| drag during the export | 376 | 31.5 | 60.2 | **358** |
Per rep (median / p90 / max): 33.8 / 61.1 / 325, 25.9 / 56.6 / 358, 31.4 / 72.8 / 276, 34.2 / 55.5 / 328, 26.1 / 63.7 / 315. Export 8.10-8.17 s (8148 8131 8098 8127 8166; idle 61 MP export before: 6.5-6.8), peak footprint 1165-1168 MB, peak sampled VmRSS 1118-1126 MB (VmHWM 1193-1197 MB).
**Frames over 300 ms: 4 of 5 reps, one each (325, 358, 328, 315 ms).** Each is one interactive frame with `upload` 275-346 ms inside `exec` (`cpu_ms 345.8 ... exec 333.7; upload 333.4`), about 0.3 s after the previous frame, in the GPU tile phase (the analyse finished 1 s earlier): the frame waited for a GPU turn held by an export tile. The log has no per-turn line, so the turn itself (which tile, how long) is not logged; the wait is the `upload` time. Context lines in `build/perf-20261116a/stall_context.txt`. No frame waited more than 358 ms (the earlier 740 / 1334 ms case did not reproduce). Median drag frame cost under export is about 7 ms worse than idle (view latency), p90 +24 ms.

### C3 Open and zoom (1 rep each)
| | open, tap to first engine frame (ms) | before | zoom, tap to first detail frame (ms) | before |
|---|---|---|---|---|
| X-S10 | 1215 | 1304 | 1069 | 926 |
| A6700 | 1180 | 944 | 1105 (this photo had film + edits on, frame cpu 443 ms) | 911 |
| A7RM4 | 1600 | 1574 | 933 | 1028 |
One rep each, within the earlier run-to-run spread except the A6700 (the opened photo carried film and colour edits from the drag session, so it is not comparable; not re-measured with a clean copy).

### C4 Correctness (viewed)
`s_nofilm_color.png` (no film after the colour drags: strongly cold, saturated, looks as an extreme colour edit should, no artefacts) and `s_film_color.png` (film selected, muted film rendering of the same edit, no artefacts); `s_local_circle.png` (circle mask overlay in the expected place). Not compared pixel for pixel.

### Not measured
Which turn held the device in the 300+ ms frames (no log line); per-frame GPU ms (log shows `gpu` as a frame counter only); a clean-copy A6700 open/zoom; whether the table spikes come from the table code or from the device state.

## 2026-11-15 (night) app-side open / zoom changes

Release APK with the app changes (same Note10+ RFCMA05Q5ZX, taps only, Battery Saver on, 3 cold reps per file, frame_profile prop set then reset; raw logs and scripts in `build/perf-20261115c/`). Files: `C6700_26mp.ARW`, `C7RM4_61mp.ARW`, `CXS10_26mp.RAF` (the public samples, new throw-away folder, removed).
* Open, ms since the tap, first engine frame (before -> after, median [reps]): X-S10 1400 -> 1304 [1304 1260 1330]; A6700 993 -> 944 [959 940 944]; A7RM4 1612 -> 1574 [1552 1604 1574]. The session open now starts 12-37 ms after the tap (was about 130 ms: the open waited for the placeholder and the page build; a quick `input tap` also never reaches the 40 ms finger-down warm-up).
* The "sharp" frame at +0.68 s is not a settle: it is the before/after picture (`{'original': true}`, 1600 px) that `_kick` renders 600 ms after the first frame once idle. The first frame is already the full 1600 px tier, nothing sharper is computed. Unchanged (it is needed for compare); the second-frame times above are 1973 / 2234 / 1622.
* Zoom (double tap 8.5 s after the open) to the first detail frame, ms: X-S10 926 [943 884 926], A7RM4 1028 [1009 1059 1028], A6700 911 [952 911 902]; before 2.0-2.4 s. The half-resolution zoom session is opened in the background 2.5 s after the first frame (device > 5 GB, MemAvailable >= 3 GB, picture > 3200 px, nothing in flight).
* Memory, five opens in a row (grid, open, back): VmRSS after each open 696 / 797 / 684 / 682 / 800 MB (before: 801 at open 1), VmHWM 1005 MB (before 931): +74 MB for the detail session. Grid after: 277 MB.
* Params JSON per frame: 10 us for a plain edit (desktop), 2.5 ms with two 3000-point brush strokes; with the cached stroke text 0.05 ms.

## 2026-11-15 (late) after the binding fix and GPU turns

Build: release APK of 02495b4 (built 22:57, `install -r`), same Note10+ (RFCMA05Q5ZX), same method and scripts as the baseline below (copies and logs in `build/perf-20261115b/`: `drag_*`, `zoom_a*`, `export_*`, `rss_*`, `dx.sh` / `idle.sh`, `an_drag.py`). State: **Battery Saver ON (`low_power` 1, unchanged)**, battery 100 % on USB, 25.4 C at start, 29.4 C at the end, `Thermal Status 0`, 60 Hz. Library: the same three public samples under new names (`Q6700_26mp.ARW`, `Q7RM4_61mp.ARW`, `QXS10_26mp.RAF`, a new throw-away folder). Film N160, plus a 局部 circle with a 光线 pad edit, on the A6700 file (fresh edit state). Drags: 3 reps (1.3 s out, 1.3 s back), pooled interactive frames; per-rep medians in brackets. Exports: 3 reps A6700, 2 reps each X-S10 and A7RM4 (device-time limit; asked for 3).

**Pipelines: `local_film_in` and `film_src` now create.** No `VkResult` / `-13` / `reason=` / `exec=cpu` line anywhere in the logcat of the whole session (1162 drag frames, all `exec=vulkan`, no crash, ANR or fatal line; tombstones directory not readable without root, so not checked).

### B1 Before / after (A6700, film on, interactive frames, engine `cpu_ms` median / p90 / max)
| class | before (bb1cf0f) | after (02495b4) | tier settled | exec |
|---|---|---|---|---|
| 局部 pad drag (光线 pad of the circle) | **175.9 / 354.5 / 462**, 36 of 37 on CPU | **7.8 / 15.0 / 235** [9.9 7.5 8.0]; view latency 20 ms med | 480x321 (168 of 296), 640 49, 810 45, 360 34 | vulkan 296 / 296 |
| 局部 circle move (area) | 1.7 / 3.3 / 102 (vulkan; this path never hit the missing pipeline) | 7.1 / 12.1 / 24 [5.7 6.5 7.3]; view latency 24 ms | 360x240 (125 of 125) | vulkan |
| film exposure-type pad (成像) | 1.3 / 9.1 / 166 | 1.2 / 11.7 / 598 [1.2 1.2 1.3] | 480 176, 640 133, 360 70 | vulkan |
| colour pad | 34.0 / 42.4 / 220 (film on) | 33.6 / 47.0 / 170 [34.1 33.5 31.9] | 360 (103 of 153) | vulkan; tables 26.5 ms of it |
| exposure (光线 pad) | 32.7 / 44.3 / 225 | 32.0 / 44.4 / 97 [32.3 30.7 32.3] | 360x240 (150 of 150) | vulkan; tables 26.0 ms |

The film frames with the local edit are now about 22 times cheaper (176 to 8 ms), the tier rises from 360x240 to 480-810. Colour and exposure are unchanged: the cost is still the CPU table bake on the frame thread (26 ms of 32-34).

### B2 Zoom, export
| | before | after |
|---|---|---|
| film + local, first detail frame (tap to frame, 3 reps) | 2.2 s, **CPU 797 ms** | 2.03 / 2.00 / 2.04 s; frame 1340x1443 `exec=vulkan`, cpu_ms 596 / 609 / 618 (match ~195, exec 384-400 incl. upload 70-72) |
| export A6700 film + local (3 reps) | stream CPU, 10.8 / 11.2 s, RSS 1069 | **still stream CPU**: 13.6 / 13.6 / 13.6 s tap to done (stream CPU 11.8-11.9 s, analyse 0.74-0.82, encode 0.89), peak RSS 1096 / 1096 / 1102 MB (HWM 1101-1103); no GPU reason logged |
| export X-S10 (2 reps, no film) | 6.3 / 6.9 s, 931 MB | 6.9 / 6.9 s (render 0.79, encode+write 0.37-0.39), RSS 927 / 939 (HWM 934-946) |
| export A7RM4 61 MP (2 reps, no film) | 6.7 / 6.7 s, 879 MB | 6.8 / 6.8 s (GPU stream, 203 windows, CPU demosaic 3.0-3.1 s), RSS 850 / 887 (HWM 885-890) |

The film export stays on the CPU stream by construction, not because of a failure: `core/src/capi/stream.cpp` only enters the GPU stream when `!film`. It is 2.2-2.6 s slower than the baseline (same photo, but film N160 instead of N400C and a different local edit; cause not measured). Film + local export is the largest remaining GPU gap.

### B3 Dragging during background work (A7RM4 61 MP, 光线 pad, 3 reps each, engine `cpu_ms`)
Editing is not blocked while the same photo exports (the bar shows "saving", the pad stays live), so the drag started 1.5 s after the export tap and ran 2.9 s inside the export (analyse and GPU tile phase).
| | frames | engine median / p90 / max | view latency median / p90 / max | export total |
|---|---|---|---|---|
| idle drag | 202 | 21.2 / 36.2 / 118 [20.6 23.0 21.4] | 36 / 51 / 158 ms | n/a |
| drag during the export | 181 | 21.0 / 40.2 / 292 [21.1 21.6 20.5] | 36.5 / 70 / 326 ms; present p90 12.8 ms (idle 0.6) | 8.15-8.22 s (idle export 6.47-6.49), CPU demosaic 4.5 s (idle 3.0), peak footprint 1246-1264 MB (idle 865-869) |

Median drag frame cost is unchanged by the export; the tail is worse (p90 +11 ms latency, one 175 and one 292 ms frame in 2 of 3 reps) and the export is about 1.7 s slower. In an earlier single try, started in the first second of the export, the frames after the drag waited 740 ms and 1334 ms (`upload` time inside `exec`, one frame each): the GPU turn was taken by the export at that moment; not reproduced in the 3 timed reps, so it is one observation. Not measured: dragging while thumbnails develop after an import, dragging while a region render is in flight (the zoom runs on a half-resolution session here, there is no region render to overlap).

### B4 Correctness (viewed)
Screenshots `s_film.png` (film N160, 光线 tab), `s_local.png` (film + 局部 circle with its mask overlay) and the pulled export `ex_6700_film_local.jpg` (6240x4168, film N160 + circle, downscaled view plus a 800x600 crop at 100 %, `ex_6700_small.png`, `ex_6700_crop.png`): no black or garbled tiles, no seams, colours as a film look (warm greens, blue sky), the circle edit smooth, grain visible at 100 %. Full-size pixels were not compared with the CPU render.

### B5 Cleanup
Removed from the phone by exact name: `/sdcard/Pictures/DappleTest1115b/{Q6700_26mp.ARW,Q7RM4_61mp.ARW,QXS10_26mp.RAF}` (folder removed) and the 11 exports the app wrote to `/sdcard/Pictures/Dapple/` (`Q6700_26mp_dapple.jpg`, `... (1)`, `... (2)`; `Q7RM4_61mp_dapple.jpg`, `... (1)` to `... (5)`; `QXS10_26mp_dapple.jpg`, `... (1)`); the older files in that folder untouched; MediaStore refreshed per file. `debug.dapple.frame_profile` reset to empty. The app's library still lists the three test photos (no delete action, `pm clear` forbidden). Other phones not touched. Screenshots only show Dapple's own screens.

---

## 2026-11-15 baseline after the planner rewrite

Build: release APK of HEAD bb1cf0f (`app/build/app/outputs/flutter-apk/app-release.apk`, installed with `install -r`), Note10+ SM-N9760, Android 12, Vulkan on Adreno 640 (`gpu probe: api=vulkan ... self-test cached, probe total 9-13 ms`). Method (taps only, the release APK has no debug hooks):
* Library: three public CC0 RAW samples (`build/rawsamples`: Sony ILCE-6700 ARW 6240x4168 = 26 MP Bayer, Sony ILCE-7RM4 ARW 9568x6376 = 61 MP Bayer, Fujifilm X-S10 lossless RAF 6246x4170 = 26 MP X-Trans) pushed to a throw-away folder, scanned in by MediaStore; `adb shell input tap/swipe` on Dapple's own screens only (grid thumb, tab bar, pad / slider / circle handle).
* Frame trace: `setprop debug.dapple.frame_profile 1` (one `dapple_engine frame ...` line per frame with `tables / exec / upload`, executor and typed `reason=`), `dapple_view` lines (render / present / latency of the shown frame), `flutter OPEN_TIMING` lines (ms since the tap) and the `session_open` / `dapple_demosaic` stage lines. Drags: 3 reps of a 1.3 s swipe out and 1.3 s back (about 40-140 interactive frames per rep, all frames pooled). Open: 5 cold-process reps (`force-stop`, start, tap) per file. RSS: `/proc/<pid>/status` VmRSS sampled every 0.25 s plus VmHWM.
* State: **Battery Saver ON (`low_power` 1, not changed)**, battery 100 % on USB, 25.5 C at start / 29.5 C battery, AP 33.1 C at the end, `Thermal Status 0` before and after, screen on, 60 Hz panel only. Raw logs `build/perf-20261115/` (`open_x*_*.log`, `drag_*_*.log`, `zoom_*.log`, `export_*.log`, `rss_*.txt`, `mem_seq.txt`, `cold*.log`, summaries `summary_note10_*.txt`, analysers `an_open.py`, `an_drag.py`).

### A1 Cold start
`am start -W` TotalTime 333 / 340 / 331 / 326 / 332 ms = `Displayed` +326..340 ms (median **332**). The grid with cached thumbnails is the first screen. Background after that: GPU warm-up "film pipelines + default stock" 617-666 ms (runs the first engine frames; three of them report `exec=cpu reason=no source store (host frame)`, these are warm-up probes, not user frames).

### A2 Open from the grid (ms since the tap, median of 5, min-max)
| file | first pixels (thumb) | first engine frame ("real_frame") | second full-tier frame ("sharp", settled) | session_open (decode to 2048) | of which unpack / mosaic / reduce / anchor | first frame cpu_ms (tables / exec incl. upload) |
|---|---|---|---|---|---|---|
| X-S10 RAF 26 MP X-Trans | 29 (23-48) | **1400** (1367-1434) | 2083 (2036-2113) | 1014 (981-1052) | 681 / 865 (mosaic incl. unpack) / 100 / 53 | 146 (22 / 124, upload 77) |
| A6700 ARW 26 MP Bayer | 24 (22-39) | **993** (965-999) | 1683 (1663-1688) | 577 (565-605) | 233 / 360 / 151 / 65 | 170 (39 / 131, upload 83) |
| A7RM4 ARW 61 MP Bayer | 31 (18-35) | **1612** (1600-1626) | 2292 (2283-2315) | 1207 (1187-1221) | 524 / 725 / 320 / 162 | 150 (38 / 112, upload 64) |

* Time split, 61 MP: 1612 = session_open 1207 (RAW decode + preview demosaic on CPU) + first frame 150 (`tables 38` CPU bake, `exec 112` incl. `upload 64`) + ~255 ms not in engine logs (Dart hand-off, swapchain creation `dapple_present`, inferred). Same ~245 ms remainder on the other two files.
* "Sharp" = the second full-tier 1600 px frame (88-98 ms, `tables 18-21`, upload 13) about 680-700 ms after the first one; the delay is a settle timer, not work (inferred). Region warm-up (256x256) after the picture: 61-77 ms for Bayer (CPU verdict), 1280 ms for X-Trans (GPU verdict, frame GPU 393 vs CPU 1990 ms).
* Every open also starts a neighbour prefetch (`session_open` of the next photo 3-7 s later, 61 MP: unpack 1.5 s, preview 3.1 s when it overlaps something else), visible as CPU load during a following drag / export.
* All first frames `exec=vulkan`, picture correct (screenshots viewed).

### A3 Drag frames (interactive tier, A6700, pooled frames; engine `cpu_ms` = frame-thread time, `tables` = CPU table bake on the frame thread, `exec` = record+submit(+wait), `upload` inside exec)
| drag | film | size the adaptive tier settled at | engine ms median / p90 / max | tables ms (med) | exec ms (med) | view render ms median / p90 | latency ms median | executor |
|---|---|---|---|---|---|---|---|---|
| exposure (光线 pad x) | off | 360x240 (171 of 206 frames), 480x321 (35); first drag started at 810x541 / 640x428 and shrank | 24.6 / 39.1 / 130.5 | 12.4 | 11.1 | 25.3 / 39.8 | 39 | vulkan 206 / 206 |
| colour pad (颜色, 冷暖 x 浓淡 y) | off | 360x240 (117 of 117) | **54.3 / 73.6 / 144.5** | **39.4** | 11.8 | 54.7 / 72.1 | 69 | vulkan |
| exposure (光线 pad x) | on (N400C) | 360x240 | 32.7 / 44.3 / 224.9 | 27.1 | 3.2 | 33.5 / 47.2 | 48 | vulkan |
| colour pad | on | 360x240 | 34.0 / 42.4 / 219.5 | 27.0 | 4.3 | 34.7 / 50.7 | 48 | vulkan |
| film strength (成像 slider) | on | 360x240 136, 480x321 204, 640x428 59 | **1.5 / 11.0 / 99** | 0.4 | 0.9 | 1.9 / 11.8 | 16 | vulkan |
| film exposure-type pad (成像 pad y) | on | 360x240 134, 480x321 209, 640x428 68 | **1.3 / 9.1 / 166** | 0.2 | 0.8 | 1.7 / 9.6 | 16 | vulkan; `film_tables stale 397 baked 421 requested 421` at the end (tables bake in the background and lag) |
| 局部 circle: move the area | on | 360x240 123, 480 28, 640 32 | 1.7 / 3.3 / 102 | 0.5 | 0.7 | 3.5 / 54 (max 773) | 19 | vulkan |
| 局部 circle: move the area | off (film removed afterwards did not take: see below) | 360x240 | 1.7 / 6.8 / 47 | 0.5 | 0.7 | 3.7 / 49 (max 1337) | 18 | vulkan |
| **局部 pad (光线) on the circle** | **on** | 360x240 | **175.9 / 354.5 / 462** (only 11-14 frames per 2.6 s) | 0.4 | 175 | 182 / 382 | 193 | **cpu 36 of 37**, reason below |

* **GPU to CPU fallback with typed reason (Adreno 640):** `exec=cpu reason=transient GPU failure: recording failed: vkCreateComputePipelines local_film_in|0=0|3=0|4=0|16=16|17=16|18=256|19=256 failed (VkResult -13)` on every frame of the 局部 adjustment drag when a film is selected. The zoom detail frame of the same photo (film + local edit, 1340x1443): `exec=cpu reason=transient GPU failure: ... vkCreateComputePipelines film_src|0=2|4=0|16=16|17=16|18=256|19=256 failed (VkResult -13)`, cpu_ms 797. The same two pipelines fail on the Redmi (Adreno 710) in `dapple_gpu_tests`; the pre-rewrite binary (`build/gles640-arm64`, 2026-11-14) passes the full suite on the same Redmi (see redmi-garnet.md). The failure repeats every frame ("transient"), nothing demotes the executor, so every frame pays pipeline-creation failure plus CPU render.
* The no-film 局部 pad drag was NOT measured: the film reset tap did not clear the film (the later runs still logged `film=on`), so the 局部 rows marked "off" are really film on. Not retried (device time).
* Table bake is the CPU cost of the pad and colour drags: colour pad 39 ms of 54 ms, exposure 12 ms of 25 ms (film on: 27 ms of 33 ms). `upload` 7-9 ms per frame without film.
* Film strength / film pad frames are 1-2 ms on the frame thread because they reuse the last baked film tables (`stale` count grows during the drag, `baked` follows `requested` within ~1 table); GPU time per film frame is not in the log (submit is asynchronous), view latency is 16 ms (one vsync).
* Zero-copy present works (`dapple_view`: `ext` = all but the first frame, present 0.4 ms median). The panel is 60 Hz, so 16 ms is the floor.

### A4 Zoom (double-tap on the picture, zoom 1.4, tap time from the device clock, +~0.1 s tap latency)
| file | tap to first detail frame | what happened |
|---|---|---|
| X-S10 RAF | 2.0 s (2 reps: 2.03 / 2.02) | half-resolution session (region 3874x4170 > region cap 3200x2400): `half-resolution reconstruction 1008-1011 ms` (X-Trans), frame 1339x1442 `cpu_ms 217-225 (tables 28, exec 189-197, upload 74-77)` |
| A7RM4 ARW 61 MP | 2.4 s | half-res session decode 886 ms (total 1403), frame 1339x1439 `cpu_ms 276 (tables 66, exec 211, upload 102)` |
| A6700 ARW 26 MP (carries film + local edit) | 2.2 s | "region skipped, the edit has local adjustments on a film frame", frame 1340x1443 on **CPU 797 ms** (the `film_src` failure above) |

### A5 Export full-size JPEG (tap on the download icon; time = tap to `export_*` line; RSS = sampled VmRSS peak, HWM in brackets)
| file | edit state | path | tap to done | stages | peak RSS |
|---|---|---|---|---|---|
| X-S10 RAF | exposure nudge | whole frame, GPU | 6.3 / 6.9 s | full demosaic 5.4-5.6 s (CPU), render 0.74-0.75, encode+write 0.34-0.38 | 931 MB (940) |
| A7RM4 61 MP | exposure nudge | streamed, GPU (203 windows) | 6.7 / 6.7 s | analyse 0.86, CPU row demosaic 3.06, render 0.84, encode 0.86 | 879 / 864 MB (882) |
| A6700 26 MP | film N400C + local circle (persisted from the drag tests) | streamed, **CPU** | 10.8 / 11.2 s | analyse 0.7, CPU demosaic 0.9, render 1.0, encode 1.0-0.84 (stream CPU 9.2-9.7 s) | 1069 / 1047 MB (1076) |

* An export of a photo that was not edited since its last export returns at once with the earlier file (no engine lines; the icon turns green): measured by accident, not a render.
* The A6700 export used the CPU stream because the photo carries film + local edits (`whole-frame peak 2751 MB (film, GPU 915) vs budget 1536 MB -> stream`; the log does not give a reason for choosing the CPU stream over the GPU stream, inferred: the same film/local GPU pipeline failure). A plain A6700 edit export was not measured.
* All three outputs were written to the app's gallery folder and deleted afterwards (cleanup below). Outputs not viewed.

### A6 Resident memory (VmRSS, MB)
Grid after start 283 | after photo 1 (X-S10) 801 | 2 (7RM4) 682 | 3 610 | 4 606 | 5 598 | **after back to the grid 282** (`memory_trim 1: released 352 MB`; `dumpsys meminfo` at the end: TOTAL PSS 205 MB, RSS 326 MB, Graphics 70 MB, Native 68 MB). VmHWM 931 MB for the whole sequence. Leaving the editor writes a 2048 px JPEG (`export_jpeg 2048x1365 render 119 / 265 ms, encode 36-49 ms`) and trims within 0.15-0.27 s.

### Findings in short (6 largest costs, details in the chat report)
1. `film_src` and `local_film_in` Vulkan pipelines fail to create on Adreno 640 and 710 (VkResult -13): film + local drags 176 ms/frame on CPU, film zoom 797 ms, export on CPU 11 s. Regression against the 2026-11-14 build.
2. CPU table bakes on the frame thread: 39 of 54 ms (colour pad), 12-27 ms of every exposure frame.
3. Open to first frame 1.0-1.6 s: CPU decode + preview demosaic (session_open 0.58-1.2 s) plus ~250 ms outside the engine.
4. Zoom to first detail 2.0-2.4 s: half-resolution session rebuilt from the RAW (X-Trans 1.0 s reconstruction), no region path above the 3200x2400 cap.
5. Export 6.3-11 s: whole-frame X-Trans demosaic 5.4 s on CPU, 61 MP row demosaic 3.1 s on CPU.
6. Adaptive tier collapses to 360x240 in every non-film drag (frame cost above budget); the first frame of the first exposure drag took cpu_ms 99 (view render 133, upload 67), one run only.

### Not measured / cleanup
Not measured: the no-film 局部 pad drag (see A3), a plain no-edit A6700 export, per-pass GPU time in the app (not in the frame log), a film-strength drag GPU time, GLES, Redmi in-app (cannot install). Cleanup: removed by exact path from the phone `/sdcard/Pictures/DappleTest1115/{P6700_26mp.ARW,P7RM4_61mp.ARW,PXS10_26mp.RAF}` (and the folder, MediaStore rows deleted) and the 8 exports the app wrote to `/sdcard/Pictures/Dapple/`: `P6700_26mp_dapple.jpg`, `P6700_26mp_dapple (1).jpg`, `P7RM4_61mp_dapple.jpg`, `P7RM4_61mp_dapple (1).jpg`, `PXS10_26mp_dapple.jpg`, `... (1).jpg`, `... (2).jpg`, `... (3).jpg` (files older than today in that folder were not touched); `debug.dapple.frame_profile` reset to empty; no files were pushed to `/data/local/tmp`. The app stays installed (updated with `install -r`); its library still lists the three test photos (rows pointing at deleted files) because there is no delete action and `pm clear` is forbidden. Screenshots (`s_*.png`) only show Dapple's own screens.

---

Borrowed owner phone, temporary permission. Same tools and methodology as `pixel5.md` (`build/pixel5-arm64-gpu` CLI + `dapple_gpu_tests`, `build/gles-android-arm64/dapple_gles_check`, `build/pixel5/main10/m10`, sustain script `build/note10/sustain.sh`). Raw logs: `build/note10/logs/`, screenshots `build/note10/s*.png`. Fixtures: synthetic only. Find N5 numbers come from `find-n5.md` / `build/n5/logs/`.

**Caveat for every timing: Android Battery Saver was ON (`settings get global low_power` = 1, "Battery Saver is currently: ON", sticky), battery 40-42 % on USB.** Settings may not be changed in this session, so all timings are "battery saver on" and they are bimodal (4.1b). Re-run with Battery Saver off before final conclusions.

## 1. Device facts

| | Note10+ | Pixel 5 | Find N5 |
|---|---|---|---|
| OS | Android 12, One UI 4.1 (`oneui=40100`), SP1A.210812.016.N9760ZCU5HVF2, patch 2022-04-01 | Android 14 | Android 17, ColorOS 17 |
| SoC | SM8150 (Snapdragon 855), board msmnile | SM7250 | SM8850 (8 Elite Gen 5) |
| CPU | 4x A55 (1.79 GHz) + 3x A76 (2.42) + 1x A76 prime (2.84), schedutil | 6x A55 + 2x A76 | 7 Oryon-class cores |
| RAM | 11.6 GB (7.7 avail), 4 GB swap | 7.6 GB | 15.5 GB |
| GPU | Adreno 640, Vulkan 1.1.128, driver 0x801f6000, deviceKey `5143:6040001:801f6000` | Adreno 620, 0x801ea000 | Adreno 840, Vulkan 1.4.295 |
| GLES | ES 3.2 V@0502.0 (Date 10/01/21), AEP, `ro.opengles.version=196610` | 3.2 | 3.2 |
| GPU limits | maxImage2D 16384, 3D 2048, shared 32 KB, subgroup 64 (0x3f), fp16 arith 1, **storage16 0**, linear RGBA32F 0, timestamps 1 | storage16 1 | storage16 1, linear RGBA32F 1 |
| Memory | one 11361 MB device-local heap; type 1 HOST_VISIBLE+CACHED, 2 +COHERENT+CACHED, 4 HOST_VISIBLE+COHERENT, 5 = 256 MB heap (same layout as Pixel 5) | same | several cached+coherent types |
| Display | 1440x3040 physical (override 1080x2280), 60 Hz only, HDR types 2,3,4 | | 120 Hz |
| Thermal | status 0 before/after every run; AP 35-37 C, BAT 30-37 C | status 0 | status 0 |

## 2. Vulkan correctness

* `dapple_gpu_tests`: **PASSED (0 failures)**. Self-test `point+geometry mean dE 0.040 p99 0.75 max 1.46 ok; spatial 0.076 / 0.86 / 1.45 ok`, identical to Pixel 5 / desktop. Per-stage tables, tile seams (tile 128..2048, 0/255 differing), steady state (pool 1023.8 MB, 47.6 ms frame), C API hook and blocklist all pass. **No driver bug found on Adreno 640** (the Pixel 5 shared-array bug is worked around and does not reappear; fp16 rounding fine). GPU vs CPU baked at 1600 px: point 0.0134/0.368/1.44, interactive 0.0307/0.456/1.25, full 0.0353/0.472/1.53; 24 MP tiled 0.0357/0.474/1.63 (same digits as Pixel 5).
* Difference: `storage16 = 0` (no 16-bit storage) on this driver; the engine passes anyway. Keep it in the persisted capability key.
* No kgsl hang at 24 MP (submit splitting works); 60 MP export fine.

## 3. GLES 3.1 backend

* `dapple_gles_check` (arm64, NDK r28, new dir `build/gles-android-arm64`, configured like the x64 one; built cleanly, 45 steps): "Adreno (TM) 640" GLES 3.2, key `5143:d5ef:0b92d5ef`, **self-test OK in 21.3 ms**, numeric case 0 of 3108 values off, buffer copy chain ok; EGL + setup ~780-830 ms. Reports: subgroup 0, fp16 arith 0, timestamps 0, linear RGBA32F 1.
* Selection: `--auto` tries Vulkan first (not linked into this tool: "Vulkan not linked into this tool"), then GLES31 succeeds. The tool always reports `forced: gles` and ignores `DAPPLE_GPU_API=vulkan|cpu`, so the full Vulkan -> GLES -> CPU order could not be observed. The shipped `libdapple.so` / `dapple_gpu_tests` do not contain the new HAL, so **GLES parity vs the CPU reference and a forced-gles engine run could not be run** (`DAPPLE_GPU_API=gles dapple_gpu_tests` just reruns the Vulkan suite).

## 4. Timings (battery saver ON)

### 4.1 1600 px (1.7 MP), source resident: GPU wall ms (GPU time)

| variant | CPU baked: Note10+ / P5 / N5 | Note10+ GPU wall (GPU / copy-out) | Pixel 5 | Find N5 |
|---|---|---|---|---|
| point-wise | 33-40 / 26.6 / 15.5 | **15.4-18** (3.5 / 9-12) | 19.4 (8.1) | 3-4.6 (0.4) |
| interactive | 87 / 101.7 / 39.3 | best 36-42 (20-25 / 13); typical 62-69 (47 / 19); worst 182 (47.8 / 127) | 58.0 (45.3) | 5.7-8 (3.3) |
| full quality | 139 / 154.0 / 68.1 | 48 (33 / 12.5); worst 110-160 (33-75 / 73-76) | 86.2 (72.7) | 10.8 (5.9) |

First frame: point 54-59, interactive 78-85, full 97-105 ms (Pixel 5: 62 / 117 / 141). CPU lattice bake 41^3: 21-25 ms. Peak RSS 451 MB.

### 4.1b Bimodal behaviour (retest without Battery Saver)
Same binary, same graph, screen on or off, different numbers between runs:
* Interactive GPU time 20.5 / 24.8 ms (best) vs 47-48 ms; full 33 ms vs 46.6 / 74.8 ms. In the slow state interactive costs as much as full, so it is not work-dependent: GPU/CPU frequency state (ramp after the short point variant, battery-saver caps).
* Copy-out of the same 6.8 MB: 10-19 ms normally, but 73-128 ms in some runs (6-10x). The engine's own pin to big cores did not prevent it. `taskset f0` / `70` were mostly good, `taskset 0f` (little cores) always bad (~120 ms), unpinned (`DAPPLE_GPU_NO_PIN=1`) 76-83 ms. The readback copy is more sensitive to core and clock placement than on the Pixel 5. Recommendation stays: avoid readback (display path).
* 150-frame run (42 s): interactive 182 ms (GPU 47.8, copy-out 127), full 63 ms.

### 4.2 24 MP (6000x4000), tile 1024

| | Note10+ | Pixel 5 | Find N5 |
|---|---|---|---|
| point-wise GPU wall / CPU | 187 (GPU 60, copy-out 118) / 447 ms | 239 / 355 | 41.6-44.9 / 209 |
| interactive GPU / CPU | 601 (GPU 483) / 1546 | 1042 / 1670 | 115-119 / 835 |
| full GPU / CPU | **831** (GPU 710) / 2096 | 1499 / 2350 | 162 |
| tiled export 24 MP, streamed / resident | tile 1024: 1.80 s (13.4 MP/s) / 1.61 s (14.9), pool 951 MB; tile 2048: 1.56 / 1.37 s (15.4 / 17.5 MP/s), pool 460 MB | (2048) 2.82 / 2.69 s | 0.64 / 0.32 s |
| 60 MP tiled, tile 1024, streamed | **5.56 s** (10.8 MP/s), pool 310 MB, RSS 930 MB | not measured | 1.1 s |
| preview 1600 from resident 24 MP | first 199, later 132 ms | 302 / 59 | |
| bench RSS (24 MP with CPU refs) | 2.0 GB, 36.7 s wall | 1.99 GB, 44.8 s | 2.0 GB |

Tiled GPU vs CPU full frame: mean 0.0357 / p99 0.474 / max 1.63.

### 4.3 CPU baselines (`edit-bench --scenario 1 --quick`, `film-render-bench --pack dapple_new.dfp --runs 3 --mp 2`)

| ms | Note10+ | Pixel 5 | Find N5 |
|---|---|---|---|
| interactive 1080 exposure drag | 33.3 | 23.7 | 16.4 |
| interactive 1080 saturation drag | 48.9 | 32.6 | 20.4 |
| interactive 1080 clarity drag | 28.0 | 27.1 | |
| interactive 1600 exposure drag | 61.6 | 36.5 | 30.1 |
| final 1600 unchanged / after bake 65^3 | 41.3 / 66.3 | 28.6 / 38.5 | |
| reference exact 1600 | 159.8 | 112.5 | 94.2 |
| bake 41^3 / 49^3 / 65^3 | 14.1 / 17.3 / 25.6 | 20.2 / 12.9 / 18.8 | 9.7 |

| film-render-bench (Portra 400) | normal | film cold | film cached |
|---|---|---|---|
| "26 MP" no spatial | 63.7 | 75.3 | 61.5 |
| "26 MP" all effects | 117.1 | 291.1 | 276.9 |
| 1600 preview no spatial | 77.5 | 84.4 | 72.5 |
| 1600 preview all effects | 117.7 | 270.0 | 254.2 |
| 1600 preview interactive | 117.2 | 199.6 | 187.8 |
| print-warmth drag frame | **228.9** (Pixel 5 243.5, N5 123.3) | | |

CPU is about 1.4-1.7x slower than the Pixel 5 in the edit-bench items (A55x4 + A76x4 plus battery saver), faster only in bake and the print-warmth drag.

### 4.4 Sustained run
A 600-frame chunk takes more than 2 min in the slow state, so one-variant (full quality) load was run for ~4.6 min (2 chunks, 13:20-13:24), then stopped on purpose: frame 152.3 and 152.4 ms (GPU 74.6, copy-out 74.5), flat, **no throttling trend**, `Thermal Status` 0, AP 35.2 -> 35.8 C, BAT 35.6 -> 37.1 C. This is the slow state (GPU 74.6 vs 33 ms best case), likely battery saver, not heat.

## 5. Codecs

* `dumpsys media.player`: HEVC encoders `OMX.qcom.video.encoder.hevc` (HW, Main + **Main10HDR10** advertised, no P010 `0x36` in its colour list: Surface, Flexible, SemiPlanar, Qualcomm 0x7fa30c0x; max 4096x2304) and `c2.android.hevc.encoder` (sw, Main/MainStill, max 512x512). AVC: `OMX.qcom.video.encoder.avc` (HW, up to High 5.2), `c2.android.avc.encoder`, `OMX.google.h264.encoder`. Decoders: `OMX.qcom.video.decoder.hevc` lists Main, **Main10**, Main10HDR10 (8192x8192), `...avc` Baseline-High.
* `m10` (Main10 P010, buffer mode): `configure` returns -10000 for the HW encoder with profile 2, 0x1000 and also the Main control (tool hard-codes colour-format 54, not listed). logcat: "Unsupported eColorFormat", "Reset to OMX_VIDEO_HEVCProfileMain10HDR10 profile instead of HEVCProfileMain10", "Failed to update the profile_level", "cannot encode HDR static metadata. Ignoring". Surface mode (profile 2 / 1 / 0x1000, 8 s watchdog): no output, including the Main control, so the tool's surface path is inconclusive here. Software encoder: no P010/Main10. **No Main10 stream could be produced, so the decode-back luma level count was not measurable.** Same conclusion as the Pixel 5: Main10 encode unavailable, app falls back to 8-bit.
* App (设置 -> 关于 -> long-press 版本 -> 编解码器 -> 全部重测; `s03`..`s05_*.png`, all viewed):
  * Self-tests: `enc8avc OMX.qcom.video.encoder.avc` PASS 229 ms (8 frames, decoded by OMX.qcom.video.decoder.avc, 220 distinct luma levels); `enc8hevc OMX.qcom.video.encoder.hevc` PASS 429 ms (221 levels); `enc8avc c2.android.avc.encoder` PASS 259 ms (219 levels).
  * Ladders `encodeLadder1080p30`, `...Hdr`, `...8Bit`: 1. HEVC Main 8-bit (OMX.qcom), 2. H.264 High HW, 3. H.264 sw. No 10-bit rung (correct).
  * `decodeLadderHevc10`: **software only** although the HW decoder advertises Main10, so HW Main10 decode is not used (worth checking why); `decodeLadderHevc8`: HW then sw; `decodeLadderAvc8`: HW then sw; `decodeLadderAvc422`: software.
* Motion Photo: not generated through the app (the in-app video picker would show the owner's library, which is not allowed). Instead the CLI `dapple motion` on the device with a synthetic cover: `gradient_hevc_main10_MP.jpg` (VALID, hvc1 640x352, 3 frames) and `sdr_h264_422_10_MP.jpg` (VALID, avc1 64x48, 15 frames); outputs in `build/note10/out/`. ffprobe (desktop ffmpeg 9.0.2) of the extracted clips: `hevc Main 10 yuv420p10le bt709` and `h264 High 4:2:2 Intra yuv422p10le bt709`. The CLI only trims/copies, so these are the source formats; **the app's 10-to-8-bit transcode path was not exercised on this device**.

## 6. Recommendations (older Adreno 6xx flagship)

* GPU is correct (no new driver bug); enable it after the startup self-test. Speed-up vs CPU: point 2.0x, interactive 1.3-2.5x (0.5x in the worst readback state), full 2.9x at 1600 px; 24 MP 2.5-2.6x; export 24 MP 1.6-1.8 s, 60 MP 5.6 s (target <= 10 s met).
* **GPU default ON for this class, gated by the self-test**, mainly for full quality, spatial stages and export. For point-only drags CPU (33-62 ms) and GPU (15-18 ms wall, readback 9-13 ms and up to 127 ms) are close; use the GPU for them only when the display path avoids readback.
* **GLES not needed** on this device (Vulkan 1.1 works); GLES 3.1 also passes its self-test and is a valid fallback, but its on-device parity is unmeasured.
* **Tile size 1024** (60 MP pool 310 MB, RSS 930 MB; 2048 is ~15 % faster at 24 MP, pool 460 MB).
* **Interactive tier 1080 px** (ladder 1080/810/640/480): 1600 px full quality is 48 ms best case, 110-160 ms bad case.
* Never submit > ~1 s per submission; persist `deviceKey` and `storage16` with the self-test result.
* Open: readback copy-out up to 127 ms in some runs; GPU frequency bimodality (retest without battery saver); HW Main10 decode not chosen by the decode ladder; no Main10 encode.

## 7. Not measured
GLES parity vs CPU on device and forced-gles engine run (shipped binaries lack the new HAL); Main10 encode/decode-back level count; in-app Motion Photo generation and the app's 10-bit transcode; a fixed 2-min sustain (ran ~4.6 min, one variant); isolated Vulkan self-test cost (only the GLES self-test, 21 ms, was timed); power; timings with Battery Saver off; 60 MP CPU path; Pixel 5 60 MP.

## 8. Cleanup
Removed `/data/local/tmp/dapple` and `/sdcard/Download/dapple_test`, uninstalled the app (it was not installed before), deleted the MediaStore row of the `dapple_test` folder (id 205095). No Motion Photos were written to the phone's media storage. `build/note10/listing_before.txt` vs `listing_after.txt` (`/data/local/tmp`, Download, DCIM, Pictures, Movies, package list): identical except the mtime of the `/data/local/tmp` directory entry. Screen woken once (KEYCODE_WAKEUP) and Home pressed; no settings changed; battery saver left ON.


## Wave 8 (2026-11-12, HEAD 1088f2d clean worktree build, release APK; Android 12 (API 31), 60 Hz panel only (modes 60 Hz at 1440x3040 / 1080x2280 / 720x1520, owner's size override 1080x2280), **Battery Saver ON** (`low_power` 1), battery 100 % on USB, Thermal Status 0 throughout)

Logs `build/dev2/note10plus/logs/`, screenshots `build/dev2/note10plus/app_*.png`. Not pinned; the native run overlapped the Pixel 5 native run (separate devices).

**Correctness (Vulkan, Adreno 640):** same as the Pixel 5: gpu_tests quick / full PASSED, film parity ok including the six enlarger rows (fix confirmed on Adreno Vulkan), film frames ok, film-tiling PASSED, coverage 192/192, golden selfcheck OK, exec tests ok, gpu_engine_tests hal/box/sched/select/external PASSED, match_cache test SKIP (hard-coded path). **GLES forced fails the film checks exactly as on the Pixel 5** (40 / 23 / 52 / 3 failures).
**Film GPU frame (GPU / wall ms):** 810x1080 interactive 53.5 / 183; 1080 px 48.7 / 164-174; 1600 interactive 131 / 374; grain drag 140 / 381; 1600 final 213 / 455; 810x1080 final 147 / 273; 3600 final 857 / 2076; 1600 final + enlarger 365 / 608. Against 8.3 / 11.1 / 16.7 ms: all interactive cases missed. GLES forced: 810x1080 114 / 170, 1600 interactive 174 / 263. Per pass (`DAPPLE_GPU_PROFILE=1 DAPPLE_GPU_PROF_REPEAT=20`): 810x1080 film_point 3.1, gnoise 0.9, **h3 6.5, v3 10.2**, pd 1.7, readback 0.5 = 23.0 ms, **while the same frame run alone is 53 ms** (back-to-back repeats are 2.3x faster: clock ramp / Battery Saver); 1600 interactive 86.8 ms (h3 27.8, v3 49.2).
First film frame: 1080 px 387 ms (upload 128, record 182, wait 59); 1600 px 476 ms. Session path wall ms (still / strength / exposure / grain / warmth): 1080 px 31.0 / 32.5 / 46.4 / 39.2 / 90.3; 1600 px 111 / 115 / 111 / 127 / 166.
Drag bench 480 px (`DAPPLE_GPU=1`, median ms): strength 9.7, grain 13.7 (first 126), **film_shoot 168.3**, print 26.2, black 28.1, latitude 16.7, wb 16.2, warmth 35.8, tint 26.7, film_exp 15.9, exposure (film on) 19.1.
Non-film 1600 px: point 16.2 wall, interactive 54.9, full 107 (GPU 26.9, copy-out 68 ms). 24 MP whole: interactive 479, full 2144 ms (**readback copy 1492 ms in the full frame**, 111-126 ms elsewhere); tiled export 24 MP 1.4-1.7 s (14-17 MP/s), vs CPU dE mean 0.0357 p99 0.474. 60 s sustained loop: GPU 53.4-54.0 ms, Thermal 0, flat.

**App (fresh install):** `gpu probe: api=vulkan device="Adreno (TM) 640" self-test fresh, probe total 307 ms`, warm-up 1220 ms; later `self-test cached pass`, frames GPU 3 / CPU 0, device lost 0, demotions 0.
**BUG 1: photo access cannot be granted on Android 12.** The 允许访问 button logs `permissions_handler: No permissions found in manifest for: []9` and toasts `需要允许访问照片`; `Permission.photos` resolves to nothing below API 33. After `pm grant ... READ_EXTERNAL_STORAGE` (revoked again afterwards) the app still shows 未允许访问照片. **No photo could be opened in the editor, so there are no editor / drag / layout / zero-copy results for the Note10+.**
**BUG 2: in-app camera (badge `RAW + JPEG`): the shutter greys out, CameraX delivers both buffers (`ImageReader-4032x3024f20` and `f100` queued for the first time) but neither `onImageSaved` nor `onError` follows (no `DappleCapture` line), nothing is saved (no file, no MediaStore row), the shutter stays at alpha 0.4.** Reproduced twice (25 s wait). Scene: a dim grey surface (phone lying camera-side down). No `sample.dng` from this phone.
**Diagnostics page** (long-press 版本): `mode auto · probe ready · in use`, `vulkan · Adreno (TM) 640`, Android API 31. Self-tests after 全部重测: `enc8avc OMX.qcom.video.encoder.avc` PASS 269 ms, `enc8hevc OMX.qcom.video.encoder.hevc` PASS 486 ms, `enc8avc c2.android.avc.encoder` PASS 281 ms. Ladders: `encodeLadder1080p30 / 1080p30Hdr / 8Bit`: 1. HEVC Main 8-bit (OMX.qcom), 2. H.264 High OMX, 3. c2 sw. **`decodeLadderHevc10`: software only** (HEVC Main10 hardware decode is still excluded, although `OMX.qcom.video.decoder.hevc` declares Main, Main10, Main10HDR10); Hevc8 hw then sw, Avc8 hw then sw, Avc422 sw. `OMX.qcom.video.encoder.hevc` declares Main and Main10HDR10 only; there is no Main10 self-test row, so Main10 encode with Surface input was NOT exercised.
Layout (portrait 1080x2280, 420 dpi): home with the `还不能读取手机里的照片` banner and the Settings page are clean and legible (`app_01_start.png`, `app_11_settings.png`).
NOT verified: editor, film in the app, zero-copy present, `dapple_view` counters, capture save / DNG, Main10 encode and decode.


## Device bug fixes round (Android 12 permission, in-app camera, settings, editor checks)

Phone: Galaxy Note10+ (SM-N976, Android 12 / API 31, Adreno 640), serial RFCMA05Q5ZX. Build: release arm64, debug key, from a worktree of HEAD 1088f2d with only my changes applied (`build/dev3/wt`; the main tree carries other engineers' in-flight edits); APKs `build/dev3/app1..4-arm64.apk` (app4 is the final one installed). Screenshots `build/dev3/note10plus/app_*.png`, widget-test renders of small and landscape screens `build/dev3/note10plus/widget/`.

### 1. Photo access below Android 13
* Cause (confirmed in permission_handler_android 13.0.1, `PermissionUtils.getManifestNames`): `Permission.photos` resolves to READ_MEDIA_IMAGES only when `SDK_INT >= 33`; below that the list is empty, hence "No permissions found in manifest for: []9", a request that can never succeed, and a status that never turns granted even after `pm grant READ_EXTERNAL_STORAGE`.
* Fix: `app/lib/services/media_permission.dart` (`mediaPermissionForSdk`: >= 33 `photos`, below that `storage` = READ_EXTERNAL_STORAGE, which the manifest already declares with maxSdkVersion 32; `MediaPermission` with injectable SDK level / status / request; granted and limited both count, so Android 14 partial access keeps working). SDK level comes from a new `sdkInt` method on the `dapple/capture` channel (CapturePlugin.kt). Used by `refreshPermission` / `requestPermission` in app_state.dart. Tests `app/test/media_permission_test.dart` (API 24, 29, 31, 32, 33, 34, 35).
* Device: with READ_EXTERNAL_STORAGE revoked, the in-app 允许访问 button shows the system dialog (`app_02_perm_dialog.png`); allow gives granted=true and no permission_handler error in logcat. The phone's own photos are all older than 7 days and the first scan only reads the last 7 days (by design), so the library stayed empty until a fresh test photo was pushed (synthetic 24 MP JPEG in `Pictures/DappleTest`); it then appeared (`app_04_home.png`).
* Not verified: Android 14 partial selection (no such device; the limited-status path is unchanged), API 24-28 on a device. Note: `saveToGallery` uses MediaStore RELATIVE_PATH (API 29+), so saving a capture to the gallery would fail on API 24-28; not touched.

### 2. In-app camera never saved
* Cause (logcat with new `DappleCapture` lines): with RAW_JPEG on this HAL both buffers arrive within 0.6 s, CameraX delivers the JPEG (`onImageSaved` 1 of 2 at ~0.65 s) and **never delivers the DNG** (no second callback, no error; 10 s and 25 s waits, only camera WatchDog lines). The code finished only after both callbacks and had no timeout, so the shutter stayed grey. A RAW-only binding (`OUTPUT_FORMAT_RAW`) works on the same HAL: 24 415 080 byte DNG in 0.62 s. So the stall is specific to the RAW+JPEG combination in CameraX 1.5.1 on this phone; it is not the executor/looper (callbacks run on main, the JPEG one arrives) and not the output file options.
* Fix (CaptureActivity.kt, new CaptureFiles.kt): per-shot watchdog (RAW+JPEG 10 s, JPEG 15 s) and a 3 s grace when the JPEG is saved but the DNG is not; files are validated before delivery (complete JPEG FFD8..FFD9; DNG = TIFF header and plausible size); a failed or stalled RAW+JPEG marks the lens RAW-broken for the process, rebinds JPEG-only and delivers (or re-shoots) the JPEG; a failed JPEG-only shot shows 拍摄失败，请重试 in the viewfinder and rebuilds the pipeline; the shutter is always re-enabled; late callbacks of an abandoned shot are ignored. The badge is honest: `JPEG · RAW 待确认` until a DNG was verified, `RAW + JPEG` only after a verified DNG, `JPEG · 本机 RAW 不可用` after a RAW failure, `JPEG` otherwise (this also fixes the emulator, which advertised RAW and wrote none).
* Device: first shot after opening shows `JPEG · RAW 待确认` (`app_10_cam_v3_badge.png`), the JPEG is delivered 3.0 s after it landed and the editor opens; later opens show `JPEG · 本机 RAW 不可用` (`app_12_cam_open2.png`) and shoot JPEG only in 0.7 s. Shutter recovers.
* DNG sample for the opcode work: `build/dev3/note10plus/sample.dng` (24 415 080 bytes, little-endian TIFF, 4032x3024, 16-bit CFA, DNGVersion 1.4.0.0, OpcodeList2 3908 bytes and OpcodeList3 present). It was captured with a throw-away experiment build (RAW-only binding, file copied to the app's external files dir, pulled, deleted from the phone). The shipped app does NOT produce DNGs on this phone. Scene: close grey wall, a flat dim frame.
* Option for later: RAW-only works on this HAL; delivering the DNG alone needs `CaptureResult` and `AppState.ingestCapture` to accept a missing JPEG (not done; editor/app_state owners).
* Cleanup: the 6 test JPEGs in `Pictures/Dapple` and the pushed `Pictures/DappleTest` file are deleted (MediaStore rows too). The app keeps private copies of the 6 camera shots (and an entry for the synthetic photo) in its library; the library has no delete action and I did not clear app data.

### 3. Settings page on short screens
* Cause: the wide layout (width >= 600 dp, e.g. Pixel 5 landscape 2340x1080) put the six category entries in a plain `Column` of 56 dp rows (372 dp needed, about 330 dp available), nothing scrolled. The phone layout already scrolls.
* Fix (settings_page.dart): the category column is a `ListView`; 关于 gets a normal row 编解码器诊断 (subtitle, chevron) that opens the diagnostics page; the long-press on 版本 stays as a shortcut. Tests in `codec_diagnostics_test.dart`: the phone entry opens the page; at 891x411 dp the category list scrolls to 关于 and the entry opens diagnostics.
* Device (Note10+ portrait): `app_27_settings_bottom.png` shows 关于 -> 开源许可 / 编解码器诊断 / 版本. Not verified on the real Pixel 5 in landscape (not my device); verified in the widget test and `widget/zz_settings_landscape.png`.

### 4. Home and library on small and short screens
No small phone was available to me (the Note10+ is 1080x2280 and `wm size` is a system setting). Rendered home and settings at 320x568, 360x480 and 891x411 dp in widget tests and viewed them: no clipping or overflow (`widget/zz_*.png`). On the 891x411 landscape the three stacked group rows push the photo grid below the fold, it scrolls. Android 12 with the permission fix: `app_04_home.png`, `app_14_home.png`, `app_26_settings.png`. Nothing else needed fixing.

### 5. Editor checks on the Note10+ (editor UI is being replaced; observations only)
* Opened the synthetic 24 MP JPEG: `frame 640x427 tier=interactive film=on exec=vulkan | gpu 1 cpu 0 | cpu_ms 114.6 (exec 112.7; upload 71.1 gpu 0.0)` (first frame incl. upload and pipelines), then `1080x720 tier=interactive film=off exec=vulkan | gpu 31 cpu 0 | cpu_ms 41.0 (tables 1.9 exec 38.9; upload 38.1)`, `1080x720 tier=full ... gpu 61 cpu 0 | cpu_ms 31.7 (tables 22.2 exec 9.4)`. No CPU fallback frame, no reasons (`log_exposure.txt`).
* Exposure drag (two slow swipes): `dapple_view: shown 30 ext 29 host 1 hostBytes 3110400 tooSmall 0 refused 0 | last 1080x720 render 30.94 ms present 0.47 ms latency 47.42 ms | ema render 31.72 present 0.51 fenceWait 0.01`. **Zero-copy present works on Adreno 640**: 29 of 30 frames external, the single host frame is the first.
* Film (style 琥珀, 冷调) and strength drag 100% -> 76%: `shown 60 ext 59 host 1 ... last 1080x720 render 45.73 ms present 0.34 latency 65.62 ms | ema render 49.21`; another run `shown 90 ext 88 host 2 ... last 480x320 render 58.63 ms latency 88.33 ms | ema render 67.71`. Film frames are slower than plain exposure frames (31 ms) and the viewport dropped to 480x320 in the strength drag. **No `dapple_engine frame` line appears for film frames at all**, even with `debug.dapple.frame_profile=1` (set for one run, reset to empty afterwards): the film path does not pass through the frame log in `engine_session.cpp`, so executor, reasons and cpu_ms for film cannot be read from logcat. The diagnostics page shows `frames: GPU 3 · CPU 0 · last executor vulkan` for the process. A log hook in the film path is needed.
* Editor observations (not fixed): the strength change 100% -> 76% on 琥珀 gives a near-identical preview; one tap on the 风格 panel reset icon reset the style; the exposure ruler is coarse (a 300 px swipe moved several EV and blew out the picture, `app_18_style.png`).
* Codec diagnostics page (`app_28_diag*.png`): self-tests PASS enc8avc OMX.qcom (269 ms, 220 luma levels), enc8hevc OMX.qcom (486 ms, 221), enc8avc c2.android (281 ms, 219). `encodeLadder1080p30`, `...Hdr`, `...8Bit`: 1. HEVC Main 8-bit OMX.qcom.video.encoder.hevc, 2. H.264 High HW, 3. H.264 High c2 (sw); **there is no Main10 / 10-bit encode row**. `decodeLadderHevc10`: **1. software, the only row**, although Declared codecs lists `OMX.qcom.video.decoder.hevc` with profiles Main, Main10, Main10HDR10 (max 8192x8192, formats Flexible/NV12/I420). `decodeLadderHevc8`: hardware then software; `decodeLadderAvc8`: hardware then software; `decodeLadderAvc422`: software. GPU block: vulkan, Adreno 640, self-test cached pass, device lost 0, demotions 0.

### Build notes
The first native build in the worktree took 17 min (ninja pools 4 compile / 2 link and `org.gradle.workers.max=2`, set only in the worktree); later builds 15-50 s. Gradle needed `ANDROID_USER_HOME` as a Windows path.
* JVM unit tests (worktree, `:app:testDebugUnitTest`): exit 0, includes new CaptureFilesTest (JPEG/DNG validation, honest badge text), CodecLadderTest, GpuProbeTest, StreamProbeTest. Worktree: remove with the junction first (`rmdir build/dev3/wt/tools`) then `git worktree remove --force build/dev3/wt`.


### Follow-up: RAW-first capture, DNG-only ingest, old-Android gallery save

* Strategy (`CaptureStrategy` in CaptureFiles.kt, used by CaptureActivity): by what works, no device names. Per camera id + `Build.FINGERPRINT` (SharedPreferences `dapple_capture`): RAW+JPEG is tried first; if the DNG does not arrive (3 s grace after the JPEG, 10 s timeout, error) the lens goes to **RAW-only**, and only if RAW-only fails too to JPEG-only; the failed shot is repeated automatically with the next mode (at most two repeats). A mode that produced a verified DNG is remembered as `MODE+`; an unverified downgrade is stored for good, a verified mode that fails once is demoted for the running process only. Badge: `RAW + JPEG`, `RAW`, `JPEG`, with ` · 待确认` while a RAW mode has not yet produced a DNG on this install.
* Dart: `CaptureResult` takes a DNG alone (`jpegPath` nullable, `stem`); `AppState.ingestCapture` accepts it (no camera preview, `previewPath` null until the develop; the DNG goes to Pictures/Dapple at once, and after the develop the developed JPEG goes to Pictures/Dapple next to it at the save-size setting, `saveCaptureJpegToGallery`). Debug cleanup: `AppState.purgeCaptures()` (DebugOpen `{"purgeCaptures":true}`, debug builds only).
* API 24-28: `saveToGallery` copies into `<Pictures>/Dapple/<name>` (no overwrite, numeric suffix) and indexes it with MediaScannerConnection; API 29+ keeps RELATIVE_PATH (`CaptureFiles.legacyTarget`, tested; not run on a real API 24-28 device).
* Tests: JVM `CaptureStrategyTest` (7: best mode by capability, RAW+JPEG -> RAW not JPEG, RAW -> JPEG and stays, remembered per camera/fingerprint, verified demotion not persisted, remembered mode no longer offered), `CaptureFilesTest` (5: files, badge texts, legacy path), all JVM suites exit 0 (ClipEdit 4, CodecLadder 29, GpuProbe 1, StreamProbe 9 too). Dart `capture_test.dart` (DNG-only ingest, developed copy pending, JPEG capture needs none, purge) pass.
* Device (Note10+, fresh app data, `build/dev3/app5-arm64.apk`, built from a worktree of HEAD 9a19297 plus the uncommitted hunks): first shot, badge `RAW + JPEG · 待确认`: `shot 1 (RAW_JPEG): saved 1/2` at +0.83 s, DNG missing after 3 s, `RAW_JPEG failed on this lens, now RAW`, shot 3 (RAW) saved at +1.8 s, the editor opened on the DNG, the library thumbnail is the engine's develop of the DNG, `Pictures/Dapple` got `DAP_..._003913.dng` (24 415 080 B) and `DAP_..._003913.jpg` (21.4 MB, developed, 3008x4016) (`b_01..b_04`). Second shot: badge `RAW`, saved in 0.77 s, no stall.
* `sample2.dng`: `build/dev3/note10plus/sample2.dng` (24 415 080 B, same format as sample.dng). The phone lies on its back, the scene is a dark flat surface at close range, not a normal scene. **The engine's develop of it is dark with strong magenta/purple noise** (developed JPEG mean RGB about 60/40/60; the camera's own JPEG of the same spot had looked mid grey earlier): worth checking in the DNG opcode work (black level, as-shot neutral, lens shading opcodes, auto exposure for a very dark frame).
* Cleanup: test files in `Pictures/Dapple` deleted (MediaStore rows too, folder removed); the app's private library copies are gone because I ran `pm clear` on the app (only test photos and default settings were in it), then re-granted CAMERA and READ_EXTERNAL_STORAGE with `pm grant`; the app stays installed. The `dapple_capture` strategy memory was cleared with it, so the next camera use on this phone starts with RAW+JPEG again and learns RAW-only after one 3 s stall.

## 2026-11-16 (a9d76de, in the app, battery saver on) Film exports after the quarter-scale low planes

One export each, stock kodak_portra_400 at the stock's own grain and halation, `export_stream GPU ... ok`:

| file | total (ms) | before (5a57516) |
|---|---|---|
| A6700 26 MP | 4470 | 7250 |
| X-S10 26 MP | 5884 | 8700 |
| X-T50 40 MP | 9474 | 15000 |
| A7RM4 61 MP | 9266 | 18700 |

Largest stage: waiting for the GPU tiles (A6700 2305, X-T50 3876, A7RM4 5834 ms). Without film: A7RM4 3762 ms (was 4300 - 4500). No tile seams (neighbour-difference ratio at tile multiples 0.98 - 1.09). Film pad drags (one large 2 s swipe, not comparable with the earlier slow drags): exposure pad frame p50 / p90 18.7 / 28.2 ms, colour pad 29.0 / 37.4 ms; the print-table easing has still no like-for-like phone measurement.
