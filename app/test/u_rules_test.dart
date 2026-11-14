// The unification rules as checks (docs/design/guidelines.md v0.7; audit 2026-11 U1, U6, U8): every panel with a strength strip shows
// "强度 N%" in the header's readout place, and the strip's screen-reader value says the same number; every header's title is the dock
// item's name.
import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show settle;
import 'local_panel_test.dart' show addViaCard, openLocal;
import 'support/harness.dart';

Future<void> _open(WidgetTester t, EGroup g) async {
  useWindow(t, const Size(412, 945), top: 36, bottom: 24);
  await loadTestFonts();
  final app = AppState.demo();
  final id = app.photos.firstWhere((p) => p.title == '外滩夕阳' && p.id.startsWith('d')).id;
  await t.pumpWidget(MaterialApp(theme: buildTheme(Brightness.light), home: EditorPage(app: app, photoId: id, resamplePointers: false, initialGroup: g)));
  await settle(t, 8);
}

String _readout(WidgetTester t) => (t.widget<Text>(find.byKey(const ValueKey('header-readout')))).data!;

/// The strip's semantics value ("100%").
String _stripValue(WidgetTester t) {
  final strip = find.byWidgetPredicate((w) => w is Semantics && w.properties.slider == true && (w.properties.label ?? '').contains('强度'));
  return t.widget<Semantics>(strip.first).properties.value!;
}

void main() {
  const titles = {EGroup.light: '光线', EGroup.color: '颜色', EGroup.style: '成像', EGroup.effects: '质地'};
  for (final e in titles.entries) {
    testWidgets('U1 / U6 ${e.value}: the title is the dock name, the readout is 强度 N% and the strip says the same', (t) async {
      await _open(t, e.key);
      expect((t.widget<Text>(find.byKey(const ValueKey('header-title')))).data, e.value);
      final r = _readout(t);
      expect(r, startsWith('强度 '));
      final pct = RegExp(r'强度 (\d+)%').firstMatch(r)!.group(1);
      expect(_stripValue(t), '$pct%');
      expect(find.text(e.value), findsWidgets, reason: 'the dock item has the same name');
    });
  }

  testWidgets('U1 / U6 局部: the title stays 局部, the readout and the strip agree', (t) async {
    await openLocal(t);
    await addViaCard(t, 'radial');
    expect((t.widget<Text>(find.byKey(const ValueKey('header-title')))).data, '局部');
    final r = _readout(t);
    final pct = RegExp(r'强度 (\d+)%').firstMatch(r)!.group(1);
    expect(_stripValue(t), '$pct%');
  });
}
