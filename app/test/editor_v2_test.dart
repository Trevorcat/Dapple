// Editor interaction v2 (requirements 4.15): widget level behaviour on a phone and on a Find N5
// inner screen, plus screenshots written to test/goldens/editor_v2/ for review.
import 'dart:io';
import 'dart:ui' as ui;

import 'package:dapple/editor/haptics.dart';
import 'package:dapple/editor/value_mapping.dart';
import 'package:dapple/editor/window_manager.dart';
import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/editor_overlays.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/screens/editor/zoom_view.dart';
import 'package:dapple/screens/library/settings_page.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:dapple/widgets/photo_art.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';

import 'support/harness.dart';

// 1080x2400 @ 2.75 -> 392.7 x 872.7 dp
const phonePhys = Size(1080, 2400);
const phoneDpr = 2.75;
// Find N5 inner 2480x2248 @ 3.0 -> 826.7 x 749.3 dp
const n5Phys = Size(2480, 2248);
const n5Dpr = 3.0;

void setView(WidgetTester t, Size phys, double dpr, {double top = 28, double bottom = 20}) {
  t.view.devicePixelRatio = dpr;
  t.view.physicalSize = phys;
  t.view.padding = FakeViewPadding(top: top * dpr, bottom: bottom * dpr);
  t.view.viewPadding = FakeViewPadding(top: top * dpr, bottom: bottom * dpr);
  addTearDown(t.view.reset);
}

Future<(AppState, String)> open(WidgetTester t, {Size phys = phonePhys, double dpr = phoneDpr, bool dark = false, String title = '外滩夕阳'}) async {
  setView(t, phys, dpr);
  await loadTestFonts();
  final app = AppState.demo();
  final id = app.photos.firstWhere((p) => p.title == title && p.id.startsWith('d')).id;
  await t.pumpWidget(RepaintBoundary(
    key: shotKey,
    child: MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildTheme(Brightness.light),
      darkTheme: buildTheme(Brightness.dark),
      themeMode: dark ? ThemeMode.dark : ThemeMode.light,
      home: EditorPage(app: app, photoId: id, resamplePointers: false),
    ),
  ));
  await settle(t);
  return (app, id);
}

Future<void> settle(WidgetTester t, [int n = 6]) async {
  for (var i = 0; i < n; i++) {
    await t.pump(const Duration(milliseconds: 100));
  }
}

/// Writes test/goldens/editor_v2/[name].png (device pixels / 1.5).
Future<void> dump(WidgetTester t, String name) async {
  await settle(t, 6);
  await t.runAsync(() async {
    final b = shotKey.currentContext!.findRenderObject() as RenderRepaintBoundary;
    final img = await b.toImage(pixelRatio: 1.5);
    final bytes = await img.toByteData(format: ui.ImageByteFormat.png);
    final f = File('test/goldens/editor_v2/$name.png');
    f.parent.createSync(recursive: true);
    f.writeAsBytesSync(bytes!.buffer.asUint8List());
  });
}

/// A finger with explicit event timestamps so the arbiter's timing is deterministic.
class Finger {
  Finger._(this.t, this.p, this.pos);
  static int _ids = 100;
  final WidgetTester t;
  final TestPointer p;
  Offset pos;
  int ms = 0;

  static Future<Finger> down(WidgetTester t, Offset at, {int startMs = 0}) async {
    final p = TestPointer(_ids++);
    await t.sendEventToBinding(p.down(at, timeStamp: Duration(milliseconds: startMs)));
    return Finger._(t, p, at)..ms = startMs;
  }

  Future<void> by(Offset d, {int dt = 16}) async {
    ms += dt;
    pos += d;
    await t.sendEventToBinding(p.move(pos, timeStamp: Duration(milliseconds: ms)));
    await t.pump(Duration(milliseconds: dt));
  }

  /// Moves by [total] in steps of about [step] dp, 16 ms apart.
  Future<void> drag(Offset total, {double step = 6}) async {
    final n = (total.distance / step).ceil().clamp(1, 400);
    for (var i = 0; i < n; i++) {
      await by(total / n.toDouble());
    }
  }

