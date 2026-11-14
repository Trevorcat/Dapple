// The one-thumb cluster of the workbench layout (docs/design/foldables.md): floats over the photo only, at the holding edge, at the height
// the thumb is at. A rail of the six categories, the ball (press and push in any direction = a relative pad drag; hold still = compare),
// the strength ring above it, a small 分项 pill beside it, and a grip under it that moves the whole cluster along the edge (dragged past
// the middle of the photo it changes sides). It fades when idle. Meanings are the phone's: the ball is the pad, the ring is the strength
// strip, the pill is 分项 (the current category's detail, not a seventh category), the rail is the dock.
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../../editor/fold_layout.dart';
import '../../model/film_map.dart';
import '../../model/pad_model.dart';
import '../../model/params.dart';
import '../../theme/tokens.dart';
import 'editor_controller.dart';
import 'editor_overlays.dart' show GlassCapsule;
import 'film_map_view.dart' show FilmMapStore;
import 'fold_panels.dart' show foldSelect, foldSync;
import 'itemised_view.dart' show CatItemSource;
import 'pad_dock.dart' show dockLabel, padDockOrder;
import 'pad_panel.dart' show HeaderButton, PanelChip;
import 'pad_widgets.dart';

enum _Drag { none, pad, film, item, crop, remote }

/// Finger travel (dp) of one full pad range / of the whole strength strip / of the item's range.
const double kBallPadSpan = 110, kRingSpan = 110, kBallItemSpan = 150, kFilmStep = 90;

/// Time a still press on the ball takes to become the compare (the editor's long press).
const Duration kBallCompareHold = Duration(milliseconds: 420);

/// Idle time before the cluster fades.
const Duration kClusterIdle = Duration(milliseconds: 3500);

class FoldCluster extends StatefulWidget {
  const FoldCluster({super.key, required this.ctl, required this.geo, required this.onSlide, required this.onDrop});
  final EditorController ctl;
  final BallGeo geo;

  /// The grip moved the cluster by [dy] along the edge.
  final ValueChanged<double> onSlide;

  /// The grip was let go at this global x: past the middle of the photo the cluster changes sides.
  final ValueChanged<double> onDrop;

  @override
  State<FoldCluster> createState() => _FoldClusterState();
}

class _FoldClusterState extends State<FoldCluster> {
  EditorController get ctl => widget.ctl;

  bool _awake = true;
  Timer? _idle;
  int _active = 0; // fingers on the cluster

  // ball
  int? _bPointer;
  Offset _bFrom = Offset.zero, _nub = Offset.zero;
  bool _bMoved = false, _bCompare = false;
  DateTime _bT0 = DateTime.now();
  Timer? _bTimer;
  _Drag _mode = _Drag.none;
  PadSource? _src;
  PadState _p0 = PadState.centre;
  double _v0 = 0;
  CatItemSource? _items;
  PadItem? _item;
  List<FilmDot> _dots = const [];
  FilmDot? _dot0;
  String? _dotLast;
  bool moveRemote = false;

  // ring
  double _a0 = 0.5, _ringY = 0;
  PadSource? _ringSrc;

  // grip
  double _gdx = 0, _gx = 0;

  @override
  void initState() {
    super.initState();
    _wake();
  }

  @override
  void dispose() {
    _idle?.cancel();
    _bTimer?.cancel();
    if (_bPointer != null) _endBall(cancelled: true);
    super.dispose();
  }

  void _wake() {
    _idle?.cancel();
    if (!_awake) setState(() => _awake = true);
    _idle = Timer(kClusterIdle, () {
      if (mounted && _active == 0) setState(() => _awake = false);
    });
  }

  EGroup get _group => ctl.wm.focused?.group ?? EGroup.light;

  PadSource? _padSource() {
    final g = _group;
    if (g == EGroup.local) return ctl.local.sel != null ? ctl.local.pad : null;
    final cat = PadCatX.ofGroup(g);
    return cat == null ? null : CatPadSource(ctl, cat);
  }

  void _compare(bool on) {
    if (ctl.local.active) {
      ctl.localCompare(on);
    } else {
      ctl.longPress(on);
    }
  }

  // ------------------------------------------------------------ the ball

