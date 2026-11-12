// Single-pointer-stream gesture arbitration for the editor photo surface (requirements 4.15.6).
//
// One explicit state machine decides, from raw pointer events, between: tap / double tap, long
// press (compare), one-finger adjust (horizontal value / vertical parameter switch / free 2D),
// browse pan and page swipe, and the two-finger pinch / pan. Nothing here knows about widgets:
// the surface widget feeds pointer events and a clock, and a [GestureSink] receives intents.
// This replaces stacked GestureDetectors that would fight in the gesture arena.
import 'dart:math' as math;
import 'dart:typed_data';

import 'value_mapping.dart';

enum ArbiterMode { browse, adjust }

/// What the active parameter looks like to the gesture layer.
enum ActiveKind { none, oneD, discrete, twoD }

enum LockAxis { none, horizontal, vertical }

enum ArbiterState { idle, pending, adjustH, adjustV, adjust2D, panning, paging, ignored, longPress, multi, afterMulti }

class ArbiterConfig {
  const ArbiterConfig({
    this.slopDp = 10,
    this.ambiguousDp = 16,
    this.horizontalDeg = 35,
    this.verticalDeg = 55,
    this.longPressMs = 380,
    this.doubleTapMs = 280,
    this.doubleTapDp = 40,
    this.strokeSampleDp = 60,
  });
  final double slopDp, ambiguousDp, horizontalDeg, verticalDeg;
  final int longPressMs, doubleTapMs;
  final double doubleTapDp, strokeSampleDp;
}

/// One qualifying stroke / tap, used for passive handedness inference.
class StrokeSample {
  const StrokeSample({
    required this.isTap,
    required this.downX,
    required this.startX,
    required this.startY,
    required this.midX,
    required this.midY,
    required this.endX,
    required this.endY,
    required this.orientation,
    required this.radiusMajor,
    required this.radiusMinor,
  });
  final bool isTap;
  final double downX, startX, startY, midX, midY, endX, endY;

  /// Touch ellipse orientation (radians) and radii, 0 when the device does not report them.
  final double orientation, radiusMajor, radiusMinor;
}

/// Receiver of arbitrated intents plus the context the arbiter needs to decide.
abstract class GestureSink {
  // ---- context ----
  ArbiterMode get mode;
  ActiveKind get activeKind;
  bool get zoomed;

  /// False while a screen reader explores by touch: one-finger drags must not change values.
  bool get adjustEnabled => true;

  // ---- adjust (one finger, adjust mode) ----
  /// A one-finger adjust gesture locked. [axis] is none for 2D controls.
  void adjustBegin(LockAxis axis, double x, double y);

  /// [dx]/[dy]: movement since the previous sample; [offX]/[offY]: offset from the lock point;
  /// [vx]/[vy]: velocity (dp/s, last 60 ms); [dtMs]: time since the previous sample.
  void adjustMove(double dx, double dy, double offX, double offY, double vx, double vy, double dtMs);

  /// [byMultiTouch]: a second finger landed (the applied value is kept, no rollback).
  void adjustEnd({required bool byMultiTouch});

  // ---- browse ----
  void panBy(double dx, double dy);
  void panEnd(double vx, double vy);
  void pageDrag(double dx);

  /// Finger lifted after a page drag: [totalDx] since the drag began, [vx] release velocity.
  void pageEnd(double totalDx, double vx);
  void pageCancel();

  // ---- compare / zoom ----
  void longPress(bool on);
  void scaleStart(double fx, double fy);
  void scaleUpdate(double fx, double fy, double scale);
  void scaleEnd(double vx, double vy);

  // ---- taps ----
  void tap(double x, double y);
  void doubleTap(double x, double y);

  // ---- bookkeeping ----
  void strokeEnded(StrokeSample s) {}
  void pointerActivity({required bool down}) {}
}

class _P {
  int id = -1;
  double x = 0, y = 0;
  bool get active => id >= 0;
}

class GestureArbiter {
  GestureArbiter(this.sink, {this.config = const ArbiterConfig()});
  final GestureSink sink;
  final ArbiterConfig config;

  ArbiterState _state = ArbiterState.idle;
  ArbiterState get state => _state;

  final _P _a = _P(), _b = _P();
  int get _count => (_a.active ? 1 : 0) + (_b.active ? 1 : 0);

