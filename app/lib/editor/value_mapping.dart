// Small gesture helpers shared by the editor: the landmark kinds haptics fire on, and a finger velocity window. (The relative
// value mappers of v0.5 are gone: every control maps the finger absolutely, see docs/design/editor-panel.md.)
import 'dart:math' as math;

/// Landmark events (haptics only happen at landmarks).
enum Landmark { none, autoSnap, detent, endStop }

/// Finger velocity over the last ~60 ms, from a fixed ring buffer (no allocation).
class VelocityWindow {
  VelocityWindow({this.windowMs = 60});
  final int windowMs;
  static const int _cap = 32;
  final List<int> _t = List<int>.filled(_cap, 0);
  final List<double> _x = List<double>.filled(_cap, 0);
  final List<double> _y = List<double>.filled(_cap, 0);
  int _n = 0, _head = 0;
  double _vx = 0, _vy = 0;

  void clear() {
    _n = 0;
    _head = 0;
    _vx = 0;
    _vy = 0;
  }

  void add(int tMs, double x, double y) {
    _t[_head] = tMs;
    _x[_head] = x;
    _y[_head] = y;
    _head = (_head + 1) % _cap;
    if (_n < _cap) _n++;
  }

  double get vx => _vx;
  double get vy => _vy;
  double get speed => math.sqrt(_vx * _vx + _vy * _vy);

  /// Recomputes [vx] / [vy] (dp per second) from the samples inside the window ending at [nowMs].
  void compute(int nowMs) {
    _vx = 0;
    _vy = 0;
    if (_n < 2) return;
    final newest = (_head - 1 + _cap) % _cap;
    var oldest = newest;
    for (var k = 1; k < _n; k++) {
      final i = (newest - k + _cap) % _cap;
      if (nowMs - _t[i] > windowMs) break;
      oldest = i;
    }
    final dt = _t[newest] - _t[oldest];
    if (dt <= 0) return;
    _vx = (_x[newest] - _x[oldest]) / dt * 1000;
    _vy = (_y[newest] - _y[oldest]) / dt * 1000;
  }
}
