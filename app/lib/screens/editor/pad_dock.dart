// The category dock (docs/design/editor-panel.md section 2): five items. The first four are live glyphs: a small square (the
// film one a circle) with a dot at the pad position and a thin bar for the strength, so one element is the entry, the state
// and the readout. A tap opens the panel; a press-and-hold (or starting to drag on the glyph) pops the pad up above the dock
// and the drag moves the value relative to where it started; releasing closes the popup.
import 'dart:async';

import 'package:flutter/material.dart';

import '../../model/pad_model.dart';
import '../../model/params.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart' show kHit;
import '../../widgets/icons.dart';
import 'editor_controller.dart';
import 'editor_overlays.dart' show GlassCapsule;
import 'local_panel.dart' show LocalDockGlyphPainter;
import '../../model/local_kinds.dart';
import '../../model/local_pad.dart' show localStripPercent, localUiOf;
import 'pad_widgets.dart';

/// The dock's categories, in order.
const List<EGroup> padDockOrder = [EGroup.light, EGroup.color, EGroup.style, EGroup.effects, EGroup.local, EGroup.crop];

String dockLabel(EGroup g) => g == EGroup.crop ? '裁切' : (g == EGroup.local ? '局部' : PadCatX.ofGroup(g)!.label);

/// Hold time before the popup appears (a drag beyond [kHoldSlop] starts it at once).
const Duration kHoldDelay = Mo.hold;
const double kHoldSlop = 8;

class PadDock extends StatelessWidget {
  const PadDock({super.key, required this.ctl});
  final EditorController ctl;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: Listenable.merge([ctl.wm, ctl.app.editTick, ctl.hold, ctl.local]),
      builder: (context, _) => LayoutBuilder(builder: (context, cons) {
        final n = padDockOrder.length;
        // six items of at least 48 dp (the touch target, guidelines 6): the side-pane dock on a landscape phone is ~288 dp; a still
        // narrower window scrolls the dock (guidelines 15.2.2) rather than shrinking the items
        final iw = (cons.maxWidth / n).clamp(kHit, 76.0).toDouble();
        final dh = padDockH(MediaQuery.textScalerOf(context).scale(15) / 15);
        final row = SizedBox(
          width: iw * n,
          height: dh,
          child: Row(children: [
            for (final g in padDockOrder) SizedBox(width: iw, height: dh, child: DockItem(key: ValueKey('dock-$g'), ctl: ctl, group: g)),
          ]),
        );
        return GlassCapsule(
          radius: 28,
          child: iw * n <= cons.maxWidth + 0.5 ? row : SizedBox(width: cons.maxWidth, child: SingleChildScrollView(scrollDirection: Axis.horizontal, child: row)),
        );
      }),
    );
  }
}

/// Why a greyed dock category has nothing to show right now and what to do, one plain line; null when the category is not greyed
/// (nothing is said then).
String? dockDimReason(EditorController ctl, EGroup g) {
  if (g == EGroup.local) return ctl.edit.local.isEmpty ? '还没有区域，点「+」新建' : null;
  final pc = PadCatX.ofGroup(g);
  if (pc == null) return null;
  if (pc == PadCat.film && ctl.edit.film == null) return '还没有选成像，在面板里选一个';
  if (padViewOf(ctl.edit, pc, ctl.padCtx).a < 0.02) return '现在是 0，拖动盘面开始调整';
  return null;
}

class DockItem extends StatefulWidget {
  const DockItem({super.key, required this.ctl, required this.group, this.holdToAdjust = true});
  final EditorController ctl;
  final EGroup group;

  /// False where the hold popup has no place to rise from (the fold layouts' column): a tap still opens the category.
  final bool holdToAdjust;

  @override
  State<DockItem> createState() => _DockItemState();
}

class _DockItemState extends State<DockItem> {
  Timer? _timer;
  Offset _start = Offset.zero, _startGlobal = Offset.zero;
  bool _down = false, _holding = false;
  int? _pointer;

  EditorController get ctl => widget.ctl;
  PadCat? get cat => PadCatX.ofGroup(widget.group);