  // single-finger bookkeeping
  double _downX = 0, _downY = 0, _lastX = 0, _lastY = 0, _refX = 0, _refY = 0;
  int _downT = 0, _lastT = 0;
  bool _slopped = false;
  double _pageTotal = 0;
  final VelocityWindow _vel = VelocityWindow();
  double _orient = 0, _rMajor = 0, _rMinor = 0;

  // tap tracking
  int _pendingTapAt = -1; // deadline (ms) of an undelivered single tap
  double _tapX = 0, _tapY = 0;
  double _prevTapX = 0, _prevTapY = 0;
  int _prevTapT = -100000;
  bool _hasPrevTap = false;

  // two-finger
  double _dist0 = 1;
  double _fx0 = 0, _fy0 = 0;

  // stroke path buffer (decimated, fixed size, no per-move allocation)
  static const int _cap = 96;
  final Float64List _path = Float64List(_cap * 2);
  int _pathN = 0, _pathSkip = 1, _pathCtr = 0;

  /// Next time (ms) at which [tick] must be called, or null.
  int? get nextDeadline {
    int? d;
    if (_state == ArbiterState.pending && !_slopped) d = _downT + config.longPressMs;
    if (_pendingTapAt >= 0) d = d == null ? _pendingTapAt : math.min(d, _pendingTapAt);
    return d;
  }

  void _pathReset(double x, double y) {
    _pathN = 0;
    _pathSkip = 1;
    _pathCtr = 0;
    _pathAdd(x, y);
  }

  void _pathAdd(double x, double y) {
    if (_pathCtr++ % _pathSkip != 0) return;
    if (_pathN == _cap) {
      var w = 0;
      for (var r = 0; r < _cap; r += 2) {
        _path[w * 2] = _path[r * 2];
        _path[w * 2 + 1] = _path[r * 2 + 1];
        w++;
      }
      _pathN = w;
      _pathSkip *= 2;
    }
    _path[_pathN * 2] = x;
    _path[_pathN * 2 + 1] = y;
    _pathN++;
  }

  void _emitStroke({required bool tap, required double ex, required double ey}) {
    var mx = (_downX + ex) / 2, my = (_downY + ey) / 2;
    if (!tap && _pathN > 2) {
      final cx = ex - _downX, cy = ey - _downY;
      final len = math.sqrt(cx * cx + cy * cy);
      var best = -1.0;
      if (len > 1e-3) {
        for (var i = 0; i < _pathN; i++) {
          final px = _path[i * 2], py = _path[i * 2 + 1];
          final dev = ((cx * (py - _downY) - cy * (px - _downX)) / len).abs();
          if (dev > best) {
            best = dev;
            mx = px;
            my = py;
          }
        }
      }
    }
    sink.strokeEnded(StrokeSample(
      isTap: tap,
      downX: _downX,
      startX: _downX,
      startY: _downY,
      midX: mx,
      midY: my,
      endX: ex,
      endY: ey,
      orientation: _orient,
      radiusMajor: _rMajor,
      radiusMinor: _rMinor,
    ));
  }

  // ------------------------------------------------------------ pointer input

  void pointerDown(int id, double x, double y, int tMs, {double orientation = 0, double radiusMajor = 0, double radiusMinor = 0}) {
    sink.pointerActivity(down: true);
    if (_count >= 2) return; // a third finger is ignored
    if (_count == 1) {
      _secondFinger(id, x, y, tMs);
      return;
    }
    _a
      ..id = id
      ..x = x
      ..y = y;
    _downX = _lastX = x;
    _downY = _lastY = y;
    _downT = _lastT = tMs;
    _slopped = false;
    _pageTotal = 0;
    _orient = orientation;
    _rMajor = radiusMajor;
    _rMinor = radiusMinor;
    _vel
      ..clear()
      ..add(tMs, x, y);
    _pathReset(x, y);
    _state = ArbiterState.pending;
  }

  void _secondFinger(int id, double x, double y, int tMs) {
    // Anything single-finger ends here without rollback; a pending tap is voided.
    _endSingle(byMultiTouch: true);
    _pendingTapAt = -1;
    _hasPrevTap = false;
    _b
      ..id = id
      ..x = x
      ..y = y;
    _dist0 = math.max(1.0, _dist());
    _fx0 = (_a.x + _b.x) / 2;
    _fy0 = (_a.y + _b.y) / 2;
    _vel
      ..clear()
      ..add(tMs, _fx0, _fy0);
    _state = ArbiterState.multi;
    sink.scaleStart(_fx0, _fy0);
  }

