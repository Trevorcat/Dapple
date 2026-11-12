// Relative value mapping for the editor's gesture grammar (requirements 4.15.7 / 4.15.8).
// Pure Dart: no Flutter, no allocation in the per-move paths (everything is mutable state on the
// mapper objects), so it is unit-testable and cheap to call at pointer rate.
import 'dart:math' as math;

/// Landmark events a mapper reports after an update (haptics only happen at landmarks).
enum Landmark { none, autoSnap, detent, endStop }

double smoothstep(double x) {
  final t = x.clamp(0.0, 1.0);
  return t * t * (3 - 2 * t);
}

/// Velocity dependent control-display gain g(v): 0.35 (<= 40 dp/s) .. 2.5 (>= 900 dp/s).
double gainFor(double speedDpPerSec, {double slow = 0.35, double fast = 2.5}) {
  final v = speedDpPerSec.abs();
  return slow + (fast - slow) * smoothstep((v - 40) / (900 - 40));
}

/// Precision band from the vertical offset of the finger (reference: lock point).
enum PrecisionBand { standard, fine, ultra }

double bandMultiplier(PrecisionBand b) => switch (b) {
      PrecisionBand.standard => 1,
      PrecisionBand.fine => 0.25,
      PrecisionBand.ultra => 1 / 16,
    };

String bandLabel(PrecisionBand b) => switch (b) {
      PrecisionBand.standard => '',
      PrecisionBand.fine => '精细 ×¼',
      PrecisionBand.ultra => '极细 ×1/16',
    };

/// Tracks the band with +-12 dp hysteresis. Thresholds scale with the available height.
class BandTracker {
  BandTracker({double availableHeight = 800}) : scale = (availableHeight / 800).clamp(0.6, 1.0).toDouble();

  final double scale;
  static const double hysteresis = 12;
  PrecisionBand band = PrecisionBand.standard;

  double get _fineAt => 64 * scale;
  double get _ultraAt => 160 * scale;

  void reset() => band = PrecisionBand.standard;

  PrecisionBand update(double dy) {
    final a = dy.abs();
    switch (band) {
      case PrecisionBand.standard:
        if (a > _fineAt + hysteresis) band = a > _ultraAt + hysteresis ? PrecisionBand.ultra : PrecisionBand.fine;
      case PrecisionBand.fine:
        if (a > _ultraAt + hysteresis) {
          band = PrecisionBand.ultra;
        } else if (a < _fineAt - hysteresis) {
          band = PrecisionBand.standard;
        }
      case PrecisionBand.ultra:
        if (a < _ultraAt - hysteresis) band = a < _fineAt - hysteresis ? PrecisionBand.standard : PrecisionBand.fine;
    }
    return band;
  }
}

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

/// Linear ramp used so gain / band changes never jump (80 ms).
class _Ramp {
  double value = 1;
  void jump(double v) => value = v;
  double step(double target, double dtMs) {
    if (dtMs <= 0) return value;
    final k = (dtMs / 80).clamp(0.0, 1.0);
    value += (target - value) * k;
    return value;
  }
}

/// One-dimensional relative mapper: `delta = dx * g(v) * b * range / 320`.
class RelativeMapper {
  RelativeMapper({required this.min, required this.max, required this.auto, this.step = 1});

  final double min, max, auto, step;
  double get range => max - min;

  double _raw = 0; // un-snapped value
  double _out = 0; // value after snapping / clamping
  bool _snapped = false;
  double _snapTravel = 0;
  bool _noCapture = false; // must leave the capture window (x2) before capturing again
  bool _endArmed = true;
  final _Ramp _gain = _Ramp()..jump(0.35);
  final _Ramp _bandMul = _Ramp()..jump(1);

  /// -1 / +1 while pressed against a range end (readout rubber band), else 0.
  int endSide = 0;
  PrecisionBand band = PrecisionBand.standard;
  Landmark landmark = Landmark.none;

  /// True while the value is held on the auto position.
  bool get snapped => _snapped;

  /// Exact value (float).
  double get value => _out;

  /// Value quantised to the parameter step (what is sent to the engine).
  double get quantized => quantize(_out);

  double quantize(double v) => (((v - min) / step).round() * step + min).clamp(min, max).toDouble();

  void begin(double startValue) {
    _raw = startValue.clamp(min, max).toDouble();
    _out = _raw;
    _snapped = false;
    _snapTravel = 0;
    _noCapture = (_raw - auto).abs() <= captureWindow(PrecisionBand.standard) * range * 2;
    _endArmed = true;
    endSide = 0;
    band = PrecisionBand.standard;
    _gain.jump(0.35);
    _bandMul.jump(1);
    landmark = Landmark.none;
  }

