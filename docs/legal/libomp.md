# LLVM OpenMP runtime (libomp)

* Purpose: LibRaw's parallel demosaic (`DAPPLE_LIBRAW_OPENMP`, default ON, not on Apple platforms). Android links the NDK's `libomp.a` statically (`-static-openmp`); the Windows desktop tools load `libomp.dll` from the llvm-mingw toolchain (copied next to the executables).
* Licence: Apache License 2.0 with LLVM Exceptions (the LLVM project). The exception allows static linking into binaries without the Apache attribution of the object code; the app's licences screen should still list "LLVM OpenMP runtime, Apache-2.0 with LLVM exception".
