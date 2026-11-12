// The one pointer listener under all editor overlays. Raw pointer events go straight into the
// [GestureArbiter] state machine (no gesture arena, so nothing can steal or compete); a single timer
// serves its deadlines (long press, deferred single tap). Touches that start inside the system
// gesture insets are left to the system and never enter the arbiter (requirements 4.15.15).
import 'dart:async';

import 'package:flutter/widgets.dart';

import '../../editor/gesture_arbiter.dart';

class EditorGestureSurface extends StatefulWidget {
  const EditorGestureSurface({super.key, required this.sink, required this.child, this.onArbiter});
  final GestureSink sink;
  final Widget child;

  /// Test hook: receives the arbiter so tests can inspect its state.
  final ValueChanged<GestureArbiter>? onArbiter;

  @override
  State<EditorGestureSurface> createState() => _EditorGestureSurfaceState();
}

class _EditorGestureSurfaceState extends State<EditorGestureSurface> {
  late final GestureArbiter _arbiter = GestureArbiter(widget.sink);
  final Set<int> _ignored = {};
  Timer? _timer;
  int _lastMs = 0;

  @override
  void initState() {
    super.initState();
    widget.onArbiter?.call(_arbiter);
  }

  @override
  void dispose() {
    _timer?.cancel();
    _arbiter.reset();
    super.dispose();
  }

  bool _inSystemZone(Offset p) {
    final box = context.findRenderObject() as RenderBox?;
    if (box == null || !box.hasSize) return false;
    final ins = MediaQuery.systemGestureInsetsOf(context);
    final s = box.size;
    return p.dx < ins.left || p.dx > s.width - ins.right || p.dy > s.height - ins.bottom;
  }

  void _arm() {
    _timer?.cancel();
    final d = _arbiter.nextDeadline;
    if (d == null) return;
    final wait = (d - _lastMs).clamp(0, 1 << 30);
    _timer = Timer(Duration(milliseconds: wait), () {
      _lastMs += wait;
      _arbiter.tick(_lastMs);
      _arm();
    });
  }

  void _down(PointerDownEvent e) {
    if (_inSystemZone(e.localPosition)) {
      _ignored.add(e.pointer);
      return;
    }
    _lastMs = e.timeStamp.inMilliseconds;
    _arbiter.pointerDown(e.pointer, e.localPosition.dx, e.localPosition.dy, _lastMs,
        orientation: e.orientation, radiusMajor: e.radiusMajor, radiusMinor: e.radiusMinor);
    _arm();
  }

  void _move(PointerMoveEvent e) {
    if (_ignored.contains(e.pointer)) return;
    _lastMs = e.timeStamp.inMilliseconds;
    _arbiter.pointerMove(e.pointer, e.localPosition.dx, e.localPosition.dy, _lastMs);
    _arm();
  }

  void _up(PointerUpEvent e) {
    if (_ignored.remove(e.pointer)) return;
    _lastMs = e.timeStamp.inMilliseconds;
    _arbiter.pointerUp(e.pointer, e.localPosition.dx, e.localPosition.dy, _lastMs);
    _arm();
  }

  void _cancel(PointerCancelEvent e) {
    if (_ignored.remove(e.pointer)) return;
    _lastMs = e.timeStamp.inMilliseconds;
    _arbiter.pointerCancel(e.pointer, _lastMs);
    _arm();
  }

  @override
  Widget build(BuildContext context) => Listener(
        behavior: HitTestBehavior.opaque,
        onPointerDown: _down,
        onPointerMove: _move,
        onPointerUp: _up,
        onPointerCancel: _cancel,
        child: widget.child,
      );
}