  @override
  void dispose() {
    _timer?.cancel();
    if (_holding) ctl.holdEnd();
    super.dispose();
  }

  void _begin() {
    final c = cat;
    if (c == null || !_down || _holding || !widget.holdToAdjust) return;
    _holding = true;
    ctl.holdBegin(c, _startGlobal.dx);
  }

  void _onDown(PointerDownEvent e) {
    if (_pointer != null) return;
    _pointer = e.pointer;
    _down = true;
    _holding = false;
    _start = e.position;
    _startGlobal = e.position;
    _timer?.cancel();
    if (cat != null && widget.holdToAdjust) _timer = Timer(kHoldDelay, _begin);
  }

  void _onMove(PointerMoveEvent e) {
    if (e.pointer != _pointer) return;
    final d = e.position - _start;
    if (!_holding && cat != null && widget.holdToAdjust && d.distance > kHoldSlop) {
      _timer?.cancel();
      _begin();
    }
    if (_holding) ctl.holdMove(d.dx, d.dy);
  }

  void _onUp(PointerEvent e, {required bool cancelled}) {
    if (e.pointer != _pointer) return;
    _pointer = null;
    _timer?.cancel();
    final wasHolding = _holding;
    _down = false;
    _holding = false;
    if (wasHolding) {
      ctl.holdEnd();
    } else if (!cancelled) {
      final why = dockDimReason(ctl, widget.group);
      ctl.openCategory(widget.group);
      // after the switch: a tab change clears the previous page's toast
      if (why != null) ctl.app.showToast(why);
    }
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final g = widget.group;
    final selected = ctl.wm.windowOf(g) != null;
    final pressed = ctl.hold.value?.cat == cat && cat != null;
    final on = selected || pressed;
    final fg = on ? c.accent : c.text2;
    final label = dockLabel(g);
    final ed = ctl.edit;
    final pc = cat;
    final PadState s = pc == null ? PadState.centre : padViewOf(ed, pc, ctl.padCtx);
    final dim = pc != null && (s.a < 0.02 || (pc == PadCat.film && ed.film == null));
    final adjusted = pc != null && padCatAdjusted(ed, pc, ctl.padCtx);
    final local = g == EGroup.local;
    final areas = ed.local;
    final la = local && !areas.isEmpty ? (areas.byId(ctl.local.selId ?? '') ?? areas.items.last) : null;
    final Widget glyph = local
        ? Opacity(
            opacity: la == null ? 0.38 : 1,
            child: SizedBox(
                width: 34,
                height: 26,
                child: CustomPaint(
                    key: const ValueKey('glyph-local'),
                    painter: LocalDockGlyphPainter(la == null || la.mask.isEmpty ? LocalGlyph.radial : kindOfComponent(la.mask.first).glyph, areas.length, fg,
                        ink: c.surface, textScale: MediaQuery.textScalerOf(context).scale(11) / 11, base: DefaultTextStyle.of(context).style, strength: la == null ? null : localUiOf(la).a))))
        : pc == null
        ? DIcon(DI.crop, size: 24, color: fg, stroke: 1.9)
        : Opacity(opacity: dim ? 0.38 : 1, child: SizedBox(width: 34, height: 26, child: CustomPaint(key: ValueKey('glyph-${pc.name}'), painter: GlyphPainter(s.x, s.y, s.a, pc == PadCat.film, fg))));
    return Semantics(
      selected: on,
      button: true,
      label: local ? '$label${areas.isEmpty ? '' : '，${areas.length} 个区域'}${la == null ? '' : '，强度 ${localStripPercent(la, localUiOf(la).a).round()}%'}' : (pc == null ? label : '$label，${padReadout(pc, s, ctx: ctl.padCtx)}${adjusted ? '，已调整' : ''}${widget.holdToAdjust ? '，按住拖动直接调整' : ''}'),
      excludeSemantics: true,
      onTap: () => ctl.openCategory(g),
      child: Listener(
        behavior: HitTestBehavior.opaque,
        onPointerDown: _onDown,
        onPointerMove: _onMove,
        onPointerUp: (e) => _onUp(e, cancelled: false),
        onPointerCancel: (e) => _onUp(e, cancelled: true),
        child: Container(
          margin: const EdgeInsets.symmetric(horizontal: 2, vertical: 2),
          decoration: BoxDecoration(color: on ? c.accentSoft : null, borderRadius: BorderRadius.circular(26)),
          child: Column(mainAxisAlignment: MainAxisAlignment.center, mainAxisSize: MainAxisSize.min, children: [
            glyph,
            const SizedBox(height: 3),
            // one line at any text size: a long word shrinks to the item rather than wrapping under it (text scale 2.0)
            FittedBox(fit: BoxFit.scaleDown, child: Text(label, maxLines: 1, softWrap: false, style: T.cap(fg, w: on ? FontWeight.w600 : FontWeight.w400))),
          ]),
        ),
      ),
    );
  }
}

