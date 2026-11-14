// The line-icon vocabulary of the control pads (docs/design/editor-panel.md section 4), drawn on a 20 x 20 grid like the
// design prototype's SVG paths: moon / sun = 暗 / 亮, two half-shaded circles (weak, strong) = 反差低 / 高, blue / orange
// dot = 冷 / 暖, outline / filled drop = 淡 / 浓, wave / zigzag = 柔 / 锐, empty / dotted rounded square = 干净 / 颗粒.
import 'package:flutter/widgets.dart';

import '../../model/pad_model.dart' show PadIconKind;

const Color kColdDot = Color(0xFF3F7FE0), kWarmDot = Color(0xFFEE8A1C);

Path _drop() => Path()
  ..moveTo(10, 3)
  ..cubicTo(7.2, 6.8, 5.5, 9.2, 5.5, 11.8)
  ..arcToPoint(const Offset(14.5, 11.8), radius: const Radius.circular(4.5), clockwise: false)
  ..cubicTo(14.5, 9.2, 12.8, 6.8, 10, 3)
  ..close();

Path _moon() => Path()
  ..moveTo(15, 12.2)
  ..arcToPoint(const Offset(7.8, 5), radius: const Radius.circular(6.2))
  ..arcToPoint(const Offset(15, 12.2), radius: const Radius.circular(6.2), largeArc: true, clockwise: false)
  ..close();

Path _sunRays() {
  final p = Path();
  for (final l in const [
    [10.0, 2.5, 10.0, 4.5],
    [10.0, 15.5, 10.0, 17.5],
    [2.5, 10.0, 4.5, 10.0],
    [15.5, 10.0, 17.5, 10.0],
    [4.7, 4.7, 6.1, 6.1],
    [13.9, 13.9, 15.3, 15.3],
    [4.7, 15.3, 6.1, 13.9],
    [13.9, 6.1, 15.3, 4.7],
  ]) {
    p
      ..moveTo(l[0], l[1])
      ..lineTo(l[2], l[3]);
  }
  return p;
}

Path _halfDisc() => Path()
  ..moveTo(10, 3.5)
  ..arcToPoint(const Offset(10, 16.5), radius: const Radius.circular(6.5), clockwise: false)
  ..close();

Path _wave() => Path()
  ..moveTo(2.5, 10)
  ..cubicTo(4.5, 5, 6, 5, 7.5, 10)
  ..cubicTo(9, 15, 10.5, 15, 12.5, 10)
  ..cubicTo(14.5, 5, 15.5, 5, 17.5, 10);

Path _zigzag() => Path()
  ..moveTo(2.5, 13)
  ..lineTo(6.3, 6)
  ..lineTo(10, 13)
  ..lineTo(13.7, 6)
  ..lineTo(17.5, 13);

Path _sparkle() => Path()
  ..moveTo(10, 2.5)
  ..quadraticBezierTo(10.8, 9.2, 17.5, 10)
  ..quadraticBezierTo(10.8, 10.8, 10, 17.5)
  ..quadraticBezierTo(9.2, 10.8, 2.5, 10)
  ..quadraticBezierTo(9.2, 9.2, 10, 2.5)
  ..close();

final RRect _square = RRect.fromRectAndRadius(const Rect.fromLTWH(4, 4, 12, 12), const Radius.circular(3));

const _grainDots = [Offset(7.5, 7.5), Offset(11, 8.2), Offset(8.3, 11), Offset(12.4, 11.6), Offset(10, 13.2), Offset(13, 7)];

