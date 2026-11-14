// 形体 (docs/design/local-adjustments.md section 9.2): portrait reshaping on the engine's warp stage (docs/engine/pipeline-architecture.md
// section 45). The edit's `reshape` object: per face the 54 anchor points the analysis found (`face_list[i].shape`) and the operator
// strengths, per body the 33 pose points, the manual push strokes, and the person plane that protects the background. Pure: no Flutter,
// no engine.
import 'dart:math' as math;

enum ReshapeOp { slim, small, chin, eyes, nose, waist, arms, legs, longLegs, head }

/// One operator: its tile word (the plain words of the mainstream apps, docs/research/portrait-reshape.md 1), its engine key and group.
class ReshapeSpec {
  const ReshapeSpec(this.op, this.label, this.key, {this.body = false, this.min = 0});
  final ReshapeOp op;
  final String label;
  final String key;
  final bool body;

  /// The itemised range's bottom (下巴: shorter, -100); the strip runs 0 .. 100.
  final double min;
}

const List<ReshapeSpec> reshapeSpecs = [
  ReshapeSpec(ReshapeOp.slim, '瘦脸', 'slim'),
  ReshapeSpec(ReshapeOp.small, '小脸', 'small'),
  ReshapeSpec(ReshapeOp.chin, '下巴', 'chin', min: -100),
  ReshapeSpec(ReshapeOp.eyes, '大眼', 'eyes'),
  ReshapeSpec(ReshapeOp.nose, '瘦鼻', 'nose'),
  ReshapeSpec(ReshapeOp.waist, '瘦腰', 'waist', body: true),
  ReshapeSpec(ReshapeOp.arms, '瘦手臂', 'arms', body: true),
  ReshapeSpec(ReshapeOp.legs, '瘦腿', 'legs', body: true),
  ReshapeSpec(ReshapeOp.longLegs, '长腿', 'longlegs', body: true),
  ReshapeSpec(ReshapeOp.head, '小头', 'head', body: true),
];

ReshapeSpec reshapeSpecOf(ReshapeOp o) => reshapeSpecs.firstWhere((s) => s.op == o);

/// The strip: 0 at the bottom, the recommended amount in the middle (what a first tap sets), the engine's 100 at the top (the believable
/// maximum on the test set, section 45).
const double kReshapeDefault = 50;
double reshapeFromStrip(double a) => (a.clamp(0.0, 1.0) * 100).toDouble();
double reshapeToStrip(double v) => (v / 100).clamp(0.0, 1.0).toDouble();

/// The manual tool's brush sizes (radius, units of the photo's long edge).
const List<double> kPushRadius = [0.02, 0.04, 0.07, 0.11];

class ReshapeFace {
  const ReshapeFace({required this.index, required this.pts, this.v = const {}});
  final int index;
  final List<double> pts; // 108 numbers
  final Map<String, double> v; // op key -> strength

  ReshapeFace withValue(String key, double x) {
    final m = Map<String, double>.of(v);
    if (x == 0) {
      m.remove(key);
    } else {
      m[key] = x;
    }
    return ReshapeFace(index: index, pts: pts, v: m);
  }

  Map<String, dynamic> toJson() => {'i': index, ...v, 'pts': pts};
  static ReshapeFace? fromJson(Object? j) {
    if (j is! Map) return null;
    final pts = [for (final x in (j['pts'] as List? ?? const [])) if (x is num) x.toDouble()];
    if (pts.length != 108) return null;
    return ReshapeFace(index: (j['i'] as num?)?.toInt() ?? 0, pts: pts, v: {
      for (final s in reshapeSpecs)
        if (!s.body && j[s.key] is num && (j[s.key] as num) != 0) s.key: (j[s.key] as num).toDouble(),
    });
  }
}

class ReshapeBody {
  const ReshapeBody({required this.pts, this.v = const {}});
  final List<double> pts; // 99 numbers
  final Map<String, double> v;

  ReshapeBody withValue(String key, double x) {
    final m = Map<String, double>.of(v);
    if (x == 0) {
      m.remove(key);
    } else {
      m[key] = x;
    }
    return ReshapeBody(pts: pts, v: m);
  }

  Map<String, dynamic> toJson() => {...v, 'pts': pts};
  static ReshapeBody? fromJson(Object? j) {
    if (j is! Map) return null;
    final pts = [for (final x in (j['pts'] as List? ?? const [])) if (x is num) x.toDouble()];
    if (pts.length != 99) return null;
    return ReshapeBody(pts: pts, v: {
      for (final s in reshapeSpecs)
        if (s.body && j[s.key] is num && (j[s.key] as num) != 0) s.key: (j[s.key] as num).toDouble(),
    });
  }
}

/// A manual stroke: 推 drags the picture under the brush, 恢复 (restore) brings it back.
class PushStroke {
  const PushStroke({required this.r, required this.pts, this.restore = false});
  final double r;
  final List<double> pts; // x, y pairs, photo-normalised (of the reshaped picture, before crop / rotate)
  final bool restore;

