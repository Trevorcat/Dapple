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
import 'panel_widgets.dart' show NumberEntry, MiniHistogramBox, groupIcons;
import 'mini_maps.dart';
import '../../services/perf_probe.dart';

// ------------------------------------------------------------------ metrics

const double kPanelHandleH = 24;
const double kPanelStripH = 56;
const double kRulerValueH = 44;
const double kRulerH = 64;
const double kRulerTierH = kRulerValueH + kRulerH + 4;
const double kFilmStripH = 92;

/// Header of the side pane: the category's name and its reset (its own slot, never over a chip).
const double kSideHeaderH = 40;

/// Histogram block at the top of the side pane (the column above the controls would otherwise be dead).
const double kSideHistH = 112;

/// Slim row that replaces a 2D pad when the photo itself is the pad (photo-as-pad).
const double kPadHintH = 40;

/// Panel height when folded to the handle and the chip strip.
const double kPanelCollapsedH = kPanelHandleH + kPanelStripH + 4;

/// The chip strip grows with the text scale (name over value must fit in a chip).
double panelStripH(double textScale) => kPanelStripH + math.max(0.0, textScale - 1) * 36;

double panelCollapsedH(double textScale) => kPanelHandleH + panelStripH(textScale) + 4;

bool isFilmRow(ParamRow r) => r.id == 'film' || r.id == 'filmPaper' || r.id.startsWith('film_');

/// Rows whose big control is a drag pad (palette, wheel, curve, the film pads; not the tone grid, which is taps).
bool isPadRow(ParamRow r) => r.kind == RowKind.twoD && r.twoD != TwoD.tone;

/// Smallest pad edge: 40 % of the screen width, at least 168 dp.
double padMinHeight(double screenWidth) => math.max(screenWidth * 0.4, 168);

const double kHistGap = 22;
const double kHistMax = 120;

// ------------------------------------------------------------------ side pane metrics

/// Chip grid of the side pane: 2 or 3 columns, as many rows as fit; the grid scrolls on its own when there are more.
int sideGridCols(double paneW) => ((paneW - 12) / (88 + 6)).floor().clamp(2, 3);

double sideChipH(double textScale) => 48 + math.max(0.0, textScale - 1) * 36;

/// One row of the chip grid (it then scrolls).
double sideGridMinHeight(double textScale) => 8 + sideChipH(textScale) + 6;

/// Height of the chip grid for [n] chips: up to three rows, at least one.
double sideGridHeight(int n, double paneW, double textScale) {
  final rows = ((n + sideGridCols(paneW) - 1) ~/ sideGridCols(paneW)).clamp(1, 3);
  return 8 + rows * (sideChipH(textScale) + 6);
}

// ------------------------------------------------------------------ needs

/// What the selected control of a category needs, per placement, and how much taller it can usefully get.
class PanelNeeds {
  const PanelNeeds({required this.bottom, required this.side, required this.sideMin, required this.growBottom, required this.growSide, required this.growMin, required this.pad});

  /// Heights of the whole panel (bottom) / of the pane without the dock (side).
  final double bottom, side;

  /// The least the side pane needs: its chip grid may shrink to one scrolling row.
  final double sideMin;

  /// How much taller a pad or the histogram can make it, and the least spare height that is worth it.
  final double growBottom, growSide, growMin;

  /// The selected row is a drag pad (candidate for photo-as-pad).
  final bool pad;
}

double _rowTier(EditorController ctl, ParamRow r, RowContext rc, double w, {required bool side, required bool padOnPhoto}) {
  switch (r.kind) {
    case RowKind.oneD:
      return kRulerTierH;
    case RowKind.twoD:
      if (r.twoD == TwoD.tone) return side ? 152 : 168;
      if (padOnPhoto) return kPadHintH;
      return (side ? 120.0 : padMinHeight(w)) + 16; // beside the photo the pad is wide and may be low
    case RowKind.discrete:
      switch (r.id) {
        case 'band':
          return 64;
        case 'look':
          return 100;
        case 'film':
          return kFilmStripH + 24; // the stock strip with its 12 dp of air above and below
        case 'filmPaper':
          return 64;
        default:
          return 60;
      }
    case RowKind.action:
      return 0;
  }
}

