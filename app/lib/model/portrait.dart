// 人像 (docs/design/local-adjustments.md section 9.1): one-tap retouch chips on the engine's portrait ops (docs/engine/pipeline-architecture.md
// section 43). A chip = one or two ordinary local areas with stable ids ("pt-skin" ...), a mask built from the AI planes of the faces it
// applies to, and a recipe: ONE strength (the strip, 0..1, 0.5 = the recommended default) scales every op of the chip between 0, the
// default and the engine's recommended maximum. Pure: no Flutter, no engine.
import 'dart:math' as math;
import 'dart:typed_data';

import 'local_adjust.dart';

enum PortraitChip { skin, undereye, shine, teeth, eyes, lips, light }

/// One op of a chip's recipe: value at the strip's middle ([def]) and top ([max]); 0 at the bottom.
class PortraitOp {
  const PortraitOp(this.key, this.def, this.max);
  final String key;
  final double def, max;

  double at(double a) => a <= 0.5 ? def * a * 2 : def + (max - def) * (a * 2 - 1);
}

/// One area a chip makes.
class PortraitAreaSpec {
  const PortraitAreaSpec(this.id, this.ops, this.plane);
  final String id;
  final List<PortraitOp> ops;

  /// The per-face plane key (`undereye` for the planes `fN:undereye`); null = the skin mask.
  final String? plane;
}

class PortraitSpec {
  const PortraitSpec(this.chip, this.label, this.areas, {this.onByDefault = true});
  final PortraitChip chip;
  final String label;
  final List<PortraitAreaSpec> areas;
  final bool onByDefault;
}

/// The recipes: defaults and maxima of section 43.8 (mild preset; never the engine's 100 where the evidence says it looks made-up).
const List<PortraitSpec> portraitSpecs = [
  PortraitSpec(PortraitChip.skin, '肤质', [
    PortraitAreaSpec('pt-skin', [
      PortraitOp('smooth', 30, 100),
      PortraitOp('even', 25, 60),
      PortraitOp('redness', 30, 50),
      PortraitOp('heal', 50, 100),
      PortraitOp('texture', 0, 20),
    ], null),
  ]),
  PortraitSpec(PortraitChip.undereye, '眼下', [
    PortraitAreaSpec('pt-undereye', [PortraitOp('undereye', 40, 80)], 'undereye'),
  ]),
  PortraitSpec(PortraitChip.shine, '油光', [
    PortraitAreaSpec('pt-shine', [PortraitOp('shine', 30, 80)], null),
  ]),
  PortraitSpec(PortraitChip.teeth, '牙齿', [
    PortraitAreaSpec('pt-teeth', [PortraitOp('whiten', 30, 60)], 'teeth'),
  ]),
  // two areas: whitening belongs on the whites, clarity on the iris (whiten would grey a brown iris)
  PortraitSpec(PortraitChip.eyes, '眼睛', [
    PortraitAreaSpec('pt-eyewhite', [PortraitOp('whiten', 30, 60)], 'sclera'),
    PortraitAreaSpec('pt-iris', [PortraitOp('clarity', 25, 50), PortraitOp('saturation', 10, 25)], 'iris'),
  ]),
  PortraitSpec(PortraitChip.lips, '嘴唇', [
    PortraitAreaSpec('pt-lips', [PortraitOp('saturation', 10, 30)], 'lips'),
  ]),
  PortraitSpec(PortraitChip.light, '补光', [
    PortraitAreaSpec('pt-light', [PortraitOp('facelight', 30, 60)], 'face'),
  ], onByDefault: false),
];

PortraitSpec specOf(PortraitChip c) => portraitSpecs.firstWhere((s) => s.chip == c);

/// The order the default set is filled in when even the largest face's chips do not fit next to the other areas: the shared skin first.
const List<PortraitChip> kPortraitPriority = [
  PortraitChip.skin,
  PortraitChip.shine,
  PortraitChip.lips,
  PortraitChip.teeth,
  PortraitChip.undereye,
  PortraitChip.eyes,
  PortraitChip.light,
];

/// The toast when a group photo's default set does not cover every face (guidelines 13.1: the result, then the action; no sentence):
/// what was done, and where the others are added (the face row at the top of the 人像 panel).
String groupToast(int done) => '${done <= 1 ? '已修最大的脸' : '已修 $done 张脸'} · 点面板里的脸加其他人';
const String kGroupMainOnly = '已修最大的脸 · 点面板里的脸加其他人';

/// The area name that marks a portrait area: `portrait:` + the area id.
String portraitName(String areaId) => 'portrait:$areaId';

