// Floating chrome of the editor: top bar, category dock, transient readout, histogram window
// (requirements 4.15.2 / 4.15.3 / 4.15.9, guidelines 15.2).
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import '../../editor/param_rows.dart';
import '../../editor/springs.dart';
import '../../editor/value_mapping.dart';
import '../../editor/window_manager.dart';
import '../../model/catalog.dart';
import '../../model/params.dart';
import '../../services/editor_engine.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/icons.dart';
import '../../widgets/photo_art.dart';
import 'editor_controller.dart';
import 'editor_widgets.dart';
import 'float_window.dart';
import 'mini_maps.dart';
import '../../services/perf_probe.dart';

BoxDecoration glassDeco(DC c, double radius) => BoxDecoration(
  color: c.glass,
  borderRadius: BorderRadius.circular(radius),
  border: Border.all(color: c.sep),
);

/// Frosted capsule (guidelines 1.4: floating navigation bars may use glass).
class GlassCapsule extends StatelessWidget {
  const GlassCapsule({super.key, required this.radius, required this.child});
  final double radius;
  final Widget child;
  @override
  Widget build(BuildContext context) => ClipRRect(
    borderRadius: BorderRadius.circular(radius),
    child: BackdropFilter(
      filter: ui.ImageFilter.blur(sigmaX: 20, sigmaY: 20),
      child: DecoratedBox(
        decoration: glassDeco(context.c, radius),
        child: child,
      ),
    ),
  );
}

// ===================================================================================== top bar

class EditorTopBar extends StatelessWidget {
  const EditorTopBar({
    super.key,
    required this.app,
    required this.photoId,
    required this.onBack,
  });
  final AppState app;
  final String Function() photoId;
  final VoidCallback onBack;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return ListenableBuilder(
      listenable: app,
      builder: (context, _) {
        final id = photoId();
        PerfProbe.count('topbar');
        final saving = app.isSaving(id);
        final done = app.photoById(id)?.exported ?? false;
        return Row(
          children: [
            GlassCapsule(
              radius: 24,
              child: DIconButton(
                DI.back,
                key: const ValueKey('toolbar-back'),
                label: '返回',
                onPressed: onBack,
              ),
            ),
            const Spacer(),
            GlassCapsule(
              radius: 24,
              child: Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  DIconButton(
                    DI.undo,
                    key: const ValueKey('toolbar-undo'),
                    label: '撤销',
                    onPressed: app.canUndo(id) ? () => app.undo(id) : null,
                  ),
                  DIconButton(
                    DI.redo,
                    key: const ValueKey('toolbar-redo'),
                    label: '重做',
                    onPressed: app.canRedo(id) ? () => app.redo(id) : null,
                  ),
                  if (saving)
                    Semantics(
                      label: '正在保存',
                      excludeSemantics: true,
                      child: SizedBox(
                        key: const ValueKey('toolbar-save'),
                        width: 48,
                        height: 48,
                        child: Center(
                          child: SizedBox(
                            width: 22,
                            height: 22,
                            child: CircularProgressIndicator(
                              strokeWidth: 2.4,
                              color: c.accent,
                            ),
                          ),
                        ),
                      ),
                    )
                  else
                    DIconButton(
                      DI.save,
                      key: const ValueKey('toolbar-save'),
                      label: done ? '已保存到相册，再存一次' : '保存到相册',
                      color: done ? c.okIcon : null,
                      onPressed: () {
                        final p = app.photoById(id);
                        if (p != null) app.saveOneToAlbum(p);
                      },
                    ),
                ],
              ),
            ),
          ],
        );
      },
    );
  }
}

// ===================================================================================== dock

/// Dock order by usage frequency (requirements 4.15.3): 8 categories plus 更多.
const dockOrder = [
  EGroup.light,
  EGroup.color,
  EGroup.style,
  EGroup.curve,
  EGroup.detail,
  EGroup.effects,
  EGroup.grade,
  EGroup.crop,
];

/// Phone dock (compact width): the fixed set that fits without scrolling. Edit this list to reorder.
const phoneDockOrder = [
  EGroup.light,
  EGroup.color,
  EGroup.style,
  EGroup.crop,
  EGroup.effects,
];

