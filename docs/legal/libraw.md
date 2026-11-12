# LibRaw

* Used for RAW decoding (`core/src/decoders/libraw_decoder.cpp`), statically linked into the desktop tools and the Android / iOS libraries.
* Licence: LibRaw is dual licensed LGPL-2.1 / CDDL-1.0; Dapple uses it under **CDDL-1.0** (file-level copyleft, allows static linking in a mobile app). No local patches: the unmodified sources are fetched by `cmake/Dependencies.cmake`.
* Version: upstream master snapshot 202611 (LibRaw 0.23.0 `Snapshot202611`, commit `a9a14b1970d188c25dade81f8326ff00a6e33649`, 2026-11-04, tarball sha256 `453b1311b4825cc1946f114b5a0bcf0d4ad62a52c667dd5cc8b2b70e28342020`). Reason: the 0.22.2 release cannot open the Sony ILCE-7M5 and cannot decode Nikon HE / HE* NEF; the snapshot can (docs/engine/raw-compat.md). Move back to a tagged release when one carries these.
* Not used: the Adobe DNG SDK integration (needed for JPEG XL compressed DNG 1.7), LibRaw's demosaic packs (GPL2 / GPL3 demosaic packs are not linked).
* The app's licences screen must list: LibRaw (CDDL-1.0, copyright LibRaw LLC / Alex Tutubalin, plus the dcraw-derived notices in `COPYRIGHT` of the sources).
