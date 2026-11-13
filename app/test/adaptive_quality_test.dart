import 'package:dapple/services/adaptive_quality.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  refreshBudgetTests();
  seedTests();
  dragHoldTests();
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
    expect(q.rung.longEdge, 360);

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

// wave 8: the budget follows the display refresh period and the rung follows the measured frame time
void refreshBudgetTests() {
  test('budget is one display refresh, clamped to 8.3..55 ms', () {
    expect(AdaptiveQuality.budgetForRefresh(120), closeTo(8.33, 0.01));
    expect(AdaptiveQuality.budgetForRefresh(144), 8.3);
    expect(AdaptiveQuality.budgetForRefresh(60), closeTo(16.67, 0.01));
    expect(AdaptiveQuality.budgetForRefresh(30), closeTo(33.3, 0.1));
    expect(AdaptiveQuality.budgetForRefresh(0), 55);
    expect(AdaptiveQuality.budgetForRefresh(double.nan), 55);
  });

  test('a 120 Hz budget steps a slow GPU down to the rung that fits', () {
    final q = AdaptiveQuality(budgetMs: AdaptiveQuality.budgetForRefresh(120));
    // cost scales with pixels: 12 ms at 810x1080 -> 810 -> 640 (7.5 ms) fits
    var cost = 17.0; // at 1080
    for (var i = 0; i < 40; i++) {
      final px = q.rung.longEdge / 1080;
      cost = 17.0 * px * px;
      q.record(cost);
    }
    expect(q.rung.longEdge, lessThan(1080));
    expect(cost, lessThanOrEqualTo(8.3));
  });
}

// wave 8: the starting rung comes from the warm-up measurement
void seedTests() {
  test('the seed picks the largest rung whose predicted cost fits the budget', () {
    final q = AdaptiveQuality(budgetMs: 8.33);
    // 20 ms / Mpx: a 3:2 frame of long edge 480 is 0.154 Mpx = 3.1 ms, 810 is 0.437 Mpx = 8.7 ms (> 0.8 * 8.33), 640 is 0.273 Mpx = 5.5 ms
    expect(q.seedFromMeasurement(20), isTrue);
    expect(q.rung.longEdge, 640);
    expect(q.seeded, isTrue);
    // a faster device keeps the top rung
    final fast = AdaptiveQuality(budgetMs: 8.33);
    expect(fast.seedFromMeasurement(5), isFalse);
    expect(fast.rung.longEdge, 1080);
    // a very slow one takes the cheapest rung, never below it
    final slow = AdaptiveQuality(budgetMs: 8.33);
    slow.seedFromMeasurement(500);
    expect(slow.rung.longEdge, 360);
    // garbage does nothing
    final none = AdaptiveQuality();
    expect(none.seedFromMeasurement(0), isFalse);
    expect(none.seeded, isFalse);
  });
}

void dragHoldTests() {
  test('the rung is frozen for the whole drag and moves by one step at its end', () {
    final q = AdaptiveQuality(budgetMs: 16.7);
    q.beginDrag();
    for (var i = 0; i < 40; i++) {
      expect(q.record(60), isFalse);
      expect(q.rung.longEdge, 1080);
    }
    expect(q.endDrag(), isTrue);
    expect(q.rung.longEdge, 810); // one step only, however slow the frames were
    q.beginDrag();
    for (var i = 0; i < 40; i++) {
      q.record(1);
    }
    expect(q.rung.longEdge, 810);
    expect(q.endDrag(), isTrue);
    expect(q.rung.longEdge, 1080);
    q.beginDrag();
    expect(q.endDrag(), isFalse);
  });
}