  void _bDown(PointerDownEvent e) {
    if (_bPointer != null) return;
    _bPointer = e.pointer;
    _bFrom = e.position;
    _bMoved = false;
    _bCompare = false;
    _bT0 = DateTime.now();
    _active++;
    ctl.wm.setInteracting(true);
    _wake();
    _bTimer?.cancel();
    _bTimer = Timer(kBallCompareHold, () {
      if (_bPointer != null && !_bMoved) {
        _bCompare = true;
        _compare(true);
      }
    });
    setState(() {});
  }

  void _bMove(PointerMoveEvent e) {
    if (e.pointer != _bPointer || _bCompare) return;
    final d = e.position - _bFrom;
    if (!_bMoved) {
      if (d.distance < 8) return;
      _bMoved = true;
      _bTimer?.cancel();
      _beginDrag();
    }
    _drag(d);
    final m = d.distance;
    setState(() => _nub = m > 30 ? d * (30 / m) : d);
  }

  void _bUp(PointerEvent e, {required bool cancelled}) {
    if (e.pointer != _bPointer) return;
    _endBall(cancelled: cancelled);
  }

  void _endBall({required bool cancelled}) {
    _bTimer?.cancel();
    _bPointer = null;
    if (_bCompare) {
      _compare(false);
    } else if (_bMoved) {
      _endDrag();
    } else if (!cancelled && DateTime.now().difference(_bT0) < const Duration(milliseconds: 400) && _group == EGroup.local && ctl.local.canMoveRemote) {
      // a tap on the ball in 局部 switches it between the pad and moving the selected area
      moveRemote = !moveRemote;
    }
    _bCompare = false;
    _bMoved = false;
    _active = math.max(0, _active - 1);
    ctl.wm.setInteracting(_active > 0);
    _wake();
    if (mounted) setState(() => _nub = Offset.zero);
  }

  void _beginDrag() {
    final g = _group;
    _mode = _Drag.none;
    if (g == EGroup.crop) {
      _mode = _Drag.crop;
      _v0 = ctl.edit.value('straighten');
      ctl.valueBegin('straighten');
      return;
    }
    final lc = ctl.local;
    if (g == EGroup.local) {
      if (moveRemote && lc.canMoveRemote) {
        _mode = _Drag.remote;
        lc.remoteMoveBegin();
        return;
      }
      final s = _padSource();
      if (s == null) return;
      _mode = _Drag.pad;
      _src = s;
      _p0 = s.view;
      s.begin();
      return;
    }
    final cat = PadCatX.ofGroup(g);
    if (cat == null) return;
    if (ctl.itemised) {
      final its = ctl.itemsOf(cat);
      if (its.isEmpty) return;
      final it = its[ctl.selItem.clamp(0, its.length - 1)];
      _items = CatItemSource(ctl, cat);
      _item = it;
      _v0 = _items!.actual(it);
      _items!.begin(it);
      _mode = _Drag.item;
      return;
    }
    if (cat == PadCat.film) {
      _dots = layoutFilmMap(ctl.app.film, FilmMapStore.positions ?? const {});
      if (_dots.isEmpty) return;
      final cur = ctl.edit.film;
      _dot0 = _dots.where((d) => d.id == cur).firstOrNull ?? nearestDot(_dots, Offset.zero);
      _dotLast = cur;
      ctl.stockBegin();
      _mode = _Drag.film;
      return;
    }
    final s = CatPadSource(ctl, cat);
    _mode = _Drag.pad;
    _src = s;
    _p0 = s.view;
    s.begin();
  }

  void _drag(Offset d) {
    switch (_mode) {
      case _Drag.none:
        return;
      case _Drag.crop:
        final sp = specByKey['straighten']!;
        ctl.valueSet('straighten', (_v0 + d.dx / 160 * (sp.max - sp.min) / 2).clamp(sp.min, sp.max).toDouble());
      case _Drag.remote:
        ctl.local.remoteMoveBy(d * 2.6);
      case _Drag.pad:
        _src?.move(_p0.x + d.dx / kBallPadSpan, _p0.y - d.dy / kBallPadSpan);
      case _Drag.film:
        final at = _dot0!.shown + Offset(d.dx / kFilmStep, -d.dy / kFilmStep);
        final n = nearestDot(_dots, at);
        if (n != null && n.id != _dotLast) {
          _dotLast = n.id;
          ctl.stockSet(n.id);
        }
      case _Drag.item:
        final it = _item!;
        _items!.set(it, it.clampV(_v0 + d.dx / kBallItemSpan * (it.max - it.min)));
    }
  }

