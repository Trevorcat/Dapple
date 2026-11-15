import 'package:dapple/services/adaptive_quality.dart';
import 'package:flutter_test/flutter_test.dart';

/// Frame time of the model the controller assumes: overhead + msPerMpx x megapixels of the current rung.
double frameMs(AdaptiveQuality q, double overhead, double msPerMpx) => overhead + msPerMpx * AdaptiveQuality.mpx(q.rung);

/// One drag of [n] frames (held), then its end. Returns the rung long edge afterwards.
int drag(AdaptiveQuality q, double overhead, double msPerMpx, {int n = 40}) {
  q.beginDrag();
  for (var i = 0; i < n; i++) {
    q.record(frameMs(q, overhead, msPerMpx));
  }
  q.endDrag();
  return q.rung.longEdge;
}

void main() {
  refreshBudgetTests();
  seedTests();
  splitTests();
  dragHoldTests();
  capTests();

  test('every rung keeps the dE-validated lattice size', () {
    for (final r in AdaptiveQuality.rungs) {
      expect(r.bake, greaterThanOrEqualTo(57));
    }
  });
}

void refreshBudgetTests() {
  test('budget is one display refresh, clamped to 8.3..55 ms', () {
    expect(AdaptiveQuality.budgetForRefresh(120), closeTo(8.33, 0.01));
    expect(AdaptiveQuality.budgetForRefresh(144), 8.3);
    expect(AdaptiveQuality.budgetForRefresh(60), closeTo(16.67, 0.01));
    expect(AdaptiveQuality.budgetForRefresh(30), closeTo(33.3, 0.1));
    expect(AdaptiveQuality.budgetForRefresh(0), 55);
    expect(AdaptiveQuality.budgetForRefresh(double.nan), 55);
  });
}

void seedTests() {
  test('the seed picks the largest rung whose predicted cost fits the budget and sets the pixel prior', () {
    final q = AdaptiveQuality(budgetMs: 8.33);
    // 20 ms / Mpx: 480 is 0.154 Mpx = 3.1 ms, 810 is 0.437 Mpx = 8.7 ms (> 0.8 * 8.33), 640 is 0.273 Mpx = 5.5 ms
    expect(q.seedFromMeasurement(20), isTrue);
    expect(q.rung.longEdge, 640);
    expect(q.seeded, isTrue);
    expect(q.pixelMsPerMpx, 20);
    final fast = AdaptiveQuality(budgetMs: 8.33);
    expect(fast.seedFromMeasurement(5), isFalse);
    expect(fast.rung.longEdge, 1080);
    final slow = AdaptiveQuality(budgetMs: 8.33);
    slow.seedFromMeasurement(500);
    expect(slow.rung.longEdge, 360);
    final none = AdaptiveQuality();
    expect(none.seedFromMeasurement(0), isFalse);
    expect(none.seeded, isFalse);
  });
}

