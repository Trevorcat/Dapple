// Local adjustments: the Dart model against the engine's own JSON (docs/engine/pipeline-architecture.md 40.1). The fixtures below were
// printed by the engine's writer (core/src/local_adjust.cpp at HEAD, localToJson) for edits mirroring tests/engine/local_tests.cpp
// richEdit() plus a name, invert, an "off" item, a raster part and an unknown kind; reading them and writing them back must give the
// same bytes.
import 'dart:convert';

import 'package:dapple/model/local_adjust.dart';
import 'package:dapple/model/local_kinds.dart';
import 'package:dapple/model/local_pad.dart';
import 'package:dapple/model/params.dart';
import 'package:flutter_test/flutter_test.dart';

const engineRich =
    r'{"v":1,"items":[{"id":"sky","strength":100,"mask":[{"kind":"linear","op":"add","x0":0.5,"y0":0.05,"x1":0.5,"y1":0.5},{"kind":"luminance","op":"intersect","lo":30,"hi":100,"feather":15},{"kind":"depth3d","op":"add","planes":[1,2,3],"x":{"y":true}}],"adjust":{"exposure":-16,"highlights":-40,"temp":-20,"dehaze":40}},{"id":"subject","name":"主体 \"quoted\"","invert":true,"strength":75,"mask":[{"kind":"radial","op":"add","invert":true,"cx":0.62,"cy":0.6,"rx":0.14,"ry":0.2,"angle":30,"feather":60}],"adjust":{"shadows":30,"saturation":20,"clarity":50,"texture":-40}},{"id":"green","on":false,"strength":100,"mask":[{"kind":"color","op":"add","hue":140,"range":30,"feather":20,"satMin":10}],"adjust":{"vibrance":50,"color_hue":60,"color_sat":40}},{"id":"brush","strength":100,"mask":[{"kind":"brush","op":"add","strokes":[{"size":0.04,"hardness":40,"flow":80,"pts":[0.15,0.8,0.18,0.79,0.210000008,0.780000031,0.24000001,0.770000041,0.27,0.76,0.3,0.75,0.329999983,0.74,0.36,0.73,0.39,0.72,0.42,0.710000038,0.45,0.700000048,0.48,0.69,0.51,0.68,0.539999962,0.67,0.57,0.66,0.6,0.650000036,0.63,0.64,0.659999967,0.63,0.689999938,0.62,0.72,0.61,0.75,0.6]},{"size":0.02,"hardness":40,"flow":80,"erase":true,"pts":[0.3,0.65,0.4,0.75]}]},{"kind":"raster","op":"subtract","ref":"subject:p1:0","refine":60}],"adjust":{"contrast":30,"whites":20,"blacks":-20,"sharpen":60,"noise":50}}]}';

/// tests/engine/local_tests.cpp testJson's clamping input, and what the engine wrote after reading it.
const clampIn = r'{"v":7,"items":[{"id":"a","strength":250,"mask":[{"kind":"radial","rx":-3,"feather":500,"future":1}],"adjust":{"exposure":999,"contrast":-400,"color_hue":-30,"noise":-5}}]}';
const clampOut = r'{"v":1,"items":[{"id":"a","strength":100,"mask":[{"kind":"radial","op":"add","cx":0.5,"cy":0.5,"rx":0.001,"ry":0.25,"angle":0,"feather":100}],"adjust":{"exposure":80,"contrast":-100}}]}';

/// tests/engine/local_capi_tests.cpp line 70.
const capiRadial = r'{"v":1,"items":[{"id":"r","strength":100,"mask":[{"kind":"radial","op":"add","cx":0.2,"cy":0.25,"rx":0.12,"ry":0.12,"angle":0,"feather":20}],"adjust":{"exposure":40}}]}';

/// The engine (HEAD writer, section 41) reading a raster range and an identity range plus unknown keys: it writes the range only when it is
/// not the identity and drops what it does not know.
const rasterIn =
    r'{"v":1,"items":[{"id":"d","strength":100,"mask":[{"kind":"raster","op":"add","ref":"ai:0123456789abcdef:depth","refine":50,"lo":5,"hi":35.5,"feather":12},{"kind":"raster","op":"intersect","ref":"ai:0123456789abcdef:f1:lips","refine":60,"lo":0,"hi":100,"feather":20,"newkey":3}],"adjust":{"exposure":10},"future":{"a":1}}]}';
