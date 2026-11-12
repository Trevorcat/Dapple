// Floating category windows (requirements 4.15.4, guidelines 15.2.3): compact cards that list a
// category's parameters with small readouts. No slider look anywhere: rows are text + a value.
// Layout comes from the [WindowManager] anchors; motion is physics springs that keep position and
// velocity when interrupted. The window collapses to a one-line readout while a finger adjusts a value
// and springs back after it lifts.
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:flutter/semantics.dart';
import 'package:flutter/services.dart';

import '../../editor/param_rows.dart';
import '../../editor/springs.dart';
import '../../editor/window_manager.dart';
import '../../model/params.dart';
import '../../models.dart';
import '../../services/editor_engine.dart';
import '../../theme/tokens.dart';
import '../../widgets/icons.dart';
import 'editor_controller.dart';
import 'control_pad.dart';
import 'editor_widgets.dart';
import 'mini_maps.dart';
import '../../services/perf_probe.dart';

const groupIcons = {
  EGroup.style: DI.styles,
  EGroup.light: DI.light,
  EGroup.color: DI.color,
  EGroup.curve: DI.curve,
  EGroup.effects: DI.effects,
  EGroup.detail: DI.detail,
  EGroup.grade: DI.grade,
  EGroup.crop: DI.crop,
};

const double kHeaderH = 44;
const double kPillH = 40;

/// Height of the picker / map shown under the active discrete or 2D row.
double extraHeight(ParamRow r) {
  switch (r.id) {
    case 'band':
    case 'channel':
    case 'zone':
      return 88;
    case 'look':
      return 80;
    case 'film':
      return 108;
    case 'filmPaper':
      return 56;
    case 'ratio':
      return 128;
    case 'toneTarget':
      return 92;
    case 'film_expo':
    case 'film_texture':
    case 'palette':
      return 148; // a pad of about 172 x 132 dp: big enough to steer with a thumb
    case 'wheel':
      return 160;
    default:
      return r.twoD == TwoD.curve ? 148 : 0;
  }
}

bool _hasMiniHist(EGroup g, LayoutClass lc) =>
    !lc.histWindow && (g == EGroup.light || g == EGroup.curve || g == EGroup.style);

/// Size of the expanded card of [g] (requirements 4.15.4: width by class, max height rule).
Size cardSize({
  required EGroup g,
  required List<ParamRow> rows,
  required ParamRow active,
  required Anchor anchor,
  required LayoutClass lc,
  required WindowArea area,
}) {
  final rowH = lc.rowHeight;
  final bounds = area.bounds;
  // min(available height x fraction, bounds - readout 72 - 24); the landscape rail may use all of it
  final maxH = lc.railDock
      ? bounds.height
      : math.min(area.availHeight * lc.maxHeightFraction, bounds.height - 72 - 24).clamp(120.0, 4000.0).toDouble();
  final extra = extraHeight(active);
  if (anchor.edge == DockEdge.bottom) {
    final w = math.min(480.0, bounds.width - 24);
    final h = kHeaderH + (rows.length / 2).ceil() * rowH + extra + 8;
    return Size(w, _snapToRow(h, maxH, rows: rows, active: active, rowH: rowH, mini: 0, twoCol: true));
  }
  final mini = _hasMiniHist(g, lc) ? 44.0 : 0.0;
  final h = kHeaderH + mini + rows.length * rowH + extra + 8;
  return Size(lc.windowWidth, _snapToRow(h, maxH, rows: rows, active: active, rowH: rowH, mini: mini, twoCol: false));
}

/// Height of a window whose natural height is [h]. When it exceeds [maxH] the window scrolls, and its
/// viewport then ends on a whole row (never half a row at the bottom edge when scrolled to the top).
double _snapToRow(double h, double maxH, {required List<ParamRow> rows, required ParamRow active, required double rowH, required double mini, required bool twoCol}) {
  if (h <= maxH) return h;
  const border = 2.0; // the card's 1 dp outline on both sides
  final usable = maxH - kHeaderH - border;
  // Row boundaries at scroll offset 0. The active row's map unfolds right below it, so a boundary is
  // only valid after the whole block.
  final extra = extraHeight(active);
  final n = twoCol ? (rows.length / 2).ceil() : rows.length;
  var best = 0.0, y = mini;
  for (var i = 0; i < n; i++) {
    y += rowH;
    final isActive = twoCol ? false : rows[i].id == active.id;
    if (isActive) y += extra;
    if (y <= usable) best = y;
  }
  if (twoCol && n > 0 && n * rowH + extra <= usable) best = n * rowH + extra;
  return best >= rowH ? kHeaderH + border + best : maxH;
}

/// Smallest top edge any category window could reach at [anchor]: the stable photo region while any
/// window is open on a phone (switching windows must not move the photo).
double tallestCardTop(EditorController ctl, Anchor anchor, LayoutClass lc, WindowArea area) {
  var top = double.infinity;
  for (final g in EGroup.values) {
    final rows = ctl.rowsOf(g);
    if (rows.isEmpty) continue;
    for (final a in rows) {
      final r = windowRect(anchor, cardSize(g: g, rows: rows, active: a, anchor: anchor, lc: lc, area: area), area);
      top = math.min(top, r.top);
    }
  }
  return top.isFinite ? top : area.bounds.bottom;
}

