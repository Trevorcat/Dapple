// Layer 3 of the panel, the itemised view (docs/design/editor-panel.md section 8): the category's parameters as a character
// chart (radar) plus ONE ruler. The radar is both overview and selector: pressing inside it picks the spoke by direction and
// sets its value by the distance from the centre (the spoke stays locked for the rest of the gesture); pressing in the outer
// ring (where the names are) only selects. The dashed polygon is what the pad position implies, the solid polygon the values
// after the item edits. The ruler below fine-tunes the selected parameter; its tape follows the finger.
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../../editor/haptics.dart';
import '../../model/pad_model.dart';
import '../../theme/tokens.dart';
import 'editor_controller.dart';
import 'pad_gestures.dart';
import 'panel_widgets.dart' show NumberEntry;

// ------------------------------------------------------------------------------------------------ geometry

/// Radar geometry for a box of [size]: centre, inner and outer radius of the data, and where the names sit. The radius is as
/// large as the box allows with every name on one line outside the chart (names are [labels], one per spoke).
class RadarGeo {
  RadarGeo(this.size, [List<String> labels = const []])
      : cx = size.width / 2,
        cy = size.height / 2,
        r0 = 10,
        _hw = [for (final l in labels) l.runes.length * 13 / 2 + 8] {
    var r = size.height / 2 - 30;
    final n = labels.length;
    for (var i = 0; i < n; i++) {
      final dx = math.cos(-math.pi / 2 + i * 2 * math.pi / n).abs();
      if (dx < 0.01) continue;
      // |dx| * (r + 12 + hw) + hw <= half the width
      r = math.min(r, (size.width / 2 - 2 - _hw[i]) / dx - 12 - _hw[i]);
    }
    r1 = math.max(24.0, r);
  }
  final Size size;
  final double cx, cy, r0;
  late final double r1;
  final List<double> _hw;

  Offset spokeDir(int i, int n) {
    final a = -math.pi / 2 + i * 2 * math.pi / n;
    return Offset(math.cos(a), math.sin(a));
  }

  Offset point(int i, int n, double t) => Offset(cx, cy) + spokeDir(i, n) * (r0 + t * (r1 - r0));

  /// Where the name of spoke [i] sits (an ellipse: wide boxes leave room to the sides).
  Offset label(int i, int n) {
    final d = spokeDir(i, n);
    final hw = i < _hw.length ? _hw[i] : 28.0;
    return Offset(cx + d.dx * (r1 + 12 + hw), cy + d.dy * (r1 + 17));
  }

  /// The spoke whose direction is nearest to the polar angle of [p] (every item owns a wide sector).
  int spokeAt(Offset p, int n) {
    final a = math.atan2(p.dy - cy, p.dx - cx);
    final step = 2 * math.pi / n;
    return (((a + math.pi / 2) / step).round() % n + n) % n;
  }

  double radius(Offset p) => (p - Offset(cx, cy)).distance;

  /// 0..1 along spoke [i] at the position [p] (distance from the centre projected on the spoke).
  double tAlong(Offset p, int i, int n) {
    final d = spokeDir(i, n);
    final v = p - Offset(cx, cy);
    final along = v.dx * d.dx + v.dy * d.dy;
    return ((along - r0) / (r1 - r0)).clamp(0.0, 1.0).toDouble();
  }

  /// Inside the chart (the disc plus a little margin): sets values. Beyond it, in the ring of names, a press only selects.
  bool inside(Offset p) => radius(p) <= r1 + 10;
}

// ------------------------------------------------------------------------------------------------ the radar

class RadarView extends StatefulWidget {
  const RadarView({super.key, required this.ctl, required this.cat, required this.items, required this.selected, required this.onSelect});
  final EditorController ctl;
  final PadCat cat;
  final List<PadItem> items;
  final int selected;
  final ValueChanged<int> onSelect;

  @override
  State<RadarView> createState() => _RadarViewState();
}

class _RadarViewState extends State<RadarView> {
  int? _lock;
  Size _size = Size.zero;
  bool _began = false;

  EditorController get ctl => widget.ctl;

