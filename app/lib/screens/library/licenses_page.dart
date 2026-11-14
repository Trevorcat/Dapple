import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show rootBundle;

import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/icons.dart';
import 'page_bar.dart';

/// 关于 / 开源许可: third-party notices. The film profiles are CC BY-SA 4.0 and their credit line has to be
/// shown (docs/film/README.md "Licence and attribution"): the line comes from the engine, the notice and the
/// licence text from the assets next to the pack.
class LicensesPage extends StatefulWidget {
  const LicensesPage({super.key, required this.app});
  final AppState app;

  /// Credit line used when the engine reports none (no pack loaded).
  static const fallbackAttribution =
      'Film and paper profiles: spektrafilm by Andrea Volpato, github.com/andreavolpato/spektrafilm, licensed CC BY-SA 4.0 (converted to a binary format by Dapple)';
  static const licenseUrl = 'https://creativecommons.org/licenses/by-sa/4.0/';

  /// docs/legal/ffmpeg-lgpl.md: notice for the FFmpeg software decoder used by the Motion Photo tool (LGPL-2.1+).
  static const ffmpegNotice =
      'This software uses libraries from the FFmpeg project (https://ffmpeg.org) under the GNU Lesser General Public License version 2.1 or later: libavutil, libavcodec, libavformat, libswscale and libswresample, version 9.0.2, built with only the H.264 / HEVC decoders (software and Android MediaCodec), the PCM audio decoders, the MP4/MOV demuxer, the scaler and the resampler, and used as separate shared libraries that can be replaced. FFmpeg is a trademark of Fabrice Bellard, originator of the FFmpeg project. The source code of the version used, the build configuration and the build script are available from the Dapple project on request.';
  static const ffmpegLicenseUrl = 'https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html';

  /// docs/legal/libraw.md: RAW decoding. LibRaw is dual licensed LGPL-2.1 / CDDL-1.0; Dapple uses it under CDDL-1.0 (statically linked, unmodified).
  static const librawNotice =
      'RAW decoding uses LibRaw 0.23.0 (snapshot 202611, unmodified), Copyright (C) 2008-2026 LibRaw LLC, with code derived from dcraw by Dave Coffin, used under the Common Development and Distribution License version 1.0 (CDDL-1.0). The source of the version used is available from https://github.com/LibRaw/LibRaw (commit a9a14b1970d188c25dade81f8326ff00a6e33649).';
  static const librawLicenseUrl = 'https://opensource.org/license/cddl-1-0';

  /// docs/legal/libjxl.md: JPEG XL decoder for DNG 1.7 raw files (Samsung Galaxy S22 and newer, Adobe DNG Converter).
  static const libjxlNotice =
      'JPEG XL decoding (DNG 1.7 raw files) uses libjxl 0.11.1, Copyright (c) the JPEG XL Project Authors, and Highway 1.2.0, Copyright Google LLC, both under the BSD 3-Clause License: redistribution and use in source and binary forms, with or without modification, are permitted provided that the copyright notice, the list of conditions and the following disclaimer are retained (in binary form: reproduced in the documentation); the names of the copyright holders may not be used to endorse or promote products derived from this software without specific prior written permission. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE.';
  static const libjxlLicenseUrl = 'https://github.com/libjxl/libjxl/blob/main/LICENSE';

  /// docs/legal/libomp.md: the parallel demosaic of the Android builds (statically linked OpenMP runtime).
  static const libompNotice =
      'The parallel RAW demosaic uses the LLVM OpenMP runtime (libomp), Copyright (c) the LLVM Project contributors, under the Apache License 2.0 with LLVM Exceptions.';
  static const libompLicenseUrl = 'https://llvm.org/LICENSE.txt';

  /// docs/legal/ai-models.md: AI masks. Models: Google MediaPipe (Apache-2.0) and Depth Anything V2 Small (Apache-2.0); runtimes: LiteRT (Apache-2.0)
  /// and ONNX Runtime (MIT). The texts shown below come from assets/legal/ai/.
  static const aiNotice =
      'AI masks run on the device with these models, all under the Apache License 2.0: Face Detector (BlazeFace short range), Face Mesh v2, Selfie Multiclass 256x256 and the Interactive Segmenter "MagicTouch" from Google MediaPipe, Copyright The MediaPipe Authors (https://github.com/google-ai-edge/mediapipe), unmodified; and Depth Anything V2 Small, Copyright the Depth Anything V2 authors (https://github.com/DepthAnything/Depth-Anything-V2), converted to ONNX Runtime format without changing the weights. The face outline, lip, eye and eyebrow shapes use the landmark index sets of MediaPipe Face Mesh (face_mesh_connections.py). Inference uses LiteRT 1.4.0 (Google, Apache License 2.0) and a size-reduced build of ONNX Runtime 1.32.0 (Copyright (c) Microsoft Corporation, MIT License; its third-party notices follow).';
  static const aiModelsUrl = 'https://github.com/google-ai-edge/mediapipe';

