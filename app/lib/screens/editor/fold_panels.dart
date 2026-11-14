// The control surfaces of the fold layouts (docs/design/foldables.md): the histogram card, the workbench of the near-square window (the
// info half and the work half), the column beside a portrait photo, and the category grid. Everything inside is the phone panel's own
// pieces: the header (title, readout, 复位, 分项), the body (strength strip and pad, the film map, 分项, the complex editors, 裁切's chips and
// ruler, 局部's cards), the chips. Nothing is forked.
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../../editor/fold_layout.dart';
import '../../editor/window_manager.dart' show Hand;
import '../../model/params.dart';
import '../../theme/tokens.dart';
import 'editor_controller.dart';
import 'local_panel.dart' show LocalHeader;
import 'pad_dock.dart' show DockItem, padDockH, padDockOrder;
import 'pad_panel.dart';

/// Selects the category [g] in a fold layout: the controls are always open there, so a second tap on the selected one changes nothing.
/// 成像 opens on its pad like on the phone; the film map is a layer the stock chip opens.
void foldSelect(EditorController ctl, EGroup g) {
  if (ctl.wm.focused?.group != g) ctl.showCategory(g);
  foldSync(ctl);
}

/// A fold layout always has a category open (the first one when nothing is).
void foldSync(EditorController ctl) {
  if (ctl.wm.focused == null) ctl.showCategory(EGroup.light);
}

/// Lines 局部's header holds under the title row at [width] (the area chips, the 光线 / 颜色 / 质地 switch): the info card has to make room.
int foldHeaderLines(EditorController ctl, double width, double textScale) =>
    ctl.wm.focused?.group == EGroup.local ? LocalHeader.belowLinesFor(ctl.local, width, textScale) : 0;

BoxDecoration foldCard(DC c) => BoxDecoration(color: c.surface, borderRadius: BorderRadius.circular(Rd.panel), border: Border.all(color: c.sep), boxShadow: Fx.shadowPanel);

/// The histogram of the rendered photo: a soft filled area on the panel's own ground.
class HistogramCard extends StatelessWidget {
  const HistogramCard({super.key, required this.ctl});
  final EditorController ctl;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      label: '直方图',
      image: true,
      child: ListenableBuilder(
        listenable: Listenable.merge([ctl.app.editTick, ctl.engine]),
        builder: (context, _) {
          final a = ctl.analysis();
          return DecoratedBox(
            key: const ValueKey('histogram'),
            decoration: BoxDecoration(color: c.bg, borderRadius: BorderRadius.circular(Rd.block)),
            child: ClipRRect(
              borderRadius: BorderRadius.circular(Rd.block),
              child: CustomPaint(painter: _HistPainter(a.hist, c.histFill, c.sep), child: const SizedBox.expand()),
            ),
          );
        },
      ),
    );
  }
}

/// The height the histogram is scaled to: the tallest bin that is not an end bin, so a pile-up at the black or white end (clipping) does not
/// flatten the rest into a line. The end bins that exceed it are drawn cut off at the top. All mass at the ends: the tallest bin.
double histogramCeiling(List<double> h) {
  if (h.length < 3) return h.fold<double>(0, math.max);
  final inner = h.sublist(1, h.length - 1).fold<double>(0, math.max);
  final all = h.fold<double>(0, math.max);
  return inner > all * 0.02 ? inner : all;
}

class _HistPainter extends CustomPainter {
  const _HistPainter(this.hist, this.fill, this.grid);
  final List<double> hist;
  final Color fill, grid;

  @override
  void paint(Canvas canvas, Size size) {
    final g = Paint()..color = grid..strokeWidth = 1;
    for (var i = 1; i < 4; i++) {
      canvas.drawLine(Offset(size.width * i / 4, 0), Offset(size.width * i / 4, size.height), g);
    }
    final mx = histogramCeiling(hist);
    if (mx <= 0 || hist.length < 2) return;
    final area = Path()..moveTo(0, size.height);
    for (var i = 0; i < hist.length; i++) {
      area.lineTo(i / (hist.length - 1) * size.width, size.height - math.min(1.0, hist[i] / mx) * size.height * 0.92);
    }
    area
      ..lineTo(size.width, size.height)
      ..close();
    canvas.drawPath(area, Paint()..color = fill);
  }

  @override
  bool shouldRepaint(_HistPainter o) => o.hist != hist || o.fill != fill || o.grid != grid;
}

/// The six categories as the dock's own items (glyph over word, [DockItem]) in a grid of [perRow], for the column. The dock's items, so
/// the glyphs show the state of each category and a tap behaves as on the phone; only the press-and-hold popup is left out (it has no
/// place to rise from here: the pad is on the card).
class FoldCatGrid extends StatelessWidget {
  const FoldCatGrid({super.key, required this.ctl, this.perRow = 3});
  final EditorController ctl;
  final int perRow;