/// Categories the phone dock leaves to the 更多 menu, in [dockOrder] order.
final phoneMoreOrder = [
  for (final g in dockOrder)
    if (!phoneDockOrder.contains(g)) g,
];

class CategoryDock extends StatefulWidget {
  const CategoryDock({
    super.key,
    required this.controller,
    required this.onMore,
    this.vertical = false,
    this.groups = dockOrder,
  });
  final List<EGroup> groups;
  final EditorController controller;
  final VoidCallback onMore;

  /// Side rail variant (phone landscape).
  final bool vertical;

  @override
  State<CategoryDock> createState() => _CategoryDockState();
}

class _CategoryDockState extends State<CategoryDock> {
  final ScrollController _sc = ScrollController();
  final Map<Object, GlobalKey> _keys = {
    for (final g in EGroup.values) g: GlobalKey(),
    'more': GlobalKey(),
  };
  EGroup? _lastFocus;

  @override
  void dispose() {
    _sc.dispose();
    super.dispose();
  }

  void _centre(EGroup g) {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final ctx = _keys[g]?.currentContext;
      if (ctx == null || !_sc.hasClients) return;
      Scrollable.ensureVisible(
        ctx,
        alignment: 0.5,
        duration: const Duration(milliseconds: 200),
        curve: Curves.easeOut,
      );
    });
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final ctl = widget.controller;
    final v = widget.vertical;
    return ListenableBuilder(
      listenable: Listenable.merge([ctl.wm, ctl.edits]),
      builder: (context, _) {
        final f = ctl.wm.focused?.group;
        PerfProbe.count('dock');
        if (f != _lastFocus && f != null) _centre(f);
        _lastFocus = f;
        final ed = ctl.edit;
        final hiddenOpen = ctl.wm.windows.any(
          (w) => !widget.groups.contains(w.group),
        );
        return LayoutBuilder(
          builder: (context, cons) {
            final n = widget.groups.length + 1;
            final extent = v ? cons.maxHeight : cons.maxWidth;
            // items share the free width down to the 48 dp touch minimum; beyond that the dock scrolls
            final iw = v ? 56.0 : (extent / n).clamp(48.0, 56.0).toDouble();
            final ih = v ? (extent / n).clamp(48.0, 56.0).toDouble() : 56.0;
            Widget item(
              Object k,
              DI icon,
              String label,
              bool selected,
              bool dot,
              VoidCallback onTap, {
              String? sem,
            }) => Semantics(
              selected: selected,
              button: true,
              label: sem ?? label,
              excludeSemantics: true,
              child: GestureDetector(
                key: ValueKey('dock-$k'),
                behavior: HitTestBehavior.opaque,
                onTap: onTap,
                child: Container(
                  key: _keys[k],
                  width: v ? 56 : iw,
                  height: ih,
                  margin: const EdgeInsets.all(0),
                  decoration: BoxDecoration(
                    color: selected ? c.accentSoft : null,
                    borderRadius: BorderRadius.circular(v ? 16 : 28),
                  ),
                  child: Stack(
                    alignment: Alignment.center,
                    children: [
                      Column(
                        mainAxisSize: MainAxisSize.min,
                        mainAxisAlignment: MainAxisAlignment.center,
                        children: [
                          DIcon(
                            icon,
                            size: 24,
                            color: selected ? c.accent : c.text2,
                            stroke: 1.9,
                          ),
                          const SizedBox(height: 2),
                          Text(
                            label,
                            style: T.mini(
                              selected ? c.accent : c.text2,
                              w: selected ? FontWeight.w600 : FontWeight.w400,
                            ),
                          ),
                        ],
                      ),
                      if (dot)
                        Positioned(
                          right: 12,
                          top: 8,
                          child: Container(
                            key: ValueKey('dot-${k is EGroup ? k.name : k}'),
                            width: 6,
                            height: 6,
                            decoration: BoxDecoration(
                              color: c.accent,
                              shape: BoxShape.circle,
                            ),
                          ),
                        ),
                    ],
                  ),
                ),
              ),
            );
            final items = [
              for (final g in widget.groups)
                item(
                  g,
                  groupIcons[g]!,
                  groupLabels[g]!,
                  ctl.wm.windowOf(g) != null,
                  ed.groupAdjusted(g),
                  () => ctl.openCategory(g),
                  sem: groupLabels[g]! + (ed.groupAdjusted(g) ? '，已调整' : ''),
                ),
              item('more', DI.more, '更多', hiddenOpen, false, widget.onMore),
            ];
            final total = (v ? ih : iw) * items.length;
            final scrolls = total > extent;
            final inner = SingleChildScrollView(
              controller: _sc,
              scrollDirection: v ? Axis.vertical : Axis.horizontal,
              physics: scrolls ? null : const NeverScrollableScrollPhysics(),
              child: v ? Column(children: items) : Row(children: items),
            );
            final faded = scrolls
                ? ShaderMask(
                    shaderCallback: (r) {
                      const f = 12.0;
                      final len = v ? r.height : r.width;
                      return LinearGradient(
                        begin: v ? Alignment.topCenter : Alignment.centerLeft,
                        end: v ? Alignment.bottomCenter : Alignment.centerRight,
                        colors: const [
                          Color(0x00FFFFFF),
                          Colors.white,
                          Colors.white,
                          Color(0x00FFFFFF),
                        ],
                        stops: [0, f / len, 1 - f / len, 1],
                      ).createShader(r);
                    },
                    blendMode: BlendMode.dstIn,
                    child: inner,
                  )
                : inner;
            return GlassCapsule(
              radius: v ? 28 : 28,
              child: SizedBox(
                width: v ? 56 : math.min(total, extent),
                height: v ? math.min(total, extent) : 56,
                child: faded,
              ),
            );
          },
        );
      },
    );
  }
}