/// The chip and area spec of a stored area (null for an ordinary area).
(PortraitSpec, PortraitAreaSpec)? portraitOf(LocalAdjustment a) {
  if (!a.name.startsWith('portrait:')) return null;
  final id = a.name.substring(9);
  for (final s in portraitSpecs) {
    for (final ar in s.areas) {
      if (ar.id == id) return (s, ar);
    }
  }
  return null;
}

/// The recipe's ops at strip position [a] (JSON units), clamped to the engine's ranges.
Map<String, double> portraitRecipe(PortraitAreaSpec ar, double a) => {
      for (final o in ar.ops) o.key: o.at(a.clamp(0.0, 1.0)).clamp(localOpByKey[o.key]!.min, localOpByKey[o.key]!.max).toDouble(),
    };

/// The largest value an op of [ar] may take in the itemised view (the recipe's maximum; ops outside the recipe keep the engine range).
double portraitMax(PortraitAreaSpec ar, String key) {
  for (final o in ar.ops) {
    if (o.key == key) return o.max;
  }
  return localOpByKey[key]?.max ?? 100;
}

// ------------------------------------------------------------------------------------------------ what the analysis found

/// One face of the analysis: index N (1-based, largest first), which feature planes exist, its scale (iod / photo long edge, 0 unknown).
class PortraitFace {
  const PortraitFace(this.n, this.keys, this.iod);
  final int n;

  /// Feature keys present for this face: face, eyes, iris, sclera, lips, brows, teeth, undereye.
  final Set<String> keys;
  final double iod;

  /// A frontal face has the eye features (the mesh rejects turned heads for them).
  bool get frontal => keys.contains('eyes') || keys.contains('sclera');
}

/// The faces of the analysis from its plane keys ("f1:lips" ...).
List<PortraitFace> facesOf(Iterable<String> planeKeys, {Map<int, double> iods = const {}}) {
  final by = <int, Set<String>>{};
  final re = RegExp(r'^f(\d+):(\w+)$');
  for (final k in planeKeys) {
    final m = re.firstMatch(k);
    if (m == null) continue;
    by.putIfAbsent(int.parse(m.group(1)!), () => {}).add(m.group(2)!);
  }
  final ns = by.keys.toList()..sort();
  return [for (final n in ns) PortraitFace(n, by[n]!, iods[n] ?? 0)];
}

/// The chips that can apply: skin and shine need the skin plane, the others a feature plane of at least one face.
List<PortraitChip> availableChips(Set<String> planeKeys, List<PortraitFace> faces) => [
      for (final s in portraitSpecs)
        if (s.areas.every((ar) => ar.plane == null ? planeKeys.contains('faceskin') : faces.any((f) => f.keys.contains(ar.plane))))
          s.chip,
    ];

/// The mask of [ar] for the faces [sel] (face numbers; the chip applies to these). [planeId] maps a key to its plane id.
///
/// Skin (and shine): face skin + body skin, minus each face's lips, teeth and open-mouth interior (`mouth`: shine reduction would grey the
/// teeth and the inside of the mouth) and,
/// with room, eyes and brows; faces not selected are taken out with their face oval. At most 8 parts (the engine's cap), distinct planes
/// are what count against the edit's 12.
List<MaskComponent> portraitMask(PortraitAreaSpec ar, List<PortraitFace> faces, Set<int> sel, String? Function(String key) planeId) {
  MaskComponent r(String id, MaskOp op) => MaskComponent(kind: MaskKindId.raster, op: op, ref: id, refine: 50);
  final out = <MaskComponent>[];
  void add(String key, MaskOp op) {
    final id = planeId(key);
    if (id != null && out.length < kMaxMaskComponents) out.add(r(id, out.isEmpty ? MaskOp.add : op));
  }

  if (ar.plane != null) {
    for (final f in faces) {
      if (sel.contains(f.n) && f.keys.contains(ar.plane)) add('f${f.n}:${ar.plane}', MaskOp.add);
    }
    return out;
  }
  add('faceskin', MaskOp.add);
  if (out.isEmpty) return out;
  add('bodyskin', MaskOp.add);
  final inSel = [for (final f in faces) if (sel.contains(f.n)) f];
  // the mouth first (required: lips, teeth and the open-mouth interior `mouth`), then the eyes and brows while the 8 parts last
  for (final f in inSel) {
    for (final k in const ['lips', 'teeth', 'mouth']) {
      if (f.keys.contains(k)) add('f${f.n}:$k', MaskOp.subtract);
    }
  }
  // eyes and brows only on a one-face photo: with several faces the planes go to the mouths (the 12-plane budget)
  if (faces.length == 1 && inSel.length == 1) {
    for (final k in const ['eyes', 'brows']) {
      if (inSel.first.keys.contains(k)) add('f${inSel.first.n}:$k', MaskOp.subtract);
    }
  }
  for (final f in faces) {
    if (!sel.contains(f.n) && f.keys.contains('face')) add('f${f.n}:face', MaskOp.subtract);
  }
  return out;
}