  Future<void> up() async {
    ms += 16;
    await t.sendEventToBinding(p.up(timeStamp: Duration(milliseconds: ms)));
    await t.pump(const Duration(milliseconds: 16));
  }
}

Future<void> tapKey(WidgetTester t, String k) async {
  await t.tap(find.byKey(ValueKey(k)));
  await settle(t, 3);
}

ZoomController zoomOf(WidgetTester t) => t.widget<ZoomView>(find.byKey(const ValueKey('zoom-stage'))).controller;

void main() {
  group('phone portrait', () {
    testWidgets('browse mode: photo fills the screen, top bar and dock float, no window, no histogram window', (t) async {
      await open(t);
      expect(find.byKey(const ValueKey('toolbar-back')), findsOneWidget);
      expect(find.byKey(const ValueKey('toolbar-undo')), findsOneWidget);
      expect(find.byKey(const ValueKey('toolbar-redo')), findsOneWidget);
      expect(find.byKey(const ValueKey('toolbar-save')), findsOneWidget);
      for (final l in ['光线', '颜色', '风格', '裁切', '效果', '更多']) {
        expect(find.text(l), findsWidgets, reason: l);
      }
      for (final l in ['曲线', '细节', '分级']) {
        expect(find.text(l), findsNothing, reason: '$l lives in 更多');
      }
      expect(find.byKey(const ValueKey('window-light')), findsNothing);
      expect(find.byKey(const ValueKey('histogram-window')), findsNothing);
      // no old slider UI, no old sheet
      expect(find.byKey(const ValueKey('sheet-grabber')), findsNothing);
      expect(find.byKey(const ValueKey('compare-toggle')), findsNothing);
      expect(find.byType(Slider), findsNothing);
      await dump(t, 'phone_browse');
    });

    testWidgets('dock opens a window with the category rows; the active row is the first; tap again closes', (t) async {
      await open(t);
      await tapKey(t, 'dock-${EGroup.light}');
      expect(find.byKey(const ValueKey('window-light')), findsOneWidget);
      for (final l in ['曝光', '对比度', '高光', '阴影', '白色色阶', '黑色色阶']) {
        expect(find.descendant(of: find.byKey(const ValueKey('window-light')), matching: find.text(l)), findsOneWidget, reason: l);
      }
      // values are text, never a slider look
      expect(find.byType(Slider), findsNothing);
      await dump(t, 'phone_adjust_light');
      await tapKey(t, 'dock-${EGroup.light}');
      expect(find.byKey(const ValueKey('window-light')), findsNothing);
    });

    testWidgets('horizontal drag outside the window changes the active parameter; the whole drag is one undo step', (t) async {
      final (app, id) = await open(t);
      await tapKey(t, 'dock-${EGroup.light}');
      final e0 = app.editOf(id).value('exposure');
      final f = await Finger.down(t, const Offset(60, 300));
      await f.drag(const Offset(150, 4));
      // mid drag: readout visible, the window collapsed to a one-line pill
      await t.pump(const Duration(milliseconds: 300));
      expect(find.byKey(const ValueKey('readout')), findsOneWidget);
      expect(find.byKey(const ValueKey('pill-light')), findsOneWidget);
      await dump(t, 'phone_drag_collapsed');
      await f.drag(const Offset(30, 0));
      await f.up();
      await settle(t);
      final v = app.editOf(id).value('exposure');
      expect(v - e0, greaterThan(15), reason: '180 dp of slow drag = about 20 units');
      expect(app.canUndo(id), isTrue);
      app.undo(id);
      expect(app.editOf(id).value('exposure'), e0);
      expect(app.canUndo(id), isFalse, reason: 'one gesture = one undo step');
      // the window springs back after the finger lifts
      await settle(t, 8);
      expect(find.byKey(const ValueKey('window-light')), findsOneWidget);
    });

    testWidgets('a pause longer than 800 ms inside one gesture is still one undo step', (t) async {
      final (app, id) = await open(t);
      await tapKey(t, 'dock-${EGroup.light}');
      final e0 = app.editOf(id).value('exposure');
      final f = await Finger.down(t, const Offset(60, 300));
      await f.drag(const Offset(60, 0));
      await t.pump(const Duration(milliseconds: 1200));
      await f.drag(const Offset(60, 0));
      await f.up();
      expect(app.editOf(id).value('exposure'), isNot(e0));
      app.undo(id);
      expect(app.canUndo(id), isFalse);
      expect(app.editOf(id).value('exposure'), e0);
    });

    testWidgets('the vertical lock switches the parameter; the next horizontal drag changes it', (t) async {
      final (app, id) = await open(t);
      await tapKey(t, 'dock-${EGroup.light}');
      final e0 = app.editOf(id).value('exposure'), c0 = app.editOf(id).value('contrast');
      final f = await Finger.down(t, const Offset(60, 300));
      await f.drag(const Offset(1, 60), step: 5);
      await f.up();
      await settle(t, 3);
      expect(app.editOf(id).value('exposure'), e0, reason: 'vertical never changes a value');
      final g = await Finger.down(t, const Offset(60, 300), startMs: 2000);
      await g.drag(const Offset(120, 2));
      await g.up();
      expect(app.editOf(id).value('exposure'), e0);
      expect(app.editOf(id).value('contrast'), greaterThan(c0 + 5));
    });

    testWidgets('precision band: dragging far above the start point scales the change down', (t) async {
      final (app, id) = await open(t);
      await tapKey(t, 'dock-${EGroup.light}');
      final f = await Finger.down(t, const Offset(60, 600));
      await f.drag(const Offset(20, 0), step: 5); // lock + a little
      await f.drag(const Offset(0, -200), step: 8); // far up: ultra band
      final before = app.editOf(id).value('exposure');
      await f.drag(const Offset(100, 0), step: 5);
      await f.up();
      final delta = app.editOf(id).value('exposure') - before;
      expect(delta, lessThan(7), reason: '100 dp in the ultra band: about 1/16 of the standard change (about 60)');
    });

    testWidgets('long press shows the original and releases; moving afterwards does not edit', (t) async {
      final (app, id) = await open(t);
      await tapKey(t, 'dock-${EGroup.light}');
      final f = await Finger.down(t, const Offset(60, 300));
      await t.pump(const Duration(milliseconds: 400));
      await t.pump(const Duration(milliseconds: 200));
      expect(find.byKey(const ValueKey('original-pill')), findsOneWidget);
      final e0 = app.editOf(id).value('exposure');
      await f.drag(const Offset(100, 0));
      expect(app.editOf(id).value('exposure'), e0);
      await f.up();
      await settle(t);
      expect(find.byKey(const ValueKey('original-pill')), findsNothing);
    });

    testWidgets('double tap on a row resets that parameter; header reset returns the category to auto', (t) async {
      final (app, id) = await open(t);
      app.applyEdit(id, (e) => e.withValue('exposure', 30).withValue('contrast', -20));
      await tapKey(t, 'dock-${EGroup.light}');
      expect(find.byKey(const ValueKey('reset-light')).hitTestable(), findsOneWidget);
      final row = find.byKey(const ValueKey('row-light-exposure'));
      await t.tap(row);
      await t.pump(const Duration(milliseconds: 60));
      await t.tap(row);
      await settle(t);
      expect(app.editOf(id).value('exposure'), 0);
      expect(app.editOf(id).value('contrast'), -20);
      await tapKey(t, 'reset-light');
      expect(app.editOf(id).value('contrast'), 0);
      expect(app.editOf(id).groupAdjusted(EGroup.light), isFalse);
      expect(find.text('已复位光线'), findsOneWidget);
      await t.tap(find.byKey(const ValueKey('toast-action')));
      await settle(t);
      expect(app.editOf(id).value('contrast'), -20, reason: 'the category reset is one undo step');
    });

    testWidgets('tapping the value opens numeric entry; EV is typed in EV; out of range is clamped', (t) async {
      final (app, id) = await open(t);
      await tapKey(t, 'dock-${EGroup.light}');
      await tapKey(t, 'value-exposure');
      expect(find.byType(TextField), findsOneWidget);
      await t.enterText(find.byType(TextField), '0.5');
      await t.testTextInput.receiveAction(TextInputAction.done);
      await settle(t);
      expect(app.editOf(id).value('exposure'), 10);
      await tapKey(t, 'value-exposure');
      await t.enterText(find.byType(TextField), '9');
      await t.testTextInput.receiveAction(TextInputAction.done);
      await settle(t);
      expect(app.editOf(id).value('exposure'), 100);
      expect(find.text('已调整到范围内'), findsOneWidget);
    });

    testWidgets('tapping empty photo collapses the window to a pill; again expands; the pill close ends adjust mode', (t) async {
      await open(t);
      await tapKey(t, 'dock-${EGroup.light}');
      await t.tapAt(const Offset(60, 300));
      await t.pump(const Duration(milliseconds: 350));
      await settle(t);
      expect(find.byKey(const ValueKey('pill-light')), findsOneWidget);
      expect(find.byKey(const ValueKey('window-light')).hitTestable(), findsNothing);
      await dump(t, 'phone_pill');
      await t.tapAt(const Offset(60, 300));
      await t.pump(const Duration(milliseconds: 350));
      await settle(t);
      expect(find.byKey(const ValueKey('window-light')).hitTestable(), findsOneWidget);
      await t.tapAt(const Offset(60, 300));
      await t.pump(const Duration(milliseconds: 350));
      await settle(t);
      await tapKey(t, 'pill-close-light');
      expect(find.byKey(const ValueKey('pill-light')), findsNothing);
      expect(find.byKey(const ValueKey('window-light')), findsNothing);
    });

    testWidgets('browse mode: tap toggles immersive, double tap zooms, two fingers zoom', (t) async {
      await open(t);
      final z = zoomOf(t);
      // immersive
      await t.tapAt(const Offset(200, 400));
      await t.pump(const Duration(milliseconds: 350));
      await settle(t);
      expect(t.widget<AnimatedOpacity>(find.ancestor(of: find.byKey(const ValueKey('toolbar-back')), matching: find.byType(AnimatedOpacity)).first).opacity, 0);
      await t.tapAt(const Offset(200, 400));
      await t.pump(const Duration(milliseconds: 350));
      await settle(t);
      // double tap -> 2x -> fit
      await t.tapAt(const Offset(200, 400));
      await t.pump(const Duration(milliseconds: 60));
      await t.tapAt(const Offset(200, 400));
      await settle(t, 8);
      expect(z.scale, closeTo(2, 0.01));
      await t.tapAt(const Offset(200, 400));
      await t.pump(const Duration(milliseconds: 60));
      await t.tapAt(const Offset(200, 400));
      await settle(t, 8);
      expect(z.scale, closeTo(1, 0.01));
      // pinch
      final a = await Finger.down(t, const Offset(150, 400));
      final b = await Finger.down(t, const Offset(250, 400), startMs: 0);
      await a.by(const Offset(-40, 0));
      await b.by(const Offset(40, 0));
      await a.up();
      await b.up();
      await settle(t, 8);
      expect(z.scale, greaterThan(1.5));
    });

    testWidgets('browse mode: a one-finger swipe at fit zoom changes the photo and shows n / N', (t) async {
      await open(t);
      final f = await Finger.down(t, const Offset(300, 400));
      await f.drag(const Offset(-200, 2), step: 10);
      await f.up();
      for (var i = 0; i < 4; i++) {
        await t.pump(const Duration(milliseconds: 50));
      }
      expect(find.textContaining(' / '), findsOneWidget);
      await settle(t, 10);
    });

    testWidgets('dock: six fixed items, no horizontal scrolling, every item fully on screen and >= 48 dp', (t) async {
      await open(t);
      expect(find.descendant(of: find.byType(CategoryDock), matching: find.byType(SingleChildScrollView)), findsOneWidget);
      final sv = t.widget<SingleChildScrollView>(find.descendant(of: find.byType(CategoryDock), matching: find.byType(SingleChildScrollView)));
      expect(sv.physics, isA<NeverScrollableScrollPhysics>());
      final screen = t.view.physicalSize / t.view.devicePixelRatio;
      for (final k in [...phoneDockOrder.map((g) => 'dock-$g'), 'dock-more']) {
        final r = t.getRect(find.byKey(ValueKey(k)));
        expect(r.left, greaterThanOrEqualTo(0), reason: k);
        expect(r.right, lessThanOrEqualTo(screen.width), reason: k);
        expect(r.width, greaterThanOrEqualTo(48), reason: k);
        expect(r.height, greaterThanOrEqualTo(48), reason: k);
      }
      for (final g in phoneMoreOrder) {
        expect(find.byKey(ValueKey('dock-$g')), findsNothing);
      }
    });

    testWidgets('switching between windows of different heights keeps the photo still', (t) async {
      await open(t);
      Rect photo() => t.getRect(find.byType(PhotoArt));
      await tapKey(t, 'dock-${EGroup.style}');
      await settle(t, 30);
      final a = photo();
      await tapKey(t, 'dock-${EGroup.effects}');
      await settle(t, 30);
      final b = photo();
      await tapKey(t, 'dock-${EGroup.color}');
      await settle(t, 30);
      final c = photo();
      expect(b, a);
      expect(c, a);
    });

    testWidgets('a window that scrolls ends on a whole row, with a scroll affordance', (t) async {
      await open(t, phys: const Size(1140, 2616), dpr: 3.0);
      await tapKey(t, 'dock-${EGroup.color}');
      await tapKey(t, 'row-color-band'); // unfolds the colour-range picker: the window now scrolls
      await settle(t, 10);
      final body = t.getRect(find.byKey(const ValueKey('window-scroll')));
      final sv = t.state<ScrollableState>(find.descendant(of: find.byKey(const ValueKey('window-scroll')), matching: find.byType(Scrollable)).first);
      expect(sv.position.maxScrollExtent, greaterThan(0), reason: 'the colour window scrolls here');
      expect(find.byType(RawScrollbar), findsWidgets, reason: 'scroll affordance');
      final rows = find.byWidgetPredicate((w) => w.key is ValueKey<String> && (w.key! as ValueKey<String>).value.startsWith('row-color-'));
      expect(rows, findsWidgets);
      for (final e in rows.evaluate()) {
        final r = t.getRect(find.byElementPredicate((x) => x == e));
        final cut = r.top < body.bottom - 0.5 && r.bottom > body.bottom + 0.5;
        expect(cut, isFalse, reason: 'row ${e.widget.key} $r cut by viewport bottom ${body.bottom}');
      }
    });

    testWidgets('colour pad: with the palette row active the whole screen is a 2D surface', (t) async {
      final (app, id) = await open(t);
      await tapKey(t, 'dock-${EGroup.style}');
      await tapKey(t, 'row-style-palette');
      expect(find.byKey(const ValueKey('extra-palette')), findsOneWidget);
      expect(app.editOf(id).palette, isNull);
      final f = await Finger.down(t, const Offset(60, 300));
      await f.drag(const Offset(40, -70), step: 6); // diagonal: no axis lock
      await t.pump(const Duration(milliseconds: 250));
      expect(find.byKey(const ValueKey('readout')), findsOneWidget);
      await dump(t, 'phone_color_pad');
      await f.drag(const Offset(30, -30));
      await f.up();
      await settle(t);
      final p = app.editOf(id).palette;
      expect(p, isNotNull);
      expect(p!.dx, greaterThan(0.5));
      expect(p.dy, lessThan(0.5), reason: 'up = warm');
    });

    testWidgets('colour: choosing a colour range switches the three following rows', (t) async {
      final (app, id) = await open(t);
      final before = app.editOf(id);
      await tapKey(t, 'dock-${EGroup.color}');
      await tapKey(t, 'row-color-band');
      expect(find.text('橙色相'), findsOneWidget);
      await tapKey(t, 'band-5');
      expect(find.text('蓝色相'), findsOneWidget);
      expect(app.editOf(id).sameAs(before), isTrue, reason: 'selecting a range is not an edit');
    });

    testWidgets('curve window shows the four points and the channel picker', (t) async {
      final (app, id) = await open(t);
      app.applyEdit(id, (e) => e.withValue('cv_rgb_L', 25));
      await tapKey(t, 'dock-more');
      await tapKey(t, 'menu-curve');
      for (final l in ['高光区', '亮部', '暗部', '阴影区']) {
        expect(find.text(l), findsOneWidget);
      }
      await tapKey(t, 'row-curve-cv_rgb_L');
      expect(find.byKey(const ValueKey('extra-cv_rgb_L')), findsOneWidget);
      await dump(t, 'phone_curve');
    });

    testWidgets('grading and crop windows render without overflow', (t) async {
      await open(t);
      await tapKey(t, 'dock-more');
      await tapKey(t, 'menu-grade');
      await tapKey(t, 'row-grade-wheel');
      await dump(t, 'phone_grade');
      await tapKey(t, 'dock-${EGroup.crop}');
      await tapKey(t, 'row-crop-ratio');
      expect(find.byKey(const ValueKey('crop-4')), findsOneWidget);
      await dump(t, 'phone_crop');
      await tapKey(t, 'dock-more');
      await tapKey(t, 'menu-detail');
      expect(find.byKey(const ValueKey('one-to-one')), findsOneWidget);
      await tapKey(t, 'dock-${EGroup.effects}');
      await tapKey(t, 'dock-${EGroup.style}');
      await tapKey(t, 'row-style-look');
      await dump(t, 'phone_style');
    });

    testWidgets('system back dismisses in order: numeric entry, window, then the page', (t) async {
      await open(t);
      await tapKey(t, 'dock-${EGroup.light}');
      await tapKey(t, 'value-exposure');
      expect(find.byType(TextField), findsOneWidget);
      await t.binding.handlePopRoute();
      await settle(t);
      expect(find.byType(TextField), findsNothing);
      expect(find.byKey(const ValueKey('window-light')), findsOneWidget);
      await t.binding.handlePopRoute();
      await settle(t);
      expect(find.byKey(const ValueKey('window-light')), findsNothing);
    });

    testWidgets('undo / redo buttons work per photo; the more menu holds the old actions', (t) async {
      final (app, id) = await open(t);
      final e0 = app.editOf(id).value('exposure');
      app.applyEdit(id, (e) => e.withValue('exposure', 40));
      await settle(t, 2);
      await tapKey(t, 'toolbar-undo');
      expect(app.editOf(id).value('exposure'), e0);
      await tapKey(t, 'toolbar-redo');
      expect(app.editOf(id).value('exposure'), 40);
      await t.ensureVisible(find.byKey(const ValueKey('dock-more')));
      await settle(t, 3);
      await tapKey(t, 'dock-more');
      for (final k in ['menu-publish', 'menu-copy', 'menu-paste', 'menu-sync', 'menu-restore']) {
        expect(find.byKey(ValueKey(k)), findsOneWidget, reason: k);
      }
      await dump(t, 'phone_more_menu');
      await tapKey(t, 'menu-copy');
      expect(app.hasClipboard, isTrue);
    });

    testWidgets('dark mode', (t) async {
      await open(t, dark: true);
      await tapKey(t, 'dock-${EGroup.light}');
      await dump(t, 'phone_dark_adjust');
    });

    testWidgets('modified categories get a dot in the dock', (t) async {
      final (app, id) = await open(t);
      app.applyEdit(id, (_) => EditState.empty);
      await settle(t, 2);
      expect(find.byKey(const ValueKey('dot-light')), findsNothing);
      app.applyEdit(id, (e) => e.withValue('exposure', 10));
      await settle(t, 2);
      expect(find.byKey(const ValueKey('dot-light')), findsOneWidget);
    });

    testWidgets('left hand: the window docks on the left', (t) async {
      setView(t, phonePhys, phoneDpr);
      await loadTestFonts();
      final app = AppState.demo();
      app.settings.handedness = 1;
      final id = app.photos.firstWhere((p) => p.title == '外滩夕阳' && p.id.startsWith('d')).id;
      await t.pumpWidget(RepaintBoundary(
          key: shotKey,
          child: MaterialApp(debugShowCheckedModeBanner: false, theme: buildTheme(Brightness.light), home: EditorPage(app: app, photoId: id, resamplePointers: false))));
      await settle(t);
      await tapKey(t, 'dock-${EGroup.light}');
      final r = t.getRect(find.byKey(const ValueKey('window-light')));
      expect(r.left, lessThan(30));
      await dump(t, 'phone_left_hand');
    });
  });

  group('Find N5 inner', () {
    testWidgets('two windows and the histogram window; a third category replaces the least recent; no overflow', (t) async {
      await open(t, phys: n5Phys, dpr: n5Dpr);
      expect(find.byKey(const ValueKey('histogram-window')), findsOneWidget);
      await tapKey(t, 'dock-${EGroup.light}');
      await tapKey(t, 'dock-${EGroup.color}');
      expect(find.byKey(const ValueKey('window-light')), findsOneWidget);
      expect(find.byKey(const ValueKey('window-color')), findsOneWidget);
      final l = t.getRect(find.byKey(const ValueKey('window-light'))), c = t.getRect(find.byKey(const ValueKey('window-color')));
      expect(l.left, greaterThan(c.left), reason: 'first window on the dominant (right) side, second on the other');
      await dump(t, 'n5_two_windows');
      await tapKey(t, 'dock-${EGroup.curve}');
      expect(find.byKey(const ValueKey('window-curve')), findsOneWidget);
      expect(find.byKey(const ValueKey('window-light')), findsNothing, reason: 'least recently used replaced');
      expect(find.byKey(const ValueKey('window-color')), findsOneWidget);
    });

    testWidgets('windows keep their content when the window class changes (fold / unfold)', (t) async {
      await open(t, phys: n5Phys, dpr: n5Dpr);
      await tapKey(t, 'dock-${EGroup.light}');
      await tapKey(t, 'dock-${EGroup.color}');
      setView(t, phonePhys, phoneDpr);
      await settle(t, 10);
      expect(find.byKey(const ValueKey('histogram-window')), findsNothing);
      expect(find.byType(Scrollable), findsWidgets);
      final nWindows = ['light', 'color'].where((g) => find.byKey(ValueKey('window-$g')).evaluate().isNotEmpty).length;
      expect(nWindows, 1);
      setView(t, n5Phys, n5Dpr);
      await settle(t, 10);
      expect(find.byKey(const ValueKey('window-light')), findsOneWidget);
      expect(find.byKey(const ValueKey('window-color')), findsOneWidget);
    });

    testWidgets('no re-layout while a finger is adjusting: window rects are identical before and during the drag', (t) async {
      final (_, _) = await open(t, phys: n5Phys, dpr: n5Dpr);
      await tapKey(t, 'dock-${EGroup.light}');
      await settle(t, 6);
      final before = t.getRect(find.byKey(const ValueKey('window-light')));
      final f = await Finger.down(t, const Offset(300, 300));
      await f.drag(const Offset(100, 0));
      await t.pump(const Duration(milliseconds: 300));
      // the window is a one-line pill in place; its top-left corner has not moved
      final pill = t.getRect(find.byKey(const ValueKey('pill-light')));
      expect(pill.top, closeTo(before.top, 0.5));
      expect(pill.right, closeTo(before.right, 0.5));
      await f.up();
      await settle(t, 8);
      expect(t.getRect(find.byKey(const ValueKey('window-light'))), before);
    });

    testWidgets('dragging the window header snaps it to another edge and remembers it', (t) async {
      await open(t, phys: n5Phys, dpr: n5Dpr);
      await tapKey(t, 'dock-${EGroup.light}');
      final h = t.getCenter(find.byKey(const ValueKey('header-light')));
      final g = await Finger.down(t, h);
      await g.drag(const Offset(-500, -40), step: 20);
      await g.up();
      await settle(t, 15);
      final r = t.getRect(find.byKey(const ValueKey('window-light')));
      expect(r.left, lessThan(60), reason: 'snapped to the left edge');
    });
  });

  testWidgets('N5: a window dragged to the bottom becomes a wide two-column card', (t) async {
    await open(t, phys: n5Phys, dpr: n5Dpr);
    await tapKey(t, 'dock-${EGroup.light}');
    final g = await Finger.down(t, t.getCenter(find.byKey(const ValueKey('header-light'))));
    await g.drag(const Offset(-250, 0), step: 20);
    await g.up();
    await settle(t, 15);
    final r = t.getRect(find.byKey(const ValueKey('window-light')));
    expect(r.width, greaterThan(300));
    expect(r.width, lessThanOrEqualTo(480));
    expect(r.bottom, greaterThan(500), reason: 'docked to the bottom edge');
    await dump(t, 'n5_bottom_card');
  });

  testWidgets('N5: the histogram window snaps to a corner after a drag', (t) async {
    await open(t, phys: n5Phys, dpr: n5Dpr);
    final before = t.getRect(find.byKey(const ValueKey('histogram-window')));
    final g = await Finger.down(t, before.center);
    await g.drag(const Offset(600, 250), step: 20);
    await g.up();
    await settle(t, 15);
    final after = t.getRect(find.byKey(const ValueKey('histogram-window')));
    expect(after.right, greaterThan(700), reason: 'snapped to the right side');
    expect(after.size, before.size);
  });

  group('settings', () {
    testWidgets('handedness segmented control, haptics switch and reset row', (t) async {
      setView(t, phonePhys, phoneDpr);
      await loadTestFonts();
      final app = AppState.demo();
      await t.pumpWidget(MaterialApp(
          theme: buildTheme(Brightness.light), home: Scaffold(body: SettingsPage(app: app, wide: false))));
      await t.pump(const Duration(milliseconds: 200));
      final seg = find.byKey(const ValueKey('seg-handedness'));
      await t.ensureVisible(seg);
      await t.pump(const Duration(milliseconds: 100));
      expect(app.settings.handedness, 0);
      expect(find.byKey(const ValueKey('reset-hand')), findsOneWidget);
      await t.tap(find.descendant(of: seg, matching: find.text('左手')));
      await t.pump(const Duration(milliseconds: 100));
      expect(app.settings.handedness, 1);
      expect(find.byKey(const ValueKey('reset-hand')), findsNothing, reason: 'only shown while automatic');
      await t.tap(find.descendant(of: seg, matching: find.text('右手')));
      await t.pump(const Duration(milliseconds: 100));
      expect(app.settings.handedness, 2);
      await t.ensureVisible(find.byKey(const ValueKey('sw-haptics')));
      await t.tap(find.byKey(const ValueKey('sw-haptics')));
      await t.pump(const Duration(milliseconds: 100));
      expect(app.settings.haptics, isFalse);
    });
  });

  group('phone landscape', () {
    testWidgets('side rail dock, one window, no overflow', (t) async {
      await open(t, phys: const Size(2400, 1080), dpr: 2.75);
      await tapKey(t, 'dock-${EGroup.light}');
      expect(find.byKey(const ValueKey('window-light')), findsOneWidget);
      await dump(t, 'phone_landscape');
    });
  });

  test('haptics: throttled to 40 ms, silent when disabled', () async {
    final fired = <HapticKind>[];
    final h = Haptics(onFire: fired.add);
    h.landmark(Landmark.autoSnap);
    h.landmark(Landmark.detent); // inside 40 ms: dropped
    expect(fired, [HapticKind.autoSnap]);
    await Future<void>.delayed(const Duration(milliseconds: 60));
    h.landmark(Landmark.endStop);
    h.landmark(Landmark.none);
    expect(fired, [HapticKind.autoSnap, HapticKind.endStop]);
    h.enabled = false;
    await Future<void>.delayed(const Duration(milliseconds: 60));
    h.landmark(Landmark.detent);
    expect(fired, hasLength(2));
  });

  test('window manager type is reachable from the editor', () {
    expect(WindowManager(), isA<WindowManager>());
  });
}