// ===================================================================================== readout

/// Transient readout (requirements 4.15.9): name + value, precision label and a read-only ruler
/// (fixed pointer, scrolling ticks, white auto marker). Solid translucent black, never touchable.
class ReadoutOverlay extends StatefulWidget {
  const ReadoutOverlay({
    super.key,
    required this.controller,
    required this.compactHist,
  });
  final EditorController controller;

  /// Compact widths append a 96x32 histogram for 光线 / 曲线 / 风格.
  final bool compactHist;

  @override
  State<ReadoutOverlay> createState() => _ReadoutOverlayState();
}

class _ReadoutOverlayState extends State<ReadoutOverlay>
    with SingleTickerProviderStateMixin {
  late final SpringValue _fade = SpringValue(this);
  ReadoutModel get r => widget.controller.readout;

  @override
  void initState() {
    super.initState();
    r.addListener(_sync);
    _sync();
  }

  @override
  void dispose() {
    r.removeListener(_sync);
    _fade.dispose();
    super.dispose();
  }

  void _sync() {
    final target = r.visible ? (r.dimmed ? 0.4 : 1.0) : 0.0;
    if (target != _fade.target) {
      _fade.animateTo(target, spring: SpringTier.small.opacity);
    }
    if (mounted) setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    return IgnorePointer(
      child: AnimatedBuilder(
        animation: _fade,
        builder: (context, _) {
          final o = _fade.value.clamp(0.0, 1.0);
          if (o < 0.01) return const SizedBox.shrink();
          final t0 = PerfProbe.begin();
          final w = RepaintBoundary(child: Opacity(opacity: o, child: _content(context)));
          PerfProbe.end('readout', t0);
          return w;
        },
      ),
    );
  }

  Widget _content(BuildContext context) {
    final c = context.c;
    if (r.compare) {
      return Container(
        key: const ValueKey('original-pill'),
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 6),
        decoration: BoxDecoration(
          color: Fx.overlay60,
          borderRadius: BorderRadius.circular(14),
        ),
        child: Text('原片', style: T.body(Colors.white, w: FontWeight.w600)),
      );
    }
    final ctl = widget.controller;
    final isOneD = r.kind == RowKind.oneD || r.twoD == TwoD.curve;
    final e = ctl.edit;
    Widget? map;
    if (r.kind == RowKind.twoD) {
      switch (r.twoD!) {
        case TwoD.palette:
          final look = lookById(e.look ?? ctl.app.settings.defaultLookId);
          map = PaletteMini(
            pos: e.palette ?? Offset(look.px, look.py),
            def: Offset(look.px, look.py),
            size: 72,
          );
        case TwoD.tone:
          map = ToneMini(
            photo: ctl.app.photoById(ctl.id)!,
            engine: ctl.engine,
            app: ctl.app,
            edit: e,
            nearest: ctl.analysis().nearest,
            onSelect: (_) {},
          );
        case TwoD.wheel:
          final z = ctl.sel.gradeZone;
          map = WheelMini(
            hue: e.value(grKey(z, 'h')),
            sat: e.value(grKey(z, 's')),
            size: 72,
          );
        case TwoD.curve:
        case TwoD.offset:
          break;
      }
    }
    final hist =
        widget.compactHist &&
        (ctl.wm.focused?.group == EGroup.light ||
            ctl.wm.focused?.group == EGroup.curve ||
            ctl.wm.focused?.group == EGroup.style);
    return Container(
      key: const ValueKey('readout'),
      padding: const EdgeInsets.fromLTRB(14, 8, 14, 8),
      decoration: BoxDecoration(
        color: Fx.overlay60,
        borderRadius: BorderRadius.circular(14),
      ),
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          Column(
            mainAxisSize: MainAxisSize.min,
            crossAxisAlignment: CrossAxisAlignment.center,
            children: [
              Row(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.baseline,
                textBaseline: TextBaseline.alphabetic,
                children: [
                  Text(
                    r.name,
                    style: T.small(Colors.white, w: FontWeight.w600),
                  ),
                  const SizedBox(width: 8),
                  Text(
                    r.value,
                    key: const ValueKey('readout-value'),
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 17,
                      height: 1.2,
                      fontWeight: FontWeight.w700,
                      fontFeatures: [FontFeature.tabularFigures()],
                    ),
                  ),
                ],
              ),
              if (r.bandText.isNotEmpty)
                Padding(
                  padding: const EdgeInsets.only(top: 2),
                  child: Text(
                    r.bandText,
                    key: const ValueKey('readout-band'),
                    style: T.cap(const Color(0xFFFFD27A), w: FontWeight.w600),
                  ),
                ),
              if (isOneD)
                Padding(
                  padding: const EdgeInsets.only(top: 6),
                  child: CustomPaint(
                    size: const Size(200, 34),
                    painter: RulerPainter(
                      r,
                      c.accent,
                      DefaultTextStyle.of(context).style,
                    ),
                  ),
                ),
              if (map != null)
                Padding(padding: const EdgeInsets.only(top: 6), child: map),
            ],
          ),
          if (hist) ...[
            const SizedBox(width: 12),
            MiniHistogramBox(analysis: ctl.analysis()),
          ],
        ],
      ),
    );
  }
}

