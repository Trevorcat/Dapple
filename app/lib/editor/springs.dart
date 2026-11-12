// Physics springs for the editor (requirements 4.15.13, guidelines 15.3). mass = 1 everywhere.
// Every spring is interruptible: retargeting keeps the current position AND velocity.
import 'package:flutter/foundation.dart';
import 'package:flutter/physics.dart';
import 'package:flutter/scheduler.dart';

/// A spring tier: displacement spring and the critically damped one used for opacity.
class SpringTier {
  const SpringTier(this.name, this.displacement, this.opacity);
  final String name;
  final SpringDescription displacement;
  final SpringDescription opacity;

  /// 150 ms: readout fade, highlight bar, auto-mark flash, pill <-> readout line.
  static const small = SpringTier(
    'small',
    SpringDescription(mass: 1, stiffness: 1100, damping: 40),
    SpringDescription(mass: 1, stiffness: 1100, damping: 66),
  );

  /// 200 ms: window expand / collapse, content swap, near snaps (<= 200 dp).
  static const medium = SpringTier(
    'medium',
    SpringDescription(mass: 1, stiffness: 600, damping: 30),
    SpringDescription(mass: 1, stiffness: 600, damping: 49),
  );

  /// 350 ms: handedness switch, long snaps (> 200 dp), page settle, photo avoidance shift.
  static const large = SpringTier(
    'large',
    SpringDescription(mass: 1, stiffness: 200, damping: 17),
    SpringDescription(mass: 1, stiffness: 200, damping: 28),
  );

  /// Distance based tier choice for window snapping.
  static SpringTier forDistance(double dp) => dp <= 200 ? medium : large;
}

/// UIScrollView style projection: rest position of a flick with decelerationRate 0.99.
double projectRest(double position, double velocity) => position + velocity * 0.099;

/// Rubber band: `(1 - 1 / (x * 0.55 / d + 1)) * d` for an overshoot [x] with range [d].
double rubberBand(double x, double d) => (1 - 1 / (x * 0.55 / d + 1)) * d;

/// A scalar driven by a [SpringSimulation]. [value] is the position; [velocity] its derivative.
/// With [reduceMotion] every animation becomes a 150 ms linear tween (no overshoot).
class SpringValue extends ChangeNotifier implements ValueListenable<double> {
  SpringValue(TickerProvider vsync, {double initial = 0, this.reduceMotion = false}) : _value = initial, _target = initial {
    _ticker = vsync.createTicker(_tick);
  }

  late final Ticker _ticker;
  double _value, _target, _velocity = 0;
  bool reduceMotion;
  SpringSimulation? _sim;
  Duration _t0 = Duration.zero;
  bool _started = false;
  // linear tween (reduced motion)
  double _from = 0;
  bool _linear = false;

  @override
  double get value => _value;
  double get velocity => _velocity;
  double get target => _target;
  bool get isAnimating => _ticker.isActive;

  /// Jumps (follow-the-finger phases): cancels any animation.
  void jumpTo(double v, {double velocity = 0}) {
    _ticker.stop();
    _sim = null;
    _value = v;
    _target = v;
    _velocity = velocity;
    notifyListeners();
  }

  /// Springs to [target] from the current position and velocity (or [velocity] when given).
  void animateTo(double target, {SpringDescription? spring, double? velocity}) {
    if (velocity != null) _velocity = velocity;
    _target = target;
    if (reduceMotion) {
      _linear = true;
      _from = _value;
      _sim = null;
    } else {
      _linear = false;
      _sim = SpringSimulation(spring ?? SpringTier.medium.displacement, _value, target, _velocity);
    }
    _started = false;
    if (!_ticker.isActive) {
      _ticker.start();
    }
  }

  void _tick(Duration elapsed) {
    if (!_started) {
      _started = true;
      _t0 = elapsed;
    }
    final t = (elapsed - _t0).inMicroseconds / 1e6;
    if (_linear) {
      final k = (t / 0.15).clamp(0.0, 1.0);
      _value = _from + (_target - _from) * k;
      _velocity = 0;
      if (k >= 1) _ticker.stop();
      notifyListeners();
      return;
    }
    final s = _sim!;
    if (s.isDone(t)) {
      _value = _target;
      _velocity = 0;
      _ticker.stop();
    } else {
      _value = s.x(t);
      _velocity = s.dx(t);
    }
    notifyListeners();
  }

  @override
  void dispose() {
    _ticker.dispose();
    super.dispose();
  }
}
