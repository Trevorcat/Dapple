import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';
import 'dart:ui' as ui;

/// A cheap plausibility check before an HDR merge or a panorama stitch (the picked frames may be anything: panorama tiles picked for
/// HDR, one framing picked for a panorama). Works on tiny previews, so it costs a few milliseconds per pair and never touches the
/// engine. It only says "this does not look like what you asked for"; an inconclusive frame (no preview, a flat picture) never blocks.

/// Longest edge of a probe.
const int kProbeEdge = 64;

/// One small grey preview (row-major, 0..1) and, when known, the frame's exposure in EV (EXIF or the group's badge).
class FrameProbe {
  FrameProbe(this.w, this.h, this.luma, {this.ev}) : assert(luma.length == w * h);
  final int w, h;
  final Float32List luma;
  final double? ev;

  /// Mean brightness in stops (log2 of the mean linear value): a measured exposure when no EV is known.
  double get meanStops {
    var s = 0.0;
    for (final v in luma) {
      s += math.pow(v, 2.2);
    }
    return math.log(math.max(1e-4, s / luma.length)) / math.ln2;
  }
}

/// RGBA bytes (already scaled to at most [kProbeEdge]) to a probe.
FrameProbe probeFromRgba(Uint8List rgba, int w, int h, {double? ev}) {
  final l = Float32List(w * h);
  for (var i = 0; i < w * h; i++) {
    l[i] = (0.2126 * rgba[i * 4] + 0.7152 * rgba[i * 4 + 1] + 0.0722 * rgba[i * 4 + 2]) / 255;
  }
  return FrameProbe(w, h, l, ev: ev);
}

/// Decodes [path] at probe size; null when it cannot be read (the check then has nothing to say about that frame).
Future<FrameProbe?> loadProbe(String path, {double? ev}) async {
  try {
    final f = File(path);
    if (!await f.exists()) return null;
    final codec = await ui.instantiateImageCodec(await f.readAsBytes(), targetWidth: kProbeEdge);
    final fr = await codec.getNextFrame();
    final img = fr.image;
    final w = img.width, h = img.height;
    final bd = await img.toByteData(format: ui.ImageByteFormat.rawRgba);
    img.dispose();
    codec.dispose();
    if (bd == null) return null;
    return probeFromRgba(bd.buffer.asUint8List(), w, h, ev: ev);
  } catch (_) {
    return null;
  }
}

enum MergeProblem { none, notSameScene, sameExposure, noOverlap, sameFraming }

class MergeVerdict {
  const MergeVerdict(this.problem, [this.message]);
  static const ok = MergeVerdict(MergeProblem.none);
  final MergeProblem problem;

  /// Plain words for the user (guidelines 13: what is wrong, no cause guessing).
  final String? message;
  bool get isOk => problem == MergeProblem.none;
}

const Map<MergeProblem, String> kMergeProblemText = {
  MergeProblem.notSameScene: '这几张不像同一个场景',
  MergeProblem.sameExposure: '这几张的曝光几乎一样',
  MergeProblem.noOverlap: '有几张和相邻的照片没有重叠',
  MergeProblem.sameFraming: '这几张的取景几乎一样',
};

MergeVerdict _problem(MergeProblem p) => MergeVerdict(p, kMergeProblemText[p]);

// ---------------------------------------------------------------------------------------------------- numerics

/// Local detail: the picture minus its box blur. Overall tone and gradients (a bright sky over a dark ground, every exposure of a
/// bracket) drop out; edges and texture, which identify a scene, stay.
Float32List _detail(FrameProbe p) {
  final w = p.w, h = p.h;
  final r = math.max(2, math.min(w, h) ~/ 12);
  final integ = Float64List((w + 1) * (h + 1));
  for (var y = 0; y < h; y++) {
    var row = 0.0;
    for (var x = 0; x < w; x++) {
      row += p.luma[y * w + x];
      integ[(y + 1) * (w + 1) + x + 1] = integ[y * (w + 1) + x + 1] + row;
    }
  }
  final out = Float32List(w * h);
  for (var y = 0; y < h; y++) {
    final y0 = math.max(0, y - r), y1 = math.min(h, y + r + 1);
    for (var x = 0; x < w; x++) {
      final x0 = math.max(0, x - r), x1 = math.min(w, x + r + 1);
      final sum = integ[y1 * (w + 1) + x1] - integ[y0 * (w + 1) + x1] - integ[y1 * (w + 1) + x0] + integ[y0 * (w + 1) + x0];
      out[y * w + x] = p.luma[y * w + x] - sum / ((x1 - x0) * (y1 - y0));
    }
  }
  return out;
}