/// The needs of category [g]. [groupLevel] takes the tallest row (the stable basis for choosing a placement);
/// otherwise the selected row. [padOnPhoto] replaces the selected pad by the slim hint row.
PanelNeeds panelNeeds(EditorController ctl, EGroup g,
    {required double screenW, required double paneW, double textScale = 1, bool collapsed = false, bool groupLevel = false, bool padOnPhoto = false}) {
  final rows = ctl.rowsOf(g);
  final rc = ctl.ctx();
  final ts = textScale;
  final gridH = sideGridHeight(rows.length, paneW, ts);
  final strip = panelStripH(ts);
  if (rows.isEmpty) {
    return PanelNeeds(bottom: kPanelHandleH + strip, side: gridH + kSideHeaderH, sideMin: gridH + kSideHeaderH, growBottom: 0, growSide: 0, growMin: 0, pad: false);
  }
  final active = ctl.activeRowOf(g, rows);
  var tierB = 0.0, tierS = 0.0;
  if (collapsed) {
    return PanelNeeds(
        bottom: kPanelHandleH + strip + 4,
        side: gridH + kSideHeaderH + 4,
        sideMin: sideGridMinHeight(ts) + kSideHeaderH + 4,
        growBottom: 0,
        growSide: 0,
        growMin: 0,
        pad: false);
  }
  final list = groupLevel ? rows : [active];
  for (final r in list) {
    tierB = math.max(tierB, _rowTier(ctl, r, rc, screenW, side: false, padOnPhoto: padOnPhoto));
    tierS = math.max(tierS, _rowTier(ctl, r, rc, paneW, side: true, padOnPhoto: padOnPhoto));
  }
  if (!groupLevel) {
    // The panel never gets lower than a ruler (so moving between rulers and pickers keeps one height) and grows for
    // a taller control; a pad replaced by the hint row is just that row: the photo gets the room back.
    if (padOnPhoto && isPadRow(active)) {
      // tierB / tierS are already the hint row
    } else {
      final hasRuler = rows.any((r) => r.kind == RowKind.oneD);
      var baseB = 0.0, baseS = 0.0;
      for (final r in rows) {
        if (isPadRow(r) || r.id == 'toneTarget') continue; // the tall ones size the panel only while selected
        baseB = math.max(baseB, _rowTier(ctl, r, rc, screenW, side: false, padOnPhoto: padOnPhoto));
        baseS = math.max(baseS, _rowTier(ctl, r, rc, paneW, side: true, padOnPhoto: padOnPhoto));
      }
      if (hasRuler) {
        baseB = math.min(baseB, kRulerTierH);
        baseS = math.min(baseS, kRulerTierH);
      }
      tierB = math.max(tierB, baseB);
      tierS = math.max(tierS, baseS);
    }
  }
  // Growth: a pad takes spare height, a ruler takes it for the wide histogram. At group level the tallest of its rows
  // sets it, so the panel keeps one height while moving between the category's parameters.
  var gb = 0.0, gs = 0.0, gm = 0.0;
  var padGrows = false;
  var pad = false;
  for (final r in list) {
    if (isPadRow(r)) {
      pad = true;
      if (!padOnPhoto) {
        padGrows = true;
        gb = math.max(gb, math.max(0.0, screenW - 24 - padMinHeight(screenW)));
        gs = math.max(gs, math.max(0.0, paneW - 24 - 120));
      }
    }
  }
  // the wide histogram under a ruler: only a ruler can use that height (a picker would float in a blank band)
  if (groupLevel || active.kind == RowKind.oneD) {
    for (final r in rows) {
      if (r.kind == RowKind.oneD && (g == EGroup.light || g == EGroup.curve || g == EGroup.style) && !isFilmRow(r)) {
        gb = math.max(gb, kHistGap + kHistMax);
        gm = 70;
      }
    }
  }
  if (padGrows) gm = 0;
  // the side pane shows the histogram above its chips instead (not for a pad, which takes that room)
  final sideHist = (!groupLevel && !isPadRow(active)) ? kSideHistH : 0.0;
  if (!groupLevel && !isPadRow(active)) {
    gs = 0;
  }
  return PanelNeeds(
    bottom: kPanelHandleH + strip + tierB,
    side: gridH + kSideHeaderH + tierS + sideHist,
    sideMin: sideGridMinHeight(ts) + kSideHeaderH + tierS,
    growBottom: gb,
    growSide: gs,
    growMin: gm,
    pad: pad,
  );
}

