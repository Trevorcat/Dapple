// Effective ranges of the film control pads for the current photo and stock, as the engine reports them
// (dapple_session_film_pad_json). A pad axis end that does not change the render (a slide has no print:
// film_print +1 and film_cast do nothing) is "ineffective": the UI makes it unreachable.
//
// Expected engine JSON (all fields optional; anything missing means "effective"):
//   {"ok":true, ...,
//    "effective_lo": {"film_shoot":true,"film_print":true,"film_black":true,"film_cast":true},
//    "effective_hi": {"film_shoot":true,"film_print":false,"film_black":true,"film_cast":false}}
// A per-key object form is read too:  {"film_print":{"effective_lo":true,"effective_hi":false}, ...}.
import 'dart:convert';

/// The four pad keys the engine reports on.
const filmPadKeys = ['film_shoot', 'film_print', 'film_black', 'film_cast'];

/// Which ends of each pad key change the render.
class FilmPadRanges {
  const FilmPadRanges([this._lo = const {}, this._hi = const {}]);

  /// Everything effective (no engine answer yet, an old engine, an error).
  static const all = FilmPadRanges();

  final Map<String, bool> _lo, _hi;

  /// Whether moving [key] to -1 (resp. +1) changes the render; true when unknown.
  bool lo(String key) => _lo[key] ?? true;
  bool hi(String key) => _hi[key] ?? true;

  /// Neither end changes anything: the axis is dead.
  bool dead(String key) => !lo(key) && !hi(key);

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

    final lo = <String, bool>{}, hi = <String, bool>{};
    for (final k in filmPadKeys) {
      final a = flag('effective_lo', k), b = flag('effective_hi', k);
      if (a != null) lo[k] = a;
      if (b != null) hi[k] = b;
    }
    return FilmPadRanges(lo, hi);
  }

  @override
  bool operator ==(Object other) =>
      other is FilmPadRanges && [for (final k in filmPadKeys) (lo(k), hi(k))].toString() == [for (final k in filmPadKeys) (other.lo(k), other.hi(k))].toString();

  @override
  int get hashCode => Object.hashAll([for (final k in filmPadKeys) Object.hash(lo(k), hi(k))]);

  @override
  String toString() => 'FilmPadRanges(${[for (final k in filmPadKeys) '$k:${lo(k) ? '-' : 'x'}${hi(k) ? '+' : 'x'}'].join(' ')})';
}