  @override
  Widget build(BuildContext context) {
    final dh = padDockH(MediaQuery.textScalerOf(context).scale(15) / 15);
    return ListenableBuilder(
      listenable: Listenable.merge([ctl.wm, ctl.app.editTick, ctl.local]),
      builder: (context, _) {
        final rows = <Widget>[];
        for (var r = 0; r < padDockOrder.length; r += perRow) {
          rows.add(SizedBox(
            height: dh,
            child: Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
              for (var i = r; i < math.min(r + perRow, padDockOrder.length); i++)
                Expanded(child: KeyedSubtree(key: ValueKey('fold-cat-${padDockOrder[i].name}'), child: DockItem(ctl: ctl, group: padDockOrder[i], holdToAdjust: false))),
            ]),
          ));
        }
        return Column(mainAxisSize: MainAxisSize.min, children: rows);
      },
    );
  }
}

/// Histogram card above the open category's header: the left half of the workbench and the top of the column. With little height the
/// histogram gives way and the header (which never shrinks) stays.
class FoldInfo extends StatelessWidget {
  const FoldInfo({super.key, required this.ctl, required this.textScale, this.linesWidth, this.headerOnly = false});
  final EditorController ctl;
  final double textScale;

  /// The width 局部's header lines are laid out for (null: the card's own).
  final double? linesWidth;

  /// Only the title row and what hangs under it, no histogram (the column, when 局部's lines leave no room for one).
  final bool headerOnly;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: Listenable.merge([ctl.wm, ctl.edits, ctl.editingRow]),
      builder: (context, _) {
        final g = ctl.wm.focused?.group ?? EGroup.light;
        if (linesWidth != null) ctl.local.headerInnerW = linesWidth;
        final header = PanelHeaderOf(ctl: ctl, group: g, side: true, textScale: textScale);
        return LayoutBuilder(builder: (context, cons) {
          final lines = g == EGroup.local ? LocalHeader.belowLinesFor(ctl.local, ctl.local.headerInnerW ?? cons.maxWidth - 2 * Sp.l, textScale) : 0;
          final headerH = PanelHeader.heightOf(textScale: textScale, side: true, subTappable: true, lines: lines);
          final room = cons.maxHeight - headerH - Sp.s;
          if (headerOnly) return Padding(padding: const EdgeInsets.only(top: Sp.s), child: header);
          if (room - Sp.s < 56) return SingleChildScrollView(physics: const ClampingScrollPhysics(), child: Padding(padding: const EdgeInsets.only(top: Sp.s), child: header));
          return Column(children: [
            Expanded(child: Padding(padding: const EdgeInsets.fromLTRB(Sp.l, Sp.m, Sp.l, 0), child: HistogramCard(ctl: ctl))),
            const SizedBox(height: Sp.xs),
            header,
            const SizedBox(height: Sp.s),
          ]);
        });
      },
    );
  }
}

/// The strength strip and the pad (or the film map, 分项, 裁切's chips, 局部's cards) of the open category: the right half of the workbench.
class FoldWork extends StatelessWidget {
  const FoldWork({super.key, required this.ctl});
  final EditorController ctl;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: Listenable.merge([ctl.wm, ctl.edits, ctl.editingRow]),
      builder: (context, _) => PanelBodyOf(ctl: ctl, group: ctl.wm.focused?.group ?? EGroup.light, side: false),
    );
  }
}

/// The workbench under a landscape photo: the info half and the work half, the fold (or the plain gap) between them where no control sits.
/// [plan] holds the two rectangles in window coordinates; this widget is laid out over the whole window.
class FoldBench extends StatelessWidget {
  const FoldBench({super.key, required this.ctl, required this.plan, required this.textScale});
  final EditorController ctl;
  final FoldPlan plan;
  final double textScale;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    Widget card(Key k, Rect r, Widget child) => Positioned.fromRect(
          rect: r,
          child: Container(key: k, decoration: foldCard(c), clipBehavior: Clip.antiAlias, child: child),
        );
    return ListenableBuilder(
      listenable: ctl.wm,
      builder: (context, _) {
        // both cards keep the full height of the bench in every category (a card shrunk to its title row would leave a blank strip under it)
        final info = plan.info;
        return Stack(children: [
          card(const ValueKey('fold-info'), info, FoldInfo(ctl: ctl, textScale: textScale, linesWidth: plan.info.width - 2 * Sp.l)),
          card(const ValueKey('fold-work'), plan.work, FoldWork(ctl: ctl)),
        ]);
      },
    );
  }
}