// ------------------------------------------------------------------ host (frame + animated rect)

/// The panel frame at the animated [rect] (bottom panel or side pane). The content is laid out at its final size
/// ([contentSize]) and revealed from the top, so nothing reflows during the animation.
class DockedPanelHost extends StatefulWidget {
  const DockedPanelHost({
    super.key,
    required this.controller,
    required this.rect,
    required this.contentSize,
    required this.group,
    required this.collapsed,
    required this.side,
    required this.padOnPhoto,
    this.activeId,
  });
  final EditorController controller;
  final String? activeId;

  /// Animated visible rectangle (height 0 = closed).
  final ValueListenable<Rect> rect;
  final Size contentSize;
  final EGroup? group;
  final bool collapsed, side, padOnPhoto;

  @override
  State<DockedPanelHost> createState() => _DockedPanelHostState();
}

class _DockedPanelHostState extends State<DockedPanelHost> {
  EGroup? _last;
  bool _lastCollapsed = false, _lastSide = false, _lastPad = false;
  String? _lastActive;
  Size _lastContent = const Size(0, kPanelCollapsedH);

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    if (widget.group != null) {
      _last = widget.group;
      _lastCollapsed = widget.collapsed;
      _lastSide = widget.side;
      _lastPad = widget.padOnPhoto;
      _lastActive = widget.activeId;
      _lastContent = widget.contentSize;
    }
    final g = _last;
    return ValueListenableBuilder<Rect>(
      valueListenable: widget.rect,
      builder: (context, r, _) {
        if (r.height < 1 || r.width < 1 || g == null) return const SizedBox.shrink();
        return Positioned.fromRect(
          rect: r,
          child: Opacity(
            opacity: (r.height / 48).clamp(0.0, 1.0),
            child: Container(
              key: const ValueKey('docked-panel'),
              decoration: BoxDecoration(
                color: c.surface,
                borderRadius: BorderRadius.circular(20),
                border: Border.all(color: c.sep),
                boxShadow: const [BoxShadow(color: Color(0x1F000000), blurRadius: 16, offset: Offset(0, 4))],
              ),
              clipBehavior: Clip.antiAlias,
              child: OverflowBox(
                alignment: Alignment.topCenter,
                minWidth: _lastContent.width,
                maxWidth: _lastContent.width,
                minHeight: _lastContent.height,
                maxHeight: _lastContent.height,
                child: DockedPanel(controller: widget.controller, group: g, collapsed: _lastCollapsed, side: _lastSide, padOnPhoto: _lastPad, activeId: _lastActive),
              ),
            ),
          ),
        );
      },
    );
  }
}

// ------------------------------------------------------------------ panel

class DockedPanel extends StatefulWidget {
  const DockedPanel({super.key, required this.controller, required this.group, required this.collapsed, this.side = false, this.padOnPhoto = false, this.activeId});

  /// The row the panel is laid out for; null = the controller's selection.
  final String? activeId;
  final EditorController controller;
  final EGroup group;
  final bool collapsed;

  /// The side pane: chips wrap into a grid, the control below uses the pane's width.
  final bool side;

