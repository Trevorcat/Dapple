// Docked control panel of the phone-portrait editor (docs/design/editor-panel.md). It lives in the
// free area between the photo and the category dock; nothing covers the photo. Two tiers, no vertical
// scrolling: a strip of parameter chips (name + value) and, right under it, ONE large control for the
// selected parameter: a full-width ruler, a 2D pad, or a picker. A drag on the ruler / pad goes through
// the same [EditorController] path as a drag on the photo, so both stay in sync.
//
// Performance: the panel rebuilds on structural changes only (selection, first step of a drag, undo).
// While a finger moves, the ruler, the pad and the selected chip's value repaint from the controller's
// readout notifier; the rest of the panel is not rebuilt.
import 'dart:math' as math;

import 'package:flutter/foundation.dart' show ValueListenable;
import 'package:flutter/material.dart';
import 'package:flutter/semantics.dart';

import '../../editor/gesture_arbiter.dart' show LockAxis;
import '../../editor/param_rows.dart';
import '../../editor/value_mapping.dart';
import '../../editor/window_manager.dart';
import '../../model/params.dart';
import '../../theme/tokens.dart';
import '../../widgets/icons.dart';
import 'control_pad.dart';
import 'editor_controller.dart';
import 'editor_overlays.dart' show RulerPainter;
import 'editor_widgets.dart';
import 'float_window.dart' show NumberEntry, MiniHistogramBox;
import 'mini_maps.dart';
import '../../services/perf_probe.dart';

// ------------------------------------------------------------------ metrics

const double kPanelHandleH = 24;
const double kPanelStripH = 56;
const double kRulerValueH = 44;
const double kRulerH = 64;
const double kRulerTierH = kRulerValueH + kRulerH + 4;
const double kFilmStripH = 92;

/// Panel height when folded to the handle and the chip strip.
const double kPanelCollapsedH = kPanelHandleH + kPanelStripH + 4;

/// The chip strip grows with the text scale (name over value must fit in a chip).
double panelStripH(double textScale) => kPanelStripH + math.max(0.0, textScale - 1) * 36;

double panelCollapsedH(double textScale) => kPanelHandleH + panelStripH(textScale) + 4;

bool isFilmRow(ParamRow r) => r.id == 'film' || r.id == 'filmPaper' || r.id.startsWith('film_');

/// Smallest pad edge: 40 % of the screen width, at least 168 dp.
double padMinHeight(double screenWidth) => math.max(screenWidth * 0.4, 168);

double _tierNeed(ParamRow r, double screenWidth) {
  switch (r.kind) {
    case RowKind.oneD:
      return kRulerTierH;
    case RowKind.twoD:
      return r.twoD == TwoD.tone ? 168 : padMinHeight(screenWidth) + 16;
    case RowKind.discrete:
      switch (r.id) {
        case 'band':
          return 64;
        case 'look':
          return 100;
        case 'filmPaper':
          return 64;
        default:
          return 60;
      }
    case RowKind.action:
      return 0;
  }
}

/// Minimum panel height for category [g]: the tallest control any of its rows needs, so that moving
/// between rows never moves the photo.
double dockedPanelMinHeight(EditorController ctl, EGroup g, double screenWidth, {double textScale = 1}) {
  var tier = 0.0;
  var film = false;
  for (final r in ctl.rowsOf(g)) {
    if (isFilmRow(r)) {
      film = true;
      continue;
    }
    tier = math.max(tier, _tierNeed(r, screenWidth));
  }
  if (film) tier = math.max(tier, kFilmStripH + kRulerTierH);
  return kPanelHandleH + panelStripH(textScale) + tier;
}

/// Height the selected parameter's controls need (handle + strip + lower tier).
double dockedPanelNeed(EditorController ctl, EGroup g, double screenWidth, {double textScale = 1}) {
  final rows = ctl.rowsOf(g);
  if (rows.isEmpty) return kPanelHandleH + panelStripH(textScale);
  final active = ctl.activeRowOf(g, rows);
  double tier;
  if (isFilmRow(active) && !ctl.ctx().film.isEmpty && active.kind != RowKind.twoD) {
    tier = kFilmStripH + (active.id == 'film' ? 8 : (active.id == 'filmPaper' ? 64 : kRulerTierH));
  } else {
    tier = _tierNeed(active, screenWidth);
  }
  return kPanelHandleH + panelStripH(textScale) + tier;
}

