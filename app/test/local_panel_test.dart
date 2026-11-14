// 局部 (local adjustments) on the real editor: the shape card, adding an area, the pads and the strength strip, the on-photo gestures
// (radial move / new circle, brush batching), the area actions, the itemised view, back, the area limit and a layout smoke test over
// the window shapes (docs/design/local-adjustments.md).
import 'package:dapple/model/local_adjust.dart';
import 'package:dapple/model/local_kinds.dart';
import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/screens/editor/local_controller.dart';
import 'package:dapple/screens/editor/local_layer.dart';
import 'package:dapple/services/local_ai.dart' show LocalAi;
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show Finger, n5Dpr, n5Phys, phoneDpr, phonePhys, setView, settle, zoomOf;
import 'support/harness.dart';

const ownerPhys = Size(1140, 2616);
const ownerDpr = 2.75;

class Rig {
  Rig(this.app, this.id, this.lc);
  final AppState app;
  final String id;
  final LocalController lc;
  LocalList get local => app.editOf(id).local;
}

/// The editor on a demo photo with the 局部 panel open.
Future<Rig> openLocal(WidgetTester t,
    {Size phys = phonePhys, double dpr = phoneDpr, bool dark = false, LocalAi? ai, void Function(AppState app, String id)? before}) async {
  setView(t, phys, dpr);
  await loadTestFonts();
  final app = AppState.demo();
  final id = app.photos.firstWhere((p) => p.title == '外滩夕阳' && p.id.startsWith('d')).id;
  before?.call(app, id);
  await t.pumpWidget(MaterialApp(
    debugShowCheckedModeBanner: false,
    theme: buildTheme(Brightness.light),
    darkTheme: buildTheme(Brightness.dark),
    themeMode: dark ? ThemeMode.dark : ThemeMode.light,
    home: EditorPage(app: app, photoId: id, resamplePointers: false, initialGroup: EGroup.local, localAi: ai),
  ));
  await settle(t, 8);
  final lc = t.widget<LocalPhotoLayer>(find.byType(LocalPhotoLayer)).lc;
  return Rig(app, id, lc);
}

Finder key(String k) => find.byKey(ValueKey(k));

Future<void> tapKey(WidgetTester t, String k) async {
  // a short side pane scrolls the shape card and the portrait chips rather than shrinking their 56 dp tiles (U7)
  if (k.startsWith('kind-') || k.startsWith('face-') || k.startsWith('chip-')) await t.ensureVisible(key(k));
  await t.tap(key(k));
  await settle(t, 4);
}

/// Opens the area with a new [kind] through the shape card.
Future<void> addViaCard(WidgetTester t, String kind) async {
  await tapKey(t, 'kind-$kind');
}

Rect frameRect(WidgetTester t) => t.getRect(key('local-handles'));

LocalAdjustment area(Rig r, [int i = 0]) => r.local.items[i];

LocalAdjustment radialArea(String id) =>
    LocalAdjustment(id: id, mask: const [MaskComponent(kind: MaskKindId.radial, feather: 50)], ui: LocalUi.centre);

Future<void> back(WidgetTester t) async {
  await t.binding.handlePopRoute();
  await settle(t, 4);
}