  PushStroke append(double x, double y) => PushStroke(r: r, pts: [...pts, x, y], restore: restore);
  Map<String, dynamic> toJson() => {'r': r, if (restore) 'restore': true, 'pts': pts};
  static PushStroke? fromJson(Object? j) {
    if (j is! Map) return null;
    final pts = [for (final x in (j['pts'] as List? ?? const [])) if (x is num) x.toDouble()];
    return PushStroke(r: (j['r'] as num?)?.toDouble() ?? 0.04, pts: pts, restore: j['restore'] == true);
  }
}

/// The most strokes an edit keeps (the engine's cap).
const int kMaxPushStrokes = 64;

class Reshape {
  const Reshape({this.matte, this.faces = const [], this.body, this.push = const []});
  final String? matte;
  final List<ReshapeFace> faces;
  final ReshapeBody? body;
  final List<PushStroke> push;

  static const empty = Reshape();

  bool get isEmpty => faces.isEmpty && body == null && push.isEmpty;

  /// Anything that moves a pixel.
  bool get active => faces.any((f) => f.v.isNotEmpty) || (body?.v.isNotEmpty ?? false) || push.isNotEmpty;

  /// Whether [op] is on for any face / the body.
  bool on(ReshapeOp op) {
    final s = reshapeSpecOf(op);
    return s.body ? (body?.v[s.key] ?? 0) != 0 : faces.any((f) => (f.v[s.key] ?? 0) != 0);
  }

  /// The value shown for [op]: the first face that has it (faces carry the same value unless itemised), or the body's.
  double value(ReshapeOp op) {
    final s = reshapeSpecOf(op);
    if (s.body) return body?.v[s.key] ?? 0;
    for (final f in faces) {
      final x = f.v[s.key];
      if (x != null && x != 0) return x;
    }
    return 0;
  }

  /// The faces (indices) [op] acts on.
  Set<int> facesOf(ReshapeOp op) {
    final k = reshapeSpecOf(op).key;
    return {for (final f in faces) if ((f.v[k] ?? 0) != 0) f.index};
  }

  /// [op] at [x] on the faces [sel] (other faces: off for this op), or on the body.
  Reshape withValue(ReshapeOp op, double x, {Set<int>? sel}) {
    final s = reshapeSpecOf(op);
    final v = x.clamp(s.min, 100.0).toDouble();
    if (s.body) return body == null ? this : copyWith(body: body!.withValue(s.key, v));
    return copyWith(faces: [
      for (final f in faces) f.withValue(s.key, sel == null || sel.contains(f.index) ? v : 0),
    ]);
  }

  Reshape copyWith({Object? matte = _keep, List<ReshapeFace>? faces, Object? body = _keep, List<PushStroke>? push}) => Reshape(
        matte: identical(matte, _keep) ? this.matte : matte as String?,
        faces: faces ?? this.faces,
        body: identical(body, _keep) ? this.body : body as ReshapeBody?,
        push: push ?? this.push,
      );
  static const Object _keep = Object();

  /// The analysis' anchors merged in (same face index keeps its values; new faces start off).
  Reshape withAnchors({required List<(int, List<double>)> faces, List<double>? pose, String? matte}) {
    final byIndex = {for (final f in this.faces) f.index: f};
    return Reshape(
      matte: matte ?? this.matte,
      faces: [
        for (final (n, pts) in faces)
          if (pts.length == 108) ReshapeFace(index: n, pts: pts, v: byIndex[n]?.v ?? const {}),
      ],
      body: pose != null && pose.length == 99 ? ReshapeBody(pts: pose, v: body?.v ?? const {}) : body,
      push: push,
    );
  }

  /// The engine's `reshape` object (null when there is nothing to keep).
  Map<String, dynamic>? toEngineJson() {
    if (isEmpty) return null;
    return {
      'v': 1,
      if (matte != null) 'matte': matte,
      if (faces.isNotEmpty) 'faces': [for (final f in faces) f.toJson()],
      if (body != null) 'bodies': [body!.toJson()],
      if (push.isNotEmpty) 'push': [for (final s in push) s.toJson()],
    };
  }

  /// The app's persisted form is the engine's (all fields are resolution independent).
  Map<String, dynamic>? toAppJson() => toEngineJson();

  static Reshape fromJson(Object? j) {
    if (j is! Map) return empty;
    final bodies = j['bodies'];
    return Reshape(
      matte: j['matte'] as String?,
      faces: [for (final f in (j['faces'] as List? ?? const [])) ?ReshapeFace.fromJson(f)],
      body: bodies is List && bodies.isNotEmpty ? ReshapeBody.fromJson(bodies.first) : null,
      push: [for (final s in (j['push'] as List? ?? const [])) ?PushStroke.fromJson(s)],
    );
  }

  bool same(Reshape o) => _eq(toEngineJson(), o.toEngineJson());
}

