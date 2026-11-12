// The film map (docs/design/editor-panel.md section 7): every catalog stock is a dot placed by its character, x = pale (-1) ..
// rich (+1), y = low (-1) .. high (+1) contrast. The positions come from assets/film/film_map.json (measured, see
// docs/film/film-map.md: {stock id: {x, y, type, code}}); a stock missing from the file gets a place on an automatic grid.
// The dots are then nudged apart so that none touches another (relative order kept).
import 'dart:convert';
import 'dart:math' as math;
import 'dart:ui' show Offset, Size;

import 'film_catalog.dart';

const String kFilmMapAsset = 'assets/film/film_map.json';

/// Parses the asset text into {id: character position}; anything unusable is skipped.
Map<String, Offset> parseFilmMap(String? text) {
  final out = <String, Offset>{};
  if (text == null || text.isEmpty) return out;
  try {
    final j = jsonDecode(text);
    if (j is! Map) return out;
    j.forEach((k, v) {
      if (k is String && v is Map && v['x'] is num && v['y'] is num) {
        final x = (v['x'] as num).toDouble(), y = (v['y'] as num).toDouble();
        if (x.isFinite && y.isFinite) out[k] = Offset(x.clamp(-1.0, 1.0), y.clamp(-1.0, 1.0));
      }
    });
  } catch (_) {}
  return out;
}

class FilmDot {
  const FilmDot(this.stock, this.character, this.shown, {required this.placed});
  final FilmStock stock;

  /// The measured character (x pale..rich, y low..high contrast), or the grid slot when [placed] is false.
  final Offset character;

  /// Where the dot is drawn, in the same -1..1 coordinates (nudged apart).
  final Offset shown;

  /// True when the position comes from the file.
  final bool placed;
  String get id => stock.id;
  String get name => stock.name;
}

/// The reference map the nudging works in (dp): the dots are 18 dp, so they keep at least [kFilmDotGap] between centres.
const Size kFilmMapRef = Size(340, 210);
const double kFilmDotGap = 27;
const double kFilmMapX = 0.38, kFilmMapY = 0.32; // fraction of the map's width / height one unit of x / y covers

Offset _toRef(Offset c) => Offset((0.5 + c.dx * kFilmMapX) * kFilmMapRef.width, (0.5 - c.dy * kFilmMapY) * kFilmMapRef.height);
Offset _fromRef(Offset p) => Offset((p.dx / kFilmMapRef.width - 0.5) / kFilmMapX, (0.5 - p.dy / kFilmMapRef.height) / kFilmMapY);

/// Places the stocks of [catalog]. Deterministic.
List<FilmDot> layoutFilmMap(FilmCatalog catalog, Map<String, Offset> positions) {
  final stocks = catalog.stocks;
  final missing = [for (final s in stocks) if (!positions.containsKey(s.id)) s];
  // an automatic grid over the whole map for the stocks the file does not know
  final cols = math.max(1, math.sqrt(missing.length * 1.6).ceil());
  final rows = missing.isEmpty ? 1 : (missing.length / cols).ceil();
  final slot = <String, Offset>{
    for (var i = 0; i < missing.length; i++)
      missing[i].id: Offset(cols == 1 ? 0 : -0.85 + 1.7 * (i % cols) / (cols - 1), rows == 1 ? 0 : 0.85 - 1.7 * (i ~/ cols) / (rows - 1)),
  };
  final ch = [for (final s in stocks) positions[s.id] ?? slot[s.id]!];
  final p = [for (final c in ch) _toRef(c)];
  // keep the dots inside the margin the four edge icons need (a little beyond the -1..1 square)
  final minX = kFilmMapRef.width * (0.5 - kFilmMapX * 1.12), maxX = kFilmMapRef.width * (0.5 + kFilmMapX * 1.12);
  final minY = kFilmMapRef.height * (0.5 - kFilmMapY * 1.12), maxY = kFilmMapRef.height * (0.5 + kFilmMapY * 1.12);
  for (var pass = 0; pass < 240; pass++) {
    var moved = false;
    for (var i = 0; i < p.length; i++) {
      for (var j = i + 1; j < p.length; j++) {
        var d = p[j] - p[i];
        var dist = d.distance;
        if (dist >= kFilmDotGap) continue;
        if (dist < 1e-6) {
          d = Offset(math.cos(i * 2.399), math.sin(i * 2.399)); // a tie: spread in a deterministic direction
          dist = 1;
        }
        final push = (kFilmDotGap - dist) / 2 / dist * 1.02;
        p[i] = Offset((p[i].dx - d.dx * push).clamp(minX, maxX), (p[i].dy - d.dy * push).clamp(minY, maxY));
        p[j] = Offset((p[j].dx + d.dx * push).clamp(minX, maxX), (p[j].dy + d.dy * push).clamp(minY, maxY));
        moved = true;
      }
    }
    if (!moved) break;
  }
  return [for (var i = 0; i < stocks.length; i++) FilmDot(stocks[i], ch[i], _fromRef(p[i]), placed: positions.containsKey(stocks[i].id))];
}

/// Smallest distance between two dots in the reference map (tests: no two touch).
double minDotDistance(List<FilmDot> dots) {
  var m = double.infinity;
  for (var i = 0; i < dots.length; i++) {
    for (var j = i + 1; j < dots.length; j++) {
      m = math.min(m, (_toRef(dots[i].shown) - _toRef(dots[j].shown)).distance);
    }
  }
  return m;
}

/// The dot nearest to a point of the map given in -1..1 coordinates (drawn positions).
FilmDot? nearestDot(List<FilmDot> dots, Offset at) {
  FilmDot? best;
  var bd = double.infinity;
  final a = _toRef(at);
  for (final d in dots) {
    final dd = (_toRef(d.shown) - a).distanceSquared;
    if (dd < bd) {
      bd = dd;
      best = d;
    }
  }
  return best;
}

/// Colour class of a dot: negative amber, reversal blue, cine teal.
enum FilmDotType { negative, slide, cinema }

FilmDotType dotTypeOf(FilmStock s) => switch (s.group) {
      FilmGroup.negative => FilmDotType.negative,
      FilmGroup.slide => FilmDotType.slide,
      FilmGroup.cinema => FilmDotType.cinema,
    };