/// Pixel rect of the expanded card of [w] (also used by the page for the photo avoidance shift).
Rect cardRectFor(EditorController ctl, WinState w, LayoutClass lc, WindowArea area) {
  final rows = ctl.rowsOf(w.group);
  final active = ctl.activeRowOf(w.group, rows);
  return windowRect(w.anchor, cardSize(g: w.group, rows: rows, active: active, anchor: w.anchor, lc: lc, area: area), area);
}

Size pillSize(String name, String value) =>
    Size((60 + 15.0 * (name.length + value.length)).clamp(120.0, 220.0).toDouble(), kPillH);

// ===================================================================================== layer

/// All category windows. Rebuilds on window-model / edit changes, but not on every edit while a
/// finger drags (the windows are a one-line readout then; they refresh when the finger lifts).
class FloatingWindowsLayer extends StatefulWidget {
  const FloatingWindowsLayer({
    super.key,
    required this.controller,
    required this.layout,
    required this.area,
    required this.reduceMotion,
    required this.photo,
    required this.nearest,
  });
  final EditorController controller;
  final LayoutClass layout;
  final WindowArea area;
  final bool reduceMotion;
  final Photo photo;
  final String nearest;

  @override
  State<FloatingWindowsLayer> createState() => _FloatingWindowsLayerState();
}

class _FloatingWindowsLayerState extends State<FloatingWindowsLayer> {
  EditorController get ctl => widget.controller;
  bool _wasDragging = false;
  late final Listenable _all = Listenable.merge([ctl.wm, ctl.edits, ctl.dragging]);

  @override
  void initState() {
    super.initState();
    _all.addListener(_changed);
  }

  @override
  void dispose() {
    _all.removeListener(_changed);
    super.dispose();
  }

  void _changed() {
    final d = ctl.dragging.value;
    final skip = d && _wasDragging;
    _wasDragging = d;
    if (!skip && mounted) setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    final wins = ctl.wm.windows;
    PerfProbe.count('winLayer');
    final multi = wins.length > 1;
    final focus = ctl.wm.focused;
    return Stack(children: [
      for (final w in wins) _hostFor(w, multi && w.group == focus?.group, multi),
    ]);
  }

  Widget _hostFor(WinState w, bool focused, bool multi) {
    final rows = ctl.rowsOf(w.group);
    final active = ctl.activeRowOf(w.group, rows);
    final card = cardSize(g: w.group, rows: rows, active: active, anchor: w.anchor, lc: widget.layout, area: widget.area);
    final rc = ctl.ctx();
    final pill = pillSize(groupLabels[w.group]!, active.text(rc));
    final cardRect = windowRect(w.anchor, card, widget.area);
    final pillRect = windowRect(w.anchor, pill, widget.area);
    // The one-line readout while dragging sits at the card's top, on the window's side.
    final lineRect = Rect.fromLTWH(
        w.anchor.edge == DockEdge.right ? cardRect.right - pill.width : cardRect.left, cardRect.top, pill.width, pill.height);
    return WindowHost(
      key: ValueKey('win-slot-${w.slot}'),
      controller: ctl,
      win: w,
      rows: rows,
      active: active,
      cardRect: cardRect,
      pillRect: pillRect,
      lineRect: lineRect,
      area: widget.area,
      cardSize: card,
      layout: widget.layout,
      focused: focused,
      multi: multi,
      reduceMotion: widget.reduceMotion,
      photo: widget.photo,
      nearest: widget.nearest,
    );
  }
}

// ===================================================================================== host

class WindowHost extends StatefulWidget {
  const WindowHost({
    super.key,
    required this.controller,
    required this.win,
    required this.rows,
    required this.active,
    required this.cardRect,
    required this.pillRect,
    required this.lineRect,
    required this.area,
    required this.cardSize,
    required this.layout,
    required this.focused,
    required this.multi,
    required this.reduceMotion,
    required this.photo,
    required this.nearest,
  });
  final EditorController controller;
  final WinState win;
  final List<ParamRow> rows;
  final ParamRow active;
  final Rect cardRect, pillRect, lineRect;
  final WindowArea area;
  final Size cardSize;
  final LayoutClass layout;
  final bool focused, multi, reduceMotion;
  final Photo photo;
  final String nearest;

  @override
  State<WindowHost> createState() => _WindowHostState();
}

class _WindowHostState extends State<WindowHost> with TickerProviderStateMixin {
  late final SpringValue _dx = SpringValue(this, reduceMotion: widget.reduceMotion);
  late final SpringValue _dy = SpringValue(this, reduceMotion: widget.reduceMotion);
  late final SpringValue _mode = SpringValue(this, initial: widget.win.collapsed ? 1 : 0, reduceMotion: widget.reduceMotion);
  late final SpringValue _line = SpringValue(this, reduceMotion: widget.reduceMotion);
  late final SpringValue _appear = SpringValue(this, reduceMotion: widget.reduceMotion);

