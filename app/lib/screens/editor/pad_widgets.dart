// The pad and the strength strip (docs/design/editor-panel.md sections 3 and 4). Both map the finger *absolutely*: the cursor
// sits under the finger, the pad reacts only inside its drawn area, and nothing else on the panel moves while they are
// dragged (only the cursor layer repaints, from the edit tick).
import 'dart:math' as math;
import 'dart:ui' as ui;
import 'dart:ui' show PointMode;

import 'package:flutter/material.dart';
import 'package:flutter/semantics.dart';

import '../../model/catalog.dart';
import '../../model/params.dart' show PadState;
import '../../model/pad_model.dart';
import '../../theme/tokens.dart';
import 'editor_controller.dart';
import 'pad_gestures.dart';
import 'pad_icons.dart';

/// The cursor can reach the pad's edge minus this inset (so it always stays inside the drawn field and under the finger).
const double kPadInset = 22;

/// Pad coordinates (-1..1, y up) of a local position in a field of [size].
Offset padFromLocal(Size size, Offset p) {
  final w = math.max(1.0, size.width / 2 - kPadInset), h = math.max(1.0, size.height / 2 - kPadInset);
  return Offset(((p.dx - size.width / 2) / w).clamp(-1.0, 1.0).toDouble(), (-(p.dy - size.height / 2) / h).clamp(-1.0, 1.0).toDouble());
}

Offset padToLocal(Size size, double x, double y) {
  final w = math.max(1.0, size.width / 2 - kPadInset), h = math.max(1.0, size.height / 2 - kPadInset);
  return Offset(size.width / 2 + x * w, size.height / 2 - y * h);
}

BoxDecoration padFieldDecoration(PadCat cat, bool dark, {double radius = 18}) {
  final r = BorderRadius.circular(radius);
  switch (cat) {
    case PadCat.light:
      return BoxDecoration(borderRadius: r, gradient: const LinearGradient(colors: [Color(0xFF34373D), Color(0xFFF4F5F6)]));
    case PadCat.color:
      return BoxDecoration(
        borderRadius: r,
        gradient: LinearGradient(colors: [const Color(0xFF3D78E2), dark ? const Color(0xFFD9D6D0) : const Color(0xFFE8E5DF), const Color(0xFFEE8F22)], stops: const [0, 0.5, 1]),
      );
    case PadCat.film:
      return BoxDecoration(borderRadius: r, gradient: const LinearGradient(colors: [Color(0xFFDCD6CC), Color(0xFFC08A4E)]));
    case PadCat.texture:
      return BoxDecoration(
        borderRadius: r,
        gradient: LinearGradient(begin: Alignment.topLeft, end: Alignment.bottomRight, colors: dark ? const [Color(0xFF5A5F68), Color(0xFF3F434A)] : const [Color(0xFFBFC3CB), Color(0xFF9EA3AD)]),
      );
  }
}

/// A vertical overlay that explains the second axis where colour itself does (colour pad: fade to grey toward the bottom;
/// film pad: dark toward the bottom, light toward the top).
Widget? padFieldOverlay(PadCat cat, bool dark, double radius) {
  final r = BorderRadius.circular(radius);
  switch (cat) {
    case PadCat.color:
      return DecoratedBox(
          decoration: BoxDecoration(
              borderRadius: r,
              gradient: LinearGradient(begin: Alignment.bottomCenter, end: Alignment.topCenter, colors: [(dark ? const Color(0xFFB8B6B1) : const Color(0xFFDEDCD8)).withValues(alpha: 0.96), (dark ? const Color(0xFFB8B6B1) : const Color(0xFFDEDCD8)).withValues(alpha: 0)], stops: const [0, 0.86])));
    case PadCat.film:
      return DecoratedBox(
          decoration: BoxDecoration(
              borderRadius: r,
              gradient: const LinearGradient(begin: Alignment.bottomCenter, end: Alignment.topCenter, colors: [Color(0x801C1610), Color(0x001C1610), Color(0x00FFFFFF), Color(0x66FFFFFF)], stops: [0, 0.5, 0.5, 1])));
    case PadCat.light:
    case PadCat.texture:
      return null;
  }
}

/// The static part of a pad: the dot grid, the hairline border, the four edge icons and the ring that marks automatic.
class _FieldPainter extends CustomPainter {
  const _FieldPainter(this.cat, this.radius);
  final PadCat cat;
  final double radius;

