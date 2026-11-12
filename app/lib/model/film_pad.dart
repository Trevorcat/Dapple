// Effective ranges of the film control pads for the current photo and stock, as the engine reports them
// (dapple_session_film_pad_json). A pad axis end that does not change the render (a slide has no print:
// film_print +1 does nothing; film_wb +1 does nothing when the scene light already matches the stock's balance)
// is "ineffective": the UI makes it unreachable.
//
// Expected engine JSON (all fields optional; anything missing means "effective"):
//   {"ok":true, ...,
//    "effective_lo": {"film_shoot":true,"film_print":true,"film_black":true,"film_wb":true,"film_latitude":true},
//    "effective_hi": {"film_shoot":true,"film_print":false,"film_black":true,"film_wb":false,"film_latitude":true},
//    "scene_k": 4600, "stock_k": 5500}
// A per-key object form is read too:  {"film_print":{"effective_lo":true,"effective_hi":false}, ...}.
// scene_k / stock_k (also scene_kelvin / stock_kelvin): the scene's colour temperature and the stock's reference
// temperature, shown as small labels at the two ends of the 质感 pad's X axis when present.
import 'dart:convert';

/// The keys the engine reports effective ranges for.
const filmPadKeys = ['film_shoot', 'film_print', 'film_black', 'film_wb', 'film_latitude'];

/// Keys whose range is one-sided: 0 (the default, always effective) to +1. Their usable end is the "hi" end.
const filmOneSidedKeys = {'film_wb'};

/// Which ends of each pad key change the render.
class FilmPadRanges {
  const FilmPadRanges([this._lo = const {}, this._hi = const {}, this.sceneK, this.stockK]);

  /// Everything effective (no engine answer yet, an old engine, an error).
  static const all = FilmPadRanges();

  final Map<String, bool> _lo, _hi;

  /// Scene colour temperature and the stock's reference temperature in kelvin, when the engine reports them.
  final int? sceneK, stockK;

  /// Whether moving [key] to -1 (resp. +1) changes the render; true when unknown.
  bool lo(String key) => _lo[key] ?? true;
  bool hi(String key) => _hi[key] ?? true;

  /// Nothing the user can reach changes the render: both ends are dead (a one-sided key only has its "hi" end).
  bool dead(String key) => filmOneSidedKeys.contains(key) ? !hi(key) : (!lo(key) && !hi(key));

  /// Parses the engine answer; tolerant of missing fields, wrong types and garbage.
  factory FilmPadRanges.parse(String? json) {
    if (json == null || json.isEmpty) return all;
    Object? j;
    try {
      j = jsonDecode(json);
    } catch (_) {
      return all;
    }
    if (j is! Map) return all;
    final Map m = j;
    if (m['ok'] == false) return all;
    bool? flag(String field, String key) {
      final f = m[field];
      if (f is Map && f[key] is bool) return f[key] as bool;
      final k = m[key];
      if (k is Map && k[field] is bool) return k[field] as bool;
      return null;
    }

    int? kelvin(List<String> names) {
      for (final n in names) {
        final v = m[n];
        if (v is num && v.isFinite && v > 0 && v < 100000) return v.round();
      }
      return null;
    }

    final lo = <String, bool>{}, hi = <String, bool>{};
    for (final k in filmPadKeys) {
      final a = flag('effective_lo', k), b = flag('effective_hi', k);
      if (a != null) lo[k] = a;
      if (b != null) hi[k] = b;
    }
    return FilmPadRanges(lo, hi, kelvin(const ['scene_k', 'scene_kelvin', 'scene_cct']), kelvin(const ['stock_k', 'stock_kelvin', 'stock_cct']));
  }

  @override
  bool operator ==(Object other) => other is FilmPadRanges && other.toString() == toString();

  @override
  int get hashCode => toString().hashCode;

  @override
  String toString() =>
      'FilmPadRanges(${[for (final k in filmPadKeys) '$k:${lo(k) ? '-' : 'x'}${hi(k) ? '+' : 'x'}'].join(' ')} $sceneK/$stockK)';
}