  final ScrollMemory _scroll = ScrollMemory();
  bool _userDragging = false;
  Offset _dragPos = Offset.zero; // top-left of the card while the header is dragged
  Offset _releaseVelocity = Offset.zero;
  late Rect _lastTarget = widget.win.collapsed ? widget.pillRect : widget.cardRect;
  late EGroup _lastGroup = widget.win.group;

  EditorController get ctl => widget.controller;

  @override
  void initState() {
    super.initState();
    _appear.animateTo(1, spring: SpringTier.medium.opacity);
    ctl.dragging.addListener(_onDragging);
  }

  @override
  void dispose() {
    ctl.dragging.removeListener(_onDragging);
    for (final s in [_dx, _dy, _mode, _line, _appear]) {
      s.dispose();
    }
    super.dispose();
  }

  void _onDragging() {
    _line.animateTo(ctl.dragging.value && !ctl.dragFromWindow ? 1 : 0, spring: SpringTier.small.opacity);
  }

  @override
  void didUpdateWidget(WindowHost old) {
    super.didUpdateWidget(old);
    _mode.reduceMotion = _dx.reduceMotion = _dy.reduceMotion = _line.reduceMotion = widget.reduceMotion;
    _mode.animateTo(widget.win.collapsed ? 1 : 0, spring: SpringTier.medium.opacity);
    final target = widget.win.collapsed ? widget.pillRect : widget.cardRect;
    if (_userDragging) {
      _lastTarget = target;
      return;
    }
    if (widget.win.group != _lastGroup) {
      // content replaced in place: cross-fade
      _lastGroup = widget.win.group;
      _appear.jumpTo(0.4);
      _appear.animateTo(1, spring: SpringTier.medium.opacity);
    }
    final moved = target.topLeft - _lastTarget.topLeft;
    if (moved.distance > 0.5) {
      // keep the visual position and velocity: offset = where it is - where it should be
      final nx = _dx.value - moved.dx, ny = _dy.value - moved.dy;
      final vx = _releaseVelocity.dx != 0 ? _releaseVelocity.dx : _dx.velocity;
      final vy = _releaseVelocity.dy != 0 ? _releaseVelocity.dy : _dy.velocity;
      _releaseVelocity = Offset.zero;
      _dx.jumpTo(nx, velocity: vx);
      _dy.jumpTo(ny, velocity: vy);
      final tier = SpringTier.forDistance(math.max(nx.abs(), ny.abs()));
      _dx.animateTo(0, spring: tier.displacement);
      _dy.animateTo(0, spring: tier.displacement);
    }
    _lastTarget = target;
  }

  // ---- header drag

  void _dragStart(DragStartDetails d) {
    _userDragging = true;
    ctl.wm.setInteracting(true);
    _dragPos = (widget.win.collapsed ? widget.pillRect : widget.cardRect).topLeft + Offset(_dx.value, _dy.value);
    _dx.jumpTo(_dx.value);
    _dy.jumpTo(_dy.value);
  }

  void _dragUpdate(DragUpdateDetails d) {
    _dragPos += d.delta;
    final base = (widget.win.collapsed ? widget.pillRect : widget.cardRect).topLeft;
    _dx.jumpTo(_dragPos.dx - base.dx);
    _dy.jumpTo(_dragPos.dy - base.dy);
  }

  void _dragEnd(DragEndDetails d) {
    _userDragging = false;
    ctl.wm.setInteracting(false);
    final size = widget.win.collapsed ? widget.pillRect.size : widget.cardSize;
    final at = Rect.fromLTWH(_dragPos.dx, _dragPos.dy, size.width, size.height);
    final v = d.velocity.pixelsPerSecond;
    _releaseVelocity = v;
    final a = anchorForDrop(at, v, size, widget.area, current: widget.win.anchor.edge);
    ctl.wm.drop(widget.win.group, a);
    // A drop that lands on the same anchor produces no retarget: spring back here.
    final newTarget = windowRect(a, size, widget.area);
    if ((newTarget.topLeft - _lastTarget.topLeft).distance <= 0.5) {
      final tier = SpringTier.forDistance(math.max(_dx.value.abs(), _dy.value.abs()));
      _dx.animateTo(0, spring: tier.displacement, velocity: v.dx);
      _dy.animateTo(0, spring: tier.displacement, velocity: v.dy);
      _releaseVelocity = Offset.zero;
    }
  }

