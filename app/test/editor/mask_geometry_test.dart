// Pure geometry of the area handles (lib/editor/mask_geometry.dart): the frame <-> photo map (composed and from native probes), the
// handle space, the linear / radial handle maths and the brush input / append helpers.
import 'dart:math' as math;
import 'dart:ui' show Offset, Size;

import 'package:dapple/editor/mask_geometry.dart';
import 'package:dapple/model/local_adjust.dart';
import 'package:flutter_test/flutter_test.dart';

const double photoAspect = 1.5;

/// The centred 1:1 crop of a 3:2 photo turned by [rot] (what the editor's fallback map composes).
Rect01 squareCrop(int rot) {
  final turned = (rot ~/ 90).isOdd ? 1 / photoAspect : photoAspect;
  const ar = 1.0;
  return ar < turned ? Rect01((1 - ar / turned) / 2, 0, ar / turned, 1) : Rect01(0, (1 - turned / ar) / 2, 1, turned / ar);
}

FrameMap mapOf(int rot, bool flip, {bool crop = true}) =>
    FrameMap.compose(photoAspect: photoAspect, rot: rot, flip: flip, crop: crop ? squareCrop(rot) : Rect01.full);

/// A handle space whose frame has the aspect the map implies (frame pixels are square), like LocalController.space.
HandleSpace spaceOf(FrameMap m, {double width = 1000}) {
  final o = m.toLE(m.toPhoto(Offset.zero));
  final fx = m.toLE(m.toPhoto(const Offset(1, 0))) - o;
  final fy = m.toLE(m.toPhoto(const Offset(0, 1))) - o;
  return HandleSpace(m, Size(width, width * fy.distance / fx.distance));
}

void expectOffset(Offset a, Offset b, double tol, {String? reason}) {
  expect((a - b).distance, lessThanOrEqualTo(tol), reason: '${reason ?? ''} $a vs $b');
}

double angleDiffDeg(double a, double b) {
  var d = (a - b) % 360;
  if (d > 180) d -= 360;
  if (d < -180) d += 360;
  return d.abs();
}

double dirDeg(Offset v) => math.atan2(v.dy, v.dx) * 180 / math.pi;

final combos = [
  for (final r in [0, 90, 180, 270])
    for (final f in [false, true]) (r, f),
];

