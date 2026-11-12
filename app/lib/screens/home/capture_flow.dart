import 'package:flutter/widgets.dart';

import '../../services/capture.dart';
import '../../state/app_state.dart';
import '../editor/editor_page.dart';

bool _busy = false;

/// Home camera button: permission -> capture screen -> ingest -> editor on the new photo.
/// Denied / blocked permission ends in a toast (blocked offers the system settings).
Future<void> captureAndEdit(BuildContext context, AppState app) async {
  if (_busy) return;
  _busy = true;
  try {
    final b = app.captureBackend;
    var perm = await b.permission();
    if (perm == CapturePerm.denied) perm = await b.permission(request: true);
    if (perm != CapturePerm.granted) {
      app.showToast('没有相机权限',
          actionLabel: perm == CapturePerm.blocked ? '设置' : '重试',
          onAction: () => perm == CapturePerm.blocked ? b.openSettings() : captureAndEdit(context, app));
      return;
    }
    CaptureResult? r;
    try {
      r = await b.capture();
    } catch (_) {
      app.showToast('无法打开相机');
      return;
    }
    if (r == null) return;
    final ph = await app.ingestCapture(r);
    if (context.mounted) openEditor(context, app, ph.id);
  } finally {
    _busy = false;
  }
}