/// Read-only ruler: ticks scroll under a fixed accent pointer. Visible span is 40 % of the range
/// times the band multiplier (at least 5 steps), so finer bands visibly magnify the scale.
class RulerPainter extends CustomPainter {
  RulerPainter(this.m, this.accent, this.baseStyle) : super(repaint: m);
  final ReadoutModel m;
  final Color accent;
  final TextStyle baseStyle;

  /// The three fixed labels are laid out once (a text layout per frame is expensive on a slow phone).
  static final Map<String, TextPainter> _labels = {};
  static TextStyle? _labelStyle;

  TextPainter _label(String t) {
    if (_labelStyle != baseStyle) {
      _labels.clear();
      _labelStyle = baseStyle;
    }
    return _labels.putIfAbsent(
      t,
      () => TextPainter(
        text: TextSpan(
          text: t,
          style: baseStyle.copyWith(color: Colors.white, fontSize: 9, height: 1, fontWeight: FontWeight.w500),
        ),
        textDirection: TextDirection.ltr,
      )..layout(),
    );
  }

  static double spanFor(double range, double step, PrecisionBand b) =>
      math.max(range * 0.4 * bandMultiplier(b), step * 5);

  @override
  void paint(Canvas canvas, Size size) {
    final range = m.max - m.min;
    if (range <= 0) return;
    final span = spanFor(range, m.step, m.band);
    final w = size.width, cx = w / 2;
    final ppu = w / span;
    final shift = m.endSide * 4.0;
    canvas.save();
    canvas.clipRect(Offset.zero & size);
    canvas.translate(-shift, 0);
    final tick = Paint()
      ..color = const Color(0x99FFFFFF)
      ..strokeWidth = 1;
    var ts = m.step;
    while (ts * ppu < 6) {
      ts *= ts * 2 < 5 ? 2 : 5;
    }
    final lo = math.max(m.min, m.v - span / 2 - ts),
        hi = math.min(m.max, m.v + span / 2 + ts);
    final first = (lo / ts).ceil();
    for (var i = first; i * ts <= hi; i++) {
      final u = i * ts;
      final x = cx + (u - m.v) * ppu;
      final major = i % 5 == 0;
      canvas.drawLine(Offset(x, 34), Offset(x, 34 - (major ? 12 : 7)), tick);
    }
    // auto marker
    final ax = cx + (m.auto - m.v) * ppu;
    final snapped = (m.v - m.auto).abs() < 1e-6;
    final white = Paint()
      ..color = Colors.white.withValues(alpha: snapped ? 1.0 : 0.85)
      ..strokeWidth = snapped ? 3 : 2;
    void label(String t, double x, TextAlign al) {
      final tp = _label(t);
      final dx = al == TextAlign.left
          ? x
          : (al == TextAlign.right ? x - tp.width : x - tp.width / 2);
      tp.paint(canvas, Offset(dx + shift, 0));
    }

    if (ax >= 4 && ax <= w - 4) {
      canvas.drawLine(Offset(ax, 34), Offset(ax, 20), white);
      label('自动', ax, TextAlign.center);
    } else if (ax < 4) {
      label('‹ 自动', 2, TextAlign.left);
    } else {
      label('自动 ›', w - 2, TextAlign.right);
    }
    canvas.restore();
    canvas.drawLine(
      Offset(cx, 34),
      Offset(cx, 16),
      Paint()
        ..color = accent
        ..strokeWidth = 2
        ..strokeCap = StrokeCap.round,
    );
  }