  /// A 2D value is driven by dragging the photo: the big pad is replaced by a slim hint row.
  final bool padOnPhoto;

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
    final active = rows.where((r) => r.id == widget.activeId).firstOrNull ?? ctl.activeRowOf(g, rows);
    final rc = ctl.ctx();
    final ts = MediaQuery.textScalerOf(context).scale(15) / 15;
    final tier = widget.collapsed ? null : _Tier(ctl: ctl, group: g, rows: rows, active: active, rc: rc, side: widget.side, padOnPhoto: widget.padOnPhoto);
    if (widget.side) {
      return Semantics(
        container: true,
        label: '${groupLabels[g]}调整面板',
        child: LayoutBuilder(builder: (context, cons) {
          // the grid takes what the control below leaves, between one scrolling row and its full height
          final tierNeed = widget.collapsed ? 0.0 : _rowTier(ctl, active, rc, cons.maxWidth, side: true, padOnPhoto: widget.padOnPhoto);
          // whole rows only: the leftover goes to the control below
          final rowH = sideChipH(ts) + 6;
          final fullRows = ((sideGridHeight(rows.length, cons.maxWidth, ts) - 8) / rowH).round();
          final room = cons.maxHeight - kSideHeaderH - tierNeed;
          // the histogram goes above the chips when there is room for it besides one row of chips (a pad takes the room instead)
          final showHist = !widget.collapsed && !isPadRow(active) && room - sideGridMinHeight(ts) >= kSideHistH;
          final fitRows = ((room - (showHist ? kSideHistH : 0) - 8) / rowH).floor().clamp(1, fullRows);
          final gridH = 8 + fitRows * rowH;
          return Column(key: ValueKey('panel-${g.name}'), children: [
            _SideHeader(ctl: ctl, group: g),
            if (showHist) SizedBox(key: const ValueKey('side-histogram'), height: kSideHistH, child: Padding(padding: const EdgeInsets.fromLTRB(12, 0, 12, 8), child: SizedBox.expand(child: _WideHistogram(ctl: ctl)))),
            SizedBox(
              height: gridH,
              child: _ChipGrid(ctl: ctl, group: g, rows: rows, active: active, rc: rc, collapsed: widget.collapsed, cols: sideGridCols(cons.maxWidth), scrolls: fitRows < fullRows),
            ),
            if (tier != null) Expanded(child: tier),
          ]);
        }),
      );
    }
    return Semantics(
      container: true,
      label: '${groupLabels[g]}调整面板',
      child: Column(key: ValueKey('panel-${g.name}'), children: [
        _Handle(ctl: ctl, group: g, collapsed: widget.collapsed),
        SizedBox(
          height: panelStripH(ts),
          child: _ChipStrip(ctl: ctl, group: g, rows: rows, active: active, rc: rc, collapsed: widget.collapsed),
        ),
        if (tier != null) Expanded(child: tier),
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
  const _RowChip({super.key, required this.ctl, required this.group, required this.row, required this.rc, required this.selected, required this.collapsed, this.width});

  /// Fixed width (the side pane's grid cell); null = sized to the content (the strip).
  final double? width;
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
    const nameOnly = false; // every chip shows its value, the selected one too (the big number above the ruler repeats it)
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
      alignment: width != null ? Alignment.center : null,
      constraints: width != null ? BoxConstraints(minWidth: width!, maxWidth: width!, minHeight: 48) : const BoxConstraints(minWidth: 64, maxWidth: 132, minHeight: 48),
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

// ------------------------------------------------------------------ side pane chip grid

/// The side pane's chips: two or three columns, wrapping; the grid scrolls vertically on its own when there are more
/// rows than its box holds (the control below is a separate region, so no gesture conflicts), and keeps the selected
/// chip in view.
class _ChipGrid extends StatefulWidget {
  const _ChipGrid({required this.ctl, required this.group, required this.rows, required this.active, required this.rc, required this.collapsed, required this.cols, this.scrolls = false});

  /// More rows than the box shows: fade the edges that have more behind them (the scroll cue).
  final bool scrolls;
  final EditorController ctl;
  final EGroup group;
  final List<ParamRow> rows;
  final ParamRow active;
  final RowContext rc;
  final bool collapsed;
  final int cols;

  @override
  State<_ChipGrid> createState() => _ChipGridState();
}

class _ChipGridState extends State<_ChipGrid> {
  final ScrollController _sc = ScrollController();
  final Map<String, GlobalKey> _keys = {};

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) => _reveal(false));
  }

  @override
  void didUpdateWidget(_ChipGrid old) {
    super.didUpdateWidget(old);
    if (old.active.id != widget.active.id) WidgetsBinding.instance.addPostFrameCallback((_) => _reveal(true));
  }

  void _reveal(bool animate) {
    final ctx = _keys[widget.active.id]?.currentContext;
    if (!mounted || ctx == null || !_sc.hasClients) return;
    Scrollable.ensureVisible(ctx, alignment: 0.5, duration: animate ? const Duration(milliseconds: 200) : Duration.zero);
  }

  @override
  void dispose() {
    _sc.dispose();
    super.dispose();
  }

  double _off = 0, _max = 0;

  @override
  Widget build(BuildContext context) {
    final ctl = widget.ctl;
    return LayoutBuilder(builder: (context, cons) {
      const gap = 6.0;
      final cellW = (cons.maxWidth - 16 - (widget.cols - 1) * gap) / widget.cols;
      final grid = SingleChildScrollView(
        key: ValueKey('chip-grid-${widget.group.name}'),
        controller: _sc,
        padding: const EdgeInsets.fromLTRB(8, 0, 8, 0),
        child: Wrap(spacing: gap, runSpacing: gap, children: [
          for (final r in widget.rows)
            KeyedSubtree(
              key: _keys.putIfAbsent(r.id, GlobalKey.new),
              child: _RowChip(
                key: ValueKey('chip-${widget.group.name}-${r.id}'),
                ctl: ctl,
                group: widget.group,
                row: r,
                rc: widget.rc,
                selected: r.id == widget.active.id,
                collapsed: widget.collapsed,
                width: cellW,
              ),
            ),
        ]),
      );
      // more rows than fit: the edges that have more behind them fade out (the scroll cue; no neighbouring row peeks in as a hairline)
      return NotificationListener<ScrollNotification>(
        onNotification: (n) {
          if (n.depth == 0 && (n.metrics.pixels != _off || n.metrics.maxScrollExtent != _max)) {
            WidgetsBinding.instance.addPostFrameCallback((_) {
              if (mounted) setState(() {});
            });
            _off = n.metrics.pixels;
            _max = n.metrics.maxScrollExtent;
          }
          return false;
        },
        child: ShaderMask(
          blendMode: BlendMode.dstIn,
          shaderCallback: (r) {
            final has = _sc.hasClients && _sc.position.hasContentDimensions;
            // (the same widget tree whether it scrolls or not: a controller must never sit in two scroll views)
            final up = widget.scrolls && has && _sc.position.pixels > 1;
            final down = widget.scrolls && (!has || _sc.position.pixels < _sc.position.maxScrollExtent - 1);
            const f = 14.0;
            return LinearGradient(
              begin: Alignment.topCenter,
              end: Alignment.bottomCenter,
              colors: [up ? const Color(0x00FFFFFF) : Colors.white, Colors.white, Colors.white, down ? const Color(0x00FFFFFF) : Colors.white],
              stops: [0, f / r.height, 1 - f / r.height, 1],
            ).createShader(r);
          },
          child: ClipRect(child: grid),
        ),
      );
    });
  }
}

/// Side pane header: the category's icon and name on the left, its reset in its own slot on the right.
class _SideHeader extends StatelessWidget {
  const _SideHeader({required this.ctl, required this.group});
  final EditorController ctl;
  final EGroup group;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final adjusted = ctl.edit.groupAdjusted(group);
    return SizedBox(
      height: kSideHeaderH,
      child: Padding(
        padding: const EdgeInsets.only(left: 16),
        child: Row(children: [
          DIcon(groupIcons[group]!, size: 18, color: c.text2),
          const SizedBox(width: 8),
          Expanded(child: Text(groupLabels[group]!, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.small(c.text2, w: FontWeight.w600))),
          IgnorePointer(
            ignoring: !adjusted,
            child: Visibility(
              visible: adjusted,
              maintainSize: true,
              maintainAnimation: true,
              maintainState: true,
              child: Semantics(
                button: true,
                label: '复位${groupLabels[group]}',
                excludeSemantics: true,
                child: GestureDetector(
                  key: ValueKey('reset-${group.name}'),
                  behavior: HitTestBehavior.opaque,
                  onTap: () => ctl.resetCategoryOf(group),
                  child: SizedBox(width: 48, height: kSideHeaderH, child: Center(child: DIcon(DI.reset, size: 18, color: c.text2))),
                ),
              ),
            ),
          ),
        ]),
      ),
    );
  }
}