/// Dock height: 56, taller when the label (12 / 16, guidelines 3.2) grows with the system text size (glyph 26 + gap 3 + label + margin 4).
double padDockH(double textScale) => textScale <= 1.0 ? 56.0 : 34 + 16 * textScale;

/// The dock glyph: a rounded square (a circle for film) with a dot at the pad position and a thin bar for the strength.
class GlyphPainter extends CustomPainter {
  const GlyphPainter(this.x, this.y, this.a, this.round, this.color);
  final double x, y, a;
  final bool round;
  final Color color;

  @override
  void paint(Canvas canvas, Size size) {
    const g = 24.0;
    final box = Rect.fromLTWH(1, (size.height - g) / 2, g, g);
    final line = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.5
      ..color = color;
    if (round) {
      canvas.drawCircle(box.center, g / 2 - 0.75, line);
    } else {
      canvas.drawRRect(RRect.fromRectAndRadius(box.deflate(0.75), const Radius.circular(7)), line);
    }
    final hair = Paint()
      ..strokeWidth = 1
      ..color = color.withValues(alpha: 0.25);
    canvas.drawLine(Offset(box.center.dx, box.top + 2), Offset(box.center.dx, box.bottom - 2), hair);
    canvas.drawLine(Offset(box.left + 2, box.center.dy), Offset(box.right - 2, box.center.dy), hair);
    // the dot sits where the pad cursor sits
    canvas.drawCircle(Offset(box.center.dx + x * g * 0.36, box.center.dy - y * g * 0.36), 3.5, Paint()..color = color);
    // the strength bar grows from the bottom
    final h = 2 + a * 22;
    canvas.drawRRect(RRect.fromRectAndRadius(Rect.fromLTRB(box.right + 3.5, box.bottom + 1.5 - h, box.right + 6, box.bottom + 1.5), const Radius.circular(1.25)), Paint()..color = color);
  }

  @override
  bool shouldRepaint(GlyphPainter o) => o.x != x || o.y != y || o.a != a || o.round != round || o.color != color;
}

/// The popup above the dock while a glyph is held: the pad at full size, the photo stays where it is.
class HoldPopupLayer extends StatelessWidget {
  const HoldPopupLayer({super.key, required this.ctl, required this.dockTop, required this.screenW});
  final EditorController ctl;
  final double dockTop, screenW;

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<HoldPopup?>(
      valueListenable: ctl.hold,
      builder: (context, h, _) {
        if (h == null) return const SizedBox.shrink();
        final c = context.c;
        const side = 216.0;
        final left = (h.anchorX - side / 2).clamp(8.0, screenW - side - 8).toDouble();
        return Positioned(
          left: left,
          top: dockTop - 10 - side,
          width: side,
          height: side,
          child: IgnorePointer(
            child: Container(
              key: const ValueKey('hold-popup'),
              padding: const EdgeInsets.all(Sp.s),
              decoration: BoxDecoration(color: c.surface, borderRadius: BorderRadius.circular(Rd.panel), boxShadow: Fx.shadowFloat),
              child: PadFace(ctl: ctl, cat: h.cat, interactive: false, presets: h.cat == PadCat.color, radius: Rd.cardLg),
            ),
          ),
        );
      },
    );
  }
}
