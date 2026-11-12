// The category dock (docs/design/editor-panel.md section 2): five items. The first four are live glyphs: a small square (the
// film one a circle) with a dot at the pad position and a thin bar for the strength, so one element is the entry, the state
// and the readout. A tap opens the panel; a press-and-hold (or starting to drag on the glyph) pops the pad up above the dock
// and the drag moves the value relative to where it started; releasing closes the popup.
import 'dart:async';

import 'package:flutter/material.dart';

import '../../model/pad_model.dart';
import '../../model/params.dart';
import '../../theme/tokens.dart';
import '../../widgets/icons.dart';
import 'editor_controller.dart';
import 'editor_overlays.dart' show GlassCapsule;
import 'pad_widgets.dart';

/// The dock's categories, in order.
const List<EGroup> padDockOrder = [EGroup.light, EGroup.color, EGroup.style, EGroup.effects, EGroup.crop];

String dockLabel(EGroup g) => g == EGroup.crop ? '裁切' : PadCatX.ofGroup(g)!.label;

/// Hold time before the popup appears (a drag beyond [kHoldSlop] starts it at once).
const Duration kHoldDelay = Duration(milliseconds: 260);
const double kHoldSlop = 8;

class PadDock extends StatelessWidget {
  const PadDock({super.key, required this.ctl});
  final EditorController ctl;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: Listenable.merge([ctl.wm, ctl.app.editTick, ctl.hold]),
      builder: (context, _) => LayoutBuilder(builder: (context, cons) {
        final n = padDockOrder.length;
        final iw = (cons.maxWidth / n).clamp(52.0, 76.0).toDouble();
        return GlassCapsule(
          radius: 28,
          child: SizedBox(
            width: iw * n,
            height: 56,
            child: Row(children: [
              for (final g in padDockOrder)
                SizedBox(width: iw, height: 56, child: DockItem(key: ValueKey('dock-$g'), ctl: ctl, group: g)),
            ]),
          ),
        );
      }),
    );
  }
}

class DockItem extends StatefulWidget {
  const DockItem({super.key, required this.ctl, required this.group});
  final EditorController ctl;
  final EGroup group;

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
    if (c == null || !_down || _holding) return;
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
    if (cat != null) _timer = Timer(kHoldDelay, _begin);
  }

  void _onMove(PointerMoveEvent e) {
    if (e.pointer != _pointer) return;
    final d = e.position - _start;
    if (!_holding && cat != null && d.distance > kHoldSlop) {
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
      ctl.openCategory(widget.group);
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
    final Widget glyph = pc == null
        ? DIcon(DI.crop, size: 24, color: fg, stroke: 1.9)
        : Opacity(opacity: dim ? 0.38 : 1, child: SizedBox(width: 34, height: 26, child: CustomPaint(key: ValueKey('glyph-${pc.name}'), painter: GlyphPainter(s.x, s.y, s.a, pc == PadCat.film, fg))));
    return Semantics(
      selected: on,
      button: true,
      label: pc == null ? label : '$label，${padReadout(pc, s)}${adjusted ? '，已调整' : ''}，按住拖动直接调整',
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
            Text(label, style: T.mini(fg, w: on ? FontWeight.w600 : FontWeight.w400)),
          ]),
        ),
      ),
    );
  }
}

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
              padding: const EdgeInsets.all(6),
              decoration: BoxDecoration(color: c.surface, borderRadius: BorderRadius.circular(26), boxShadow: const [BoxShadow(color: Color(0x52000000), blurRadius: 28, offset: Offset(0, 8))]),
              child: PadFace(ctl: ctl, cat: h.cat, interactive: false, presets: h.cat == PadCat.color, radius: 21),
            ),
          ),
        );
      },
    );
  }
}
