// Zoomable photo stage of the editor: pure display + motion. It owns no gesture recognizers; the
// editor's single gesture arbiter (lib/editor/gesture_arbiter.dart) drives it through the
// [ZoomController]: pinch / pan with momentum and hard edge clamping, double-tap fit <-> 2x about the
// tap, and the browse-mode page drag with spring settle. Zoom range 1x-8x.
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/physics.dart';

import '../../editor/springs.dart';
import '../../services/perf_probe.dart';

const double kZoomMax = 8;
const double kZoomDouble = 2;

/// Lets the page read the zoom state and drive the view.
class ZoomController extends ChangeNotifier {
  double scale = 1;
  Offset offset = Offset.zero;
  _ZoomViewState? _view;

  /// True once the picture is meaningfully magnified.
  bool get zoomed => scale > 1.02;

  /// Size of the frame at fit zoom and of the viewport (0 until laid out).
  Size get frame => _view?._frame ?? Size.zero;
  Size get viewport => _view?._vp ?? Size.zero;

  void _set(double s, Offset o) {
    if (s == scale && o == offset) return;
    scale = s;
    offset = o;
    notifyListeners();
  }

  /// Back to fit. [animate] glides, otherwise it jumps (used when switching photo).
  void reset({bool animate = false}) {
    final v = _view;
    if (v != null) {
      v._resetTo(animate);
    } else {
      _set(1, Offset.zero);
    }
  }

  // ---- driven by the gesture arbiter ----
  void scaleStart(Offset focal) => _view?._scaleStart(focal);
  void scaleUpdate(Offset focal, double scale) => _view?._scaleUpdate(focal, scale);
  void scaleEnd(Offset velocity) => _view?._scaleEnd(velocity);
  void panBy(Offset d) => _view?._panBy(d);
  void panEnd(Offset velocity) => _view?._panEnd(velocity);
  void doubleTap(Offset at) => _view?._doubleTap(at);

  /// Animates to [targetScale] keeping the view centre (the 1:1 button).
  void zoomTo(double targetScale) => _view?._animateTo(targetScale.clamp(1.0, kZoomMax), offset * (targetScale / scale));

  void pageDrag(double dx) => _view?._pageDrag(dx);
  void pageEnd(double total, double vx, ValueChanged<int> onSwipe) => _view?._pageEnd(total, vx, onSwipe);
  void pageCancel() => _view?._pageSettle(0);
}

class ZoomView extends StatefulWidget {
  const ZoomView({
    super.key,
    required this.controller,
    required this.aspect,
    required this.builder,
    this.padding = const EdgeInsets.all(12),
    this.shiftY,
    this.onViewport,
  });

  final ZoomController controller;

  /// Width / height of the picture frame.
  final double aspect;

  /// Builds the picture at its fitted (unzoomed) frame size.
  final Widget Function(BuildContext context, Size frame) builder;

  /// Space kept around the picture at fit (system insets). Keep it symmetric: the frame is centred.
  final EdgeInsets padding;

  /// Extra vertical offset of the picture (window avoidance), animated by the page.
  final ValueListenable<double>? shiftY;

  /// Visible part of the frame (normalised 0..1), the zoom factor relative to fit and the frame size.
  /// Debounced while moving, immediate when the transform settles.
  final void Function(Rect visible, double scale, Size frame)? onViewport;

  @override
  State<ZoomView> createState() => _ZoomViewState();
}

class _ZoomViewState extends State<ZoomView> with TickerProviderStateMixin {
  ZoomController get zc => widget.controller;

  Size _vp = Size.zero;
  Size _frame = Size.zero;
  Offset get _vc => Offset(_vp.width / 2, _vp.height / 2);

  late final AnimationController _anim = AnimationController(vsync: this, duration: const Duration(milliseconds: 240));
  late final AnimationController _fling = AnimationController.unbounded(vsync: this);
  late final SpringValue _page = SpringValue(this);

  double _s0 = 1;
  Offset _t0 = Offset.zero, _f0 = Offset.zero;
  Timer? _vpTimer;

  @override
  void initState() {
    super.initState();
    zc._view = this;
    zc.addListener(_changed);
    _page.addListener(_repaint);
  }

  @override
  void didUpdateWidget(ZoomView old) {
    super.didUpdateWidget(old);
    if (old.controller != widget.controller) {
      old.controller._view = null;
      old.controller.removeListener(_changed);
      widget.controller._view = this;
      widget.controller.addListener(_changed);
    }
  }