/// The column beside a portrait photo: histogram and title row on top, the six categories, then the strength strip and the pad (swapping
/// with 分项 in the same place), and a way to move the column to the other side. A horizontal fold across the column splits it in two
/// blocks with nothing in the band between.
class FoldColumn extends StatelessWidget {
  const FoldColumn({super.key, required this.ctl, required this.plan, required this.textScale, required this.hand, required this.onSwap});
  final EditorController ctl;
  final FoldPlan plan;
  final double textScale;
  final Hand hand;
  final VoidCallback onSwap;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final r = plan.controls;
    // the same gesture as the cluster's grip: drag the column's top handle toward the other side to move it there
    Widget withHandle(Widget child) {
      var dx = 0.0;
      return Stack(children: [
        child,
        Positioned(
          top: 0,
          left: 0,
          right: 0,
          height: 48,
          child: Semantics(
            label: '拖到另一边',
            child: GestureDetector(
              key: const ValueKey('fold-swap'),
              behavior: HitTestBehavior.translucent,
              onHorizontalDragStart: (_) => dx = 0,
              onHorizontalDragUpdate: (d) => dx += d.delta.dx,
              onHorizontalDragEnd: (_) {
                if (hand == Hand.right ? dx < -48 : dx > 48) onSwap();
              },
              child: Align(
                alignment: Alignment.topCenter,
                child: Padding(padding: const EdgeInsets.only(top: 6), child: Container(width: 36, height: 4, decoration: BoxDecoration(color: c.sep, borderRadius: BorderRadius.circular(2)))),
              ),
            ),
          ),
        ),
      ]);
    }

    final cats = Padding(padding: const EdgeInsets.fromLTRB(Sp.l - 2, 0, Sp.l - 2, Sp.xs), child: FoldCatGrid(ctl: ctl));
    final catsH = 2 * padDockH(textScale) + Sp.xs;
    // the handle (drag to the other side) has its own band above the histogram
    final histogram = Padding(padding: const EdgeInsets.fromLTRB(Sp.l, 16, Sp.l, Sp.xs), child: HistogramCard(ctl: ctl));
    // The hierarchy reads top to bottom: the histogram, the six categories (level 1), then the open category's own header (title, readout,
    // 复位, 分项, and 局部's area chips with the 光线 / 颜色 / 质地 switch, or 成像's stock chip), then the strength strip and the pad (or what
    // swaps with it). The header has its natural height: the readout keeps its own line.
    Widget header({bool hist = false}) => ListenableBuilder(
          listenable: Listenable.merge([ctl.wm, ctl.edits, ctl.editingRow, ctl.local]),
          builder: (context, _) {
            final g = ctl.wm.focused?.group ?? EGroup.light;
            ctl.local.headerInnerW = r.width - 2 * Sp.l;
            return Padding(padding: const EdgeInsets.only(top: Sp.xs), child: PanelHeaderOf(ctl: ctl, group: g, side: true, textScale: textScale));
          },
        );
    // the histogram gives way when the rest leaves it too little (its height is a share of what the fixed rows leave)
    Widget upperRows(double height, {required bool withHeader}) => ListenableBuilder(
          listenable: Listenable.merge([ctl.wm, ctl.edits, ctl.editingRow, ctl.local]),
          builder: (context, _) {
            final lines = foldHeaderLines(ctl, r.width - 2 * Sp.l, textScale);
            final headerMax = PanelHeader.heightOf(textScale: textScale, side: true, subTappable: true, lines: lines) + Sp.xs;
            final room = height - catsH - (withHeader ? headerMax : 0) - (withHeader ? 220 : 0);
            return Column(children: [
              if (room >= 56) Expanded(flex: 2, child: histogram) else const SizedBox(height: 16),
              cats,
              if (withHeader) header(),
              if (withHeader) Expanded(flex: room >= 56 ? 5 : 1, child: FoldWork(ctl: ctl)),
            ]);
          },
        );
    final split = plan.split;
    if (split == null) {
      return Positioned.fromRect(
        rect: r,
        child: Container(
          key: const ValueKey('fold-column'),
          decoration: foldCard(c),
          clipBehavior: Clip.antiAlias,
          child: withHandle(upperRows(r.height, withHeader: true)),
        ),
      );
    }
    final topH = (split.$1 - r.top).clamp(0.0, r.height).toDouble();
    final lowTop = split.$2;
    return Stack(children: [
      Positioned(left: r.left, top: r.top, width: r.width, height: topH, child: Container(key: const ValueKey('fold-column'), decoration: foldCard(c), clipBehavior: Clip.antiAlias, child: withHandle(upperRows(topH, withHeader: false)))),
      Positioned(
        left: r.left,
        top: lowTop,
        width: r.width,
        height: math.max(0.0, r.bottom - lowTop),
        child: Container(key: const ValueKey('fold-column-low'), decoration: foldCard(c), clipBehavior: Clip.antiAlias, child: Column(children: [header(), Expanded(child: FoldWork(ctl: ctl))])),
      ),
    ]);
  }
}