/// The face scale written to `retouch.face`: the LARGEST face's iod of the frame (the engine retouches a frame at one scale, the largest
/// face's; docs/engine/pipeline-architecture.md section 43). [sel] is not used for the scale (kept for the call sites).
double portraitFaceScale(List<PortraitFace> faces, [Set<int>? sel]) {
  final v = [for (final f in faces) if (f.iod > 0) f.iod];
  return v.isEmpty ? 0 : v.reduce(math.max);
}

/// The faces an area applies to, read back from its mask (per-face planes added, or for skin: every face not taken out).
Set<int> facesOfMask(LocalAdjustment a, List<PortraitFace> faces) {
  final re = RegExp(r':f(\d+):(\w+)$');
  final added = <int>{}, removedOval = <int>{};
  var skin = false;
  for (final m in a.mask) {
    if (m.ref.endsWith(':faceskin')) skin = true;
    final x = re.firstMatch(m.ref);
    if (x == null) continue;
    final n = int.parse(x.group(1)!);
    if (m.op == MaskOp.add) added.add(n);
    if (m.op == MaskOp.subtract && x.group(2) == 'face') removedOval.add(n);
  }
  if (skin) return {for (final f in faces) if (!removedOval.contains(f.n)) f.n};
  return added;
}

// ------------------------------------------------------------------------------------------------ the face scale from a plane

/// The inter-ocular distance (fraction of the long edge) from a gray RGBA preview of an eyes / iris plane: the distance between the centroids
/// of the left and right halves of the mask's extent. 0 when the plane holds no two eyes.
double iodFromPlane(Uint8List rgba, int w, int h, {int threshold = 96}) {
  var x0 = w, x1 = -1;
  for (var y = 0; y < h; y++) {
    for (var x = 0; x < w; x++) {
      if (rgba[(y * w + x) * 4] >= threshold) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
      }
    }
  }
  if (x1 <= x0) return 0;
  // two eyes = two column runs with an empty gap between them; one blob (one eye visible) is no distance
  final cols = List<bool>.filled(w, false);
  for (var y = 0; y < h; y++) {
    for (var x = x0; x <= x1; x++) {
      if (rgba[(y * w + x) * 4] >= threshold) cols[x] = true;
    }
  }
  var bestLen = 0, bestAt = -1, run = 0;
  for (var x = x0; x <= x1; x++) {
    if (!cols[x]) {
      run++;
      if (run > bestLen) {
        bestLen = run;
        bestAt = x - run + 1;
      }
    } else {
      run = 0;
    }
  }
  if (bestLen < 2) return 0;
  final mid = bestAt + bestLen / 2;
  double sx0 = 0, sy0 = 0, n0 = 0, sx1 = 0, sy1 = 0, n1 = 0;
  for (var y = 0; y < h; y++) {
    for (var x = 0; x < w; x++) {
      final v = rgba[(y * w + x) * 4];
      if (v < threshold) continue;
      if (x < mid) {
        sx0 += x * v;
        sy0 += y * v;
        n0 += v;
      } else {
        sx1 += x * v;
        sy1 += y * v;
        n1 += v;
      }
    }
  }
  if (n0 == 0 || n1 == 0) return 0;
  final d = math.sqrt(math.pow(sx1 / n1 - sx0 / n0, 2) + math.pow(sy1 / n1 - sy0 / n0, 2));
  return d / math.max(w, h);
}

/// A rougher scale from the face oval: about 0.42 x its width (a frontal face's iod is ~0.42 of the oval's width).
double iodFromFaceOval(Uint8List rgba, int w, int h, {int threshold = 96}) {
  var x0 = w, x1 = -1;
  for (var i = 0; i < w * h; i++) {
    if (rgba[i * 4] >= threshold) {
      final x = i % w;
      if (x < x0) x0 = x;
      if (x > x1) x1 = x;
    }
  }
  return x1 <= x0 ? 0 : 0.42 * (x1 - x0 + 1) / math.max(w, h);
}

/// Radius (long-edge units) of a manual heal spot at size [i] (0..3) for a face of scale [iod]: a blemish is 3 - 8 % of the iod.
double healRadius(double iod, int i) {
  const f = [0.025, 0.04, 0.06, 0.09];
  final s = iod > 0 ? iod : 0.06;
  return (s * f[i.clamp(0, 3)]).clamp(0.0005, 0.02).toDouble();
}