  @override
  void dispose() {
    if (zc._view == this) zc._view = null;
    zc.removeListener(_changed);
    _page.dispose();
    _vpTimer?.cancel();
    _anim.dispose();
    _fling.dispose();
    super.dispose();
  }

  void _repaint() {
    if (mounted) setState(() {});
  }

  void _changed() {
    if (mounted) setState(() {});
    _scheduleReport();
  }

  // ------------------------------------------------------------ geometry

  Offset _clampOffset(Offset t, double s) {
    final mx = math.max(0.0, (_frame.width * s - _vp.width) / 2);
    final my = math.max(0.0, (_frame.height * s - _vp.height) / 2);
    return Offset(t.dx.clamp(-mx, mx), t.dy.clamp(-my, my));
  }

  void _stopMotion() {
    _anim.stop();
    _fling.stop();
  }

  void _resetTo(bool animate) {
    _stopMotion();
    _page.jumpTo(0);
    if (animate) {
      _animateTo(1, Offset.zero);
    } else {
      zc._set(1, Offset.zero);
      _report();
    }
  }

  void _animateTo(double s, Offset o) {
    _stopMotion();
    final s0 = zc.scale, o0 = zc.offset;
    final target = _clampOffset(o, s);
    if ((s0 - s).abs() < 1e-4 && (o0 - target).distance < 0.5) {
      zc._set(s, target);
      _report();
      return;
    }
    _anim.value = 0; // before listening: resetting a finished run reports `dismissed`
    final curve = CurvedAnimation(parent: _anim, curve: Curves.easeOutCubic);
    void tick() {
      final k = curve.value;
      zc._set(s0 + (s - s0) * k, Offset.lerp(o0, target, k)!);
    }

    void done(AnimationStatus st) {
      if (st == AnimationStatus.completed || st == AnimationStatus.dismissed) {
        _anim.removeStatusListener(done);
        curve.removeListener(tick);
        curve.dispose();
        if (st == AnimationStatus.completed) _report();
      }
    }

    curve.addListener(tick);
    _anim.addStatusListener(done);
    _anim.forward();
  }

  // ------------------------------------------------------------ pinch / pan

  void _scaleStart(Offset focal) {
    _stopMotion();
    _s0 = zc.scale;
    _t0 = zc.offset;
    _f0 = focal;
    if (_page.value != 0) _page.animateTo(0, spring: SpringTier.medium.displacement);
  }

  void _scaleUpdate(Offset focal, double scale) {
    final s = (_s0 * scale).clamp(0.85, kZoomMax * 1.25);
    // Keep the content point that was under the start focal point under the current focal point.
    final q = _vc + (_f0 - _vc - _t0) / _s0;
    final t = focal - _vc - (q - _vc) * s;
    zc._set(s, _clampOffset(t, s));
  }

  void _scaleEnd(Offset v) {
    final s = zc.scale;
    if (s < 1 || s > kZoomMax) {
      _animateTo(s.clamp(1.0, kZoomMax), zc.offset);
      return;
    }
    if (zc.zoomed && v.distance > 120) {
      _startFling(v);
    } else {
      _report();
    }
  }

  void _panBy(Offset d) {
    _stopMotion();
    zc._set(zc.scale, _clampOffset(zc.offset + d, zc.scale));
  }

  void _panEnd(Offset v) {
    if (zc.zoomed && v.distance > 120) {
      _startFling(v);
    } else {
      _report();
    }
  }

  void _startFling(Offset v) {
    final o = zc.offset, s = zc.scale;
    final sx = FrictionSimulation(0.12, o.dx, v.dx);
    final sy = FrictionSimulation(0.12, o.dy, v.dy);
    final secs = math.min(1.6, math.max(_settle(sx, v.dx), _settle(sy, v.dy)));
    _fling.value = 0;
    void tick() {
      final t = _fling.value;
      final raw = Offset(sx.x(t), sy.x(t));
      final c = _clampOffset(raw, s);
      zc._set(s, c);
      // Hitting an edge kills that axis' momentum instead of sliding along it.
      if (c.dx != raw.dx && c.dy != raw.dy) _fling.stop();
    }

    _fling.addListener(tick);
    _fling.animateTo(secs, duration: Duration(milliseconds: (secs * 1000).round()), curve: Curves.linear).whenComplete(() {
      _fling.removeListener(tick);
      _report();
    });
  }

  double _settle(FrictionSimulation sim, double v) {
    if (v.abs() < 1) return 0;
    var t = 0.0;
    while (t < 1.6 && !sim.isDone(t)) {
      t += 0.05;
    }
    return t;
  }

