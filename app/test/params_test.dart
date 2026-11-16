
import 'package:dapple/model/params.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('spec table has the 75 numeric keys of chapter 6', () {
    expect(sliderSpecs.length, 86, reason: '75 (incl. the single-image HDR) + 6 film knobs + the 5 film pad / ruler keys');
    expect(specByKey['sharpen']!.def, 40);
    expect(specByKey['nrColor']!.def, 25);
    expect(specByKey['gr_blend']!.def, 50);
  });

  test('value formats', () {
    expect(fmtValue(Fmt.ev, 20), '+1.00 EV');
    expect(fmtValue(Fmt.ev, -10), '−0.50 EV');
    expect(fmtValue(Fmt.signed, 0), '0');
    expect(fmtValue(Fmt.deg, -3), '−3°');
    expect(fmtValue(Fmt.radius, 15), '1.5');
  });

  test('default values are not stored; wire carries chapter-6 keys', () {
    var e = EditState.empty.withValue('exposure', 20).withValue('sharpen', 40).withValue('temp', 0);
    expect(e.values, {'exposure': 20});
    expect(e.groupAdjusted(EGroup.light), isTrue);
    expect(e.groupAdjusted(EGroup.color), isFalse);
    e = e.copyWith(palette: const Offset(0.6, 0.3), toneTarget: 'low-long');
    final w = e.toWire(defaultLook: 'std', defaultToneStrength: 60);
    expect(w['exposure'], 20);
    expect(w['look'], 'std');
    expect(w['toneTarget'], 'low-long');
    expect(w['toneStrength'], 60);
    expect(w.containsKey('palette'), isFalse, reason: 'the palette moved into the colour pad (docs/design/editor-panel.md)');
  });

  test('sync merges, paste replaces, copy drops crop', () {
    const src = EditState(values: {'exposure': 10, 'contrast': 5, 'straighten': 2}, crop: '1:1', look: 'warm');
    const dst = EditState(values: {'shadows': 30, 'exposure': -5}, crop: '3:4');
    final m = dst.merged(src, {SyncCat.light});
    expect(m.values, {'shadows': 30, 'exposure': 10, 'contrast': 5});
    expect(m.crop, '3:4');
    expect(m.look, isNull);
    final p = dst.pasted(src.withoutCrop());
    expect(p.values.containsKey('shadows'), isFalse);
    expect(p.look, 'warm');
    expect(p.crop, '3:4');
    expect(src.withoutCrop().values.containsKey('straighten'), isFalse);
  });

  test('undo stack coalesces within 800 ms', () {
    final u = UndoStack();
    const a = EditState(), b = EditState(values: {'exposure': 1}), c = EditState(values: {'exposure': 2});
    u.record(a, coalesceKey: 'x', nowMs: 1000);
    u.record(b, coalesceKey: 'x', nowMs: 1500);
    expect(u.undo(c), a);
    expect(u.canUndo, isFalse);
    expect(u.redo(a), c);
  });

  group('film_wb is two-sided (edit schema 2)', () {
    test('range -1..1, default 0, and the stored edit carries its version', () {
      final sp = specByKey['film_wb']!;
      expect([sp.min, sp.max, sp.def], [-1, 1, 0]);
      expect(EditState.empty.toJson()['v'], kEditSchema);
    });

    test('an edit saved before schema 2 migrates v -> 2v - 1 (renders as before)', () {
      EditState load(Map<String, dynamic> values, [Map<String, dynamic>? pads]) => EditState.fromJson({
            'film': 'kodak_portra_400',
            'values': values,
            'pads': ?pads,
          });
      expect(load({'film_wb': 0.0}).value('film_wb'), -1);
      expect(load({'film_wb': 0.5}).value('film_wb'), 0);
      expect(load({'film_wb': 1.0}).value('film_wb'), 1);
      expect(load({'film_grain': 50.0}).value('film_wb'), 0, reason: 'no explicit value: the new default');
      final p = load({'film_wb': 0.25}, {'film': {'x': 0, 'y': 0, 'a': 0.5, 'off': {'film_wb': 0.25}}});
      expect(p.pads['film']!.off['film_wb'], -0.5);
    });

    test('a schema 2 edit round-trips unchanged', () {
      final e = EditState.empty.copyWith(film: 'kodak_portra_400', values: {'film_wb': -0.4});
      final r = EditState.fromJson(e.toJson());
      expect(r.value('film_wb'), -0.4);
      expect(EditState.fromJson(r.toJson()).value('film_wb'), -0.4);
    });
  });
}
