import 'dart:convert';

/// JSON text that is already written (a brush stroke's points never change, so they are written once). [encodeWire] copies it as it is;
/// any other encoder reads it back through [toJson], so a `jsonEncode` of a wire map still gives the same text.
class RawJson {
  const RawJson(this.text);
  final String text;
  Object? toJson() => jsonDecode(text);
}

/// `jsonEncode` for the engine's parameter maps: the same text, with [RawJson] parts copied instead of walked. Every frame of a drag sends
/// the whole edit, and an edit with brush strokes is mostly their points.
String encodeWire(Object? v) {
  final b = StringBuffer();
  _write(b, v);
  return b.toString();
}

void _write(StringBuffer b, Object? v) {
  switch (v) {
    case null:
      b.write('null');
    case RawJson():
      b.write(v.text);
    case String():
      b.write(jsonEncode(v));
    case int():
      b.write(v);
    case double() when v.isFinite:
      b.write(v);
    case bool():
      b.write(v);
    case Map():
      b.write('{');
      var first = true;
      for (final e in v.entries) {
        if (e.key is! String) throw JsonUnsupportedObjectError(v);
        if (!first) b.write(',');
        first = false;
        b.write(jsonEncode(e.key));
        b.write(':');
        _write(b, e.value);
      }
      b.write('}');
    case List():
      b.write('[');
      for (var i = 0; i < v.length; i++) {
        if (i > 0) b.write(',');
        _write(b, v[i]);
      }
      b.write(']');
    default:
      b.write(jsonEncode(v)); // objects with toJson, non-finite numbers (throws like jsonEncode)
  }
}