  void _doubleTap(Offset at) {
    if (zc.zoomed) {
      _animateTo(1, Offset.zero);
    } else {
      const s = kZoomDouble;
      final q = _vc + (at - _vc - zc.offset) / zc.scale;
      _animateTo(s, -(q - _vc) * s);
    }
  }

  // ------------------------------------------------------------ page drag (browse mode, fit zoom)

  void _pageDrag(double dx) {
    _page.jumpTo(_page.value + dx);
  }

  void _pageSettle(double vx) => _page.animateTo(0, spring: SpringTier.large.displacement, velocity: vx);

  void _pageEnd(double total, double vx, ValueChanged<int> onSwipe) {
    final w = math.max(1.0, _vp.width);
    final commit = total.abs() > w * 0.35 || (vx.abs() > 600 && total.abs() > 24);
    if (!commit) {
      _pageSettle(vx);
      return;
    }
    final dir = total < 0 ? 1 : -1; // swiped left -> next
    // Slide out, switch the photo, then slide the new one in from the other side.
    _page.animateTo(-dir * w * 0.6, spring: SpringTier.medium.displacement, velocity: vx);
    Future<void>.delayed(const Duration(milliseconds: 90), () {
      if (!mounted) return;
      onSwipe(dir);
      _page.jumpTo(dir * w * 0.3);
      _page.animateTo(0, spring: SpringTier.large.displacement);
    });
  }

  // ------------------------------------------------------------ viewport report

  void _scheduleReport() {
    if (widget.onViewport == null) return;
    _vpTimer?.cancel();
    _vpTimer = Timer(const Duration(milliseconds: 120), _report);
  }

  void _report() {
    _vpTimer?.cancel();
    final cb = widget.onViewport;
    if (cb == null || _frame.isEmpty) return;
    final s = zc.scale, c = _vc + zc.offset;
    final fr = Rect.fromCenter(center: c, width: _frame.width * s, height: _frame.height * s);
    final vis = fr.intersect(Offset.zero & _vp);
    if (vis.isEmpty) return;
    final n = Rect.fromLTRB(
      ((vis.left - fr.left) / fr.width).clamp(0.0, 1.0),
      ((vis.top - fr.top) / fr.height).clamp(0.0, 1.0),
      ((vis.right - fr.left) / fr.width).clamp(0.0, 1.0),
      ((vis.bottom - fr.top) / fr.height).clamp(0.0, 1.0),
    );
    cb(n, math.max(1.0, s), _frame);
  }

  // ------------------------------------------------------------ build

  @override
  Widget build(BuildContext context) {
    return LayoutBuilder(builder: (context, cons) {
    PerfProbe.count('zoomView');
      final vp = Size(cons.maxWidth, cons.maxHeight);
      final pad = widget.padding;
      final inner = Size(math.max(0, vp.width - pad.horizontal), math.max(0, vp.height - pad.vertical));
      var w = inner.width, h = w / widget.aspect;
      if (h > inner.height) {
        h = inner.height;
        w = h * widget.aspect;
      }
      final frame = (w > 0 && h > 0) ? Size(w, h) : Size.zero;
      if (frame != _frame || vp != _vp) {
        _frame = frame;
        _vp = vp;
        if (_clampOffset(zc.offset, zc.scale) != zc.offset) {
          WidgetsBinding.instance.addPostFrameCallback((_) {
            if (mounted) zc._set(zc.scale, _clampOffset(zc.offset, zc.scale));
          });
        }
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (mounted) _report();
        });
      }
      if (frame.isEmpty) return const SizedBox.shrink();
      return ClipRect(
        child: SizedBox.expand(
          child: ListenableBuilder(
            listenable: widget.shiftY ?? const _NoListenable(),
            builder: (context, _) {
              final sy = widget.shiftY?.value ?? 0;
              return Transform(
                alignment: Alignment.center,
                transform: Matrix4.translationValues(zc.offset.dx + _page.value, zc.offset.dy + (zc.zoomed ? 0 : sy), 0) *
                    Matrix4.diagonal3Values(zc.scale, zc.scale, 1),
                child: Center(
                  child: SizedBox(
                    width: frame.width,
                    height: frame.height,
                    child: RepaintBoundary(child: widget.builder(context, frame)),
                  ),
                ),
              );
            },
          ),
        ),
      );
    });
  }
}

class _NoListenable implements Listenable {
  const _NoListenable();
  @override
  void addListener(VoidCallback listener) {}
  @override
  void removeListener(VoidCallback listener) {}
}
