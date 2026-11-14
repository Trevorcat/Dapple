// 局部's one full-size pad with the 光线 / 颜色 / 质地 switch (docs/design/local-panel-redesign.md, alternative B): the segments change
// the pad and the header readout, the texture pad writes clarity / texture / dehaze, a dot marks a pad that is away from its centre, the
// switch takes a line of its own on a narrow phone with several areas, and an edit saved before the change still has the same parameters.
import 'dart:convert';

import 'package:dapple/model/local_adjust.dart';
import 'package:dapple/model/local_pad.dart';
import 'package:dapple/model/pad_model.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show Finger, settle;
import 'local_panel_test.dart' show addViaCard, area, key, openLocal, tapKey;

/// An area as the app wrote it before the texture pad existed: ui without tx / ty, the ops of the light and colour pads' recipe plus an
/// offset from the itemised view (highlights), strip a = 0.75 (recipe x1.5).
const savedBefore =
    r'{"v":1,"items":[{"id":"old","strength":100,"mask":[{"kind":"linear","op":"add","x0":0.5,"y0":0.05,"x1":0.5,"y1":0.5}],'
    r'"adjust":{"exposure":-24.75,"contrast":12,"highlights":-10,"whites":3,"blacks":-3,"temp":18,"saturation":20.25,"vibrance":10.13},'
    r'"ui":{"lx":-0.55,"ly":0.2,"cx":0.3,"cy":0.45,"a":0.75,"off":{"highlights":-10}}}]}';

