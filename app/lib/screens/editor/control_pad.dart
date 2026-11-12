// Direct-manipulation surfaces that live inside a panel or window: the 2D pads (colour palette,
// grading wheel, curve) and the shared pointer plumbing of the docked ruler. They claim their own
// drags in the gesture arena, so a scroll view around them (a floating window) can never steal the
// drag, and they feed the same [EditorController] code path as a drag on the photo
// (beginInteraction with the active parameter, editTick, one undo step per gesture).
import 'dart:async';

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';

import '../../editor/gesture_arbiter.dart' show LockAxis;
import '../../editor/param_rows.dart';
import '../../editor/value_mapping.dart';
import '../../model/catalog.dart';
import '../../model/params.dart';
import '../../theme/tokens.dart';
import 'editor_controller.dart';
import 'mini_maps.dart';

/// Wins the arena the moment a pointer lands, then reports the pointer's events.
class ClaimPanRecognizer extends OneSequenceGestureRecognizer {
  ClaimPanRecognizer({super.debugOwner});
  void Function(PointerDownEvent)? onDown;
  void Function(PointerMoveEvent)? onMove;
  void Function(PointerEvent, bool cancelled)? onEnd;
  int? _pointer;

  @override
  void addAllowedPointer(PointerDownEvent event) {
    if (_pointer != null) return; // one finger
    _pointer = event.pointer;
    startTrackingPointer(event.pointer, event.transform);
    resolve(GestureDisposition.accepted);
    onDown?.call(event);
  }

  @override
  void handleEvent(PointerEvent event) {
    if (event.pointer != _pointer) return;
    if (event is PointerMoveEvent) {
      onMove?.call(event);
    } else if (event is PointerUpEvent || event is PointerCancelEvent) {
      _pointer = null;
      onEnd?.call(event, event is PointerCancelEvent);
      stopTrackingPointer(event.pointer);
    }
  }

  @override
  void didStopTrackingLastPointer(int pointer) {
    _pointer = null;
  }

  @override
  String get debugDescription => 'claimPan';
}

/// One-finger surface bound to [row] of the controller: a drag beyond 3 dp starts an adjust gesture
/// ([axis] none = free 2D), a tap calls [onTap], a double tap resets the row.
class ControlSurface extends StatefulWidget {
  const ControlSurface({super.key, required this.ctl, required this.row, required this.axis, required this.child, this.onTap});
  final EditorController ctl;
  final ParamRow row;
  final LockAxis axis;
  final Widget child;

  final ValueChanged<Offset>? onTap;

  @override
  State<ControlSurface> createState() => _ControlSurfaceState();
}

class _ControlSurfaceState extends State<ControlSurface> {
  Offset _start = Offset.zero, _last = Offset.zero;
  int _lastMs = 0;
  bool _began = false;
  final VelocityWindow _vel = VelocityWindow();
  Timer? _tapTimer;

  @override
  void dispose() {
    _tapTimer?.cancel();
    if (_began) widget.ctl.adjustEnd(byMultiTouch: true);
    super.dispose();
  }

  void _down(PointerDownEvent e) {
    _start = _last = e.position;
    _lastMs = e.timeStamp.inMilliseconds;
    _began = false;
    _vel.clear();
    _vel.add(_lastMs, e.position.dx, e.position.dy);
  }

  void _move(PointerMoveEvent e) {
    final ms = e.timeStamp.inMilliseconds;
    if (!_began) {
      if ((e.position - _start).distance < 3) return;
      _began = true;
      _tapTimer?.cancel();
      _tapTimer = null;
      widget.ctl.beginOn(widget.row, widget.axis, _start.dx, _start.dy);
      _lastMs = ms;
    }
    final d = e.position - _last;
    _last = e.position;
    final dt = (ms - _lastMs).toDouble().clamp(0.0, 1000.0);
    _lastMs = ms;
    _vel.add(ms, e.position.dx, e.position.dy);
    _vel.compute(ms);
    final off = e.position - _start;
    widget.ctl.adjustMove(d.dx, d.dy, off.dx, off.dy, _vel.vx, _vel.vy, dt);
  }

  void _end(PointerEvent e, bool cancelled) {
    if (_began) {
      _began = false;
      widget.ctl.adjustEnd(byMultiTouch: false);
      return;
    }
    if (cancelled) return;
    widget.onTap?.call(e.localPosition);
    if (_tapTimer != null) {
      _tapTimer!.cancel();
      _tapTimer = null;
      widget.ctl.resetRow(widget.row);
    } else {
      _tapTimer = Timer(const Duration(milliseconds: 300), () => _tapTimer = null);
    }
  }

