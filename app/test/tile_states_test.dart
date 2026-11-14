import 'dart:io';

import 'package:dapple/models.dart';
import 'package:dapple/services/ingest_log.dart';
import 'package:dapple/state/app_state.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'support/harness.dart';

Future<AppState> _open(WidgetTester tester, List<Photo> photos) async {
  useWindow(tester, phoneSize);
  final app = AppState.demo();
  for (final p in photos) {
    app.simulatePhotoForTest(p);
  }
  await pumpDapple(tester, app);
  return app;
}

Photo _ph(String id, PhotoStatus st, {String? error}) {
  final p = Photo(id: id, name: '$id.jpg', addedMs: DateTime(2026, 11, 11, 16).millisecondsSinceEpoch, status: st, error: error);
  return p;
}

void main() {
  testWidgets('queued, developing and failed tiles are never anonymous', (tester) async {
    final app = await _open(tester, [
      _ph('q', PhotoStatus.pending),
      _ph('d', PhotoStatus.developing),
      _ph('f', PhotoStatus.failed, error: 'PlatformException(convert, cannot convert IMG_1.HEIC to JPEG)'),
    ]);
    await tester.pump(const Duration(milliseconds: 200));
    // guidelines 10.12 / audit F17: waiting = the hollow dot only, developing = the dot and the corner spinner; no word pill, no edge bar
    expect(find.text('等待处理'), findsNothing);
    expect(find.byKey(const ValueKey('progress-q')), findsNothing);
    expect(find.bySemanticsLabel(RegExp(r'^q，.*等待处理')), findsOneWidget);
    expect(find.bySemanticsLabel(RegExp(r'^d，.*处理中')), findsOneWidget);
    // a format the engine does not read: what happened on the amber pill, and no button (retrying cannot help, audit F06)
    expect(find.byKey(const ValueKey('reason-f')), findsOneWidget);
    expect(find.text('还不支持这种格式'), findsOneWidget);
    expect(find.byKey(const ValueKey('retry-f')), findsNothing);
    expect(find.byKey(const ValueKey('remove-f')), findsNothing);
    expect(app.photoById('f')!.failAction, FailAction.none);
  });

  testWidgets('a failure retrying can fix offers 重试; a gone original offers 移除 with 撤销', (tester) async {
    final app = await _open(tester, [
      _ph('n', PhotoStatus.failed, error: 'network: icloud download failed'),
      _ph('g', PhotoStatus.failed, error: 'asset not found: ph://x'),
    ]);
    await tester.pump(const Duration(milliseconds: 200));
    expect(find.text('读不到原图'), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey('retry-n')));
    await tester.pump(const Duration(milliseconds: 100));
    expect(app.photoById('n')!.status, isNot(PhotoStatus.failed));
    expect(find.text('原图不在了'), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey('remove-g')));
    await tester.pump(const Duration(milliseconds: 100));
    expect(app.photoById('g'), isNull);
    expect(app.toast?.actionLabel, '撤销');
    app.toast!.onAction!();
    expect(app.photoById('g'), isNotNull);
  });

  testWidgets('a failed develop is not reported as a broken camera transfer', (tester) async {
    final app = await _open(tester, [_ph('f', PhotoStatus.failed, error: 'asset not found')]);
    expect(app.connState, isNot(ConnState.interrupted));
    expect(app.interruptedCount, 0);
  });

  testWidgets('a done tile whose thumbnail file is gone is developed again, once', (tester) async {
    final p = _ph('g', PhotoStatus.done)..previewPath = '${Directory.systemTemp.path}/definitely_missing_preview.jpg';
    final app = await _open(tester, [p]);
    await tester.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 300)));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
    expect(app.photoById('g')!.status, PhotoStatus.pending);
    expect(app.photoById('g')!.previewPath, isNull);
  });

  test('plain failure reasons', () {
    // what happened, no guessed cause, no 请 (guidelines 13.1, audit C)
    expect(_ph('a', PhotoStatus.failed, error: 'asset not found: ph://x').failReason, '原图不在了');
    expect(_ph('a', PhotoStatus.failed, error: '找不到原始文件').failReason, '原图不在了');
    expect(_ph('a', PhotoStatus.failed, error: 'JPEG decode failed: bad').failReason, '文件已损坏');
    expect(_ph('a', PhotoStatus.failed, error: 'network timeout').failReason, '读不到原图');
    expect(_ph('a', PhotoStatus.failed, error: 'weird').failReason, '没处理成功');
    expect(_ph('a', PhotoStatus.failed, error: 'asset not found').failAction, FailAction.remove);
    expect(_ph('a', PhotoStatus.failed, error: 'network timeout').failAction, FailAction.retry);
    expect(_ph('a', PhotoStatus.failed, error: 'unsupported').failAction, FailAction.none);
  });

  test('log lines carry file names, never paths', () {
    expect(redactPaths('FAILED IMG_1.dng after 5 ms: /var/mobile/Containers/Data/Application/AB-12/Documents/inbox/IMG_1.dng missing'),
        'FAILED IMG_1.dng after 5 ms: IMG_1.dng missing');
    expect(redactPaths(r'open C:\Users\me\Pictures\a b.jpg'), contains('b.jpg'));
    expect(redactPaths('developed A.jpg 4000x3000 in 120 ms'), 'developed A.jpg 4000x3000 in 120 ms');
  });
}
