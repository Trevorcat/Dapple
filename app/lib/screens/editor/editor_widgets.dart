// Editor building blocks: histogram card, 3x3 tone grid, palette pad, curve chart.
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import '../../model/params.dart';
import '../../models.dart';
import '../../services/editor_engine.dart';
import '../../theme/tokens.dart';
import '../../widgets/photo_art.dart';

// ------------------------------------------------------------------ histogram

class HistPainter extends CustomPainter {
  HistPainter(this.a, this.fill, this.line, this.zoneA, this.accent, {this.zones = true});
  final Analysis a;
  final Color fill, line, zoneA, accent;
  final bool zones;

  @override
  void paint(Canvas canvas, Size size) {
    final w = size.width, h = size.height;
    if (zones) {
      final bw = w / 11;
      for (var i = 0; i < 11; i += 2) {
        canvas.drawRect(Rect.fromLTWH(i * bw, 0, bw, h), Paint()..color = zoneA);
      }
    }
    final hist = a.hist;
    final mx = hist.fold<double>(0, math.max);
    if (mx > 0) {
      final pts = [
        for (var i = 0; i < hist.length; i++) Offset(i / (hist.length - 1) * w, h - hist[i] / mx * h * 0.92 - 1)
      ];
      final top = Path()..moveTo(pts.first.dx, pts.first.dy);
      for (var i = 1; i < pts.length - 1; i++) {
        final m = (pts[i] + pts[i + 1]) / 2;
        top.quadraticBezierTo(pts[i].dx, pts[i].dy, m.dx, m.dy);
      }
      top.lineTo(pts.last.dx, pts.last.dy);
      final area = Path.from(top)
        ..lineTo(w, h)
        ..lineTo(0, h)
        ..close();
      canvas.drawPath(area, Paint()..color = fill);
      canvas.drawPath(
          top,
          Paint()
            ..color = line
            ..style = PaintingStyle.stroke
            ..strokeWidth = 1.5
            ..strokeJoin = StrokeJoin.round);
    }
    if (zones) {
      final p = Paint()
        ..color = accent
        ..strokeWidth = 1.5;
      for (final q in [a.p5, a.p50, a.p95]) {
        final x = (q * w).clamp(0.75, w - 0.75);
        for (double y = 0; y < h; y += 6) {
          canvas.drawLine(Offset(x, y), Offset(x, math.min(y + 3, h)), p);
        }
      }
    }
  }

  @override
  bool shouldRepaint(HistPainter o) => o.a != a || o.fill != fill;
}

/// Histogram card (requirements 4.6.1). [mini] = the 40-high variant of the 光线 group.
class HistogramCard extends StatelessWidget {
  const HistogramCard({super.key, required this.analysis, this.target, this.large = false});
  final Analysis analysis;

  /// Chosen tone target (shows "→ target" when it differs from the nearest class).
  final String? target;
  final bool large;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final nearest = analysis.nearest;
    return Semantics(
      image: true,
      label: '直方图，${tonePlain(nearest)}',
      child: Container(
        padding: const EdgeInsets.all(12),
        decoration: BoxDecoration(color: c.bg, borderRadius: BorderRadius.circular(Rd.card)),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
          Row(children: [
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
              decoration: BoxDecoration(color: c.chip, borderRadius: BorderRadius.circular(8)),
              child: Text(tonePlain(nearest), key: const ValueKey('hist-nearest'), style: T.small(c.text, w: FontWeight.w500)),
            ),
            if (target != null && target != nearest) ...[
              const SizedBox(width: 8),
              Text('→ ${tonePlain(target!)}', key: const ValueKey('hist-target'), style: T.small(c.accent, w: FontWeight.w500)),
            ],
          ]),
          const SizedBox(height: 8),
          SizedBox(
            height: large ? 50 : 56,
            width: double.infinity,
            child: CustomPaint(painter: HistPainter(analysis, c.histFill, c.text, c.zoneA, c.accent)),
          ),
          const SizedBox(height: 8),
          Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, children: [
            Text('暗部', style: T.mini(c.text2)),
            Text('中间调', style: T.mini(c.text2)),
            Text('亮部', style: T.mini(c.text2)),
          ]),
        ]),
      ),
    );
  }
}

