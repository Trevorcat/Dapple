// The editorial side of the film library, in one place: display names, grouping and the default print paper.
// Real stock names are trademarks: the UI shows neutral codes (owner decision), see [kFilmDisplayNames]; to rename a
// stock change its entry there and nothing else. Swatch colours are not editorial: the engine renders them
// (dapple_film_swatches_json, FilmCatalog.swatch). Ids are the profile ids of
// the engine (core/data/film/profiles/*.json); an id missing here shows [kFilmUnnamed].
import 'dart:convert';
import 'dart:math' as math;
import 'dart:ui' show Color;

/// stock id -> display name: a neutral code, never a trademark. The scheme (the group header says 负片 / 反转片 /
/// 电影 already, so the code carries type letter + ISO):
///  * N+ISO  colour negative, R+ISO slide, C+ISO+D|T cinema negative (D daylight, T tungsten balanced);
///  * a letter A, B, C... after the ISO tells apart stocks of the same ISO (order of the engine ids);
///  * N800+1 / N800+2  the same stock pushed one / two stops;
///  * P+number print paper, CP+number cinema print film.
/// To rename a stock change its entry here and nothing else. Ids are the profile ids of the engine
/// (core/data/film/profiles/*.json), which stay unchanged.
const Map<String, String> kFilmDisplayNames = {
  // camera negatives
  'fujifilm_c200': 'N200A',
  'kodak_gold_200': 'N200B',
  'kodak_verita_200d': 'N200C',
  'kodak_ektar_100': 'N100',
  'kodak_portra_160': 'N160',
  'fujifilm_pro_400h': 'N400A',
  'fujifilm_xtra_400': 'N400B',
  'kodak_portra_400': 'N400C',
  'kodak_ultramax_400': 'N400D',
  'kodak_portra_800': 'N800',
  'kodak_portra_800_push1': 'N800+1',
  'kodak_portra_800_push2': 'N800+2',
  // cinema negatives
  'kodak_vision3_50d': 'C50D',
  'kodak_vision3_200t': 'C200T',
  'kodak_vision3_250d': 'C250D',
  'kodak_vision3_500t': 'C500T',
  // slides
  'fujifilm_provia_100f': 'R100A',
  'fujifilm_velvia_100': 'R100B',
  'kodak_ektachrome_100': 'R100C',
  'kodak_kodachrome_64': 'R64',
  // print papers and print films
  'fujifilm_crystal_archive_typeii': 'P1',
  'kodak_ektacolor_edge': 'P2',
  'kodak_endura_premier': 'P3',
  'kodak_portra_endura': 'P4',
  'kodak_supra_endura': 'P5',
  'kodak_ultra_endura': 'P6',
  'kodak_2383': 'CP1',
  'kodak_2393': 'CP2',
};

/// What an id that is not in the table is called: never the engine's own name (it is a trademark).
const String kFilmUnnamed = '自定义';

/// Name shown in the UI for [id]; [kFilmUnnamed] when the table has no entry. [engineName] is accepted for the callers
/// that have it but is never shown: the engine reports the manufacturer's trademarked names.
String filmDisplayName(String id, [String? engineName]) => kFilmDisplayNames[id] ?? kFilmUnnamed;

/// Cinema negatives (the engine reports them as plain negatives; the UI groups them apart).
bool isCinemaStock(String id) => id.contains('_vision3_');

/// Print paper a negative gets when the user did not pick one (the engine's own default, docs/film/README.md).
const String kDefaultPaper = 'kodak_portra_endura';
const String kDefaultCinemaPaper = 'kodak_2383';

/// Paper id meaning "no print: scan the film itself" (the `film_paper` wire value).
const String kPaperNone = 'none';

/// Colours of a stock whose swatch the engine has not produced (yet): neutral grey tones.
const List<Color> kFilmSwatchFallback = [Color(0xFFCFC8BC), Color(0xFF9A948A), Color(0xFF7C7469)];

/// Parses the engine's `{"id":["rrggbb",...],...}` swatch table (dapple_film_swatch_bands_json); malformed entries are dropped.
/// Each entry lists the stock's rendering of a skin, a sky and a deep-shadow patch, in that order. These are the raw
/// renderings: real stocks differ by only a few levels, see [characterSwatches].
Map<String, List<Color>> parseFilmSwatches(String? json) {
  final out = <String, List<Color>>{};
  if (json == null || json.isEmpty) return out;
  try {
    final j = jsonDecode(json);
    if (j is! Map) return out;
    j.forEach((id, v) {
      if (id is! String || v is! List || v.length != 3) return;
      final cs = <Color>[];
      for (final h in v) {
        final n = h is String && h.length == 6 ? int.tryParse(h, radix: 16) : null;
        if (n == null) return;
        cs.add(Color(0xFF000000 | n));
      }
      out[id] = cs;
    });
  } catch (_) {}
  return out;
}