const double kHistGap = 22;
const double kHistMax = 120;

/// Panel height for [g]: what the selected parameter needs, plus what it can use of the [spare] height the
/// photo leaves free. The panel hugs its content; whatever it cannot use stays with the photo region.
/// A wide histogram (at most [kHistMax]) and a taller pad (at most square) are the only things that grow.
double dockedPanelHeight(EditorController ctl, EGroup g, double screenWidth, {double textScale = 1, required double regionH, required double photoH}) {
  final need = dockedPanelNeed(ctl, g, screenWidth, textScale: textScale);
  final spare = math.max(0.0, regionH - photoH - 8 - need);
  if (!dockedRoomy(ctl, g)) return need;
  final rows = ctl.rowsOf(g);
  final a = ctl.activeRowOf(g, rows);
  if (a.kind == RowKind.twoD) {
    final padMax = screenWidth - 24;
    return need + math.min(spare, math.max(0.0, padMax - padMinHeight(screenWidth)));
  }
  return need + (spare >= 70 ? kHistGap + math.min(kHistMax, spare - kHistGap) : 0);
}

/// True when the selected parameter can use spare room: pads, and rulers of the categories that show a
/// wide histogram. The pickers and plain rulers stay at their natural height.
bool dockedRoomy(EditorController ctl, EGroup g) {
  final rows = ctl.rowsOf(g);
  if (rows.isEmpty) return false;
  final a = ctl.activeRowOf(g, rows);
  if (a.kind == RowKind.twoD) return a.twoD != TwoD.tone;
  final hist = g == EGroup.light || g == EGroup.curve || g == EGroup.style;
  return a.kind == RowKind.oneD && hist && !isFilmRow(a);
}

// ------------------------------------------------------------------ host (frame + animated height)

/// Positions the panel above the dock and clips it to the animated [height]; the content is laid out at
/// its final [contentHeight] and revealed from the top, so chips never reflow during the animation.
class DockedPanelHost extends StatefulWidget {
  const DockedPanelHost({
    super.key,
    required this.controller,
    required this.height,
    required this.contentHeight,
    required this.group,
    required this.collapsed,
    required this.bottom,
    required this.left,
    required this.right,
  });
  final EditorController controller;

  /// Animated visible height (0 = closed).
  final ValueListenable<double> height;
  final double contentHeight;
  final EGroup? group;
  final bool collapsed;
  final double bottom, left, right;

  @override
  State<DockedPanelHost> createState() => _DockedPanelHostState();
}

class _DockedPanelHostState extends State<DockedPanelHost> {
  EGroup? _last;
  bool _lastCollapsed = false;
  double _lastContent = kPanelCollapsedH;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    if (widget.group != null) {
      _last = widget.group;
      _lastCollapsed = widget.collapsed;
      _lastContent = widget.contentHeight;
    }
    final g = _last;
    return Positioned(
      left: widget.left,
      right: widget.right,
      bottom: widget.bottom,
      child: ValueListenableBuilder<double>(
        valueListenable: widget.height,
        builder: (context, h, child) {
          if (h < 1 || g == null) return const SizedBox.shrink();
          return Opacity(
            opacity: (h / 48).clamp(0.0, 1.0),
            child: Container(
              key: const ValueKey('docked-panel'),
              height: h,
              decoration: BoxDecoration(
                color: c.surface,
                borderRadius: BorderRadius.circular(20),
                border: Border.all(color: c.sep),
                boxShadow: const [BoxShadow(color: Color(0x1F000000), blurRadius: 16, offset: Offset(0, 4))],
              ),
              clipBehavior: Clip.antiAlias,
              child: OverflowBox(
                alignment: Alignment.topCenter,
                minHeight: _lastContent,
                maxHeight: _lastContent,
                child: DockedPanel(controller: widget.controller, group: g, collapsed: _lastCollapsed),
              ),
            ),
          );
        },
      ),
    );
  }
}

// ------------------------------------------------------------------ panel