  void _endDrag() {
    switch (_mode) {
      case _Drag.none:
        break;
      case _Drag.crop:
        ctl.valueEnd();
      case _Drag.remote:
        ctl.local.remoteMoveEnd();
      case _Drag.pad:
        _src?.end();
      case _Drag.film:
        ctl.stockEnd();
      case _Drag.item:
        _items?.end();
    }
    _mode = _Drag.none;
    _src = null;
    _items = null;
  }

  // ------------------------------------------------------------ the ring

  void _rDown(DragStartDetails d) {
    final s = _padSource();
    if (s == null || !s.enabled) return;
    _active++;
    ctl.wm.setInteracting(true);
    _ringSrc = s;
    _a0 = s.view.a;
    _ringY = 0;
    s.begin();
    _wake();
  }

  void _rMove(DragUpdateDetails d) {
    final s = _ringSrc;
    if (s == null) return;
    _ringY += d.delta.dy;
    s.strip(_a0 - _ringY / kRingSpan);
  }

  void _rUp() {
    final s = _ringSrc;
    if (s == null) return;
    _ringSrc = null;
    s.end();
    _active = math.max(0, _active - 1);
    ctl.wm.setInteracting(_active > 0);
    _wake();
  }

  // ------------------------------------------------------------ build

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final g = widget.geo;
    return ListenableBuilder(
      listenable: Listenable.merge([ctl.wm, ctl.edits, ctl.local]),
      builder: (context, _) {
        final grp = _group;
        final crop = grp == EGroup.local ? false : PadCatX.ofGroup(grp) == null;
        final remoteOk = grp == EGroup.local && ctl.local.canMoveRemote;
        if (!remoteOk) moveRemote = false;
        final label = moveRemote ? '移动' : dockLabel(grp);
        final src = _padSource();
        Widget ring = const SizedBox.shrink();
        if (src != null && !crop) {
          ring = _Ring(src: src, onStart: _rDown, onUpdate: _rMove, onEnd: _rUp);
        }
        final pill = (crop || (grp == EGroup.local && ctl.local.sel == null))
            ? const SizedBox.shrink()
            : HeaderButton(key: const ValueKey('cluster-items'), label: '分项', pressed: ctl.itemised, onTap: () {
                _wake();
                ctl.setItemised(!ctl.itemised);
                foldSync(ctl);
              });
        return AnimatedOpacity(
          key: const ValueKey('fold-cluster'),
          opacity: _awake || _active > 0 ? 1 : 0.45,
          duration: const Duration(milliseconds: 300),
          child: Transform.translate(
            offset: Offset(_gdx, 0),
            child: Stack(clipBehavior: Clip.none, children: [
              Positioned.fromRect(rect: g.rail, child: _Rail(ctl: ctl, onTap: _wake, cols: g.rail.width > BallGeo.railW + 1 ? 2 : 1)),
              Positioned.fromRect(rect: g.ring, child: Center(child: SizedBox(width: BallGeo.ringD, height: BallGeo.ringD, child: ring))),
              Positioned.fromRect(rect: g.pill, child: Center(child: pill)),
              Positioned.fromRect(
                rect: g.ball,
                child: Semantics(
                  label: '浮球，按住对比，推动调整$label',
                  child: Listener(
                    key: const ValueKey('cluster-ball'),
                    behavior: HitTestBehavior.opaque,
                    onPointerDown: _bDown,
                    onPointerMove: _bMove,
                    onPointerUp: (e) => _bUp(e, cancelled: false),
                    onPointerCancel: (e) => _bUp(e, cancelled: true),
                    child: GlassCapsule(
                      radius: BallGeo.ballD / 2,
                      child: Column(mainAxisAlignment: MainAxisAlignment.center, mainAxisSize: MainAxisSize.min, children: [
                        Transform.translate(
                          offset: _nub,
                          child: Container(width: 26, height: 26, decoration: BoxDecoration(color: moveRemote ? c.accentBtn : c.accent, shape: BoxShape.circle, boxShadow: const [BoxShadow(color: Color(0x4D000000), blurRadius: 4, offset: Offset(0, 1))])),
                        ),
                        const SizedBox(height: 3),
                        Text(label, maxLines: 1, style: T.cap(c.accent, w: FontWeight.w600)),
                      ]),
                    ),
                  ),
                ),
              ),
              Positioned.fromRect(
                rect: g.grip,
                child: Semantics(
                  label: '拖动浮球换位置',
                  child: GestureDetector(
                    key: const ValueKey('cluster-grip'),
                    behavior: HitTestBehavior.opaque,
                    onPanStart: (d) {
                      _active++;
                      _gx = d.globalPosition.dx;
                      _wake();
                    },
                    onPanUpdate: (d) {
                      _gx = d.globalPosition.dx;
                      setState(() => _gdx += d.delta.dx);
                      widget.onSlide(d.delta.dy);
                    },
                    onPanEnd: (_) => _dropGrip(),
                    onPanCancel: _dropGrip,
                    child: Center(
                      child: SizedBox(
                        width: BallGeo.gripW,
                        height: BallGeo.gripH,
                        child: GlassCapsule(radius: 15, child: Center(child: Text('•••', style: T.cap(c.text2).copyWith(letterSpacing: 2)))),
                      ),
                    ),
                  ),
                ),
              ),
            ]),
          ),
        );
      },
    );
  }

  void _dropGrip() {
    _active = math.max(0, _active - 1);
    final x = _gx;
    setState(() => _gdx = 0);
    widget.onDrop(x);
    _wake();
  }
}

