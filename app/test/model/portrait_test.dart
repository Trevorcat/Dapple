// 人像 model (docs/engine/pipeline-architecture.md section 43): the recipes, which chips an analysis offers, the masks, the face scale, the
// heal radius, the 12-plane budget of a full default portrait and the JSON of its areas.
import 'dart:typed_data';

import 'package:dapple/model/local_adjust.dart';
import 'package:dapple/model/local_pad.dart';
import 'package:dapple/model/portrait.dart';
import 'package:flutter_test/flutter_test.dart';

String? pid(String k) => k == 'bodyskin' ? null : 'ai:h:$k'; // the body skin plane is absent unless a test says so
String? pidBody(String k) => 'ai:h:$k';

const _all = ['face', 'eyes', 'iris', 'sclera', 'lips', 'brows', 'teeth', 'undereye'];

Set<String> keysOf(Map<int, List<String>> faces, {bool skin = true, bool body = false}) =>
    {if (skin) 'faceskin', if (body) 'bodyskin', for (final e in faces.entries) for (final k in e.value) 'f${e.key}:$k'};

List<PortraitChip> chipsOf(Set<String> keys) => availableChips(keys, facesOf(keys));

Uint8List gray(int w, int h, bool Function(int x, int y) on) {
  final b = Uint8List(w * h * 4);
  for (var y = 0; y < h; y++) {
    for (var x = 0; x < w; x++) {
      final v = on(x, y) ? 255 : 0;
      final i = (y * w + x) * 4;
      b[i] = b[i + 1] = b[i + 2] = v;
      b[i + 3] = 255;
    }
  }
  return b;
}

PortraitAreaSpec area(String id) => [for (final s in portraitSpecs) ...s.areas].firstWhere((a) => a.id == id);

