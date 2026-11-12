// Editor interaction v2 (requirements 4.15): widget level behaviour on a Find N5 inner screen (floating
// windows; the phone-portrait docked panel is tested in editor_panel_test.dart), plus screenshots written to test/goldens/editor_v2/ for review.
import 'dart:io';
import 'dart:ui' as ui;

import 'package:dapple/editor/haptics.dart';
import 'package:dapple/editor/value_mapping.dart';
import 'package:dapple/editor/window_manager.dart';
import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/screens/editor/zoom_view.dart';
import 'package:dapple/screens/library/settings_page.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
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
      // phone portrait: the least recent window is suspended, the other becomes the docked panel
      expect(find.byKey(const ValueKey('docked-panel')), findsOneWidget);
      expect(find.byKey(const ValueKey('window-light')), findsNothing);
      expect(find.byKey(const ValueKey('window-color')), findsNothing);
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