// ------------------------------------------------------------------ photo-as-pad hint

/// Replaces a 2D pad when it would shrink the photo too much: a drag on the photo drives both axes; this slim row
/// names the axes and shows the values.
class _PadHint extends StatelessWidget {
  const _PadHint({required this.ctl, required this.row});
  final EditorController ctl;
  final ParamRow row;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Padding(
      key: ValueKey('pad-hint-${row.id}'),
      padding: const EdgeInsets.fromLTRB(16, 4, 8, 4),
      child: Row(children: [
        DIcon(DI.compare, size: 18, color: c.text2),
        const SizedBox(width: 10),
        if (row.padLabels.length == 4) Text(row.padLabels[0], style: T.cap(c.text2)),
        const SizedBox(width: 6),
        Expanded(
          child: Center(
            child: ListenableBuilder(
              listenable: Listenable.merge([ctl.readout, ctl.edits]),
              builder: (context, _) {
                final live = ctl.dragging.value && ctl.readout.name == row.label;
                return Text(live ? ctl.readout.value : row.text(ctl.ctx()), maxLines: 1, style: T.body(c.accent, w: FontWeight.w600, tabular: true));
              },
            ),
          ),
        ),
        const SizedBox(width: 6),
        if (row.padLabels.length == 4) Text(row.padLabels[1], style: T.cap(c.text2)),
        GestureDetector(
          key: const ValueKey('pad-hint-reset'),
          behavior: HitTestBehavior.opaque,
          onTap: () => ctl.resetRow(row),
          child: SizedBox(width: 44, height: 44, child: Center(child: DIcon(DI.reset, size: 18, color: c.text2))),
        ),
      ]),
    );
  }
}

