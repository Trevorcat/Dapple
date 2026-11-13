import 'package:path/path.dart' as p;

const _maxStem = 120;

/// A file-name stem for any externally supplied id (PHAsset identifiers contain '/', MediaStore ids ':', names can be anything).
/// Keeps [A-Za-z0-9._-]; ':' becomes '_' (the historical rule, so "ms:123" stays "ms_123" and no existing preview is orphaned).
/// Every other character becomes '_' and a stable FNV-1a hash of the full original id is appended, as it is for a truncated id,
/// so different ids that differ in anything but ':' can not share a name ("a/b" vs "a_b"). Never empty, never all dots.
String safeFileStem(String id) {
  var changed = false;
  final sb = StringBuffer();
  for (final r in id.runes) {
    final c = String.fromCharCode(r);
    if (RegExp(r'^[A-Za-z0-9._-]$').hasMatch(c)) {
      sb.write(c);
    } else if (c == ':') {
      sb.write('_');
    } else {
      sb.write('_');
      changed = true;
    }
  }
  var s = sb.toString();
  if (s.length > _maxStem) {
    s = s.substring(0, _maxStem);
    changed = true;
  }
  if (s.isEmpty || RegExp(r'^\.+$').hasMatch(s)) changed = true;
  return changed ? '${s}_${fnv1aHex(id)}' : s;
}

/// A safe file name for an externally supplied name: stem through [safeFileStem], extension reduced to [A-Za-z0-9.].
String safeFileName(String name) {
  final base = name.replaceAll('\\', '/').split('/').last;
  final ext = p.extension(base).replaceAll(RegExp(r'[^A-Za-z0-9.]'), '');
  final stem = base.substring(0, base.length - p.extension(base).length);
  return '${safeFileStem(stem.isEmpty ? name : stem)}${ext.length > 12 ? '' : ext}';
}

/// 32-bit FNV-1a of the UTF-8 bytes, 8 hex digits; stable across runs and platforms.
String fnv1aHex(String s) {
  var h = 0x811c9dc5;
  for (final b in s.codeUnits.expand((u) => u < 0x80 ? [u] : _utf8(u))) {
    h = ((h ^ b) * 0x01000193) & 0xffffffff;
  }
  return h.toRadixString(16).padLeft(8, '0');
}

List<int> _utf8(int u) => u < 0x800 ? [0xC0 | (u >> 6), 0x80 | (u & 63)] : [0xE0 | (u >> 12), 0x80 | ((u >> 6) & 63), 0x80 | (u & 63)];
