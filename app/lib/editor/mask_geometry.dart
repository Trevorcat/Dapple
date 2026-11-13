// Coordinates of the area handles (docs/design/local-adjustments.md section 8). Three spaces:
//   frame    the edit's output picture as laid out by the ZoomView, in dp at fit zoom (0..frame size); the zoom / pan transform is
//            Flutter's, so a handle layer inside the ZoomView works in this space and stays correct under zoom and pan;
//   photo    the engine's mask space: (u, v) in 0..1 of the decoded picture before crop / rotate / straighten / flip;
//   LE       photo units of the long edge (x = u * sx, y = v * sy; sx = W / L, sy = H / L): radii and brush sizes live here, so a
//            circle stays a circle.
// Frame <-> photo is affine (rotate 90, straighten, flip, crop): [FrameMap] holds it, built from three probes of
// dapple_session_map_point per geometry (native) or composed in Dart (fallback, tests). Pure; no Flutter widgets.
import 'dart:math' as math;
import 'dart:ui' show Offset, Size;

import '../model/local_adjust.dart';

/// Photo (u, v) = o + M * f, with f the frame position normalised to 0..1.
class FrameMap {
  const FrameMap({required this.ou, required this.ov, required this.a, required this.b, required this.c, required this.d, required this.photoAspect});

  /// Frame == photo (no geometry).
  factory FrameMap.identity(double photoAspect) => FrameMap(ou: 0, ov: 0, a: 1, b: 0, c: 0, d: 1, photoAspect: photoAspect);

  /// From the native probes: (u0, v0) at frame pixel (0, 0), (ux, vx) at (L, 0), (uy, vy) at (0, L), for a frame of [w] x [h] pixels.
  factory FrameMap.fromProbes({
    required double u0,
    required double v0,
    required double ux,
    required double vx,
    required double uy,
    required double vy,
    required double probe,
    required double w,
    required double h,
    required double photoAspect,
  }) {
    // per frame pixel
    final pa = (ux - u0) / probe, pc = (vx - v0) / probe, pb = (uy - u0) / probe, pd = (vy - v0) / probe;
    return FrameMap(ou: u0, ov: v0, a: pa * w, b: pb * h, c: pc * w, d: pd * h, photoAspect: photoAspect);
  }

  /// The Dart composition of the engine's geometry for tests and the no-engine fallback: the photo turned by [rot] (0, 90, 180, 270,
  /// clockwise), mirrored when [flip], then the crop rectangle [crop] (normalised in the turned picture). Straightening is not
  /// modelled here (the native probes carry it).
  factory FrameMap.compose({required double photoAspect, int rot = 0, bool flip = false, Rect01 crop = Rect01.full}) {
    // frame f -> turned picture t = crop.origin + f * crop.size -> unflipped -> photo
    Offset toPhoto(Offset f) {
      var t = Offset(crop.l + f.dx * crop.w, crop.t + f.dy * crop.h);
      if (flip) t = Offset(1 - t.dx, t.dy);
      switch (((rot % 360) + 360) % 360) {
        case 90: // turned clockwise: t = (1 - v, u)
          return Offset(t.dy, 1 - t.dx);
        case 180:
          return Offset(1 - t.dx, 1 - t.dy);
        case 270:
          return Offset(1 - t.dy, t.dx);
        default:
          return t;
      }
    }

    final o = toPhoto(Offset.zero), x = toPhoto(const Offset(1, 0)), y = toPhoto(const Offset(0, 1));
    return FrameMap(ou: o.dx, ov: o.dy, a: x.dx - o.dx, b: y.dx - o.dx, c: x.dy - o.dy, d: y.dy - o.dy, photoAspect: photoAspect);
  }

  final double ou, ov, a, b, c, d;

  /// Width / height of the decoded photo.
  final double photoAspect;

  double get sx => photoAspect >= 1 ? 1 : photoAspect;
  double get sy => photoAspect >= 1 ? 1 / photoAspect : 1;

  /// Frame (0..1) -> photo (u, v).
  Offset toPhoto(Offset f) => Offset(ou + a * f.dx + b * f.dy, ov + c * f.dx + d * f.dy);

  /// Photo (u, v) -> frame (0..1).
  Offset toFrame(Offset uv) {
    final det = a * d - b * c;
    final du = uv.dx - ou, dv = uv.dy - ov;
    return Offset((d * du - b * dv) / det, (-c * du + a * dv) / det);
  }

  /// Photo (u, v) -> LE units, and back.
  Offset toLE(Offset uv) => Offset(uv.dx * sx, uv.dy * sy);
  Offset fromLE(Offset p) => Offset(p.dx / sx, p.dy / sy);

  @override
  bool operator ==(Object other) => other is FrameMap && other.ou == ou && other.ov == ov && other.a == a && other.b == b && other.c == c && other.d == d && other.photoAspect == photoAspect;

  @override
  int get hashCode => Object.hash(ou, ov, a, b, c, d, photoAspect);
}

/// A normalised rectangle (left, top, width, height in 0..1).
class Rect01 {
  const Rect01(this.l, this.t, this.w, this.h);
  static const full = Rect01(0, 0, 1, 1);
  final double l, t, w, h;
}

