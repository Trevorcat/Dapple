import 'dart:io';

import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/services/ingest.dart';
import 'package:dapple/state/app_state.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'support/harness.dart';

PickedItem _new(String key, String name, Directory d) {
  final f = File('${d.path}/$name')..writeAsBytesSync([0xFF, 0xD8, 1, 2]);
  return PickedItem(key: key, name: name, file: IncomingFile(id: key, name: name, addedMs: 1, path: f.path, size: 4));
}

Future<AppState> _open(WidgetTester tester, List<PickedItem> Function(Directory) items, {DemoScenario sc = DemoScenario.normal, Size size = phoneSize}) async {
  useWindow(tester, size);
  final tmp = Directory.systemTemp.createTempSync('dapple_imp_');
  addTearDown(() {
    try {
      tmp.deleteSync(recursive: true);
    } catch (_) {} // Windows: the image cache may still hold a file
  });
  final app = AppState.demo(scenario: sc)..pickBackend = (s, known) async => items(tmp);
  await pumpDapple(tester, app);
  return app;
}

void main() {
  testWidgets('entry exists in the empty and the non-empty home screen, on phone and fold', (tester) async {
    for (final size in [phoneSize, foldSize]) {
      await _open(tester, (_) => [], sc: DemoScenario.empty, size: size);
      expect(find.byKey(const ValueKey('empty-import')), findsOneWidget);
      expect(find.text('从相册选择'), findsWidgets);
      expect(find.byKey(const ValueKey('empty-connect')), findsOneWidget);
      expect(find.byKey(const ValueKey('import-button')), findsOneWidget);
      await _open(tester, (_) => [], size: size);
      expect(find.byKey(const ValueKey('empty-import')), findsNothing);
      expect(find.byKey(const ValueKey('import-button')), findsOneWidget);
      expect(find.bySemanticsLabel('从相册选择'), findsOneWidget);
    }
  });

  testWidgets('exactly one pick opens the editor on it and it joins the list', (tester) async {
    final app = await _open(tester, (d) => [_new('imp:a|4', 'A.jpg', d)], sc: DemoScenario.empty);
    await tester.tap(find.byKey(const ValueKey('empty-import')));
    await tester.pump(const Duration(milliseconds: 100));
    await tester.pump(const Duration(milliseconds: 600));
    expect(app.photoById('imp:a|4'), isNotNull);
    expect(find.byType(EditorPage), findsOneWidget);
  });

  testWidgets('several picks stay on the list; a duplicate is not added twice', (tester) async {
    final app = await _open(tester, (d) => [_new('imp:a|4', 'A.jpg', d), _new('imp:b|4', 'B.jpg', d)]);
    final before = app.allPhotos.length;
    await tester.tap(find.byKey(const ValueKey('import-button')));
    await tester.pump(const Duration(milliseconds: 100));
    await tester.pump(const Duration(milliseconds: 600));
    expect(app.allPhotos.length, before + 2);
    expect(find.byType(EditorPage), findsNothing);
    expect(app.importing, isFalse);
    // the same two again: nothing new, plain message
    await app.importFromPicker();
    expect(app.allPhotos.length, before + 2);
    expect(app.toast?.message, '所选照片已经都在照片里');
  });

  testWidgets('one duplicate opens the existing photo; unsupported and failed files get a message', (tester) async {
    final app = await _open(tester, (d) => [_new('imp:a|4', 'A.jpg', d)]);
    expect(await app.importFromPicker(), 'imp:a|4');
    final n = app.allPhotos.length;
    expect(await app.importFromPicker(), 'imp:a|4');
    expect(app.allPhotos.length, n);
    expect(app.toast?.message, '这张已经在照片里');
    // an iOS scan id "ph:ASSET:0" is the same photo as the picker key "ph:ASSET"
    app.pickBackend = (s, k) async => [PickedItem(key: 'ph:XYZ', name: 'IMG', dup: true)];
    app.simulatePhotoForTest('ph:XYZ:0');
    expect(await app.importFromPicker(), 'ph:XYZ:0');
    app.pickBackend = (s, k) async => [
          PickedItem(key: 'imp:x', name: 'x.png', error: 'unsupported'),
          PickedItem(key: 'imp:y', name: 'y.dng', error: 'copy'),
        ];
    expect(await app.importFromPicker(), isNull);
    expect(app.toast?.message, '1 个文件不是支持的照片格式，已跳过；1 张没能读取，请再试一次');
  });
}
