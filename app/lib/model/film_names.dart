// The editorial side of the film library, in one place: display names, grouping and the default print paper.
// Real stock names are trademarks: the UI shows neutral codes (owner decision), see [kFilmDisplayNames]; to rename a
// stock change its entry there and nothing else. Swatch colours are not editorial: the engine renders them
// (dapple_film_swatches_json, FilmCatalog.swatch). Ids are the profile ids of
// the engine (core/data/film/profiles/*.json); an id missing here shows [kFilmUnnamed].
import 'dart:convert';
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

/// Parses the engine's `{"id":["rrggbb",...],...}` swatch table (dapple_film_swatches_json); malformed entries are dropped.
/// Each swatch lists the stock's rendering of a skin, a neutral and a sky patch, in that order.
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
      // engine order: neutral, skin, sky -> swatch order: skin, neutral, sky
      out[id] = [cs[1], cs[0], cs[2]];
    });
  } catch (_) {}
  return out;
}
