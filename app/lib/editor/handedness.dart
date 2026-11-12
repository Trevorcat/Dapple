// Passive handedness inference (requirements 4.15.12). No prompts, no calibration page, nothing
// leaves the device: only aggregate scores are kept. The decision only ever changes after the
// finger has lifted and the editor has been idle (the caller enforces idleness; [evaluate] enforces
// the statistics, the majority rule, hysteresis and the 5 minute cool-down).
import 'dart:math' as math;

import 'gesture_arbiter.dart';
import 'window_manager.dart';

/// Result of scoring one stroke or tap: [score] in [-1, +1] (+ = right hand), [confidence] in [0, 1].
class HandVote {
  const HandVote(this.score, this.confidence);
  final double score, confidence;
}

class HandednessInferrer {
  HandednessInferrer({required this.screenWidth, Hand initial = Hand.right, this.windowSize = 40}) : _side = initial;

  /// Logical width of the screen (updated on rotation / fold).
  double screenWidth;
  final int windowSize;

  Hand _side;
  Hand get side => _side;
  int _lastSwitchMs = -1 << 40;

  // ring of votes
  final List<double> _score = [];
  final List<double> _conf = [];
  // recent touch-down x positions (normalised) with times, for the two-handed check
  final List<double> _recentX = [];
  final List<int> _recentT = [];
  // orientation statistics (feature enabled only when the device reports it with spread)
  int _oN = 0;
  double _oSum = 0, _oSq = 0;

  int get sampleCount => _score.length;

  void reset() {
    _score.clear();
    _conf.clear();
    _recentX.clear();
    _recentT.clear();
    _oN = 0;
    _oSum = 0;
    _oSq = 0;
  }

  /// Arc feature: the centre of curvature of the stroke lies towards the pivot of the thumb. A right
  /// thumb pivots at the bottom right, so its arcs curve around a centre on the right half.
  static HandVote? arcVote(StrokeSample s, double screenWidth) {
    final ax = s.startX, ay = s.startY, bx = s.midX, by = s.midY, cx = s.endX, cy = s.endY;
    final d = 2 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
    if (d.abs() < 1e-6) return null; // straight line
    final a2 = ax * ax + ay * ay, b2 = bx * bx + by * by, c2 = cx * cx + cy * cy;
    final ux = (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / d;
    final uy = (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / d;
    final r = math.sqrt((ax - ux) * (ax - ux) + (ay - uy) * (ay - uy));
    final chord = math.sqrt((cx - ax) * (cx - ax) + (cy - ay) * (cy - ay));
    if (chord < 1) return null;
    final sagitta = ((cx - ax) * (by - ay) - (cy - ay) * (bx - ax)).abs() / chord;
    if (sagitta < 4 || r < 80 || r > 2000) return null;
    final score = ((ux - screenWidth / 2) / (screenWidth / 2)).clamp(-1.0, 1.0);
    return HandVote(score.toDouble(), (sagitta / 14).clamp(0.0, 1.0).toDouble());
  }

  /// Touch ellipse orientation: Android's touch orientation is 0 for a vertical major axis and
  /// negative when the top leans left (a right thumb comes in from the lower right).
  HandVote? _orientationVote(StrokeSample s) {
    if (s.radiusMajor <= 0 || s.radiusMajor == s.radiusMinor) return null;
    _oN++;
    _oSum += s.orientation;
    _oSq += s.orientation * s.orientation;
    if (_oN < 8) return null;
    final mean = _oSum / _oN;
    final varr = _oSq / _oN - mean * mean;
    if (varr < 0.0025) return null; // device reports a constant: no information
    final v = (-s.orientation / 0.5).clamp(-1.0, 1.0).toDouble();
    return HandVote(v, (s.orientation.abs() / 0.3).clamp(0.0, 1.0).toDouble());
  }

  /// Adds one qualifying stroke or tap. Returns the vote that was recorded (null if uninformative).
  HandVote? add(StrokeSample s, int nowMs) {
    // x distribution: mean touch-down position over recent samples
    final nx = (s.downX / screenWidth).clamp(0.0, 1.0);
    _recentX.add(nx);
    _recentT.add(nowMs);
    if (_recentX.length > windowSize) {
      _recentX.removeAt(0);
      _recentT.removeAt(0);
    }
    var sum = 0.0, wsum = 0.0, csum = 0.0;
    final arc = s.isTap ? null : arcVote(s, screenWidth);
    if (arc != null) {
      sum += 0.5 * arc.score;
      wsum += 0.5;
      csum += 0.5 * arc.confidence;
    }
    final ori = _orientationVote(s);
    if (ori != null) {
      sum += 0.3 * ori.score;
      wsum += 0.3;
      csum += 0.3 * ori.confidence;
    }
    var meanX = 0.0;
    for (final x in _recentX) {
      meanX += x;
    }
    meanX /= _recentX.length;
    final xv = ((meanX - 0.5) * 2 / 0.5).clamp(-1.0, 1.0).toDouble();
    sum += 0.2 * xv;
    wsum += 0.2;
    csum += 0.2 * (_recentX.length >= 6 ? 0.6 : 0.2);
    final vote = HandVote(sum / wsum, csum / wsum);
    _score.add(vote.score);
    _conf.add(vote.confidence);
    if (_score.length > windowSize) {
      _score.removeAt(0);
      _conf.removeAt(0);
    }
    return vote;
  }

  /// Confidence weighted mean of the recent scores, in [-1, +1].
  double get aggregate {
    var num = 0.0, den = 0.0;
    for (var i = 0; i < _score.length; i++) {
      num += _score[i] * _conf[i];
      den += _conf[i];
    }
    return den <= 1e-9 ? 0 : num / den;
  }

  bool _twoHanded(int nowMs) {
    var left = false, right = false;
    for (var i = 0; i < _recentX.length; i++) {
      if (nowMs - _recentT[i] > 2000) continue;
      if (_recentX[i] < 0.3) left = true;
      if (_recentX[i] > 0.7) right = true;
    }
    return left && right;
  }

  /// Majority vote with hysteresis: switches only with >= 12 samples, |S| >= 0.35 in favour of the
  /// other hand, outside the 5 minute cool-down and not while both sides are being used.
  /// Returns true when [side] changed.
  bool evaluate(int nowMs) {
    if (_score.length < 12) return false;
    if (nowMs - _lastSwitchMs < 5 * 60 * 1000) return false;
    if (_twoHanded(nowMs)) return false;
    final s = aggregate;
    if (_side == Hand.right && s <= -0.35) {
      _side = Hand.left;
    } else if (_side == Hand.left && s >= 0.35) {
      _side = Hand.right;
    } else {
      return false;
    }
    _lastSwitchMs = nowMs;
    return true;
  }

  void setSide(Hand h) {
    _side = h;
  }
}