  void _setFrom(Offset p, int i) {
    final geo = RadarGeo(_size, [for (final it in widget.items) it.label]);
    final it = widget.items[i];
    final t = geo.tAlong(p, i, widget.items.length);
    ctl.itemSet(widget.cat, it, _quantise(it, it.fromNorm(t)));
  }

  double _quantise(PadItem it, double v) {
    final s = it.step;
    return (((v - it.min) / s).round() * s + it.min).clamp(it.min, it.max).toDouble();
  }

  void _onDown(Offset p) {
    final geo = RadarGeo(_size, [for (final it in widget.items) it.label]);
    final n = widget.items.length;
    final i = geo.spokeAt(p, n);
    widget.onSelect(i);
    if (geo.inside(p)) {
      _lock = i;
      _began = true;
      ctl.itemBegin(widget.cat, widget.items[i]);
      _setFrom(p, i);
    } else {
      _lock = null;
    }
  }

  void _onMove(Offset p) {
    final i = _lock;
    if (i == null) return;
    _setFrom(p, i);
  }

  void _onEnd(Offset p, bool cancelled) {
    if (_began) ctl.itemEnd();
    _began = false;
    _lock = null;
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final n = widget.items.length;
    return LayoutBuilder(builder: (context, cons) {
      _size = Size(cons.maxWidth, cons.maxHeight);
      final geo = RadarGeo(_size, [for (final it in widget.items) it.label]);
      return Stack(clipBehavior: Clip.none, children: [
        Positioned.fill(
          child: ClaimSurface(
            onDown: _onDown,
            onMove: _onMove,
            onEnd: _onEnd,
            child: RepaintBoundary(
              child: ListenableBuilder(
                listenable: Listenable.merge([ctl.app.editTick, ctl.edits]),
                builder: (context, _) {
                  final ctx = ctl.padCtx;
                  final e = ctl.edit;
                  final implied = padImplied(e, widget.cat, ctx);
                  return CustomPaint(
                    key: const ValueKey('radar'),
                    painter: _RadarPainter(
                      geo: geo,
                      items: widget.items,
                      actual: [for (final it in widget.items) readItem(e, it, ctx)],
                      implied: [for (final it in widget.items) implied[it.id] ?? it.auto],
                      selected: widget.selected,
                      accent: c.accent,
                      line: c.sep,
                      muted: c.text2,
                      card: c.surface,
                    ),
                  );
                },
              ),
            ),
          ),
        ),
        for (var i = 0; i < n; i++)
          Positioned(
            left: geo.label(i, n).dx,
            top: geo.label(i, n).dy,
            child: FractionalTranslation(
              translation: const Offset(-0.5, -0.5),
              child: IgnorePointer(
                child: Container(
                  key: ValueKey('spoke-${widget.items[i].id}'),
                  padding: const EdgeInsets.symmetric(horizontal: 7, vertical: 4),
                  decoration: BoxDecoration(color: i == widget.selected ? c.accentSoft : null, borderRadius: BorderRadius.circular(10)),
                  child: Text(widget.items[i].label, softWrap: false, style: T.cap(i == widget.selected ? c.accent : c.text2, w: i == widget.selected ? FontWeight.w600 : FontWeight.w400)),
                ),
              ),
            ),
          ),
      ]);
    });
  }
}

class _RadarPainter extends CustomPainter {
  _RadarPainter({required this.geo, required this.items, required this.actual, required this.implied, required this.selected, required this.accent, required this.line, required this.muted, required this.card});
  final RadarGeo geo;
  final List<PadItem> items;
  final List<double> actual, implied;
  final int selected;
  final Color accent, line, muted, card;

