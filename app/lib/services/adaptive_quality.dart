import 'dart:math' as math;

/// Dynamic resolution for drag-time frames.
///
/// A drag frame costs (size-independent overhead: bake / tables / encode / dispatch, all CPU) + (pixel part: GPU execution
/// and readback / present, which scales with the number of pixels). Shrinking the picture only buys back the pixel part, so
/// the controller sizes the preview from that part alone. The engine reports one number per frame (render ms, which includes
/// the tables), so the split is modelled: pixel part = [pixelMsPerMpx] x megapixels of the rung (prior: the engine's warm-up
/// film-frame measurement, [seedFromMeasurement]); overhead = smoothed measured frame time minus the pixel part of the rung it
/// ran at.
///
/// Policy (pure Dart, unit tested):
///  * allowance = budget - overhead, kept within [0.5, 1] x budget: when the overhead alone is over the budget, shrinking
///    cannot help, so the pixel part is still allowed up to half a refresh instead of collapsing to the cheapest rung;
///  * the target is the largest rung whose pixel part fits the allowance;
///  * step down after [downAfter] consecutive frames over budget while the target is a smaller rung (frames over budget
///    because of the overhead do not count: the target is the rung they already run at);
///  * step up after [upAfter] frames at which the target (with [upMargin] x allowance, hysteresis) is a larger rung;
///  * the rung is frozen during a drag ([beginDrag]); the move the drag asked for is applied at [endDrag], straight to the target;
///  * a thermal bias ([thermalCap]) can forbid the top rungs before the SoC actually throttles.
class QualityRung {
  const QualityRung(this.longEdge, this.bake);
  final int longEdge;

  /// Lattice size used for drag frames at this rung (41 = engine default).
  final int bake;
  @override
  String toString() => '$longEdge/$bake';
}

class AdaptiveQuality {
  AdaptiveQuality({this.budgetMs = 55, this.downAfter = 2, this.upAfter = 12, this.upMargin = 0.85});

  /// Budget for a display of [hz]: one display refresh (the drag frame should fit in a vsync), never tighter than 8.3 ms (120 Hz) and never looser than
  /// the 55 ms default. Wave 8: a mid-range Adreno 710 phone renders the 810x1080 interactive film frame in ~12 ms on the GPU, so with a 55 ms budget it
  /// stayed on the top rung at 25 Hz; with the refresh period as the budget it steps down to the rung that fits (measured, not a fixed size).
  static double budgetForRefresh(double hz) => hz.isFinite && hz >= 24 ? (1000.0 / hz).clamp(8.3, 55.0) : 55.0;

  /// Rungs from best to cheapest. Only the frame size steps down: the lattice stays at 57^3 on every rung, the
  /// smallest size that keeps a colour drag (fresh bake per frame) within mean dE2000 0.5 of the exact pipeline on
  /// every surveyed photo (edit-bench "drag frames vs exact": 41^3 reached 0.77, 29^3 1.2). Tone / white-balance
  /// drags do not bake at all (the engine reuses the final frame's 65^3 lattice), so this only costs on colour drags.
  static const rungs = [
    QualityRung(1080, 57),
    QualityRung(810, 57),
    QualityRung(640, 57),
    QualityRung(480, 57),
    QualityRung(360, 57), // wave 8: the 120 Hz budget on a mid-range GPU needs a rung below 480
  ];

  double budgetMs;
  final int downAfter, upAfter;
  final double upMargin;

  /// Pixel-dependent cost of a frame, ms per megapixel (GPU execution + present). Prior 12; [seedFromMeasurement] sets it from the
  /// engine's warm-up measurement of an interactive film frame (an upper bound for frames without film).
  double pixelMsPerMpx = 12;

  int _rung = 0;
  int _over = 0, _under = 0;
  int _thermalCap = 0; // minimum rung index allowed by the thermal state
  double _ema = 0;

  /// Rung the display size allows at most (small screens never need 1080): index into [rungs].
  int _displayCap = 0;

  int get rungIndex => _rung;
  QualityRung get rung => rungs[_rung];
  double get emaMs => _ema;

  /// Megapixels of a 3:2 frame at [r].
  static double mpx(QualityRung r) => r.longEdge * r.longEdge * 2 / 3 / 1e6;

  /// Size-independent part of the smoothed frame time at the current rung, ms.
  double get overheadMs => _ema == 0 ? 0 : math.max(0, _ema - pixelMsPerMpx * mpx(rung));

  /// Time the pixel part may take: what is left of the budget after the overhead, never under half a refresh (when the overhead
  /// alone fills the budget a smaller picture buys nothing).
  double get allowanceMs => (budgetMs - overheadMs).clamp(budgetMs * 0.5, budgetMs).toDouble();

