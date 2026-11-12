// Position maps and pickers shown inside a window for the active discrete / 2D row
// (requirements 4.15.8, guidelines 15.2.6). They only display state; the whole screen is the
// operating surface while one of them is active, so these never own a drag.
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../../editor/param_rows.dart';
import '../../model/catalog.dart';
import '../../model/film_catalog.dart';
import '../../model/params.dart';
import '../../models.dart';
import '../../services/editor_engine.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/photo_art.dart';
import 'editor_widgets.dart';

// ------------------------------------------------------------------ palette

/// 72x72 palette map: the look's own point as a white ring, the cursor as a white dot. No labels.
class PaletteMini extends StatelessWidget {
  const PaletteMini({super.key, required this.pos, required this.def, this.size = 72});
  final Offset pos, def;
  final double size;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      image: true,
      label: '调色盘，${paletteText(pos)}',
      child: ClipRRect(
        borderRadius: BorderRadius.circular(12),
        child: SizedBox(
          width: size,
          height: size,
          child: Stack(children: [
            Positioned.fill(
                child: CustomPaint(
                    painter: PaletteBackdropPainter(c.paletteTop, c.paletteMid, c.paletteBot, c.text2.withValues(alpha: .25),
                        c.dark ? const Color(0x66888888) : const Color(0x55A0A0A0)))),
            Positioned(
              left: def.dx * size - 5,
              top: def.dy * size - 5,
              child: Container(
                  width: 10,
                  height: 10,
                  decoration: BoxDecoration(shape: BoxShape.circle, border: Border.all(color: Colors.white, width: 1.5))),
            ),
            Positioned(
              left: pos.dx * size - 7,
              top: pos.dy * size - 7,
              child: Container(
                width: 14,
                height: 14,
                decoration: BoxDecoration(
                  color: Colors.white,
                  shape: BoxShape.circle,
                  border: Border.all(color: c.accentBtn, width: 2),
                  boxShadow: const [BoxShadow(color: Color(0x4D000000), blurRadius: 3)],
                ),
              ),
            ),
          ]),
        ),
      ),
    );
  }
}

// ------------------------------------------------------------------ tone 3x3

/// 3x3 tone target cells with a preview of this photo under each target. Tap selects a cell.
class ToneMini extends StatelessWidget {
  const ToneMini({super.key, required this.photo, required this.engine, required this.app, required this.edit, required this.nearest, required this.onSelect});
  final Photo photo;
  final EditorEngine engine;
  final AppState app;
  final EditState edit;
  final String nearest;
  final ValueChanged<String> onSelect;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final selected = edit.toneTarget ?? nearest;
    final de = DemoEdit.of(app, photo);
    final ts = math.max(60.0, de.edit.toneStrength ?? de.defaultTone);
    const cell = 20.0, gap = 2.0;
    return Semantics(
      container: true,
      label: '明暗目标',
      child: SizedBox(
        width: cell * 3 + gap * 2,
        height: cell * 3 + gap * 2,
        child: Column(children: [
          for (final k in toneKeys)
            Padding(
              padding: EdgeInsets.only(bottom: k == 'low' ? 0 : gap),
              child: Row(children: [
                for (final r in toneRanges)
                  Padding(
                    padding: EdgeInsets.only(right: r == 'long' ? 0 : gap),
                    child: Builder(builder: (context) {
                      final id = toneClassId(k, r);
                      final sel = id == selected;
                      final tile = engine.tiles[id];
                      Widget art;
                      if (tile != null) {
                        art = RawImage(image: tile, fit: BoxFit.cover);
                      } else if (engine.live) {
                        art = PhotoArt(photo, cacheWidth: 60);
                      } else {
                        art = PhotoArt(photo, cacheWidth: 60, demoEdit: DemoEdit(de.edit.copyWith(toneTarget: id, toneStrength: ts), de.defaultLook, de.defaultTone));
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
                            width: cell,
                            height: cell,
                            child: Stack(fit: StackFit.expand, children: [
                              ClipRRect(borderRadius: BorderRadius.circular(4), child: art),
                              if (id == nearest)
                                Positioned(
                                  right: 2,
                                  top: 2,
                                  child: Container(width: 5, height: 5, decoration: const BoxDecoration(color: Colors.white, shape: BoxShape.circle)),
                                ),
                              if (sel)
                                DecoratedBox(
                                  decoration: BoxDecoration(
                                      borderRadius: BorderRadius.circular(4), border: Border.all(color: c.accentBtn, width: 2)),
                                ),
                              if (sel)
                                Padding(
                                  padding: const EdgeInsets.all(2),
                                  child: DecoratedBox(
                                    decoration: BoxDecoration(
                                        borderRadius: BorderRadius.circular(2), border: Border.all(color: Colors.white, width: 1)),
                                  ),
                                ),
                            ]),
                          ),
                        ),
                      );
                    }),
                  ),
              ]),
            ),
        ]),
      ),
    );
  }
}

