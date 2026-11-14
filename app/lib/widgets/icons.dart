// Hand-drawn line icons (guidelines section 8): 24x24 canvas, stroke 2, round caps.
// One painter for the whole set so iOS / Android share the same glyphs.
import 'dart:math' as math;

import 'package:flutter/material.dart';

enum DI {
  photos,
  styles,
  settings,
  light,
  color,
  curve,
  effects,
  detail,
  grade,
  crop,
  back,
  forward,
  down,
  up,
  undo,
  redo,
  share,
  more,
  compare,
  sideBySide,
  eye,
  select,
  save,
  merge,
  publish,
  reset,
  flip,
  rotateL,
  rotateR,
  camera,
  spinner,
  star,
  play,
  check,
  close,
  plus,
  warn,
  // added for 局部 / 人像 and the import sheet (audit 2026-11 F10, F24)
  minus,
  folder,
  addPart,
  subPart,
  invert,
  duplicate,
  trash,
  removePart,
  strokeAdd,
  strokeErase,
  eraser,
  skin,
  underEye,
  eyes,
  shine,
  teeth,
  lips,
  faceLight,
}

class DIcon extends StatelessWidget {
  const DIcon(this.icon, {super.key, this.size = 24, this.color, this.stroke = 2, this.filled = false});
  final DI icon;
  final double size;
  final Color? color;
  final double stroke;

  /// Selected state in the navigation bar (fills the shape).
  final bool filled;

  @override
  Widget build(BuildContext context) {
    final c = color ?? IconTheme.of(context).color ?? Colors.black;
    return ExcludeSemantics(
      child: SizedBox(
        width: size,
        height: size,
        child: CustomPaint(painter: _IconPainter(icon, c, stroke, filled)),
      ),
    );
  }
}

class _IconPainter extends CustomPainter {
  _IconPainter(this.icon, this.color, this.stroke, this.filled);
  final DI icon;
  final Color color;
  final double stroke;
  final bool filled;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.scale(size.width / 24, size.height / 24);
    final line = Paint()
      ..color = color
      ..style = PaintingStyle.stroke
      ..strokeWidth = stroke
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round;
    final fill = Paint()
      ..color = color
      ..style = PaintingStyle.fill;
    final soft = Paint()
      ..color = color.withValues(alpha: 0.45)
      ..style = PaintingStyle.fill;

    Path poly(List<double> p, {bool close = false}) {
      final path = Path()..moveTo(p[0], p[1]);
      for (var i = 2; i < p.length; i += 2) {
        path.lineTo(p[i], p[i + 1]);
      }
      if (close) path.close();
      return path;
    }

    void rr(double x, double y, double w, double h, double r, {bool fillIt = false}) {
      final rect = RRect.fromRectAndRadius(Rect.fromLTWH(x, y, w, h), Radius.circular(r));
      canvas.drawRRect(rect, fillIt || filled ? fill : line);
    }