  double _dist() {
    final dx = _a.x - _b.x, dy = _a.y - _b.y;
    return math.sqrt(dx * dx + dy * dy);
  }

  void _endSingle({required bool byMultiTouch}) {
    switch (_state) {
      case ArbiterState.adjustH:
      case ArbiterState.adjustV:
      case ArbiterState.adjust2D:
        sink.adjustEnd(byMultiTouch: byMultiTouch);
      case ArbiterState.longPress:
        sink.longPress(false);
      case ArbiterState.paging:
        sink.pageCancel();
      case ArbiterState.panning:
        sink.panEnd(0, 0);
      default:
        break;
    }
  }

  void pointerMove(int id, double x, double y, int tMs) {
    if (_state == ArbiterState.multi) {
      if (id == _a.id) {
        _a.x = x;
        _a.y = y;
      } else if (id == _b.id) {
        _b.x = x;
        _b.y = y;
      } else {
        return;
      }
      final fx = (_a.x + _b.x) / 2, fy = (_a.y + _b.y) / 2;
      _vel.add(tMs, fx, fy);
      sink.scaleUpdate(fx, fy, _dist() / _dist0);
      return;
    }
    if (id != _a.id) return;
    _a.x = x;
    _a.y = y;
    _vel.add(tMs, x, y);
    _pathAdd(x, y);
    switch (_state) {
      case ArbiterState.pending:
        _movePending(x, y, tMs);
      case ArbiterState.adjustH:
      case ArbiterState.adjustV:
      case ArbiterState.adjust2D:
        _vel.compute(tMs);
        final dt = (tMs - _lastT).toDouble();
        sink.adjustMove(x - _lastX, y - _lastY, x - _refX, y - _refY, _vel.vx, _vel.vy, dt);
      case ArbiterState.panning:
        sink.panBy(x - _lastX, y - _lastY);
      case ArbiterState.paging:
        final dx = x - _lastX;
        _pageTotal += dx;
        sink.pageDrag(dx);
      default:
        break;
    }
    _lastX = x;
    _lastY = y;
    _lastT = tMs;
  }

  void _movePending(double x, double y, int tMs) {
    final dx = x - _downX, dy = y - _downY;
    final disp = math.sqrt(dx * dx + dy * dy);
    if (disp < config.slopDp) return;
    _slopped = true; // cancels the long press
    _flushTapBeforeGesture();
    final adx = dx.abs(), ady = dy.abs();
    final theta = math.atan2(ady, adx) * 180 / math.pi;
    final adjust = sink.mode == ArbiterMode.adjust && sink.adjustEnabled;
    if (adjust) {
      final kind = sink.activeKind;
      if (kind == ActiveKind.twoD) {
        _lock(ArbiterState.adjust2D, LockAxis.none, x, y, tMs);
        return;
      }
      if (kind == ActiveKind.none) {
        _state = ArbiterState.ignored;
        return;
      }
      final axis = _decideAxis(theta, disp, adx, ady);
      if (axis == LockAxis.none) return; // keep watching until 16 dp
      _lock(axis == LockAxis.horizontal ? ArbiterState.adjustH : ArbiterState.adjustV, axis, x, y, tMs);
      return;
    }
    // browse (or exploring by touch: browsing only)
    if (sink.zoomed) {
      _state = ArbiterState.panning;
      sink.panBy(dx, dy);
      _lastX = x;
      _lastY = y;
      return;
    }
    final axis = _decideAxis(theta, disp, adx, ady);
    if (axis == LockAxis.none) return;
    if (axis == LockAxis.horizontal) {
      _state = ArbiterState.paging;
      _pageTotal = dx;
      sink.pageDrag(dx);
    } else {
      _state = ArbiterState.ignored; // vertical at fit zoom: no action
    }
  }

  LockAxis _decideAxis(double theta, double disp, double adx, double ady) {
    if (theta < config.horizontalDeg) return LockAxis.horizontal;
    if (theta > config.verticalDeg) return LockAxis.vertical;
    if (disp >= config.ambiguousDp) return adx >= ady ? LockAxis.horizontal : LockAxis.vertical;
    return LockAxis.none;
  }

  void _lock(ArbiterState s, LockAxis axis, double x, double y, int tMs) {
    _state = s;
    _refX = x;
    _refY = y;
    _lastX = x;
    _lastY = y;
    _lastT = tMs;
    sink.adjustBegin(axis, x, y);
  }