// ------------------------------------------------------------------ curve

/// 160x120 curve map: histogram backdrop, 3x3 grid, diagonal, the channel curve and its four points.
class CurveMini extends StatelessWidget {
  const CurveMini({super.key, required this.analysis, required this.edit, required this.channel, required this.selected, required this.onSelect});
  final Analysis analysis;
  final EditState edit;
  final int channel;

  /// 0..3 = highlights, lights, darks, shadows.
  final int selected;
  final ValueChanged<int> onSelect;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    const colors = [Fx.chRgb, Fx.chR, Fx.chG, Fx.chB];
    double v(String r) => edit.value(cvKey(channel, r));
    const xs = [0.88, 0.62, 0.37, 0.12];
    return Semantics(
      image: true,
      label: '曲线',
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTapDown: (d) {
          // nearest point by x (v1: points sit at fixed x)
          final x = d.localPosition.dx / 160;
          var best = 0;
          for (var i = 1; i < 4; i++) {
            if ((xs[i] - x).abs() < (xs[best] - x).abs()) best = i;
          }
          onSelect(best);
        },
        child: ClipRRect(
          borderRadius: BorderRadius.circular(12),
          child: Container(
            width: 160,
            height: 120,
            color: c.bg,
            child: CustomPaint(
              painter: CurvePainter(analysis, v('H'), v('L'), v('D'), v('S'), channel == 0 ? c.text : colors[channel], c.histFill, c.sep,
                  c.text2.withValues(alpha: .5),
                  sel: selected, dot: c.surface, showPoints: true),
            ),
          ),
        ),
      ),
    );
  }
}

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

/// Horizontal strip of look swatches (风格).
class LookStrip extends StatelessWidget {
  const LookStrip({super.key, required this.index, required this.onSelect});
  final int index;
  final ValueChanged<int> onSelect;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return SizedBox(
      height: 72,
      child: ListView.builder(
        scrollDirection: Axis.horizontal,
        itemCount: dLooks.length,
        itemBuilder: (context, i) {
          final l = dLooks[i];
          final on = i == index;
          return Semantics(
            checked: on,
            inMutuallyExclusiveGroup: true,
            label: l.name,
            excludeSemantics: true,
            child: GestureDetector(
              key: ValueKey('look-${l.id}'),
              behavior: HitTestBehavior.opaque,
              onTap: () => onSelect(i),
              child: SizedBox(
                width: 52,
                child: Column(mainAxisSize: MainAxisSize.min, children: [
                  Container(
                    width: 44,
                    height: 44,
                    alignment: Alignment.center,
                    decoration: BoxDecoration(shape: BoxShape.circle, border: on ? Border.all(color: c.accentBtn, width: 2) : null),
                    child: Container(
                      width: 36,
                      height: 36,
                      decoration: BoxDecoration(
                          shape: BoxShape.circle,
                          gradient: LinearGradient(
                              begin: Alignment.topLeft,
                              end: Alignment.bottomRight,
                              colors: l.id == 'teal' ? [l.swatch[0], l.swatch[0], l.swatch[1], l.swatch[1]] : l.swatch,
                              stops: l.id == 'teal' ? const [0, .48, .52, 1] : null)),
                    ),
                  ),
                  const SizedBox(height: 2),
                  Text(l.name, maxLines: 1, style: T.mini(on ? c.text : c.text2, w: on ? FontWeight.w500 : FontWeight.w400)),
                ]),
              ),
            ),
          );
        },
      ),
    );
  }
}