class DockedPanel extends StatefulWidget {
  const DockedPanel({super.key, required this.controller, required this.group, required this.collapsed});
  final EditorController controller;
  final EGroup group;
  final bool collapsed;

  @override
  State<DockedPanel> createState() => _DockedPanelState();
}

class _DockedPanelState extends State<DockedPanel> {
  EditorController get ctl => widget.controller;
  bool _wasDragging = false;
  late final Listenable _all = Listenable.merge([ctl.wm, ctl.edits, ctl.dragging, ctl.editingRow]);

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
    PerfProbe.count('dockPanel');
    final g = widget.group;
    final rows = ctl.rowsOf(g);
    if (rows.isEmpty) return const SizedBox.shrink();
    final active = ctl.activeRowOf(g, rows);
    final rc = ctl.ctx();
    return Semantics(
      container: true,
      label: '${groupLabels[g]}调整面板',
      child: Column(key: ValueKey('panel-${g.name}'), children: [
        _Handle(ctl: ctl, group: g, collapsed: widget.collapsed),
        SizedBox(
          height: panelStripH(MediaQuery.textScalerOf(context).scale(15) / 15),
          child: _ChipStrip(ctl: ctl, group: g, rows: rows, active: active, rc: rc, collapsed: widget.collapsed),
        ),
        if (!widget.collapsed) Expanded(child: _Tier(ctl: ctl, group: g, rows: rows, active: active, rc: rc)),
      ]),
    );
  }
}

// ------------------------------------------------------------------ handle

class _Handle extends StatelessWidget {
  const _Handle({required this.ctl, required this.group, required this.collapsed});
  final EditorController ctl;
  final EGroup group;
  final bool collapsed;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    void toggle() => ctl.wm.setCollapsed(group, !collapsed);
    return Semantics(
      button: true,
      label: collapsed ? '展开面板' : '收起面板',
      excludeSemantics: true,
      onTap: toggle,
      child: GestureDetector(
        key: const ValueKey('panel-handle'),
        behavior: HitTestBehavior.opaque,
        onTap: toggle,
        onVerticalDragEnd: (d) {
          final v = d.velocity.pixelsPerSecond.dy;
          if (v > 150 && !collapsed) ctl.wm.setCollapsed(group, true);
          if (v < -150 && collapsed) ctl.wm.setCollapsed(group, false);
        },
        child: SizedBox(
          height: kPanelHandleH,
          child: Center(child: Container(width: 36, height: 4, decoration: BoxDecoration(color: c.track, borderRadius: BorderRadius.circular(2)))),
        ),
      ),
    );
  }
}

// ------------------------------------------------------------------ chip strip

class _ChipStrip extends StatefulWidget {
  const _ChipStrip({required this.ctl, required this.group, required this.rows, required this.active, required this.rc, required this.collapsed});
  final EditorController ctl;
  final EGroup group;
  final List<ParamRow> rows;
  final ParamRow active;
  final RowContext rc;
  final bool collapsed;

  @override
  State<_ChipStrip> createState() => _ChipStripState();
}

