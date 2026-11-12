import 'dart:math' as math;

import 'package:dapple/editor/gesture_arbiter.dart';
import 'package:dapple/editor/handedness.dart';
import 'package:dapple/editor/window_manager.dart';
import 'package:flutter_test/flutter_test.dart';

const double kW = 412;

/// A thumb swipe around a pivot at the bottom corner: an arc of radius [r] from angle [a0] to [a1]
/// (degrees, screen coordinates). Right thumbs pivot at the bottom right, left thumbs at the left.
StrokeSample arc(bool right, {double r = 450, double a0 = 200, double a1 = 245, double orient = 0}) {
  final px = right ? kW + 20 : -20.0, py = 1000.0;
  Offset at(double deg) => Offset(px + r * math.cos(deg * math.pi / 180), py + r * math.sin(deg * math.pi / 180));
  // left thumbs sweep the mirrored arc
  final s = right ? at(a0) : at(540 - a0), m = right ? at((a0 + a1) / 2) : at(540 - (a0 + a1) / 2), e = right ? at(a1) : at(540 - a1);
  return StrokeSample(
    isTap: false,
    downX: s.dx.clamp(0, kW).toDouble(),
    startX: s.dx,
    startY: s.dy,
    midX: m.dx,
    midY: m.dy,
    endX: e.dx,
    endY: e.dy,
    orientation: orient,
    radiusMajor: orient == 0 ? 0 : 12,
    radiusMinor: orient == 0 ? 0 : 8,
  );
}

StrokeSample tapAt(double x) => StrokeSample(isTap: true, downX: x, startX: x, startY: 700, midX: x, midY: 700, endX: x, endY: 700, orientation: 0, radiusMajor: 0, radiusMinor: 0);

void main() {
  group('arc feature', () {
    test('arcs of a right thumb vote right, of a left thumb vote left, with confidence', () {
      final r = HandednessInferrer.arcVote(arc(true), kW)!;
      final l = HandednessInferrer.arcVote(arc(false), kW)!;
      expect(r.score, greaterThan(0.5));
      expect(l.score, lessThan(-0.5));
      expect(r.confidence, greaterThan(0.5));
    });

    test('a straight stroke carries no information', () {
      const s = StrokeSample(isTap: false, downX: 100, startX: 100, startY: 500, midX: 200, midY: 500, endX: 300, endY: 500, orientation: 0, radiusMajor: 0, radiusMinor: 0);
      expect(HandednessInferrer.arcVote(s, kW), isNull);
    });
  });

  group('majority vote with hysteresis', () {
    test('a right-handed history never flips, a left-handed one does after 12+ qualifying gestures', () {
      final h = HandednessInferrer(screenWidth: kW);
      for (var i = 0; i < 30; i++) {
        h.add(arc(true), i * 3000);
        expect(h.evaluate(i * 3000), isFalse);
      }
      expect(h.side, Hand.right);

      final l = HandednessInferrer(screenWidth: kW);
      var t = 0;
      for (var i = 0; i < 11; i++) {
        l.add(arc(false), t += 3000);
        expect(l.evaluate(t), isFalse, reason: 'fewer than 12 samples');
      }
      l.add(arc(false), t += 3000);
      expect(l.evaluate(t), isTrue);
      expect(l.side, Hand.left);
    });

    test('mixed evidence stays on the current side (|S| < 0.35)', () {
      final h = HandednessInferrer(screenWidth: kW);
      var t = 0;
      for (var i = 0; i < 40; i++) {
        h.add(arc(i.isEven), t += 4000);
      }
      expect(h.evaluate(t), isFalse);
      expect(h.side, Hand.right);
      expect(h.aggregate.abs(), lessThan(0.35));
    });

    test('after a switch there is a 5 minute cool-down; the opposite evidence then switches back', () {
      final h = HandednessInferrer(screenWidth: kW);
      var t = 0;
      for (var i = 0; i < 14; i++) {
        h.add(arc(false), t += 3000);
      }
      expect(h.evaluate(t), isTrue);
      final switched = t;
      for (var i = 0; i < 40; i++) {
        h.add(arc(true), t += 1000);
      }
      expect(h.aggregate, greaterThan(0.35));
      expect(h.evaluate(switched + 4 * 60 * 1000), isFalse);
      expect(h.evaluate(switched + 5 * 60 * 1000 + 1), isTrue);
      expect(h.side, Hand.right);
    });

    test('both sides used within 2 seconds (two-handed): keeps the last side', () {
      final h = HandednessInferrer(screenWidth: kW);
      var t = 0;
      for (var i = 0; i < 20; i++) {
        h.add(arc(false), t += 3000);
      }
      // taps alternating between the left and right edge right now
      h.add(tapAt(40), t += 300);
      h.add(tapAt(380), t += 300);
      expect(h.evaluate(t), isFalse);
      expect(h.side, Hand.right);
    });

    test('touch-down x distribution alone leans towards the side taps land on', () {
      final h = HandednessInferrer(screenWidth: kW);
      for (var i = 0; i < 20; i++) {
        h.add(tapAt(60 + (i % 3) * 5), i * 5000);
      }
      expect(h.aggregate, lessThan(0));
    });

    test('touch ellipse orientation only counts when the device reports it with spread', () {
      final h = HandednessInferrer(screenWidth: kW);
      for (var i = 0; i < 7; i++) {
        expect(h.add(arc(true, orient: -0.3), i * 3000), isNotNull);
      }
      // constant orientation (no information): the feature never turns on
      final c = HandednessInferrer(screenWidth: kW);
      for (var i = 0; i < 20; i++) {
        c.add(arc(true, orient: 0.1), i * 3000);
      }
      final varied = HandednessInferrer(screenWidth: kW);
      for (var i = 0; i < 20; i++) {
        varied.add(arc(true, orient: i.isEven ? -0.4 : -0.1), i * 3000);
      }
      expect(varied.aggregate, greaterThan(0));
    });

    test('reset clears the samples', () {
      final h = HandednessInferrer(screenWidth: kW);
      for (var i = 0; i < 14; i++) {
        h.add(arc(false), i * 3000);
      }
      h.reset();
      expect(h.sampleCount, 0);
      expect(h.evaluate(100000), isFalse);
    });
  });
}
