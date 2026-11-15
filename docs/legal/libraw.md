# LibRaw

* Used for RAW decoding (`core/src/decoders/libraw_decoder.cpp`), statically linked into the desktop tools and the Android / iOS libraries.
* Licence: LibRaw is dual licensed LGPL-2.1 / CDDL-1.0; Dapple uses it under **CDDL-1.0** (file-level copyleft, allows static linking in a mobile app). The unmodified sources are fetched by `cmake/Dependencies.cmake`, which applies the two local patches below at configure time.
* Version: upstream master snapshot 202611 (LibRaw 0.23.0 `Snapshot202611`, commit `a9a14b1970d188c25dade81f8326ff00a6e33649`, 2026-11-04, tarball sha256 `453b1311b4825cc1946f114b5a0bcf0d4ad62a52c667dd5cc8b2b70e28342020`). Reason: the 0.22.2 release cannot open the Sony ILCE-7M5 and cannot decode Nikon HE / HE* NEF; the snapshot can (docs/engine/raw-compat.md). Move back to a tagged release when one carries these.
* Not used: the Adobe DNG SDK integration (needed for JPEG XL compressed DNG 1.7), LibRaw's demosaic packs (GPL2 / GPL3 demosaic packs are not linked).
* The app's licences screen must list: LibRaw (CDDL-1.0, copyright LibRaw LLC / Alex Tutubalin, plus the dcraw-derived notices in `COPYRIGHT` of the sources).

## Local patches (CDDL-1.0 file-level terms: each modified file stays under CDDL-1.0; the modification is published with the app's source / licence screen entry for LibRaw)

1. **Lock patch** (2026-11-14, option `DAPPLE_LIBRAW_LOCKPATCH`, default ON): every `#pragma omp critical[(name)]` becomes a scoped `std::mutex`. Files and reasons: docs/legal/libomp.md, "LibRaw patch".
2. **Parallel decompression** (2026-11-16, always applied; marker `dapple patch 2`; the configure fails if the patch no longer applies to a new LibRaw version):
   * `src/decoders/decoders_dcraw.cpp`, `LibRaw::sony_arw2_load_raw` (Sony ARW 2.3, the 8 bit per pixel compressed ARW, e.g. ILCE-7RM4): the rows are read in chunks of 256 into one buffer (each row followed by one zero byte, as the original single-row buffer of `raw_width + 1` bytes) and decoded by an OpenMP `parallel for` over the rows of the chunk; the per-row decoding statements are unchanged, their variables are declared per row. Every row is an independent run of 16-byte blocks, so the samples are the same; without OpenMP (Apple) the loop runs serially. Measured on the 61 MP ILCE-7RM4 sample: desktop open 479 -> 274 ms (28 threads).
   * `src/decoders/fuji_compressed.cpp`, `LibRaw::fuji_decode_loop`: `schedule(dynamic, 1)` added to the existing `#pragma omp parallel for` over the compressed strips (a phone has fast and slow cores; a static split could put two strips on one slow core). The strips are independent; the samples are the same.
   Both are checked by decoding the public samples before / after (mosaics and exports byte-identical) and by `demosaic.wire_arw2_threads` (ARW 2.3 decode with 1 thread == with all threads). Origin of the change: Dapple; no code from other projects.