  @override
  void paint(Canvas canvas, Size size) {
    final n = items.length;
    final c = Offset(geo.cx, geo.cy);
    canvas.drawCircle(c, geo.r1, Paint()..style = PaintingStyle.stroke..color = line);
    // the middle ring: zero of the signed items
    canvas.drawCircle(c, (geo.r0 + geo.r1) / 2, Paint()..style = PaintingStyle.stroke..color = line.withValues(alpha: 0.5)..strokeWidth = 0.7);
    // the selected spoke owns a faint wedge (its sector) and a heavier line
    if (n > 2) {
      final step = 2 * math.pi / n, mid = -math.pi / 2 + selected * step;
      final wedge = Path()
        ..moveTo(c.dx, c.dy)
        ..arcTo(Rect.fromCircle(center: c, radius: geo.r1), mid - step / 2, step, false)
        ..close();
      canvas.drawPath(wedge, Paint()..color = accent.withValues(alpha: 0.08));
    }
    for (var i = 0; i < n; i++) {
      canvas.drawLine(c, c + geo.spokeDir(i, n) * geo.r1, Paint()..color = i == selected ? accent : line..strokeWidth = i == selected ? 2.2 : 1);
    }
    Path poly(List<double> vals) {
      final p = Path();
      for (var i = 0; i < n; i++) {
        final pt = geo.point(i, n, items[i].norm(vals[i]));
        i == 0 ? p.moveTo(pt.dx, pt.dy) : p.lineTo(pt.dx, pt.dy);
      }
      return p..close();
    }

    // what the pad position implies: dashed
    final dashed = poly(implied);
    for (final m in dashed.computeMetrics()) {
      var d = 0.0;
      while (d < m.length) {
        canvas.drawPath(m.extractPath(d, math.min(d + 3, m.length)), Paint()..style = PaintingStyle.stroke..color = muted..strokeWidth = 1);
        d += 6;
      }
    }
    final solid = poly(actual);
    canvas.drawPath(solid, Paint()..color = accent.withValues(alpha: 0.22));
    canvas.drawPath(solid, Paint()..style = PaintingStyle.stroke..color = accent..strokeWidth = 1.5..strokeJoin = StrokeJoin.round);
    for (var i = 0; i < n; i++) {
      final pt = geo.point(i, n, items[i].norm(actual[i]));
      canvas.drawCircle(pt, i == selected ? 7.5 : 3.5, Paint()..color = accent);
      canvas.drawCircle(pt, i == selected ? 7.5 : 3.5, Paint()..style = PaintingStyle.stroke..strokeWidth = 1.5..color = card);
    }
  }

  @override
  bool shouldRepaint(_RadarPainter o) => true;
}

// ------------------------------------------------------------------------------------------------ the ruler

/// One tick of the tape is 1 % of the range, [kTapePx] dp wide; a full range is 100 ticks (700 dp), so the tape is a precise
/// instrument rather than a slider.
const double kTapePx = 7;

/// The tape position of [v] relative to the pointer, in dp, for a ruler over [min]..[max] (tests: the tape follows the finger).
double tapeOffset(double v, double min, double max) => -(v - min) / ((max - min) / 100) * kTapePx;

/// A ruler for one parameter: the value above, the tape below; the pointer is fixed in the middle and the tape FOLLOWS the
/// finger (drag right, the tape moves right and the value goes down). Haptic detents at [auto] and at the range ends.
class TapeRuler extends StatefulWidget {
  const TapeRuler({
    super.key,
    required this.label,
    required this.value,
    required this.min,
    required this.max,
    required this.auto,
    required this.format,
    required this.onBegin,
    required this.onChanged,
    required this.onEnd,
    required this.haptics,
    this.step = 1,
    this.typed,
    this.onReset,
    this.entryText,
    this.onSubmit,
    this.editing = false,
    this.onTapValue,
  });
  final String label;
  final double value, min, max, auto, step;
  final String Function(double) format;
  final VoidCallback onBegin, onEnd;
  final ValueChanged<double> onChanged;
  final Haptics haptics;

  /// The value is not at the position the pad implies: a reset button appears.
  final bool? typed;
  final VoidCallback? onReset;

  /// Numeric entry: tapping the value calls [onTapValue]; while [editing] the entry shows [entryText] and reports [onSubmit].
  final String? entryText;
  final ValueChanged<String>? onSubmit;
  final bool editing;
  final VoidCallback? onTapValue;

  @override
  State<TapeRuler> createState() => _TapeRulerState();
}

