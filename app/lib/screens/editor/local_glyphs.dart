// Line glyphs of the area kinds (docs/design/local-adjustments.md): the shape card, the area chips, the dock item and the tool strip
// draw the same small pictures, so a kind is recognised by its shape, not by a word. Drawn with Path on the given rectangle.
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../../model/local_kinds.dart';

void paintLocalGlyph(Canvas canvas, Rect r, LocalGlyph g, Color color, {double stroke = 1.6}) {
  final line = Paint()
    ..style = PaintingStyle.stroke
    ..strokeWidth = stroke
    ..strokeCap = StrokeCap.round
    ..strokeJoin = StrokeJoin.round
    ..color = color;
  final fill = Paint()..color = color;
  final c = r.center;
  final s = r.shortestSide;
  switch (g) {
    case LocalGlyph.linear:
      // full line, then fading bands down to the dashed "none" line
      final box = Rect.fromCenter(center: c, width: s * 0.86, height: s * 0.86);
      final top = box.top + s * 0.12, bottom = box.bottom - s * 0.12;
      canvas.drawLine(Offset(box.left, top), Offset(box.right, top), line);
      for (var i = 1; i <= 3; i++) {
        final y = top + (bottom - top) * i / 4;
        canvas.drawLine(Offset(box.left + s * 0.06, y), Offset(box.right - s * 0.06, y), Paint()
          ..strokeWidth = stroke
          ..strokeCap = StrokeCap.round
          ..color = color.withValues(alpha: 1 - i * 0.22));
      }
      _dashed(canvas, Offset(box.left, bottom), Offset(box.right, bottom), line, s * 0.09);
    case LocalGlyph.radial:
      canvas.drawOval(Rect.fromCenter(center: c, width: s * 0.5, height: s * 0.42), line);
      final outer = Path()..addOval(Rect.fromCenter(center: c, width: s * 0.88, height: s * 0.74));
      for (final m in outer.computeMetrics()) {
        var d = 0.0;
        while (d < m.length) {
          canvas.drawPath(m.extractPath(d, math.min(d + s * 0.08, m.length)), line);
          d += s * 0.15;
        }
      }
    case LocalGlyph.brush:
      final p = Path()
        ..moveTo(r.left + s * 0.14, r.bottom - s * 0.3)
        ..cubicTo(r.left + s * 0.3, r.top + s * 0.1, r.left + s * 0.5, r.bottom - s * 0.05, r.left + s * 0.62, r.top + s * 0.42)
        ..cubicTo(r.left + s * 0.7, r.top + s * 0.2, r.left + s * 0.82, r.top + s * 0.24, r.right - s * 0.12, r.top + s * 0.18);
      canvas.drawPath(p, Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = s * 0.16
        ..strokeCap = StrokeCap.round
        ..color = color.withValues(alpha: 0.35));
      canvas.drawPath(p, line);
    case LocalGlyph.luminance:
      // a tone bar from dark to light with the picked band raised
      final bar = Rect.fromCenter(center: c.translate(0, s * 0.12), width: s * 0.86, height: s * 0.26);
      final rr = RRect.fromRectAndRadius(bar, Radius.circular(s * 0.08));
      canvas.drawRRect(rr, Paint()..shader = LinearGradient(colors: [color, color.withValues(alpha: 0.08)]).createShader(bar));
      canvas.drawRRect(rr, line..strokeWidth = stroke * 0.8);
      final band = Rect.fromLTRB(bar.left + bar.width * 0.55, bar.top - s * 0.2, bar.left + bar.width * 0.82, bar.bottom);
      canvas.drawLine(Offset(band.left, band.top), Offset(band.left, bar.top), line..strokeWidth = stroke);
      canvas.drawLine(Offset(band.right, band.top), Offset(band.right, bar.top), line);
      canvas.drawLine(Offset(band.left, band.top), Offset(band.right, band.top), line);
    case LocalGlyph.color:
      // a drop with three tints: colour explains itself
      final drop = Path()
        ..moveTo(c.dx, r.top + s * 0.08)
        ..cubicTo(c.dx + s * 0.34, r.top + s * 0.42, c.dx + s * 0.34, r.bottom - s * 0.1, c.dx, r.bottom - s * 0.08)
        ..cubicTo(c.dx - s * 0.34, r.bottom - s * 0.1, c.dx - s * 0.34, r.top + s * 0.42, c.dx, r.top + s * 0.08)
        ..close();
      canvas.save();
      canvas.clipPath(drop);
      final b = drop.getBounds();
      const tints = [Color(0xFFE5574C), Color(0xFFE8B23A), Color(0xFF3F86E0)];
      for (var i = 0; i < 3; i++) {
        canvas.drawRect(Rect.fromLTWH(b.left + b.width * i / 3, b.top, b.width / 3 + 0.5, b.height), Paint()..color = tints[i].withValues(alpha: 0.85));
      }
      canvas.restore();
      canvas.drawPath(drop, line);
    case LocalGlyph.subject:
    case LocalGlyph.person:
      canvas.drawCircle(c.translate(0, -s * 0.2), s * 0.15, line);
      final body = Path()
        ..moveTo(c.dx - s * 0.32, r.bottom - s * 0.1)
        ..quadraticBezierTo(c.dx, r.top + s * 0.28, c.dx + s * 0.32, r.bottom - s * 0.1);
      canvas.drawPath(body, line);
    case LocalGlyph.sky:
      final cloud = Path()
        ..moveTo(r.left + s * 0.18, c.dy + s * 0.16)
        ..arcToPoint(Offset(c.dx - s * 0.08, c.dy - s * 0.06), radius: Radius.circular(s * 0.16))
        ..arcToPoint(Offset(c.dx + s * 0.22, c.dy - s * 0.02), radius: Radius.circular(s * 0.2))
        ..arcToPoint(Offset(r.right - s * 0.16, c.dy + s * 0.16), radius: Radius.circular(s * 0.14))
        ..close();
      canvas.drawPath(cloud, line);
    case LocalGlyph.depth:
      for (var i = 0; i < 3; i++) {
        final w = s * (0.8 - i * 0.2);
        canvas.drawLine(Offset(c.dx - w / 2, r.bottom - s * (0.18 + i * 0.22)), Offset(c.dx + w / 2, r.bottom - s * (0.18 + i * 0.22)), line);
      }
    case LocalGlyph.raster:
      canvas.drawRRect(RRect.fromRectAndRadius(Rect.fromCenter(center: c, width: s * 0.7, height: s * 0.7), Radius.circular(s * 0.14)), line);
      canvas.drawCircle(c, s * 0.12, fill);
  }
}

void _dashed(Canvas canvas, Offset a, Offset b, Paint p, double dash) {
  final d = b - a;
  final n = d.distance;
  if (n <= 0) return;
  final u = d / n;
  var t = 0.0;
  while (t < n) {
    canvas.drawLine(a + u * t, a + u * math.min(n, t + dash), p);
    t += dash * 2;
  }
}

/// A glyph as a widget.
class LocalGlyphIcon extends StatelessWidget {
  const LocalGlyphIcon(this.glyph, {super.key, this.size = 24, required this.color});
  final LocalGlyph glyph;
  final double size;
  final Color color;

  @override
  Widget build(BuildContext context) => SizedBox(width: size, height: size, child: CustomPaint(painter: _GlyphPainter(glyph, color)));
}

class _GlyphPainter extends CustomPainter {
  const _GlyphPainter(this.g, this.color);
  final LocalGlyph g;
  final Color color;
  @override
  void paint(Canvas canvas, Size size) => paintLocalGlyph(canvas, Offset.zero & size, g, color, stroke: math.max(1.3, size.shortestSide / 15));
  @override
  bool shouldRepaint(_GlyphPainter o) => o.g != g || o.color != color;
}
