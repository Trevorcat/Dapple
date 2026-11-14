// Design audit 2026-11, the round that needed no owner decision: the action card hugs its chips, the portrait chips are in two groups, the
// undo toast is a quiet capsule on the photo that ends with the tab, the settings reset can be undone, the selection mode is quiet and keeps
// 合成 in the capsule. (The shape card groups, the home cards, the publish 更多 row and the merge tool changes are asserted in the tests of
// those screens.)
import 'package:dapple/models.dart';
import 'package:dapple/screens/editor/pad_panel.dart' show kPanelFixedH;
import 'package:dapple/screens/library/settings_page.dart';
import 'package:dapple/services/merge_engine.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show phoneDpr, phonePhys, setView, settle;
import 'local_ai_test.dart' show FakeLocalAi;
import 'local_panel_test.dart' show addViaCard, key, openLocal, padDockOrderForTest, tapKey;
import 'local_portrait_test.dart' show openPortrait, portraitResult;
import 'support/harness.dart';

class _MergeBuild implements MergeEngine {
  @override
  bool get available => true;
  @override
  dynamic noSuchMethod(Invocation i) => super.noSuchMethod(i);
}

double _opacityOf(WidgetTester t, String k) {
  final o = find.descendant(of: find.byKey(ValueKey(k)), matching: find.byType(Opacity));
  return t.widget<Opacity>(o.first).opacity;
}

void main() {
  group('the action card', () {
    testWidgets('hangs under the chips and the panel is as tall as its tiles (no empty band), tiles at least 56 dp, word 12', (t) async {
      await openLocal(t);
      await addViaCard(t, 'radial');
      final padPanel = t.getSize(key('docked-panel')).height;
      await tapKey(t, 'local-chip-0');
      expect(key('act-删除'), findsOneWidget);
      await t.pump(const Duration(seconds: 2)); // the height change is animated
      final panel = t.getSize(key('docked-panel')).height;
      expect(panel, lessThan(padPanel - 40), reason: 'the panel shrank with its content: the photo gets the room back');
      final chips = t.getRect(key('local-chip-0'));
      final tile = t.getRect(key('act-加一块'));
      expect(tile.top - chips.bottom, lessThan(24), reason: 'the tiles follow the chip row: no band where the pad was');
      expect(t.getSize(key('act-删除')).height, greaterThanOrEqualTo(56));
      expect(t.getSize(key('act-删除')).width, greaterThanOrEqualTo(56));
      final label = t.widget<Text>(find.descendant(of: key('act-删除'), matching: find.text('删除')));
      expect(label.style!.fontSize, greaterThanOrEqualTo(12));
      expect(panel, greaterThan(kPanelFixedH));
      expect(t.takeException(), isNull);
      await tapKey(t, 'act-反选');
      expect(t.takeException(), isNull);
    });
  });

  group('portrait chips', () {
    testWidgets('two groups under small titles: 脸部 and 五官', (t) async {
      final fake = FakeLocalAi();
      await openPortrait(t, fake, portraitResult());
      expect(key('portrait-group-脸部'), findsOneWidget);
      expect(key('portrait-group-五官'), findsOneWidget);
      final faceY = t.getCenter(key('portrait-group-脸部')).dy, featY = t.getCenter(key('portrait-group-五官')).dy;
      expect(featY, greaterThan(faceY));
      for (final k in ['skin', 'undereye', 'shine']) {
        final y = t.getCenter(key('chip-$k')).dy;
        expect(y, greaterThan(faceY), reason: k);
        expect(y, lessThan(featY), reason: '$k is in 脸部');
      }
      for (final k in ['eyes', 'lips', 'teeth']) {
        expect(t.getCenter(key('chip-$k')).dy, greaterThan(featY), reason: '$k is in 五官');
      }
      expect(t.takeException(), isNull);
    });
  });

  group('the undo toast', () {
    testWidgets('a quiet capsule on the photo, above the panel; a tab switch ends it', (t) async {
      final r = await openLocal(t);
      r.app.showToast('已复位光线', actionLabel: '撤销', onAction: () {});
      await settle(t, 3);
      expect(find.text('已复位光线'), findsOneWidget);
      final box = find.ancestor(of: find.text('已复位光线'), matching: find.byType(Container)).first;
      final panel = t.getRect(key('docked-panel'));
      expect(t.getRect(box).bottom, lessThanOrEqualTo(panel.top + 1), reason: 'never over the pad, the dock or the join of photo and panel');
      // the capsule's surface and a hairline, not the hard dark bar
      final deco = t.widget<Container>(box).decoration! as BoxDecoration;
      expect(deco.color, isNot(equals(const Color(0xFF313033))));
      expect(deco.border, isNotNull);
      // the first dock item that is not 局部: another tab
      await tapKey(t, 'dock-${padDockOrderForTest.first}');
      await settle(t, 3);
      expect(r.app.toast, isNull, reason: 'it was about the page the user left');
      expect(find.text('已复位光线'), findsNothing);
    });

    test('an undo toast ends within four seconds', () {
      expect(Toast('x', actionLabel: '撤销').duration, lessThanOrEqualTo(const Duration(seconds: 4)));
    });
  });

  group('settings: 重置自动判断', () {
    testWidgets('says what it does, and 撤销 puts the learned values back', (t) async {
      setView(t, phonePhys, phoneDpr);
      await loadTestFonts();
      final app = AppState.demo();
      app.settings.handSide = 2;
      app.settings.editorLayout = 'learned';
      await t.pumpWidget(MaterialApp(theme: buildTheme(Brightness.light), home: Scaffold(body: SettingsPage(app: app, wide: false))));
      await t.pump(const Duration(milliseconds: 200));
      final row = find.byKey(const ValueKey('reset-hand'));
      await t.scrollUntilVisible(row, 200, scrollable: find.byType(Scrollable).first);
      expect(find.text('清除已学到的左右手习惯，重新判断'), findsOneWidget);
      await t.tap(row);
      await t.pump(const Duration(milliseconds: 100));
      expect(app.settings.handSide, 1);
      expect(app.settings.editorLayout, '');
      expect(app.toast?.actionLabel, '撤销');
      app.toast!.onAction!();
      await t.pump(const Duration(milliseconds: 100));
      expect(app.settings.handSide, 2);
      expect(app.settings.editorLayout, 'learned');
    });
  });

  group('selection mode', () {
    testWidgets('the connection card is one quiet line; 合成 is always in the capsule and says why it waits', (t) async {
      useWindow(t, phoneSize, bottom: 24);
      final app = AppState.demo()..engine = _MergeBuild();
      await pumpDapple(t, app);
      expect(key('conn-card'), findsOneWidget);
      final done = app.photos.firstWhere((p) => p.status == PhotoStatus.done && p.kind == PhotoKind.photo);
      await t.longPress(key('tile-${done.id}'));
      await t.pump(const Duration(milliseconds: 100));
      expect(app.selectMode, isTrue);
      expect(key('conn-card'), findsNothing);
      expect(key('conn-card-compact'), findsOneWidget);
      expect(t.getSize(key('conn-card-compact')).height, lessThan(48));
      // one photo selected: 合成 is there, drawn off, and the tap says why
      expect(key('sel-compose'), findsOneWidget);
      expect(_opacityOf(t, 'sel-compose'), lessThan(1));
      await t.tap(key('sel-compose'));
      await t.pump();
      expect(app.toast?.message, '合成至少选 2 张');
      expect(find.text('合成 1 张'), findsNothing);
    });
  });
}
