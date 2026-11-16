# OPPO Find N6 (SM8850, Adreno 840) device results, 2026-11-16

## 2026-11-16 (d8b6643 + floating bar 60 dp): first run on this phone

Release APK of the working tree (floating bar / dock 60 dp, built 21:4x, `install -r`), Find N6 3B163700HQ200000, outer screen (1140x2616, 520 dpi = 3.25 px/dp), taps and swipes only (resumed-activity check before every input), scripts and logs in `build/perf-1116n6/` (`open.sh`, `exp.sh`, `both.sh`, `none.sh`, `dr.sh`, `an_open.py`). SoC `ro.soc.model` SM8850. State: battery saver off (`low_power` 0), charging, battery 34 %, no thermal reading taken. Photo access: "全部照片和视频" (all). `debug.dapple.frame_profile` 1 (2 for the film pad traces), reset to empty. Public samples as `t62_{a6700.ARW, 7rm4.ARW, xs10.RAF, xt50.RAF}` (A6700 DSC00002, A7RM4 DSC00396, X-S10 compressed, X-T50 DSCF0118), scanned with `MEDIA_SCANNER_SCAN_FILE`. Backend: `exec=vulkan` in every frame line (device Adreno (TM) 840, shared present device); no GLES, `exec=cpu`, FATAL or ANR line in any log. Demosaic verdict at the first open: frame GPU 47 ms vs CPU 65 ms, region GPU 9 ms vs CPU 29 ms.

### Open to first frame (tap to the first `frame ... tier=` line, app restarted before each; 3 reps)
| file | ms |
| --- | --- |
| A6700 | 279 / 363 / 373 |
| A7RM4 | 418 / 462 / 450 |
| X-S10 | 562 / 511 / 504 |
| X-T50 | 605 / 601 / 683 |

(preview decode 167-436 ms of it; the X-Trans files are the slowest.)

### Full-size JPEG export (tap to the export log line; stage totals from the log)
| file | no film | N400C |
| --- | --- | --- |
| A6700 6240x4168 | 0.78 s: `export_jpeg` render 190, encode+write 250 | `export_stream` 1161 ms (stages: render 1065 = analyses 267 + GPU render 774; finish 58, write 37); peak 829 MB. Tap time not recorded for this one |
| A7RM4 9568x6376 | 1.43 s: `export_stream` 1336 ms (render 1259, GPU render 823, finish 55) | 2.70 s: 2618 ms (render 2472, GPU render 1781 with 628 ms waiting for the previous tile; finish 105); peak 1257 MB |
| X-S10 6246x4170 | 1.02 s: `export_jpeg` render 203, encode+write 135 | 2.19 s: 1602 ms (render 1512, GPU render 778; finish 72); peak 995 MB |
| X-T50 7752x5178 | 1.74 s: `export_jpeg` render 323, encode+write 373 | 2.82 s: 2366 ms (render 2217, GPU render 1132, finish 79); peak 1166 MB (the X-T50 carried the pad position left from the R100A test; its edit is not exactly the centre N400C) |

The largest stage is the render phase (analyses + windows + GPU + hand-off) in every film export, and for the non-film streamed A7RM4. Rows from the stage lines; the no-film whole-frame exports are render + encode only.

### Drags on the A6700 with N400C (`frame_profile` 1, interactive frames, 700 ms swipe, exec=vulkan, film=on, preview 810x541 throughout)
| pad | frames | cpu_ms first / rest p50 / p90 / max | frame interval p50 / p90 / max |
| --- | --- | --- | --- |
| exposure (光线 pad, horizontal) | 36 | 37.6 / 9.7 / 15.2 / 37.6 | 17 / 26 / 30 ms |
| colour pad (diagonal) | 45 | 18.2 / 7.7 / 13.2 / 19.3 | 13 / 22 / 29 ms |

One drag each, nothing repeated.

### Slide stock R100A on the X-T50 (成像 pad, `frame_profile` 2 `film_trace` luma_mean; no dimmed area on the pad in any position)
Horizontal swipe at constant height, 2 s (155 frames): luma_mean 0.442 start / 0.434 middle / 0.443 end (range 0.429-0.464), luma_med 0.412 / 0.404 / 0.404: the median is held; the right side is the denser picture. Vertical swipe at constant x from the bottom to the top, 2 s (153 frames): 0.452 (start, pad at the right end) / 0.414 middle / 0.518 end, lowest 0.362 near the bottom quarter; it rises towards 亮 as expected. Top end: sky pale blue, light but not clipped to white, leaves lighter; no hard highlight clipping seen at phone size.

### Floating bar (60 dp), outer screen at 3.25 px/dp
Capsule 195 px = 60 dp (home and editor). Ink above the icons / below the label: editor dock 36 / 34 px (11.1 / 10.5 dp), home 40 / 36 px (12.3 / 11.1 dp): the glyph and label boxes carry about 2 dp of their own air, so the box gaps are the designed 10 / 8 dp. The top is no longer cramped.

### Not measured
Thermal state, power; inner screen; sustained sessions; drags on the other three files; the GLES backend.

### Cleanup
Pushed files, `rm` of each export by exact path under `/sdcard/Pictures/Dapple/` (13 `t62_*` files), `rmdir /sdcard/Pictures/DappleT62`; `ls` of those two paths reports them absent. Prop reset (read back empty). Dapple stays installed; after the files were gone the library dropped its four entries.
