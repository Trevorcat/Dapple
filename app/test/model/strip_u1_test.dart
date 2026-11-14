// U1 (one meaning for every strength strip): the readout changed, the strip's mapping to the parameters did not. These pins are the
// values of padRecipe / the portrait recipe as they were before U1 (round 1, main 52e9950), so a saved edit renders the same: the
// engine receives the same numbers for the same stored strip positions.
import 'dart:convert';

import 'package:dapple/model/params.dart';
import 'package:dapple/model/local_adjust.dart';
import 'package:dapple/model/local_pad.dart';
import 'package:dapple/model/pad_model.dart';
import 'package:dapple/model/portrait.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  const ctx = PadCtx(defaultTone: 60);

  group('the strip mapping is unchanged (saved edits look identical)', () {
    test('global recipes at the bottom, a quarter, the middle, three quarters and the top', () {
      // [a] -> the strength-carrying parameters, pinned
      final light = {0.0: 0.0, 0.25: 30.0, 0.5: 60.0, 0.75: 80.0, 1.0: 100.0};
      light.forEach((a, v) => expect(padRecipe(PadCat.light, 0, 0, a, ctx: ctx)['toneStrength'], closeTo(v, 1e-9), reason: 'tone at $a'));
      final look = {0.0: 0.0, 0.25: 50.0, 0.5: 100.0, 0.75: 100.0, 1.0: 100.0};
      look.forEach((a, v) => expect(padRecipe(PadCat.color, 0, 0, a, ctx: ctx)['lookStrength'], closeTo(v, 1e-9), reason: 'look at $a'));
      expect(padRecipe(PadCat.color, 0, 0, 1)['vibrance'], closeTo(20, 1e-9));
      expect(padRecipe(PadCat.color, 0, 0, 1)['saturation'], closeTo(8, 1e-9));
      final film = {0.0: 0.0, 0.25: 50.0, 0.5: 100.0, 0.75: 100.0, 1.0: 100.0};
      film.forEach((a, v) => expect(padRecipe(PadCat.film, 0, 0, a)['film_strength'], closeTo(v, 1e-9), reason: 'film at $a'));
      expect(padRecipe(PadCat.film, 0, 0, 1)['film_grain'], closeTo(200, 1e-9));
      expect(padRecipe(PadCat.film, 0, 0, 1)['film_black'], closeTo(-0.5, 1e-9));
      final sharpen = {0.0: 0.0, 0.25: 20.0, 0.5: 40.0, 0.75: 56.0, 1.0: 72.0};
      sharpen.forEach((a, v) => expect(padRecipe(PadCat.texture, 0, 0, a)['sharpen'], closeTo(v, 1e-9), reason: 'sharpen at $a'));
    });

    test('an old edit (no pad state) and a stored pad state read back to the same strip position and parameters', () {
      // stored before U1: tone strength 80 (the strip above the middle), a colour pad state with a = 0.75
      final json = {
        'toneStrength': 80,
        'lookStrength': 100,
        'pads': {
          'color': {'x': 0.2, 'y': -0.1, 'a': 0.75, 'off': <String, double>{}},
        },
      };
      final e = EditState.fromJson(jsonDecode(jsonEncode(json)) as Map<String, dynamic>);
      final light = padStateOf(e, PadCat.light, ctx);
      expect(light.a, closeTo(0.75, 1e-9), reason: 'tone 80 with the default 60 is three quarters up');
      expect(padStateOf(e, PadCat.color, ctx).a, 0.75);
      expect(e.toneStrength, 80);
      expect(e.lookStrength, 100);
    });

    test('local and portrait strips: engine strength and recipe values unchanged', () {
      final a = LocalAdjustment(id: 'a', mask: const [], ui: LocalUi.centre);
      expect(applyLocalUi(a, LocalUi.centre.copyWith(a: 0.25)).strength, closeTo(50, 1e-9));
      expect(applyLocalUi(a, LocalUi.centre.copyWith(a: 0.5)).strength, closeTo(100, 1e-9));
      final skin = specOf(PortraitChip.skin).areas.first;
      expect(portraitRecipe(skin, 0.5)['smooth'], 30);
      expect(portraitRecipe(skin, 1.0)['smooth'], 100);
      expect(portraitRecipe(skin, 0.25)['smooth'], 15);
    });
  });

  group('what the strip reads (U1): bottom 0, middle 100 %, top = the most it gives', () {
    test('global categories', () {
      expect(stripPercent(PadCat.light, 0, ctx: ctx), 0);
      expect(stripPercent(PadCat.light, 0.5, ctx: ctx), closeTo(100, 1e-9));
      expect(stripPercent(PadCat.light, 1, ctx: ctx).round(), 167, reason: 'tone 100 over the default 60');
      expect(stripPercent(PadCat.color, 0.5), 100);
      expect(stripPercent(PadCat.color, 1), 200);
      expect(stripPercent(PadCat.film, 0.5), 100);
      expect(stripPercent(PadCat.film, 1), 200);
      expect(stripPercent(PadCat.texture, 0.5), 100);
      expect(stripPercent(PadCat.texture, 1), closeTo(180, 1e-9));
      expect(padReadout(PadCat.light, const PadState(a: 1), ctx: ctx), '强度 167%');
    });

    test('local areas and portrait chips', () {
      expect(localStripPercent(null, 0.5), 100);
      expect(localStripPercent(null, 1), 200);
      final skin = LocalAdjustment(id: 'pt-skin', name: portraitName('pt-skin'), mask: const [], ui: LocalUi.centre);
      expect(localStripPercent(skin, 0.5), closeTo(100, 1e-9));
      expect(localStripPercent(skin, 1).round(), 333, reason: 'smoothing 30 -> 100');
      final teeth = LocalAdjustment(id: 'pt-teeth', name: portraitName('pt-teeth'), mask: const [], ui: LocalUi.centre);
      expect(localStripPercent(teeth, 1).round(), 200);
    });
  });
}
