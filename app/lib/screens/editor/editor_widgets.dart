// The curve chart of the 曲线 editor (docs/design/editor-panel.md section 5).
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../../services/editor_engine.dart';

// ------------------------------------------------------------------ curve chart

/// Curve of one channel from its four zone sliders (bell shapes, centres .88/.62/.37/.12, amplitude .18).
double curveY(double x, double h, double l, double d, double s) {
  double bell(double c, double v) => v / 100 * 0.18 * math.exp(-math.pow((x - c) / 0.14, 2));
  return (x + bell(0.88, h) + bell(0.62, l) + bell(0.37, d) + bell(0.12, s)).clamp(0.0, 1.0);
}

class CurvePainter extends CustomPainter {
  CurvePainter(this.a, this.h, this.l, this.d, this.s, this.stroke, this.fill, this.grid, this.dash,
      {this.sel = -1, this.dot = Colors.white, this.showPoints = false});
  final Analysis a;
  final double h, l, d, s;
  final Color stroke, fill, grid, dash, dot;

  /// Index (0 = highlights .. 3 = shadows) of the selected point, -1 = none.
  final int sel;
  final bool showPoints;

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width, ht = size.height;
    final hist = a.hist;
    final mx = hist.fold<double>(0, math.max);
    if (mx > 0) {
      final area = Path()..moveTo(0, ht);
      for (var i = 0; i < hist.length; i++) {
        area.lineTo(i / (hist.length - 1) * w, ht - hist[i] / mx * ht * 0.95);
      }
      area
        ..lineTo(w, ht)
        ..close();
      canvas.drawPath(area, Paint()..color = fill);
    }
    final gp = Paint()
      ..color = grid
      ..strokeWidth = 1;
    for (var i = 1; i < 4; i++) {
      canvas.drawLine(Offset(w * i / 4, 0), Offset(w * i / 4, ht), gp);
      canvas.drawLine(Offset(0, ht * i / 4), Offset(w, ht * i / 4), gp);
    }
    final dp = Paint()
      ..color = dash
      ..strokeWidth = 1;
    for (double t = 0; t < 1; t += 0.04) {
      canvas.drawLine(Offset(t * w, ht - t * ht), Offset((t + 0.02) * w, ht - (t + 0.02) * ht), dp);
    }
    final path = Path();
    for (var i = 0; i <= 48; i++) {
      final x = i / 48;
      final y = curveY(x, h, l, d, s);
      final p = Offset(x * w, ht - y * (ht - 6) - 3);
      if (i == 0) {
        path.moveTo(p.dx, p.dy);
      } else {
        path.lineTo(p.dx, p.dy);
      }
    }
    canvas.drawPath(
        path,
        Paint()
          ..color = stroke
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2.5
          ..strokeCap = StrokeCap.round
          ..strokeJoin = StrokeJoin.round);
    if (showPoints) {
      const xs = [0.88, 0.62, 0.37, 0.12];
      for (var i = 0; i < 4; i++) {
        final p = Offset(xs[i] * w, ht - curveY(xs[i], h, l, d, s) * (ht - 6) - 3);
        final big = i == sel;
        canvas.drawCircle(p, big ? 7 : 4, Paint()..color = dot);
        canvas.drawCircle(
            p,
            big ? 7 : 4,
            Paint()
              ..style = PaintingStyle.stroke
              ..strokeWidth = 2
              ..color = stroke);
      }
    }
  }

  @override
  bool shouldRepaint(CurvePainter o) => true;
}