bool _eq(Object? a, Object? b) {
  if (a is Map && b is Map) {
    if (a.length != b.length) return false;
    for (final k in a.keys) {
      if (!b.containsKey(k) || !_eq(a[k], b[k])) return false;
    }
    return true;
  }
  if (a is List && b is List) {
    if (a.length != b.length) return false;
    for (var i = 0; i < a.length; i++) {
      if (!_eq(a[i], b[i])) return false;
    }
    return true;
  }
  return a == b;
}

/// The face operators that make sense for a face: all five on a frontal face; a turned head (|yaw| >= 28°) only 瘦脸 and 小脸 (the far
/// side's contour is hidden, the eye / nose anchors are unreliable).
List<ReshapeOp> faceOpsFor({required bool anyFrontal}) => anyFrontal
    ? const [ReshapeOp.slim, ReshapeOp.small, ReshapeOp.chin, ReshapeOp.eyes, ReshapeOp.nose]
    : const [ReshapeOp.slim, ReshapeOp.small];

/// Brush radius in photo long-edge units for size [i], scaled by the largest face (a head-shot gets a larger brush).
double pushRadius(int i, {double iod = 0}) {
  final r = kPushRadius[i.clamp(0, kPushRadius.length - 1)];
  return iod > 0 ? math.max(0.008, math.min(0.25, r * (iod / 0.06).clamp(0.5, 2.5))) : r;
}

/// Why a body operator cannot work on this photo: the short word under the greyed tile and the sentence of the toast a tap on it shows.
class BodyBlock {
  const BodyBlock(this.note, this.message);
  final String note, message;
}

/// What each body operator needs, from the pose of the main person (33 x (x, y, visibility), null = no pose found). The same rules as the engine's
/// operators (core/src/warp_field.cpp: a point counts at visibility >= 0.5): an operator whose landmarks are missing or doubtful is greyed here
/// instead of warping on guesses. [covered] = the person plane covers the posed body (a segmenter that missed the person, e.g. on a
/// black-and-white photo, leaves the protected operators without anything to confine them: the engine skips them, so they are greyed).
/// Null = available.
BodyBlock? bodyBlockFor(ReshapeOp op, List<double>? pose, {bool covered = true, double aspect = 1}) {
  const none = BodyBlock('没找到', '没找到身体，试试拍到半身或全身的照片');
  if (pose == null || pose.length != 99) return none;
  bool ok(int k) => pose[3 * k + 2] >= 0.5;
  bool all(List<int> ks) => ks.every(ok);
  bool anyPair(List<(int, int)> ps) => ps.any((p) => ok(p.$1) && ok(p.$2));
  const protectedOps = {ReshapeOp.waist, ReshapeOp.arms, ReshapeOp.legs, ReshapeOp.head};
  if (protectedOps.contains(op) && !covered) {
    return const BodyBlock('轮廓不清', '看不清人的轮廓，改动会带着背景一起变形，所以先关着');
  }
  switch (op) {
    case ReshapeOp.waist:
      if (!all([11, 12, 23, 24])) return const BodyBlock('没拍到腰', '要拍到肩和腰才能瘦腰');
    case ReshapeOp.arms:
      if (!anyPair([(11, 13), (13, 15), (12, 14), (14, 16)])) return const BodyBlock('没拍到手臂', '没看清手臂，不能瘦手臂');
    case ReshapeOp.legs:
      if (!anyPair([(23, 25), (25, 27), (24, 26), (26, 28)])) return const BodyBlock('没拍到腿', '没看清腿，不能瘦腿');
    case ReshapeOp.longLegs:
      if (!all([23, 24])) return const BodyBlock('没拍到腿', '要拍到腰和腿才能拉长腿');
      var foot = -1.0;
      for (var k = 27; k <= 32; k++) {
        if (ok(k)) foot = math.max(foot, pose[3 * k + 1]);
      }
      if (foot < 0) return const BodyBlock('没拍到脚', '要拍到脚才能拉长腿');
      final hipY = (pose[3 * 23 + 1] + pose[3 * 24 + 1]) / 2;
      if (all([11, 12])) {
        final dx = ((pose[3 * 23] + pose[3 * 24]) / 2 - (pose[3 * 11] + pose[3 * 12]) / 2) * aspect;  // aspect = width / height: the engine measures in isotropic units
        final dy = hipY - (pose[3 * 11 + 1] + pose[3 * 12 + 1]) / 2;
        final l = math.sqrt(dx * dx + dy * dy);
        if (l > 0 && dy / l < math.cos(30 * math.pi / 180)) return const BodyBlock('要站直', '人要站直才能拉长腿');
      }
      if (foot + 0.03 > 0.99) return const BodyBlock('脚在边上', '脚太靠近照片边缘，没有地方拉长');
      if (math.min(foot + 0.03, 1.0) - hipY < 0.05) return const BodyBlock('腿太短', '腿在画面里太小，不能拉长腿');
    case ReshapeOp.head:
      if (!all([7, 8, 11, 12, 23, 24])) return const BodyBlock('没拍到腰', '要拍到头、肩和腰才能小头');
    default:
      break;
  }
  return null;
}