/// Mini histogram (光线 group): 40 high, radius 8, no zone bars, with the class text on the right.
class MiniHistogram extends StatelessWidget {
  const MiniHistogram({super.key, required this.analysis});
  final Analysis analysis;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      image: true,
      label: '直方图，${tonePlain(analysis.nearest)}',
      child: Row(children: [
        Expanded(
          child: ClipRRect(
            borderRadius: BorderRadius.circular(8),
            child: Container(
              height: 40,
              color: c.bg,
              child: CustomPaint(
                  size: Size.infinite,
                  painter: HistPainter(analysis, c.histFill, c.text, c.zoneA, c.accent, zones: false)),
            ),
          ),
        ),
        const SizedBox(width: 12),
        Text(tonePlain(analysis.nearest), style: T.small(c.text2)),
      ]),
    );
  }
}

// ------------------------------------------------------------------ tone grid

class ToneGrid extends StatelessWidget {
  const ToneGrid({
    super.key,
    required this.photo,
    required this.engine,
    required this.demoEdit,
    required this.nearest,
    required this.selected,
    required this.onSelect,
    this.large = false,
  });
  final Photo photo;
  final EditorEngine engine;
  final DemoEdit demoEdit;
  final String nearest;
  final String selected;
  final ValueChanged<String> onSelect;
  final bool large;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      container: true,
      label: '明暗目标',
      child: LayoutBuilder(builder: (context, cons) {
        const gap = 8.0;
        final cw = (cons.maxWidth - gap * 2) / 3;
        final ch = large ? cw / 1.5 : cw * 0.66;
        final ts = math.max(60.0, demoEdit.edit.toneStrength ?? demoEdit.defaultTone);
        return Column(children: [
          for (final k in toneKeys)
            Padding(
              padding: EdgeInsets.only(bottom: k == 'low' ? 0 : gap),
              child: Row(children: [
                for (final r in toneRanges) ...[
                  if (r != 'short') const SizedBox(width: gap),
                  Builder(builder: (context) {
                    final id = toneClassId(k, r);
                    final sel = id == selected;
                    final tile = engine.tiles[id];
                    Widget art;
                    if (tile != null) {
                      art = RawImage(image: tile, fit: BoxFit.cover);
                    } else if (engine.live) {
                      art = PhotoArt(photo, cacheWidth: 200);
                    } else {
                      final e = DemoEdit(demoEdit.edit.copyWith(toneTarget: id, toneStrength: ts), demoEdit.defaultLook,
                          demoEdit.defaultTone);
                      art = PhotoArt(photo, demoEdit: e);
                    }
                    return Semantics(
                      checked: sel,
                      inMutuallyExclusiveGroup: true,
                      label: '${tonePlain(id)}，${tonePro(id)}${id == nearest ? '，与这张最接近' : ''}',
                      excludeSemantics: true,
                      child: GestureDetector(
                        key: ValueKey('tone-$id'),
                        behavior: HitTestBehavior.opaque,
                        onTap: () => onSelect(id),
                        child: SizedBox(
                          width: cw,
                          height: ch,
                          child: Stack(fit: StackFit.expand, children: [
                            ClipRRect(borderRadius: BorderRadius.circular(large ? 8 : Rd.tile), child: art),
                            Positioned(
                              left: 4,
                              bottom: 4,
                              child: Container(
                                padding: EdgeInsets.symmetric(horizontal: 6, vertical: large ? 2 : 1),
                                decoration: BoxDecoration(color: const Color(0x80000000), borderRadius: BorderRadius.circular(8)),
                                child: large
                                    ? Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
                                        Text(tonePlain(id), style: T.cap(Colors.white, w: FontWeight.w500)),
                                        Text(tonePro(id), style: T.mini(const Color(0xBFFFFFFF))),
                                      ])
                                    : Text(tonePro(id), style: T.mini(Colors.white)),
                              ),
                            ),
                            if (id == nearest)
                              Positioned(
                                right: 6,
                                top: 6,
                                child: Container(
                                  width: 10,
                                  height: 10,
                                  decoration: BoxDecoration(
                                      color: Colors.white,
                                      shape: BoxShape.circle,
                                      border: Border.all(color: const Color(0x73000000), width: 1.5)),
                                ),
                              ),
                            if (sel)
                              IgnorePointer(
                                child: Container(
                                  decoration: BoxDecoration(
                                      borderRadius: BorderRadius.circular(large ? 8 : Rd.tile),
                                      border: Border.all(color: c.accentBtn, width: 2)),
                                ),
                              ),
                          ]),
                        ),
                      ),
                    );
                  }),
                ],
              ]),
            ),
        ]);
      }),
    );
  }
}

// ------------------------------------------------------------------ palette pad

String paletteText(Offset p) {
  final n = ((p.dy - 0.5).abs() * 200).round();
  final m = ((p.dx - 0.5).abs() * 200).round();
  return '${p.dy <= 0.5 ? '偏暖' : '偏冷'} $n，${p.dx >= 0.5 ? '偏浓' : '偏淡'} $m';
}

