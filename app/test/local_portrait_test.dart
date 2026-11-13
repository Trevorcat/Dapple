// 人像 on the real editor (docs/design/local-adjustments.md section 9.1, docs/engine/pipeline-architecture.md section 43): the tile, the analysis,
// the default set in one undo step, the chips (add, select, remove), the strip, 分项, 点一下祛斑, 对比, several faces, re-analysis and the
// layout across window shapes. The AI service is the fully controllable [FakeLocalAi] of local_ai_test.dart.
import 'dart:convert';

import 'package:dapple/model/local_adjust.dart';
import 'package:dapple/model/local_kinds.dart' show AiBits;
import 'package:dapple/model/portrait.dart';
import 'package:dapple/screens/editor/local_controller.dart' show LocalBody;
import 'package:dapple/services/local_ai.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show Finger, n5Dpr, n5Phys, phoneDpr, phonePhys, settle;
import 'local_ai_test.dart' show FakeLocalAi, open, plane, planeId, result, tapPhoto;
import 'local_panel_test.dart' show Rig, back, frameRect, key, ownerDpr, ownerPhys, tapKey;

const _defaultIds = ['pt-skin', 'pt-undereye', 'pt-shine', 'pt-teeth', 'pt-eyewhite', 'pt-iris', 'pt-lips'];

/// A portrait analysis: [faces] faces, each frontal (eye features) unless [frontal] is false, with teeth when [teeth].
AiJobResult portraitResult({int faces = 1, bool teeth = true, bool frontal = true, bool body = false, bool person = true}) => result(
      person: person,
      faces: faces,
      planes: [
        plane('person'),
        plane('faceskin'),
        if (body) plane('bodyskin'),
        plane('hair'),
        for (var n = 1; n <= faces; n++) ...[
          plane('f$n:face', face: n, group: 'faces'),
          if (frontal) ...[
            for (final k in ['eyes', 'iris', 'sclera', 'lips', 'brows', 'undereye']) plane('f$n:$k', face: n, group: 'faces'),
            if (teeth) plane('f$n:teeth', face: n, group: 'faces'),
          ],
        ],
      ],
    );

/// Opens 人像 and finishes the analysis with [r].
Future<Rig> openPortrait(WidgetTester t, FakeLocalAi fake, AiJobResult r, {bool dark = false, Size? phys, double? dpr, int faces = 1, double iod = 0.125}) async {
  for (var n = 1; n <= faces; n++) {
    fake.setFace(n, iod: iod, cx: n / (faces + 1));
  }
  final rig = await open(t, fake, dark: dark, phys: phys, dpr: dpr);
  await tapKey(t, 'kind-portrait');
  fake.finish(r);
  await settle(t, 4);
  return rig;
}

/// The header buttons (对比, 分项) and the tool strip (点一下祛斑) belong to the pads body: 返回 from the chips.
Future<void> toPads(WidgetTester t, Rig r) async {
  r.lc.showBody(LocalBody.pads);
  await settle(t, 3);
}

List<String> ids(Rig r) => [for (final a in r.local.items) a.id];
LocalAdjustment byId(Rig r, String id) => r.local.byId(id)!;