// ------------------------------------------------------------------ lower tier

class _Tier extends StatelessWidget {
  const _Tier({required this.ctl, required this.group, required this.rows, required this.active, required this.rc, this.side = false, this.padOnPhoto = false});
  final bool side, padOnPhoto;
  final EditorController ctl;
  final EGroup group;
  final List<ParamRow> rows;
  final ParamRow active;
  final RowContext rc;

  @override
  Widget build(BuildContext context) {
    // One control per selected chip: the stock strip is the 胶片 chip's control (a picker with air around it), not a
    // second scroller stacked under the chip strip.
    return _lower(context, active);
  }

  Widget _lower(BuildContext context, ParamRow row) {
    switch (row.kind) {
      case RowKind.oneD:
        return RulerTier(ctl: ctl, row: row, group: group, histogram: !side);
      case RowKind.twoD:
        if (row.twoD == TwoD.tone) return _tone(context, row);
        if (padOnPhoto) return Center(child: _PadHint(ctl: ctl, row: row));
        return Padding(
          padding: const EdgeInsets.fromLTRB(12, 4, 12, 12),
          // a pane is narrow and tall: the pad is a square at the pane's width, never taller than there is room for
          child: SizedBox.expand(child: ControlPad(ctl: ctl, row: row)), // the pad takes the whole width and the height there is (not necessarily square)
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
      alignment: Alignment.center, // a picker sits in the middle of the room its category's tallest control sets
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 12),
      child: child,
    );
  }
}

// ------------------------------------------------------------------ ruler tier

/// Value line, reset and the full-width ruler of a 1D parameter. In a tall panel (a landscape photo
/// leaves room) the histogram is drawn wide under the ruler; otherwise a small one sits in the value line.
class RulerTier extends StatelessWidget {
  const RulerTier({super.key, required this.ctl, required this.row, required this.group, this.histogram = true});
  final EditorController ctl;
  final ParamRow row;
  final EGroup group;

  /// False in the side pane: the histogram sits above the chips there.
  final bool histogram;

  bool get _hist => histogram && (group == EGroup.light || group == EGroup.curve || group == EGroup.style);

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
          // (the value takes what is left between them: a narrow pane has no room to balance the two sides)
          if (_hist && !wide) Padding(padding: const EdgeInsets.only(left: 12), child: MiniHistogramBox(analysis: ctl.analysis())),
          Expanded(child: Center(child: valueText)),
          const SizedBox(width: 56),
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
