# GLES backend toolchain: provenance and licences

Nothing here is shipped in the app. SPIRV-Cross is linked only into the host build tool `dapple_shaderc` (tools/shaderc); ANGLE and
SwiftShader are desktop test targets taken from the Android SDK emulator package (`tools/android-sdk/emulator/lib64/gles_angle`). What ships is the generated ESSL text (our own shaders translated).

| Component | Version / source | Licence | Use |
|---|---|---|---|
| SPIRV-Cross | KhronosGroup/SPIRV-Cross tag `vulkan-sdk-1.4.309.0`, `https://github.com/KhronosGroup/SPIRV-Cross/archive/refs/tags/vulkan-sdk-1.4.309.0.tar.gz`, sha256 `cf4f12a767d63f63024e61750e372ffdc95567513b99ed9be6f21f474b328ddd`, extracted to `tools/_dl/spirv-cross/` | Apache-2.0 (`LICENSE` in the tree) | host tool: SPIR-V -> ESSL 310 / MSL |
| ANGLE (`libEGL.dll`, `libGLESv2.dll`) | **Android SDK Emulator** package (Pkg.Revision 37.2.13), the files in `tools/android-sdk/emulator/lib64/gles_angle/` (ANGLE 2.1.22762, git `533fa5cedc61`; sha256 libEGL `fa71d18e3c4d11f1158d41d0f3852ffcbc19a1b48b144444cbc977043d6777fe`, libGLESv2 `c8974609aef6feafb07d97afa7859f28fd2494a8807c82b67526e5aab10b2049`). Nothing is copied: tests point at that folder (`DAPPLE_ANGLE_DIR`, ctest/cmake variable of the same name) | BSD-3-Clause (The ANGLE Project Authors; text in `tools/android-sdk/emulator/NOTICE.txt`, component list in `NOTICE.csv`, row `angle-renderer`) | desktop GLES 3.1 conformance target |
| SwiftShader (`vk_swiftshader.dll`, ICD `vk_swiftshader_icd.json`) | same folder | Apache-2.0 (`tools/android-sdk/emulator/LICENSE`, `NOTICE.csv` row `swiftshader`) | deterministic software Vulkan under ANGLE (`DAPPLE_ANGLE_BACKEND=swiftshader`) |
| Vulkan loader (`vulkan-1.dll`), `d3dcompiler_47.dll` | same folder | Apache-2.0 (loader); `d3dcompiler_47.dll` is Microsoft's redistributable, not used by our tests (D3D11 backend only) | loads the SwiftShader ICD for ANGLE |
| Khronos EGL / GLES headers | copied from the Android NDK 28.2.13676358 sysroot to `tools/_dl/khronos-gl/include` | Apache-2.0 / MIT (Khronos headers; per-file notices) | compile-time types only; the entry points are loaded dynamically |

Why this route for ANGLE: Google publishes no standalone ANGLE binaries and a source build needs depot_tools plus a Chromium-scale
checkout. The Android SDK emulator (an official Google component this project already uses for the x86_64 emulator) ships the
same ANGLE that Chromium uses, built with the Vulkan backend and SwiftShader, together with its NOTICE/LICENSE files. Decision of the
owner/lead 2026-11-12: use that copy in place. An earlier prototype extracted ANGLE from the Electron 33.2.0 release zip; that copy
(`tools/_dl/angle`, including the 115 MB zip) was deleted. The ANGLE DLLs are test-only and never shipped in the app.