class _ChipStripState extends State<_ChipStrip> {
  final ScrollController _sc = ScrollController();
  final Map<String, GlobalKey> _keys = {};

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _centre(animate: false));
  }

  @override
  void didUpdateWidget(_ChipStrip old) {
    super.didUpdateWidget(old);
    if (old.active.id != widget.active.id || old.group != widget.group) {
      WidgetsBinding.instance.addPostFrameCallback((_) => _centre(animate: old.group == widget.group));
    }
  }

  void _centre({required bool animate}) {
    final ctx = _keys[widget.active.id]?.currentContext;
    if (!mounted || ctx == null || !_sc.hasClients) return;
    Scrollable.ensureVisible(ctx, alignment: 0.5, duration: animate ? const Duration(milliseconds: 220) : Duration.zero, curve: Curves.easeOutCubic);
  }

  @override
  void dispose() {
    _sc.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final ctl = widget.ctl;
    final adjusted = ctl.edit.groupAdjusted(widget.group);
    final resetLeft = ctl.wm.side == Hand.right; // the far side from the thumb
    final reset = SizedBox(
      width: 48,
      child: Visibility(
        visible: adjusted,
        maintainSize: true,
        maintainAnimation: true,
        maintainState: true,
        child: Semantics(
          button: true,
          label: '复位${groupLabels[widget.group]}',
          excludeSemantics: true,
          child: GestureDetector(
            key: ValueKey('reset-${widget.group.name}'),
            behavior: HitTestBehavior.opaque,
            onTap: () => ctl.resetCategoryOf(widget.group),
            child: Center(child: DIcon(DI.reset, size: 20, color: c.text2)),
          ),
        ),
      ),
    );
    final list = SingleChildScrollView(
      key: ValueKey('chip-strip-${widget.group.name}'),
      controller: _sc,
      scrollDirection: Axis.horizontal,
      padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 4),
      child: Row(children: [
        for (var i = 0; i < widget.rows.length; i++) ...[
          if (i > 0) const SizedBox(width: 6),
          KeyedSubtree(
            key: _keys.putIfAbsent(widget.rows[i].id, GlobalKey.new),
            child: _RowChip(
              key: ValueKey('chip-${widget.group.name}-${widget.rows[i].id}'),
              ctl: ctl,
              group: widget.group,
              row: widget.rows[i],
              rc: widget.rc,
              selected: widget.rows[i].id == widget.active.id,
              collapsed: widget.collapsed,
            ),
          ),
        ],
      ]),
    );
    // A fixed slot for the category reset on the far side of the thumb; the chips fade out under its edge.
    final faded = ShaderMask(
      blendMode: BlendMode.dstIn,
      shaderCallback: (r) => LinearGradient(
        colors: resetLeft ? const [Color(0x00FFFFFF), Colors.white] : const [Colors.white, Color(0x00FFFFFF)],
        stops: resetLeft ? const [0, 0.04] : const [0.96, 1],
      ).createShader(r),
      child: list,
    );
    return Row(children: [
      if (resetLeft) reset,
      Expanded(child: faded),
      if (!resetLeft) reset,
    ]);
  }
}

class _RowChip extends StatelessWidget {
  const _RowChip({super.key, required this.ctl, required this.group, required this.row, required this.rc, required this.selected, required this.collapsed});
  final EditorController ctl;
  final EGroup group;
  final ParamRow row;
  final RowContext rc;
  final bool selected, collapsed;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final modified = row.isModified(rc);
    final isAction = row.kind == RowKind.action;
    final adjustable = row.kind == RowKind.oneD || row.kind == RowKind.discrete || row.twoD == TwoD.curve;
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
    final fg = modified ? c.accent : c.text;
    final valueStyle = T.small(selected ? c.accent : (modified ? c.accent : c.text2), w: FontWeight.w600, tabular: true);
    final rest = row.text(rc);
    // A selected ruler parameter shows only its name: its value is the big number above the ruler.
    final nameOnly = selected && row.kind == RowKind.oneD;
    Widget value = Text(rest, maxLines: 1, overflow: TextOverflow.ellipsis, style: valueStyle);
    if (selected && !isAction && !nameOnly) {
      value = ListenableBuilder(
        listenable: ctl.readout,
        builder: (context, _) {
          final live = ctl.dragging.value && ctl.readout.name == row.label;
          return Text(live ? ctl.readout.value : rest, maxLines: 1, overflow: TextOverflow.ellipsis, style: valueStyle);
        },
      );
    }
    final body = Container(
      key: ValueKey('chip-body-${row.id}'),
      constraints: const BoxConstraints(minWidth: 64, maxWidth: 132, minHeight: 48),
      padding: const EdgeInsets.symmetric(horizontal: 10),
      decoration: BoxDecoration(
        color: selected ? c.accentSoft : c.chip,
        borderRadius: BorderRadius.circular(14),
        border: selected ? Border.all(color: c.accent, width: 1.5) : null,
      ),
      child: Stack(clipBehavior: Clip.none, children: [
        Column(mainAxisSize: MainAxisSize.min, mainAxisAlignment: MainAxisAlignment.center, children: [
          const SizedBox(height: 4),
          Text(row.label, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.small(selected ? c.accent : fg, w: modified || selected ? FontWeight.w600 : FontWeight.w400)),
          if (!nameOnly) value,
          const SizedBox(height: 4),
        ]),
        if (modified && !isAction)
          Positioned(
            key: ValueKey('mark-${row.id}'),
            right: -4,
            top: 2,
            child: Container(width: 6, height: 6, decoration: BoxDecoration(color: c.accent, shape: BoxShape.circle)),
          ),
      ]),
    );
    return Semantics(
      container: true,
      label: row.label,
      hint: row.hint,
      value: row.semanticValue(rc),
      selected: selected,
      increasedValue: inc,
      decreasedValue: dec,
      onIncrease: adjustable ? () => ctl.stepRow(row, 1) : null,
      onDecrease: adjustable ? () => ctl.stepRow(row, -1) : null,
      onTap: () => ctl.selectRow(group, row),
      customSemanticsActions: {
        if (!isAction) const CustomSemanticsAction(label: '复位到自动'): () => ctl.resetRow(row),
        if (row.typeable) const CustomSemanticsAction(label: '输入数值'): () => ctl.beginNumeric(row),
      },
      excludeSemantics: true,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        // An unselected chip selects on tap at once (no double-tap delay); the selected chip resets on double tap.
        onTap: selected && !collapsed ? null : () => ctl.selectRow(group, row),
        onDoubleTap: selected && !isAction ? () => ctl.resetRow(row) : null,
        child: body,
      ),
    );
  }
}

