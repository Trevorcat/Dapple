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
import 'pad_dock.dart' show dockLabel, padDockOrder;
import 'pad_panel.dart';

/// Selects the category [g] in a fold layout: the controls are always open there, so a second tap on the selected one changes nothing.
/// 成像 shows its map as the home layer.
void foldSelect(EditorController ctl, EGroup g) {
  if (ctl.wm.focused?.group != g) ctl.showCategory(g);
  foldSync(ctl);
}

/// A fold layout always has a category open (the first one when nothing is), and 成像 shows its map unless 分项 or an editor is open.
void foldSync(EditorController ctl) {
  if (ctl.wm.focused == null) ctl.showCategory(EGroup.light);
  final film = ctl.wm.focused?.group == EGroup.style;
  if (film && !ctl.itemised && ctl.extra == null && !ctl.filmMapOpen) ctl.setFilmMap(true);
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
    final mx = hist.fold<double>(0, math.max);
    if (mx <= 0 || hist.length < 2) return;
    final area = Path()..moveTo(0, size.height);
    for (var i = 0; i < hist.length; i++) {
      area.lineTo(i / (hist.length - 1) * size.width, size.height - hist[i] / mx * size.height * 0.92);
    }
    area
      ..lineTo(size.width, size.height)
      ..close();
    canvas.drawPath(area, Paint()..color = fill);
  }

  @override
  bool shouldRepaint(_HistPainter o) => o.hist != hist || o.fill != fill || o.grid != grid;
}

/// The six categories as chips in a grid of [perRow] (the column), the same chips as the rail's.
class FoldCatGrid extends StatelessWidget {
  const FoldCatGrid({super.key, required this.ctl, this.perRow = 3});
  final EditorController ctl;
  final int perRow;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: Listenable.merge([ctl.wm, ctl.edits]),
      builder: (context, _) {
        final cur = ctl.wm.focused?.group;
        final rows = <Widget>[];
        for (var r = 0; r < padDockOrder.length; r += perRow) {
          rows.add(Row(children: [
            for (var i = r; i < math.min(r + perRow, padDockOrder.length); i++) ...[
              if (i > r) const SizedBox(width: Sp.s),
              Expanded(child: PanelChip(key: ValueKey('fold-cat-${padDockOrder[i].name}'), label: dockLabel(padDockOrder[i]), selected: cur == padDockOrder[i], compact: true, onTap: () => foldSelect(ctl, padDockOrder[i]))),
            ],
          ]));
        }
        return Column(mainAxisSize: MainAxisSize.min, children: rows);
      },
    );
  }
}

/// Histogram card above the open category's header: the left half of the workbench and the top of the column. With little height the
/// histogram gives way and the header (which never shrinks) stays.
class FoldInfo extends StatelessWidget {
  const FoldInfo({super.key, required this.ctl, required this.textScale, this.linesWidth});
  final EditorController ctl;
  final double textScale;

  /// The width 局部's header lines are laid out for (null: the card's own).
  final double? linesWidth;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: Listenable.merge([ctl.wm, ctl.edits, ctl.editingRow]),
      builder: (context, _) {
        final g = ctl.wm.focused?.group ?? EGroup.light;
        if (linesWidth != null) ctl.local.headerInnerW = linesWidth;
        final header = PanelHeaderOf(ctl: ctl, group: g, side: true, textScale: textScale, filmHome: true);
        return LayoutBuilder(builder: (context, cons) {
          final lines = g == EGroup.local ? LocalHeader.belowLinesFor(ctl.local, ctl.local.headerInnerW ?? cons.maxWidth - 2 * Sp.l, textScale) : 0;
          final headerH = PanelHeader.heightOf(textScale: textScale, side: true, subTappable: true, lines: lines);
          final room = cons.maxHeight - headerH - Sp.s;
          if (room - Sp.s < 56) return SingleChildScrollView(physics: const ClampingScrollPhysics(), child: header);
          return Column(children: [
            Expanded(child: Padding(padding: const EdgeInsets.fromLTRB(Sp.m, Sp.m, Sp.m, 0), child: HistogramCard(ctl: ctl))),
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
        // 裁切 has nothing for a histogram to show: its half shrinks to the title row and the space is left to the photo's ground
        var info = plan.info;
        if (ctl.wm.focused?.group == EGroup.crop) {
          final hh = math.max(56.0, 0.0) * math.max(1.0, textScale);
          info = Rect.fromLTWH(info.left, info.top, info.width, math.min(info.height, hh));
        }
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

    final cats = Padding(padding: const EdgeInsets.fromLTRB(Sp.m, 0, Sp.m, Sp.xs), child: FoldCatGrid(ctl: ctl));
    final info = FoldInfo(ctl: ctl, textScale: textScale, linesWidth: r.width - 2 * Sp.l);
    final work = Expanded(flex: 5, child: FoldWork(ctl: ctl));
    final split = plan.split;
    if (split == null) {
      return Positioned.fromRect(
        rect: r,
        child: Container(
          key: const ValueKey('fold-column'),
          decoration: foldCard(c),
          clipBehavior: Clip.antiAlias,
          child: withHandle(Column(children: [Expanded(flex: 2, child: info), cats, work])),
        ),
      );
    }
    final topH = (split.$1 - r.top).clamp(0.0, r.height).toDouble();
    final lowTop = split.$2;
    return Stack(children: [
      Positioned(left: r.left, top: r.top, width: r.width, height: topH, child: Container(key: const ValueKey('fold-column'), decoration: foldCard(c), clipBehavior: Clip.antiAlias, child: withHandle(info))),
      Positioned(
        left: r.left,
        top: lowTop,
        width: r.width,
        height: math.max(0.0, r.bottom - lowTop),
        child: Container(key: const ValueKey('fold-column-low'), decoration: foldCard(c), clipBehavior: Clip.antiAlias, child: Column(children: [const SizedBox(height: Sp.s), cats, work])),
      ),
    ]);
  }
}