/// Wrap of text chips (curve channel, grading zone, crop ratio).
class ChipPicker extends StatelessWidget {
  const ChipPicker({super.key, required this.labels, required this.index, required this.onSelect, this.dots, required this.keyPrefix, this.label});
  final List<String> labels;
  final int index;
  final ValueChanged<int> onSelect;
  final List<Color>? dots;
  final String keyPrefix;
  final String? label;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      container: true,
      label: label,
      child: Wrap(spacing: 6, runSpacing: 6, children: [
        for (var i = 0; i < labels.length; i++)
          Semantics(
            checked: i == index,
            inMutuallyExclusiveGroup: true,
            label: labels[i],
            excludeSemantics: true,
            child: GestureDetector(
              key: ValueKey('$keyPrefix-$i'),
              behavior: HitTestBehavior.opaque,
              onTap: () => onSelect(i),
              child: Container(
                height: 36,
                padding: const EdgeInsets.symmetric(horizontal: 12),
                decoration: BoxDecoration(
                  color: i == index ? c.accentSoft : c.chip,
                  borderRadius: BorderRadius.circular(18),
                  border: i == index ? Border.all(color: c.accent, width: 1.5) : null,
                ),
                child: Row(mainAxisSize: MainAxisSize.min, children: [
                  if (dots != null) ...[
                    Container(width: 8, height: 8, decoration: BoxDecoration(color: dots![i], shape: BoxShape.circle)),
                    const SizedBox(width: 6),
                  ],
                  Text(labels[i], style: T.small(i == index ? c.accent : c.text, w: i == index ? FontWeight.w500 : FontWeight.w400)),
                ]),
              ),
            ),
          ),
      ]),
    );
  }
}

// ------------------------------------------------------------------ film

/// Horizontal strip of film stocks: 无, then negative / slide / cinema. A swatch is the stock's colour (a
/// gradient of its two tones), never a photo. [onSelect] gets the index of the 胶片 row (0 = 无).
class FilmStrip extends StatefulWidget {
  const FilmStrip({super.key, required this.catalog, required this.selected, required this.onSelect});
  final FilmCatalog catalog;
  final String? selected;
  final ValueChanged<int> onSelect;

  @override
  State<FilmStrip> createState() => _FilmStripState();
}

class _FilmStripState extends State<FilmStrip> {
  static const double chipW = 64, gap = 10;
  final ScrollController _sc = ScrollController();