/// The handle space of one frame laid out at [size] dp: photo / LE points <-> local dp.
class HandleSpace {
  HandleSpace(this.map, this.size);
  final FrameMap map;
  final Size size;

  Offset local(Offset uv) {
    final f = map.toFrame(uv);
    return Offset(f.dx * size.width, f.dy * size.height);
  }

  Offset photo(Offset p) => map.toPhoto(Offset(p.dx / size.width, p.dy / size.height));

  Offset localLE(Offset le) => local(map.fromLE(le));
  Offset le(Offset p) => map.toLE(photo(p));

  /// dp per LE unit (the composite map is a similarity: rotation, flip and one scale).
  double get dpPerLE {
    final o = localLE(Offset.zero), x = localLE(const Offset(1, 0));
    return (x - o).distance;
  }

  /// True when the photo -> screen map mirrors (a flip): rotation senses reverse.
  bool get mirrored {
    final o = localLE(Offset.zero), x = localLE(const Offset(1, 0)), y = localLE(const Offset(0, 1));
    final ax = x - o, ay = y - o;
    return ax.dx * ay.dy - ax.dy * ay.dx < 0;
  }

  /// Direction (unit, local dp) of an LE direction given by the angle [deg] (clockwise, y down).
  Offset dirLocal(double deg) {
    final t = deg * math.pi / 180;
    final o = localLE(Offset.zero), p = localLE(Offset(math.cos(t), math.sin(t)));
    final v = p - o;
    return v / v.distance;
  }
}

// ------------------------------------------------------------------------------------------------ linear

/// A linear gradient on screen: full effect along the line through [p0], none along the line through [p1] (both perpendicular to
/// p0 -> p1); a rotate knob on the full line.
class LinearHandles {
  LinearHandles(HandleSpace s, MaskComponent m)
      : p0 = s.local(Offset(m.x0, m.y0)),
        p1 = s.local(Offset(m.x1, m.y1));
  final Offset p0, p1;

  Offset get axis {
    final d = p1 - p0;
    final n = d.distance;
    return n < 1e-6 ? const Offset(0, 1) : d / n;
  }

  /// Along the lines (perpendicular to the axis).
  Offset get along => Offset(-axis.dy, axis.dx);
  Offset get mid => (p0 + p1) / 2;

  /// Where the rotate knob sits ([knobDist] dp along the full line from the axis).
  Offset knob(double knobDist) => p0 + along * knobDist;

  static double distToLine(Offset p, Offset through, Offset dir) {
    final v = p - through;
    return (v.dx * dir.dy - v.dy * dir.dx).abs();
  }
}

/// A new / moved gradient from local dp points.
MaskComponent linearFrom(HandleSpace s, MaskComponent m, Offset p0, Offset p1) {
  final a = s.photo(p0), b = s.photo(p1);
  return m.copyWith(x0: a.dx, y0: a.dy, x1: b.dx, y1: b.dy);
}

/// [m] turned about the axis point of its full line, so that the knob (on the full line) stays under [finger].
MaskComponent linearRotated(HandleSpace s, MaskComponent m, Offset finger) {
  final h = LinearHandles(s, m);
  final len = (h.p1 - h.p0).distance;
  final toFinger = finger - h.p0;
  if (toFinger.distance < 4) return m;
  // the knob direction is `along` = the axis turned by +90 degrees; the axis is `along` turned back
  final al = toFinger / toFinger.distance;
  final ax = Offset(al.dy, -al.dx);
  return linearFrom(s, m, h.p0, h.p0 + ax * len);
}

// ------------------------------------------------------------------------------------------------ radial

/// A radial shape on screen. Outer ellipse = where the effect ends, inner ellipse = full effect (1 - feather of the outer).
class RadialHandles {
  RadialHandles(this.s, this.m)
      : centre = s.local(Offset(m.cx, m.cy)),
        k = s.dpPerLE,
        u = s.dirLocal(m.angle),
        v = s.dirLocal(m.angle + 90);
  final HandleSpace s;
  final MaskComponent m;
  final Offset centre;

  /// dp per LE unit.
  final double k;

  /// Local directions of the ellipse's own x and y axes.
  final Offset u, v;

  double get rxDp => m.rx * k;
  double get ryDp => m.ry * k;
  double get inner => 1 - m.feather / 100;

  /// A point of the outer (scale 1) or inner (scale [inner]) ellipse at parameter [t] (radians).
  Offset at(double t, {double scale = 1}) => centre + u * (math.cos(t) * rxDp * scale) + v * (math.sin(t) * ryDp * scale);

  /// The four size handles on the full-effect (inner) ellipse: +x, -x, +y, -y.
  List<Offset> sizeHandles() => [at(0, scale: inner), at(math.pi, scale: inner), at(math.pi / 2, scale: inner), at(-math.pi / 2, scale: inner)];

  /// The rotate knob, beyond the outer ellipse on the -y side.
  Offset knob(double gap) => centre - v * (ryDp + gap);