  @override
  Widget build(BuildContext context) {
    final w = widget;
    // Built once per widget update, not per animation tick: the card (all its rows and their text) is the
    // expensive part of the fade-out / fade-in at the start and end of every drag, and only its opacity,
    // scale and offset change while the springs run. Its own layer makes those changes compositing-only.
    final card = RepaintBoundary(child: _Card(host: w, scroll: _scroll, onDragStart: _dragStart, onDragUpdate: _dragUpdate, onDragEnd: _dragEnd));
    final pill = _Pill(host: w, onDragStart: _dragStart, onDragUpdate: _dragUpdate, onDragEnd: _dragEnd);
    return AnimatedBuilder(
      animation: Listenable.merge([_dx, _dy, _mode, _line, _appear]),
      builder: (context, _) {
        final k = _mode.value.clamp(0.0, 1.0);
        PerfProbe.count('winHost');
        final line = _line.value.clamp(0.0, 1.0);
        final off = Offset(_dx.value, _dy.value);
        final appear = _appear.value.clamp(0.0, 1.0);
        final cardOpacity = ((1 - k) * (1 - line) * appear).clamp(0.0, 1.0);
        final pillBase = w.win.collapsed ? w.pillRect : w.lineRect;
        final pillOpacity = (w.win.collapsed ? math.max(k, line * 0.7) : line * 0.7) * appear;
        return Stack(children: [
          if (cardOpacity > 0.01)
            Positioned.fromRect(
              rect: w.cardRect.shift(off),
              child: Opacity(
                opacity: cardOpacity,
                child: Transform.scale(
                  scale: 0.94 + 0.06 * cardOpacity,
                  alignment: w.win.anchor.edge == DockEdge.right ? Alignment.bottomRight : Alignment.bottomLeft,
                  child: IgnorePointer(
                    ignoring: line > 0.5 || k > 0.5,
                    child: card,
                  ),
                ),
              ),
            ),
          if (pillOpacity > 0.01)
            Positioned.fromRect(
              rect: pillBase.shift(off),
              child: Opacity(
                opacity: pillOpacity.clamp(0.0, 1.0),
                child: IgnorePointer(
                  ignoring: !w.win.collapsed || k < 0.5,
                  child: pill,
                ),
              ),
            ),
        ]);
      },
    );
  }
}

// ===================================================================================== pill

BoxDecoration _surfaceDeco(DC c, {double radius = 16, Color? border}) => BoxDecoration(
      color: c.surface,
      borderRadius: BorderRadius.circular(radius),
      border: Border.all(color: border ?? c.sep),
      boxShadow: const [BoxShadow(color: Color(0x38000000), blurRadius: 24, offset: Offset(0, 6))],
    );

class _Pill extends StatelessWidget {
  const _Pill({required this.host, required this.onDragStart, required this.onDragUpdate, required this.onDragEnd});
  final WindowHost host;
  final GestureDragStartCallback onDragStart;
  final GestureDragUpdateCallback onDragUpdate;
  final GestureDragEndCallback onDragEnd;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final ctl = host.controller;
    final g = host.win.group;
    return ListenableBuilder(
      listenable: Listenable.merge([ctl.edits, ctl.readout]),
      builder: (context, _) {
        final dragging = ctl.dragging.value;
        PerfProbe.count('winLine');
        final name = dragging && ctl.readout.name.isNotEmpty ? ctl.readout.name : host.active.label;
        final value = dragging ? ctl.readout.value : host.active.text(ctl.ctx());
        // its own layer: the line's text changes with every pointer move and must not repaint the page
        return RepaintBoundary(child: Semantics(
          container: true,
          button: true,
          label: '${groupLabels[g]}，$name，$value，展开',
          excludeSemantics: true,
          onTap: () => ctl.wm.setCollapsed(g, false),
          child: GestureDetector(
            key: ValueKey('pill-${g.name}'),
            behavior: HitTestBehavior.opaque,
            onTap: () => ctl.wm.setCollapsed(g, false),
            onPanStart: onDragStart,
            onPanUpdate: onDragUpdate,
            onPanEnd: onDragEnd,
            child: Container(
              decoration: _surfaceDeco(c, radius: 20, border: c.accentSoft),
              padding: const EdgeInsets.only(left: 12),
              child: Row(children: [
                DIcon(groupIcons[g]!, size: 18, color: c.accent),
                const SizedBox(width: 6),
                Expanded(
                  child: Text.rich(
                    TextSpan(children: [
                      TextSpan(text: name, style: T.small(c.text, w: FontWeight.w500)),
                      TextSpan(text: '  $value', style: T.small(c.text2, tabular: true)),
                    ]),
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                  ),
                ),
                GestureDetector(
                  key: ValueKey('pill-close-${g.name}'),
                  behavior: HitTestBehavior.opaque,
                  onTap: () => ctl.openCategory(g),
                  child: Semantics(
                    button: true,
                    label: '关闭${groupLabels[g]}窗口',
                    excludeSemantics: true,
                    child: SizedBox(width: 40, height: kPillH, child: Center(child: DIcon(DI.close, size: 14, color: c.text2))),
                  ),
                ),
              ]),
            ),
          ),
        ));
      },
    );
  }
}

// ===================================================================================== card

/// Scroll offset of a window's body, kept by the window (not by the card): the card is unmounted while the window
/// fades to its drag line and when it collapses, and must come back where the user left it.
class ScrollMemory {
  double offset = 0;
}

