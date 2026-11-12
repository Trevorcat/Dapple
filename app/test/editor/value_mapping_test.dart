import 'package:dapple/editor/value_mapping.dart';
import 'package:flutter_test/flutter_test.dart';

RelativeMapper exposure() => RelativeMapper(min: -100, max: 100, auto: 0);

void main() {
  group('gain', () {
    test('slow = 0.35, fast = 2.5, smooth in between', () {
      expect(gainFor(0), closeTo(0.35, 1e-9));
      expect(gainFor(40), closeTo(0.35, 1e-9));
      expect(gainFor(900), closeTo(2.5, 1e-9));
      expect(gainFor(5000), closeTo(2.5, 1e-9));
      final mid = gainFor(470);
      expect(mid, greaterThan(1.2));
      expect(mid, lessThan(1.6)); // spec table: about 1.4
      var prev = 0.0;
      for (var v = 0.0; v <= 1000; v += 25) {
        expect(gainFor(v), greaterThanOrEqualTo(prev));
        prev = gainFor(v);
      }
    });
  });

  group('relative mapping', () {
    test('slow drag: 100 dp moves 100*0.35*range/320 regardless of where the finger is', () {
      final m = exposure()..begin(50);
      for (var i = 0; i < 20; i++) {
        m.update(5, 20, PrecisionBand.standard, 16);
      }
      expect(m.value - 50, closeTo(100 * 0.35 * 200 / 320, 0.01));
    });

    test('fast drag covers far more range than slow for the same distance', () {
      final slow = exposure()..begin(-60);
      final fast = exposure()..begin(-60);
      for (var i = 0; i < 15; i++) {
        slow.update(6, 30, PrecisionBand.standard, 16);
        fast.update(6, 1000, PrecisionBand.standard, 16);
      }
      expect(fast.value - (-60), greaterThan((slow.value - (-60)) * 3));
    });

    test('a thumb sweep of about 250 dp at speed reaches >= 70 % of the range', () {
      final m = exposure()..begin(-100);
      for (var i = 0; i < 20; i++) {
        m.update(12.5, 780, PrecisionBand.standard, 16);
      }
      expect((m.value + 100) / 200, greaterThan(0.7));
    });

    test('direction: left decreases, right increases; clamped without overshoot', () {
      final m = exposure()..begin(90);
      for (var i = 0; i < 40; i++) {
        m.update(30, 900, PrecisionBand.standard, 16);
      }
      expect(m.value, 100);
      for (var i = 0; i < 80; i++) {
        m.update(-30, 900, PrecisionBand.standard, 16);
      }
      expect(m.value, -100);
    });

    test('quantises to the step only at the output, the raw value stays float', () {
      final m = RelativeMapper(min: 0, max: 150, auto: 40)..begin(100);
      m.update(1, 20, PrecisionBand.standard, 16); // 1 * .35 * 150 / 320 = 0.16
      expect(m.value, closeTo(100.164, 0.01));
      expect(m.quantized, 100);
    });
  });

  group('precision bands', () {
    test('thresholds 64 / 160 dp with +-12 dp hysteresis', () {
      final b = BandTracker();
      expect(b.update(30), PrecisionBand.standard);
      expect(b.update(70), PrecisionBand.standard); // inside hysteresis
      expect(b.update(90), PrecisionBand.fine);
      expect(b.update(60), PrecisionBand.fine); // still within 12 dp of 64
      expect(b.update(50), PrecisionBand.standard);
      expect(b.update(-200), PrecisionBand.ultra); // downward offsets count too
      expect(b.update(150), PrecisionBand.ultra);
      expect(b.update(140), PrecisionBand.fine);
    });

    test('thresholds scale with the available height (landscape phones)', () {
      final b = BandTracker(availableHeight: 360); // clamp(0.45) -> 0.6
      expect(b.scale, 0.6);
      expect(b.update(60), PrecisionBand.fine); // 64 * .6 + 12 = 50.4
    });

    test('fine band gives a quarter of the delta, ultra 1/16', () {
      double run(PrecisionBand band) {
        final m = exposure()..begin(40);
        // let the band ramp settle first
        for (var i = 0; i < 20; i++) {
          m.update(0, 20, band, 16);
        }
        final v0 = m.value;
        m.update(10, 20, band, 16);
        return m.value - v0;
      }

      final std = run(PrecisionBand.standard);
      expect(run(PrecisionBand.fine), closeTo(std / 4, std * 0.02));
      expect(run(PrecisionBand.ultra), closeTo(std / 16, std * 0.02));
    });
  });

  group('magnetic snap to the auto position', () {
    test('captures inside +-1.5 % of the range, holds, breaks free after 10 dp without a jump', () {
      final m = exposure()..begin(12);
      Landmark? seen;
      var v = 12.0;
      for (var i = 0; i < 40 && seen != Landmark.autoSnap; i++) {
        v = m.update(-3, 20, PrecisionBand.standard, 16);
        if (m.landmark == Landmark.autoSnap) seen = m.landmark;
      }
      expect(seen, Landmark.autoSnap);
      expect(v, 0);
      expect(m.snapped, isTrue);
      // 3 more steps of 3 dp = 9 dp: still held
      for (var i = 0; i < 3; i++) {
        expect(m.update(-3, 20, PrecisionBand.standard, 16), 0);
      }
      // the next one passes 10 dp: continues from 0 by exactly that step's delta
      final after = m.update(-3, 20, PrecisionBand.standard, 16);
      expect(m.snapped, isFalse);
      expect(after, closeTo(-3 * 0.35 * 200 / 320, 0.01));
      expect(after, lessThan(0));
    });

    test('a gesture that starts on the auto position must leave the window before it can capture again', () {
      final m = exposure()..begin(0);
      for (var i = 0; i < 3; i++) {
        m.update(2, 20, PrecisionBand.standard, 16);
        expect(m.snapped, isFalse);
        expect(m.landmark, Landmark.none);
      }
    });

    test('no capture in the ultra-fine band, narrower window in the fine band', () {
      final m = exposure()..begin(5);
      for (var i = 0; i < 6; i++) {
        m.update(-1, 20, PrecisionBand.ultra, 16);
      }
      expect(m.snapped, isFalse);
      expect(RelativeMapper.captureWindow(PrecisionBand.fine), 0.005);
      expect(RelativeMapper.captureWindow(PrecisionBand.standard), 0.015);
      expect(RelativeMapper.captureWindow(PrecisionBand.ultra), 0);
    });
  });

  group('range ends', () {
    test('fires once on arrival, re-arms only after leaving the end by 5 % of the range', () {
      final m = exposure()..begin(95);
      var ends = 0;
      for (var i = 0; i < 10; i++) {
        m.update(8, 900, PrecisionBand.standard, 16);
        if (m.landmark == Landmark.endStop) ends++;
      }
      expect(ends, 1);
      expect(m.endSide, 1);
      // back off by less than 5 % then forward again: no second haptic
      m.update(-1, 900, PrecisionBand.standard, 16);
      m.update(8, 900, PrecisionBand.standard, 16);
      m.update(8, 900, PrecisionBand.standard, 16);
      expect(m.landmark, isNot(Landmark.endStop));
      // leave by well over 5 % and return: armed again
      for (var i = 0; i < 10; i++) {
        m.update(-8, 900, PrecisionBand.standard, 16);
      }
      for (var i = 0; i < 20 && m.landmark != Landmark.endStop; i++) {
        m.update(8, 900, PrecisionBand.standard, 16);
      }
      expect(m.landmark, Landmark.endStop);
    });
  });

  group('detent stepping', () {
    test('48 dp per item, 25 % hysteresis when reversing, no wrap, end stop at the ends', () {
      final s = DetentStepper(stepDp: 48, count: 4)..begin(1);
      expect(s.update(47), isFalse);
      expect(s.update(2), isTrue);
      expect(s.index, 2);
      expect(s.landmark, Landmark.detent);
      // reversing needs only 12 dp (25 %) from the last step
      expect(s.update(-11), isFalse);
      expect(s.update(-2), isTrue);
      expect(s.index, 1);
      // reversing again is 12 dp as well, but continuing the same way needs a full 48
      expect(s.update(11), isFalse);
      expect(s.update(2), isTrue);
      expect(s.index, 2);
      s.update(200);
      expect(s.index, 3);
      expect(s.landmark, Landmark.endStop);
    });

    test('one fast sample can cross several detents', () {
      final s = DetentStepper(stepDp: 40, count: 9)..begin(0);
      s.update(130);
      expect(s.index, 3);
    });

    test('never goes below 0', () {
      final s = DetentStepper(stepDp: 40, count: 3)..begin(0);
      expect(s.update(-100), isFalse);
      expect(s.index, 0);
      expect(s.landmark, Landmark.endStop);
    });
  });

  group('2D mappers', () {
    test('palette: 300 dp = full range, up = warm, clamps 0.04..0.96, snaps to the look point', () {
      final p = PaletteMapper(defX: 0.5, defY: 0.5)..begin(0.8, 0.8);
      for (var i = 0; i < 40; i++) {
        p.update(0, -10, 20, 16);
      }
      expect(p.y, lessThan(0.8));
      expect(p.x, closeTo(0.8, 1e-9));
      var snapped = false;
      for (var i = 0; i < 200 && !snapped; i++) {
        p.update(-4, 0, 20, 16);
        snapped = p.snapped;
      }
      expect(snapped, isTrue);
      expect(p.x, 0.5);
      expect(p.y, 0.5);
      final c = PaletteMapper(defX: 0.5, defY: 0.5)..begin(0.9, 0.9);
      for (var i = 0; i < 60; i++) {
        c.update(40, 40, 900, 16);
      }
      expect(c.x, 0.96);
      expect(c.y, 0.96);
    });

    test('wheel: hue from the angle, saturation from the radius, centre snaps and keeps the old hue', () {
      final w = WheelMapper()..begin(0, 60); // red at 60 %
      for (var i = 0; i < 150; i++) {
        w.update(-3, 0, 20, 16);
      }
      // moving left through the centre flips the hue to the opposite side
      expect(w.hue, closeTo(180, 1));
      final w2 = WheelMapper()..begin(90, 20);
      var snapped = false;
      for (var i = 0; i < 80 && !snapped; i++) {
        w2.update(0, -2, 20, 16);
        snapped = w2.snapped;
      }
      expect(snapped, isTrue);
      expect(w2.sat, 0);
      expect(w2.hue, closeTo(90, 1));
    });
  });

  test('velocity window uses the last 60 ms only', () {
    final v = VelocityWindow();
    v.add(0, 0, 0);
    v.add(100, 100, 0); // fast, old
    for (var t = 110; t <= 200; t += 10) {
      v.add(t, 100 + (t - 100) * 0.1, 0); // 100 dp/s
    }
    v.compute(200);
    expect(v.vx, closeTo(100, 5));
  });
}
