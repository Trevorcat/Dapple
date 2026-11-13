// The pad recipes are pure functions (x, y, strength) -> parameter values: ranges, monotonicity, the centre = the defaults,
// old edits opening with their values as item offsets, the persistence of the pad state, the film map layout.

import 'package:dapple/model/catalog.dart' show lookById;
import 'package:dapple/model/film_map.dart';
import 'package:dapple/model/pad_model.dart';
import 'package:dapple/model/params.dart';
import 'package:flutter_test/flutter_test.dart';

import '../support/film_fixture.dart';

const ctx = PadCtx(defaultTone: 60);

void main() {
  group('recipes', () {
    test('the centre of every pad gives exactly the parameter defaults (automatic)', () {
      for (final cat in PadCat.values) {
        final r = padRecipe(cat, 0, 0, 0.5, ctx: ctx);
        for (final it in padItems(cat)) {
          expect(r[it.id], closeTo(it.auto, 1e-9), reason: '${cat.name}.${it.id}');
        }
      }
    });

    test('every recipe value stays inside its parameter range on the whole pad and the whole strip', () {
      for (final cat in PadCat.values) {
        for (final it in padItems(cat)) {
          for (var xi = -4; xi <= 4; xi++) {
            for (var yi = -4; yi <= 4; yi++) {
              for (var ai = 0; ai <= 10; ai++) {
                final v = padRecipe(cat, xi / 4, yi / 4, ai / 10, ctx: ctx)[it.id]!;
                expect(v, inInclusiveRange(it.min - 1e-9, it.max + 1e-9), reason: '${cat.name}.${it.id} at ${xi / 4},${yi / 4},${ai / 10}: $v');
              }
            }
          }
        }
      }
    });

    double at(PadCat c, String k, {double x = 0, double y = 0, double a = 0.5}) => padRecipe(c, x, y, a, ctx: ctx)[k]!;

    void monotone(PadCat c, String k, {required bool x, bool up = true, double a = 0.5}) {
      var prev = double.nan;
      for (var i = -10; i <= 10; i++) {
        final t = i / 10;
        final v = x ? at(c, k, x: t, a: a) : at(c, k, y: t, a: a);
        if (!prev.isNaN) expect(up ? v >= prev - 1e-9 : v <= prev + 1e-9, isTrue, reason: '${c.name}.$k ${x ? 'x' : 'y'}=$t');
        prev = v;
      }
    }

    test('monotonic along the pad directions', () {
      monotone(PadCat.light, 'exposure', x: true);
      monotone(PadCat.light, 'contrast', x: false);
      monotone(PadCat.light, 'highlights', x: true, up: false);
      monotone(PadCat.light, 'shadows', x: true);
      monotone(PadCat.color, 'temp', x: true);
      monotone(PadCat.color, 'vibrance', x: false);
      monotone(PadCat.color, 'saturation', x: false);
      monotone(PadCat.film, 'film_shoot', x: true);
      monotone(PadCat.film, 'film_print', x: false);
      monotone(PadCat.texture, 'clarity', x: true);
      monotone(PadCat.texture, 'sharpen', x: true);
      monotone(PadCat.texture, 'grain', x: false);
      monotone(PadCat.texture, 'nrColor', x: false, up: false); // 干净 is the bottom: more noise reduction there
    });

    test('the strength strip: 0 = no effect, the middle = the defaults, above = heavier', () {
      expect(at(PadCat.light, 'toneStrength', a: 0), 0);
      expect(at(PadCat.light, 'toneStrength'), 60);
      expect(at(PadCat.light, 'toneStrength', a: 1), 100);
      expect(at(PadCat.color, 'lookStrength', a: 0), 0);
      expect(at(PadCat.color, 'lookStrength'), 100);
      expect(at(PadCat.color, 'vibrance', a: 1), greaterThan(at(PadCat.color, 'vibrance')));
      expect(at(PadCat.film, 'film_strength', a: 0), 0);
      expect(at(PadCat.film, 'film_strength', a: 0.25), 50);
      expect(at(PadCat.film, 'film_strength'), 100);
      expect(at(PadCat.film, 'film_strength', a: 1), 100);
      // above the middle the film's black lifts; grain and halation reach the stock's own 100 % in the lower half
      expect(at(PadCat.film, 'film_black', a: 1), lessThan(0));
      expect(at(PadCat.film, 'film_grain', a: 0.25), 50);
      // and above the middle grain and halation grow to the top of their range, read from the param spec (200 % in the engine schema)
      expect(at(PadCat.film, 'film_grain', a: 1), specByKey['film_grain']!.max);
      expect(at(PadCat.film, 'film_halation', a: 1), specByKey['film_halation']!.max);
      expect(at(PadCat.film, 'film_grain', a: 0.75), closeTo((100 + specByKey['film_grain']!.max) / 2, 1e-9));
      expect(specByKey['film_grain']!.max, greaterThan(100));
      expect(at(PadCat.texture, 'sharpen', a: 0), 0);
      expect(at(PadCat.texture, 'sharpen'), 40);
      expect(at(PadCat.texture, 'sharpen', a: 1), greaterThan(40));
      for (final cat in PadCat.values) {
        final s = strengthItemOf(cat);
        if (s == null) continue;
        var prev = -1.0;
        for (var i = 0; i <= 20; i++) {
          final v = at(cat, s, a: i / 20);
          expect(v, greaterThanOrEqualTo(prev - 1e-9), reason: '${cat.name} strength not monotonic');
          prev = v;
        }
      }
    });

    test('film pad: the two directions stay inside the engine safe range at every corner', () {
      for (final x in [-1.0, 1.0]) {
        for (final y in [-1.0, 1.0]) {
          final r = padRecipe(PadCat.film, x, y, 0.5);
          expect(r['film_shoot']!.abs(), lessThanOrEqualTo(0.85));
          expect(r['film_print']!.abs(), lessThanOrEqualTo(0.85));
        }
      }
    });
  });

  group('pad state in the edit', () {
    test('an edit without pad state opens with the pad at the centre and its values as item offsets', () {
      final old = const EditState().withValue('exposure', 12).withValue('contrast', 8).copyWith(toneStrength: 30.0);
      final s = padStateOf(old, PadCat.light, ctx);
      expect(s.x, 0);
      expect(s.y, 0);
      expect(s.off['exposure'], 12);
      expect(s.off['contrast'], 8);
      expect(s.a, closeTo(0.25, 1e-9), reason: 'the strip stands for the tone strength 30 of 60');
      expect(s.off.containsKey('toneStrength'), isFalse);
      // the untouched edit: everything automatic, nothing to show
      expect(padStateOf(EditState.empty, PadCat.light, ctx).isCentre, isTrue);
    });

    test('moving the pad keeps the offsets: value = recipe + offset', () {
      final old = const EditState().withValue('exposure', 12);
      final s = padStateOf(old, PadCat.light, ctx);
      final moved = applyPadState(old, PadCat.light, s.copyWith(x: 1), ctx);
      expect(moved.value('exposure'), closeTo(16 + 12, 1e-9));
      expect(moved.value('contrast'), 0);
      expect(moved.pads['light']!.x, 1);
      expect(moved.pads['light']!.off['exposure'], 12);
      // back to the centre: the old value again, and the pad state is only the offset
      final back = applyPadState(moved, PadCat.light, moved.pads['light']!.copyWith(x: 0), ctx);
      expect(back.value('exposure'), 12);
    });

    test('an item edit writes the value as is and the pad keeps its place', () {
      var e = applyPadState(EditState.empty, PadCat.light, const PadState(x: 0.5), ctx);
      final it = padItems(PadCat.light).firstWhere((i) => i.id == 'contrast');
      e = setItemValue(e, PadCat.light, it, 25, ctx);
      expect(e.value('contrast'), 25);
      expect(e.pads['light']!.x, 0.5);
      expect(e.pads['light']!.off['contrast'], 25);
      expect(e.value('exposure'), closeTo(8, 1e-9));
    });

    test('the pad at the centre with no offsets is absent from the edit; reset removes everything of the category', () {
      var e = applyPadState(EditState.empty, PadCat.color, const PadState(x: 0.4, y: -0.3, a: 0.7), ctx);
      expect(e.pads.containsKey('color'), isTrue);
      expect(e.value('temp'), closeTo(12, 1e-9));
      e = resetPadCat(e, PadCat.color, ctx);
      expect(e.pads, isEmpty);
      expect(e.values, isEmpty);
      expect(e.lookStrength, 100);
      expect(const EditState().sameAs(e), isTrue);
    });

    test('the strength item follows the strip: tone strength, look strength, film strength', () {
      var e = applyPadState(EditState.empty, PadCat.light, const PadState(a: 0), ctx);
      expect(e.toneStrength, 0);
      e = applyPadState(e, PadCat.light, const PadState(a: 0.5), ctx);
      expect(e.toneStrength, isNull, reason: 'the default is stored as absent');
      e = applyPadState(const EditState(film: 'x'), PadCat.film, const PadState(a: 0.25), ctx);
      expect(e.value('film_strength'), 50);
      e = applyPadState(EditState.empty, PadCat.color, const PadState(a: 0.25), ctx);
      expect(e.lookStrength, 50);
    });

    test('pad state and offsets survive a save and reopen; garbage is ignored; the engine wire does not carry it', () {
      var e = applyPadState(const EditState(film: 'kodak_portra_400'), PadCat.film, const PadState(x: 0.3, y: -0.2, a: 0.8), ctx);
      e = setItemValue(e, PadCat.film, padItems(PadCat.film).firstWhere((i) => i.id == 'film_grain'), 40, ctx);
      final back = EditState.fromJson(e.toJson());
      expect(back.sameAs(e), isTrue);
      expect(back.pads['film']!.x, 0.3);
      expect(back.pads['film']!.off['film_grain'], isNotNull);
      final bad = EditState.fromJson({
        'pads': {
          'film': {'x': 'no'},
          'light': 5,
        },
        'values': {},
      });
      expect(bad.pads['film']!.x, 0);
      expect(bad.pads.containsKey('light'), isFalse);
      final wire = e.toWire(defaultLook: 'std', defaultToneStrength: 60);
      expect(wire.containsKey('pads'), isFalse);
      expect(wire['film_shoot'], closeTo(0.24, 1e-9));
    });

    test('colour presets sit where the looks sit and the film pad limits clamp', () {
      expect(colorPresets.length, greaterThanOrEqualTo(4));
      final names = {for (final p in colorPresets) p.look};
      expect(names.contains('warm'), isTrue);
      final warm = colorPresets.firstWhere((p) => p.look == 'warm');
      expect(warm.x, greaterThan(0), reason: 'the warm look is on the warm side');
      final cool = colorPresets.firstWhere((p) => p.look == 'cool');
      expect(cool.x, lessThan(0));
      const lim = PadLimits(yHi: 0);
      expect(lim.clamp(0.5, 0.7), const Offset(0.5, 0));
    });

    test('the old palette moves into the colour pad on load and is no longer sent', () {
      // an old edit: the 暖阳 look pushed to the warm / rich corner of its palette (x pale -> rich, y warm -> cool)
      final old = {
        'look': 'warm',
        'palette': [0.8, 0.2],
        'values': {'temp': 5},
      };
      final e = EditState.fromJson(old);
      expect(e.palette, isNull, reason: 'the palette is dropped');
      final pos = palettePadPosition(const Offset(0.8, 0.2));
      expect(pos.dx, greaterThan(0.5), reason: 'warm is on the right of the colour pad');
      expect(pos.dy, greaterThan(0.5), reason: 'rich is at the top');
      expect(e.pads['color']!.x, closeTo(pos.dx, 1e-9));
      expect(e.pads['color']!.y, closeTo(pos.dy, 1e-9));
      expect(e.value('temp'), closeTo(30 * pos.dx + 5, 1e-6), reason: 'the existing value stays as an offset on top of the recipe');
      expect(e.toWire(defaultLook: 'std', defaultToneStrength: 60).containsKey('palette'), isFalse);
      // an edit that already has a colour pad state only loses the palette
      final both = EditState.fromJson({'palette': [0.8, 0.2], 'pads': {'color': {'x': 0.1, 'y': 0.1, 'a': 0.5}}});
      expect(both.palette, isNull);
      expect(both.pads['color']!.x, 0.1);
      // the style presets sit at their looks' own palette points
      final warm = colorPresets.firstWhere((p) => p.look == 'warm');
      final p0 = palettePadPosition(Offset(lookById('warm').px, lookById('warm').py));
      expect(warm.x, closeTo(p0.dx, 1e-9));
    });

    test('every parameter sent to the engine has an item (sharpRadius included); the tone target is reset with the category', () {
      expect(padItems(PadCat.texture).map((i) => i.id), contains('sharpRadius'));
      final e = const EditState().copyWith(toneTarget: 'high-short');
      expect(resetPadCat(e, PadCat.light, ctx).toneTarget, isNull);
      expect(padCatAdjusted(e, PadCat.light, ctx), isTrue);
    });

    test('merging and pasting carry the pad states', () {
      final src = applyPadState(EditState.empty, PadCat.light, const PadState(x: 0.5), ctx);
      final pasted = EditState.empty.pasted(src);
      expect(pasted.pads['light']!.x, 0.5);
      final merged = EditState.empty.merged(src, {SyncCat.light});
      expect(merged.pads['light']!.x, 0.5);
      expect(EditState.empty.merged(src, {SyncCat.color}).pads, isEmpty);
    });
  });

  group('film map layout', () {
    test('every stock gets a dot; positions come from the file; none of the dots touch', () {
      final positions = {
        for (final s in testFilmCatalog.stocks) s.id: Offset(((s.id.hashCode % 100) - 50) / 60, ((s.id.hashCode ~/ 100 % 100) - 50) / 60),
      };
      final dots = layoutFilmMap(testFilmCatalog, positions, withModern: false);
      expect(dots.length, testFilmCatalog.stocks.length);
      expect(minDotDistance(dots), greaterThanOrEqualTo(kFilmDotGap - 1.0));
      for (final d in dots) {
        expect(d.placed, isTrue);
        expect(d.shown.dx.abs(), lessThanOrEqualTo(1.2));
      }
    });

    test('stocks missing from the file go on an automatic grid, still without touching', () {
      final dots = layoutFilmMap(testFilmCatalog, const {}, withModern: false);
      expect(dots.length, testFilmCatalog.stocks.length);
      expect(dots.every((d) => !d.placed), isTrue);
      expect(minDotDistance(dots), greaterThanOrEqualTo(kFilmDotGap - 1.0));
      final partial = layoutFilmMap(testFilmCatalog, {'kodak_portra_400': const Offset(0.5, 0.5)});
      expect(partial.where((d) => d.placed).single.id, 'kodak_portra_400');
    });

    test('ties are separated', () {
      final ids = testFilmCatalog.stocks.take(3).map((s) => s.id).toList();
      final dots = layoutFilmMap(testFilmCatalog, {for (final id in ids) id: const Offset(0.2, 0.2)});
      final three = dots.where((d) => ids.contains(d.id)).toList();
      expect(minDotDistance(three), greaterThan(kFilmDotGap - 1));
    });

    test('parseFilmMap reads {id: {x, y}} and skips garbage', () {
      final m = parseFilmMap('{"a":{"x":0.5,"y":-0.5,"type":"neg","code":"N1"},"b":{"x":"no","y":1},"c":3,"d":{"x":9,"y":-9}}');
      expect(m['a'], const Offset(0.5, -0.5));
      expect(m.containsKey('b'), isFalse);
      expect(m['d'], const Offset(1, -1), reason: 'clamped into the map');
      expect(parseFilmMap('not json'), isEmpty);
      expect(parseFilmMap(null), isEmpty);
    });

    test('the nearest dot is found by drawn position', () {
      final dots = layoutFilmMap(testFilmCatalog, const {}, withModern: false);
      final d = dots[3];
      expect(nearestDot(dots, d.shown + const Offset(0.001, 0.001))!.id, d.id);
    });
  });
}