class _Card extends StatelessWidget {
  const _Card({required this.host, required this.scroll, required this.onDragStart, required this.onDragUpdate, required this.onDragEnd});
  final WindowHost host;
  final ScrollMemory scroll;
  final GestureDragStartCallback onDragStart;
  final GestureDragUpdateCallback onDragUpdate;
  final GestureDragEndCallback onDragEnd;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final ctl = host.controller;
    final g = host.win.group;
    final lc = host.layout;
    final bottom = host.win.anchor.edge == DockEdge.bottom;
    final ed = ctl.edit;
    final adjusted = ed.groupAdjusted(g);
    final rc = ctl.ctx();
    final header = _Header(
      group: g,
      adjusted: adjusted,
      focused: host.focused && host.multi,
      onDragStart: onDragStart,
      onDragUpdate: onDragUpdate,
      onDragEnd: onDragEnd,
      onReset: () => ctl.resetCategoryOf(g),
      onCollapse: () => ctl.wm.setCollapsed(g, true),
      onOneToOne: g == EGroup.detail ? () => ctl.zoom.zoomTo(_oneToOneScale(ctl)) : null,
      onFocus: () => ctl.wm.focus(g),
    );
    final rows = host.rows;
    final active = host.active;
    final extra = _Extra(row: active, host: host, rc: rc);
    List<Widget> rowWidgets(Iterable<ParamRow> rs, {bool withExtra = true}) => [
          for (final r in rs)
            () {
              final tile = _RowTile(
                key: ValueKey('row-${g.name}-${r.id}'),
                row: r,
                rc: rc,
                active: r.id == active.id,
                height: lc.rowHeight,
                controller: ctl,
                group: g,
              );
              // the map / picker unfolds right below the active row (one block, kept in view together)
              return r.id == active.id && withExtra ? _ActiveBlock(children: [tile, extra]) : tile as Widget;
            }(),
        ];
    Widget body;
    if (bottom) {
      final half = (rows.length / 2).ceil();
      body = Column(children: [
        Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Expanded(child: Column(children: rowWidgets(rows.take(half), withExtra: false))),
          Expanded(child: Column(children: rowWidgets(rows.skip(half), withExtra: false))),
        ]),
        extra,
      ]);
    } else {
      body = Column(children: [
        if (_hasMiniHist(g, lc))
          Padding(
            padding: const EdgeInsets.fromLTRB(12, 0, 12, 4),
            child: Align(alignment: Alignment.centerLeft, child: MiniHistogramBox(analysis: ctl.analysis())),
          ),
        ...rowWidgets(rows),
      ]);
    }
    return Container(
      key: ValueKey('window-${g.name}'),
      decoration: _surfaceDeco(c, radius: 16),
      clipBehavior: Clip.antiAlias,
      child: Column(children: [
        header,
        Expanded(child: _ScrollBody(activeKey: '${g.name}-${active.id}', memory: scroll, child: body)),
      ]),
    );
  }

  double _oneToOneScale(EditorController ctl) {
    final img = ctl.engine.image;
    final fw = ctl.zoom.frame.width;
    if (img == null || fw <= 0) return 2.5;
    return (img.width / fw / 2.75).clamp(1.5, 8.0).toDouble();
  }
}

/// Scrolls inside the window when the rows do not fit; keeps the active row visible.
class _ScrollBody extends StatefulWidget {
  const _ScrollBody({required this.activeKey, required this.memory, required this.child});
  final String activeKey;
  final ScrollMemory memory;
  final Widget child;
  @override
  State<_ScrollBody> createState() => _ScrollBodyState();
}

class _ScrollBodyState extends State<_ScrollBody> {
  late final ScrollController _sc = ScrollController(initialScrollOffset: widget.memory.offset);

  @override
  void didUpdateWidget(_ScrollBody old) {
    super.didUpdateWidget(old);
    if (old.activeKey != widget.activeKey) {
      WidgetsBinding.instance.addPostFrameCallback((_) => _revealActive());
      // the window itself resizes to the new row's map / picker: reveal again once it has settled
      _settle?.cancel();
      _settle = Timer(const Duration(milliseconds: 360), _revealActive);
    }
  }

  Timer? _settle;

  /// Brings the active row with its picker into view: the end first, then (it wins when the block is taller than
  /// the window) its first line, so the row is never scrolled away above a picker.
  void _revealActive() {
    if (!mounted) return;
    final ctx = _activeContext;
    if (ctx == null || !ctx.mounted) return;
    Scrollable.ensureVisible(ctx, duration: Duration.zero, alignmentPolicy: ScrollPositionAlignmentPolicy.keepVisibleAtEnd);
    Scrollable.ensureVisible(ctx, duration: Duration.zero, alignmentPolicy: ScrollPositionAlignmentPolicy.keepVisibleAtStart);
  }

  BuildContext? get _activeContext {
    BuildContext? found;
    void visit(Element e) {
      if (found != null) return;
      final w = e.widget;
      if (w is _ActiveBlock) {
        found = e;
        return;
      }
      e.visitChildren(visit);
    }

    (context as Element).visitChildren(visit);
    return found;
  }