// ------------------------------------------------------------------ lower tier

class _Tier extends StatelessWidget {
  const _Tier({required this.ctl, required this.group, required this.rows, required this.active, required this.rc});
  final EditorController ctl;
  final EGroup group;
  final List<ParamRow> rows;
  final ParamRow active;
  final RowContext rc;

  @override
  Widget build(BuildContext context) {
    // the stock strip stays above a ruler or the paper strip; a pad takes the room instead
    final film = isFilmRow(active) && !rc.film.isEmpty && active.kind != RowKind.twoD;
    if (!film) return _lower(context, active);
    final filmRow = rows.firstWhere((r) => r.id == 'film', orElse: () => active);
    final e = rc.edit;
    return Column(children: [
      SizedBox(
        height: kFilmStripH,
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 6),
          child: FilmStrip(catalog: rc.film, selected: e.film, onSelect: (i) => ctl.selectDiscrete(filmRow, i)),
        ),
      ),
      Expanded(child: active.id == 'film' ? const SizedBox.shrink() : _lower(context, active)),
    ]);
  }

  Widget _lower(BuildContext context, ParamRow row) {
    switch (row.kind) {
      case RowKind.oneD:
        return RulerTier(ctl: ctl, row: row, group: group);
      case RowKind.twoD:
        if (row.twoD == TwoD.tone) return _tone(context, row);
        return Padding(
          padding: const EdgeInsets.fromLTRB(12, 4, 12, 12),
          child: SizedBox.expand(child: ControlPad(ctl: ctl, row: row)),
        );
      case RowKind.discrete:
        return _picker(context, row);
      case RowKind.action:
        return const SizedBox.shrink();
    }
  }

  Widget _tone(BuildContext context, ParamRow row) {
    final c = context.c;
    final e = rc.edit;
    return LayoutBuilder(builder: (context, cons) {
      const gap = 4.0;
      final cell = ((cons.maxHeight - 16 - 2 * gap) / 3).clamp(32.0, 64.0);
      return Padding(
        padding: const EdgeInsets.fromLTRB(16, 8, 16, 8),
        child: Row(crossAxisAlignment: CrossAxisAlignment.center, children: [
          ToneMini(
            key: const ValueKey('extra-toneTarget'),
            photo: ctl.app.photoById(ctl.id)!,
            engine: ctl.engine,
            app: ctl.app,
            edit: e,
            nearest: rc.nearestTone,
            onSelect: ctl.selectTone,
            cell: cell,
            gap: gap,
          ),
          const SizedBox(width: 16),
          Expanded(child: Text(tonePlain(e.toneTarget ?? rc.nearestTone), style: T.small(c.text2), maxLines: 3)),
        ]),
      );
    });
  }

  Widget _picker(BuildContext context, ParamRow row) {
    final e = rc.edit;
    Widget? child;
    switch (row.id) {
      case 'band':
        child = Center(child: BandDots(index: rc.sel.band, onSelect: (i) => ctl.selectDiscrete(row, i)));
      case 'look':
        child = LookStrip(index: row.readIndex!(rc), onSelect: (i) => ctl.selectDiscrete(row, i));
      case 'filmPaper':
        child = FilmPaperStrip(labels: [for (final id in rc.film.paperIds) rc.film.paperName(id)], index: row.readIndex!(rc), onSelect: (i) => ctl.selectDiscrete(row, i));
      case 'channel':
        child = ChipPicker(
            scroll: true,
            keyPrefix: 'channel',
            label: '通道',
            labels: curveChannelNames,
            index: rc.sel.curveCh,
            dots: const [Fx.chRgb, Fx.chR, Fx.chG, Fx.chB],
            onSelect: (i) => ctl.selectDiscrete(row, i));
      case 'zone':
        child = ChipPicker(scroll: true, keyPrefix: 'zone', label: '区域', labels: gradeZoneNames, index: rc.sel.gradeZone, onSelect: (i) => ctl.selectDiscrete(row, i));
      case 'ratio':
        child = ChipPicker(scroll: true, keyPrefix: 'crop', label: '裁切比例', labels: cropRatioNames, index: row.readIndex!(rc), onSelect: (i) => ctl.selectDiscrete(row, i));
      case 'film':
        child = FilmStrip(catalog: rc.film, selected: e.film, onSelect: (i) => ctl.selectDiscrete(row, i));
    }
    if (child == null) return const SizedBox.shrink();
    return Container(
      key: ValueKey('extra-${row.id}'),
      alignment: Alignment.topCenter,
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 12),
      child: child,
    );
  }
}