  /// Capture window as a fraction of the range: 1.5 % / 0.5 % / off.
  static double captureWindow(PrecisionBand b) => switch (b) {
        PrecisionBand.standard => 0.015,
        PrecisionBand.fine => 0.005,
        PrecisionBand.ultra => 0,
      };

  /// [dx]: finger movement along the axis since the last sample (dp, signed, oriented so that
  /// "increase" is positive). [speed]: |velocity| along the axis (dp/s). [dtMs]: time since the
  /// previous sample. Returns the (snapped) value.
  double update(double dx, double speed, PrecisionBand newBand, double dtMs) {
    landmark = Landmark.none;
    band = newBand;
    final g = _gain.step(gainFor(speed), dtMs);
    final b = _bandMul.step(bandMultiplier(newBand), dtMs);
    final cap = captureWindow(newBand) * range;
    final delta = dx * g * b * range / 320;

    if (_snapped) {
      _snapTravel += dx.abs();
      final breakout = newBand == PrecisionBand.standard ? 10.0 : 5.0;
      if (_snapTravel > breakout) {
        _snapped = false;
        _noCapture = true;
        _raw = auto + delta; // continue from the auto position, no jump
      }
    } else {
      _raw += delta;
    }
    if (_raw > max) _raw = max;
    if (_raw < min) _raw = min;

    if (!_snapped) {
      final d = (_raw - auto).abs();
      if (_noCapture && d > cap * 2) _noCapture = false;
      if (!_noCapture && cap > 0 && d <= cap) {
        _snapped = true;
        _snapTravel = 0;
        landmark = Landmark.autoSnap;
      }
    }
    _out = _snapped ? auto : _raw;

    final atMin = _raw <= min, atMax = _raw >= max;
    if ((atMin || atMax) && _endArmed) {
      _endArmed = false;
      landmark = Landmark.endStop;
    }
    endSide = atMax ? 1 : (atMin ? -1 : 0);
    if (!atMin && !atMax && !_endArmed && (_raw - min) > range * 0.05 && (max - _raw) > range * 0.05) {
      _endArmed = true;
    }
    return _out;
  }

  /// Jumps to [v] (reset, numeric entry, accessibility step) keeping the mapper consistent.
  void set(double v) {
    _raw = v.clamp(min, max).toDouble();
    _out = _raw;
    _snapped = false;
    _noCapture = true;
  }
}

/// Discrete stepping: one item per [stepDp] of finger travel with 25 % reversal hysteresis.
/// Used for discrete rows (48 dp), parameter switching (40 dp) and 3x3 cells (56 dp per axis).
class DetentStepper {
  DetentStepper({required this.stepDp, required this.count, this.reverseFraction = 0.25});
  final double stepDp;
  final int count;
  final double reverseFraction;

  int index = 0;
  double _anchor = 0, _pos = 0;
  int _dir = 0;
  Landmark landmark = Landmark.none;

  void begin(int startIndex) {
    index = startIndex.clamp(0, count - 1);
    _anchor = 0;
    _pos = 0;
    _dir = 0;
    landmark = Landmark.none;
  }

  /// [delta]: finger movement along the axis since the last sample, positive = towards the next
  /// index. Returns true when [index] changed.
  bool update(double delta) {
    landmark = Landmark.none;
    _pos += delta;
    var changed = false;
    for (var guard = 0; guard < 64; guard++) {
      final d = _pos - _anchor;
      final fwd = _dir >= 0 ? stepDp : stepDp * reverseFraction;
      final back = _dir <= 0 ? stepDp : stepDp * reverseFraction;
      if (d >= fwd) {
        if (index < count - 1) {
          index++;
          landmark = Landmark.detent;
          changed = true;
        } else {
          landmark = Landmark.endStop;
        }
        _anchor += fwd;
        _dir = 1;
      } else if (-d >= back) {
        if (index > 0) {
          index--;
          landmark = Landmark.detent;
          changed = true;
        } else {
          landmark = Landmark.endStop;
        }
        _anchor -= back;
        _dir = -1;
      } else {
        break;
      }
    }
    return changed;
  }
}

/// 2D relative mapper for the colour palette (0.04..0.96 on both axes, 300 dp = full range).
class PaletteMapper {
  PaletteMapper({required this.defX, required this.defY, this.lo = 0.04, this.hi = 0.96, double? loX, double? hiX, double? loY, double? hiY})
      : loX = loX ?? lo,
        hiX = hiX ?? hi,
        loY = loY ?? lo,
        hiY = hiY ?? hi;
  final double defX, defY;

  /// Reachable range on both axes (0.04..0.96 for the colour palette, 0..1 for the film offset pads) and, when an
  /// axis end is unreachable (an ineffective half of a film pad), per axis: the cursor stops there.
  final double lo, hi;
  final double loX, hiX, loY, hiY;