  /// Ellipse-normalised distance of [p] (1 = on the outer ellipse).
  double norm(Offset p) {
    final d = p - centre;
    final xr = (d.dx * u.dx + d.dy * u.dy) / math.max(1e-6, rxDp), yr = (d.dx * v.dx + d.dy * v.dy) / math.max(1e-6, ryDp);
    return math.sqrt(xr * xr + yr * yr);
  }

  /// Size handle [i] dragged to [p]: that axis' inner radius follows the finger, the feather fraction stays.
  MaskComponent sized(int i, Offset p) {
    final d = p - centre;
    final along = i < 2 ? (d.dx * u.dx + d.dy * u.dy).abs() : (d.dx * v.dx + d.dy * v.dy).abs();
    final outer = (along / math.max(0.05, inner)) / k;
    final r = outer.clamp(0.005, 4.0).toDouble();
    return i < 2 ? m.copyWith(rx: r) : m.copyWith(ry: r);
  }

  /// The outer ring dragged to [p]: the outer ellipse passes through the finger, the inner one stays where it is.
  MaskComponent feathered(Offset p) {
    final e = norm(p);
    final innerAbs = inner; // inner boundary in units of the current outer
    final ne = math.max(e, innerAbs + 0.01);
    final f = (1 - innerAbs / ne).clamp(0.0, 1.0);
    final rx = (m.rx * ne).clamp(0.005, 4.0).toDouble(), ry = (m.ry * ne).clamp(0.005, 4.0).toDouble();
    return m.copyWith(rx: rx, ry: ry, feather: (f * 100).roundToDouble());
  }

  /// Turned so that the knob points at [p].
  MaskComponent rotated(Offset p) {
    final d = p - centre;
    if (d.distance < 4) return m;
    // the knob sits on -v; the LE angle of -v is angle - 90 (mirrored pictures turn the other way)
    final o = s.localLE(Offset.zero);
    final le0 = s.le(o), le1 = s.le(o + d / d.distance);
    final dirDeg = math.atan2(le1.dy - le0.dy, le1.dx - le0.dx) * 180 / math.pi;
    var a = dirDeg + 90;
    a = ((a % 360) + 360) % 360;
    if (a > 180) a -= 360;
    return m.copyWith(angle: a.roundToDouble());
  }

  MaskComponent moved(Offset delta) {
    final c = s.photo(centre + delta);
    return m.copyWith(cx: c.dx, cy: c.dy);
  }
}

/// A new circle centred on [c] (local dp) with outer radius [rDp] dp.
MaskComponent radialFrom(HandleSpace s, MaskComponent m, Offset c, double rDp) {
  final uv = s.photo(c);
  final r = (rDp / s.dpPerLE).clamp(0.01, 4.0).toDouble();
  return m.copyWith(cx: uv.dx, cy: uv.dy, rx: r, ry: r);
}

// ------------------------------------------------------------------------------------------------ brush input

/// Pointer smoothing and spacing of one brush stroke (pressure is ignored). Feeds photo-space points; [take] hands over what
/// accumulated since the last call (one batch per frame).
class BrushInput {
  BrushInput({required this.space, required this.radiusLE, this.smoothing = 0.45});
  final HandleSpace space;

  /// Brush radius in LE units: the minimum spacing between kept points is a quarter of it.
  final double radiusLE;

  /// Exponential smoothing of the finger (0 = none).
  final double smoothing;

  Offset? _s; // smoothed local position
  Offset? _lastKeptLE;
  final List<double> _pending = [];
  int total = 0;

  /// Adds a raw local position; returns true when a point was kept.
  bool add(Offset local) {
    final s = _s == null ? local : Offset.lerp(_s!, local, 1 - smoothing)!;
    _s = s;
    final le = space.le(s);
    final last = _lastKeptLE;
    if (last != null && (le - last).distance < radiusLE * 0.25) return false;
    _lastKeptLE = le;
    final uv = space.photo(s);
    _pending..add(uv.dx)..add(uv.dy);
    total++;
    return true;
  }

  /// The last raw point (the stroke end is always kept so a short tap leaves a dot where the finger was).
  void finish(Offset local) {
    final uv = space.photo(local);
    final last = _lastKeptLE;
    final le = space.le(local);
    if (last != null && (le - last).distance < 1e-6) return;
    _lastKeptLE = le;
    _pending..add(uv.dx)..add(uv.dy);
    total++;
  }

  bool get hasPending => _pending.isNotEmpty;

  List<double> take() {
    final out = List<double>.of(_pending);
    _pending.clear();
    return out;
  }
}

/// [m] with [pts] (photo x, y pairs) appended to its last stroke, or as a new stroke when [newStroke].
MaskComponent brushAppend(MaskComponent m, List<double> pts, {required bool newStroke, double size = 0.05, bool erase = false, double hardness = 50}) {
  if (pts.isEmpty) return m;
  final strokes = List<BrushStroke>.of(m.strokes);
  if (newStroke || strokes.isEmpty) {
    strokes.add(BrushStroke(size: size, erase: erase, hardness: hardness).appended(pts));
  } else {
    strokes[strokes.length - 1] = strokes.last.appended(pts);
  }
  return m.copyWith(strokes: strokes);
}