// ------------------------------------------------------------------ ruler tier

/// Value line, reset and the full-width ruler of a 1D parameter. In a tall panel (a landscape photo
/// leaves room) the histogram is drawn wide under the ruler; otherwise a small one sits in the value line.
class RulerTier extends StatelessWidget {
  const RulerTier({super.key, required this.ctl, required this.row, required this.group});
  final EditorController ctl;
  final ParamRow row;
  final EGroup group;

  bool get _hist => group == EGroup.light || group == EGroup.curve || group == EGroup.style;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final rc = ctl.ctx();
    final modified = row.isModified(rc);
    final rest = row.value(rc);
    return LayoutBuilder(builder: (context, cons) {
      final extra = cons.maxHeight - kRulerTierH;
      final wide = _hist && extra >= 70;
      final valueStyle = T.h2(modified ? c.accent : c.text).copyWith(fontWeight: FontWeight.w600, fontFeatures: const [FontFeature.tabularFigures()]);
      final editing = ctl.editingRow.value == row.id;
      Widget valueText;
      if (editing) {
        valueText = NumberEntry(key: ValueKey('entry-${row.id}'), initial: row.inputText(rc), style: valueStyle, onSubmit: (t) => ctl.commitNumeric(row, t));
      } else {
        valueText = ListenableBuilder(
          listenable: ctl.readout,
          builder: (context, _) {
            final live = ctl.dragging.value && ctl.readout.name == row.label;
            return Text(live ? ctl.readout.value : row.text(rc), maxLines: 1, style: valueStyle);
          },
        );
        if (row.typeable) {
          valueText = GestureDetector(
            key: ValueKey('value-${row.id}'),
            behavior: HitTestBehavior.opaque,
            onTap: () => ctl.beginNumeric(row),
            child: ConstrainedBox(constraints: const BoxConstraints(minWidth: 88, minHeight: 44), child: Center(child: valueText)),
          );
        }
      }
      final resetBtn = SizedBox(
        width: 48,
        child: Visibility(
          visible: modified,
          maintainSize: true,
          maintainAnimation: true,
          maintainState: true,
          child: Semantics(
            button: true,
            label: '复位${row.label}',
            excludeSemantics: true,
            child: GestureDetector(
              key: const ValueKey('reset-row'),
              behavior: HitTestBehavior.opaque,
              onTap: () => ctl.resetRow(row),
              child: SizedBox(height: 44, child: Center(child: DIcon(DI.reset, size: 18, color: c.text2))),
            ),
          ),
        ),
      );
      final line = SizedBox(
        height: kRulerValueH,
        child: Row(children: [
          // a small histogram where a wide one does not fit; the same width as the reset slot keeps the value centred
          SizedBox(width: 112, child: _hist && !wide ? Padding(padding: const EdgeInsets.only(left: 12), child: MiniHistogramBox(analysis: ctl.analysis())) : null),
          Expanded(child: Center(child: valueText)),
          const SizedBox(width: 112),
        ]),
      );
      final ruler = SizedBox(height: kRulerH, child: DockRuler(ctl: ctl, row: row, restValue: rest));
      return Align(
        alignment: Alignment.topCenter,
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          Stack(alignment: Alignment.centerRight, children: [line, Padding(padding: const EdgeInsets.only(right: 8), child: resetBtn)]),
          ruler,
          if (wide)
            Padding(
              padding: const EdgeInsets.fromLTRB(12, 10, 12, 12),
              child: SizedBox(height: math.min(extra - kHistGap, kHistMax), width: double.infinity, child: _WideHistogram(ctl: ctl)),
            ),
        ]),
      );
    });
  }
}