// ---- swatch character ------------------------------------------------------------------------------------------------
// Stocks are calibrated to render a given scene alike (grey at the same level, skin within a few levels), so raw swatches of
// N200A and N400A are nearly the same colour. A swatch has to say "this stock" at a glance, so each patch shows how its stock
// deviates from the average stock, amplified: the warm or cool skin, the greener or bluer sky, the lifted, crushed or tinted
// shadow. It is an icon of the stock's character, not a preview of a photo.

double _lin(double v) => v <= 0.04045 ? v / 12.92 : math.pow((v + 0.055) / 1.055, 2.4).toDouble();
double _gam(double v) => v <= 0.0031308 ? v * 12.92 : 1.055 * math.pow(v, 1 / 2.4).toDouble() - 0.055;
double _labF(double t) => t > 0.008856 ? math.pow(t, 1 / 3).toDouble() : 7.787 * t + 16 / 116;
double _labFi(double t) => t * t * t > 0.008856 ? t * t * t : (t - 16 / 116) / 7.787;

/// sRGB colour -> CIE L*a*b* (D65).
List<double> colorToLab(Color c) {
  final r = _lin(c.r), g = _lin(c.g), b = _lin(c.b);
  final x = (0.4124 * r + 0.3576 * g + 0.1805 * b) / 0.9505;
  final y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  final z = (0.0193 * r + 0.1192 * g + 0.9505 * b) / 1.089;
  final fx = _labF(x), fy = _labF(y), fz = _labF(z);
  return [116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz)];
}

/// CIE L*a*b* (D65) -> sRGB colour, clamped into the gamut.
Color labToColor(List<double> lab) {
  final fy = (lab[0] + 16) / 116, fx = fy + lab[1] / 500, fz = fy - lab[2] / 200;
  final x = _labFi(fx) * 0.9505, y = _labFi(fy), z = _labFi(fz) * 1.089;
  final r = 3.2406 * x - 1.5372 * y - 0.4986 * z;
  final g = -0.9689 * x + 1.8758 * y + 0.0415 * z;
  final b = 0.0557 * x - 0.2040 * y + 1.0570 * z;
  int ch(double v) => (_gam(v.clamp(0.0, 1.0)) * 255).round().clamp(0, 255);
  return Color(0xFF000000 | (ch(r) << 16) | (ch(g) << 8) | ch(b));
}

/// Mean distance (CIE76 delta E over the bands) between two swatches of equal length.
double swatchDistance(List<Color> a, List<Color> b) {
  var sum = 0.0;
  for (var i = 0; i < a.length; i++) {
    final x = colorToLab(a[i]), y = colorToLab(b[i]);
    sum += math.sqrt(math.pow(x[0] - y[0], 2) + math.pow(x[1] - y[1], 2) + math.pow(x[2] - y[2], 2));
  }
  return sum / a.length;
}

/// Raw swatches (see [parseFilmSwatches]) -> swatches that show each stock's character: per band, the stock's deviation from
/// the mean of all stocks, in L*a*b*, is multiplied by [lightGain] (L*) and [chromaGain] (a*, b*) around that mean.
Map<String, List<Color>> characterSwatches(Map<String, List<Color>> raw, {double lightGain = 2.5, double chromaGain = 4}) {
  if (raw.length < 2) return Map.of(raw);
  final n = raw.values.map((v) => v.length).reduce(math.min);
  final labs = {
    for (final e in raw.entries) e.key: [for (var i = 0; i < n; i++) colorToLab(e.value[i])]
  };
  final mean = [
    for (var i = 0; i < n; i++)
      [for (var k = 0; k < 3; k++) labs.values.map((l) => l[i][k]).reduce((a, b) => a + b) / labs.length]
  ];
  return {
    for (final e in labs.entries)
      e.key: [
        for (var i = 0; i < n; i++)
          labToColor([
            mean[i][0] + lightGain * (e.value[i][0] - mean[i][0]),
            mean[i][1] + chromaGain * (e.value[i][1] - mean[i][1]),
            mean[i][2] + chromaGain * (e.value[i][2] - mean[i][2]),
          ])
      ]
  };
}

/// The colour stops for a 36 px swatch: each band twice, so a diagonal gradient shows three (nearly) flat bands.
List<Color> swatchBands(List<Color> bands) => [for (final c in bands) ...[c, c]];