const rasterEngineOut =
    r'{"v":1,"items":[{"id":"d","strength":100,"mask":[{"kind":"raster","op":"add","ref":"ai:0123456789abcdef:depth","refine":50,"lo":5,"hi":35.5,"feather":12},{"kind":"raster","op":"intersect","ref":"ai:0123456789abcdef:f1:lips","refine":60}],"adjust":{"exposure":10}}]}';

/// The engine's writer (HEAD, section 43) reading a portrait retouch edit with an unknown op ("glow"), an unknown item key ("future"), a
/// heal sensitivity and two manual spots (the second with a source offset beyond its limits: clamped), and a teeth area.
const retouchIn =
    r'{"v":2,"items":[{"id":"pt-skin","name":"portrait:skin","strength":100,"mask":[{"kind":"raster","op":"add","ref":"ai:0123456789abcdef:faceskin","refine":50},{"kind":"raster","op":"subtract","ref":"ai:0123456789abcdef:f1:lips","refine":50}],"adjust":{"texture":10,"smooth":30,"even":25,"redness":30,"heal":50,"glow":7},"retouch":{"face":0.0812,"sens":40,"spots":[[0.41,0.33,0.004],[0.5,0.5,0.03,0.2,0]]},"future":true},{"id":"pt-teeth","strength":100,"mask":[{"kind":"raster","op":"add","ref":"ai:0123456789abcdef:f1:teeth","refine":50}],"adjust":{"whiten":30},"retouch":{"face":0.0812}}]}';
const retouchEngineOut =
    r'{"v":2,"items":[{"id":"pt-skin","name":"portrait:skin","strength":100,"mask":[{"kind":"raster","op":"add","ref":"ai:0123456789abcdef:faceskin","refine":50},{"kind":"raster","op":"subtract","ref":"ai:0123456789abcdef:f1:lips","refine":50}],"adjust":{"texture":10,"smooth":30,"even":25,"redness":30,"heal":50,"glow":7},"retouch":{"face":0.0812,"sens":40,"spots":[[0.41,0.33,0.004],[0.5,0.5,0.02,0.08,0]]},"future":true},{"id":"pt-teeth","strength":100,"mask":[{"kind":"raster","op":"add","ref":"ai:0123456789abcdef:f1:teeth","refine":50}],"adjust":{"whiten":30},"retouch":{"face":0.0812}}]}';
const spotsOnlyEngineOut = r'{"v":2,"items":[{"id":"s","strength":100,"mask":[],"adjust":{},"retouch":{"spots":[[0.2,0.3,0.004]]}}]}';

