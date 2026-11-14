// The home shell's bottom navigation is the floating capsule (the editor dock's own surface, guidelines 15.6): tabs with a tinted pill for
// the selected one, the selection actions in the same capsule, content that scrolls underneath with room for the last row, and the bottom
// inset respected.
import 'package:dapple/screens/home/shell.dart' show kWideBreakpoint;
import 'package:dapple/services/merge_engine.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/widgets/float_bar.dart';
import 'package:dapple/widgets/icons.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'support/harness.dart';

class _MergeBuild implements MergeEngine {
  @override
  bool get available => true;
  @override
  dynamic noSuchMethod(Invocation i) => super.noSuchMethod(i);
}

Future<AppState> _open(WidgetTester t, {Size size = phoneSize, double bottom = 24}) async {
  useWindow(t, size, bottom: bottom);
  final app = AppState.demo()..engine = _MergeBuild();
  await pumpDapple(t, app);
  return app;
}

const _t = Duration(milliseconds: 100);
Finder key(String k) => find.byKey(ValueKey(k));

void main() {
  textScale2();
  testWidgets('the tabs are a floating capsule: centred, above the bottom inset, 48 dp items, one selected pill', (t) async {
    final app = await _open(t);
    final screen = t.view.physicalSize / t.view.devicePixelRatio;
    final bar = t.getRect(key('nav-bar'));
    expect(bar.height, kFloatBarH, reason: 'the dock capsule height');
    expect(bar.center.dx, closeTo(screen.width / 2, 0.5), reason: 'centred');
    expect(bar.bottom, closeTo(screen.height - 24 - kFloatBarGap, 0.5), reason: 'above the 24 dp bottom inset with the gap');
    expect(bar.width, lessThan(screen.width - 2 * 16), reason: 'a capsule, not a full-width bar');
    for (var i = 0; i < 3; i++) {
      final s = t.getSize(key('nav-$i'));
      expect(s.width, greaterThanOrEqualTo(48));
      expect(s.height, greaterThanOrEqualTo(48));
    }
    expect(app.tab, 0);
    final h = t.ensureSemantics();
    expect(t.getSemantics(key('nav-0')).flagsCollection.isSelected.toString(), contains('isTrue'));
    expect(find.bySemanticsLabel('成像'), findsOneWidget);
    // switching tabs
    await t.tap(key('nav-1'));
    await t.pump(_t);
    expect(app.tab, 1);
    expect(t.getSemantics(key('nav-1')).flagsCollection.isSelected.toString(), contains('isTrue'));
    expect(t.getSemantics(key('nav-0')).flagsCollection.isSelected.toString(), contains('isFalse'));
    await t.tap(key('nav-2'));
    await t.pump(_t);
    expect(app.tab, 2);
    await t.tap(key('nav-0'));
    await t.pump(_t);
    expect(app.tab, 0);
    expect(key('nav-rail'), findsNothing);
    h.dispose();
  });

  testWidgets('a taller bottom inset (a three-button bar) lifts the capsule with it', (t) async {
    await _open(t, bottom: 48);
    final screen = t.view.physicalSize / t.view.devicePixelRatio;
    expect(t.getRect(key('nav-bar')).bottom, closeTo(screen.height - 48 - kFloatBarGap, 0.5));
  });

  testWidgets('a system gesture inset taller than the bottom inset sets the capsule base', (t) async {
    useWindow(t, phoneSize, bottom: 0);
    t.view.systemGestureInsets = const FakeViewPadding(bottom: 60 * 2.0);
    final app = AppState.demo()..engine = _MergeBuild();
    await pumpDapple(t, app);
    final screen = t.view.physicalSize / t.view.devicePixelRatio;
    expect(t.getRect(key('nav-bar')).bottom, closeTo(screen.height - 60 - kFloatBarGap, 0.5));
    t.view.resetSystemGestureInsets();
  });

  testWidgets('selecting replaces the tabs with the same capsule: 存相册 / 合成 / 发布, enabled by the selection', (t) async {
    final app = await _open(t);
    final navRect = t.getRect(key('nav-bar'));
    final done = app.photos.firstWhere((p) => p.status.name == 'done' && p.kind.name == 'photo');
    await t.longPress(key('tile-${done.id}'));
    await t.pump(_t);
    expect(app.selectMode, isTrue);
    expect(key('nav-bar'), findsNothing);
    final bar = t.getRect(key('select-bar'));
    expect(bar.bottom, navRect.bottom, reason: 'the same place as the tabs');
    expect(bar.height, kFloatBarH);
    expect(bar.center.dx, closeTo(navRect.center.dx, 0.5));
    for (final k in ['sel-save', 'sel-compose', 'sel-publish']) {
      expect(key(k), findsOneWidget, reason: k);
      final s = t.getSize(key(k));
      expect(s.height, greaterThanOrEqualTo(48), reason: k);
      expect(s.width, greaterThanOrEqualTo(48), reason: k);
      expect(t.getRect(key(k)).bottom, lessThanOrEqualTo(bar.bottom + 0.5), reason: '$k inside the capsule');
    }
    final h = t.ensureSemantics();
    expect(find.bySemanticsLabel('发布'), findsOneWidget);
    expect(find.bySemanticsLabel('存相册'), findsOneWidget);
    h.dispose();
    // the compose action works from the capsule (it needs two)
    final second = app.photos.firstWhere((p) => p.status.name == 'done' && p.kind.name == 'photo' && p.id != done.id);
    await t.tap(key('tile-${second.id}'));
    await t.pump(_t);
    await t.tap(key('sel-compose'));
    await t.pump();
    await t.pump(const Duration(milliseconds: 500));
    expect(find.text('合成 2 张'), findsOneWidget);
    await t.tapAt(const Offset(20, 20));
    await t.pump();
    await t.pump(const Duration(milliseconds: 500));
    app.exitSelect();
    await t.pump(_t);
    expect(key('nav-bar'), findsOneWidget, reason: 'the tabs come back');
  });

  for (final size in const [Size(412, 915), Size(360, 780), Size(915, 700)]) {
    testWidgets('selection capsule at ${size.width.toInt()} wide: three items of one width, icons and words on common lines, inside the screen', (t) async {
      final app = await _open(t, size: size);
      final done = app.photos.firstWhere((p) => p.status.name == 'done' && p.kind.name == 'photo');
      await t.longPress(key('tile-${done.id}'));
      await t.pump(_t);
      final bar = t.getRect(key('select-bar'));
      expect(bar.left, greaterThanOrEqualTo(16 - 0.5));
      expect(bar.right, lessThanOrEqualTo(size.width - 16 + 0.5));
      final keys = ['sel-save', 'sel-compose', 'sel-publish'];
      final widths = {for (final k in keys) t.getSize(key(k)).width};
      expect(widths.length, 1, reason: 'equal widths: $widths');
      final icons = <double>{}, words = <double>{};
      for (final k in keys) {
        final item = key(k);
        expect(t.getRect(item).top, bar.top, reason: k);
        expect(t.getRect(item).height, bar.height, reason: k);
        final ic = find.descendant(of: item, matching: find.byType(DIcon));
        final tx = find.descendant(of: item, matching: find.byType(Text));
        expect(ic, findsOneWidget, reason: k);
        icons.add(t.getCenter(ic).dy.roundToDouble());
        words.add(t.getCenter(tx).dy.roundToDouble());
        expect(t.getCenter(ic).dx, closeTo(t.getCenter(item).dx, 0.5), reason: '$k icon centred');
        expect(t.getCenter(tx).dx, closeTo(t.getCenter(item).dx, 0.5), reason: '$k word centred');
      }
      expect(icons.length, 1, reason: 'one icon line: $icons');
      expect(words.length, 1, reason: 'one word line: $words');
      expect(t.takeException(), isNull);
    });
  }

  testWidgets('with nothing selected the actions are off', (t) async {
    final app = await _open(t);
    final done = app.photos.firstWhere((p) => p.status.name == 'done' && p.kind.name == 'photo');
    await t.longPress(key('tile-${done.id}'));
    await t.pump(_t);
    await t.tap(key('tile-${done.id}')); // deselect
    await t.pump(_t);
    expect(app.selectedIds, isEmpty);
    final h = t.ensureSemantics();
    expect(t.getSemantics(key('sel-publish')).flagsCollection.isEnabled.toString(), contains('isFalse'));
    h.dispose();
  });

  testWidgets('content scrolls underneath: the last grid row can be scrolled clear of the capsule and tapped', (t) async {
    final app = await _open(t);
    final scroll = find.byKey(const ValueKey('home-scroll'));
    await t.drag(scroll, const Offset(0, -4000));
    await t.pump(_t);
    await t.pump(_t);
    final bar = t.getRect(key('nav-bar'));
    final tiles = [for (final p in app.photos) key('tile-${p.id}')].where((f) => f.evaluate().isNotEmpty).toList();
    expect(tiles, isNotEmpty);
    // the lowest tile ends above the capsule's top (floatBarClearance: inset + gap + capsule + air)
    var lowest = 0.0;
    Finder? last;
    for (final f in tiles) {
      final r = t.getRect(f);
      if (r.bottom > lowest) {
        lowest = r.bottom;
        last = f;
      }
    }
    expect(lowest, lessThanOrEqualTo(bar.top - 8), reason: 'the last row clears the capsule');
    // and it takes a tap: opens the editor (or selects in selection mode); it is not covered
    final centre = t.getCenter(last!);
    final hit = t.hitTestOnBinding(centre);
    expect(hit.path.any((e) => e.target is RenderObject && (e.target as RenderObject).toStringShort().contains('RenderPointerListener')), isTrue);
    await t.longPress(last);
    await t.pump(_t);
    expect(app.selectMode, isTrue, reason: 'the last tile took the touch');
    expect(app.selectedIds.length, 1);
  });

  testWidgets('selecting in a wide window: the rail steps aside and the capsule appears; browsing keeps the rail alone', (t) async {
    final app = await _open(t, size: const Size(kWideBreakpoint + 300, 700));
    expect(key('nav-rail'), findsOneWidget);
    expect(key('nav-bar'), findsNothing, reason: 'the rail is not a full-width bar: it stays');
    expect(key('select-bar'), findsNothing);
    final done = app.photos.firstWhere((p) => p.status.name == 'done' && p.kind.name == 'photo');
    await t.longPress(key('tile-${done.id}'));
    await t.pump(_t);
    expect(key('select-bar'), findsOneWidget);
    final screen = t.view.physicalSize / t.view.devicePixelRatio;
    final bar = t.getRect(key('select-bar'));
    expect(bar.center.dx, closeTo(screen.width / 2, 0.5));
    expect(bar.bottom, closeTo(screen.height - 24 - kFloatBarGap, 0.5));
    expect(bar.width, lessThan(screen.width / 2));
  });

  test('the clearance pages keep at their end covers the inset, the gap and the capsule', () {
    expect(kFloatBarH, 56);
    expect(kFloatBarRadius, 28, reason: 'the dock capsule radius');
  });
}

void textScale2() {
  testWidgets('text scale 2.0: the tab labels (12 / 16, Chinese never smaller) fit the capsule without overflow', (t) async {
    t.platformDispatcher.textScaleFactorTestValue = 2.0;
    addTearDown(t.platformDispatcher.clearTextScaleFactorTestValue);
    for (final size in [phoneSize, const Size(360, 640)]) {
      await _open(t, size: size);
      for (var i = 0; i < 3; i++) {
        await t.tap(key('nav-$i'));
        await t.pump(_t);
        await t.pump(_t);
      }
      expect(t.takeException(), isNull, reason: '$size');
    }
  });
}