  @override
  void dispose() {
    _settle?.cancel();
    _sc.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return NotificationListener<ScrollNotification>(
      onNotification: (n) {
        if (n.depth != 0) return false;
        final m = n.metrics;
        widget.memory.offset = m.pixels;
        final up = m.pixels > 1, down = m.pixels < m.maxScrollExtent - 1;
        if (up != _more.$1 || down != _more.$2) {
          _more = (up, down);
          WidgetsBinding.instance.addPostFrameCallback((_) {
            if (mounted) setState(() {});
          });
        }
        return false;
      },
      child: Stack(children: [
        // A thin always-visible thumb marks a window that scrolls; the fades show which side has more.
        RawScrollbar(
          controller: _sc,
          thumbVisibility: true,
          thickness: 3,
          radius: const Radius.circular(2),
          thumbColor: c.text2.withValues(alpha: 0.5),
          child: ShaderMask(
            shaderCallback: (r) => LinearGradient(
              begin: Alignment.topCenter,
              end: Alignment.bottomCenter,
              colors: [_more.$1 ? const Color(0x00FFFFFF) : Colors.white, Colors.white, Colors.white, _more.$2 ? const Color(0x00FFFFFF) : Colors.white],
              stops: [0, math.min(0.4, 14 / math.max(r.height, 1)), math.max(0.6, 1 - 14 / math.max(r.height, 1)), 1],
            ).createShader(r),
            blendMode: BlendMode.dstIn,
            child: SingleChildScrollView(key: const ValueKey('window-scroll'), controller: _sc, child: widget.child),
          ),
        ),
      ]),
    );
  }

  (bool, bool) _more = (false, false);
}

/// The active row together with its map / picker.
class _ActiveBlock extends StatelessWidget {
  const _ActiveBlock({required this.children});
  final List<Widget> children;
  @override
  Widget build(BuildContext context) => Column(children: children);
}

class MiniHistogramBox extends StatelessWidget {
  const MiniHistogramBox({super.key, required this.analysis});
  final Analysis analysis;
  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      image: true,
      label: '直方图，${tonePlain(analysis.nearest)}',
      child: ClipRRect(
        borderRadius: BorderRadius.circular(8),
        child: Container(
          width: 96,
          height: 32,
          color: c.bg,
          child: CustomPaint(painter: HistPainter(analysis, c.histFill, c.text, c.zoneA, c.accent, zones: false)),
        ),
      ),
    );
  }
}

// ===================================================================================== header

class _Header extends StatelessWidget {
  const _Header({
    required this.group,
    required this.adjusted,
    required this.focused,
    required this.onDragStart,
    required this.onDragUpdate,
    required this.onDragEnd,
    required this.onReset,
    required this.onCollapse,
    required this.onFocus,
    this.onOneToOne,
  });
  final EGroup group;
  final bool adjusted, focused;
  final GestureDragStartCallback onDragStart;
  final GestureDragUpdateCallback onDragUpdate;
  final GestureDragEndCallback onDragEnd;
  final VoidCallback onReset, onCollapse, onFocus;
  final VoidCallback? onOneToOne;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    Widget btn(Key k, Widget child, String label, VoidCallback? onTap) => Semantics(
          button: true,
          label: label,
          excludeSemantics: true,
          child: GestureDetector(key: k, behavior: HitTestBehavior.opaque, onTap: onTap, child: SizedBox(width: 44, height: 44, child: Center(child: child))),
        );
    return GestureDetector(
      key: ValueKey('header-${group.name}'),
      behavior: HitTestBehavior.opaque,
      onTapDown: (_) => onFocus(),
      onPanStart: onDragStart,
      onPanUpdate: onDragUpdate,
      onPanEnd: onDragEnd,
      child: Container(
        height: kHeaderH,
        color: focused ? c.accentSoft : null,
        padding: const EdgeInsets.only(left: 12),
        child: Row(children: [
          DIcon(groupIcons[group]!, size: 18, color: c.text),
          const SizedBox(width: 6),
          Expanded(child: Text(groupLabels[group]!, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.body(c.text, w: FontWeight.w600))),
          if (onOneToOne != null) btn(const ValueKey('one-to-one'), Text('1:1', style: T.small(c.accent, w: FontWeight.w600)), '放大到 100%', onOneToOne),
          Visibility(
            visible: adjusted,
            maintainSize: true,
            maintainAnimation: true,
            maintainState: true,
            child: btn(ValueKey('reset-${group.name}'), DIcon(DI.reset, size: 20, color: c.text2), '复位${groupLabels[group]}', onReset),
          ),
          btn(ValueKey('collapse-${group.name}'), DIcon(DI.down, size: 20, color: c.text2), '收起', onCollapse),
        ]),
      ),
    );
  }
}

// ===================================================================================== rows

/// Minimum gap between a row's label and its value, and the least width the value keeps before the label ellipsizes.
const double labelGap = 12;
const double rowValueMinWidth = 48;