  @override
  void paint(Canvas canvas, Size size) {
    final rr = RRect.fromRectAndRadius(Offset.zero & size, Radius.circular(radius));
    canvas.save();
    canvas.clipRRect(rr);
    // a fine dot grid
    final dots = <Offset>[];
    for (var x = 8.0; x < size.width; x += 16) {
      for (var y = 8.0; y < size.height; y += 16) {
        dots.add(Offset(x, y));
      }
    }
    canvas.drawPoints(
        PointMode.points,
        dots,
        Paint()
          ..color = const Color(0x55FFFFFF)
          ..strokeWidth = 1.6
          ..strokeCap = StrokeCap.round);
    canvas.restore();
    canvas.drawRRect(rr.deflate(0.5), Paint()..style = PaintingStyle.stroke..strokeWidth = 1..color = const Color(0x14000000));
    // the four direction icons, near the middle of each edge
    final ic = cat.edgeIcons;
    const s = 20.0, m = 20.0;
    final centres = [Offset(m, size.height / 2), Offset(size.width - m, size.height / 2), Offset(size.width / 2, size.height - m), Offset(size.width / 2, m)];
    for (var i = 0; i < 4; i++) {
      paintPadIcon(canvas, ic[i], Rect.fromCenter(center: centres[i], width: s, height: s), Colors.white, shadow: true);
    }
    // automatic: a small ring in the centre
    final c = size.center(Offset.zero);
    canvas.drawCircle(c, 6.5, Paint()..style = PaintingStyle.stroke..strokeWidth = 1..color = const Color(0x4D000000));
    canvas.drawCircle(c, 6, Paint()..style = PaintingStyle.stroke..strokeWidth = 1..color = const Color(0xF2FFFFFF));
  }

  @override
  bool shouldRepaint(_FieldPainter o) => o.cat != cat || o.radius != radius;
}

/// The moving part: the cursor, and the grey-out of the ends the engine says change nothing (film pad).
/// The cursor layer of a pad (public so tests can read the reachable area it dims).
class PadCursorPainter extends CustomPainter {
  PadCursorPainter(this.x, this.y, this.limits, this.radius);
  final double x, y;
  final PadLimits limits;
  final double radius;

  @override
  void paint(Canvas canvas, Size size) {
    if (!limits.isFull) {
      final dead = Paint()..color = const Color(0x40000000);
      canvas.save();
      canvas.clipRRect(RRect.fromRectAndRadius(Offset.zero & size, Radius.circular(radius)));
      final lo = padToLocal(size, limits.xLo, limits.yLo), hi = padToLocal(size, limits.xHi, limits.yHi);
      // everything outside the reachable box is dimmed
      final reach = Rect.fromLTRB(lo.dx, hi.dy, hi.dx, lo.dy);
      final path = Path()
        ..addRect(Offset.zero & size)
        ..addRect(reach)
        ..fillType = PathFillType.evenOdd;
      canvas.drawPath(path, dead);
      canvas.restore();
    }
    final p = padToLocal(size, x, y);
    canvas.drawCircle(p + const Offset(0, 2), 15, Paint()..color = const Color(0x4D000000)..maskFilter = const MaskFilter.blur(BlurStyle.normal, 3));
    canvas.drawCircle(p, 14, Paint()..color = const Color(0x2EFFFFFF));
    canvas.drawCircle(p, 12.5, Paint()..style = PaintingStyle.stroke..strokeWidth = 3..color = Colors.white);
    canvas.drawCircle(p, 14.2, Paint()..style = PaintingStyle.stroke..strokeWidth = 1..color = const Color(0x59000000));
  }

  @override
  bool shouldRepaint(PadCursorPainter o) => o.x != x || o.y != y || o.limits != limits;
}

/// What a pad and a strength strip read and write. The global categories use [CatPadSource]; a local area (局部) supplies its own,
/// so the same pad and strip drive it (docs/design/local-adjustments.md section 4).
abstract class PadSource {
  /// Repaints the cursor / thumb.
  Listenable get changes;
  PadState get view;
  PadLimits get limits;
  void begin();
  void move(double x, double y);
  void strip(double a);
  void end();
  void reset();
  String get readout;

  /// What the strip at [a] reads, in percent of the recommended amount (audit U1); the header readout and the screen reader say it.
  double percent(double a) => a.clamp(0.0, 1.0) * 200;