void main() {
  group('portrait retouch (v2)', () {
    test('read like the engine, written byte for byte like the engine (unknown op and item key kept)', () {
      final l = LocalList.fromJson(jsonDecode(retouchIn));
      expect(jsonEncode(l.toEngineJson()), retouchEngineOut);
      expect(jsonEncode(LocalList.fromJson(jsonDecode(retouchEngineOut)).toEngineJson()), retouchEngineOut);
      final skin = l.items.first;
      expect(skin.face, 0.0812);
      expect(skin.sens, 40);
      expect(skin.spots, hasLength(2));
      expect(skin.spots[1].r, 0.02);
      expect(skin.spots[1].dx, closeTo(0.08, 1e-9));
      expect(skin.op('smooth'), 30);
      expect(l.version, 2);
    });

    test('spots alone make v2 too; an edit without retouch stays v1', () {
      expect(jsonEncode(LocalList.fromJson(jsonDecode(spotsOnlyEngineOut)).toEngineJson()), spotsOnlyEngineOut);
      expect(LocalList.fromJson(jsonDecode(capiRadial)).version, 1);
    });

    test('at most 64 spots; the raster cap is 12', () {
      final many = {
        'id': 'h',
        'retouch': {
          'spots': [for (var i = 0; i < 80; i++) [0.1, 0.1, 0.004]],
        },
      };
      expect(LocalList.fromJson([many]).items.first.spots, hasLength(kMaxHealSpots));
      expect(kMaxRasterComponents, 12);
    });
  });

  group('raster range and unknown keys', () {
    test('the range of a raster part travels; identity is not written (as the engine)', () {
      final l = LocalList.fromJson(jsonDecode(rasterEngineOut));
      expect(jsonEncode(l.toEngineJson()), rasterEngineOut);
      final d = l.items.first.mask.first;
      expect([d.lo, d.hi, d.lumFeather], [5, 35.5, 12]);
      expect(d.hasRange, isTrue);
      expect(l.items.first.mask[1].hasRange, isFalse);
    });

    test('keys this build does not know are kept verbatim (component and item), the known ones written like the engine', () {
      final l = LocalList.fromJson(jsonDecode(rasterIn));
      final out = jsonDecode(jsonEncode(l.toEngineJson())) as Map;
      final item = (out['items'] as List).first as Map;
      expect(item['future'], {'a': 1});
      final lips = (item['mask'] as List)[1] as Map;
      expect(lips['newkey'], 3);
      expect(lips.containsKey('lo'), isFalse);
      // stripped of the unknown keys, the bytes are the engine's
      item.remove('future');
      lips.remove('newkey');
      expect(jsonEncode(out), rasterEngineOut);
      // and a second round trip is stable
      final again = LocalList.fromJson(jsonDecode(jsonEncode(l.toEngineJson())));
      expect(jsonEncode(again.toEngineJson()), jsonEncode(l.toEngineJson()));
    });

    test('an unknown key survives an edit of the component', () {
      final l = LocalList.fromJson(jsonDecode(rasterIn));
      final c = l.items.first.mask[1].copyWith(refine: 70);
      expect(c.toJson()['newkey'], 3);
      expect(c.toJson()['refine'], 70);
    });
  });

  group('engine JSON round trip', () {
    test('rich edit: byte-identical', () {
      final l = LocalList.fromJson(jsonDecode(engineRich));
      expect(l.length, 4);
      expect(jsonEncode(l.toEngineJson()), engineRich);
    });

    test('the C API example', () {
      expect(jsonEncode(LocalList.fromJson(jsonDecode(capiRadial)).toEngineJson()), capiRadial);
    });

    test('clamping and a newer version read like the engine (its unknown key "future" kept verbatim)', () {
      final out = jsonDecode(jsonEncode(LocalList.fromJson(jsonDecode(clampIn)).toEngineJson())) as Map;
      final comp = ((out['items'] as List).first['mask'] as List).first as Map;
      expect(comp.remove('future'), 1);
      expect(jsonEncode(out), clampOut);
    });

    test('fields land where the engine puts them', () {
      final l = LocalList.fromJson(jsonDecode(engineRich));
      final sky = l.items[0];
      expect(sky.mask[0].kind, MaskKindId.linear);
      expect(sky.mask[1].op, MaskOp.intersect);
      expect(sky.mask[1].lumFeather, 15);
      expect(sky.mask[2].kind, MaskKindId.unknown);
      expect(sky.op('exposure'), -16); // EV x 20
      final subj = l.items[1];
      expect(subj.name, '主体 "quoted"');
      expect(subj.invert && subj.mask[0].invert, isTrue);
      expect(subj.mask[0].angle, 30);
      expect(subj.mask[0].feather, 60);
      expect(l.items[2].on, isFalse);
      expect(l.items[2].mask[0].hueFeather, 20);
      final b = l.items[3].mask[0];
      expect(b.strokes.length, 2);
      expect(b.strokes[0].count, 21);
      expect(b.strokes[1].erase, isTrue);
      expect(l.items[3].mask[1].ref, 'subject:p1:0');
      expect(kindOfComponent(l.items[3].mask[1]).id, 'raster'); // not an "ai:" id: a generic plane
    });

    test('a bare array of items, caps 16 / 8', () {
      final item = {
        'id': 'x',
        'mask': [for (var i = 0; i < 10; i++) {'kind': 'linear'}],
        'adjust': {'exposure': 20},
      };
      final l = LocalList.fromJson([for (var i = 0; i < 20; i++) item]);
      expect(l.length, kMaxLocalAdjustments);
      expect(l.items.first.mask.length, kMaxMaskComponents);
      expect(const LocalList().toEngineJson(), isNull);
    });

    test('jnum writes like the engine', () {
      expect(jnum(100), 100);
      expect(jnum(0.2), 0.2);
      expect(jsonEncode(jnum(0.15 + 0.03 * 2)), '0.21'); // a double that is close to 0.21 in float
      expect(jnum(double.nan), isA<num>());
    });
  });

  group('the app record', () {
    test('ui travels with the app JSON, not with the engine JSON', () {
      final a = applyLocalUi(const LocalAdjustment(id: 'k', mask: [MaskComponent(kind: MaskKindId.radial)]), const LocalUi(lx: 0.5, a: 0.5));
      final e = EditState.empty.copyWith(local: LocalList([a]));
      final wire = e.toWire(defaultLook: 'std', defaultToneStrength: 60);
      expect((wire['local'] as Map)['items'][0].containsKey('ui'), isFalse);
      final back = EditState.fromJson(jsonDecode(jsonEncode(e.toJson())) as Map<String, dynamic>);
      expect(back.local.same(e.local), isTrue);
      expect(back.local.items.first.ui!.lx, 0.5);
      expect(back.sameAs(e), isTrue);
      expect(e.isAdjusted, isTrue);
    });

    test('an edit without areas sends no "local"', () {
      expect(EditState.empty.toWire(defaultLook: 'std', defaultToneStrength: 60).containsKey('local'), isFalse);
      expect(EditState.empty.toJson().containsKey('local'), isFalse);
    });

    test('copy / paste keep the target photo\'s own areas', () {
      final mine = EditState.empty.copyWith(local: LocalList.fromJson(jsonDecode(capiRadial)));
      final other = EditState.empty.copyWith(local: LocalList.fromJson(jsonDecode(engineRich)));
      expect(other.withoutCrop().local.isEmpty, isTrue);
      expect(mine.pasted(other.withoutCrop()).local.same(mine.local), isTrue);
    });
  });

  group('pads of an area', () {
    test('centre and 100 % = no ops', () {
      final r = localRecipe(LocalUi.centre);
      expect(r.values.every((v) => v == 0), isTrue);
      final a = applyLocalUi(const LocalAdjustment(id: 'k'), LocalUi.centre);
      expect(a.ops, isEmpty);
      expect(a.strength, 100);
    });

    test('the strip: bottom 0 strength, top doubles the recipe', () {
      final half = applyLocalUi(const LocalAdjustment(id: 'k'), const LocalUi(lx: 1, a: 0.25));
      expect(half.strength, 50);
      expect(half.op('exposure'), 30);
      final top = applyLocalUi(const LocalAdjustment(id: 'k'), const LocalUi(lx: 1, a: 1));
      expect(top.strength, 100);
      expect(top.op('exposure'), 60);
      final zero = applyLocalUi(const LocalAdjustment(id: 'k'), const LocalUi(lx: 1, a: 0));
      expect(zero.strength, 0);
    });

    test('itemised value = offset; the pads keep their place', () {
      var a = applyLocalUi(const LocalAdjustment(id: 'k'), const LocalUi(lx: 0.5));
      a = setLocalItem(a, 'exposure', 40);
      expect(a.op('exposure'), 40);
      expect(a.ui!.lx, 0.5);
      expect(a.ui!.off['exposure'], 25);
      a = applyLocalUi(a, a.ui!.copyWith(lx: 0));
      expect(a.op('exposure'), 25); // the offset stays when the pad moves
      a = setLocalItem(a, 'clarity', 30);
      expect(a.op('clarity'), 30);
    });

    test('an area without pad state opens with its ops as offsets: the first touch changes nothing', () {
      final l = LocalList.fromJson(jsonDecode(engineRich));
      final sky = l.items[0];
      final u = localUiOf(sky);
      final again = applyLocalUi(sky, u);
      expect(again.ops, sky.ops);
      expect(again.strength, sky.strength);
    });

    test('values stay in the engine range', () {
      final a = applyLocalUi(const LocalAdjustment(id: 'k', ops: {}), const LocalUi(lx: 1, ly: 1, cx: 1, cy: 1, a: 1, off: {'exposure': 70}));
      for (final e in a.ops.entries) {
        final s = localOpByKey[e.key]!;
        expect(e.value, inInclusiveRange(s.min, s.max));
      }
    });
  });

  group('kinds registry', () {
    test('five kinds offered, AI kinds kept hidden until a provider exists', () {
      expect(pickableKinds.map((k) => k.id), ['linear', 'radial', 'brush', 'luminance', 'color']);
    });

    test('AI kinds follow the device and the analysis', () {
      expect(offeredKinds(deviceTasks: AiBits.person | AiBits.faces | AiBits.sky | AiBits.tapPrepare).map((k) => k.id),
          ['linear', 'radial', 'brush', 'luminance', 'color', 'portrait', 'person', 'background', 'tap', 'sky']);
      expect(offeredKinds(deviceTasks: 31, absent: {'background', 'sky'}).map((k) => k.id), contains('person'));
      expect(offeredKinds(deviceTasks: 31, absent: {'background', 'sky'}).map((k) => k.id), isNot(contains('sky')));
      expect(offeredKinds(deviceTasks: 31).map((k) => k.id), contains('depth'));
    });

    test('a stored AI plane tells its kind and part', () {
      MaskComponent r(String ref) => MaskComponent(kind: MaskKindId.raster, ref: ref);
      expect(kindOfComponent(r('ai:0123456789abcdef:hair')).id, 'person');
      expect(kindOfComponent(r('ai:0123456789abcdef:f2:lips')).id, 'person');
      expect(kindOfComponent(r('ai:0123456789abcdef:background')).id, 'background');
      expect(kindOfComponent(r('ai:0123456789abcdef:tap:+412,531;-200,700')).id, 'tap');
      expect(kindOfComponent(r('ai:0123456789abcdef:depth')).id, 'depth');
      expect(kindOfComponent(r('')).id, 'tap'); // a tap area before its first tap
      expect(partLabel('f1:iris'), '虹膜');
      expect(partLabel('faceskin'), '脸部皮肤');
      expect(partLabel('f1:face'), isNull);
      expect(aiPlaneKey('ai:0123456789abcdef:f1:lips'), 'f1:lips');
    });

    test('the AI ranges: shapes, not limits', () {
      expect(opForNew(kindById('person'), first: false, subtract: false), MaskOp.add);
      LocalList rasters(int n) => LocalList([for (var i = 0; i < n; i++) LocalAdjustment(id: '$i', mask: [MaskComponent(kind: MaskKindId.raster, ref: 'ai:0123456789abcdef:p$i')])]);
      expect(limitFor(rasters(11), kindById('sky'), n: 2), isNotNull);
      expect(limitFor(rasters(10), kindById('sky'), n: 2), isNull);
      // the engine counts distinct planes: the same plane twice is one
      final twice = LocalList([for (var i = 0; i < 12; i++) LocalAdjustment(id: '$i', mask: const [MaskComponent(kind: MaskKindId.raster, ref: 'ai:0123456789abcdef:faceskin')])]);
      expect(twice.rasterPlanes, 1);
    });

    test('combine rule: shapes add, ranges limit, 减 subtracts', () {
      expect(opForNew(kindById('radial'), first: false, subtract: false), MaskOp.add);
      expect(opForNew(kindById('luminance'), first: false, subtract: false), MaskOp.intersect);
      expect(opForNew(kindById('luminance'), first: false, subtract: true), MaskOp.subtract);
      expect(opForNew(kindById('color'), first: true, subtract: true), MaskOp.add);
    });

    test('limits', () {
      final full = LocalList([for (var i = 0; i < 16; i++) LocalAdjustment(id: '$i')]);
      expect(limitFor(full, kindById('radial')), '最多 16 个区域');
      final brushes = LocalList([for (var i = 0; i < 8; i++) LocalAdjustment(id: '$i', mask: const [MaskComponent(kind: MaskKindId.brush)])]);
      expect(limitFor(brushes, kindById('brush')), '画笔最多 8 个');
      expect(limitFor(brushes, kindById('radial')), isNull);
    });
  });
}
