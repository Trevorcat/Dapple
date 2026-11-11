import 'package:flutter/material.dart';

import '../../services/tool_models.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';

enum ToolDialogResult { done, background, cancelled, changeBase, closedFailure }

/// The task dialog of requirements 4.9.6 (alertdialog): running with 转入后台 / 取消, failed with 换基准帧 / 取消.
Future<ToolDialogResult> showToolProgress(BuildContext context, AppState app, ToolJob job) async {
  final r = await showDialog<ToolDialogResult>(
    context: context,
    barrierDismissible: false,
    builder: (ctx) => _ToolProgressDialog(app: app, job: job),
  );
  return r ?? ToolDialogResult.cancelled;
}

class _ToolProgressDialog extends StatefulWidget {
  const _ToolProgressDialog({required this.app, required this.job});
  final AppState app;
  final ToolJob job;

  @override
  State<_ToolProgressDialog> createState() => _ToolProgressDialogState();
}

class _ToolProgressDialogState extends State<_ToolProgressDialog> {
  bool _popped = false;

  void _pop(ToolDialogResult r) {
    if (_popped || !mounted) return;
    _popped = true;
    Navigator.of(context).pop(r);
  }

  @override
  void initState() {
    super.initState();
    widget.job.addListener(_onJob);
    WidgetsBinding.instance.addPostFrameCallback((_) => _onJob());
  }

  void _onJob() {
    final j = widget.job;
    if (j.status == JobStatus.done) _pop(ToolDialogResult.done);
    if (j.status == JobStatus.cancelled) _pop(ToolDialogResult.cancelled);
  }

  @override
  void dispose() {
    widget.job.removeListener(_onJob);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final j = widget.job;
    return ListenableBuilder(
      listenable: j,
      builder: (context, _) {
        final failed = j.status == JobStatus.failed;
        final isLive = j.kind == ToolKind.live;
        final title = failed ? (isLive ? '没能生成动态照片' : '没能对齐这几张') : j.kind.runningTitle;
        return Semantics(
          scopesRoute: true,
          explicitChildNodes: true,
          label: title,
          child: Dialog(
            key: const ValueKey('tool-dialog'),
            backgroundColor: c.surfaceHi,
            insetPadding: const EdgeInsets.symmetric(horizontal: 40),
            shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(Rd.dialog)),
            child: Padding(
              padding: const EdgeInsets.all(24),
              child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.stretch, children: [
                Text(title, key: const ValueKey('tool-dialog-title'), style: T.h2(c.text)),
                if (failed && isLive && j.error != null) ...[
                  const SizedBox(height: Sp.s),
                  Text(j.error!, maxLines: 3, overflow: TextOverflow.ellipsis, style: T.body(c.text2)),
                ],
                if (!failed) ...[
                  const SizedBox(height: Sp.m),
                  Text('${j.stepLine} · ${j.step}', key: const ValueKey('tool-dialog-step'), style: T.body(c.text2)),
                  const SizedBox(height: Sp.m),
                  Semantics(
                      label: '${j.percent}%', value: '${j.percent}', child: DProgress(j.fraction, height: 6)),
                  const SizedBox(height: Sp.s),
                  Align(
                      alignment: Alignment.centerRight,
                      child: Text('${j.percent}%', key: const ValueKey('tool-dialog-percent'), style: T.small(c.text2, tabular: true))),
                ],
                const SizedBox(height: Sp.l),
                if (!failed)
                  Row(mainAxisAlignment: MainAxisAlignment.end, children: [
                    KeyedSubtree(
                        key: const ValueKey('tool-background'),
                        child: ActionChip32('转入后台', onPressed: () {
                          widget.app.sendToBackground(j);
                          _pop(ToolDialogResult.background);
                        })),
                    const SizedBox(width: Sp.s),
                    KeyedSubtree(
                        key: const ValueKey('tool-cancel-job'),
                        child: TextAction('取消', size: 17, bold: true, onPressed: () {
                          widget.app.cancelJob(j);
                          _pop(ToolDialogResult.cancelled);
                        })),
                  ])
                else
                  Row(mainAxisAlignment: MainAxisAlignment.end, children: [
                    KeyedSubtree(
                        key: const ValueKey('tool-close-failure'),
                        child: TextAction('取消', size: 17, onPressed: () => _pop(ToolDialogResult.closedFailure))),
                    const SizedBox(width: Sp.s),
                    if (!isLive)
                      KeyedSubtree(
                          key: const ValueKey('tool-change-base'),
                          child: SizedBox(
                              width: 132,
                              child: PrimaryButton('换基准帧', height: 44, onPressed: () => _pop(ToolDialogResult.changeBase)))),
                  ]),
              ]),
            ),
          ),
        );
      },
    );
  }
}