/// Normalised cross-correlation of [a] and [b] with [b] moved by (dx, dy), over the overlap only. Null when the overlap is smaller than
/// [minPixels] or one side has no detail there.
double? _ncc(Float32List a, Float32List b, int w, int h, int dx, int dy, int minPixels) {
  final x0 = math.max(0, dx), x1 = math.min(w, w + dx);
  final y0 = math.max(0, dy), y1 = math.min(h, h + dy);
  if (x1 <= x0 || y1 <= y0 || (x1 - x0) * (y1 - y0) < minPixels) return null;
  var sa = 0.0, sb = 0.0, saa = 0.0, sbb = 0.0, sab = 0.0;
  var n = 0;
  for (var y = y0; y < y1; y++) {
    final ra = y * w, rb = (y - dy) * w - dx;
    for (var x = x0; x < x1; x++) {
      final va = a[ra + x], vb = b[rb + x];
      sa += va;
      sb += vb;
      saa += va * va;
      sbb += vb * vb;
      sab += va * vb;
      n++;
    }
  }
  final va = saa - sa * sa / n, vb = sbb - sb * sb / n;
  // "no detail": the picture is (nearly) flat here, the score would be noise
  if (va < n * 1e-5 || vb < n * 1e-5) return null;
  return (sab - sa * sb / n) / math.sqrt(va * vb);
}

class _Best {
  _Best(this.score, this.dx, this.dy);
  final double score;
  final int dx, dy;
}

/// Best correlation of two probes over shifts in [rx] x [ry]; null when nothing could be compared.
_Best? _align(FrameProbe a, FrameProbe b, int rx, int ry, double minOverlap) {
  final da = _detail(a), db = _detail(b);
  final minPx = (a.w * a.h * minOverlap).round();
  _Best? best;
  for (var dy = -ry; dy <= ry; dy++) {
    for (var dx = -rx; dx <= rx; dx++) {
      final s = _ncc(da, db, a.w, a.h, dx, dy, minPx);
      if (s != null && (best == null || s > best.score)) best = _Best(s, dx, dy);
    }
  }
  return best;
}

bool _sameSize(List<FrameProbe> f) => f.every((p) => p.w == f.first.w && p.h == f.first.h);

// ---------------------------------------------------------------------------------------------------- the checks

/// Whether [frames] (oldest first; null = no preview) look like one exposure bracket: the same scene (local detail matches the middle
/// frame within a hand-held shift) and a real spread of exposure (EV when known, else measured brightness).
MergeVerdict checkBracket(List<FrameProbe?> frames) {
  final f = [for (final p in frames) ?p];
  if (f.length < 2) return MergeVerdict.ok;
  if (!_sameSize(f)) return _problem(MergeProblem.notSameScene);
  final mid = f[f.length ~/ 2];
  final shift = math.max(2, mid.w ~/ 12);
  for (final p in f) {
    if (identical(p, mid)) continue;
    final b = _align(mid, p, shift, shift, 0.5);
    if (b != null && b.score < 0.35) return _problem(MergeProblem.notSameScene);
  }
  // spread: EV from EXIF / the detected group when every frame has one; else the measured brightness (a flat result is only reported
  // when it is practically identical: a preview that was already normalised must not raise a false alarm)
  final evs = [for (final p in f) p.ev];
  if (evs.every((e) => e != null)) {
    final v = evs.cast<double>();
    if (v.reduce(math.max) - v.reduce(math.min) < 0.3) return _problem(MergeProblem.sameExposure);
  } else {
    final m = [for (final p in f) p.meanStops];
    if (m.reduce(math.max) - m.reduce(math.min) < 0.1) return _problem(MergeProblem.sameExposure);
  }
  return MergeVerdict.ok;
}

/// Whether [frames] (shooting order) look like a panorama: every neighbour pair overlaps (a shifted match that correlates) and the
/// framing really moves across the set.
MergeVerdict checkPanorama(List<FrameProbe?> frames) {
  final f = [for (final p in frames) ?p];
  if (f.length < 2) return MergeVerdict.ok;
  if (!_sameSize(f)) return _problem(MergeProblem.noOverlap);
  final w = f.first.w, h = f.first.h;
  var moved = false, compared = 0;
  for (var i = 0; i + 1 < f.length; i++) {
    // sideways (small vertical drift) and up-down (small sideways drift), at least 20 % of the picture in common
    final side = _align(f[i], f[i + 1], (w * 0.8).round(), math.max(2, h ~/ 8), 0.2);
    final up = _align(f[i], f[i + 1], math.max(2, w ~/ 8), (h * 0.8).round(), 0.2);
    _Best? best;
    for (final c in [side, up]) {
      if (c != null && (best == null || c.score > best.score)) best = c;
    }
    if (best == null) continue; // flat pictures: nothing to say
    compared++;
    if (best.score < 0.4) return _problem(MergeProblem.noOverlap);
    if (best.dx.abs() > w * 0.05 || best.dy.abs() > h * 0.05) moved = true;
  }
  if (compared > 0 && !moved) return _problem(MergeProblem.sameFraming);
  return MergeVerdict.ok;
}

/// Runs the check for [kind] ('hdr' | 'pano').
MergeVerdict checkFrames(String kind, List<FrameProbe?> frames) => kind == 'pano' ? checkPanorama(frames) : checkBracket(frames);
