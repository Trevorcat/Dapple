// B: a greyed dock category says why in one line; C: one-character edge labels on the pads and the first-use hint (once per install).
import 'package:dapple/model/pad_model.dart';
import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show settle, setView;
import 'support/harness.dart';

Future<AppState> _open(WidgetTester t, double w, double h, {bool hint = false, EGroup? group, AppState? use}) async {
  setView(t, Size(w * 3, h * 3), 3.0);
  await loadTestFonts();
  final app = use ?? AppState.demo();
  app.padHintSeen = !hint;
  final id = app.photos.firstWhere((p) => p.title == '外滩夕阳' && p.id.startsWith('d')).id;
  await t.pumpWidget(RepaintBoundary(
    key: shotKey,
    child: MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildTheme(Brightness.light),
      home: EditorPage(app: app, photoId: id, resamplePointers: false, initialGroup: group),
    ),
  ));
  await settle(t, 8);
  return app;
}

void main() {
  test('edge characters', () {
    expect([for (final w in PadCat.light.edgeWords) edgeChar(w)], ['暗', '亮', '低', '高']);
    expect([for (final w in PadCat.color.edgeWords) edgeChar(w)], ['冷', '暖', '淡', '浓']);
    expect([for (final w in PadCat.texture.edgeWords) edgeChar(w)], ['柔', '锐', '净', '粒']);
    expect([for (final w in PadCat.film.edgeWords) edgeChar(w)], ['淡', '浓', '暗', '亮']);
  });

  for (final (w, h) in [(412.0, 892.0), (360.0, 780.0)]) {
    testWidgets('C ${w.round()}: edge labels on the pad, hint once; shot', (t) async {
      final app = await _open(t, w, h, hint: true, group: EGroup.light);
      for (final c in ['暗', '亮', '低', '高']) {
        final f = find.byKey(ValueKey('pad-edge-${c == '低' ? '反差低' : c == '高' ? '反差高' : c}'));
        expect(f, findsOneWidget, reason: c);
        final txt = t.widget<Text>(f);
        expect(txt.style!.fontSize, greaterThanOrEqualTo(12));
        expect(txt.style!.fontWeight!.value, lessThanOrEqualTo(600));
      }
      expect(find.byKey(const ValueKey('pad-hint')), findsOneWidget);
      await shot(t, 'pad_hint_${w.round()}');
      // the first touch dismisses it, and it does not come back
      await t.tapAt(const Offset(200, 120));
      await settle(t, 3);
      expect(find.byKey(const ValueKey('pad-hint')), findsNothing);
      expect(app.padHintSeen, isTrue);
    });
  }

  testWidgets('C: once seen, the hint never shows again', (t) async {
    final app = AppState.demo();
    await _open(t, 412, 892, hint: true, group: EGroup.light, use: app);
    await t.tapAt(const Offset(200, 120));
    await settle(t, 3);
    await t.pumpWidget(const SizedBox());
    app.padHintSeen = true; // what the stored flag gives on the next launch
    await _open(t, 412, 892, group: EGroup.light, use: app);
    app.padHintSeen = true;
    expect(find.byKey(const ValueKey('pad-hint')), findsNothing);
  });

  testWidgets('B: tapping a greyed category says why; an ordinary one says nothing', (t) async {
    final app = await _open(t, 412, 892);
    // 局部 with no area is greyed
    await t.tap(find.byKey(ValueKey('dock-${EGroup.local}')));
    await settle(t, 3);
    expect(app.toast?.message, '还没有区域，点「+」新建');
    app.toast = null;
    await t.tap(find.byKey(ValueKey('dock-${EGroup.color}')));
    await settle(t, 3);
    // 颜色 is at its automatic value in the demo: either it says the plain "0" line or nothing, never the 局部 line
    expect(app.toast?.message, isNot('还没有区域，点「+」新建'));
  });
}