  /// Scroll offset that brings the selected stock into view (the strip opens on the current choice).
  double _offsetFor(String? id) {
    if (id == null) return 0;
    var x = chipW + gap; // 无
    for (final g in FilmGroup.values) {
      final list = widget.catalog.inGroup(g);
      if (list.isEmpty) continue;
      final i = list.indexWhere((s) => s.id == id);
      if (i >= 0) return math.max(0, i <= 1 ? x - gap : x + i * chipW - chipW); // near the group's start its label stays in view
      x += list.length * chipW + gap;
    }
    return 0;
  }

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || !_sc.hasClients) return;
      _sc.jumpTo(_offsetFor(widget.selected).clamp(0.0, _sc.position.maxScrollExtent).toDouble());
    });
  }

  @override
  void dispose() {
    _sc.dispose();
    super.dispose();
  }

  Widget _chip(BuildContext context,
      {required Key key, required String name, required String semantics, required bool on, required VoidCallback onTap, required Widget swatch}) {
    final c = context.c;
    return Semantics(
      checked: on,
      inMutuallyExclusiveGroup: true,
      label: semantics,
      excludeSemantics: true,
      child: GestureDetector(
        key: key,
        behavior: HitTestBehavior.opaque,
        onTap: onTap,
        child: SizedBox(
          width: chipW,
          child: Column(mainAxisSize: MainAxisSize.min, children: [
            Container(
              width: 44,
              height: 44,
              alignment: Alignment.center,
              decoration: BoxDecoration(shape: BoxShape.circle, border: on ? Border.all(color: c.accentBtn, width: 2) : null),
              child: swatch,
            ),
            const SizedBox(height: 2),
            Text(name,
                maxLines: 2,
                textAlign: TextAlign.center,
                overflow: TextOverflow.ellipsis,
                style: T.mini(on ? c.text : c.text2, w: on ? FontWeight.w500 : FontWeight.w400)),
          ]),
        ),
      ),
    );
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final cat = widget.catalog;
    final sel = widget.selected;
    var index = 1; // row index of the next stock (0 = 无)
    final groups = <Widget>[];
    for (final g in FilmGroup.values) {
      final list = cat.inGroup(g);
      if (list.isEmpty) continue;
      final first = index;
      index += list.length;
      groups.add(Padding(
        padding: const EdgeInsets.only(left: gap),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
          SizedBox(height: 16, child: Text(filmGroupNames[g]!, style: T.cap(c.text2, w: FontWeight.w500))),
          Row(mainAxisSize: MainAxisSize.min, children: [
            for (var i = 0; i < list.length; i++)
              _chip(
                context,
                key: ValueKey('film-stock-${list[i].id}'),
                name: list[i].name,
                semantics: '${list[i].name}，${filmGroupNames[g]}',
                on: list[i].id == sel,
                onTap: () => widget.onSelect(first + i),
                swatch: Container(
                  width: 36,
                  height: 36,
                  decoration: BoxDecoration(
                    shape: BoxShape.circle,
                    gradient: LinearGradient(begin: Alignment.topLeft, end: Alignment.bottomRight, colors: cat.swatch(list[i].id)),
                  ),
                ),
              ),
          ]),
        ]),
      ));
    }
    return SizedBox(
      height: 92,
      child: SingleChildScrollView(
        key: const ValueKey('film-strip'),
        controller: _sc,
        scrollDirection: Axis.horizontal,
        child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Column(mainAxisSize: MainAxisSize.min, children: [
            const SizedBox(height: 16),
            _chip(
              context,
              key: const ValueKey('film-none'),
              name: '无',
              semantics: '无胶片',
              on: cat.stock(sel) == null,
              onTap: () => widget.onSelect(0),
              swatch: Container(
                width: 36,
                height: 36,
                decoration: BoxDecoration(shape: BoxShape.circle, border: Border.all(color: c.text2.withValues(alpha: .6), width: 1.5)),
                child: CustomPaint(painter: _SlashPainter(c.text2.withValues(alpha: .6))),
              ),
            ),
          ]),
          ...groups,
        ]),
      ),
    );
  }
}

class _SlashPainter extends CustomPainter {
  const _SlashPainter(this.color);
  final Color color;
  @override
  void paint(Canvas canvas, Size size) {
    final r = size.width / 2;
    final d = r * 0.7071;
    canvas.drawLine(Offset(r - d, r + d), Offset(r + d, r - d), Paint()
      ..color = color
      ..strokeWidth = 1.5
      ..strokeCap = StrokeCap.round);
  }

  @override
  bool shouldRepaint(_SlashPainter o) => o.color != color;
}

/// Print paper chips of a negative stock: 直接扫描, then the papers. Scrolls sideways.
class FilmPaperStrip extends StatelessWidget {
  const FilmPaperStrip({super.key, required this.labels, required this.index, required this.onSelect});
  final List<String> labels;
  final int index;
  final ValueChanged<int> onSelect;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return SizedBox(
      height: 40,
      child: ListView.separated(
        key: const ValueKey('film-paper-strip'),
        scrollDirection: Axis.horizontal,
        itemCount: labels.length,
        separatorBuilder: (_, _) => const SizedBox(width: 6),
        itemBuilder: (context, i) {
          final on = i == index;
          return Semantics(
            checked: on,
            inMutuallyExclusiveGroup: true,
            label: labels[i],
            excludeSemantics: true,
            child: GestureDetector(
              key: ValueKey('film-paper-$i'),
              behavior: HitTestBehavior.opaque,
              onTap: () => onSelect(i),
              child: Container(
                height: 36,
                alignment: Alignment.center,
                padding: const EdgeInsets.symmetric(horizontal: 12),
                decoration: BoxDecoration(
                  color: on ? c.accentSoft : c.chip,
                  borderRadius: BorderRadius.circular(18),
                  border: on ? Border.all(color: c.accent, width: 1.5) : null,
                ),
                child: Text(labels[i], maxLines: 1, style: T.small(on ? c.accent : c.text, w: on ? FontWeight.w500 : FontWeight.w400)),
              ),
            ),
          );
        },
      ),
    );
  }
}