class _WideHistogram extends StatelessWidget {
  const _WideHistogram({required this.ctl});
  final EditorController ctl;
  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final a = ctl.analysis();
    return Semantics(
      image: true,
      label: '直方图，${tonePlain(a.nearest)}',
      child: ClipRRect(
        borderRadius: BorderRadius.circular(12),
        child: ColoredBox(color: c.bg, child: CustomPaint(key: const ValueKey('panel-histogram'), painter: HistPainter(a, c.histFill, c.text, c.zoneA, c.accent))),
      ),
    );
  }
}

// ------------------------------------------------------------------ ruler

/// The full-width ruler of the selected 1D parameter. Dragging adjusts the value right under the eye
/// (relative mapping with the same velocity gain and precision bands as a drag on the photo: moving the
/// finger up or down while dragging gives fine / ultra-fine). Double tap resets. It repaints from the
/// readout notifier, so a drag on the photo moves this ruler too.
class DockRuler extends StatelessWidget {
  const DockRuler({super.key, required this.ctl, required this.row, required this.restValue});
  final EditorController ctl;
  final ParamRow row;
  final double restValue;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final rc = ctl.ctx();
    final v = restValue;
    final inc = row.fmt!((v + row.step).clamp(row.min, row.max).toDouble());
    final dec = row.fmt!((v - row.step).clamp(row.min, row.max).toDouble());
    return Semantics(
      container: true,
      label: row.label,
      value: row.semanticValue(rc),
      increasedValue: inc,
      decreasedValue: dec,
      onIncrease: () => ctl.stepRow(row, 1),
      onDecrease: () => ctl.stepRow(row, -1),
      excludeSemantics: true,
      child: ControlSurface(
        ctl: ctl,
        row: row,
        axis: LockAxis.horizontal,
        child: RepaintBoundary(
          child: CustomPaint(
            key: const ValueKey('panel-ruler'),
            size: Size.infinite,
            painter: DockRulerPainter(
              ctl: ctl,
              row: row,
              rest: restValue,
              accent: c.accent,
              tick: c.text2,
              label: c.text2,
              marker: c.text,
              baseStyle: T.cap(c.text2),
            ),
          ),
        ),
      ),
    );
  }
}

class DockRulerPainter extends CustomPainter {
  DockRulerPainter({
    required this.ctl,
    required this.row,
    required this.rest,
    required this.accent,
    required this.tick,
    required this.label,
    required this.marker,
    required this.baseStyle,
  }) : super(repaint: Listenable.merge([ctl.readout, ctl.edits]));

  final EditorController ctl;
  final ParamRow row;
  final double rest;
  final Color accent, tick, label, marker;
  final TextStyle baseStyle;

  /// Tick and marker labels are laid out once per text and colour (a text layout per frame is expensive
  /// on a slow phone); the cache is cleared when it grows past a few hundred entries.
  static final Map<String, TextPainter> _labels = {};

