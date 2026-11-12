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

  @override
  State<LicensesPage> createState() => _LicensesPageState();
}

class _LicensesPageState extends State<LicensesPage> {
  String? _notice, _license;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    String? n, l;
    try {
      n = await rootBundle.loadString('assets/film/NOTICE.md');
      l = await rootBundle.loadString('assets/film/SPEKTRAFILM_LICENSE.txt');
    } catch (_) {}
    if (!mounted) return;
    setState(() {
      _notice = n;
      _license = l;
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
          ]),
        ),
      ]),
    );
  }
}