  /// False: drawn dimmed and ignores the finger (an empty 局部).
  bool get enabled => true;
}

class CatPadSource extends PadSource {
  CatPadSource(this.ctl, this.cat);
  final EditorController ctl;
  final PadCat cat;
  @override
  Listenable get changes => Listenable.merge([ctl.app.editTick, ctl.filmRanges, ctl.lookTick]);
  @override
  PadState get view => ctl.padView(cat);
  @override
  PadLimits get limits => ctl.padLimits(cat);
  @override
  void begin() => ctl.padBegin(cat);
  @override
  void move(double x, double y) => ctl.padMove(cat, x, y);
  @override
  void strip(double a) => ctl.stripSet(cat, a);
  @override
  void end() => ctl.padEnd();
  @override
  void reset() => ctl.resetPad(cat);
  @override
  String get readout => padReadout(cat, ctl.padView(cat), ctx: ctl.padCtx);
  @override
  double percent(double a) => stripPercent(cat, a, ctx: ctl.padCtx);
}

/// A pad of [cat]. [interactive] false = a picture of the pad (the hold popup, where the dock glyph takes the finger).
class PadFace extends StatefulWidget {
  const PadFace({super.key, required this.ctl, required this.cat, this.interactive = true, this.presets = false, this.radius = 18, this.source});
  final EditorController ctl;
  final PadCat cat;
  final bool interactive, presets;
  final double radius;

  /// Where the cursor reads and writes; null = the category's own pad.
  final PadSource? source;

  @override
  State<PadFace> createState() => _PadFaceState();
}

class _PadFaceState extends State<PadFace> {
  ColorPreset? _cand, _pressed;
  Offset _down = Offset.zero;
  Size _size = Size.zero;
  bool _begun = false;

  EditorController get ctl => widget.ctl;
  late PadSource _src = widget.source ?? CatPadSource(ctl, widget.cat);

  @override
  void didUpdateWidget(PadFace old) {
    super.didUpdateWidget(old);
    if (old.source != widget.source || old.cat != widget.cat) _src = widget.source ?? CatPadSource(ctl, widget.cat);
  }

  ColorPreset? _hit(Offset p) {
    if (!widget.presets) return null;
    for (final pr in colorPresets) {
      if ((padToLocal(_size, pr.x, pr.y) - p).distance <= 22) return pr; // a 44 dp hit area
    }
    return null;
  }

  void _set(Offset p) {
    final v = padFromLocal(_size, p);
    _src.move(v.dx, v.dy);
  }

  void _onDown(Offset p) {
    _down = p;
    _begun = false;
    if (!_src.enabled) return;
    _cand = _hit(p);
    if (_cand != null) {
      setState(() => _pressed = _cand);
      return;
    }
    _src.begin();
    _begun = true;
    _set(p);
  }

  void _onMove(Offset p) {
    if (_cand != null) {
      if ((p - _down).distance < 8) return;
      _cand = null;
      setState(() => _pressed = null);
      _src.begin();
      _begun = true;
    }
    if (_begun) _set(p);
  }

  void _onEnd(Offset p, bool cancelled) {
    final c = _cand;
    _cand = null;
    if (c != null) {
      setState(() => _pressed = null);
      if (!cancelled) ctl.selectPreset(c);
      return;
    }
    if (_begun) _src.end();
    _begun = false;
  }