  static TextPainter _tp(String t, Color color, TextStyle base, double size) {
    if (_labels.length > 400) _labels.clear();
    return _labels.putIfAbsent(
      '$t|${color.toARGB32()}|$size',
      () => TextPainter(
        text: TextSpan(text: t, style: base.copyWith(color: color, fontSize: size, height: 1, fontWeight: FontWeight.w500, fontFeatures: const [FontFeature.tabularFigures()])),
        textDirection: TextDirection.ltr,
      )..layout(),
    );
  }

  String _tickText(double u) {
    final v = u / row.inputScale;
    final s = row.inputScale == 1 ? v.round().toString() : v.toStringAsFixed(row.inputScale >= 20 ? 2 : 1);
    return s.startsWith('-') ? '−${s.substring(1)}' : s;
  }

  @override
  void paint(Canvas canvas, Size size) {
    final range = row.max - row.min;
    if (range <= 0 || size.isEmpty) return;
    final r = ctl.readout;
    final live = ctl.dragging.value && r.name == row.label;
    final v = live ? r.v : rest;
    final band = live ? r.band : PrecisionBand.standard;
    final endSide = live ? r.endSide : 0;
    final span = RulerPainter.spanFor(range, row.step, band);
    final w = size.width, h = size.height, cx = w / 2;
    final ppu = w / span;
    final y0 = h - 6;
    canvas.save();
    canvas.clipRect(Offset.zero & size);
    canvas.translate(-endSide * 4.0, 0);
    var ts = row.step;
    while (ts * ppu < 7) {
      ts *= ts * 2 < 5 ? 2 : 5;
    }
    final lo = math.max(row.min, v - span / 2 - ts), hi = math.min(row.max, v + span / 2 + ts);
    final first = (lo / ts).ceil();
    final p = Paint()..strokeCap = StrokeCap.round;
    for (var i = first; i * ts <= hi; i++) {
      final u = i * ts;
      final x = cx + (u - v) * ppu;
      final major = i % 5 == 0;
      final d = ((x - cx).abs() / (w / 2)).clamp(0.0, 1.0);
      final fade = 1 - smoothstep((d - 0.55) / 0.45);
      p
        ..color = tick.withValues(alpha: (major ? 0.75 : 0.4) * fade)
        ..strokeWidth = major ? 1.6 : 1.2;
      canvas.drawLine(Offset(x, y0), Offset(x, y0 - (major ? 20 : 11)), p);
      // the label nearest the pointer would sit under it: fade it out within 34 dp of the centre
      final nearPointer = smoothstep(((x - cx).abs() - 12) / 26);
      if (major && fade * nearPointer > 0.12) {
        final tp = _tp(_tickText(u), label.withValues(alpha: fade * nearPointer), baseStyle, 11);
        tp.paint(canvas, Offset(x - tp.width / 2, y0 - 20 - 4 - tp.height));
      }
    }
    // auto marker: a taller mark with its label, at the parameter's own default
    final ax = cx + (row.auto - v) * ppu;
    final atAuto = (v - row.auto).abs() < 1e-6;
    if (ax >= 10 && ax <= w - 10) {
      canvas.drawLine(
          Offset(ax, y0),
          Offset(ax, y0 - 22),
          Paint()
            ..color = marker.withValues(alpha: atAuto ? 1 : 0.55)
            ..strokeWidth = atAuto ? 3 : 2
            ..strokeCap = StrokeCap.round);
      final tp = _tp('自动', marker.withValues(alpha: atAuto ? 1 : 0.7), baseStyle, 10);
      tp.paint(canvas, Offset(ax - tp.width / 2, 1));
    } else {
      final left = ax < 10;
      final tp = _tp(left ? '‹ 自动' : '自动 ›', marker.withValues(alpha: 0.7), baseStyle, 10);
      tp.paint(canvas, Offset(left ? 6 : w - 6 - tp.width, 1));
    }
    canvas.restore();
    // pointer
    canvas.drawLine(
        Offset(cx, 18),
        Offset(cx, y0 + 2),
        Paint()
          ..color = accent
          ..strokeWidth = 2.5
          ..strokeCap = StrokeCap.round);
    canvas.drawCircle(Offset(cx, 18), 3.5, Paint()..color = accent);
  }

  @override
  bool shouldRepaint(DockRulerPainter old) =>
      old.rest != rest || old.row.id != row.id || old.accent != accent || old.tick != tick || old.baseStyle != baseStyle;
}