class _RowTile extends StatelessWidget {
  const _RowTile({super.key, required this.row, required this.rc, required this.active, required this.height, required this.controller, required this.group});
  final ParamRow row;
  final RowContext rc;
  final bool active;
  final double height;
  final EditorController controller;
  final EGroup group;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final ctl = controller;
    final modified = row.isModified(rc);
    final isAction = row.kind == RowKind.action;
    final atAuto = !isAction && row.isAuto(rc) && row.kind != RowKind.discrete;
    final text = row.text(rc);
    final editing = ctl.editingRow.value == row.id;

    // semantics: adjustable node (increase / decrease / big step / reset / type)
    String? inc, dec;
    if (row.kind == RowKind.oneD || row.twoD == TwoD.curve) {
      final v = row.value(rc);
      inc = row.fmt!((v + row.step).clamp(row.min, row.max).toDouble());
      dec = row.fmt!((v - row.step).clamp(row.min, row.max).toDouble());
    } else if (row.kind == RowKind.discrete) {
      final i = row.readIndex!(rc);
      inc = row.itemLabel!((i + 1).clamp(0, row.count - 1));
      dec = row.itemLabel!((i - 1).clamp(0, row.count - 1));
    }
    final adjustable = row.kind == RowKind.oneD || row.kind == RowKind.discrete || row.twoD == TwoD.curve;
    final valueStyle = T.body(modified ? c.accent : c.text2, tabular: true);
    Widget valueWidget = Text(text, maxLines: 1, overflow: TextOverflow.ellipsis, style: valueStyle, textAlign: TextAlign.right);
    if (editing) {
      valueWidget = NumberEntry(
        key: ValueKey('entry-${row.id}'),
        initial: row.inputText(rc),
        style: valueStyle,
        onSubmit: (t) => ctl.commitNumeric(row, t),
      );
    } else if (row.typeable) {
      valueWidget = GestureDetector(
        key: ValueKey('value-${row.id}'),
        behavior: HitTestBehavior.opaque,
        onTap: () {
          ctl.setActive(group, row);
          ctl.beginNumeric(row);
        },
        child: ConstrainedBox(
          constraints: const BoxConstraints(minWidth: 44, minHeight: 44),
          child: Align(alignment: Alignment.centerRight, widthFactor: 1, child: valueWidget),
        ),
      );
    }

    return Semantics(
      container: true,
      label: row.label,
      hint: row.hint,
      value: row.semanticValue(rc),
      increasedValue: inc,
      decreasedValue: dec,
      onIncrease: adjustable ? () => ctl.stepRow(row, 1) : null,
      onDecrease: adjustable ? () => ctl.stepRow(row, -1) : null,
      onTap: isAction ? () => ctl.performAction(row) : () => ctl.setActive(group, row),
      customSemanticsActions: {
        if (adjustable && row.kind == RowKind.oneD) ...{
          const CustomSemanticsAction(label: '大步增大'): () => ctl.stepRow(row, 1, big: true),
          const CustomSemanticsAction(label: '大步减小'): () => ctl.stepRow(row, -1, big: true),
        },
        if (!isAction) const CustomSemanticsAction(label: '复位到自动'): () => ctl.resetRow(row),
        if (row.typeable) const CustomSemanticsAction(label: '输入数值'): () => ctl.beginNumeric(row),
      },
      excludeSemantics: true,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTapDown: (_) {
          if (isAction) return;
          ctl.setActive(group, row);
        },
        onTap: isAction ? () => ctl.performAction(row) : null,
        onDoubleTap: isAction ? null : () => ctl.resetRow(row),
        child: Container(
          height: height,
          color: active ? c.accentSoft : null,
          child: LayoutBuilder(builder: (context, bc) {
            // The label keeps its natural width plus a 12 dp gap; the value gets what is left (up to its cap) and
            // ellipsizes: label and value never touch, even in the narrow (~194 dp) window.
            final labelStyle = T.body(modified ? c.accent : c.text, w: modified ? FontWeight.w600 : FontWeight.w400);
            final labelW = (TextPainter(
              text: TextSpan(text: row.label, style: labelStyle),
              textDirection: TextDirection.ltr,
              textScaler: MediaQuery.textScalerOf(context),
              maxLines: 1,
            )..layout())
                .width;
            final fixed = 3 + 9 + 12 + labelGap + (atAuto ? 13 : 0);
            final cap = row.kind == RowKind.discrete ? 140.0 : 110.0;
            final valueMax = (bc.maxWidth - fixed - labelW).clamp(rowValueMinWidth, cap).toDouble();
            return Row(children: [
              Container(width: 3, color: active ? c.accent : Colors.transparent),
              const SizedBox(width: 9),
              Expanded(child: Text(row.label, maxLines: 1, overflow: TextOverflow.ellipsis, style: labelStyle)),
              const SizedBox(width: labelGap),
              if (atAuto)
                Container(
                  key: ValueKey('auto-${row.id}'),
                  width: 8,
                  height: 8,
                  margin: const EdgeInsets.only(right: 5),
                  decoration: BoxDecoration(
                    shape: BoxShape.circle,
                    border: Border.all(color: Colors.white, width: 1.5),
                    boxShadow: [BoxShadow(color: c.text2.withValues(alpha: .5), spreadRadius: 0.5)],
                  ),
                ),
              Flexible(flex: 0, child: ConstrainedBox(constraints: BoxConstraints(maxWidth: valueMax), child: valueWidget)),
              const SizedBox(width: 12),
            ]);
          }),
        ),
      ),
    );
  }
}