  @override
  bool shouldRepaint(RulerPainter old) => old.baseStyle != baseStyle;
}

// ===================================================================================== histogram window

/// Always visible histogram window (width >= 600): draggable to the corners, double tap toggles its
/// size, never collapses and keeps its opacity while a value is dragged.
class HistogramWindow extends StatefulWidget {
  const HistogramWindow({
    super.key,
    required this.controller,
    required this.area,
    required this.reduceMotion,
  });
  final EditorController controller;
  final WindowArea area;
  final bool reduceMotion;
  @override
  State<HistogramWindow> createState() => _HistogramWindowState();
}

class _HistogramWindowState extends State<HistogramWindow>
    with TickerProviderStateMixin {
  late final SpringValue _dx = SpringValue(
    this,
    reduceMotion: widget.reduceMotion,
  );
  late final SpringValue _dy = SpringValue(
    this,
    reduceMotion: widget.reduceMotion,
  );
  Offset _pos = Offset.zero;
  bool _dragging = false;
  Rect? _lastRect;

  EditorController get ctl => widget.controller;

  @override
  void dispose() {
    _dx.dispose();
    _dy.dispose();
    super.dispose();
  }

  Rect _rect() =>
      histogramRect(ctl.wm.histCorner, widget.area, small: ctl.wm.histSmall);

  @override
  void didUpdateWidget(HistogramWindow old) {
    super.didUpdateWidget(old);
    final r = _rect();
    final last = _lastRect;
    if (!_dragging &&
        last != null &&
        (r.topLeft - last.topLeft).distance > 0.5) {
      final moved = r.topLeft - last.topLeft;
      _dx.jumpTo(_dx.value - moved.dx, velocity: _dx.velocity);
      _dy.jumpTo(_dy.value - moved.dy, velocity: _dy.velocity);
      final tier = SpringTier.forDistance(moved.distance);
      _dx.animateTo(0, spring: tier.displacement);
      _dy.animateTo(0, spring: tier.displacement);
    }
    _lastRect = r;
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return ListenableBuilder(
      listenable: Listenable.merge([
        ctl.wm,
        ctl.compare,
        ctl.edits,
        ctl.engine,
        _dx,
        _dy,
      ]),
      builder: (context, _) {
        final rect = _rect();
        _lastRect ??= rect;
        final a = _analysis();
        final off = Offset(_dx.value, _dy.value);
        return Positioned.fromRect(
          rect: rect.shift(off),
          child: Semantics(
            image: true,
            label: '直方图，${tonePlain(a.nearest)}',
            child: GestureDetector(
              key: const ValueKey('histogram-window'),
              behavior: HitTestBehavior.opaque,
              onDoubleTap: ctl.wm.toggleHistSize,
              onPanStart: (_) {
                _dragging = true;
                ctl.wm.setInteracting(true);
                _pos = rect.topLeft + off;
              },
              onPanUpdate: (d) {
                _pos += d.delta;
                _dx.jumpTo(_pos.dx - rect.left);
                _dy.jumpTo(_pos.dy - rect.top);
              },
              onPanEnd: (d) {
                _dragging = false;
                ctl.wm.setInteracting(false);
                final at = Rect.fromLTWH(
                  _pos.dx,
                  _pos.dy,
                  rect.width,
                  rect.height,
                );
                final corner = histCornerForDrop(
                  at.shift(d.velocity.pixelsPerSecond * 0.099),
                  widget.area,
                );
                final target = histogramRect(
                  corner,
                  widget.area,
                  small: ctl.wm.histSmall,
                );
                _lastRect = target;
                ctl.wm.dropHistogram(corner);
                _dx.jumpTo(
                  _pos.dx - target.left,
                  velocity: d.velocity.pixelsPerSecond.dx,
                );
                _dy.jumpTo(
                  _pos.dy - target.top,
                  velocity: d.velocity.pixelsPerSecond.dy,
                );
                final tier = SpringTier.forDistance(
                  math.max(_dx.value.abs(), _dy.value.abs()),
                );
                _dx.animateTo(
                  0,
                  spring: tier.displacement,
                  velocity: d.velocity.pixelsPerSecond.dx,
                );
                _dy.animateTo(
                  0,
                  spring: tier.displacement,
                  velocity: d.velocity.pixelsPerSecond.dy,
                );
              },
              child: Opacity(
                opacity: 0.85,
                child: Container(
                  decoration: BoxDecoration(
                    color: c.surface,
                    borderRadius: BorderRadius.circular(12),
                    border: Border.all(color: c.sep),
                    boxShadow: const [
                      BoxShadow(
                        color: Color(0x38000000),
                        blurRadius: 16,
                        offset: Offset(0, 4),
                      ),
                    ],
                  ),
                  clipBehavior: Clip.antiAlias,
                  child: Stack(
                    children: [
                      Positioned.fill(
                        child: CustomPaint(
                          painter: HistPainter(
                            a,
                            c.histFill,
                            c.text,
                            c.zoneA,
                            c.accent,
                          ),
                        ),
                      ),
                      if (ctl.compare.value)
                        Positioned(
                          left: 6,
                          top: 4,
                          child: Text(
                            '原片',
                            style: T.cap(c.text2, w: FontWeight.w600),
                          ),
                        ),
                    ],
                  ),
                ),
              ),
            ),
          ),
        );
      },
    );
  }

  Analysis _analysis() {
    final original = ctl.compare.value;
    if (ctl.engine.live) {
      final a = original ? ctl.engine.originalAnalysis : ctl.engine.analysis;
      if (a != null) return a;
    }
    final p = ctl.app.photoById(ctl.id);
    final di = p?.demo;
    if (di != null) {
      final spec = demoPhotos[di];
      final l = DemoEdit.of(ctl.app, p!).look(spec, original);
      return Analysis.demo(spec, brightness: l.b, contrast: l.c);
    }
    return Analysis(
      List<double>.filled(64, 1 / 64),
      .1,
      .5,
      .9,
      p?.nearest ?? 'mid-mid',
    );
  }
}