  @override
  State<LicensesPage> createState() => _LicensesPageState();
}

class _LicensesPageState extends State<LicensesPage> {
  String? _notice, _license, _apache, _ortMit, _ortNotices;
  bool _showOrtNotices = false;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    String? n, l, a, m, t;
    try {
      n = await rootBundle.loadString('assets/film/NOTICE.md');
      l = await rootBundle.loadString('assets/film/SPEKTRAFILM_LICENSE.txt');
    } catch (_) {}
    try {
      a = await rootBundle.loadString('assets/legal/ai/APACHE-2.0.txt');
      m = await rootBundle.loadString('assets/legal/ai/ONNXRUNTIME-MIT.txt');
      t = await rootBundle.loadString('assets/legal/ai/ONNXRUNTIME-THIRD-PARTY-NOTICES.txt');
    } catch (_) {}
    if (!mounted) return;
    setState(() {
      _notice = n;
      _license = l;
      _apache = a;
      _ortMit = m;
      _ortNotices = t;
    });
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final line = widget.app.film.attribution.isNotEmpty ? widget.app.film.attribution : LicensesPage.fallbackAttribution;
    Widget block(String key, String title, String body, {bool mono = false}) => Padding(
          padding: const EdgeInsets.symmetric(horizontal: Sp.l, vertical: Sp.s),
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(title, style: T.body17(c.text)),
            const SizedBox(height: Sp.xs),
            SelectableText(body, key: ValueKey(key), style: T.small(c.text2)),
          ]),
        );
    return Scaffold(
      backgroundColor: c.bg,
      body: Column(children: [
        PageBar('开源许可',
            leading: DIconButton(DI.back, label: '返回', onPressed: () => Navigator.of(context).maybePop()),
            trailing: TextAction('完成', bold: true, size: 17, onPressed: () => Navigator.of(context).maybePop())),
        Expanded(
          child: ListView(padding: const EdgeInsets.only(bottom: 80), children: [
            const SectionTitle('胶片模拟'),
            block('licence-attribution', '胶片与相纸数据', line),
            block('licence-link', '许可证', 'CC BY-SA 4.0  ${LicensesPage.licenseUrl}'),
            if (_notice != null) block('licence-notice', '数据说明', _notice!),
            if (_license != null) block('licence-text', 'CC BY-SA 4.0 许可证全文', _license!),
            const SectionTitle('视频解码'),
            block('licence-ffmpeg', 'FFmpeg（动态照片，软件解码）', LicensesPage.ffmpegNotice),
            block('licence-ffmpeg-link', '许可证', 'LGPL 2.1  ${LicensesPage.ffmpegLicenseUrl}'),
            const SectionTitle('RAW 解码'),
            block('licence-libraw', 'LibRaw', LicensesPage.librawNotice),
            block('licence-libraw-link', '许可证', 'CDDL-1.0  ${LicensesPage.librawLicenseUrl}'),
            block('licence-libjxl', 'libjxl 与 Highway（JPEG XL）', LicensesPage.libjxlNotice),
            block('licence-libjxl-link', '许可证', 'BSD 3-Clause  ${LicensesPage.libjxlLicenseUrl}'),
            block('licence-libomp', 'LLVM OpenMP 运行库', LicensesPage.libompNotice),
            block('licence-libomp-link', '许可证', 'Apache-2.0 with LLVM Exceptions  ${LicensesPage.libompLicenseUrl}'),
            const SectionTitle('人物与天空识别'),
            block('licence-ai', 'AI 模型与推理库（LiteRT、ONNX Runtime）', LicensesPage.aiNotice),
            if (_apache != null) block('licence-ai-apache', 'Apache License 2.0 全文（MediaPipe 模型、LiteRT、Depth Anything V2 Small）', _apache!),
            if (_ortMit != null) block('licence-ai-ort-mit', 'ONNX Runtime: MIT License', _ortMit!),
            if (_ortNotices != null)
              // 335 KB of third-party notices: laid out only when asked for
              _showOrtNotices
                  ? block('licence-ai-ort-notices', 'ONNX Runtime 第三方声明', _ortNotices!)
                  : Padding(
                      padding: const EdgeInsets.symmetric(horizontal: Sp.l, vertical: Sp.s),
                      child: Align(
                        alignment: Alignment.centerLeft,
                        child: TextAction('显示 ONNX Runtime 第三方声明全文', onPressed: () => setState(() => _showOrtNotices = true)),
                      ),
                    ),
          ]),
        ),
      ]),
    );
  }
}
