import 'dart:convert';
import 'dart:ui' show Color;

import 'package:dapple/model/film_catalog.dart';
import 'package:dapple/model/film_names.dart';
import 'package:flutter_test/flutter_test.dart';

/// The raw table of dapple_film_swatch_bands_json (skin, sky and deep-shadow patches through each camera stock with its default
/// paper; dumped from the desktop engine build). Real stocks differ by a few levels only.
const _raw = {
  'fujifilm_c200': ['c49770', '5997ca', '181819'],
  'fujifilm_pro_400h': ['b9957a', '6296c3', '191b19'],
  'fujifilm_provia_100f': ['c0a28b', '7192b3', '1a171a'],
  'fujifilm_velvia_100': ['c2a386', '7196c2', '0c0c0e'],
  'fujifilm_xtra_400': ['c69a6d', '5796cd', '141617'],
  'kodak_ektachrome_100': ['b69c83', '7191b3', '181614'],
  'kodak_ektar_100': ['bc9174', '5a9cc6', '18191a'],
  'kodak_gold_200': ['bc9279', '6a9cc3', '181b19'],
  'kodak_kodachrome_64': ['b09484', '7594a7', '1d1916'],
  'kodak_portra_160': ['b89278', '629fc7', '181c1d'],
  'kodak_portra_400': ['ba9279', '629cc5', '181a1a'],
  'kodak_portra_800': ['b8927a', '6297c1', '1a1c1c'],
  'kodak_portra_800_push1': ['c29470', '579bcb', '131517'],
  'kodak_portra_800_push2': ['ca9260', '4c9ecd', '111416'],
  'kodak_ultramax_400': ['ba947a', '6397c2', '1a1c1b'],
  'kodak_verita_200d': ['b29079', '6991b3', '081111'],
  'kodak_vision3_200t': ['b19279', '7391b3', '0b1411'],
  'kodak_vision3_250d': ['b59279', '6791b1', '0c1310'],
  'kodak_vision3_500t': ['b19178', '7493b3', '0c1310'],
  'kodak_vision3_50d': ['b79479', '6890b5', '0a1311'],
};

void main() {
  final parsed = parseFilmSwatches(jsonEncode(_raw));

  test('the raw engine swatches of N200A and N400A are close to identical', () {
    expect(parsed.length, _raw.length);
    final a = parsed['fujifilm_c200']!, b = parsed['fujifilm_pro_400h']!;
    expect(swatchDistance(a, b), lessThan(8), reason: 'this is the problem the character step solves');
  });

  test('character swatches make the stocks tell apart; same-family stocks stay similar', () {
    final out = characterSwatches(parsed);
    expect(out.keys.toSet(), parsed.keys.toSet());
    final a = out['fujifilm_c200']!, b = out['fujifilm_pro_400h']!;
    expect(swatchDistance(a, b), greaterThan(swatchDistance(parsed['fujifilm_c200']!, parsed['fujifilm_pro_400h']!) * 3));
    final ids = out.keys.toList();
    final ds = <double>[];
    for (var i = 0; i < ids.length; i++) {
      for (var j = i + 1; j < ids.length; j++) {
        ds.add(swatchDistance(out[ids[i]]!, out[ids[j]]!));
      }
    }
    ds.sort();
    final median = ds[ds.length ~/ 2];
    final tooClose = ds.where((d) => d < 6).length;
    // measured on this table: median ~19, 4 of 190 pairs below 6 (stocks that are the same emulsion at another speed, or
    // pushed): ok. The raw N200A / N400A distance is 4.9.
    expect(median, greaterThan(15));
    expect(tooClose, lessThanOrEqualTo(8));
    for (final s in out.values) {
      expect(s.length, 3);
    }
  });

  test('swatch() gives banded colour stops and the neutral fallback before the engine table is there', () {
    final stocks = [for (final id in parsed.keys) FilmStock(id, id, FilmGroup.negative)];
    final cat = FilmCatalog.fromJson(jsonEncode({
      'loaded': true,
      'stocks': [for (final s in stocks) {'id': s.id, 'name': s.id, 'type': 'negative'}]
    })).withSwatches(parsed);
    final sw = cat.swatch('fujifilm_c200');
    expect(sw.length, 6);
    expect(sw[0], sw[1]);
    expect(sw[2], sw[3]);
    expect(sw[4], sw[5]);
    expect(sw[0], isNot(sw[2]));
    expect(cat.swatch('nope'), kFilmSwatchFallback);
  });

  test('Lab round trip keeps colours', () {
    for (final c in const [Color(0xFFC49770), Color(0xFF5997CA), Color(0xFF101010), Color(0xFFFFFFFF)]) {
      final r = labToColor(colorToLab(c));
      expect((r.r - c.r).abs() * 255, lessThan(1.5));
      expect((r.g - c.g).abs() * 255, lessThan(1.5));
      expect((r.b - c.b).abs() * 255, lessThan(1.5));
    }
  });
}