  @override
  Widget build(BuildContext context) {
    final cat = widget.cat;
    final src = _src;
    final dark = context.c.dark;
    final r = widget.radius;
    final overlay = padFieldOverlay(cat, dark, r);
    final words = cat.edgeWords;
    Widget face = LayoutBuilder(builder: (context, cons) {
      _size = Size(cons.maxWidth, cons.maxHeight);
      return Stack(fit: StackFit.expand, children: [
        DecoratedBox(decoration: padFieldDecoration(cat, dark, radius: r)),
        ?overlay,
        RepaintBoundary(child: CustomPaint(painter: _FieldPainter(cat, r))),
        RepaintBoundary(
          child: ListenableBuilder(
            listenable: src.changes,
            builder: (context, _) {
              final s = src.view;
              final children = <Widget>[
                if (src.enabled) CustomPaint(painter: PadCursorPainter(s.x, s.y, src.limits, r)),
              ];
              if (widget.presets) children.addAll(_presetWidgets(context, _size));
              return Stack(fit: StackFit.expand, clipBehavior: Clip.hardEdge, children: children);
            },
          ),
        ),
      ]);
    });
    if (widget.interactive) {
      face = ClaimSurface(onDown: _onDown, onMove: _onMove, onEnd: _onEnd, child: face);
    }
    return Semantics(
      key: ValueKey('pad-${cat.name}'),
      container: true,
      label: '${cat.label}：横向 ${words[0]} 到 ${words[1]}，纵向 ${words[2]} 到 ${words[3]}',
      value: src.readout,
      increasedValue: words[1],
      decreasedValue: words[0],
      onIncrease: () => _step(0.1, 0),
      onDecrease: () => _step(-0.1, 0),
      customSemanticsActions: {
        CustomSemanticsAction(label: '往${words[3]}'): () => _step(0, 0.1),
        CustomSemanticsAction(label: '往${words[2]}'): () => _step(0, -0.1),
        const CustomSemanticsAction(label: '回到自动'): src.reset,
      },
      child: ClipRRect(borderRadius: BorderRadius.circular(r), child: Opacity(opacity: src.enabled ? 1 : 0.38, child: face)),
    );
  }

  void _step(double dx, double dy) {
    final s = _src.view;
    _src.begin();
    _src.move(s.x + dx, s.y + dy);
    _src.end();
  }

  List<Widget> _presetWidgets(BuildContext context, Size size) {
    final e = ctl.edit;
    final cur = e.look ?? ctl.app.settings.defaultLookId;
    final out = <Widget>[];
    final sw = ctl.lookSwatches..start(); // real renderings of the photo with each look, on its current base
    for (final p in colorPresets) {
      final at = padToLocal(size, p.x, p.y);
      final look = lookById(p.look);
      final selected = p.look == cur;
      final pressed = identical(_pressed, p);
      final showName = selected || pressed;
      // the label goes to the side that has room for it (about 12 dp per character plus its padding)
      final labelW = p.name.runes.length * 12.0 + 16;
      final right = at.dx + 14 + labelW <= size.width - 4;
      out.add(Positioned(
        left: at.dx - 22,
        top: at.dy - 22,
        width: 44,
        height: 44,
        child: Center(child: LookDot(key: ValueKey('preset-${p.look}'), image: sw.imageOf(p.look), tint: look.swatch.last, selected: selected)),
      ));
      if (showName) {
        out.add(Positioned(
          left: right ? at.dx + 14 : null,
          right: right ? null : size.width - at.dx + 14,
          top: at.dy - 9,
          child: IgnorePointer(
            child: Container(
              key: ValueKey('preset-name-${p.look}'),
              height: 18,
              padding: const EdgeInsets.symmetric(horizontal: 6),
              alignment: Alignment.center,
              decoration: BoxDecoration(color: Fx.padLabel, borderRadius: BorderRadius.circular(Rd.thumb), boxShadow: const [BoxShadow(color: Color(0x26000000), blurRadius: 3)]),
              child: Text(p.name, style: T.mini(Fx.onPadLabel, w: FontWeight.w600)),
            ),
          ),
        ));
      }
    }
    return out;
  }
}

// ------------------------------------------------------------------------------------------------ strength strip

/// The vertical strength strip: bottom 0 (the category has no effect), the middle line 100 %, the top heavier. Absolute: the
/// thumb is under the finger.
class StrengthStrip extends StatefulWidget {
  const StrengthStrip({super.key, required this.ctl, required this.cat, this.width = 40, this.source, this.label});
  final EditorController ctl;
  final PadCat cat;
  final double width;

  /// Where the thumb reads and writes; null = the category's own strip.
  final PadSource? source;

  /// Spoken name; null = the category's.
  final String? label;

  @override
  State<StrengthStrip> createState() => _StrengthStripState();
}

/// Usable part of the strip's height: the thumb travels between 10 % and 90 %.
double stripFromLocal(double h, double y) => ((1 - y / h - 0.1) / 0.8).clamp(0.0, 1.0).toDouble();
double stripToLocal(double h, double a) => h * (1 - (0.1 + a * 0.8));

class _StrengthStripState extends State<StrengthStrip> {
  double _h = 1;
  bool _begun = false;

  EditorController get ctl => widget.ctl;

  PadSource get _src => widget.source ?? (_own ??= CatPadSource(ctl, widget.cat));
  PadSource? _own;