    switch (icon) {
      case DI.photos:
        rr(3.5, 3.5, 7.5, 7.5, 2);
        rr(13, 3.5, 7.5, 7.5, 2);
        rr(3.5, 13, 7.5, 7.5, 2);
        rr(13, 13, 7.5, 7.5, 2);
        break;
      case DI.styles:
        for (final o in const [Offset(12, 8.5), Offset(8.2, 15), Offset(15.8, 15)]) {
          if (filled) canvas.drawCircle(o, 5, soft);
          canvas.drawCircle(o, 5, line);
        }
        break;
      case DI.settings:
        if (filled) canvas.drawCircle(const Offset(12, 12), 3.2, fill);
        canvas.drawCircle(const Offset(12, 12), 3.2, line);
        for (var i = 0; i < 8; i++) {
          final a = i * math.pi / 4;
          canvas.drawLine(Offset(12 + 6.6 * math.cos(a), 12 + 6.6 * math.sin(a)),
              Offset(12 + 9.6 * math.cos(a), 12 + 9.6 * math.sin(a)), line);
        }
        break;
      case DI.light:
        canvas.drawCircle(const Offset(12, 12), 4, line);
        for (var i = 0; i < 8; i++) {
          final a = i * math.pi / 4;
          canvas.drawLine(Offset(12 + 7 * math.cos(a), 12 + 7 * math.sin(a)),
              Offset(12 + 9.5 * math.cos(a), 12 + 9.5 * math.sin(a)), line);
        }
        break;
      case DI.color:
        final d = Path()
          ..moveTo(12, 3)
          ..cubicTo(12, 3, 5.5, 10, 5.5, 14.5)
          ..arcToPoint(const Offset(18.5, 14.5), radius: const Radius.circular(6.5), clockwise: false)
          ..cubicTo(18.5, 10, 12, 3, 12, 3)
          ..close();
        canvas.drawPath(d, line);
        break;
      case DI.curve:
        canvas.drawPath(poly([4, 3.5, 4, 20, 20.5, 20]), line);
        canvas.drawPath(
            Path()
              ..moveTo(7, 16)
              ..cubicTo(11, 15, 11, 8, 20, 7),
            line);
        break;
      case DI.effects:
        Path star(double cx, double cy, double r) => Path()
          ..moveTo(cx, cy - r)
          ..quadraticBezierTo(cx, cy, cx + r, cy)
          ..quadraticBezierTo(cx, cy, cx, cy + r)
          ..quadraticBezierTo(cx, cy, cx - r, cy)
          ..quadraticBezierTo(cx, cy, cx, cy - r)
          ..close();
        canvas.drawPath(star(10, 11, 7), line);
        canvas.drawPath(star(18, 18, 3), fill);
        break;
      case DI.detail:
        canvas.drawPath(poly([12, 3.5, 21, 19.5, 3, 19.5], close: true), line);
        canvas.drawPath(poly([12, 10.5, 15.6, 16.8, 8.4, 16.8], close: true), line);
        break;
      case DI.grade:
        canvas.drawCircle(const Offset(12, 12), 9, line);
        for (final y in const [8.0, 12.0, 16.0]) {
          final dx = math.sqrt(81 - (y - 12) * (y - 12));
          canvas.drawLine(Offset(12 - dx, y), Offset(12 + dx, y), line);
        }
        break;
      case DI.crop:
        canvas.drawPath(poly([6.5, 2.5, 6.5, 17.5, 21.5, 17.5]), line);
        canvas.drawPath(poly([2.5, 6.5, 17.5, 6.5, 17.5, 21.5]), line);
        break;
      case DI.back:
        canvas.drawPath(poly([15, 5, 8, 12, 15, 19]), line);
        break;
      case DI.forward:
        canvas.drawPath(poly([9, 5, 16, 12, 9, 19]), line);
        break;
      case DI.down:
        canvas.drawPath(poly([5, 9, 12, 16, 19, 9]), line);
        break;
      case DI.up:
        canvas.drawPath(poly([5, 15, 12, 8, 19, 15]), line);
        break;
      case DI.undo:
      case DI.redo:
        if (icon == DI.redo) {
          canvas.translate(24, 0);
          canvas.scale(-1, 1);
        }
        canvas.drawPath(poly([8.5, 4.5, 4, 9, 8.5, 13.5]), line);
        canvas.drawPath(
            Path()
              ..moveTo(4.5, 9)
              ..lineTo(14, 9)
              ..arcToPoint(const Offset(14, 19), radius: const Radius.circular(5), clockwise: true)
              ..lineTo(10, 19),
            line);
        break;
      case DI.share:
        canvas.drawLine(const Offset(12, 15), const Offset(12, 3.5), line);
        canvas.drawPath(poly([7.5, 8, 12, 3.5, 16.5, 8]), line);
        canvas.drawPath(poly([6, 11.5, 5, 11.5, 5, 20, 19, 20, 19, 11.5, 18, 11.5]), line);
        break;
      case DI.more:
        for (final x in const [5.0, 12.0, 19.0]) {
          canvas.drawCircle(Offset(x, 12), 1.8, fill);
        }
        break;
      case DI.compare:
        rr(3, 5, 18, 14, 3);
        canvas.drawLine(const Offset(12, 5), const Offset(12, 19), line);
        canvas.drawLine(const Offset(7.5, 10), const Offset(7.5, 14), line);
        canvas.drawLine(const Offset(16.5, 10), const Offset(16.5, 14), line);
        break;
      case DI.sideBySide:
        rr(3, 5, 18, 14, 3);
        canvas.drawLine(const Offset(12, 5), const Offset(12, 19), line);
        break;
      case DI.eye:
        canvas.drawPath(
            Path()
              ..moveTo(2.5, 12)
              ..cubicTo(6, 5.5, 18, 5.5, 21.5, 12)
              ..cubicTo(18, 18.5, 6, 18.5, 2.5, 12),
            line);
        canvas.drawCircle(const Offset(12, 12), 3, line);
        break;
      case DI.select:
        rr(3.5, 3.5, 17, 17, 4);
        canvas.drawPath(poly([8, 12.5, 11, 15.5, 16.5, 9]), line);
        break;
      case DI.save:
        canvas.drawLine(const Offset(12, 3.5), const Offset(12, 15), line);
        canvas.drawPath(poly([7.5, 10.5, 12, 15, 16.5, 10.5]), line);
        canvas.drawLine(const Offset(5, 20), const Offset(19, 20), line);
        break;
      case DI.merge:
        rr(3.5, 3.5, 11, 11, 2.5);
        rr(9.5, 9.5, 11, 11, 2.5);
        break;
      case DI.publish:
        canvas.drawPath(poly([21, 3, 10, 14]), line);
        canvas.drawPath(poly([21, 3, 14.5, 21, 10, 14, 3, 9.5], close: true), line);
        break;
      case DI.reset:
        final arc = Path()..addArc(Rect.fromCircle(center: const Offset(12, 12), radius: 7.5), -math.pi * .75, -math.pi * 1.55);
        canvas.drawPath(arc, line);
        canvas.drawPath(poly([3.5, 4.5, 5.5, 9.5, 10.5, 8]), line);
        break;
      case DI.flip:
        canvas.drawLine(const Offset(12, 3.5), const Offset(12, 20.5), line);
        canvas.drawPath(poly([9, 7, 4, 17, 9, 17], close: true), line);
        canvas.drawPath(poly([15, 7, 20, 17, 15, 17], close: true), line);
        break;
      case DI.rotateL:
      case DI.rotateR:
        if (icon == DI.rotateR) {
          canvas.translate(24, 0);
          canvas.scale(-1, 1);
        }
        canvas.drawPath(Path()..addArc(Rect.fromCircle(center: const Offset(12, 13), radius: 7), -math.pi * .5, math.pi * 1.5), line);
        canvas.drawPath(poly([9, 2.5, 12, 6, 8.5, 8.5]), line);
        break;
      case DI.camera:
        final p = Paint()
          ..color = color
          ..style = PaintingStyle.stroke
          ..strokeWidth = stroke * .8
          ..strokeCap = StrokeCap.round
          ..strokeJoin = StrokeJoin.round;
        canvas.drawRRect(RRect.fromRectAndRadius(const Rect.fromLTWH(2.5, 6.5, 19, 13.5), const Radius.circular(3)), p);
        canvas.drawPath(poly([8, 6.5, 9.5, 4, 14.5, 4, 16, 6.5]), p);
        canvas.drawCircle(const Offset(12, 13.2), 3.8, p);
        break;
      case DI.spinner:
        final p = Paint()
          ..color = color
          ..style = PaintingStyle.stroke
          ..strokeWidth = 3
          ..strokeCap = StrokeCap.round;
        canvas.drawArc(Rect.fromCircle(center: const Offset(12, 12), radius: 8.5), -math.pi / 2, math.pi * 1.5, false, p);
        break;
      case DI.star:
        final path = Path();
        for (var i = 0; i < 10; i++) {
          final r = i.isEven ? 9.5 : 4.2;
          final a = -math.pi / 2 + i * math.pi / 5;
          final pt = Offset(12 + r * math.cos(a), 12.5 + r * math.sin(a));
          if (i == 0) {
            path.moveTo(pt.dx, pt.dy);
          } else {
            path.lineTo(pt.dx, pt.dy);
          }
        }
        path.close();
        canvas.drawPath(path, filled ? fill : line);
        break;
      case DI.play:
        canvas.drawPath(poly([8, 5, 19, 12, 8, 19], close: true), fill);
        break;
      case DI.check:
        canvas.drawPath(poly([4.5, 12.5, 9.5, 17.5, 19.5, 6.5]), line);
        break;
      case DI.close:
        canvas.drawLine(const Offset(5.5, 5.5), const Offset(18.5, 18.5), line);
        canvas.drawLine(const Offset(18.5, 5.5), const Offset(5.5, 18.5), line);
        break;
      case DI.plus:
        canvas.drawLine(const Offset(12, 5), const Offset(12, 19), line);
        canvas.drawLine(const Offset(5, 12), const Offset(19, 12), line);
        break;
      case DI.minus:
        canvas.drawLine(const Offset(5, 12), const Offset(19, 12), line);
        break;
      case DI.folder:
        canvas.drawPath(
            Path()
              ..moveTo(3.5, 7)
              ..arcToPoint(const Offset(5.5, 5), radius: const Radius.circular(2))
              ..lineTo(9.5, 5)
              ..lineTo(11.5, 7.5)
              ..lineTo(18.5, 7.5)
              ..arcToPoint(const Offset(20.5, 9.5), radius: const Radius.circular(2))
              ..lineTo(20.5, 17)
              ..arcToPoint(const Offset(18.5, 19), radius: const Radius.circular(2))
              ..lineTo(5.5, 19)
              ..arcToPoint(const Offset(3.5, 17), radius: const Radius.circular(2))
              ..close(),
            line);
        break;
      case DI.addPart:
      case DI.subPart:
        canvas.drawCircle(const Offset(12, 12), 8.5, line);
        canvas.drawLine(const Offset(8, 12), const Offset(16, 12), line);
        if (icon == DI.addPart) canvas.drawLine(const Offset(12, 8), const Offset(12, 16), line);
        break;
      case DI.invert:
        final box = RRect.fromRectAndRadius(const Rect.fromLTWH(4, 4, 16, 16), const Radius.circular(3));
        canvas.save();
        canvas.clipRRect(box);
        canvas.drawRect(const Rect.fromLTWH(12, 4, 8, 16), fill);
        canvas.restore();
        canvas.drawRRect(box, line);
        break;
      case DI.duplicate:
        rr(8.5, 8.5, 12, 12, 2.5);
        canvas.drawPath(
            Path()
              ..moveTo(15.5, 8.5)
              ..lineTo(15.5, 5.5)
              ..arcToPoint(const Offset(13.5, 3.5), radius: const Radius.circular(2), clockwise: false)
              ..lineTo(5.5, 3.5)
              ..arcToPoint(const Offset(3.5, 5.5), radius: const Radius.circular(2), clockwise: false)
              ..lineTo(3.5, 13.5)
              ..arcToPoint(const Offset(5.5, 15.5), radius: const Radius.circular(2), clockwise: false)
              ..lineTo(8.5, 15.5),
            line);
        break;
      case DI.trash:
        canvas.drawLine(const Offset(4, 6.5), const Offset(20, 6.5), line);
        canvas.drawPath(poly([9, 6.5, 9.5, 3.5, 14.5, 3.5, 15, 6.5]), line);
        canvas.drawPath(poly([6, 6.5, 7, 20, 17, 20, 18, 6.5]), line);
        canvas.drawLine(const Offset(10, 10.5), const Offset(10, 16), line);
        canvas.drawLine(const Offset(14, 10.5), const Offset(14, 16), line);
        break;
      case DI.removePart:
        rr(3.5, 7.5, 12, 12, 3);
        canvas.drawLine(const Offset(16, 3.5), const Offset(20.5, 8), line);
        canvas.drawLine(const Offset(20.5, 3.5), const Offset(16, 8), line);
        break;
      case DI.strokeAdd:
      case DI.strokeErase:
        canvas.drawPath(
            Path()
              ..moveTo(3.5, 18)
              ..cubicTo(6, 11, 8.5, 21, 11.5, 15)
              ..cubicTo(13, 12, 14.5, 14, 15.5, 16),
            line);
        canvas.drawLine(const Offset(14.5, 7), const Offset(20.5, 7), line);
        if (icon == DI.strokeAdd) canvas.drawLine(const Offset(17.5, 4), const Offset(17.5, 10), line);
        break;
      case DI.eraser:
        canvas.save();
        canvas.translate(12, 12);
        canvas.rotate(-math.pi / 4);
        final r = RRect.fromRectAndRadius(Rect.fromCenter(center: Offset.zero, width: 18, height: 9), const Radius.circular(2.5));
        canvas.save();
        canvas.clipRRect(r);
        canvas.drawRect(const Rect.fromLTWH(-9, -4.5, 7, 9), fill);
        canvas.restore();
        canvas.drawRRect(r, line);
        canvas.restore();
        break;
      case DI.skin:
        // a face with a small four-point sparkle: smooth skin
        canvas.drawOval(Rect.fromCenter(center: const Offset(10, 13.5), width: 12, height: 15), line);
        canvas.drawPath(
            Path()
              ..moveTo(18.5, 2)
              ..quadraticBezierTo(18.5, 5.5, 22, 5.5)
              ..quadraticBezierTo(18.5, 5.5, 18.5, 9)
              ..quadraticBezierTo(18.5, 5.5, 15, 5.5)
              ..quadraticBezierTo(18.5, 5.5, 18.5, 2)
              ..close(),
            fill);
        break;
      case DI.underEye:
        canvas.drawPath(
            Path()
              ..moveTo(3, 10)
              ..cubicTo(7, 4.5, 17, 4.5, 21, 10)
              ..cubicTo(17, 15.5, 7, 15.5, 3, 10),
            line);
        canvas.drawCircle(const Offset(12, 10), 2.4, fill);
        canvas.drawPath(
            Path()
              ..moveTo(6, 18.5)
              ..quadraticBezierTo(12, 21.5, 18, 18.5),
            line);
        break;
      case DI.eyes:
        canvas.drawPath(
            Path()
              ..moveTo(2.5, 12)
              ..cubicTo(6, 5.5, 18, 5.5, 21.5, 12)
              ..cubicTo(18, 18.5, 6, 18.5, 2.5, 12),
            line);
        canvas.drawCircle(const Offset(12, 12), 3.6, line);
        canvas.drawCircle(const Offset(13.4, 10.6), 1.4, fill);
        break;
      case DI.shine:
        canvas.drawCircle(const Offset(12, 12), 8.5, line);
        canvas.drawArc(Rect.fromCircle(center: const Offset(12, 12), radius: 5), math.pi * 1.08, math.pi * 0.42, false, line);
        canvas.drawCircle(const Offset(15.5, 15.5), 1.2, fill);
        break;
      case DI.teeth:
        canvas.drawPath(
            Path()
              ..moveTo(4, 9)
              ..lineTo(20, 9)
              ..cubicTo(20, 15.5, 16.5, 19, 12, 19)
              ..cubicTo(7.5, 19, 4, 15.5, 4, 9)
              ..close(),
            line);
        canvas.drawLine(const Offset(4.6, 12.5), const Offset(19.4, 12.5), line);
        canvas.drawLine(const Offset(9.5, 9), const Offset(9.5, 12.5), line);
        canvas.drawLine(const Offset(14.5, 9), const Offset(14.5, 12.5), line);
        break;
      case DI.lips:
        canvas.drawPath(
            Path()
              ..moveTo(3, 12)
              ..cubicTo(5.5, 8, 8.5, 6.5, 10.5, 8)
              ..quadraticBezierTo(12, 9, 13.5, 8)
              ..cubicTo(15.5, 6.5, 18.5, 8, 21, 12)
              ..cubicTo(17.5, 17.5, 6.5, 17.5, 3, 12)
              ..close(),
            line);
        canvas.drawPath(
            Path()
              ..moveTo(3, 12)
              ..quadraticBezierTo(12, 13.8, 21, 12),
            line);
        break;
      case DI.faceLight:
        canvas.drawOval(Rect.fromCenter(center: const Offset(9, 13), width: 11, height: 14), line);
        canvas.drawLine(const Offset(16.5, 7.5), const Offset(20, 5.5), line);
        canvas.drawLine(const Offset(17.5, 12.5), const Offset(21.5, 12.5), line);
        canvas.drawLine(const Offset(16.5, 17.5), const Offset(20, 19.5), line);
        break;
      case DI.warn:
        canvas.drawCircle(const Offset(12, 12), 9.5, line);
        canvas.drawLine(const Offset(12, 7.5), const Offset(12, 13), line);
        canvas.drawCircle(const Offset(12, 16.4), 1.1, fill);
        break;
    }
  }

  @override
  bool shouldRepaint(_IconPainter o) => o.icon != icon || o.color != color || o.filled != filled || o.stroke != stroke;
}