/// Paints [kind] into [box] (square) in [color]. [shadow] adds the thin dark outline that keeps a white icon readable on a
/// light field.
void paintPadIcon(Canvas canvas, PadIconKind kind, Rect box, Color color, {bool shadow = false}) {
  canvas.save();
  canvas.translate(box.left, box.top);
  canvas.scale(box.width / 20, box.height / 20);
  void pass(Color c, double extra, MaskFilter? blur) {
    final stroke = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.6 + extra
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round
      ..color = c
      ..maskFilter = blur;
    final fill = Paint()
      ..style = PaintingStyle.fill
      ..color = c
      ..maskFilter = blur;
    final isShadow = extra > 0;
    switch (kind) {
      case PadIconKind.moon:
        canvas.drawPath(_moon(), stroke);
      case PadIconKind.sun:
        canvas.drawCircle(const Offset(10, 10), 3.2, stroke);
        canvas.drawPath(_sunRays(), stroke);
      case PadIconKind.contrastLow:
      case PadIconKind.contrastHigh:
        canvas.drawCircle(const Offset(10, 10), 6.5, stroke);
        canvas.drawPath(_halfDisc(), fill..color = kind == PadIconKind.contrastLow ? c.withValues(alpha: c.a * 0.3) : c);
      case PadIconKind.cold:
        canvas.drawCircle(const Offset(10, 10), 5, fill..color = isShadow ? c : kColdDot);
        if (!isShadow) canvas.drawCircle(const Offset(10, 10), 5.6, stroke..strokeWidth = 1.2..color = const Color(0xF2FFFFFF));
      case PadIconKind.warm:
        canvas.drawCircle(const Offset(10, 10), 5, fill..color = isShadow ? c : kWarmDot);
        if (!isShadow) canvas.drawCircle(const Offset(10, 10), 5.6, stroke..strokeWidth = 1.2..color = const Color(0xF2FFFFFF));
      case PadIconKind.pale:
        canvas.drawPath(_drop(), stroke);
      case PadIconKind.rich:
        canvas.drawPath(_drop(), stroke);
        canvas.drawPath(_drop(), fill);
      case PadIconKind.soft:
        canvas.drawPath(_wave(), stroke..strokeWidth = 1.6 + extra);
      case PadIconKind.sharp:
        canvas.drawPath(_zigzag(), stroke);
      case PadIconKind.clean:
        canvas.drawRRect(_square, stroke);
      case PadIconKind.hazy:
        // 朦胧: three veils of fog, the lower ones shorter
        for (final l in const [
          [3.0, 6.5, 17.0],
          [5.0, 10.0, 15.0],
          [7.0, 13.5, 13.0],
        ]) {
          canvas.drawLine(Offset(l[0], l[1]), Offset(l[2], l[1]), stroke);
        }
      case PadIconKind.clear:
        // 通透: a four-point sparkle
        canvas.drawPath(_sparkle(), stroke);
      case PadIconKind.grain:
        canvas.drawRRect(_square, stroke);
        for (final d in _grainDots) {
          canvas.drawCircle(d, 0.9 + extra / 2, fill);
        }
    }
  }

  if (shadow) {
    pass(const Color(0x59000000), 1.4, const MaskFilter.blur(BlurStyle.normal, 0.9));
  }
  pass(color, 0, null);
  canvas.restore();
}

/// A single pad icon as a widget (the dock glyph legend, the film map edges, docs).
class PadIcon extends StatelessWidget {
  const PadIcon(this.kind, {super.key, this.size = 20, required this.color, this.shadow = false});
  final PadIconKind kind;
  final double size;
  final Color color;
  final bool shadow;

  @override
  Widget build(BuildContext context) =>
      CustomPaint(size: Size.square(size), painter: _IconPainter(kind, color, shadow));
}

class _IconPainter extends CustomPainter {
  const _IconPainter(this.kind, this.color, this.shadow);
  final PadIconKind kind;
  final Color color;
  final bool shadow;

  @override
  void paint(Canvas canvas, Size size) => paintPadIcon(canvas, kind, Offset.zero & size, color, shadow: shadow);

  @override
  bool shouldRepaint(_IconPainter o) => o.kind != kind || o.color != color || o.shadow != shadow;
}