class _TapeRulerState extends State<TapeRuler> {
  late final ValueNotifier<double> _v = ValueNotifier(widget.value);
  double _startV = 0, _startX = 0;
  bool _drag = false;

  @override
  void didUpdateWidget(TapeRuler old) {
    super.didUpdateWidget(old);
    if (!_drag) _v.value = widget.value;
  }

  @override
  void dispose() {
    _v.dispose();
    super.dispose();
  }

  double _q(double v) => (((v - widget.min) / widget.step).round() * widget.step + widget.min).clamp(widget.min, widget.max).toDouble();

  void _down(Offset p) {
    _drag = true;
    _startV = _v.value;
    _startX = p.dx;
    widget.onBegin();
  }

  void _move(Offset p) {
    if (!_drag) return;
    final unit = (widget.max - widget.min) / 100;
    final raw = _startV - (p.dx - _startX) / kTapePx * unit; // the tape follows the finger: right = value down
    final v = _q(raw);
    if (v == _v.value) return;
    final prev = _v.value;
    _v.value = v;
    if ((prev < widget.auto) != (v < widget.auto) || v == widget.auto) {
      widget.haptics.fire(HapticKind.autoSnap);
    } else if ((v == widget.min || v == widget.max) && prev != v) {
      widget.haptics.fire(HapticKind.endStop);
    }
    widget.onChanged(v);
  }

  void _end(Offset p, bool cancelled) {
    if (!_drag) return;
    _drag = false;
    widget.onEnd();
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final valueStyle = T.body17(c.text, w: FontWeight.w600).copyWith(fontFeatures: const [FontFeature.tabularFigures()]);
    final value = ValueListenableBuilder<double>(
      valueListenable: _v,
      builder: (context, v, _) => Text('${widget.label}  ${widget.format(v)}', key: const ValueKey('ruler-value'), maxLines: 1, style: valueStyle),
    );
    return Semantics(
      container: true,
      slider: true,
      label: widget.label,
      value: widget.format(widget.value),
      increasedValue: widget.format(_q(widget.value + widget.step * 5)),
      decreasedValue: widget.format(_q(widget.value - widget.step * 5)),
      onIncrease: () {
        widget.onBegin();
        widget.onChanged(_q(widget.value + widget.step * 5));
        widget.onEnd();
      },
      onDecrease: () {
        widget.onBegin();
        widget.onChanged(_q(widget.value - widget.step * 5));
        widget.onEnd();
      },
      child: Container(
        key: const ValueKey('tape-ruler'),
        height: 64,
        decoration: BoxDecoration(color: c.bg, borderRadius: BorderRadius.circular(14)),
        clipBehavior: Clip.antiAlias,
        child: Stack(children: [
          Positioned.fill(
            child: ClaimSurface(
              onDown: _down,
              onMove: _move,
              onEnd: _end,
              child: RepaintBoundary(
                child: CustomPaint(
                  key: const ValueKey('tape'),
                  painter: TapePainter(_v, widget.min, widget.max, widget.auto, c.text2, c.text, c.accent),
                ),
              ),
            ),
          ),
          Positioned(
            left: 48,
            right: 48,
            top: -2,
            height: 32,
            child: Center(
              child: widget.editing
                  ? NumberEntry(key: const ValueKey('ruler-entry'), initial: widget.entryText ?? '', style: valueStyle, onSubmit: widget.onSubmit ?? (_) {})
                  : GestureDetector(behavior: HitTestBehavior.opaque, onTap: widget.onTapValue, child: Center(widthFactor: 1, child: value)),
            ),
          ),
          if (widget.onReset != null && (widget.typed ?? false))
            Positioned(
              right: 0,
              top: 0,
              child: GestureDetector(
                key: const ValueKey('ruler-reset'),
                behavior: HitTestBehavior.opaque,
                onTap: widget.onReset,
                child: SizedBox(width: 48, height: 34, child: Icon(Icons.refresh, size: 18, color: c.text2)),
              ),
            ),
        ]),
      ),
    );
  }
}

class TapePainter extends CustomPainter {
  TapePainter(this.v, this.min, this.max, this.auto, this.tick, this.ink, this.accent) : super(repaint: v);
  final ValueNotifier<double> v;
  final double min, max, auto;
  final Color tick, ink, accent;