  void pointerUp(int id, double x, double y, int tMs) {
    sink.pointerActivity(down: false);
    if (_state == ArbiterState.multi) {
      if (id != _a.id && id != _b.id) return;
      _vel.compute(tMs);
      sink.scaleEnd(_vel.vx, _vel.vy);
      if (id == _a.id) _a.id = -1;
      if (id == _b.id) _b.id = -1;
      _state = _count == 0 ? ArbiterState.idle : ArbiterState.afterMulti;
      return;
    }
    if (_state == ArbiterState.afterMulti) {
      if (id == _a.id) _a.id = -1;
      if (id == _b.id) _b.id = -1;
      if (_count == 0) _state = ArbiterState.idle;
      return;
    }
    if (id != _a.id) return;
    _a.id = -1;
    final s = _state;
    _vel.add(tMs, x, y);
    _vel.compute(tMs);
    switch (s) {
      case ArbiterState.pending:
        _state = ArbiterState.idle;
        _tapUp(x, y, tMs);
      case ArbiterState.adjustH:
      case ArbiterState.adjustV:
      case ArbiterState.adjust2D:
        final dx = x - _downX, dy = y - _downY;
        if (s != ArbiterState.adjustV && math.sqrt(dx * dx + dy * dy) >= config.strokeSampleDp) {
          _emitStroke(tap: false, ex: x, ey: y);
        }
        _state = ArbiterState.idle;
        sink.adjustEnd(byMultiTouch: false);
      case ArbiterState.longPress:
        _state = ArbiterState.idle;
        sink.longPress(false);
      case ArbiterState.panning:
        _state = ArbiterState.idle;
        sink.panEnd(_vel.vx, _vel.vy);
      case ArbiterState.paging:
        _state = ArbiterState.idle;
        sink.pageEnd(_pageTotal, _vel.vx);
      default:
        _state = ArbiterState.idle;
    }
  }

  void pointerCancel(int id, int tMs) {
    sink.pointerActivity(down: false);
    if (id != _a.id && id != _b.id) return;
    if (_state == ArbiterState.multi) {
      sink.scaleEnd(0, 0);
    } else if (id == _a.id) {
      _endSingle(byMultiTouch: false);
    }
    if (id == _a.id) _a.id = -1;
    if (id == _b.id) _b.id = -1;
    _state = _count == 0 ? ArbiterState.idle : ArbiterState.afterMulti;
  }

  // ------------------------------------------------------------ taps

  void _tapUp(double x, double y, int tMs) {
    if (_hasPrevTap &&
        tMs - _prevTapT <= config.doubleTapMs &&
        math.sqrt((x - _prevTapX) * (x - _prevTapX) + (y - _prevTapY) * (y - _prevTapY)) <= config.doubleTapDp) {
      _hasPrevTap = false;
      _pendingTapAt = -1; // the double tap swallows the single tap
      sink.doubleTap(x, y);
      return;
    }
    _emitStroke(tap: true, ex: x, ey: y);
    _hasPrevTap = true;
    _prevTapT = tMs;
    _prevTapX = x;
    _prevTapY = y;
    _pendingTapAt = tMs + config.doubleTapMs;
    _tapX = x;
    _tapY = y;
  }

  /// A new non-tap gesture started: the earlier single tap is delivered first (never reordered).
  void _flushTapBeforeGesture() {
    if (_pendingTapAt >= 0) {
      _pendingTapAt = -1;
      _hasPrevTap = false;
      sink.tap(_tapX, _tapY);
    }
  }

  /// Advances time-based decisions: long press and the deferred single tap.
  void tick(int nowMs) {
    if (_state == ArbiterState.pending && !_slopped && nowMs - _downT >= config.longPressMs) {
      _flushTapBeforeGesture();
      _state = ArbiterState.longPress;
      sink.longPress(true);
    }
    if (_pendingTapAt >= 0 && nowMs >= _pendingTapAt) {
      _pendingTapAt = -1;
      _hasPrevTap = false;
      sink.tap(_tapX, _tapY);
    }
  }

  /// Drops everything (the surface is being disposed or the screen changed under the finger).
  void reset() {
    _endSingle(byMultiTouch: false);
    _a.id = -1;
    _b.id = -1;
    _state = ArbiterState.idle;
    _pendingTapAt = -1;
    _hasPrevTap = false;
  }
}