void main() {
  group('saved edits open unchanged', () {
    test('an edit saved before this change keeps its parameters and its pad state', () {
      final list = LocalList.fromJson(jsonDecode(savedBefore));
      final a = list.items.single;
      expect(a.ops, {'exposure': -24.75, 'contrast': 12, 'highlights': -10, 'whites': 3, 'blacks': -3, 'temp': 18, 'saturation': 20.25, 'vibrance': 10.13});
      expect(a.strength, 100);
      final u = localUiOf(a);
      expect([u.lx, u.ly, u.cx, u.cy, u.tx, u.ty, u.a], [-0.55, 0.2, 0.3, 0.45, 0, 0, 0.75]);
      // the new pad is not in the old edit: neither read nor written
      expect(u.toJson().containsKey('tx'), isFalse);
      expect(u.toJson().containsKey('ty'), isFalse);
      // writing it back and reading it again: same parameters, same pad state
      final back = LocalList.fromJson(jsonDecode(jsonEncode(list.toAppJson()))).items.single;
      expect(back.ops, a.ops);
      expect(back.strength, a.strength);
      expect(back.ui!.same(a.ui!), isTrue);
      // what the engine is sent is exactly the saved "adjust"
      final sent = (jsonDecode(jsonEncode(list.toEngineJson()))['items'][0]['adjust'] as Map).map((k, v) => MapEntry(k as String, (v as num).toDouble()));
      expect(sent, a.ops);
    });

    test('opening it and touching nothing (the pad state applied again) gives the same ops and strength', () {
      final a = LocalList.fromJson(jsonDecode(savedBefore)).items.single;
      final again = applyLocalUi(a, localUiOf(a));
      expect(again.ops, a.ops);
      expect(again.strength, a.strength);
    });

    test('a pad state without the texture pad is a recipe with no clarity, texture or dehaze of its own', () {
      final r = localRecipe(const LocalUi(lx: -0.55, ly: 0.2, cx: 0.3, cy: 0.45, a: 0.75));
      expect([r['clarity'], r['texture'], r['dehaze']], [0, 0, 0]);
    });
  });

  group('texture pad recipe', () {
    test('柔 - 锐 is clarity and texture; 朦胧 - 通透 is dehaze; the strip above the middle scales them', () {
      var r = localRecipe(const LocalUi(tx: 0.5));
      expect([r['clarity'], r['texture'], r['dehaze']], [20, 10, 0]);
      r = localRecipe(const LocalUi(tx: -1, ty: 1));
      expect([r['clarity'], r['texture'], r['dehaze']], [-40, -20, 35]);
      r = localRecipe(const LocalUi(ty: -1, a: 1));
      expect(r['dehaze'], -70, reason: 'the top of the strip doubles the recipe');
    });

    test('applyLocalUi writes the ops of the texture pad and keeps the other pads', () {
      final a = LocalAdjustment(id: 'x', mask: const [], ui: LocalUi.centre);
      final b = applyLocalUi(a, const LocalUi(lx: 0.2, tx: 1, ty: -0.4));
      expect(b.op('clarity'), 40);
      expect(b.op('texture'), 20);
      expect(b.op('dehaze'), -14);
      expect(b.op('exposure'), 6);
      expect(b.strength, 100);
      expect(localPadMoved(b.ui!, PadCat.texture), isTrue);
      expect(localPadMoved(b.ui!, PadCat.light), isTrue);
      expect(localPadMoved(b.ui!, PadCat.color), isFalse);
    });

    test('the pad state with a texture position round-trips through the app JSON', () {
      final a = applyLocalUi(LocalAdjustment(id: 'x', mask: const [], ui: LocalUi.centre), const LocalUi(tx: 0.25, ty: -0.5));
      final back = LocalAdjustment.fromJson(a.toJson(withUi: true))!;
      expect([back.ui!.tx, back.ui!.ty], [0.25, -0.5]);
      expect(back.ops, a.ops);
    });

    test('readouts follow the pad that shows', () {
      const u = LocalUi(lx: -0.5, cx: 0.3, tx: 0.4, ty: -0.2);
      expect(localReadout(u, pad: PadCat.light), '强度 100% · 暗 50');
      expect(localReadout(u, pad: PadCat.color), '强度 100% · 暖 30');
      expect(localReadout(u, pad: PadCat.texture), '强度 100% · 锐 40 · 朦胧 20');
    });
  });

  group('the switch in the panel', () {
    testWidgets('one pad at a time: the segments swap the pad and the readout follows it', (t) async {
      final r = await openLocal(t);
      await addViaCard(t, 'radial');
      await settle(t, 8);
      expect(key('local-pad-light'), findsOneWidget);
      expect(key('local-pad-color'), findsNothing);
      expect(key('local-pad-texture'), findsNothing);
      // light: drag right (亮)
      var pad = t.getRect(key('local-pad-light'));
      var f = await Finger.down(t, pad.center);
      await f.drag(Offset(pad.width * 0.3, 0));
      await f.up();
      await settle(t, 2);
      expect(area(r).op('exposure'), greaterThan(0));
      expect(find.textContaining('亮'), findsWidgets);
      // colour
      await tapKey(t, 'local-seg-color');
      expect(key('local-pad-color'), findsOneWidget);
      expect(key('local-pad-light'), findsNothing);
      expect(find.textContaining('· 亮'), findsNothing, reason: 'the readout shows the visible pad only');
      pad = t.getRect(key('local-pad-color'));
      f = await Finger.down(t, pad.center);
      await f.drag(Offset(pad.width * 0.3, 0));
      await f.up();
      await settle(t, 2);
      expect(area(r).op('temp'), greaterThan(0));
      expect(find.textContaining('暖'), findsWidgets);
      // texture: 锐 (right) and 通透 (up)
      await tapKey(t, 'local-seg-texture');
      expect(key('local-pad-texture'), findsOneWidget);
      expect(key('local-pad-color'), findsNothing);
      expect(find.textContaining('暖'), findsNothing);
      pad = t.getRect(key('local-pad-texture'));
      f = await Finger.down(t, pad.center);
      await f.drag(Offset(pad.width * 0.3, 0));
      await f.up();
      await settle(t, 2);
      expect(find.textContaining('锐'), findsWidgets);
      expect(area(r).op('dehaze'), 0);
      // up = 通透 (dehaze); the readout drops the directions on a narrow header, the pad's cursor and the ops are the evidence here
      f = await Finger.down(t, pad.center);
      await f.drag(Offset(pad.width * 0.3, -pad.height * 0.3));
      await f.up();
      await settle(t, 2);
      final a = area(r);
      expect(a.op('clarity'), greaterThan(0));
      expect(a.op('texture'), greaterThan(0));
      expect(a.op('clarity'), closeTo(a.op('texture') * 2, 0.02), reason: 'clarity 40 : texture 20');
      expect(a.op('dehaze'), greaterThan(0));
      expect(a.op('exposure'), greaterThan(0), reason: 'the other pads keep their values');
      expect(a.op('temp'), greaterThan(0));
      // the dots: all three pads were moved; back to light, the readout is the light pad's again
      for (final c in ['light', 'color', 'texture']) {
        expect(key('local-seg-dot-$c'), findsOneWidget, reason: '$c pad is away from its centre');
      }
      await tapKey(t, 'local-seg-light');
      expect(key('local-pad-light'), findsOneWidget);
      expect(find.textContaining('锐'), findsNothing);
      expect(t.takeException(), isNull);
    });

    testWidgets('a dot only on a segment whose pad is not neutral; the texture pad is one undo step', (t) async {
      final r = await openLocal(t);
      await addViaCard(t, 'radial');
      await settle(t, 8);
      for (final c in ['light', 'color', 'texture']) {
        expect(key('local-seg-dot-$c'), findsNothing);
      }
      await tapKey(t, 'local-seg-texture');
      final pad = t.getRect(key('local-pad-texture'));
      final f = await Finger.down(t, pad.center);
      await f.drag(Offset(-pad.width * 0.3, 0));
      await f.up();
      await settle(t, 2);
      expect(area(r).op('clarity'), lessThan(0), reason: '柔 on the left');
      expect(key('local-seg-dot-texture'), findsOneWidget);
      expect(key('local-seg-dot-light'), findsNothing);
      expect(key('local-seg-dot-color'), findsNothing);
      r.app.undo(r.id);
      await settle(t, 2);
      expect(area(r).op('clarity'), 0);
      expect(key('local-seg-dot-texture'), findsNothing);
    });

    testWidgets('segments are 48 dp targets with their word, selected state and the dot in the label', (t) async {
      final r = await openLocal(t);
      await addViaCard(t, 'radial');
      await settle(t, 8);
      for (final c in ['light', 'color', 'texture']) {
        final s = t.getSize(key('local-seg-$c'));
        expect(s.height, greaterThanOrEqualTo(48), reason: c);
        expect(s.width, greaterThanOrEqualTo(48), reason: c);
      }
      final h = t.ensureSemantics();
      expect(find.bySemanticsLabel('颜色'), findsOneWidget);
      await tapKey(t, 'local-seg-color');
      final pad = t.getRect(key('local-pad-color'));
      final f = await Finger.down(t, pad.center);
      await f.drag(Offset(pad.width * 0.3, 0));
      await f.up();
      await settle(t, 2);
      expect(find.bySemanticsLabel('颜色，已调整'), findsOneWidget);
      expect(r.lc.padCat, PadCat.color);
      h.dispose();
    });

    testWidgets('the switch shares the chips line on a roomy phone and takes a line of its own with several areas', (t) async {
      final r = await openLocal(t);
      await addViaCard(t, 'radial');
      await settle(t, 8);
      final chips = t.getRect(key('local-chip-0'));
      final seg = t.getRect(key('local-seg-light'));
      expect(seg.center.dy, closeTo(chips.center.dy, 1), reason: 'one area: the switch is on the chips line');
      expect(seg.left, greaterThan(chips.right));
      // many areas: no room beside the chips
      r.app.applyEdit(r.id, (e) => e.copyWith(local: LocalList([for (var i = 0; i < 6; i++) LocalAdjustment(id: 'x$i', mask: const [], ui: LocalUi.centre)])));
      await tapKey(t, 'local-chip-0');
      await settle(t, 10);
      final c0 = t.getRect(key('local-chip-0'));
      final s2 = t.getRect(key('local-seg-light'));
      expect(s2.top, greaterThanOrEqualTo(c0.bottom - 1), reason: 'the switch is under the chips');
      expect(t.takeException(), isNull);
    });
  });
}
