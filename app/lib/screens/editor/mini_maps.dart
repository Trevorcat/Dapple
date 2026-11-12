// Small pictures used inside the complex editors of the panel (colour ranges, grading): the grading wheel and the colour-range
// dots. They only display state; the operating surface is the panel's own absolute-mapping gesture surface.
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../../model/params.dart';
import '../../theme/tokens.dart';

// ------------------------------------------------------------------ grading wheel

class _WheelPainter extends CustomPainter {
  _WheelPainter(this.hue, this.sat, this.ring);
  final double hue, sat;
  final Color ring;

  @override
  void paint(Canvas canvas, Size s) {
    final c = s.center(Offset.zero);
    final r = s.shortestSide / 2 - 2;
    const stops = 7;
    final colors = [for (var i = 0; i <= stops; i++) HSLColor.fromAHSL(1, (i * 360 / stops) % 360, .85, .55).toColor()];
    final rect = Rect.fromCircle(center: c, radius: r);
    canvas.drawCircle(c, r, Paint()..shader = SweepGradient(colors: colors).createShader(rect));
    canvas.drawCircle(
        c,
        r,
        Paint()
          ..shader = RadialGradient(colors: const [Color(0xFFBDBDBD), Color(0x00BDBDBD)], stops: const [0, 1]).createShader(rect));
    canvas.drawCircle(
        c,
        r,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 1
          ..color = ring);
    final a = hue * math.pi / 180;
    final p = c + Offset(math.cos(a), math.sin(a)) * (sat / 100 * r);
    canvas.drawCircle(p, 7, Paint()..color = Colors.white);
    canvas.drawCircle(
        p,
        7,
        Paint()
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2
          ..color = const Color(0xFF1F5FD1));
  }

  @override
  bool shouldRepaint(_WheelPainter o) => o.hue != hue || o.sat != sat;
}

class WheelMini extends StatelessWidget {
  const WheelMini({super.key, required this.hue, required this.sat, this.size = 88});
  final double hue, sat;
  final double size;

  @override
  Widget build(BuildContext context) => Semantics(
        image: true,
        label: '分级色轮，色相 ${hue.round()}°，饱和度 ${sat.round()}',
        child: SizedBox(width: size, height: size, child: CustomPaint(painter: _WheelPainter(hue, sat, context.c.sep))),
      );
}

// ------------------------------------------------------------------ pickers

/// Colour range dots (颜色 · 色范围).
class BandDots extends StatelessWidget {
  const BandDots({super.key, required this.index, required this.onSelect});
  final int index;
  final ValueChanged<int> onSelect;

  Color _hsl(double h) => HSLColor.fromAHSL(1, h, .8, .52).toColor();

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      container: true,
      label: '色范围',
      child: Wrap(spacing: 2, runSpacing: 2, children: [
        for (var i = 0; i < 8; i++)
          Semantics(
            checked: i == index,
            inMutuallyExclusiveGroup: true,
            label: hslBandNames[i],
            excludeSemantics: true,
            child: GestureDetector(
              key: ValueKey('band-$i'),
              behavior: HitTestBehavior.opaque,
              onTap: () => onSelect(i),
              child: SizedBox(
                width: 40,
                height: 40,
                child: Center(
                  child: Container(
                    width: 28,
                    height: 28,
                    alignment: Alignment.center,
                    decoration: BoxDecoration(shape: BoxShape.circle, border: i == index ? Border.all(color: c.text, width: 2) : null),
                    child: Container(width: 20, height: 20, decoration: BoxDecoration(shape: BoxShape.circle, color: _hsl(hslBandHue[i].toDouble()))),
                  ),
                ),
              ),
            ),
          ),
      ]),
    );
  }
}

