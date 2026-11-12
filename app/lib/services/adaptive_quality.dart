/// Dynamic resolution for drag-time frames.
///
/// The interactive path costs roughly (lattice bake) + (pixels x neighbourhood stages). On a fast phone one
/// 1080 px frame fits in a display frame or two; on a low-end SoC it does not. This controller picks a rung of
/// [rungs] (long edge + lattice size) from the measured frame cycle time, so the preview keeps updating at
/// >= ~15 Hz with bounded latency instead of falling behind.
///
/// Policy (pure Dart, unit tested):
///  * budget: the whole cycle (native + upload) of an interactive frame should stay under [budgetMs];
///  * step down after [downAfter] consecutive frames over budget (fast reaction: ~2 frames);
///  * step up only after [upAfter] frames whose *predicted* cost at the next rung (cost scales with pixels)
///    stays under [upMargin] x budget - hysteresis, so it does not oscillate;
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
  AdaptiveQuality({this.budgetMs = 55, this.downAfter = 2, this.upAfter = 12, this.upMargin = 0.6});

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

  int _rung = 0;
  int _over = 0, _under = 0;
  int _thermalCap = 0; // minimum rung index allowed by the thermal state
  double _ema = 0;

  /// Rung the display size allows at most (small screens never need 1080): index into [rungs].
  int _displayCap = 0;

  int get rungIndex => _rung;
  QualityRung get rung => rungs[_rung];
  double get emaMs => _ema;

  bool _seeded = false;
  bool get seeded => _seeded;

  /// Seeds the starting rung from the engine's warm-up measurement ([msPerMpx]: hot milliseconds per megapixel of an interactive film frame): the largest
  /// rung whose predicted cost (a 3:2 frame of that long edge) fits [fill] of the budget. The first drag then already meets the frame period instead of
  /// stepping down through slow frames; the normal hysteresis moves it from there. Returns true when the rung changed.
  bool seedFromMeasurement(double msPerMpx, {double fill = 0.8}) {
    if (!(msPerMpx > 0) || !msPerMpx.isFinite) return false;
    _seeded = true;
    var pick = rungs.length - 1;
    for (var i = 0; i < rungs.length; i++) {
      final e = rungs[i].longEdge.toDouble();
      final predicted = msPerMpx * (e * e * 2 / 3) / 1e6;
      if (predicted <= budgetMs * fill) {
        pick = i;
        break;
      }
    }
    final floor = _displayCap > _thermalCap ? _displayCap : _thermalCap;
    if (pick < floor) pick = floor;
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

  /// Feed the cycle time of an interactive frame rendered at the current rung. Returns true when the rung changed.
  bool record(double cycleMs) {
    _ema = _ema == 0 ? cycleMs : _ema * 0.6 + cycleMs * 0.4;
    final floor = _displayCap > _thermalCap ? _displayCap : _thermalCap;
    if (cycleMs > budgetMs) {
      _over++;
      _under = 0;
      if (_over >= downAfter && _rung < rungs.length - 1) {
        _rung++;
        _over = 0;
        _ema = 0;
        return true;
      }
      return false;
    }
    _over = 0;
    if (_rung > floor) {
      final px = rungs[_rung - 1].longEdge / rungs[_rung].longEdge;
      final predicted = cycleMs * px * px;
      if (predicted < budgetMs * upMargin) {
        if (++_under >= upAfter) {
          _rung--;
          _under = 0;
          _ema = 0;
          return true;
        }
      } else {
        _under = 0;
      }
    }
    return false;
  }
}
