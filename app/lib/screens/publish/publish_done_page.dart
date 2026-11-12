import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../../model/catalog.dart';
import '../../services/share_service.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/export_size.dart';
import '../../widgets/icons.dart';

enum DoneResult { finish, next, again }

/// Honest result screen (requirements 4.11.4): every step appears only once it is done.
class PublishDonePage extends StatefulWidget {
  const PublishDonePage({
    super.key,
    required this.app,
    required this.preset,
    required this.count,
    required this.ratio,
    required this.size,
    required this.fallback,
    this.nextBatch,
  });
  final AppState app;
  final Preset preset;
  final int count;
  final String ratio;
  final int size;

  /// true = saved to the gallery (the target app has not been opened yet).
  final bool fallback;

  /// 1-based number of the next post when the selection was split.
  final int? nextBatch;

  @override
  State<PublishDonePage> createState() => _PublishDonePageState();
}

class _PublishDonePageState extends State<PublishDonePage> {
  bool _opened = false;

  String get _appName => widget.preset.app ?? '其他 App';

  Future<void> _open() async {
    final pkg = widget.preset.package;
    if (pkg == null) return;
    final ok = await ShareService.instance.openApp(pkg);
    if (!mounted) return;
    if (ok) {
      setState(() => _opened = true);
      widget.app.showToast('已打开$_appName');
    } else {
      widget.app.showToast('没能打开$_appName');
    }
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final mq = MediaQuery.of(context);
    final footer = math.max(mq.padding.bottom, 16.0);
    final title = widget.fallback ? '已存入相册' : '已交给$_appName';
    final ratio = widget.ratio == 'orig' ? '原比例' : widget.ratio;
    final steps = <String>[
      '已导出 ${widget.count} 张 · $ratio · ${exportSizeName(widget.size)}',
      if (widget.fallback) '已存入相册「Dapple」' else '已交给$_appName',
      if (_opened) '已打开$_appName',
    ];
    final showOpen = widget.fallback && !_opened && widget.preset.package != null;
    final buttons = <Widget>[
      if (showOpen) SecondaryButton('打开$_appName', soft: true, onPressed: _open, key: const ValueKey('done-open')),
      if (widget.nextBatch != null)
        PrimaryButton('继续第 ${widget.nextBatch} 篇', key: const ValueKey('done-next'), onPressed: () => Navigator.of(context).pop(DoneResult.next)),
      SecondaryButton('再发到其他平台', key: const ValueKey('done-again'), onPressed: () => Navigator.of(context).pop(DoneResult.again)),
      widget.nextBatch == null
          ? PrimaryButton('完成', key: const ValueKey('done-finish'), onPressed: () => Navigator.of(context).pop(DoneResult.finish))
          : SecondaryButton('完成', key: const ValueKey('done-finish'), onPressed: () => Navigator.of(context).pop(DoneResult.finish)),
    ];
    return Scaffold(
      backgroundColor: c.bg,
      body: Stack(children: [
        Center(
          child: ConstrainedBox(
            constraints: const BoxConstraints(maxWidth: 440),
            child: Padding(
              padding: EdgeInsets.fromLTRB(24, mq.padding.top + 24, 24, footer),
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                const Spacer(),
                Container(
                    width: 56,
                    height: 56,
                    decoration: BoxDecoration(color: c.okBg, shape: BoxShape.circle),
                    alignment: Alignment.center,
                    child: DIcon(DI.check, size: 28, color: c.okIcon, stroke: 3)),
                const SizedBox(height: 24),
                Semantics(header: true, child: Text(title, key: const ValueKey('done-title'), style: T.h1(c.text))),
                const SizedBox(height: 24),
                for (final s in steps)
                  Padding(
                    padding: const EdgeInsets.only(bottom: 12),
                    child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
                      DIcon(DI.check, size: 18, color: c.okIcon, stroke: 2.4),
                      const SizedBox(width: 12),
                      Expanded(child: Text(s, style: T.body(c.text))),
                    ]),
                  ),
                const Spacer(),
                for (var i = 0; i < buttons.length; i++) ...[if (i > 0) const SizedBox(height: 12), buttons[i]],
              ]),
            ),
          ),
        ),
        ToastLayer(app: widget.app, bottom: footer + buttons.length * 52 + buttons.length * 12 + 12),
      ]),
    );
  }
}