  @override
  void paint(Canvas canvas, Size size) {
    final unit = (max - min) / 100;
    if (unit <= 0) return;
    final cx = size.width / 2;
    final base = size.height - 8;
    final x0 = cx + tapeOffset(v.value, min, max); // x of the range's low end
    final p = Paint()..strokeCap = StrokeCap.round;
    final first = math.max(0, ((0 - x0) / kTapePx).floor() - 1), last = math.min(100, ((size.width - x0) / kTapePx).ceil() + 1);
    for (var i = first; i <= last; i++) {
      final x = x0 + i * kTapePx;
      final major = i % 5 == 0;
      final d = ((x - cx).abs() / (size.width / 2)).clamp(0.0, 1.0);
      final fade = 1 - 0.7 * d * d;
      p
        ..color = (major ? ink : tick).withValues(alpha: (major ? 0.7 : 0.45) * fade)
        ..strokeWidth = major ? 1.5 : 1;
      canvas.drawLine(Offset(x, base), Offset(x, base - (major ? 18 : 9)), p);
    }
    // the parameter's own automatic position: a taller mark
    final ax = x0 + (auto - min) / unit * kTapePx;
    if (ax > -4 && ax < size.width + 4) {
      canvas.drawLine(Offset(ax, base + 2), Offset(ax, base - 24), Paint()..color = ink.withValues(alpha: 0.9)..strokeWidth = 2.5..strokeCap = StrokeCap.round);
    }
    // the fixed pointer
    canvas.drawLine(Offset(cx, base - 24), Offset(cx, base + 4), Paint()..color = accent..strokeWidth = 2.5..strokeCap = StrokeCap.round);
  }

  @override
  bool shouldRepaint(TapePainter o) => o.min != min || o.max != max || o.auto != auto || o.tick != tick || o.accent != accent;
}

// ------------------------------------------------------------------------------------------------ layer 3

/// The itemised view of [cat]: radar on top, the ruler of the selected parameter below.
class ItemisedView extends StatelessWidget {
  const ItemisedView({super.key, required this.ctl, required this.cat});
  final EditorController ctl;
  final PadCat cat;

  @override
  Widget build(BuildContext context) {
    final items = ctl.itemsOf(cat);
    final sel = ctl.selItem.clamp(0, items.length - 1);
    return Column(children: [
      Expanded(child: RadarView(ctl: ctl, cat: cat, items: items, selected: sel, onSelect: ctl.selectItem)),
      const SizedBox(height: 6),
      ItemRuler(ctl: ctl, cat: cat, item: items[sel]),
    ]);
  }
}

/// The ruler of one item: reads and writes it through the pad's offsets.
class ItemRuler extends StatelessWidget {
  const ItemRuler({super.key, required this.ctl, required this.cat, required this.item});
  final EditorController ctl;
  final PadCat cat;
  final PadItem item;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: Listenable.merge([ctl.app.editTick, ctl.editingRow]),
      builder: (context, _) {
        final ctx = ctl.padCtx;
        final v = readItem(ctl.edit, item, ctx);
        final implied = padImplied(ctl.edit, cat, ctx)[item.id] ?? item.auto;
        final editing = ctl.editingRow.value == item.id;
        return TapeRuler(
          key: ValueKey('ruler-${item.id}'),
          label: item.label,
          value: v,
          min: item.min,
          max: item.max,
          auto: item.auto,
          step: item.step,
          format: item.fmt,
          haptics: ctl.haptics,
          onBegin: () => ctl.itemBegin(cat, item),
          onChanged: (x) => ctl.itemSet(cat, item, x),
          onEnd: ctl.itemEnd,
          typed: (v - implied).abs() > 1e-6,
          onReset: () => ctl.resetItem(cat, item),
          editing: editing,
          entryText: ctl.inputText(item, v),
          onTapValue: () => ctl.beginNumeric(item),
          onSubmit: (t) => ctl.commitItem(cat, item, t),
        );
      },
    );
  }
}