void main() {
  group('recipes', () {
    for (final s in portraitSpecs) {
      for (final ar in s.areas) {
        test('${ar.id}: strip 0 -> 0, 0.5 -> default, 1 -> maximum', () {
          final r0 = portraitRecipe(ar, 0), rm = portraitRecipe(ar, 0.5), r1 = portraitRecipe(ar, 1);
          for (final o in ar.ops) {
            expect(r0[o.key], 0, reason: o.key);
            expect(rm[o.key], closeTo(o.def, 1e-9), reason: o.key);
            expect(r1[o.key], closeTo(o.max, 1e-9), reason: o.key);
            expect(portraitRecipe(ar, 0.25)[o.key], closeTo(o.def / 2, 1e-9), reason: o.key);
            expect(portraitRecipe(ar, 2)[o.key], closeTo(o.max, 1e-9), reason: 'clamped above');
          }
        });
      }
    }

    test('only 补光 is off by default; the stable ids', () {
      expect([for (final s in portraitSpecs) if (!s.onByDefault) s.chip], [PortraitChip.light]);
      expect([for (final s in portraitSpecs) ...s.areas.map((a) => a.id)], ['pt-skin', 'pt-undereye', 'pt-shine', 'pt-teeth', 'pt-eyewhite', 'pt-iris', 'pt-lips', 'pt-light']);
      expect(specOf(PortraitChip.eyes).areas, hasLength(2));
    });
  });

  group('availableChips', () {
    test('(a) a frontal face with an open mouth: every chip', () {
      expect(chipsOf(keysOf({1: _all})), PortraitChip.values);
    });

    test('(b) closed mouth: no teeth chip', () {
      final c = chipsOf(keysOf({1: [for (final k in _all) if (k != 'teeth') k]}));
      expect(c, isNot(contains(PortraitChip.teeth)));
      expect(c, hasLength(PortraitChip.values.length - 1));
      expect(c, contains(PortraitChip.lips));
    });

    test('(c) a turned face (the face plane only): skin, shine, light', () {
      expect(chipsOf(keysOf({1: ['face']})), [PortraitChip.skin, PortraitChip.shine, PortraitChip.light]);
    });

    test('(d) no faces but face skin: skin and shine only', () {
      expect(chipsOf({'faceskin', 'person'}), [PortraitChip.skin, PortraitChip.shine]);
    });

    test('(e) three faces, none frontal: skin, shine, light', () {
      expect(chipsOf(keysOf({1: ['face'], 2: ['face'], 3: ['face']})), [PortraitChip.skin, PortraitChip.shine, PortraitChip.light]);
    });

    test('no skin plane: the skin chips are gone, the feature chips stay', () {
      final c = chipsOf(keysOf({1: _all}, skin: false));
      expect(c, isNot(contains(PortraitChip.skin)));
      expect(c, isNot(contains(PortraitChip.shine)));
      expect(c, contains(PortraitChip.eyes));
    });

    test('eyes needs both sclera and iris', () {
      expect(chipsOf(keysOf({1: ['face', 'eyes', 'sclera']})), isNot(contains(PortraitChip.eyes)));
    });

    test('nothing at all: no chip', () => expect(chipsOf(<String>{}), isEmpty));
  });

  group('facesOf', () {
    test('groups the keys by face, sorted, frontal = eye features, iods applied', () {
      final fs = facesOf(['f2:face', 'f1:face', 'f1:eyes', 'faceskin', 'f2:lips'], iods: {1: 0.1});
      expect([for (final f in fs) f.n], [1, 2]);
      expect(fs[0].frontal, isTrue);
      expect(fs[1].frontal, isFalse);
      expect(fs[0].iod, 0.1);
      expect(fs[1].iod, 0);
      expect(fs[1].keys, {'face', 'lips'});
    });
  });

  group('portraitMask', () {
    List<(String, MaskOp)> parts(List<MaskComponent> m) => [for (final c in m) (c.ref.substring(5), c.op)];
    final skin = area('pt-skin');

    test('one face: face skin and body skin added; lips, teeth, eyes, brows subtracted', () {
      final fs = facesOf(keysOf({1: _all}, body: true));
      final m = portraitMask(skin, fs, {1}, pidBody);
      expect(parts(m), [
        ('faceskin', MaskOp.add),
        ('bodyskin', MaskOp.add),
        ('f1:lips', MaskOp.subtract),
        ('f1:teeth', MaskOp.subtract),
        ('f1:eyes', MaskOp.subtract),
        ('f1:brows', MaskOp.subtract),
      ]);
      expect([for (final c in m) c.kind].every((k) => k == MaskKindId.raster), isTrue);
    });

    test('no body skin plane: skipped; no face skin: empty', () {
      final fs = facesOf(keysOf({1: _all}));
      expect(parts(portraitMask(skin, fs, {1}, pid)).map((e) => e.$1), ['faceskin', 'f1:lips', 'f1:teeth', 'f1:eyes', 'f1:brows']);
      expect(portraitMask(skin, fs, {1}, (k) => k == 'faceskin' ? null : pid(k)), isEmpty);
    });

    test('shine uses the same mask as skin', () {
      final fs = facesOf(keysOf({1: _all}, body: true));
      expect(parts(portraitMask(area('pt-shine'), fs, {1}, pidBody)), parts(portraitMask(skin, fs, {1}, pidBody)));
    });

    test('several faces: the mouths of all, no eyes / brows', () {
      final fs = facesOf(keysOf({1: _all, 2: _all}));
      final p = parts(portraitMask(skin, fs, {1, 2}, pid)).map((e) => e.$1).toList();
      expect(p, ['faceskin', 'f1:lips', 'f1:teeth', 'f2:lips', 'f2:teeth']);
    });

    test('a face that is not selected is taken out with its oval', () {
      final fs = facesOf(keysOf({1: _all, 2: _all}));
      final p = parts(portraitMask(skin, fs, {1}, pid));
      expect(p.last, ('f2:face', MaskOp.subtract));
      expect(p.map((e) => e.$1), isNot(contains('f2:lips')));
      expect(p.map((e) => e.$1), isNot(contains('f1:eyes')), reason: 'a several-face photo spends its planes on the mouths, not on eyes / brows');
    });

    test('at most 8 parts, the mouths first', () {
      final fs = facesOf(keysOf({for (var n = 1; n <= 4; n++) n: _all}, body: true));
      final m = portraitMask(skin, fs, {1, 2, 3, 4}, pidBody);
      expect(m, hasLength(8));
      expect(m.first.op, MaskOp.add);
      expect(parts(m).skip(2).map((e) => e.$1), ['f1:lips', 'f1:teeth', 'f2:lips', 'f2:teeth', 'f3:lips', 'f3:teeth']);
      final one = portraitMask(skin, facesOf(keysOf({for (var n = 1; n <= 5; n++) n: _all}, body: true)), {1}, pidBody);
      expect(one, hasLength(8));
      expect(one.where((c) => c.op == MaskOp.subtract).first.ref, endsWith('f1:lips'));
    });

    test('a feature chip: the plane of every selected face that has it', () {
      final fs = facesOf(keysOf({1: _all, 2: ['face', 'eyes', 'iris', 'sclera'], 3: _all}));
      final teeth = area('pt-teeth');
      expect(parts(portraitMask(teeth, fs, {1, 2, 3}, pid)), [('f1:teeth', MaskOp.add), ('f3:teeth', MaskOp.add)]);
      expect(parts(portraitMask(area('pt-iris'), fs, {2}, pid)), [('f2:iris', MaskOp.add)]);
      expect(portraitMask(teeth, fs, {2}, pid), isEmpty);
    });
  });

  group('facesOfMask', () {
    LocalAdjustment withMask(List<MaskComponent> m) => LocalAdjustment(id: 'x', mask: m);

    test('is the inverse of portraitMask (skin)', () {
      final fs = facesOf(keysOf({1: _all, 2: _all, 3: _all}));
      for (final sel in [
        {1, 2, 3},
        {1},
        {2, 3},
        {1, 3}
      ]) {
        expect(facesOfMask(withMask(portraitMask(area('pt-skin'), fs, sel, pid)), fs), sel, reason: '$sel');
      }
    });

    test('is the inverse of portraitMask (feature plane)', () {
      final fs = facesOf(keysOf({1: _all, 2: _all, 3: _all}));
      for (final sel in [
        {1, 2, 3},
        {2},
        {1, 3}
      ]) {
        expect(facesOfMask(withMask(portraitMask(area('pt-teeth'), fs, sel, pid)), fs), sel, reason: '$sel');
      }
    });

    test('an area without face planes: none', () {
      expect(facesOfMask(const LocalAdjustment(id: 'x'), facesOf(keysOf({1: _all}))), isEmpty);
    });
  });

  group('the face scale', () {
    test('iodFromPlane: two blobs 50 px apart on a 100 px plane = 0.5', () {
      bool blob(int x, int y, int cx, int cy) => (x - cx).abs() <= 2 && (y - cy).abs() <= 2;
      final p = gray(100, 50, (x, y) => blob(x, y, 25, 25) || blob(x, y, 75, 25));
      expect(iodFromPlane(p, 100, 50), closeTo(0.5, 1e-9));
    });

    test('iodFromPlane: tilted eyes count the vertical offset; a tall plane divides by the long edge', () {
      bool blob(int x, int y, int cx, int cy) => (x - cx).abs() <= 1 && (y - cy).abs() <= 1;
      final p = gray(40, 80, (x, y) => blob(x, y, 10, 30) || blob(x, y, 30, 50));
      expect(iodFromPlane(p, 40, 80), closeTo(28.2842712 / 80, 1e-6));
    });

    test('iodFromPlane: an empty plane, a single pixel column: 0', () {
      expect(iodFromPlane(gray(50, 50, (x, y) => x == 25), 50, 50), 0);
      expect(iodFromPlane(gray(50, 50, (x, y) => false), 50, 50), 0);
    });

    test('iodFromFaceOval: 0.42 x the width / long edge', () {
      final p = gray(100, 100, (x, y) => x >= 30 && x < 70);
      expect(iodFromFaceOval(p, 100, 100), closeTo(0.42 * 40 / 100, 1e-9));
      expect(iodFromFaceOval(gray(10, 10, (x, y) => false), 10, 10), 0);
    });

    test('portraitFaceScale: the LARGEST known iod of the frame (the engine retouches a frame at one scale, the largest face)', () {
      const fs = [PortraitFace(1, {'face'}, 0.12), PortraitFace(2, {'face'}, 0.08), PortraitFace(3, {'face'}, 0)];
      expect(portraitFaceScale(fs, {1, 2, 3}), 0.12);
      expect(portraitFaceScale(fs, {2}), 0.12, reason: 'one scale per frame, whatever faces the chip applies to');
      expect(portraitFaceScale(fs), 0.12);
      expect(portraitFaceScale(const [PortraitFace(1, {'face'}, 0)]), 0);
    });

    test('the skin mask takes out the open-mouth interior (f<N>:mouth) of every face that has it, with the lips and teeth', () {
      const fs = [PortraitFace(1, {'face', 'lips', 'teeth', 'mouth'}, 0.1), PortraitFace(2, {'face', 'lips'}, 0.05)];
      String? pid(String k) => 'ai:p:$k';
      final m = portraitMask(specOf(PortraitChip.skin).areas.first, fs, {1, 2}, pid);
      final out = [for (final c in m) if (c.op == MaskOp.subtract) c.ref];
      expect(out, containsAll(['ai:p:f1:lips', 'ai:p:f1:teeth', 'ai:p:f1:mouth', 'ai:p:f2:lips']));
      expect(out, isNot(contains('ai:p:f2:mouth')), reason: 'face 2 has no open mouth');
    });

    test('healRadius: sizes grow, clamped to 0.0005 .. 0.02, unknown face = 0.06', () {
      expect(healRadius(0.1, 0), closeTo(0.0025, 1e-12));
      expect(healRadius(0.1, 3), closeTo(0.009, 1e-12));
      final v = [for (var i = 0; i < 4; i++) healRadius(0.1, i)];
      expect(v, orderedEquals([...v]..sort()));
      expect(healRadius(1, 3), 0.02);
      expect(healRadius(0.001, 0), 0.0005);
      expect(healRadius(0, 1), closeTo(0.06 * 0.04, 1e-12));
      expect(healRadius(0.1, 9), healRadius(0.1, 3));
      expect(healRadius(0.1, -2), healRadius(0.1, 0));
    });
  });

  group('a full default portrait', () {
    LocalList build({bool light = false, double face = 0.11}) {
      final fs = facesOf(keysOf({1: _all}, body: true), iods: {1: face});
      var l = const LocalList();
      for (final s in portraitSpecs) {
        if (!s.onByDefault && !light) continue;
        for (final ar in s.areas) {
          l = l.add(applyLocalUi(LocalAdjustment(id: ar.id, name: portraitName(ar.id), mask: portraitMask(ar, fs, {1}, pidBody), face: portraitFaceScale(fs, {1})), LocalUi.centre));
        }
      }
      return l;
    }

    test('stays within 12 distinct raster planes, with 补光 too', () {
      final l = build();
      expect(l.length, 7);
      expect(l.rasterPlanes, lessThanOrEqualTo(kMaxRasterComponents));
      expect(l.rasterPlanes, 9);
      final all = build(light: true);
      expect(all.length, 8);
      expect(all.rasterPlanes, lessThanOrEqualTo(kMaxRasterComponents));
      expect(all.rasterPlanes, 10);
    });

    test('JSON is v2 with retouch.face on every portrait area; strength 100; default recipe', () {
      final l = build();
      final j = l.toEngineJson()!;
      expect(j['v'], 2);
      final items = (j['items']! as List).cast<Map<String, Object?>>();
      expect(items, hasLength(7));
      for (final it in items) {
        expect((it['retouch']! as Map)['face'], closeTo(0.11, 1e-9), reason: '${it['id']}');
        expect(it['strength'], 100);
        expect(it['name']! as String, startsWith('portrait:'));
      }
      final skin = l.byId('pt-skin')!;
      expect(skin.ops['smooth'], 30);
      expect(skin.ops['even'], 25);
      expect(skin.ops['redness'], 30);
      expect(skin.ops['heal'], 50);
      expect(skin.ops.containsKey('texture'), isFalse, reason: 'default 0');
      expect(l.byId('pt-iris')!.ops, {'clarity': 25, 'saturation': 10});
      expect(l.byId('pt-eyewhite')!.ops, {'whiten': 30});
      final back = LocalList.fromJson(j);
      expect(back.byId('pt-skin')!.face, closeTo(0.11, 1e-9));
    });

    test('a list without retouch stays v1', () {
      expect(const LocalList([LocalAdjustment(id: 'a', ops: {'exposure': 5})]).toEngineJson()!['v'], 1);
    });
  });

  group('the pads of a portrait area', () {
    LocalAdjustment fresh(String id) => applyLocalUi(LocalAdjustment(id: id, name: portraitName(id), face: 0.1), LocalUi.centre);

    test('applyLocalUi keeps strength 100 and the strip scales the ops', () {
      final a = fresh('pt-skin');
      expect(a.strength, 100);
      expect(a.ops['smooth'], 30);
      final lo = applyLocalUi(a, const LocalUi(a: 0.25)), hi = applyLocalUi(a, const LocalUi(a: 1));
      expect(lo.strength, 100);
      expect(hi.strength, 100);
      expect(lo.ops['smooth'], 15);
      expect(hi.ops['smooth'], 100);
      expect(hi.ops['even'], 60);
      expect(hi.ops['texture'], 20);
      final off = applyLocalUi(a, const LocalUi(a: 0));
      expect(off.strength, 100, reason: 'a portrait strip at the bottom is a recipe of 0, not strength 0');
      expect(off.ops, isEmpty);
    });

    test('setLocalItem clamps to the recipe maximum', () {
      final a = fresh('pt-skin');
      expect(setLocalItem(a, 'smooth', 150).ops['smooth'], 100);
      expect(setLocalItem(a, 'even', 90).ops['even'], 60);
      expect(setLocalItem(a, 'redness', 90).ops['redness'], 50, reason: 'the engine maximum of redness is 50');
      expect(setLocalItem(a, 'even', 40).ops['even'], 40);
      expect(setLocalItem(a, 'even', 90).strength, 100);
      expect(setLocalItem(a, 'even', 10).ops['smooth'], 30);
      expect(setLocalItem(fresh('pt-teeth'), 'whiten', 100).ops['whiten'], 60);
    });

    test('portraitOf finds the chip and area of a stored area; ordinary areas are null', () {
      final a = fresh('pt-iris');
      expect(portraitOf(a)!.$1.chip, PortraitChip.eyes);
      expect(portraitOf(a)!.$2.id, 'pt-iris');
      expect(portraitOf(const LocalAdjustment(id: 'z')), isNull);
      expect(portraitMax(area('pt-skin'), 'smooth'), 100);
      expect(portraitMax(area('pt-skin'), 'exposure'), localOpByKey['exposure']!.max);
    });
  });
}