/// The six categories at the holding edge: the dock's words as chips.
class _Rail extends StatelessWidget {
  const _Rail({required this.ctl, required this.onTap, this.cols = 1});
  final EditorController ctl;
  final int cols;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: Listenable.merge([ctl.wm, ctl.edits]),
      builder: (context, _) {
        final cur = ctl.wm.focused?.group;
        return GlassCapsule(
          radius: 32,
          child: SingleChildScrollView(
            key: const ValueKey('cluster-rail'),
            physics: const ClampingScrollPhysics(),
            padding: const EdgeInsets.all(4),
            child: Row(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
              for (var col = 0; col < cols; col++)
                Column(children: [
                  for (var i = col * (6 ~/ cols); i < (col + 1) * (6 ~/ cols); i++)
                    SizedBox(
                      height: BallGeo.railChip,
                      width: BallGeo.railW - 8,
                      child: PanelChip(
                        key: ValueKey('cluster-cat-${padDockOrder[i].name}'),
                        label: dockLabel(padDockOrder[i]),
                        selected: cur == padDockOrder[i],
                        compact: true,
                        onTap: () {
                          onTap();
                          foldSelect(ctl, padDockOrder[i]);
                        },
                      ),
                    ),
                ]),
            ]),
          ),
        );
      },
    );
  }
}

/// The strength ring: the strength strip's value as an arc, the percent inside; a vertical drag changes it.
class _Ring extends StatelessWidget {
  const _Ring({required this.src, required this.onStart, required this.onUpdate, required this.onEnd});
  final PadSource src;
  final GestureDragStartCallback onStart;
  final GestureDragUpdateCallback onUpdate;
  final VoidCallback onEnd;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      slider: true,
      label: '强度',
      child: GestureDetector(
        key: const ValueKey('cluster-ring'),
        behavior: HitTestBehavior.opaque,
        onVerticalDragStart: onStart,
        onVerticalDragUpdate: onUpdate,
        onVerticalDragEnd: (_) => onEnd(),
        onVerticalDragCancel: onEnd,
        child: ListenableBuilder(
          listenable: src.changes,
          builder: (context, _) {
            final a = src.view.a;
            return CustomPaint(
              painter: _RingPainter(a, c.accent, c.glass, c.sep),
              child: Center(child: Text('${src.percent(a).round()}', style: T.mini(c.text, w: FontWeight.w600))),
            );
          },
        ),
      ),
    );
  }
}

class _RingPainter extends CustomPainter {
  const _RingPainter(this.a, this.accent, this.glass, this.sep);
  final double a;
  final Color accent, glass, sep;

  @override
  void paint(Canvas canvas, Size size) {
    final r = size.width / 2;
    final ctr = size.center(Offset.zero);
    canvas.drawCircle(ctr, r, Paint()..color = glass);
    canvas.drawCircle(ctr, r - 0.5, Paint()..style = PaintingStyle.stroke..strokeWidth = 1..color = sep);
    final ring = Rect.fromCircle(center: ctr, radius: r - 4);
    canvas.drawArc(ring, -math.pi / 2, 2 * math.pi * a.clamp(0.0, 1.0), false, Paint()..style = PaintingStyle.stroke..strokeWidth = 4..strokeCap = StrokeCap.round..color = accent);
  }

  @override
  bool shouldRepaint(_RingPainter o) => o.a != a || o.accent != accent || o.glass != glass || o.sep != sep;
}
