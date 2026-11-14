// Windows too low for photo + panel (flip cover 360x380, wide-fold cover 400x400): nothing overlaps - the photo keeps a minimum
// visible height, the panel gives way and stays under the top bar. The real design for these shapes is docs/design/foldables.md.
import 'package:dapple/screens/editor/editor_overlays.dart' show EditorTopBar;
import 'package:dapple/screens/editor/pad_panel.dart' show kMinPhotoH;
import 'package:dapple/screens/editor/pad_dock.dart' show padDockOrder;
import 'package:dapple/screens/publish/publish_done_page.dart';
import 'package:dapple/model/catalog.dart';
import 'package:dapple/screens/library/styles_page.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show open, settle;
import 'support/harness.dart';

void main() {
  for (final (name, w, h) in [('flip cover 360x380', 360.0, 380.0), ('wide-fold cover 400x400', 400.0, 400.0), ('phone landscape 915x412', 915.0, 412.0)]) {
    testWidgets('$name: the editor panel never covers the photo or the top bar', (t) async {
      await open(t, phys: Size(w * 3, h * 3), dpr: 3);
      for (final k in padDockOrder) {
        await t.tap(find.byKey(ValueKey('dock-$k')));
        await settle(t, 8);
        expect(t.takeException(), isNull, reason: '$k');
        final panel = t.getRect(find.byKey(const ValueKey('docked-panel')));
        final topBar = t.getRect(find.byType(EditorTopBar));
        final belowPhoto = panel.width > panel.height * 1.2 && panel.top > t.view.physicalSize.height / 3 / 2;
        if (belowPhoto) {
          expect(panel.top, greaterThanOrEqualTo(topBar.bottom - 1), reason: '$k: top bar clear of the panel header');
          expect(panel.top - topBar.bottom, greaterThanOrEqualTo(kMinPhotoH), reason: '$k: the photo keeps a minimum height');
        }
      }
    });
  }

  testWidgets('the publish done page scrolls instead of overflowing in a small window', (t) async {
    useWindow(t, const Size(360, 380), top: 24, bottom: 0);
    await loadTestFonts();
    final app = AppState.demo();
    await t.pumpWidget(MaterialApp(
      theme: buildTheme(Brightness.light),
      home: PublishDonePage(app: app, preset: presets.first, count: 3, ratio: '4:5', size: 2048, fallback: true, nextBatch: 2),
    ));
    await t.pump(const Duration(milliseconds: 300));
    expect(t.takeException(), isNull);
    expect(find.byType(SingleChildScrollView), findsOneWidget);
    await t.drag(find.byType(SingleChildScrollView), const Offset(0, -300));
    await t.pump(const Duration(milliseconds: 300));
    expect(find.byKey(const ValueKey('done-finish')), findsOneWidget);
    expect(t.takeException(), isNull);
  });

  testWidgets('默认成像 in phone landscape: sample card beside the choices, scrolls, no overflow', (t) async {
    for (final size in [const Size(915, 412), const Size(800, 500)]) {
      useWindow(t, size, top: 0, bottom: 0);
      await loadTestFonts();
      final app = AppState.demo();
      app.tab = 1;
      await t.pumpWidget(MaterialApp(theme: buildTheme(Brightness.light), home: Scaffold(body: StylesPage(app: app, wide: true))));
      await t.pump(const Duration(milliseconds: 300));
      expect(t.takeException(), isNull, reason: '$size');
      expect(find.byKey(const ValueKey('default-card')), findsOneWidget);
      expect(find.byType(SingleChildScrollView), findsWidgets);
    }
  });
}