/// The typed-value field (requirements 4.15.10): numeric keyboard with minus, commit on done or
/// when it loses focus (tap on the photo), empty = cancel.
class NumberEntry extends StatefulWidget {
  const NumberEntry({super.key, required this.initial, required this.style, required this.onSubmit});
  final String initial;
  final TextStyle style;
  final ValueChanged<String> onSubmit;
  @override
  State<NumberEntry> createState() => NumberEntryState();
}

class NumberEntryState extends State<NumberEntry> {
  late final TextEditingController _tc = TextEditingController(text: widget.initial);
  final FocusNode _fn = FocusNode();
  bool _done = false;

  @override
  void initState() {
    super.initState();
    _tc.selection = TextSelection(baseOffset: 0, extentOffset: widget.initial.length);
    _fn.addListener(() {
      if (!_fn.hasFocus) _commit();
    });
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) _fn.requestFocus();
    });
  }

  void _commit() {
    if (_done) return;
    _done = true;
    widget.onSubmit(_tc.text);
  }

  @override
  void dispose() {
    _fn.dispose();
    _tc.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 76,
      child: TextField(
        controller: _tc,
        focusNode: _fn,
        textAlign: TextAlign.right,
        style: widget.style,
        keyboardType: const TextInputType.numberWithOptions(signed: true, decimal: true),
        inputFormatters: [FilteringTextInputFormatter.allow(RegExp(r'[0-9.\-−+]'))],
        textInputAction: TextInputAction.done,
        onSubmitted: (_) => _commit(),
        decoration: const InputDecoration(isDense: true, border: InputBorder.none, contentPadding: EdgeInsets.symmetric(vertical: 8)),
      ),
    );
  }
}

// ===================================================================================== extras

/// Map / picker under the active discrete or 2D row.
class _Extra extends StatelessWidget {
  const _Extra({required this.row, required this.host, required this.rc});
  final ParamRow row;
  final WindowHost host;
  final RowContext rc;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final ctl = host.controller;
    final e = rc.edit;
    final h = extraHeight(row);
    if (h == 0) return const SizedBox.shrink();
    Widget? child;
    switch (row.id) {
      case 'band':
        child = BandDots(index: rc.sel.band, onSelect: (i) => ctl.selectDiscrete(row, i));
      case 'look':
        child = LookStrip(index: row.readIndex!(rc), onSelect: (i) => ctl.selectDiscrete(row, i));
      case 'film':
        child = FilmStrip(catalog: rc.film, selected: e.film, onSelect: (i) => ctl.selectDiscrete(row, i));
      case 'filmPaper':
        child = FilmPaperStrip(
            labels: [for (final id in rc.film.paperIds) rc.film.paperName(id)], index: row.readIndex!(rc), onSelect: (i) => ctl.selectDiscrete(row, i));
      case 'channel':
        child = ChipPicker(
            keyPrefix: 'channel',
            label: '通道',
            labels: curveChannelNames,
            index: rc.sel.curveCh,
            dots: const [Fx.chRgb, Fx.chR, Fx.chG, Fx.chB],
            onSelect: (i) => ctl.selectDiscrete(row, i));
      case 'zone':
        child = ChipPicker(keyPrefix: 'zone', label: '区域', labels: gradeZoneNames, index: rc.sel.gradeZone, onSelect: (i) => ctl.selectDiscrete(row, i));
      case 'ratio':
        child = ChipPicker(keyPrefix: 'crop', label: '裁切比例', labels: cropRatioNames, index: row.readIndex!(rc), onSelect: (i) => ctl.selectDiscrete(row, i));
      case 'palette':
        child = SizedBox.expand(child: ControlPad(ctl: ctl, row: row, fromWindow: true));
      case 'toneTarget':
        child = Row(crossAxisAlignment: CrossAxisAlignment.center, children: [
          ToneMini(photo: host.photo, engine: ctl.engine, app: ctl.app, edit: e, nearest: host.nearest, onSelect: ctl.selectTone),
          const SizedBox(width: 12),
          Expanded(child: Text(tonePlain(e.toneTarget ?? host.nearest), style: T.small(c.text2), maxLines: 2)),
        ]);
      case 'wheel':
        child = SizedBox.expand(child: ControlPad(ctl: ctl, row: row, fromWindow: true));
      default:
        if (row.twoD == TwoD.curve || row.twoD == TwoD.offset) {
          child = SizedBox.expand(child: ControlPad(ctl: ctl, row: row, fromWindow: true));
        }
    }
    if (child == null) return const SizedBox.shrink();
    return Container(
      key: ValueKey('extra-${row.id}'),
      height: h,
      alignment: Alignment.topLeft,
      padding: const EdgeInsets.fromLTRB(12, 4, 12, 8),
      child: child,
    );
  }
}
