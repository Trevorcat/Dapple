import 'package:dapple/services/adaptive_quality.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('steps down after two frames over budget, not after one', () {
    final q = AdaptiveQuality(budgetMs: 55);
    expect(q.rung.longEdge, 1080);
    expect(q.record(80), isFalse);
    expect(q.record(40), isFalse); // a good frame resets the count
    expect(q.record(80), isFalse);
    expect(q.record(80), isTrue);
    expect(q.rung.longEdge, 810);
  });

  test('steps up only after a run of frames predicted to fit with margin (hysteresis)', () {
    final q = AdaptiveQuality(budgetMs: 55, upAfter: 12);
    q
      ..record(90)
      ..record(90);
    expect(q.rung.longEdge, 810);
    // 810 -> 1080 is 1.78x the pixels: 20 ms predicts 35.6 ms < 0.6 * 55 = 33? no -> stays
    for (var i = 0; i < 30; i++) {
      q.record(20);
    }
    expect(q.rung.longEdge, 810);
    for (var i = 0; i < 11; i++) {
      expect(q.record(15), isFalse);
    }
    expect(q.record(15), isTrue); // 12th frame with 26.7 ms predicted
    expect(q.rung.longEdge, 1080);
  });

  test('never below the cheapest rung, display and thermal caps are floors', () {
    final q = AdaptiveQuality();
    for (var i = 0; i < 20; i++) {
      q.record(500);
    }
    expect(q.rung.longEdge, 480);

    final small = AdaptiveQuality()..setDisplayLongEdge(700);
    expect(small.rung.longEdge, 810); // the smallest rung still >= the on-screen size
    for (var i = 0; i < 40; i++) {
      small.record(1);
    }
    expect(small.rung.longEdge, 810); // no stepping up beyond what the display shows

    final hot = AdaptiveQuality()..setThermalHeadroom(0.85);
    expect(hot.rung.longEdge, 810);
    for (var i = 0; i < 40; i++) {
      hot.record(1);
    }
    expect(hot.rung.longEdge, 810);
    hot.setThermalHeadroom(1.0);
    expect(hot.rung.longEdge, 640);
  });

  test('every rung keeps the dE-validated lattice size', () {
    for (final r in AdaptiveQuality.rungs) {
      expect(r.bake, greaterThanOrEqualTo(57));
    }
  });
}
