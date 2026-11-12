import 'package:dapple/editor/value_mapping.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
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
