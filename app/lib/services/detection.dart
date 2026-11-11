import '../models.dart';

/// A bracket set or panorama sequence found by `dapple_detect_sequences_json`, mapped to library photos.
class DetectedGroup {
  DetectedGroup({
    required this.kind,
    required this.confidence,
    required this.photoIds,
    required this.evSteps,
    required this.startMs,
    this.reason = '',
  });

  /// "bracket" | "panorama"
  final String kind;
  final double confidence;
  final List<String> photoIds;

  /// EV relative to the first frame, parallel to [photoIds] (null when unknown).
  final List<double?> evSteps;
  final int startMs;
  final String reason;

  bool get isBracket => kind == 'bracket';
  int get length => photoIds.length;
  String get key => '$kind:${photoIds.join(',')}';

  /// "−2 / 0 / +2 EV" (bracket) or "水平拍摄" (panorama).
  String get subtitle {
    if (!isBracket) return '连续拍摄';
    final l = [for (final e in evSteps) ?e]..sort();
    if (l.isEmpty) return '包围曝光';
    return '${l.map(evNumber).join(' / ')} EV';
  }

  String get title => isBracket ? 'HDR · $length 张包围曝光' : '全景 · $length 张连续';

  Map<String, dynamic> toJson() => {
        'kind': kind,
        'confidence': confidence,
        'ids': photoIds,
        'ev': evSteps,
        'startMs': startMs,
        'reason': reason,
      };

  factory DetectedGroup.fromJson(Map<String, dynamic> j) => DetectedGroup(
        kind: j['kind'] as String,
        confidence: (j['confidence'] as num).toDouble(),
        photoIds: (j['ids'] as List).cast<String>(),
        evSteps: [for (final e in j['ev'] as List) (e as num?)?.toDouble()],
        startMs: (j['startMs'] as num).toInt(),
        reason: (j['reason'] as String?) ?? '',
      );
}

/// "−2", "0", "+1.5" with the typographic minus of the design.
String evNumber(double ev) {
  final r = (ev * 10).round() / 10;
  if (r == 0) return '0';
  final body = r == r.roundToDouble() ? r.abs().toStringAsFixed(0) : r.abs().toStringAsFixed(1);
  return r < 0 ? '−$body' : '+$body';
}

/// Converts the engine JSON into groups worth showing. [idByPath] maps the paths passed to the engine
/// back to library ids. Weak candidates are dropped: a 2-frame panorama needs real overlap evidence.
List<DetectedGroup> parseDetection(Map<String, dynamic> json, Map<String, String> idByPath, Map<String, int> timeById) {
  final out = <DetectedGroup>[];
  for (final g in (json['groups'] as List? ?? const [])) {
    final m = (g as Map).cast<String, dynamic>();
    final kind = m['kind'] == 'bracket' ? 'bracket' : 'panorama';
    final conf = (m['confidence'] as num?)?.toDouble() ?? 0;
    final ids = <String>[];
    final evs = <double?>[];
    for (final f in (m['frames'] as List? ?? const [])) {
      final fm = (f as Map).cast<String, dynamic>();
      final id = idByPath[fm['path'] as String?];
      if (id == null) continue;
      ids.add(id);
      evs.add((fm['evStep'] as num?)?.toDouble());
    }
    if (ids.length < 2) continue;
    if (kind == 'panorama' && ids.length == 2 && conf < 0.75) continue;
    if (conf < 0.4) continue;
    var start = 0;
    for (final id in ids) {
      final t = timeById[id] ?? 0;
      if (start == 0 || (t != 0 && t < start)) start = t;
    }
    out.add(DetectedGroup(
        kind: kind,
        confidence: conf,
        photoIds: ids,
        evSteps: evs,
        startMs: start,
        reason: (m['reason'] as String?) ?? ''));
  }
  return out;
}

/// Writes group membership onto the photos (corner badges of the grid): EV for brackets, n/N for panoramas.
void applyGroupBadges(Iterable<DetectedGroup> groups, Photo? Function(String id) byId) {
  for (final g in groups) {
    for (var i = 0; i < g.photoIds.length; i++) {
      final ph = byId(g.photoIds[i]);
      if (ph == null || ph.kind == PhotoKind.hdr || ph.kind == PhotoKind.pano || ph.kind == PhotoKind.live) continue;
      if (g.isBracket) {
        ph.kind = PhotoKind.bracket;
        final e = g.evSteps[i];
        ph.badge = e == null ? 'EV' : '${evNumber(e)} EV';
      } else {
        ph.kind = PhotoKind.frame;
        ph.badge = '${i + 1}/${g.length}';
      }
    }
  }
}