  @override
  Widget build(BuildContext context) => RawGestureDetector(
        behavior: HitTestBehavior.opaque,
        gestures: {
          ClaimPanRecognizer: GestureRecognizerFactoryWithHandlers<ClaimPanRecognizer>(
            ClaimPanRecognizer.new,
            (r) => r
              ..onDown = _down
              ..onMove = _move
              ..onEnd = _end,
          ),
        },
        child: widget.child,
      );
}

/// The "fine" switch of the 2D pads (a quarter of the normal gain). 44 dp hit target, 32 dp look.
class FineSwitch extends StatelessWidget {
  const FineSwitch({super.key, required this.ctl, this.compact = false});
  final EditorController ctl;
  final bool compact;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return ValueListenableBuilder<bool>(
      valueListenable: ctl.fine2D,
      builder: (context, on, _) => Semantics(
        toggled: on,
        button: true,
        label: '精细',
        excludeSemantics: true,
        child: GestureDetector(
          key: const ValueKey('fine-switch'),
          behavior: HitTestBehavior.opaque,
          onTap: () => ctl.fine2D.value = !on,
          child: SizedBox(
            height: compact ? 32 : 44,
            child: Center(
              widthFactor: 1,
              child: Container(
                height: compact ? 26 : 32,
                padding: EdgeInsets.symmetric(horizontal: compact ? 8 : 10),
                decoration: BoxDecoration(
                  color: on ? c.accentSoft : c.chip,
                  borderRadius: BorderRadius.circular(16),
                  border: on ? Border.all(color: c.accent, width: 1.5) : null,
                ),
                child: Center(widthFactor: 1, child: Text('精细', style: T.cap(on ? c.accent : c.text2, w: FontWeight.w500))),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

/// A 2D pad (palette, grading wheel, curve) filling the space its parent gives it. Its readout is the
/// parameter's chip; the pad itself only draws the position. Repaints from the controller's readout
/// notifier while a finger moves, never through the panel.
class ControlPad extends StatelessWidget {
  const ControlPad({super.key, required this.ctl, required this.row});
  final EditorController ctl;
  final ParamRow row;

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(builder: (context, cons) {
      final w = cons.maxWidth.isFinite ? cons.maxWidth : 160.0, h = cons.maxHeight.isFinite ? cons.maxHeight : 120.0;
      final fine = Align(
        alignment: ctl.wm.side.name == 'left' ? Alignment.topLeft : Alignment.topRight,
        child: Padding(padding: const EdgeInsets.all(2), child: FineSwitch(ctl: ctl, compact: true)),
      );
      return RepaintBoundary(
        key: ValueKey('pad-${row.id}'),
        child: Stack(children: [
          Positioned.fill(
            child: ControlSurface(
              ctl: ctl,
              row: row,
              axis: LockAxis.none,
              onTap: row.twoD == TwoD.curve ? (p) => _selectPoint(p.dx / w) : null,
              child: ListenableBuilder(
                listenable: Listenable.merge([ctl.edits, ctl.readout]),
                builder: (context, _) => _visual(context, w, h),
              ),
            ),
          ),
          fine,
        ]),
      );
    });
  }

  void _selectPoint(double x) {
    const xs = [0.88, 0.62, 0.37, 0.12];
    var best = 0;
    for (var i = 1; i < 4; i++) {
      if ((xs[i] - x).abs() < (xs[best] - x).abs()) best = i;
    }
    final rows = ctl.rowsOf(EGroup.curve);
    ctl.setActive(EGroup.curve, rows[best + 1]);
  }

  Widget _visual(BuildContext context, double w, double h) {
    final e = ctl.edit;
    switch (row.twoD!) {
      case TwoD.palette:
        final look = lookById(e.look ?? ctl.app.settings.defaultLookId);
        return PaletteMini(pos: e.palette ?? Offset(look.px, look.py), def: Offset(look.px, look.py), size: w, height: h);
      case TwoD.wheel:
        final z = ctl.sel.gradeZone;
        return Center(child: WheelMini(hue: e.value(grKey(z, 'h')), sat: e.value(grKey(z, 's')), size: w < h ? w : h));
      case TwoD.curve:
        final sel = ['H', 'L', 'D', 'S'].indexWhere((r) => cvKey(ctl.sel.curveCh, r) == row.id);
        return CurveMini(analysis: ctl.analysis(), edit: e, channel: ctl.sel.curveCh, selected: sel, width: w, height: h);
      case TwoD.offset:
        return OffsetPadView(x: e.value(row.padX!), y: e.value(row.padY!), labels: row.padLabels, width: w, height: h, eff: row.padEff, xMin: row.padXMin, notes: row.padNotes);
      case TwoD.tone:
        return const SizedBox.shrink();
    }
  }
}
