# Merge engine: HDR, panorama, Motion Photo (phase 3, engine side)

Code: `core/include/dapple/merge.h`, `motion_photo.h`; `core/src/hdr.cpp`, `pano.cpp`, `sequence.cpp`, `merge_io.cpp`, `motion_photo.cpp`.
C API: end of `dapple_c.h`. CLI: `dapple hdr | pano | motion | detect`. Tests: `tests/merge_tests.cpp` (ctest `merge_tests`, `DAPPLE_PERF=1` for 26 MP timings).

## Third-party
OpenCV 4.12.0 (Apache-2.0), built from source as static libs, modules core imgproc features2d calib3d flann stitching photo video
(video only for `findTransformECC`), no contrib / non-free / GPU / IPP / OpenCL; AKAZE (+CLAHE) features, never SIFT/SURF.
Pinned tarball (sha256 in `cmake/OpenCV.cmake`) cached in `tools/_dl/opencv`. CMake option `DAPPLE_WITH_OPENCV` (default ON);
OFF builds everything except alignment / stitching (`dapple_merge_available()` returns 0, HDR merges unaligned, pano errors).

## HDR
Linear RAW, exposure-normalised inverse-variance average with a smooth highlight gate; base = caller index or middle exposure.
Alignment: AKAZE homography (1.6k px) then masked ECC homography at up to 3k px. Exposure ratio is measured from the pixels
(median of mid-tone ratios) and cross-checked with EXIF. Deghost off/low/mid/high compares each frame with the base at reduced
resolution (noise-aware + relative + gradient tolerance) and replaces disagreeing pixels with the base's.
Output is a SourceImage (1.0 = base clipping point, values > 1 allowed; the renderer's shoulder handles them).
Limitation: motion over areas where the *base* is clipped cannot be detected (no reference); choose a darker base then.
JPEG (display-referred) inputs use Mertens exposure fusion (no deghost, result display-referred).
Archival: linear DNG (16 bit, ProPhoto D50 as camera space, BaselineExposure = log2 headroom; Dapple's decoder restores it
for files marked UniqueCameraModel "Dapple Linear Merge") or 16-bit TIFF.

## Panorama
OpenCV detail pipeline: features (0.6 MP) -> BestOf2Nearest (confidence cap raised, see pano.cpp) -> homography + ray bundle
adjustment -> wave correction (horizontal or vertical chosen from the pan) or `base` frame at the projection centre ->
spherical / cylindrical / plane warp -> graph-cut seams at 0.12 MP -> gain exposure compensation -> multi-band blend on the
linear float data (13-bit gamma inside the blender) -> largest inner rectangle crop or Telea fill. `longEdge` caps the output.

## Motion Photo
No FFmpeg. `trimMp4` stream-copies a window at keyframes (rebuilds stts/ctts/stss/stsc/stsz/stco, edit list for B-frame delay,
fast-start moov); AAC audio kept, PCM (Sony XAVC S) dropped. Window length therefore snaps to the source GOP (~1 s for XAVC S).
`buildMotionPhoto` inserts the Google v1 XMP (Camera + Container directory, Primary/MotionPhoto items, Length = video bytes,
video last, no trailing bytes) after APP0/Exif, replacing any old XMP. `validateMotionPhoto` checks XMP, item lengths, JPEG EOI,
ftyp/moov/mdat structure and that every sample offset lies inside the appended MP4. On phones the preferred path is
platform trimming (MediaCodec/AVFoundation) + `dapple_motion_photo_write`. The cover still must be supplied (no video decoder here).
iOS Live Photo (MakerApple key 17 + MOV content identifier + still-image-time track) is not implemented in the engine.