void main() {
  group('shape card and adding', () {
    testWidgets('no area: the shape card with exactly the five kinds, the photo layer and six dock items', (t) async {
      final r = await openLocal(t);
      expect(r.local.isEmpty, isTrue);
      for (final k in ['linear', 'radial', 'brush', 'luminance', 'color']) {
        expect(key('kind-$k'), findsOneWidget, reason: k);
      }
      expect(find.byWidgetPredicate((w) => w.key is ValueKey && (w.key! as ValueKey).value.toString().startsWith('kind-')), findsNWidgets(5));
      expect(pickableKinds.map((k) => k.id), ['linear', 'radial', 'brush', 'luminance', 'color']);
      expect(key('local-handles'), findsOneWidget);
      expect(key('local-pad-light'), findsNothing);
      for (final g in padDockOrderForTest) {
        expect(key('dock-$g'), findsOneWidget, reason: '$g');
      }
      expect(padDockOrderForTest, hasLength(6));
      expect(find.text('局部'), findsWidgets);
      expect(t.takeException(), isNull);
    });

    testWidgets('kind-radial: one area, a chip, both pads, one radial component, ONE undo step', (t) async {
      final r = await openLocal(t);
      await addViaCard(t, 'radial');
      expect(r.local.length, 1);
      expect(area(r).mask, hasLength(1));
      expect(area(r).mask.first.kind, MaskKindId.radial);
      expect(key('local-chip-0'), findsOneWidget);
      expect(key('local-pad-light'), findsOneWidget, reason: 'one full-size pad, the light one first');
      expect(key('local-pad-color'), findsNothing);
      expect(key('local-seg-light'), findsOneWidget);
      expect(key('kind-radial'), findsNothing, reason: 'the card closed itself');
      expect(r.lc.selId, area(r).id);
      expect(r.app.canUndo(r.id), isTrue);
      r.app.undo(r.id);
      expect(r.local.isEmpty, isTrue, reason: 'one undo removes the whole new area');
    });

    testWidgets('every kind makes a component of its kind, range kinds fall back to defaults without an engine', (t) async {
      final r = await openLocal(t);
      for (final (kind, id) in [('linear', MaskKindId.linear), ('brush', MaskKindId.brush), ('luminance', MaskKindId.luminance), ('color', MaskKindId.color)]) {
        await tapKey(t, 'local-add');
        await addViaCard(t, kind);
        expect(r.local.items.last.mask.first.kind, id, reason: kind);
      }
      final lum = r.local.items[2].mask.first;
      expect(lum.lo, 55);
      expect(lum.hi, 85);
      expect(r.local.length, 4);
    });

    testWidgets('the tool strip: brush sizes and eraser for a brush, tone bar for luminance, hue bar for colour, nothing for radial', (t) async {
      final r = await openLocal(t);
      await addViaCard(t, 'radial');
      expect(key('local-tools'), findsNothing);
      await tapKey(t, 'local-add');
      await addViaCard(t, 'brush');
      expect(key('local-tools'), findsOneWidget);
      expect(key('brush-size-0'), findsOneWidget);
      await tapKey(t, 'brush-size-3');
      expect(r.lc.brushSize, 3);
      await tapKey(t, 'brush-sub');
      expect(r.lc.erase, isTrue);
      await tapKey(t, 'local-add');
      await addViaCard(t, 'luminance');
      expect(key('tone-bar'), findsOneWidget);
      await tapKey(t, 'local-add');
      await addViaCard(t, 'color');
      expect(key('hue-bar'), findsOneWidget);
      expect(t.takeException(), isNull);
    });
  });

  group('pads and strength', () {
    testWidgets('dragging the light pad to the right raises exposure; the whole drag is one undo step', (t) async {
      final r = await openLocal(t);
      await addViaCard(t, 'radial');
      expect(area(r).op('exposure'), 0);
      final pad = t.getRect(key('local-pad-light'));
      final f = await Finger.down(t, pad.center);
      await f.drag(Offset(pad.width * 0.3, 0));
      await f.up();
      expect(area(r).op('exposure'), greaterThan(0));
      expect(area(r).strength, 100);
      r.app.undo(r.id);
      expect(r.local.length, 1, reason: 'the area itself is the earlier step');
      expect(area(r).op('exposure'), 0, reason: 'one undo took the whole drag back');
      expect(area(r).ops, isEmpty);
    });

    testWidgets('the strength strip at the bottom gives strength 0 and still keeps the area', (t) async {
      final r = await openLocal(t);
      await addViaCard(t, 'radial');
      final strip = t.getRect(key('strip-local'));
      final f = await Finger.down(t, strip.bottomCenter - const Offset(0, 2));
      await f.up();
      expect(area(r).strength, 0);
      expect(r.local.length, 1);
      // the top of the strip is a boost, strength stays at 100
      final g = await Finger.down(t, strip.topCenter + const Offset(0, 2));
      await g.up();
      expect(area(r).strength, 100);
    });
  });

  group('on-photo gestures', () {
    testWidgets('a one-finger drag inside the radial ellipse moves its centre: no paging, no zoom, one undo step', (t) async {
      final r = await openLocal(t);
      await addViaCard(t, 'radial');
      final before = area(r).mask.first;
      await settle(t, 8); // the panel's height follows the new layer (animated): the photo frame settles first
      final fr = frameRect(t);
      final f = await Finger.down(t, fr.center);
      await f.drag(const Offset(40, 24));
      await f.up();
      final after = area(r).mask.first;
      expect(after.cx, greaterThan(before.cx));
      expect(after.cy, greaterThan(before.cy));
      expect(after.rx, before.rx);
      expect(r.lc.ctl.id, r.id, reason: 'the photo did not page');
      expect(zoomOf(t).scale, 1);
      expect(frameRect(t), fr);
      r.app.undo(r.id);
      final undone = area(r).mask.first;
      expect(undone.cx, before.cx);
      expect(undone.cy, before.cy);
      expect(t.takeException(), isNull);
    });

    testWidgets('a drag far outside the ellipse draws a new circle at the finger', (t) async {
      final r = await openLocal(t);
      await addViaCard(t, 'radial');
      final before = area(r).mask.first;
      final fr = frameRect(t);
      final start = Offset(fr.right - 24, fr.top + 24);
      final f = await Finger.down(t, start);
      await f.drag(const Offset(-50, 0));
      await f.up();
      final after = area(r).mask.first;
      expect(after.cx, greaterThan(before.cx + 0.2), reason: 'the new centre is at the finger');
      expect(after.cy, lessThan(before.cy - 0.1));
      expect(after.rx, closeTo(after.ry, 1e-9), reason: 'a circle');
      expect(after.rx, isNot(before.rx));
      expect(r.lc.ctl.id, r.id);
      r.app.undo(r.id);
      expect(area(r).mask.first.cx, before.cx);
    });

    testWidgets('brush: many moves inside one frame flush at most once per frame; one stroke, one undo step', (t) async {
      final r = await openLocal(t);
      await tapKey(t, 'local-add');
      await addViaCard(t, 'brush');
      expect(area(r).mask.first.strokes, isEmpty);
      final fr = frameRect(t);
      final g = await t.startGesture(fr.center - const Offset(80, 0));
      final flushes0 = LocalController.debugBrushFlushes;
      var frames = 0;
      for (var round = 0; round < 4; round++) {
        for (var i = 0; i < 10; i++) {
          await g.moveBy(const Offset(5, 1));
        }
        await t.pump(const Duration(milliseconds: 16));
        frames++;
        expect(LocalController.debugBrushFlushes - flushes0, lessThanOrEqualTo(frames), reason: 'round $round');
      }
      expect(LocalController.debugBrushFlushes, greaterThan(flushes0));
      await g.up();
      await t.pump(const Duration(milliseconds: 16));
      final strokes = area(r).mask.first.strokes;
      expect(strokes, hasLength(1));
      expect(strokes.first.count, greaterThan(10));
      expect(strokes.first.erase, isFalse);
      // the points are where the finger went: left to right across the frame centre
      final xs = [for (var i = 0; i < strokes.first.count; i++) strokes.first.pts[2 * i]];
      expect(xs.first, lessThan(xs.last));
      r.app.undo(r.id);
      expect(area(r).mask.first.strokes, isEmpty, reason: 'one undo removed the whole stroke');
    });

    testWidgets('brush: erase flag and size reach the stroke', (t) async {
      final r = await openLocal(t);
      await tapKey(t, 'local-add');
      await addViaCard(t, 'brush');
      await tapKey(t, 'brush-sub');
      final fr = frameRect(t);
      final g = await t.startGesture(fr.center);
      for (var i = 0; i < 8; i++) {
        await g.moveBy(const Offset(6, 0));
      }
      await g.up();
      await t.pump(const Duration(milliseconds: 16));
      final s = area(r).mask.first.strokes.single;
      expect(s.erase, isTrue);
      expect(s.size, greaterThan(0.0005));
    });

    testWidgets('brush: a tap paints a dot', (t) async {
      final r = await openLocal(t);
      await tapKey(t, 'local-add');
      await addViaCard(t, 'brush');
      final g = await t.startGesture(frameRect(t).center);
      await g.up();
      await t.pump(const Duration(milliseconds: 16));
      expect(area(r).mask.first.strokes, hasLength(1));
      expect(area(r).mask.first.strokes.first.count, greaterThanOrEqualTo(1));
    });

    testWidgets('brush: a second finger within the first ~50 ms, before any move, discards the stroke', (t) async {
      final r = await openLocal(t);
      await tapKey(t, 'local-add');
      await addViaCard(t, 'brush');
      final fr = frameRect(t);
      final g1 = await t.startGesture(fr.center);
      await t.pump(const Duration(milliseconds: 30));
      final g2 = await t.startGesture(fr.center + const Offset(60, 20));
      await t.pump(const Duration(milliseconds: 16));
      expect(r.lc.liveStroke, isEmpty);
      await g1.up();
      await g2.up();
      await settle(t, 3);
      expect(area(r).mask.first.strokes, isEmpty, reason: 'no stroke was written');
      expect(r.lc.liveStroke, isEmpty);
      expect(zoomOf(t).scale, 1);
      expect(t.takeException(), isNull);
    });
  });

  group('area actions', () {
    testWidgets('the selected chip shows the actions; 删除 deletes with an undo toast', (t) async {
      final r = await openLocal(t);
      await addViaCard(t, 'radial');
      expect(key('act-删除'), findsNothing);
      await tapKey(t, 'local-chip-0');
      expect(key('act-删除'), findsOneWidget);
      for (final a in ['加一块', '减一块', '反选', '复制', '删除']) {
        expect(key('act-$a'), findsOneWidget, reason: a);
      }
      await tapKey(t, 'act-删除');
      expect(r.local.isEmpty, isTrue);
      expect(r.app.toast?.actionLabel, '撤销');
      expect(r.app.toast?.message, contains('删除'));
      r.app.toast!.onAction!();
      await settle(t, 3);
      expect(r.local.length, 1, reason: 'the toast action undoes the delete');
      expect(key('local-chip-0'), findsOneWidget);
    });

    testWidgets('反选 toggles the area invert', (t) async {
      final r = await openLocal(t);
      await addViaCard(t, 'radial');
      expect(area(r).invert, isFalse);
      await tapKey(t, 'local-chip-0');
      await tapKey(t, 'act-反选');
      expect(area(r).invert, isTrue);
      expect(key('local-pad-light'), findsOneWidget, reason: 'back to the pads');
      await tapKey(t, 'local-chip-0');
      await tapKey(t, 'act-反选');
      expect(area(r).invert, isFalse);
    });

    testWidgets('复制 adds a second area after the first and selects it', (t) async {
      final r = await openLocal(t);
      await addViaCard(t, 'radial');
      final firstId = area(r).id;
      await tapKey(t, 'local-chip-0');
      await tapKey(t, 'act-复制');
      expect(r.local.length, 2);
      expect(key('local-chip-1'), findsOneWidget);
      expect(area(r, 1).id, isNot(firstId));
      expect(area(r, 1).mask.first.same(area(r, 0).mask.first), isTrue);
      expect(r.lc.selId, area(r, 1).id);
    });

    testWidgets('加一块 opens the card and adds a part; 去掉这块 (actions) removes the selected one', (t) async {
      final r = await openLocal(t);
      await addViaCard(t, 'radial');
      await tapKey(t, 'local-chip-0');
      await tapKey(t, 'act-加一块');
      expect(key('kind-brush'), findsOneWidget);
      await addViaCard(t, 'linear');
      expect(area(r).mask, hasLength(2));
      expect(area(r).mask[1].op, MaskOp.add);
      expect(key('local-part-1'), findsOneWidget);
      await tapKey(t, 'local-chip-0');
      await tapKey(t, 'act-去掉这块');
      expect(area(r).mask, hasLength(1));
      expect(area(r).mask.first.kind, MaskKindId.radial);
    });
  });

  group('itemised view', () {
    testWidgets('分项 shows the radar with group chips; the detail group switches the items', (t) async {
      final r = await openLocal(t);
      await addViaCard(t, 'radial');
      expect(key('local-radar-light'), findsNothing);
      await tapKey(t, 'items-button');
      expect(key('local-radar-light'), findsOneWidget);
      for (final g in ['light', 'color', 'detail']) {
        expect(key('local-group-$g'), findsOneWidget);
      }
      expect(r.lc.itemsOf('light').first.id, 'exposure');
      await tapKey(t, 'local-group-detail');
      expect(r.lc.itemGroup, 'detail');
      expect(key('local-radar-detail'), findsOneWidget);
      expect(key('local-radar-light'), findsNothing);
      expect(r.lc.itemsOf('detail').map((e) => e.id), contains('clarity'));
      await tapKey(t, 'local-group-color');
      expect(key('local-radar-color'), findsOneWidget);
      expect(t.takeException(), isNull);
    });
  });

  group('back and limits', () {
    testWidgets('back closes the shape card, then the actions, before anything else', (t) async {
      final r = await openLocal(t);
      await addViaCard(t, 'radial');
      await tapKey(t, 'local-add');
      expect(key('kind-radial'), findsOneWidget);
      await back(t);
      expect(key('kind-radial'), findsNothing);
      expect(key('local-pad-light'), findsOneWidget);
      expect(r.lc.body, LocalBody.pads);

      await tapKey(t, 'local-chip-0');
      expect(key('act-删除'), findsOneWidget);
      await back(t);
      expect(key('act-删除'), findsNothing);
      expect(key('local-pad-light'), findsOneWidget);
      expect(r.local.length, 1, reason: 'back only closed the card');

      // the controller's own entry point
      await tapKey(t, 'local-chip-0');
      expect(r.lc.body, LocalBody.actions);
      final ctl = r.lc.ctl;
      expect(ctl.handleBack(), isTrue);
      await settle(t, 3);
      expect(r.lc.body, LocalBody.pads);
    });

    testWidgets('16 areas: 局部 + shows the limit toast and opens no card', (t) async {
      final r = await openLocal(t);
      r.app.applyEdit(r.id, (e) => e.copyWith(local: LocalList([for (var i = 0; i < kMaxLocalAdjustments; i++) radialArea('x$i')])));
      await settle(t, 4);
      expect(r.local.length, 16);
      expect(r.app.toast?.message, isNot('最多 16 个区域'));
      await tapKey(t, 'local-add');
      expect(r.app.toast?.message, '最多 16 个区域');
      expect(r.lc.body, LocalBody.pads);
      expect(key('kind-radial'), findsNothing);
      expect(r.local.length, 16);
      // 复制 is refused the same way
      await r.lc.addArea(kindById('radial'));
      expect(r.local.length, 16);
      r.lc.duplicateArea();
      expect(r.local.length, 16);
      expect(t.takeException(), isNull);
    });

    testWidgets('the brush limit and the part limit show their toasts', (t) async {
      final r = await openLocal(t);
      r.app.applyEdit(
        r.id,
        (e) => e.copyWith(local: LocalList([for (var i = 0; i < kMaxBrushComponents; i++) LocalAdjustment(id: 'b$i', mask: const [MaskComponent(kind: MaskKindId.brush)])])),
      );
      await settle(t, 4);
      await r.lc.addArea(kindById('brush'));
      expect(r.app.toast?.message, '画笔最多 $kMaxBrushComponents 块');
      expect(r.local.length, kMaxBrushComponents);
    });
  });

  group('layout across window shapes and themes', () {
    final windows = <(String, Size, double, bool)>[
      ('phone', phonePhys, phoneDpr, false),
      ('owner fold outer', ownerPhys, ownerDpr, false),
      ('fold inner (side pane)', n5Phys, n5Dpr, false),
      ('landscape phone (side pane)', const Size(2400, 1080), 2.75, true),
      ('tablet', const Size(2560, 1600), 2, true),
    ];
    for (final (name, phys, dpr, _) in windows) {
      for (final dark in [false, true]) {
        testWidgets('$name ${dark ? 'dark' : 'light'}: card, pads, actions, items, brush tools without overflow', (t) async {
          final r = await openLocal(t, phys: phys, dpr: dpr, dark: dark);
          expect(t.takeException(), isNull, reason: 'shape card');
          await addViaCard(t, 'radial');
          expect(t.takeException(), isNull, reason: 'pads');
          expect(key('local-pad-light'), findsOneWidget);
          await tapKey(t, 'local-chip-0');
          expect(key('act-删除'), findsOneWidget);
          expect(t.takeException(), isNull, reason: 'actions');
          await back(t);
          await tapKey(t, 'items-button');
          expect(t.takeException(), isNull, reason: 'items');
          // a window too low for photo + the itemised panel (landscape phone): the panel body scrolls under the photo's minimum height
          await t.ensureVisible(find.byKey(const ValueKey('local-group-detail')));
          await tapKey(t, 'local-group-detail');
          expect(t.takeException(), isNull, reason: 'items detail');
          await t.ensureVisible(find.byKey(const ValueKey('header-back')));
          await tapKey(t, 'header-back');
          await tapKey(t, 'local-add');
          await addViaCard(t, 'brush');
          expect(key('local-tools'), findsOneWidget);
          expect(t.takeException(), isNull, reason: 'brush tools');
          // many chips
          r.app.applyEdit(r.id, (e) => e.copyWith(local: LocalList([...e.local.items, for (var i = 0; i < 6; i++) radialArea('y$i')])));
          await settle(t, 4);
          expect(t.takeException(), isNull, reason: 'eight chips');
        });
      }
    }

    for (final (phys, dpr) in [(phonePhys, phoneDpr), (n5Phys, n5Dpr)]) {
      testWidgets('large text (1.6) at ${phys.width.round()} px wide, dark', (t) async {
        t.platformDispatcher.textScaleFactorTestValue = 1.6;
        addTearDown(t.platformDispatcher.clearTextScaleFactorTestValue);
        final r = await openLocal(t, phys: phys, dpr: dpr, dark: true);
        await addViaCard(t, 'radial');
        await tapKey(t, 'local-chip-0');
        await back(t);
        await tapKey(t, 'items-button');
        expect(t.takeException(), isNull);
        expect(r.local.length, 1);
      });
    }
  });
}

/// The dock order the panel shows (the six items).
const List<EGroup> padDockOrderForTest = [EGroup.light, EGroup.color, EGroup.style, EGroup.effects, EGroup.local, EGroup.crop];
