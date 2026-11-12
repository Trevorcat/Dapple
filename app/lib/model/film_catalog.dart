// The film library as the UI sees it: the stock list reported by the engine (dapple_film_list_json),
// grouped for the picker. Names, swatches and the default paper live in film_names.dart.
import 'dart:convert';
import 'dart:ui' show Color;

import 'film_names.dart';

enum FilmGroup { negative, slide, cinema }

const filmGroupNames = {FilmGroup.negative: '负片', FilmGroup.slide: '反转片', FilmGroup.cinema: '电影'};

class FilmStock {
  const FilmStock(this.id, this.engineName, this.group);
  final String id, engineName;
  final FilmGroup group;
  String get name => filmDisplayName(id, engineName);

  /// A negative (camera or cinema) is printed: it has a paper, print warmth and tint.
  bool get printed => group != FilmGroup.slide;
}

class FilmPaper {
  const FilmPaper(this.id, this.engineName);
  final String id, engineName;
  String get name => filmDisplayName(id, engineName);
}

class FilmCatalog {
  const FilmCatalog._(this.stocks, this.papers, this.attribution, [this.swatches = const {}]);

  /// The same catalog with engine-rendered swatches ([s] = the raw table of [parseFilmSwatches]; the stock's character is
  /// brought out here, see [characterSwatches]).
  FilmCatalog withSwatches(Map<String, List<Color>> s) => FilmCatalog._(stocks, papers, attribution, characterSwatches(s));

  /// Three-band swatch colours (skin, sky, shadow) per stock id, derived from the engine (test patches through the stock);
  /// empty until computed.
  final Map<String, List<Color>> swatches;

  /// Colour stops of [id]'s swatch (each of the three bands twice, for a banded gradient), [kFilmSwatchFallback] while the
  /// engine's are not there.
  List<Color> swatch(String id) {
    final b = swatches[id];
    return b == null ? kFilmSwatchFallback : swatchBands(b);
  }

  /// No pack loaded: the film rows are not offered.
  static const empty = FilmCatalog._([], [], '');

  /// Camera films ordered negative, slide, cinema (the order of the picker and of the 胶片 row).
  final List<FilmStock> stocks;
  final List<FilmPaper> papers;

  /// Licence line of the pack (dapple_film_attribution), shown on the licences page.
  final String attribution;

  bool get isEmpty => stocks.isEmpty;

  FilmStock? stock(String? id) {
    if (id == null) return null;
    for (final s in stocks) {
      if (s.id == id) return s;
    }
    return null;
  }

  List<FilmStock> inGroup(FilmGroup g) => [for (final s in stocks) if (s.group == g) s];

  /// Paper a printed stock gets when the edit names none (the engine's default).
  String defaultPaperOf(FilmStock s) {
    final p = s.group == FilmGroup.cinema ? kDefaultCinemaPaper : kDefaultPaper;
    if (papers.any((x) => x.id == p)) return p;
    return papers.isEmpty ? kPaperNone : papers.first.id;
  }

  /// Papers offered in the paper row: "no print" first, then the library's papers.
  List<String> get paperIds => [kPaperNone, for (final p in papers) p.id];

  String paperName(String id) {
    if (id == kPaperNone) return '直接扫描';
    for (final p in papers) {
      if (p.id == id) return p.name;
    }
    return filmDisplayName(id);
  }

  /// Parses `{"loaded":bool,"stocks":[{"id","name","type"}]}`. Anything unusable gives [empty].
  factory FilmCatalog.fromJson(String? json, {String attribution = ''}) {
    if (json == null || json.isEmpty) return empty;
    try {
      final j = jsonDecode(json);
      if (j is! Map || j['loaded'] != true) return empty;
      final byGroup = {for (final g in FilmGroup.values) g: <FilmStock>[]};
      final papers = <FilmPaper>[];
      for (final e in (j['stocks'] as List)) {
        final m = (e as Map).cast<String, dynamic>();
        final id = m['id'] as String, name = (m['name'] as String?) ?? id;
        switch (m['type']) {
          case 'negative':
            byGroup[isCinemaStock(id) ? FilmGroup.cinema : FilmGroup.negative]!.add(FilmStock(id, name, isCinemaStock(id) ? FilmGroup.cinema : FilmGroup.negative));
          case 'slide':
            byGroup[FilmGroup.slide]!.add(FilmStock(id, name, FilmGroup.slide));
          case 'paper':
            papers.add(FilmPaper(id, name));
        }
      }
      return FilmCatalog._([for (final g in FilmGroup.values) ...byGroup[g]!], papers, attribution);
    } catch (_) {
      return empty;
    }
  }
}