void main() {
  group('the tile and the analysis', () {
    testWidgets('a device without AI has no 人像 tile', (t) async {
      final off = FakeLocalAi()..avail = const AiAvailability();
      await open(t, off);
      expect(key('kind-radial'), findsOneWidget);
      expect(key('kind-portrait'), findsNothing);
    });

    testWidgets('the 人像 tile needs AI; "no person" takes it away again', (t) async {
      final fake = FakeLocalAi();
      final r = await open(t, fake);
      expect(key('kind-portrait'), findsOneWidget);
      expect(find.text('人像'), findsOneWidget);
      await tapKey(t, 'kind-portrait');
      fake.finish(portraitResult(person: false, faces: 0));
      await settle(t, 3);
      expect(key('portrait-none'), findsOneWidget);
      expect(find.text('没找到人'), findsOneWidget);
      expect(key('chip-skin'), findsNothing);
      expect(r.local.isEmpty, isTrue);
      await back(t);
      expect(key('kind-portrait'), findsNothing);
      expect(key('kind-person'), findsOneWidget);
      expect(t.takeException(), isNull);
    });

    testWidgets('progress while the analysis runs, cancel goes back to the shape card, then the chips', (t) async {
      final fake = FakeLocalAi();
      final r = await open(t, fake);
      await tapKey(t, 'kind-portrait');
      expect(fake.runs, [AiBits.person | AiBits.faces]);
      expect(key('portrait-progress'), findsOneWidget);
      expect(key('chip-skin'), findsNothing);
      fake.progress(0.5);
      await settle(t, 2);
      expect(t.widget<CircularProgressIndicator>(find.descendant(of: key('portrait-progress'), matching: find.byType(CircularProgressIndicator))).value, closeTo(0.5, 1e-9));
      await tapKey(t, 'portrait-cancel');
      expect(fake.cancels, 1);
      expect(key('portrait-progress'), findsNothing);
      expect(key('kind-portrait'), findsOneWidget);
      expect(r.local.isEmpty, isTrue);

      await tapKey(t, 'kind-portrait');
      expect(fake.runs, hasLength(2));
      fake.finish(portraitResult());
      await settle(t, 4);
      expect(key('portrait-progress'), findsNothing);
      for (final c in ['skin', 'undereye', 'shine', 'teeth', 'eyes', 'lips', 'light']) {
        expect(key('chip-$c'), findsOneWidget, reason: c);
      }
      expect(find.text('人像'), findsOneWidget, reason: 'the header title');
      expect(t.takeException(), isNull);
    });
  });

  group('the default set', () {
    testWidgets('exactly the default chips (no 补光) in ONE undo step, the face scale from the planes, skin selected', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult());
      expect(ids(r), _defaultIds);
      expect(r.lc.selId, 'pt-skin');
      for (final a in r.local.items) {
        expect(a.name, startsWith('portrait:'));
        expect(a.strength, 100);
        expect(a.face, closeTo(0.125, 1e-9), reason: a.id);
        expect(a.mask.every((m) => m.kind == MaskKindId.raster), isTrue);
      }
      expect(byId(r, 'pt-skin').ops, {'smooth': 30, 'even': 25, 'redness': 30, 'heal': 50});
      expect(byId(r, 'pt-skin').mask.map((m) => m.ref), [planeId('faceskin'), planeId('f1:lips'), planeId('f1:teeth'), planeId('f1:eyes'), planeId('f1:brows')]);
      expect(r.local.rasterPlanes, lessThanOrEqualTo(kMaxRasterComponents));
      expect(r.local.toEngineJson()!['v'], 2);
      expect(r.lc.chipOn(PortraitChip.eyes), isTrue);
      expect(r.lc.chipOn(PortraitChip.light), isFalse);
      expect(key('local-handles'), findsOneWidget);
      r.app.undo(r.id);
      expect(r.local.isEmpty, isTrue, reason: 'one undo step');
      expect(t.takeException(), isNull);
    });

    testWidgets('opening it again does not duplicate or reset anything', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult());
      await tapKey(t, 'chip-undereye');
      await tapKey(t, 'chip-skin');
      final before = [for (final a in r.local.items) a.toJson()];
      r.lc.openPortrait();
      await settle(t, 3);
      expect(ids(r), _defaultIds);
      expect([for (final a in r.local.items) a.toJson()], before);
      expect(fake.runs, hasLength(1), reason: 'the analysis is cached');
      // back to the shape card and in again
      await back(t);
      await tapKey(t, 'local-add');
      await tapKey(t, 'kind-portrait');
      expect(ids(r), _defaultIds);
      expect(fake.runs, hasLength(1));
    });

    testWidgets('a closed mouth: no teeth chip and no pt-teeth area', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult(teeth: false));
      expect(key('chip-teeth'), findsNothing);
      expect(ids(r), [for (final i in _defaultIds) if (i != 'pt-teeth') i]);
      expect(key('chip-lips'), findsOneWidget);
      expect(byId(r, 'pt-skin').mask.map((m) => m.ref), isNot(contains(planeId('f1:teeth'))));
    });

    testWidgets('a turned face (face plane only): skin, shine and light chips; the default set is skin and shine', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult(frontal: false));
      expect([for (final c in PortraitChip.values) if (key('chip-${c.name}').evaluate().isNotEmpty) c], [PortraitChip.skin, PortraitChip.shine, PortraitChip.light]);
      expect(ids(r), ['pt-skin', 'pt-shine']);
      expect(key('face-1'), findsNothing);
    });
  });

  group('chips', () {
    testWidgets('tapping 补光 adds pt-light and selects it, in one undo step', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult());
      await tapKey(t, 'chip-light');
      expect(ids(r), [..._defaultIds, 'pt-light']);
      expect(r.lc.selId, 'pt-light');
      expect(byId(r, 'pt-light').ops, {'facelight': 30});
      expect(byId(r, 'pt-light').mask.single.ref, planeId('f1:face'));
      r.app.undo(r.id);
      expect(ids(r), _defaultIds);
    });

    testWidgets('on + another selected: select; on + selected: remove with the toast; undo brings it back', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult());
      expect(r.lc.selChip, PortraitChip.skin);
      await tapKey(t, 'chip-shine');
      expect(r.lc.selChip, PortraitChip.shine);
      expect(ids(r), _defaultIds, reason: 'selecting changes nothing');
      await tapKey(t, 'chip-shine');
      expect(ids(r), [for (final i in _defaultIds) if (i != 'pt-shine') i]);
      expect(r.app.toast?.message, '已去掉油光');
      expect(r.app.toast?.actionLabel, '撤销');
      r.app.toast!.onAction!();
      await settle(t, 3);
      expect(ids(r), _defaultIds);
    });

    testWidgets('眼睛 is two areas; removing the chip removes both and adding makes both again', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult());
      expect(ids(r), containsAll(['pt-eyewhite', 'pt-iris']));
      await tapKey(t, 'chip-eyes');
      expect(r.lc.selChip, PortraitChip.eyes);
      await tapKey(t, 'chip-eyes');
      expect(ids(r), isNot(contains('pt-eyewhite')));
      expect(ids(r), isNot(contains('pt-iris')));
      expect(r.app.toast?.message, '已去掉眼睛');
      expect(r.local.length, 5);
      await tapKey(t, 'chip-eyes');
      expect(ids(r), containsAll(['pt-eyewhite', 'pt-iris']));
      expect(r.local.length, 7);
      expect(ids(r).toSet(), hasLength(7), reason: 'no duplicates');
      expect(byId(r, 'pt-eyewhite').mask.single.ref, planeId('f1:sclera'));
      expect(byId(r, 'pt-iris').mask.single.ref, planeId('f1:iris'));
      expect(r.lc.selId, 'pt-eyewhite');
    });

    testWidgets('removing 肤质 while heal is on leaves heal mode', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult());
      r.lc.setHealMode(true);
      await settle(t, 2);
      await tapKey(t, 'chip-skin');
      expect(r.lc.healMode, isFalse);
      expect(ids(r), isNot(contains('pt-skin')));
    });
  });

  group('the strip', () {
    testWidgets('dragging the strip scales the selected chip: top = maximum, bottom = 0, strength stays 100', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult());
      final strip = t.getRect(key('strip-local'));
      final g = await Finger.down(t, strip.topCenter + const Offset(0, 2));
      await g.up();
      var a = byId(r, 'pt-skin');
      expect(a.strength, 100);
      expect(a.ops['smooth'], closeTo(100, 6));
      expect(a.ops['even'], closeTo(60, 4));
      expect(a.ops['texture'], greaterThan(15));
      expect(byId(r, 'pt-shine').ops, {'shine': 30}, reason: 'other chips untouched');

      final f = await Finger.down(t, strip.bottomCenter - const Offset(0, 2));
      await f.up();
      a = byId(r, 'pt-skin');
      expect(a.strength, 100, reason: 'a portrait strip never lowers the engine strength');
      expect(a.ops['smooth'] ?? 0, lessThan(5));
      expect(r.local.length, 7, reason: 'the area stays');

      final m = await Finger.down(t, strip.center);
      await m.up();
      a = byId(r, 'pt-skin');
      expect(a.ops['smooth'], closeTo(30, 6));

      await tapKey(t, 'chip-lips');
      expect(byId(r, 'pt-lips').ops, {'saturation': 10});
      final s2 = t.getRect(key('strip-local'));
      final h = await Finger.down(t, s2.topCenter + const Offset(0, 2));
      await h.up();
      expect(byId(r, 'pt-lips').ops['saturation'], closeTo(30, 2));
      expect(byId(r, 'pt-lips').strength, 100);
      expect(byId(r, 'pt-skin').ops['smooth'], closeTo(30, 6), reason: 'the skin keeps its position');
    });
  });

  group('分项', () {
    testWidgets('one radar with the recipe ops of 肤质 and 祛斑灵敏度; sens is written to retouch.sens', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult());
      await toPads(t, r);
      await tapKey(t, 'items-button');
      expect(key('local-radar-pt-skin'), findsOneWidget);
      expect(key('local-group-detail'), findsNothing, reason: 'no groups for a portrait area');
      final items = r.lc.portraitItems();
      expect([for (final i in items) i.label], ['磨皮', '匀肤', '去红', '祛斑', '纹理', '祛斑灵敏度']);
      expect([for (final i in items) i.max], [100, 60, 70, 100, 20, 100]);
      expect(items.firstWhere((i) => i.id == 'texture').min, -20);
      for (final w in ['磨皮', '匀肤', '去红', '祛斑', '纹理', '祛斑灵敏度']) {
        expect(find.text(w), findsWidgets, reason: w);
      }
      expect(byId(r, 'pt-skin').sens, 25);
      final sens = items.last;
      r.lc.items.begin(sens);
      r.lc.items.set(sens, 70);
      r.lc.items.end();
      await settle(t, 2);
      expect(byId(r, 'pt-skin').sens, 70);
      final j = jsonDecode(jsonEncode(r.local.toEngineJson())) as Map;
      final skin = (j['items'] as List).firstWhere((i) => (i as Map)['id'] == 'pt-skin') as Map;
      expect((skin['retouch'] as Map)['sens'], 70);
      expect((skin['retouch'] as Map)['face'], closeTo(0.125, 1e-9));
      expect(r.lc.items.actual(sens), 70);

      // an op is clamped to the recipe maximum
      final even = items[1];
      r.lc.items.begin(even);
      r.lc.items.set(even, 90);
      r.lc.items.end();
      expect(byId(r, 'pt-skin').ops['even'], 60);
      expect(byId(r, 'pt-skin').strength, 100);
      expect(t.takeException(), isNull);

      // other chips: their own ops, no sensitivity
      await tapKey(t, 'items-button');
      await tapKey(t, 'chip-lips');
      await tapKey(t, 'items-button');
      expect(key('local-radar-pt-lips'), findsOneWidget);
      expect([for (final i in r.lc.portraitItems()) i.label], ['唇色']);
      expect(t.takeException(), isNull);
    });
  });

  group('点一下祛斑', () {
    testWidgets('heal mode: two taps add two spots (one undo step each), a tap on a spot removes it, back leaves heal mode first', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult());
      await toPads(t, r);
      expect(key('heal-mode'), findsOneWidget);
      expect(key('heal-size-0'), findsNothing, reason: 'sizes only while on');
      await tapKey(t, 'heal-mode');
      expect(r.lc.healMode, isTrue);
      for (var i = 0; i < 4; i++) {
        expect(key('heal-size-$i'), findsOneWidget);
      }
      await tapKey(t, 'heal-size-2');
      expect(r.lc.healSize, 2);
      await tapKey(t, 'heal-size-1');

      await tapPhoto(t);
      expect(byId(r, 'pt-skin').spots, hasLength(1));
      final s0 = byId(r, 'pt-skin').spots.single;
      expect(s0.x, closeTo(0.5, 0.05));
      expect(s0.r, closeTo(healRadius(0.125, 1), 1e-9));
      expect(s0.hasSource, isFalse);
      await tapPhoto(t, by: const Offset(50, 40));
      expect(byId(r, 'pt-skin').spots, hasLength(2));
      expect(byId(r, 'pt-skin').spots[1].x, isNot(closeTo(s0.x, 0.02)));

      r.app.undo(r.id);
      expect(byId(r, 'pt-skin').spots, hasLength(1), reason: 'one undo step per spot');
      await tapPhoto(t, by: const Offset(50, 40));
      expect(byId(r, 'pt-skin').spots, hasLength(2));

      await tapPhoto(t, by: const Offset(50, 40));
      expect(byId(r, 'pt-skin').spots, hasLength(1), reason: 'a tap on a spot removes it');
      expect(byId(r, 'pt-skin').spots.single.x, closeTo(s0.x, 1e-9));
      expect(r.local.toEngineJson()!['v'], 2);
      expect(jsonEncode(r.local.toEngineJson()), contains('"spots":[['));
      expect(byId(r, 'pt-skin').ops['heal'], 50);

      await back(t);
      expect(r.lc.healMode, isFalse, reason: 'back leaves heal mode first');
      expect(r.local.length, 7);
      expect(byId(r, 'pt-skin').spots, hasLength(1));
      await tapPhoto(t, by: const Offset(-40, 30));
      expect(byId(r, 'pt-skin').spots, hasLength(1), reason: 'taps do nothing once the mode is off');
      expect(t.takeException(), isNull);
    });

    testWidgets('the heal tool is only on 肤质', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult());
      await toPads(t, r);
      await tapKey(t, 'chip-lips');
      expect(key('heal-mode'), findsNothing);
      await tapKey(t, 'chip-skin');
      expect(key('heal-mode'), findsOneWidget);
    });
  });

  group('对比', () {
    testWidgets('holding 对比 hides every local area and the overlay; letting go restores', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult());
      await toPads(t, r);
      final eng = r.lc.ctl.engine;
      expect(eng.hideLocal, isFalse);
      expect(r.lc.comparing, isFalse);
      final f = await Finger.down(t, t.getCenter(key('local-compare')));
      await t.pump(const Duration(milliseconds: 50));
      expect(r.lc.comparing, isTrue);
      expect(eng.hideLocal, isTrue);
      expect(r.lc.overlayOn.value, isFalse);
      expect(r.local.length, 7, reason: 'the edit itself is untouched');
      expect(t.takeException(), isNull);
      await f.up();
      await settle(t, 3);
      expect(r.lc.comparing, isFalse);
      expect(eng.hideLocal, isFalse);
      expect(r.local.length, 7);
      expect(t.takeException(), isNull);
    });
  });

  group('several faces', () {
    testWidgets('two frontal faces: the selector appears; toggling a face rebuilds the mask of the selected chip', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult(faces: 2, teeth: false), faces: 2);
      expect(ids(r), [for (final i in _defaultIds) if (i != 'pt-teeth') i]);
      for (final n in [1, 2]) {
        expect(key('face-$n'), findsOneWidget, reason: '$n');
      }
      expect(r.lc.selChip, PortraitChip.skin);
      expect(r.lc.selFaces, {1, 2});
      final before = byId(r, 'pt-skin').mask.map((m) => m.ref).toList();
      expect(before, [planeId('faceskin'), planeId('f1:lips'), planeId('f2:lips')]);
      expect(before.any((r) => r.endsWith(':eyes')), isFalse, reason: 'several faces: no eyes / brows');

      await tapKey(t, 'face-2');
      expect(r.lc.selFaces, {1});
      final after = byId(r, 'pt-skin');
      expect(facesOfMask(after, r.lc.portraitFaces), {1});
      expect(after.mask.where((m) => m.op == MaskOp.subtract && m.ref == planeId('f2:face')), hasLength(1));
      expect(after.mask.map((m) => m.ref), isNot(contains(planeId('f2:lips'))));
      expect(after.mask.map((m) => m.ref), isNot(contains(planeId('f1:eyes'))), reason: 'a several-face photo: eyes / brows stay (the plane budget)');
      expect(after.id, 'pt-skin');
      expect(r.local.length, 6, reason: 'no new area');
      expect(after.face, closeTo(0.125, 1e-9));
      expect(byId(r, 'pt-undereye').mask, hasLength(2), reason: 'other chips keep their faces');
      expect(after.ops, byId(r, 'pt-skin').ops);

      await tapKey(t, 'face-2');
      expect(r.lc.selFaces, {1, 2});
      expect(byId(r, 'pt-skin').mask.map((m) => m.ref).toList(), before);

      // at least one face stays
      await tapKey(t, 'face-2');
      await tapKey(t, 'face-1');
      expect(r.lc.selFaces, {1}, reason: 'at least one face stays');

      // a feature chip: its own plane per face
      await tapKey(t, 'chip-lips');
      expect(r.lc.selChip, PortraitChip.lips);
      expect(r.lc.selFaces, {1, 2});
      await tapKey(t, 'face-2');
      expect(byId(r, 'pt-lips').mask.map((m) => m.ref), [planeId('f1:lips')]);
      expect(byId(r, 'pt-skin').mask.map((m) => m.ref), contains(planeId('f2:face')), reason: 'the skin keeps its faces (face 2 was taken out of it above)');
      expect(t.takeException(), isNull);
    });

    testWidgets('two faces with open mouths: 补光 would be the 13th plane: refused with the toast, nothing changes', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult(faces: 2), faces: 2);
      expect(r.local.rasterPlanes, 11);
      await tapKey(t, 'chip-light');
      expect(r.app.toast?.message, contains('12'));
      expect(ids(r), _defaultIds, reason: '补光 would be the 13th plane');
      expect(r.local.rasterPlanes, lessThanOrEqualTo(kMaxRasterComponents));
      expect(t.takeException(), isNull);
    });

    testWidgets('two faces with open mouths: the face selector works at the default set', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult(faces: 2), faces: 2);
      await tapKey(t, 'face-2');
      expect(r.lc.selFaces, {1});
    });

    testWidgets('three faces: the plane budget (12) is exceeded, the user gets the toast and nothing is half-made', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult(faces: 3), faces: 3);
      expect(r.local.rasterPlanes, lessThanOrEqualTo(kMaxRasterComponents));
      expect(r.app.toast?.message, contains('12'));
      expect(t.takeException(), isNull);
    });

    testWidgets('three faces: at least the skin is made (a group photo is the common case)', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult(faces: 3), faces: 3);
      expect(ids(r), contains('pt-skin'));
      expect(r.local.rasterPlanes, lessThanOrEqualTo(kMaxRasterComponents));
    });

    testWidgets('right after opening 人像 the header has 对比 and 分项 and 肤质 has its heal tool', (t) async {
      final fake = FakeLocalAi();
      await openPortrait(t, fake, portraitResult());
      expect(key('local-compare'), findsOneWidget);
      expect(key('items-button'), findsOneWidget);
      expect(key('heal-mode'), findsOneWidget);
    });

    testWidgets('a single frontal face: no face selector', (t) async {
      final fake = FakeLocalAi();
      await openPortrait(t, fake, portraitResult());
      expect(key('face-1'), findsNothing);
    });

    testWidgets('two faces with only one frontal: no selector', (t) async {
      final fake = FakeLocalAi();
      final res = result(faces: 2, planes: [
        plane('person'),
        plane('faceskin'),
        plane('f1:face', face: 1, group: 'faces'),
        plane('f1:eyes', face: 1, group: 'faces'),
        plane('f1:iris', face: 1, group: 'faces'),
        plane('f1:sclera', face: 1, group: 'faces'),
        plane('f2:face', face: 2, group: 'faces'),
      ]);
      final r = await openPortrait(t, fake, res, faces: 2);
      expect(key('face-1'), findsNothing);
      expect(key('face-2'), findsNothing);
      expect(r.lc.frontalFaces, hasLength(1));
    });
  });

  group('analysing again', () {
    testWidgets('a fresh analysis and re-applying keep the same area ids (no duplicates)', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult());
      final first = ids(r);
      // the second pass: the same photo analysed again, now with a second face
      fake.setFace(2, iod: 0.1, cx: 0.8);
      r.lc.aiDone = 0;
      r.lc.openPortrait();
      await settle(t, 2);
      expect(fake.runs, hasLength(2));
      expect(key('portrait-progress'), findsOneWidget);
      fake.finish(portraitResult(faces: 2));
      await settle(t, 4);
      expect(ids(r), first, reason: 'the chips exist: nothing is created again');
      expect(r.lc.portraitFaces, hasLength(2));
      expect(key('face-2'), findsOneWidget);
      await tapKey(t, 'face-2');
      expect(ids(r), first);
      expect(r.lc.selFaces, {1});
      // every chip off and on again
      await tapKey(t, 'chip-undereye');
      await tapKey(t, 'chip-undereye');
      await tapKey(t, 'chip-undereye');
      await tapKey(t, 'chip-light');
      final all = ids(r);
      expect(all.toSet(), hasLength(all.length), reason: 'never duplicated');
      expect(all, containsAll(first));
      expect(all.where((i) => i == 'pt-undereye'), hasLength(1));
      expect(t.takeException(), isNull);
    });
  });

  group('layout across window shapes', () {
    final windows = <(String, Size, double)>[
      ('phone', phonePhys, phoneDpr),
      ('owner fold outer', ownerPhys, ownerDpr),
      ('fold inner (side pane)', n5Phys, n5Dpr),
      ('landscape phone (side pane)', const Size(2400, 1080), 2.75),
      ('tablet', const Size(2560, 1600), 2),
    ];
    for (final (name, phys, dpr) in windows) {
      for (final dark in [false, true]) {
        testWidgets('$name ${dark ? 'dark' : 'light'}: progress, chips, faces, heal tools, 分项, 对比 without exceptions', (t) async {
          final fake = FakeLocalAi();
          for (var n = 1; n <= 2; n++) {
            fake.setFace(n, iod: 0.1, cx: n / 3);
          }
          final r = await open(t, fake, dark: dark, phys: phys, dpr: dpr);
          await tapKey(t, 'kind-portrait');
          fake.progress(0.4);
          await settle(t, 2);
          expect(key('portrait-progress'), findsOneWidget);
          expect(t.takeException(), isNull, reason: 'progress');
          fake.finish(portraitResult(faces: 2, body: true, teeth: false));
          await settle(t, 4);
          expect(key('chip-skin'), findsOneWidget);
          expect(key('face-2'), findsOneWidget);
          expect(t.takeException(), isNull, reason: 'chips and faces');
          await tapKey(t, 'chip-lips');
          await tapKey(t, 'chip-skin');
          expect(t.takeException(), isNull, reason: 'chips');

          await tapKey(t, 'face-2');
          expect(t.takeException(), isNull, reason: 'face selector');
          await toPads(t, r);
          expect(key('heal-mode'), findsOneWidget);
          await tapKey(t, 'heal-mode');
          await tapKey(t, 'heal-size-3');
          await tapPhoto(t);
          await tapPhoto(t, by: const Offset(30, 20));
          expect(byId(r, 'pt-skin').spots, hasLength(2));
          expect(t.takeException(), isNull, reason: 'heal tools and rings');
          await tapKey(t, 'heal-mode');

          await tapKey(t, 'items-button');
          expect(key('local-radar-pt-skin'), findsOneWidget);
          expect(t.takeException(), isNull, reason: '分项');
          await tapKey(t, 'items-button');

          final f = await Finger.down(t, t.getCenter(key('local-compare')));
          await t.pump(const Duration(milliseconds: 50));
          expect(t.takeException(), isNull, reason: '对比');
          await f.up();
          await settle(t, 3);
          expect(t.takeException(), isNull, reason: 'after 对比');
          expect(frameRect(t).isEmpty, isFalse);
        });
      }
    }

    testWidgets('large text (1.6) with the chips, dark', (t) async {
      t.platformDispatcher.textScaleFactorTestValue = 1.6;
      addTearDown(t.platformDispatcher.clearTextScaleFactorTestValue);
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, portraitResult(faces: 2), dark: true, faces: 2);
      expect(r.local.length, 7);
      await toPads(t, r);
      await tapKey(t, 'items-button');
      expect(t.takeException(), isNull);
    });
  });
}