// the frame time is split into a size-independent overhead and a pixel part; only the pixel part moves the rung
void splitTests() {
  test('table-heavy frames keep a large rung (overhead does not shrink the picture)', () {
    // 60 Hz, 20 ms / Mpx: exposure drag on a phone, 7 ms of tables + a little GPU
    final q = AdaptiveQuality(budgetMs: 16.67)..pixelMsPerMpx = 20;
    for (var d = 0; d < 6; d++) {
      drag(q, 7, 20);
    }
    // allowance 16.67 - 7 = 9.7 ms: 810 (8.7 ms) fits, 1080 (15.6 ms) does not
    expect(q.rung.longEdge, 810);
    // a very table-heavy drag (colour pad, 34 ms of tables) is over the budget however small the picture: it must not collapse
    final heavy = AdaptiveQuality(budgetMs: 16.67)..pixelMsPerMpx = 20;
    for (var d = 0; d < 6; d++) {
      drag(heavy, 34, 20);
    }
    expect(heavy.rung.longEdge, greaterThanOrEqualTo(640), reason: 'the overhead alone is over the budget: dropping buys nothing');
  });

  test('GPU-heavy frames step down to the rung whose pixel part fits', () {
    final q = AdaptiveQuality(budgetMs: 8.33)..pixelMsPerMpx = 20;
    for (var d = 0; d < 6; d++) {
      drag(q, 1, 20); // 1 ms overhead: 1080 costs 16.6 ms, 640 6.5 ms
    }
    expect(q.rung.longEdge, 640);
    expect(frameMs(q, 1, 20), lessThanOrEqualTo(8.33));
    // a slow GPU (60 ms / Mpx) with the same budget goes lower
    final slow = AdaptiveQuality(budgetMs: 8.33)..pixelMsPerMpx = 60;
    for (var d = 0; d < 6; d++) {
      drag(slow, 1, 60);
    }
    expect(slow.rung.longEdge, 360);
  });

  test('overhead above the budget does not shrink the picture, and a rung dropped to earlier is left again', () {
    final q = AdaptiveQuality(budgetMs: 16.67)..pixelMsPerMpx = 20;
    // 40 ms of overhead: every frame is over budget, the allowance floor (half a refresh) holds 810 / 640, not 360
    drag(q, 40, 20);
    drag(q, 40, 20);
    expect(q.rung.longEdge, greaterThanOrEqualTo(640));
    // sitting on 360 (as the old controller left it after table-heavy drags): cheap frames (~3 ms, 局部) bring it back in one drag
    final sunk = AdaptiveQuality(budgetMs: 16.67)
      ..seedFromMeasurement(500)
      ..pixelMsPerMpx = 20;
    expect(sunk.rung.longEdge, 360);
    expect(drag(sunk, 3, 20), 810); // 1080 needs 15.6 ms of pixels, the allowance is 13.7 ms
  });

  test('hysteresis: noise around the boundary does not move the rung, a sustained change does', () {
    final q = AdaptiveQuality(budgetMs: 16.67)..pixelMsPerMpx = 20;
    drag(q, 7, 20);
    drag(q, 7, 20);
    expect(q.rung.longEdge, 810);
    // overhead jitters between 6 and 9 ms frame by frame; the rung stays
    q.beginDrag();
    for (var i = 0; i < 60; i++) {
      q.record(i.isEven ? 6 + 8.7 : 9 + 8.7);
    }
    expect(q.endDrag(), isFalse);
    expect(q.rung.longEdge, 810);
    // overhead 2 ms: the allowance is 14.7 ms but 1080 needs 15.6 ms: stays
    drag(q, 2, 20);
    expect(q.rung.longEdge, 810);
    // a cheaper pixel part (12 ms / Mpx: 9.3 ms for 1080) fits with the margin: goes up
    q.pixelMsPerMpx = 12;
    drag(q, 2, 12);
    expect(q.rung.longEdge, 1080);
  });

  test('outside a drag the rung moves one step at a time, only after the confirmation count', () {
    final q = AdaptiveQuality(budgetMs: 8.33)..pixelMsPerMpx = 20;
    expect(q.record(frameMs(q, 1, 20)), isFalse);
    expect(q.record(frameMs(q, 1, 20)), isTrue); // second consecutive frame over budget
    expect(q.rung.longEdge, 810);
  });
}

void dragHoldTests() {
  test('the rung is frozen for the whole drag', () {
    final q = AdaptiveQuality(budgetMs: 8.33)..pixelMsPerMpx = 20;
    q.beginDrag();
    for (var i = 0; i < 40; i++) {
      expect(q.record(frameMs(q, 1, 20)), isFalse);
      expect(q.rung.longEdge, 1080);
    }
    expect(q.held, isTrue);
    expect(q.endDrag(), isTrue);
    expect(q.rung.longEdge, lessThan(1080));
    q.beginDrag();
    expect(q.endDrag(), isFalse);
  });
}

void capTests() {
  test('display and thermal caps are floors', () {
    final small = AdaptiveQuality()..setDisplayLongEdge(700);
    expect(small.rung.longEdge, 810); // the smallest rung still >= the on-screen size
    for (var d = 0; d < 4; d++) {
      drag(small, 0.5, 5);
    }
    expect(small.rung.longEdge, 810); // no stepping up beyond what the display shows

    final hot = AdaptiveQuality()..setThermalHeadroom(0.85);
    expect(hot.rung.longEdge, 810);
    for (var d = 0; d < 4; d++) {
      drag(hot, 0.5, 5);
    }
    expect(hot.rung.longEdge, 810);
    hot.setThermalHeadroom(1.0);
    expect(hot.rung.longEdge, 640);
    for (var d = 0; d < 4; d++) {
      drag(hot, 0.5, 5);
    }
    expect(hot.rung.longEdge, 640);
    // a cool reading lifts the floor again
    hot.setThermalHeadroom(0.2);
    drag(hot, 0.5, 5);
    expect(hot.rung.longEdge, 1080);
  });

  test('never below the cheapest rung', () {
    final q = AdaptiveQuality(budgetMs: 8.33)..pixelMsPerMpx = 500;
    for (var d = 0; d < 8; d++) {
      drag(q, 1, 500);
    }
    expect(q.rung.longEdge, 360);
  });
}