  double x = 0.5, y = 0.5;
  bool snapped = false;
  double _snapTravel = 0;
  bool _noCapture = false;
  double _rx = 0.5, _ry = 0.5;
  bool _endX = true, _endY = true;
  final _Ramp _gain = _Ramp()..jump(0.25);
  Landmark landmark = Landmark.none;

  void begin(double sx, double sy) {
    _rx = x = sx.clamp(loX, hiX).toDouble();
    _ry = y = sy.clamp(loY, hiY).toDouble();
    snapped = false;
    _snapTravel = 0;
    _noCapture = _dist(x, y) <= 0.03 * 2;
    _endX = _endY = true;
    _gain.jump(0.25);
    landmark = Landmark.none;
  }

  double _dist(double a, double b) => math.sqrt((a - defX) * (a - defX) + (b - defY) * (b - defY));

  /// [fineScale]: 1, or 0.25 when the mini-map "fine" switch is on.
  void update(double dx, double dy, double speed, double dtMs, {double fineScale = 1}) {
    landmark = Landmark.none;
    final g = _gain.step(gainFor(speed, slow: 0.25), dtMs) * fineScale;
    final k = (hi - lo) / 300 * g;
    if (snapped) {
      _snapTravel += math.sqrt(dx * dx + dy * dy);
      if (_snapTravel > 10) {
        snapped = false;
        _noCapture = true;
        _rx = defX + dx * k;
        _ry = defY + dy * k;
      }
    } else {
      _rx += dx * k;
      _ry += dy * k;
    }
    var hitEnd = false;
    if (_rx > hiX || _rx < loX) {
      _rx = _rx.clamp(loX, hiX).toDouble();
      if (_endX) {
        hitEnd = true;
        _endX = false;
      }
    } else if (_rx > loX + 0.05 && _rx < hiX - 0.05) {
      _endX = true;
    }
    if (_ry > hiY || _ry < loY) {
      _ry = _ry.clamp(loY, hiY).toDouble();
      if (_endY) {
        hitEnd = true;
        _endY = false;
      }
    } else if (_ry > loY + 0.05 && _ry < hiY - 0.05) {
      _endY = true;
    }
    if (!snapped) {
      final d = _dist(_rx, _ry);
      if (_noCapture && d > 0.06) _noCapture = false;
      if (!_noCapture && d <= 0.03) {
        snapped = true;
        _snapTravel = 0;
        landmark = Landmark.autoSnap;
      }
    }
    if (hitEnd && landmark == Landmark.none) landmark = Landmark.endStop;
    x = snapped ? defX : _rx;
    y = snapped ? defY : _ry;
  }
}

/// Grading wheel: relative movement on the unit disc. Hue = atan2 (screen coordinates, y down),
/// saturation = radius * 100. Passing through the centre flips the hue naturally; at s = 0 the
/// previous hue is kept.
class WheelMapper {
  double px = 0, py = 0;
  double hue = 0; // degrees 0..360
  double sat = 0; // 0..100
  bool snapped = false;
  double _snapTravel = 0;
  bool _noCapture = false;
  bool _endArmed = true;
  final _Ramp _gain = _Ramp()..jump(0.35);
  Landmark landmark = Landmark.none;

  void begin(double hueDeg, double satPct) {
    hue = hueDeg;
    sat = satPct;
    final r = satPct / 100;
    final a = hueDeg * math.pi / 180;
    px = math.cos(a) * r;
    py = math.sin(a) * r;
    snapped = false;
    _snapTravel = 0;
    _noCapture = r <= 0.08;
    _endArmed = true;
    _gain.jump(0.35);
    landmark = Landmark.none;
  }

  void update(double dx, double dy, double speed, double dtMs, {double fineScale = 1}) {
    landmark = Landmark.none;
    final g = _gain.step(gainFor(speed), dtMs) * fineScale;
    final k = g / 160;
    if (snapped) {
      _snapTravel += math.sqrt(dx * dx + dy * dy);
      if (_snapTravel > 10) {
        snapped = false;
        _noCapture = true;
        px = dx * k;
        py = dy * k;
      }
    } else {
      px += dx * k;
      py += dy * k;
    }
    var r = math.sqrt(px * px + py * py);
    if (r >= 1) {
      px /= r;
      py /= r;
      r = 1;
      if (_endArmed) {
        _endArmed = false;
        landmark = Landmark.endStop;
      }
    } else if (r < 0.9) {
      _endArmed = true;
    }
    if (!snapped) {
      if (_noCapture && r > 0.08) _noCapture = false;
      if (!_noCapture && r <= 0.04) {
        snapped = true;
        _snapTravel = 0;
        landmark = Landmark.autoSnap;
      }
    }
    if (snapped) {
      sat = 0;
      px = 0;
      py = 0;
    } else {
      sat = r * 100;
      if (r > 1e-4) {
        var h = math.atan2(py, px) * 180 / math.pi;
        if (h < 0) h += 360;
        hue = h;
      }
    }
  }
}