class _PalettePainter extends CustomPainter {
  _PalettePainter(this.top, this.mid, this.bot, this.dot, this.gray);
  final Color top, mid, bot, dot, gray;
  @override
  void paint(Canvas canvas, Size s) {
    final r = Offset.zero & s;
    canvas.drawRect(
        r,
        Paint()
          ..shader = LinearGradient(begin: Alignment.topCenter, end: Alignment.bottomCenter, colors: [top, mid, bot])
              .createShader(r));
    canvas.drawRect(
        r,
        Paint()
          ..shader = LinearGradient(colors: [gray, gray.withValues(alpha: 0)]).createShader(r));
    final p = Paint()..color = dot;
    for (double y = 9; y < s.height; y += 18.5) {
      for (double x = 9; x < s.width; x += 18.5) {
        canvas.drawCircle(Offset(x, y), 1.5, p);
      }
    }
  }

  @override
  bool shouldRepaint(_PalettePainter o) => o.top != top || o.dot != dot;
}

class PalettePad extends StatelessWidget {
  const PalettePad({super.key, required this.value, required this.onChanged, this.size = 148});
  final Offset value;
  final ValueChanged<Offset> onChanged;
  final double size;

  Offset _clamp(Offset o) => Offset(o.dx.clamp(0.04, 0.96), o.dy.clamp(0.04, 0.96));

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    void at(Offset local) => onChanged(_clamp(Offset(local.dx / size, local.dy / size)));
    return Focus(
      onKeyEvent: (n, e) {
        if (e is KeyUpEvent) return KeyEventResult.ignored;
        const st = 0.05;
        final k = e.logicalKey;
        if (k == LogicalKeyboardKey.arrowLeft) {
          onChanged(_clamp(value + const Offset(-st, 0)));
        } else if (k == LogicalKeyboardKey.arrowRight) {
          onChanged(_clamp(value + const Offset(st, 0)));
        } else if (k == LogicalKeyboardKey.arrowUp) {
          onChanged(_clamp(value + const Offset(0, -st)));
        } else if (k == LogicalKeyboardKey.arrowDown) {
          onChanged(_clamp(value + const Offset(0, st)));
        } else {
          return KeyEventResult.ignored;
        }
        return KeyEventResult.handled;
      },
      child: Semantics(
        slider: true,
        label: '调色盘',
        value: paletteText(value),
        child: GestureDetector(
          key: const ValueKey('palette-pad'),
          onPanStart: (d) => at(d.localPosition),
          onPanUpdate: (d) => at(d.localPosition),
          onTapDown: (d) => at(d.localPosition),
          child: Container(
            width: size,
            height: size,
            decoration: BoxDecoration(borderRadius: BorderRadius.circular(Rd.cardLg), border: Border.all(color: c.sep)),
            child: ClipRRect(
              borderRadius: BorderRadius.circular(Rd.cardLg),
              child: Stack(children: [
                Positioned.fill(
                    child: CustomPaint(
                        painter: _PalettePainter(c.paletteTop, c.paletteMid, c.paletteBot, c.text2.withValues(alpha: .25),
                            c.dark ? const Color(0x66888888) : const Color(0x55A0A0A0)))),
                Positioned(
                  left: value.dx * size - 12,
                  top: value.dy * size - 12,
                  child: Container(
                    width: 24,
                    height: 24,
                    decoration: BoxDecoration(
                      color: Colors.white,
                      shape: BoxShape.circle,
                      border: Border.all(color: c.accentBtn, width: 3),
                      boxShadow: const [BoxShadow(color: Color(0x4D000000), blurRadius: 6, offset: Offset(0, 2))],
                    ),
                  ),
                ),
              ]),
            ),
          ),
        ),
      ),
    );
  }
}

// ------------------------------------------------------------------ curve chart

/// Curve of one channel from its four zone sliders (bell shapes, centres .88/.62/.37/.12, amplitude .18).
double curveY(double x, double h, double l, double d, double s) {
  double bell(double c, double v) => v / 100 * 0.18 * math.exp(-math.pow((x - c) / 0.14, 2));
  return (x + bell(0.88, h) + bell(0.62, l) + bell(0.37, d) + bell(0.12, s)).clamp(0.0, 1.0);
}

class CurvePainter extends CustomPainter {
  CurvePainter(this.a, this.h, this.l, this.d, this.s, this.stroke, this.fill, this.grid, this.dash);
  final Analysis a;
  final double h, l, d, s;
  final Color stroke, fill, grid, dash;

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
  }

  @override
  bool shouldRepaint(CurvePainter o) => true;
}