void main() {
  group('FrameMap.compose', () {
    test('toFrame(toPhoto(f)) == f for every rotation x flip, with and without a centred crop', () {
      final rnd = math.Random(7);
      for (final (rot, flip) in combos) {
        for (final crop in [false, true]) {
          final m = mapOf(rot, flip, crop: crop);
          for (var i = 0; i < 40; i++) {
            final f = Offset(rnd.nextDouble(), rnd.nextDouble());
            final back = m.toFrame(m.toPhoto(f));
            expect((back.dx - f.dx).abs(), lessThan(1e-9), reason: 'rot $rot flip $flip crop $crop');
            expect((back.dy - f.dy).abs(), lessThan(1e-9), reason: 'rot $rot flip $flip crop $crop');
          }
        }
      }
    });

    test('rot 90 turns clockwise: frame top-left = photo bottom-left, top-right = top-left', () {
      final m = mapOf(90, false, crop: false);
      expectOffset(m.toPhoto(Offset.zero), const Offset(0, 1), 1e-12);
      expectOffset(m.toPhoto(const Offset(1, 0)), const Offset(0, 0), 1e-12);
      expectOffset(m.toPhoto(const Offset(1, 1)), const Offset(1, 0), 1e-12);
      expectOffset(m.toPhoto(const Offset(0, 1)), const Offset(1, 1), 1e-12);
    });

    test('rot 180, 270 and flip corners', () {
      final m180 = mapOf(180, false, crop: false);
      expectOffset(m180.toPhoto(Offset.zero), const Offset(1, 1), 1e-12);
      final m270 = mapOf(270, false, crop: false);
      expectOffset(m270.toPhoto(Offset.zero), const Offset(1, 0), 1e-12);
      expectOffset(m270.toPhoto(const Offset(1, 0)), const Offset(1, 1), 1e-12);
      final mf = mapOf(0, true, crop: false);
      expectOffset(mf.toPhoto(Offset.zero), const Offset(1, 0), 1e-12);
      expectOffset(mf.toPhoto(const Offset(1, 1)), const Offset(0, 1), 1e-12);
    });

    test('a centred crop maps the frame centre to the photo centre and the corners inside the photo', () {
      for (final (rot, flip) in combos) {
        final m = mapOf(rot, flip);
        expectOffset(m.toPhoto(const Offset(0.5, 0.5)), const Offset(0.5, 0.5), 1e-12, reason: 'rot $rot flip $flip');
        for (final c in const [Offset(0, 0), Offset(1, 0), Offset(0, 1), Offset(1, 1)]) {
          final p = m.toPhoto(c);
          expect(p.dx, inInclusiveRange(-1e-12, 1 + 1e-12));
          expect(p.dy, inInclusiveRange(-1e-12, 1 + 1e-12));
        }
      }
    });

    test('LE conversions are inverse and use the long edge', () {
      final m = FrameMap.identity(1.5);
      expect(m.sx, 1);
      expect(m.sy, closeTo(1 / 1.5, 1e-12));
      expectOffset(m.fromLE(m.toLE(const Offset(0.3, 0.8))), const Offset(0.3, 0.8), 1e-12);
      final p = FrameMap.identity(0.5);
      expect(p.sx, 0.5);
      expect(p.sy, 1);
    });
  });

  group('FrameMap.fromProbes', () {
    test('probes taken from a composed map rebuild the same map', () {
      final rnd = math.Random(3);
      const w = 1200.0, h = 1200.0, probe = 64.0;
      for (final (rot, flip) in combos) {
        final c = mapOf(rot, flip);
        Offset at(double px, double py) => c.toPhoto(Offset(px / w, py / h));
        final o = at(0, 0), x = at(probe, 0), y = at(0, probe);
        final m = FrameMap.fromProbes(u0: o.dx, v0: o.dy, ux: x.dx, vx: x.dy, uy: y.dx, vy: y.dy, probe: probe, w: w, h: h, photoAspect: photoAspect);
        for (var i = 0; i < 30; i++) {
          final f = Offset(rnd.nextDouble(), rnd.nextDouble());
          expectOffset(m.toPhoto(f), c.toPhoto(f), 1e-9, reason: 'rot $rot flip $flip');
          expectOffset(m.toFrame(m.toPhoto(f)), f, 1e-9);
        }
      }
    });

    test('a non-square frame is handled (px per axis differ)', () {
      const w = 900.0, h = 600.0, probe = 32.0;
      final c = mapOf(90, true, crop: false);
      Offset at(double px, double py) => c.toPhoto(Offset(px / w, py / h));
      final o = at(0, 0), x = at(probe, 0), y = at(0, probe);
      final m = FrameMap.fromProbes(u0: o.dx, v0: o.dy, ux: x.dx, vx: x.dy, uy: y.dx, vy: y.dy, probe: probe, w: w, h: h, photoAspect: photoAspect);
      expectOffset(m.toPhoto(const Offset(0.25, 0.75)), c.toPhoto(const Offset(0.25, 0.75)), 1e-9);
    });
  });

  group('HandleSpace', () {
    test('rot 90 + crop + flip: the scale is the same along both LE axes and a circle stays a circle', () {
      for (final (rot, flip) in combos) {
        final s = spaceOf(mapOf(rot, flip));
        final o = s.localLE(Offset.zero);
        final dx = (s.localLE(const Offset(1, 0)) - o).distance, dy = (s.localLE(const Offset(0, 1)) - o).distance;
        expect(dx, closeTo(dy, 1e-6), reason: 'rot $rot flip $flip');
        expect(s.dpPerLE, closeTo(dx, 1e-9));
        final m = MaskComponent(kind: MaskKindId.radial, cx: 0.5, cy: 0.5, rx: 0.2, ry: 0.2, feather: 40);
        final h = RadialHandles(s, m);
        final dists = [for (final q in h.sizeHandles()) (q - h.centre).distance];
        for (final d in dists) {
          expect(d, closeTo(dists.first, 1e-6), reason: 'rot $rot flip $flip');
        }
        expect(dists.first, closeTo(0.2 * (1 - 0.4) * s.dpPerLE, 1e-6));
      }
    });

    test('mirrored is true exactly when flip', () {
      for (final (rot, flip) in combos) {
        expect(spaceOf(mapOf(rot, flip)).mirrored, flip, reason: 'rot $rot flip $flip');
        expect(spaceOf(mapOf(rot, flip, crop: false)).mirrored, flip, reason: 'rot $rot flip $flip no crop');
      }
    });

    test('local <-> photo and local <-> LE round trip', () {
      for (final (rot, flip) in combos) {
        final s = spaceOf(mapOf(rot, flip));
        const p = Offset(123, 321);
        expectOffset(s.local(s.photo(p)), p, 1e-6);
        expectOffset(s.localLE(s.le(p)), p, 1e-6);
      }
    });
  });

  group('linear handles', () {
    test('linearFrom then LinearHandles gives back the same local points', () {
      for (final (rot, flip) in combos) {
        final s = spaceOf(mapOf(rot, flip));
        final m = linearFrom(s, const MaskComponent(kind: MaskKindId.linear), const Offset(200, 100), const Offset(260, 480));
        final h = LinearHandles(s, m);
        expectOffset(h.p0, const Offset(200, 100), 1e-6, reason: 'rot $rot flip $flip');
        expectOffset(h.p1, const Offset(260, 480), 1e-6, reason: 'rot $rot flip $flip');
      }
    });

    test('linearRotated: turns about the full line axis point, the knob direction (along) points at the finger, the length stays', () {
      for (final (rot, flip) in combos) {
        final s = spaceOf(mapOf(rot, flip));
        final m0 = linearFrom(s, const MaskComponent(kind: MaskKindId.linear), const Offset(300, 200), const Offset(300, 600));
        final h0 = LinearHandles(s, m0);
        const finger = Offset(520, 140);
        final h = LinearHandles(s, linearRotated(s, m0, finger));
        final want = (finger - h0.p0) / (finger - h0.p0).distance;
        expectOffset(h.along, want, 1e-6, reason: 'rot $rot flip $flip');
        expectOffset(h.p0, h0.p0, 1e-6);
        expect((h.p1 - h.p0).distance, closeTo((h0.p1 - h0.p0).distance, 1e-6));
      }
    });

    test('a finger on the first point leaves the line unchanged', () {
      final s = spaceOf(mapOf(0, false));
      final m0 = linearFrom(s, const MaskComponent(kind: MaskKindId.linear), const Offset(300, 200), const Offset(300, 600));
      expect(identical(linearRotated(s, m0, const Offset(301, 201)), m0), isTrue);
    });
  });

  group('radial handles', () {
    MaskComponent circle(HandleSpace s, {double rx = 0.2, double ry = 0.2, double angle = 0, double feather = 40}) =>
        MaskComponent(kind: MaskKindId.radial, cx: 0.45, cy: 0.55, rx: rx, ry: ry, angle: angle, feather: feather);

    test('rotated(p) puts the knob on the ray centre -> p, with and without flip', () {
      for (final flip in [false, true]) {
        for (final rot in [0, 90]) {
          final s = spaceOf(mapOf(rot, flip));
          final m = circle(s, rx: 0.25, ry: 0.15, angle: 20);
          final h0 = RadialHandles(s, m);
          for (final deg in [-170.0, -120.0, -60.0, -10.0, 15.0, 80.0, 135.0, 179.0]) {
            final t = deg * math.pi / 180;
            final p = h0.centre + Offset(math.cos(t), math.sin(t)) * 150;
            final h = RadialHandles(s, h0.rotated(p));
            final knob = h.knob(28);
            expect(angleDiffDeg(dirDeg(knob - h.centre), dirDeg(p - h.centre)), lessThanOrEqualTo(1), reason: 'rot $rot flip $flip at $deg');
          }
        }
      }
    });

    test('rotated: a finger on the centre changes nothing', () {
      final s = spaceOf(mapOf(0, false));
      final m = circle(s);
      final h = RadialHandles(s, m);
      expect(identical(h.rotated(h.centre + const Offset(1, 1)), m), isTrue);
    });

    test('sized(i, p): the inner ellipse passes through the finger along that axis', () {
      for (final (rot, flip) in combos) {
        final s = spaceOf(mapOf(rot, flip));
        final m = circle(s, rx: 0.2, ry: 0.12, angle: 30);
        final h = RadialHandles(s, m);
        for (var i = 0; i < 4; i++) {
          final dir = switch (i) { 0 => h.u, 1 => -h.u, 2 => h.v, _ => -h.v };
          final p = h.centre + dir * 90;
          final h2 = RadialHandles(s, h.sized(i, p));
          expectOffset(h2.sizeHandles()[i], p, 1e-3, reason: 'rot $rot flip $flip handle $i');
          // the other axis and the feather fraction stay
          if (i < 2) {
            expect(h2.m.ry, m.ry);
          } else {
            expect(h2.m.rx, m.rx);
          }
          expect(h2.m.feather, m.feather);
        }
      }
    });

    test('feathered(p): the inner ellipse stays within 1 % and the outer one passes through p', () {
      for (final (rot, flip) in combos) {
        final s = spaceOf(mapOf(rot, flip));
        final m = circle(s, rx: 0.2, ry: 0.12, angle: 30);
        final h = RadialHandles(s, m);
        final p = h.centre + h.u * (h.rxDp * 1.4);
        final m2 = h.feathered(p);
        final h2 = RadialHandles(s, m2);
        expect(m2.rx * (1 - m2.feather / 100), closeTo(m.rx * (1 - m.feather / 100), m.rx * 0.01), reason: 'rot $rot flip $flip');
        expect(m2.ry * (1 - m2.feather / 100), closeTo(m.ry * (1 - m.feather / 100), m.ry * 0.01));
        expect(h2.norm(p), closeTo(1, 1e-6));
      }
    });

    test('feathered: dragging inside the inner ellipse is held just outside it (never negative feather)', () {
      final s = spaceOf(mapOf(0, false));
      final h = RadialHandles(s, circle(s));
      final m2 = h.feathered(h.centre);
      expect(m2.feather, inInclusiveRange(0, 100));
      expect(m2.rx, greaterThan(0));
    });

    test('moved(delta) moves the centre by delta in local dp', () {
      for (final (rot, flip) in combos) {
        final s = spaceOf(mapOf(rot, flip));
        final h = RadialHandles(s, circle(s));
        const d = Offset(37, -22);
        final h2 = RadialHandles(s, h.moved(d));
        expectOffset(h2.centre, h.centre + d, 1e-6, reason: 'rot $rot flip $flip');
        expect(h2.m.rx, h.m.rx);
      }
    });

    test('radialFrom makes a circle of the asked radius on the finger', () {
      for (final (rot, flip) in combos) {
        final s = spaceOf(mapOf(rot, flip));
        final m = radialFrom(s, const MaskComponent(kind: MaskKindId.radial), const Offset(400, 350), 120);
        final h = RadialHandles(s, m);
        expectOffset(h.centre, const Offset(400, 350), 1e-6);
        expect(h.rxDp, closeTo(120, 1e-6));
        expect(h.ryDp, closeTo(120, 1e-6));
      }
    });
  });

  group('BrushInput', () {
    HandleSpace flat() => HandleSpace(FrameMap.identity(1), const Size(100, 100));

    test('points closer than radiusLE / 4 are dropped', () {
      // radius 0.2 LE -> spacing 0.05 LE = 5 dp
      final b = BrushInput(space: flat(), radiusLE: 0.2, smoothing: 0);
      expect(b.add(const Offset(10, 10)), isTrue);
      expect(b.add(const Offset(12, 10)), isFalse);
      expect(b.add(const Offset(14, 10)), isFalse);
      expect(b.add(const Offset(16, 10)), isTrue);
      expect(b.total, 2);
      final pts = b.take();
      expect(pts.length, 4);
      expect(pts[0], closeTo(0.1, 1e-12));
      expect(pts[2], closeTo(0.16, 1e-12));
    });

    test('take() empties the buffer', () {
      final b = BrushInput(space: flat(), radiusLE: 0.2, smoothing: 0);
      b.add(const Offset(10, 10));
      expect(b.hasPending, isTrue);
      expect(b.take(), isNotEmpty);
      expect(b.hasPending, isFalse);
      expect(b.take(), isEmpty);
    });

    test('finish() keeps the last point even when it is closer than the spacing', () {
      final b = BrushInput(space: flat(), radiusLE: 0.2, smoothing: 0);
      b.add(const Offset(10, 10));
      b.take();
      expect(b.add(const Offset(12, 10)), isFalse);
      b.finish(const Offset(12, 10));
      final pts = b.take();
      expect(pts, hasLength(2));
      expect(pts[0], closeTo(0.12, 1e-12));
      // a tap (down, up at the same place) leaves exactly the one dot
      final t = BrushInput(space: flat(), radiusLE: 0.2, smoothing: 0);
      t.add(const Offset(50, 50));
      t.finish(const Offset(50, 50));
      expect(t.take(), hasLength(2));
    });

    test('smoothing makes the kept points lag the raw ones', () {
      final b = BrushInput(space: flat(), radiusLE: 0.02, smoothing: 0.45);
      final raw = <double>[], kept = <double>[];
      for (var x = 10.0; x <= 90; x += 4) {
        if (b.add(Offset(x, 40))) {
          raw.add(x);
          kept.add(b.take()[0] * 100);
        }
      }
      expect(kept.length, greaterThan(5));
      expect(kept.first, closeTo(raw.first, 1e-9), reason: 'the first point is the raw one');
      for (var i = 1; i < kept.length; i++) {
        expect(kept[i], lessThan(raw[i]), reason: 'point $i lags');
        expect(kept[i], greaterThan(kept[i - 1]));
      }
      // with no smoothing nothing lags
      final n = BrushInput(space: flat(), radiusLE: 0.02, smoothing: 0);
      n.add(const Offset(10, 40));
      n.take();
      expect(n.add(const Offset(14, 40)), isTrue);
      expect(n.take()[0] * 100, closeTo(14, 1e-9));
    });
  });

  group('brushAppend', () {
    const base = MaskComponent(kind: MaskKindId.brush);

    test('newStroke adds a stroke with the given size and erase flag', () {
      final m = brushAppend(base, [0.1, 0.2, 0.3, 0.4], newStroke: true, size: 0.07, erase: true);
      expect(m.strokes, hasLength(1));
      expect(m.strokes[0].size, 0.07);
      expect(m.strokes[0].erase, isTrue);
      expect(m.strokes[0].count, 2);
      expect(base.strokes, isEmpty, reason: 'the input is not mutated');
    });

    test('an empty point list returns the component itself', () {
      expect(identical(brushAppend(base, const [], newStroke: true), base), isTrue);
    });

    test('appending to the last stroke does not copy the earlier strokes', () {
      var m = brushAppend(base, [0.1, 0.1], newStroke: true, size: 0.03);
      m = brushAppend(m, [0.2, 0.2], newStroke: true, size: 0.04, erase: true);
      expect(m.strokes, hasLength(2));
      final n = brushAppend(m, [0.3, 0.3, 0.4, 0.4], newStroke: false);
      expect(n.strokes, hasLength(2));
      expect(identical(n.strokes[0], m.strokes[0]), isTrue);
      expect(identical(n.strokes[1], m.strokes[1]), isFalse);
      expect(n.strokes[1].count, 3);
      expect(n.strokes[1].erase, isTrue, reason: 'the stroke keeps its own flags');
      expect(m.strokes[1].count, 1, reason: 'the old component is untouched');
    });

    test('appending with no stroke yet starts one even without newStroke', () {
      final m = brushAppend(base, [0.5, 0.5], newStroke: false, size: 0.02);
      expect(m.strokes, hasLength(1));
      expect(m.strokes[0].size, 0.02);
    });
  });
}