  void _set(Offset p) => _src.strip(stripFromLocal(_h, p.dy));

  void _nudge(double d) {
    _src.begin();
    _src.strip(_src.view.a + d);
    _src.end();
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final src = _src;
    final a = src.view.a;
    return Semantics(
      key: ValueKey('strip-${widget.source == null ? widget.cat.name : 'local'}'),
      slider: true,
      label: widget.label ?? '${widget.cat.label}强度',
      // the same words as the header readout (audit U1)
      value: '${src.percent(a).round()}%',
      increasedValue: '${src.percent(a + 0.05).round()}%',
      decreasedValue: '${src.percent(a - 0.05).round()}%',
      onIncrease: () => _nudge(0.05),
      onDecrease: () => _nudge(-0.05),
      child: SizedBox(
        width: widget.width,
        child: LayoutBuilder(builder: (context, cons) {
          _h = math.max(1.0, cons.maxHeight);
          return ClaimSurface(
            onDown: (p) {
              if (!src.enabled) return;
              src.begin();
              _begun = true;
              _set(p);
            },
            onMove: (p) {
              if (_begun) _set(p);
            },
            onEnd: (p, cancelled) {
              if (_begun) src.end();
              _begun = false;
            },
            child: Opacity(
              opacity: src.enabled ? 1 : 0.38,
              child: DecoratedBox(
                decoration: BoxDecoration(color: c.bg, borderRadius: BorderRadius.circular(Rd.chip)),
                child: RepaintBoundary(
                  child: ListenableBuilder(
                    listenable: src.changes,
                    builder: (context, _) => CustomPaint(painter: _StripPainter(src.view.a, c.accent, c.text2)),
                  ),
                ),
              ),
            ),
          );
        }),
      ),
    );
  }
}

class _StripPainter extends CustomPainter {
  const _StripPainter(this.a, this.accent, this.mid);
  final double a;
  final Color accent, mid;

  @override
  void paint(Canvas canvas, Size size) {
    final y = stripToLocal(size.height, a);
    // the fill from the bottom up to the thumb
    final fill = RRect.fromLTRBR(5, y, size.width - 5, size.height - 5, const Radius.circular(14));
    if (fill.height > 0) canvas.drawRRect(fill, Paint()..color = accent.withValues(alpha: 0.22));
    // the 100 % line
    final my = size.height * 0.5;
    canvas.drawLine(Offset(10, my), Offset(size.width - 10, my), Paint()..color = mid.withValues(alpha: 0.5)..strokeWidth = 1);
    final p = Offset(size.width / 2, y);
    canvas.drawCircle(p + const Offset(0, 2), 15, Paint()..color = const Color(0x33000000)..maskFilter = const MaskFilter.blur(BlurStyle.normal, 3));
    canvas.drawCircle(p, 15, Paint()..color = Colors.white);
    canvas.drawCircle(p, 15, Paint()..style = PaintingStyle.stroke..strokeWidth = 1..color = const Color(0x40000000));
  }

  @override
  bool shouldRepaint(_StripPainter o) => o.a != a || o.accent != accent || o.mid != mid;
}

/// A colour look on the pad: a small round thumbnail of the photo with that look on its current base (the engine's rendering), over
/// a placeholder in the look's own colour until it arrives; white ring; selected (audit U7) = the white ring with an accent ring outside it.
class LookDot extends StatelessWidget {
  const LookDot({super.key, required this.image, required this.tint, required this.selected, this.size = 26});
  final ui.Image? image;
  final Color tint;
  final bool selected;
  final double size;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: size,
      height: size,
      decoration: BoxDecoration(
        shape: BoxShape.circle,
        border: Border.all(color: Fx.handle, width: selected ? 2 : 1.5),
        boxShadow: [BoxShadow(color: selected ? context.c.accentBtn : Fx.handleRim, blurRadius: 0, spreadRadius: selected ? 2 : 1)],
      ),
      child: ClipOval(
        child: Stack(fit: StackFit.expand, children: [
          ColoredBox(key: const ValueKey('look-placeholder'), color: tint),
          AnimatedOpacity(
            opacity: image == null ? 0 : 1,
            duration: Mo.small,
            child: image == null ? const SizedBox.shrink() : RawImage(key: const ValueKey('look-image'), image: image, fit: BoxFit.cover, filterQuality: FilterQuality.low),
          ),
        ]),
      ),
    );
  }
}
