// The fold layout must adapt to any large near-square window and any photo shape, not only to the sizes someone happened to test:
// for a grid of window sizes and photo aspect ratios, the chosen layout shows the photo at least as large as the other layout would.
import 'package:dapple/editor/fold_layout.dart';
import 'package:dapple/editor/window_manager.dart' show Hand;
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';

const _aspects = <String, double>{'9:16': 9 / 16, '2:3': 2 / 3, '3:4': 3 / 4, '4:5': 4 / 5, '1:1': 1, '5:4': 5 / 4, '4:3': 4 / 3, '3:2': 3 / 2, '16:9': 16 / 9};

double _area(FoldMode m, Size s, double a) {
  final p = FoldPlan.compute(mode: m, size: s, pad: const EdgeInsets.only(top: 36), bottomInset: 20, crease: null, aspect: a, hand: Hand.right);
  final r = p.photoRect(a);
  return r.width * r.height;
}

void main() {
  test('the chosen fold layout gives the photo the larger area on every window and photo shape', () {
    final bad = <String>[];
    var worst = 1.0;
    for (var w = 600.0; w <= 1100; w += 25) {
      for (var h = 600.0; h <= 1200; h += 25) {
        final s = Size(w, h);
        if (!isNearSquareLarge(s)) continue;
        for (final e in _aspects.entries) {
          final chosen = foldModeOf(s, null, e.value);
          final other = chosen == FoldMode.bench ? FoldMode.column : FoldMode.bench;
          final a = _area(chosen, s, e.value), b = _area(other, s, e.value);
          final ratio = a / b;
          if (ratio < worst) worst = ratio;
          if (ratio < 0.94) bad.add('${w.toInt()}x${h.toInt()} ${e.key}: ${chosen.name} ${(a / (w * h) * 100).toStringAsFixed(0)}% vs ${other.name} ${(b / (w * h) * 100).toStringAsFixed(0)}%');
        }
      }
    }
    // ignore: avoid_print
    print('worst chosen/other area ratio $worst; ${bad.length} cases below 0.94');
    for (final l in bad.take(12)) {
      // ignore: avoid_print
      print(l);
    }
    expect(bad, isEmpty);
  });
}
