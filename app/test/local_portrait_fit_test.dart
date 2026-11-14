// 局部 on a portrait-orientation photo on small phones: the shape card's three groups (the last one is 按内容选) must fit the panel or
// scroll inside it, never be pushed off the screen.
import 'dart:io';
import 'dart:ui' as ui;

import 'package:dapple/editor/fold_layout.dart' show debugFoldLayouts;
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';

import 'local_ai_test.dart' show FakeLocalAi;
import 'local_panel_test.dart' show key, openLocal;

void main() {
  setUp(() => debugFoldLayouts = false);
  tearDown(() => debugFoldLayouts = true);
  const phones = <(String, double, double)>[
    ('375x667', 375, 667),
    ('375x812', 375, 812),
    ('414x896', 414, 896),
    ('360x780', 360, 780),
  ];
  for (final (name, w, h) in phones) {
    for (final tall in [true, false]) {
      testWidgets('$name portrait photo ${tall ? '3:4' : '9:16'}: the 按内容选 row is on screen or reachable by scrolling in the panel', (t) async {
        const dpr = 3.0;
        // iPhone-like insets: status bar 44-47, home indicator 34
        final r = await openLocal(t, phys: Size(w * dpr, h * dpr), dpr: dpr, top: h >= 812 ? 47 : 20, bottom: h >= 812 ? 34 : 0, ai: FakeLocalAi(),
            before: (app, id) => app.applyEdit(id, (e) => e.copyWith(crop: tall ? '3:4' : '9:16')));
        expect(r.lc.list.isEmpty, isTrue);
        expect(t.takeException(), isNull);
        final screen = Offset.zero & Size(w, h);
        final title = key('kind-group-content');
        expect(title, findsOneWidget);
        await t.runAsync(() async {
          final b = t.renderObject<RenderRepaintBoundary>(find.byType(RepaintBoundary).first);
          final img = await b.toImage(pixelRatio: 1.5);
          final bytes = await img.toByteData(format: ui.ImageByteFormat.png);
          final f = File('build/local-fit/$name-${tall ? 'tall' : 'wide'}.png')..parent.createSync(recursive: true);
          f.writeAsBytesSync(bytes!.buffer.asUint8List());
        });
        await t.ensureVisible(find.byKey(const ValueKey('kind-sky')));
        await t.pump(const Duration(milliseconds: 300));
        final tile = t.getRect(find.byKey(const ValueKey('kind-sky')));
        expect(screen.contains(tile.center), isTrue, reason: 'sky tile $tile on $screen');
        expect(find.byKey(const ValueKey('kind-sky')).hitTestable(), findsOneWidget);
        // the photo keeps a usable height
        final stage = t.getRect(key('local-handles'));
        expect(stage.height, greaterThan(100), reason: 'the photo stays usable');
      });
    }
  }
}