  int get _floor => _displayCap > _thermalCap ? _displayCap : _thermalCap;

  /// Index of the largest rung whose pixel part fits [margin] x the allowance (floor: display / thermal caps).
  int targetIndex({double margin = 1.0}) {
    var pick = rungs.length - 1;
    for (var i = _floor; i < rungs.length; i++) {
      if (pixelMsPerMpx * mpx(rungs[i]) <= allowanceMs * margin) {
        pick = i;
        break;
      }
    }
    return pick < _floor ? _floor : pick;
  }

  bool _seeded = false;
  bool get seeded => _seeded;

  /// Seeds the starting rung from the engine's warm-up measurement ([msPerMpx]: hot milliseconds per megapixel of an interactive film frame): the largest
  /// rung whose predicted cost (a 3:2 frame of that long edge) fits [fill] of the budget. The first drag then already meets the frame period instead of
  /// stepping down through slow frames; the normal hysteresis moves it from there. Also the prior of the pixel part ([pixelMsPerMpx]).
  /// Returns true when the rung changed.
  bool seedFromMeasurement(double msPerMpx, {double fill = 0.8}) {
    if (!(msPerMpx > 0) || !msPerMpx.isFinite) return false;
    _seeded = true;
    pixelMsPerMpx = msPerMpx;
    var pick = rungs.length - 1;
    for (var i = 0; i < rungs.length; i++) {
      if (msPerMpx * mpx(rungs[i]) <= budgetMs * fill) {
        pick = i;
        break;
      }
    }
    if (pick < _floor) pick = _floor;
    if (pick <= _rung) return false;
    _rung = pick;
    _ema = 0;
    return true;
  }

  /// [displayLongEdge]: on-screen long edge of the picture in device pixels.
  void setDisplayLongEdge(int displayLongEdge) {
    var i = 0;
    while (i < rungs.length - 1 && rungs[i + 1].longEdge >= displayLongEdge) {
      i++;
    }
    _displayCap = (i).clamp(0, rungs.length - 1);
    if (_rung < _displayCap) _rung = _displayCap;
  }

  /// thermal headroom (0 cool .. 1 throttling threshold, < 0 unknown).
  void setThermalHeadroom(double h) {
    _thermalCap = h >= 1.0 ? 2 : (h >= 0.8 ? 1 : 0);
    if (_rung < _thermalCap) _rung = _thermalCap;
  }

  bool _held = false, _pendingDown = false, _pendingUp = false;

  /// True between [beginDrag] and [endDrag]: the rung does not change.
  bool get held => _held;

  /// A drag starts: the rung is frozen for its whole length (a resolution step in the middle of a drag is a visible jump; frame trace on the iPhone X
  /// showed 1 - 3 of them in every 2 s drag). The frames are still measured; the move they ask for is applied once by [endDrag].
  void beginDrag() {
    _held = true;
    _pendingDown = _pendingUp = false;
  }

  /// The drag ended: moves to the target the drag's frames asked for (down or up; a pending move is dropped if the target is the current rung).
  /// Returns true when the rung changed.
  bool endDrag() {
    _held = false;
    var changed = false;
    if (_pendingDown) {
      final t = targetIndex();
      if (t > _rung) {
        _rung = t;
        changed = true;
      }
    } else if (_pendingUp) {
      final t = targetIndex(margin: upMargin);
      if (t < _rung) {
        _rung = t;
        changed = true;
      }
    }
    _pendingDown = _pendingUp = false;
    _over = _under = 0;
    if (changed) _ema = 0;
    return changed;
  }

  /// Feed the cycle time of an interactive frame rendered at the current rung. Returns true when the rung changed.
  bool record(double cycleMs) {
    _ema = _ema == 0 ? cycleMs : _ema * 0.6 + cycleMs * 0.4;
    // a smaller picture is only asked for when it would take the pixel part under the allowance
    if (cycleMs > budgetMs && targetIndex() > _rung) {
      _over++;
      _under = 0;
      if (_over >= downAfter) {
        if (_held) {
          _pendingDown = true;
          _pendingUp = false;
          return false;
        }
        _rung++;
        _over = 0;
        _ema = 0;
        return true;
      }
      return false;
    }
    _over = 0;
    if (cycleMs <= budgetMs && targetIndex(margin: upMargin) < _rung) {
      if (++_under >= upAfter) {
        if (_held) {
          _pendingUp = !_pendingDown;
          return false;
        }
        _rung--;
        _under = 0;
        _ema = 0;
        return true;
      }
    } else {
      _under = 0;
    }
    return false;
  }
}
