// The 局部 panel (docs/design/local-adjustments.md sections 3, 4, 5 and 7): the header with the area chips, the body (the strip and
// the two pads, the itemised view, the shape card, the area's actions) and the tool strip (a reserved line under the photo, never over it) for
// the "where" of brushes and ranges. No scrolling lists, no second-level menus: a card replaces the body in place and closes itself.
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import '../../model/local_adjust.dart';
import '../../model/local_kinds.dart';
import '../../model/local_pad.dart';
import '../../model/pad_model.dart';
import '../../model/portrait.dart';
import '../../services/local_ai.dart' show AiPlane;
import '../../theme/tokens.dart';
import '../../widgets/controls.dart' show HitBox, kHit;
import '../../widgets/icons.dart';
import '../../widgets/tile_grid.dart';
import 'editor_controller.dart';
import 'itemised_view.dart';
import 'local_controller.dart';
import 'local_glyphs.dart';
import 'pad_gestures.dart';
import 'pad_panel.dart' show HeaderButton, HeaderReadout, PanelChip, PanelHeader;
import 'pad_widgets.dart';

// ------------------------------------------------------------------ header

/// Area chips: a 40 dp tile in a 48 dp touch box, laid side by side without gaps (the boxes touch, the tiles keep 8 dp apart).
const double kAreaChip = 40;

class LocalHeader extends StatelessWidget {
  const LocalHeader({super.key, required this.ctl, required this.side, required this.textScale, this.collapsed = false});
  final EditorController ctl;
  final bool side;
  final double textScale;

  /// Folded to the handle and the header: the chips line is left out.
  final bool collapsed;

  /// Number of chips (the [+] and one per shown area).
  static int chipCount(LocalController lc) {
    var n = 1, portrait = false;
    for (final a in lc.list.items) {
      if (portraitOf(a) != null) {
        if (portrait) continue;
        portrait = true;
      }
      n++;
    }
    return n;
  }

  /// Lines the area chips take at [width] (48 dp targets, wrapped; never shrunk).
  static int chipLines(LocalController lc, double width) {
    final perLine = math.max(1, (width / kHit).floor());
    return (chipCount(lc) / perLine).ceil();
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final lc = ctl.local;
    final a = lc.sel;
    // U6: the title is always the dock item's name; what the layer is shows as the subtitle; ‹ goes back one layer
    final emptyCard = lc.list.isEmpty && lc.body != LocalBody.aiParts && lc.body != LocalBody.portrait;
    final inCard = !emptyCard && lc.body != LocalBody.pads && lc.body != LocalBody.portrait;
    final parts = lc.body == LocalBody.aiParts && lc.personFound && lc.aiRunning == 0;
    final portraitPanel = lc.body == LocalBody.portrait || (lc.body == LocalBody.pads && lc.selIsPortrait);
    Text subText(String s) => Text(s, key: const ValueKey('header-sub'), maxLines: 1, overflow: TextOverflow.ellipsis, style: T.cap(c.text2));
    Widget? sub;
    VoidCallback? back;
    final buttons = <Widget>[];
    final reset = ListenableBuilder(
      listenable: ctl.app.editTick,
      builder: (context, _) {
        final s = lc.sel;
        return Visibility(
          visible: s != null && localOpsAdjusted(s),
          maintainSize: true,
          maintainAnimation: true,
          maintainState: true,
          child: HeaderButton(key: const ValueKey('reset-local'), label: '复位', icon: DI.reset, semantics: '复位这个区域的调整', onTap: lc.resetOps),
        );
      },
    );
    final readout = HeaderReadout(
      listenable: Listenable.merge([ctl.app.editTick, lc]),
      text: () {
        final s = lc.sel;
        if (s == null) return '';
        final r = localReadout(localUiOf(s), area: s);
        return portraitOf(s) != null ? '人像 · $r' : r;
      },
    );
    if (emptyCard) {
      sub = subText('新区域');
    } else if (inCard) {
      back = () {
        if (lc.body == LocalBody.aiParts) lc.cancelAnalysis();
        lc.showBody(LocalBody.pads);
      };
      sub = subText(switch (lc.body) {
        LocalBody.addArea => '新区域',
        LocalBody.addPart => '加一块',
        LocalBody.subPart => '减一块',
        LocalBody.aiParts => '人物',
        LocalBody.actions => lc.actionsTitle,
        _ => '',
      });
      // the parts card's main action: the one button at the right end
      if (parts) {
        buttons.add(
          Opacity(
            opacity: lc.partPick.isEmpty ? 0.38 : 1,
            child: HeaderButton(
              key: const ValueKey('parts-add'),
              label: lc.partMode == LocalBody.subPart ? '减去' : '加上',
              pressed: lc.partPick.isNotEmpty,
              onTap: lc.addPickedParts,
            ),
          ),
        );
      }
    } else if (ctl.itemised) {
      back = () => ctl.setItemised(false);
      sub = readout;
      if (a != null) buttons.add(reset);
    } else {
      sub = portraitPanel && a == null ? subText('人像') : readout;
      if (a != null) {
        buttons
          ..add(reset)
          ..add(HeaderButton(key: const ValueKey('items-button'), label: '分项', onTap: () => ctl.setItemised(true)));
      }
    }
    return LayoutBuilder(
      builder: (context, cons) {
        final lines = collapsed ? 0 : chipLines(lc, cons.maxWidth - 2 * Sp.l);
        return PanelHeader(
          title: '局部',
          side: side,
          textScale: textScale,
          onBack: back,
          sub: sub,
          buttons: buttons,
          // the area chips: always on a line of their own (the same place in every layer and placement)
          below: lines > 0 ? LocalChips(lc: lc) : null,
          belowLines: lines,
        );
      },
    );
  }
}

/// [+] and one chip per area: the kind's glyph, a small "+" when the area has several parts, a half-filled corner when it is
/// inverted. Tap selects; a second tap on the selected chip shows its actions; holding a chip shows its mask.
class LocalChips extends StatelessWidget {
  const LocalChips({super.key, required this.lc});
  final LocalController lc;

  @override
  Widget build(BuildContext context) {
    final items = lc.list.items;
    // the portrait areas are one chip (人像): their chips live in the portrait panel
    final shown = <int>[];
    var portraitShown = false;
    for (var i = 0; i < items.length; i++) {
      if (portraitOf(items[i]) != null) {
        if (portraitShown) continue;
        portraitShown = true;
      }
      shown.add(i);
    }
    // every chip keeps its 48 dp target; a line that is full wraps (the header made room for the lines)
    return Align(
      alignment: Alignment.centerLeft,
      child: Wrap(
        children: [
          _AddChip(lc: lc, size: kAreaChip),
          for (final i in shown) _AreaChip(lc: lc, index: i, size: kAreaChip),
        ],
      ),
    );
  }
}

class _AddChip extends StatelessWidget {
  const _AddChip({required this.lc, required this.size});
  final LocalController lc;
  final double size;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final open = lc.body == LocalBody.addArea || lc.list.isEmpty;
    final full = lc.list.length >= kMaxLocalAdjustments;
    void tap() {
      if (full) {
        lc.ctl.app.showToast('最多 $kMaxLocalAdjustments 个区域');
        return;
      }
      lc.showBody(lc.body == LocalBody.addArea && !lc.list.isEmpty ? LocalBody.pads : LocalBody.addArea);
    }

    return Semantics(
      button: true,
      label: '新区域…',
      excludeSemantics: true,
      onTap: tap,
      child: GestureDetector(
        key: const ValueKey('local-add'),
        behavior: HitTestBehavior.opaque,
        onTap: tap,
        child: HitBox(
          child: Opacity(
            opacity: full ? 0.38 : 1,
            child: Container(
              width: size,
              height: size,
              alignment: Alignment.center,
              decoration: BoxDecoration(
                color: open ? c.accentSoft : c.chip,
                borderRadius: BorderRadius.circular(Rd.tile),
                border: open ? Border.all(color: c.accent, width: 1.5) : null,
              ),
              child: DIcon(DI.plus, size: size * 0.55, color: open ? c.accent : c.text),
            ),
          ),
        ),
      ),
    );
  }
}

class _AreaChip extends StatelessWidget {
  const _AreaChip({required this.lc, required this.index, required this.size});
  final LocalController lc;
  final int index;
  final double size;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final a = lc.list.items[index];
    final portrait = portraitOf(a) != null;
    final selected = a.id == lc.selId || (portrait && lc.selIsPortrait);
    final glyph = portraitOf(a) != null ? LocalGlyph.portrait : (a.mask.isEmpty ? LocalGlyph.raster : kindOfComponent(a.mask.first).glyph);
    final fg = selected ? c.accent : c.text;
    final label = [
      portrait ? '人像' : '区域 ${index + 1}',
      if (!portrait && a.mask.isNotEmpty) kindOfComponent(a.mask.first).label,
      if (!portrait && a.mask.length > 1) '${a.mask.length} 块',
      if (a.invert) '反选',
      if (!a.on) '已关闭',
    ].join('，');
    return Semantics(
      button: true,
      selected: selected,
      label: label,
      hint: selected ? '再点一次显示操作，按住查看范围' : null,
      excludeSemantics: true,
      onTap: () => lc.select(portrait && lc.selIsPortrait ? lc.selId! : a.id),
      child: GestureDetector(
        key: ValueKey('local-chip-$index'),
        behavior: HitTestBehavior.opaque,
        onTap: () => lc.select(portrait && lc.selIsPortrait ? lc.selId! : a.id),
        onLongPressStart: (_) {
          if (!selected) lc.select(a.id);
          lc.setPeek(true);
        },
        onLongPressEnd: (_) => lc.setPeek(false),
        onLongPressCancel: () => lc.setPeek(false),
        child: HitBox(
          child: Opacity(
            opacity: a.on ? 1 : 0.4,
            child: Container(
              width: size,
              height: size,
              decoration: BoxDecoration(
                color: selected ? c.accentSoft : c.chip,
                borderRadius: BorderRadius.circular(Rd.tile),
                border: selected ? Border.all(color: c.accent, width: 1.5) : null,
              ),
              child: Stack(
                children: [
                  Center(
                    child: LocalGlyphIcon(glyph, size: size * 0.62, color: fg),
                  ),
                  // the selected chip shows a small ⌄: a second tap opens its actions (audit U3)
                  if (selected)
                    Positioned(
                      right: 2,
                      bottom: 2,
                      child: DIcon(DI.down, key: const ValueKey('chip-actions-cue'), size: 10, color: fg, stroke: 2.6),
                    ),
                  if (a.mask.length > 1)
                    Positioned(
                      right: 1.5,
                      top: 1,
                      child: Text(
                        '+',
                        style: TextStyle(fontSize: size * 0.32, height: 1, fontWeight: FontWeight.w700, color: fg),
                      ),
                    ),
                  if (a.invert)
                    Positioned(
                      left: 3,
                      bottom: 3,
                      child: Container(
                        width: size * 0.22,
                        height: size * 0.22,
                        decoration: BoxDecoration(
                          shape: BoxShape.circle,
                          border: Border.all(color: fg, width: 1),
                          gradient: LinearGradient(colors: [fg, fg, fg.withValues(alpha: 0), fg.withValues(alpha: 0)], stops: const [0, 0.5, 0.5, 1]),
                        ),
                      ),
                    ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}

// ------------------------------------------------------------------ body

class LocalBodyView extends StatelessWidget {
  const LocalBodyView({super.key, required this.ctl, required this.side});
  final EditorController ctl;
  final bool side;

  @override
  Widget build(BuildContext context) {
    final lc = ctl.local;
    // the empty state IS the first step: the shape card
    if (lc.body == LocalBody.aiParts) return AiPartsCard(lc: lc);
    if (lc.body == LocalBody.portrait && !ctl.itemised) return PortraitPanel(ctl: ctl);
    if (lc.list.isEmpty || lc.body == LocalBody.addArea) return ShapeCard(lc: lc, mode: LocalBody.addArea);
    switch (lc.body) {
      case LocalBody.addPart:
      case LocalBody.subPart:
        return ShapeCard(lc: lc, mode: lc.body);
      case LocalBody.actions:
        return AreaActions(lc: lc);
      case LocalBody.aiParts:
        return AiPartsCard(lc: lc);
      case LocalBody.portrait:
      case LocalBody.pads:
      case LocalBody.addArea:
        break;
    }
    if (ctl.itemised) return LocalItemised(ctl: ctl);
    if (lc.selIsPortrait) return PortraitPanel(ctl: ctl);
    final strip = StrengthStrip(ctl: ctl, cat: PadCat.light, source: lc.lightPad, label: '强度');
    final light = PadFace(key: const ValueKey('local-pad-light'), ctl: ctl, cat: PadCat.light, source: lc.lightPad);
    final color = PadFace(key: const ValueKey('local-pad-color'), ctl: ctl, cat: PadCat.color, source: lc.colorPad);
    return Row(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        strip,
        const SizedBox(width: Sp.m),
        // the two pads side by side or stacked, whichever keeps them closer to square (a squat pad crowds its edge icons)
        Expanded(
          child: LayoutBuilder(
            builder: (context, cons) {
              double off(double a) => (math.log(a)).abs();
              final row = off(((cons.maxWidth - 8) / 2) / cons.maxHeight), col = off(cons.maxWidth / ((cons.maxHeight - 8) / 2));
              return col < row
                  ? Column(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: [
                        Expanded(child: light),
                        const SizedBox(height: 8),
                        Expanded(child: color),
                      ],
                    )
                  : Row(
                      crossAxisAlignment: CrossAxisAlignment.stretch,
                      children: [
                        Expanded(child: light),
                        const SizedBox(width: 8),
                        Expanded(child: color),
                      ],
                    );
            },
          ),
        ),
      ],
    );
  }
}

/// The kinds as large glyph tiles with one word each (not a text list), from the registry: the shapes on the first row, the AI kinds the
/// device can do (and the analysis did not rule out) on the second. An AI tile shows a determinate ring while its analysis runs; a second
/// tap on it cancels.
class ShapeCard extends StatelessWidget {
  const ShapeCard({super.key, required this.lc, required this.mode});
  final LocalController lc;
  final LocalBody mode;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final kinds = lc.offered;
    final rows = <List<LocalKind>>[
      [
        for (final k in kinds)
          if (!k.ai) k,
      ],
      [
        for (final k in kinds)
          if (k.ai) k,
      ],
    ]..removeWhere((r) => r.isEmpty);
    return LayoutBuilder(
      builder: (context, cons) {
        // tiles never go under 56 dp (U7): a row that does not fit wraps, and the tiles get lower so every line stays in the card
        final fitN = TileGrid.fit(cons.maxWidth);
        final perRow = math.min(fitN, rows.fold<int>(1, (n, r) => math.max(n, r.length)));
        final lines = rows.fold<int>(0, (n, r) => n + (r.length / perRow).ceil());
        final tileW = TileGrid.tileWidth(cons.maxWidth, perRow);
        final tileH = math.max(56.0, math.min(84.0, (cons.maxHeight - TileGrid.gap * (lines - 1)) / lines));
        // the shared tile (audit U7): the kind's glyph, its word, the note (粗略); a running analysis draws its ring on the tile
        Widget tile(LocalKind k) {
          final running = lc.runningFor(k);
          return DTile(
            key: ValueKey('kind-${k.id}'),
            label: k.label,
            note: k.note,
            width: tileW,
            height: tileH,
            semantics: '${k.label}${k.note == null ? '' : '（${k.note}）'}${running ? '，分析中，再点一次取消' : ''}',
            iconWidget: (color, size) => LocalGlyphIcon(k.glyph, size: size, color: color),
            iconOpacity: running ? 0.35 : 1,
            overlay: running
                ? SizedBox(
                    key: ValueKey('kind-progress-${k.id}'),
                    width: 26,
                    height: 26,
                    child: CircularProgressIndicator(
                      value: lc.aiProgress <= 0 ? null : lc.aiProgress,
                      strokeWidth: 2.5,
                      color: c.accent,
                      backgroundColor: c.track,
                    ),
                  )
                : null,
            onTap: () => lc.chooseKind(k, mode),
          );
        }

        final grid = Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            for (var i = 0; i < rows.length; i++) ...[
              if (i > 0) const SizedBox(height: TileGrid.gap),
              TileGrid(children: [for (final k in rows[i]) tile(k)]),
            ],
          ],
        );
        // a short side pane (landscape phone) that cannot hold every line of 56 dp tiles: the card scrolls rather than shrinking them
        final fits = lines * 56.0 + TileGrid.gap * (lines - 1) <= cons.maxHeight;
        return fits ? Center(child: grid) : SingleChildScrollView(child: Center(child: grid));
      },
    );
  }
}

// 人物: the parts the analysis found, as small chips to pick one or several; one button adds them as one area. While the analysis runs: a
/// determinate ring and 取消; no person: a plain sentence and nothing to pick.
class AiPartsCard extends StatelessWidget {
  const AiPartsCard({super.key, required this.lc});
  final LocalController lc;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    if (lc.aiRunning != 0 && (lc.aiDone & AiBits.person) == 0) {
      return Center(
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            SizedBox(
              key: const ValueKey('parts-progress'),
              width: 28,
              height: 28,
              child: CircularProgressIndicator(value: lc.aiProgress <= 0 ? null : lc.aiProgress, strokeWidth: 3, color: c.accent, backgroundColor: c.track),
            ),
            const SizedBox(width: Sp.m),
            Text('正在找人', style: T.body(c.text2)),
            const SizedBox(width: Sp.s),
            HeaderButton(
              key: const ValueKey('parts-cancel'),
              label: '取消',
              onTap: () {
                lc.cancelAnalysis();
                lc.showBody(lc.list.isEmpty ? LocalBody.addArea : LocalBody.pads);
              },
            ),
          ],
        ),
      );
    }
    if ((lc.aiDone & AiBits.person) != 0 && !lc.personFound) {
      return Center(
        child: Text('没找到人', key: const ValueKey('parts-none'), style: T.body(c.text2)),
      );
    }
    final planes = lc.partPlanes;
    final people = [
      for (final p in planes)
        if (p.face == 0) p,
    ];
    final byFace = <int, List<AiPlane>>{};
    for (final p in planes) {
      if (p.face > 0) byFace.putIfAbsent(p.face, () => []).add(p);
    }
    Widget chip(AiPlane p) {
      final on = lc.partPick.contains(p.key);
      // a chip as wide as its word (PanelChip fills a loose width)
      return IntrinsicWidth(
        child: PanelChip(key: ValueKey('part-${p.key}'), label: partLabel(p.key) ?? p.label, selected: on, onTap: () => lc.togglePart(p.key)),
      );
    }

    // the chips are 36 dp in 48 dp touch rows: the rows touch, the chips keep 12 dp apart
    final rows = <Widget>[
      Wrap(spacing: Sp.s, children: [for (final p in people) chip(p)]),
      for (final e in byFace.entries)
        Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Padding(
              padding: const EdgeInsets.only(top: Sp.l, right: Sp.s),
              child: Text(byFace.length > 1 ? '脸 ${e.key}' : '脸', style: T.cap(c.text2)),
            ),
            Expanded(
              child: Wrap(spacing: Sp.s, children: [for (final p in e.value) chip(p)]),
            ),
          ],
        ),
    ];
    return SingleChildScrollView(
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: rows),
    );
  }
}

/// The selected area's actions: one row of icon tiles (加一块 / 减一块 / 反选 / 复制 / 删除).
class AreaActions extends StatelessWidget {
  const AreaActions({super.key, required this.lc});
  final LocalController lc;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final a = lc.sel;
    final full = a != null && a.mask.length >= kMaxMaskComponents;
    // labels that open the shape card end with "…" (guidelines 13.2). U2: no 涂抹补充 / 擦除 here: 加一块… / 减一块… → 画笔, and the brush's
    // own [+|−] switch adds or erases. U3: the destructive action is here and only here, with 撤销 on the toast.
    final chip = lc.actionChip;
    final acts = chip != null
        ? <(String, DI, VoidCallback, bool)>[('关掉', DI.close, () => lc.turnOffChip(chip), true)]
        : lc.actionPortraitGroup
        ? <(String, DI, VoidCallback, bool)>[('删除', DI.trash, lc.deletePortrait, true)]
        : <(String, DI, VoidCallback, bool)>[
            ('加一块…', DI.addPart, () => full ? lc.ctl.app.showToast('每个区域最多 $kMaxMaskComponents 块') : lc.showBody(LocalBody.addPart), false),
            ('减一块…', DI.subPart, () => full ? lc.ctl.app.showToast('每个区域最多 $kMaxMaskComponents 块') : lc.showBody(LocalBody.subPart), false),
            ('反选', DI.invert, lc.invertArea, false),
            ('复制', DI.duplicate, lc.duplicateArea, false),
            // the selected part (chosen on the tool strip) goes; the area stays
            if (a != null && a.mask.length > 1) ('去掉这块', DI.removePart, () => lc.removePart(lc.selComp), false),
            ('删除', DI.trash, lc.deleteArea, true),
          ];
    return LayoutBuilder(
      builder: (context, cons) {
        // as many to a row as fit with their word on one line (a narrow side pane takes two or three rows), at most 84 dp wide
        final scaler = MediaQuery.textScalerOf(context);
        final need = acts.fold<double>(
          kHit,
          (m, x) => math.max(
            m,
            (TextPainter(
                  text: TextSpan(text: x.$1, style: T.cap(c.text2)),
                  textDirection: TextDirection.ltr,
                  textScaler: scaler,
                )..layout()).width +
                Sp.l,
          ),
        );
        final fit = math.max(1, ((cons.maxWidth + Sp.s) / (need + Sp.s)).floor());
        final rows = (acts.length / math.min(fit, acts.length > 5 ? (acts.length / 2).ceil() : acts.length)).ceil();
        final perRow = (acts.length / rows).ceil();
        final w = TileGrid.tileWidth(cons.maxWidth, perRow);
        // the shared tile (audit U7); the destructive one in `danger`
        return Center(
          child: TileGrid(
            children: [
              for (final x in acts)
                DTile(
                  key: ValueKey('act-${x.$1.replaceAll('…', '')}'),
                  label: x.$1,
                  icon: x.$2,
                  danger: x.$4,
                  width: w,
                  height: 72 + math.max(0.0, MediaQuery.textScalerOf(context).scale(12) / 12 - 1) * 26,
                  onTap: x.$3,
                ),
            ],
          ),
        );
      },
    );
  }
}

/// The itemised view of the selected area: three group chips, the radar of that group, one ruler.
class LocalItemised extends StatelessWidget {
  const LocalItemised({super.key, required this.ctl});
  final EditorController ctl;

  @override
  Widget build(BuildContext context) {
    final lc = ctl.local;
    if (lc.selIsPortrait) {
      // a portrait chip: its own few parameters, one radar (no groups)
      final items = lc.portraitItems();
      final sel = ctl.selItem.clamp(0, items.length - 1);
      return Column(
        children: [
          Expanded(
            child: RadarView(
              key: ValueKey('local-radar-${lc.selId}'),
              ctl: ctl,
              cat: PadCat.light,
              items: items,
              selected: sel,
              onSelect: ctl.selectItem,
              source: lc.items,
            ),
          ),
          const SizedBox(height: Sp.s),
          ItemRuler(ctl: ctl, cat: PadCat.light, item: items[sel], source: lc.items),
        ],
      );
    }
    final items = lc.itemsOf(lc.itemGroup);
    final sel = ctl.selItem.clamp(0, items.length - 1);
    const groups = [('light', '光线'), ('color', '颜色'), ('detail', '质地')];
    // a short side pane (landscape phone): the gaps go before anything is clipped; the radar takes what is left
    return LayoutBuilder(
      builder: (context, cons) {
        final tight = cons.maxHeight < 200;
        return Column(
          children: [
            Row(
              children: [
                for (var i = 0; i < groups.length; i++) ...[
                  if (i > 0) const SizedBox(width: Sp.s),
                  Expanded(
                    child: PanelChip(
                      key: ValueKey('local-group-${groups[i].$1}'),
                      label: groups[i].$2,
                      selected: lc.itemGroup == groups[i].$1,
                      compact: true,
                      onTap: () => lc.setItemGroup(groups[i].$1),
                    ),
                  ),
                ],
              ],
            ),
            SizedBox(height: tight ? 0 : Sp.xs),
            Expanded(
              child: RadarView(
                key: ValueKey('local-radar-${lc.itemGroup}'),
                ctl: ctl,
                cat: PadCat.light,
                items: items,
                selected: sel,
                onSelect: ctl.selectItem,
                source: lc.items,
              ),
            ),
            SizedBox(height: tight ? 0 : Sp.s),
            ItemRuler(ctl: ctl, cat: PadCat.light, item: items[sel], source: lc.items),
          ],
        );
      },
    );
  }
}

// ------------------------------------------------------------------ the dock glyph

/// The dock's 局部 glyph: the selected (or last) area's kind, a count badge when there are several (a 16 dp disc in the glyph's
/// colour with the number in 11 px [ink], the same badge as the photo grid's counts; never smaller than the type scale's 11).
class LocalDockGlyphPainter extends CustomPainter {
  const LocalDockGlyphPainter(
    this.glyph,
    this.count,
    this.color, {
    this.ink = Fx.onPhoto,
    this.textScale = 1,
    this.base,
    this.strength,
  });
  final LocalGlyph glyph;
  final int count;
  final Color color, ink;
  final double textScale;

  /// The ambient text style (font family): a bare TextPainter would not inherit it.
  final TextStyle? base;

  /// The selected area's strip (0..1), drawn as the thin bar every dock glyph has (one element = entry + state + readout, audit F42).
  final double? strength;

  @override
  void paint(Canvas canvas, Size size) {
    const g = 24.0;
    final box = Rect.fromLTWH(1, (size.height - g) / 2, g, g);
    paintLocalGlyph(canvas, box, glyph, color, stroke: 1.5);
    final a = strength;
    if (a != null) {
      final h = 2 + a.clamp(0.0, 1.0) * 22;
      canvas.drawRRect(
        RRect.fromRectAndRadius(Rect.fromLTRB(box.right + 3.5, box.bottom + 1.5 - h, box.right + 6, box.bottom + 1.5), const Radius.circular(1.25)),
        Paint()..color = color,
      );
    }
    if (count > 1) {
      final tp = TextPainter(
        text: TextSpan(
          text: '$count',
          style: (base ?? const TextStyle()).merge(T.mini(ink, w: FontWeight.w600)).copyWith(height: 1, fontFeatures: const [FontFeature.tabularFigures()]),
        ),
        textDirection: TextDirection.ltr,
        textScaler: TextScaler.linear(textScale),
      )..layout();
      final d = math.max(16.0 * textScale, tp.width + 6);
      final centre = Offset(box.left + 1, box.top + 3); // top-left: the strength bar has the right edge
      canvas.drawRRect(
        RRect.fromRectAndRadius(Rect.fromCenter(center: centre, width: d, height: 16 * textScale), Radius.circular(8 * textScale)),
        Paint()..color = color,
      );
      tp.paint(canvas, centre - Offset(tp.width / 2, tp.height / 2));
    }
  }

  @override
  bool shouldRepaint(LocalDockGlyphPainter o) =>
      o.glyph != glyph || o.count != count || o.color != color || o.ink != ink || o.textScale != textScale || o.strength != strength;
}

// ------------------------------------------------------------------ tool strip (on the photo)

/// The "where" fine controls on the photo's bottom edge: the area's parts (when several), the brush sizes and eraser, the tone band
/// or the hue band. Hidden for a single gradient / circle.
/// Height of the line reserved under the photo for the tool strip: one line of 48 dp targets, two where the width is too narrow for the
/// widest tool (the heal tools: the switch, four sizes and the toggle) on one line.
double toolStripH(double width) => (width < 360 ? 2 : 1) * kHit;

/// Whether the tool strip has anything to show now (a tool, or the part chips of a multi-part area): its line under the photo is
/// reserved only then. The same conditions as [LocalToolStrip] builds with.
bool localToolsShown(EditorController ctl) {
  final lc = ctl.local;
  final a = lc.sel, m = lc.comp;
  if (!lc.active || a == null || m == null) return false;
  if (lc.body != LocalBody.pads && !(lc.body == LocalBody.portrait && lc.selIsPortrait)) return false;
  if (ctl.compare.value || lc.comparing) return false;
  if (a.mask.length > 1 && portraitOf(a) == null) return true; // the parts row
  final pt = portraitOf(a);
  switch (m.kind) {
    case MaskKindId.brush:
    case MaskKindId.luminance:
    case MaskKindId.color:
      return true;
    case MaskKindId.raster:
      if (lc.missingRefs.contains(m.ref) || lc.repairing) return true;
      if (pt != null) return pt.$2.id == 'pt-skin';
      final k = kindOfComponent(m).id;
      return k == 'tap' || k == 'depth';
    default:
      return false;
  }
}

class LocalToolStrip extends StatelessWidget {
  const LocalToolStrip({super.key, required this.ctl});
  final EditorController ctl;

  @override
  Widget build(BuildContext context) {
    final lc = ctl.local;
    return ListenableBuilder(
      listenable: Listenable.merge([ctl.wm, ctl.edits, ctl.app.editTick, lc]),
      builder: (context, _) {
        final a = lc.sel, m = lc.comp;
        if (!lc.active ||
            a == null ||
            m == null ||
            (lc.body != LocalBody.pads && !(lc.body == LocalBody.portrait && lc.selIsPortrait)) ||
            ctl.compare.value ||
            lc.comparing) {
          return const SizedBox.shrink();
        }
        final c = context.c;
        // a portrait area's parts are its recipe's business (the simplified view): no parts row
        final parts = a.mask.length > 1 && portraitOf(a) == null;
        final children = <Widget>[];
        if (parts) {
          for (var i = 0; i < a.mask.length; i++) {
            children.add(_PartChip(lc: lc, index: i));
          }
        }
        Widget? tool;
        final pt = portraitOf(a);
        switch (m.kind) {
          case MaskKindId.brush:
            tool = _BrushTools(lc: lc);
          case MaskKindId.luminance:
            tool = _RangeBar(lc: lc, hue: false);
          case MaskKindId.color:
            tool = _RangeBar(lc: lc, hue: true);
          case MaskKindId.raster:
            final k = kindOfComponent(m).id;
            if (lc.missingRefs.contains(m.ref) || lc.repairing) {
              tool = _Reanalyse(lc: lc);
            } else if (pt != null && pt.$2.id == 'pt-skin') {
              tool = _HealTools(lc: lc);
            } else if (pt != null) {
              tool = null;
            } else if (k == 'tap') {
              tool = _TapTools(lc: lc);
            } else if (k == 'depth') {
              tool = Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Text('近', style: T.mini(c.text2)),
                  const SizedBox(width: 4),
                  _RangeBar(lc: lc, hue: false, depth: true),
                  const SizedBox(width: 4),
                  Text('远', style: T.mini(c.text2)),
                ],
              );
            } else {
              tool = null;
            }
          default:
            tool = null;
        }
        if (tool == null && !parts) return const SizedBox.shrink();
        if (tool != null) {
          if (parts) {
            children.add(
              Container(
                width: 1,
                height: 24,
                color: c.sep,
                margin: const EdgeInsets.symmetric(horizontal: Sp.s, vertical: Sp.m),
              ),
            );
          }
          children.add(tool);
        }
        // every item is a 48 dp target: when the parts and the tool do not fit the photo's width the strip wraps onto a second line
        // (above the first, the strip stays on the photo's bottom edge); it never scales its targets down
        return Align(
          alignment: Alignment.bottomCenter,
          child: Container(
            key: const ValueKey('local-tools'),
            padding: const EdgeInsets.symmetric(horizontal: Sp.xs),
            decoration: BoxDecoration(color: c.surface, borderRadius: BorderRadius.circular(Rd.float)),
            child: Wrap(
              alignment: WrapAlignment.center,
              crossAxisAlignment: WrapCrossAlignment.center,
              verticalDirection: VerticalDirection.up,
              children: children,
            ),
          ),
        );
      },
    );
  }
}

class _PartChip extends StatelessWidget {
  const _PartChip({required this.lc, required this.index});
  final LocalController lc;
  final int index;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final a = lc.sel!;
    final m = a.mask[index];
    final selected = index == lc.selComp;
    final fg = selected ? c.accent : c.text;
    final sign = index == 0 ? '' : (m.op == MaskOp.subtract ? '−' : (m.op == MaskOp.intersect ? '∩' : '+'));
    final k = kindOfComponent(m);
    return Semantics(
      button: true,
      selected: selected,
      label: '${index == 0 ? '' : (m.op == MaskOp.subtract ? '减去 ' : (m.op == MaskOp.intersect ? '限定 ' : '加上 '))}${k.label}',
      excludeSemantics: true,
      onTap: () => lc.selectPart(index),
      child: GestureDetector(
        key: ValueKey('local-part-$index'),
        behavior: HitTestBehavior.opaque,
        onTap: () => lc.selectPart(index),
        child: HitBox(
          child: Container(
            width: 36,
            height: 36,
            decoration: BoxDecoration(
              color: selected ? c.accentSoft : c.chip,
              shape: BoxShape.circle,
              border: selected ? Border.all(color: c.accent, width: 1.5) : null,
            ),
            child: Stack(
              children: [
                Center(child: LocalGlyphIcon(k.glyph, size: 20, color: fg)),
                if (sign.isNotEmpty)
                  Positioned(
                    left: Sp.xs,
                    top: 2,
                    child: Text(sign, style: T.mini(fg, w: FontWeight.w700).copyWith(height: 1)),
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

/// The one add / remove switch of every tool that adds or takes away on the photo (audit U2): the brush, 点选 and 点一下祛斑. Always at the
/// left end of the tool strip, + by default. Two 48 dp halves on one 36 dp track; the chosen half is the persistent selection (soft fill
/// and accent border, U7).
class AddRemoveSwitch extends StatelessWidget {
  const AddRemoveSwitch({super.key, required this.prefix, required this.add, required this.onChanged});
  final String prefix;
  final bool add;
  final ValueChanged<bool> onChanged;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    Widget seg(bool a) {
      final on = add == a;
      return Semantics(
        button: true,
        inMutuallyExclusiveGroup: true,
        selected: on,
        label: a ? '加上' : '去掉',
        excludeSemantics: true,
        onTap: () => onChanged(a),
        child: GestureDetector(
          key: ValueKey(a ? '$prefix-add' : '$prefix-sub'),
          behavior: HitTestBehavior.opaque,
          onTap: () => onChanged(a),
          child: HitBox(
            child: Container(
              width: 44,
              height: 32,
              alignment: Alignment.center,
              decoration: BoxDecoration(
                color: on ? c.accentSoft : null,
                borderRadius: BorderRadius.circular(Rd.cardLg),
                border: on ? Border.all(color: c.accent, width: 1.5) : null,
              ),
              child: DIcon(a ? DI.plus : DI.minus, size: 20, color: on ? c.accent : c.text),
            ),
          ),
        ),
      );
    }

    return Stack(
      alignment: Alignment.center,
      children: [
        Container(
          width: 2 * 44 + 4,
          height: 36,
          decoration: BoxDecoration(color: c.bg, borderRadius: BorderRadius.circular(Rd.chip)),
        ),
        Row(mainAxisSize: MainAxisSize.min, children: [seg(true), seg(false)]),
      ],
    );
  }
}

class _BrushTools extends StatelessWidget {
  const _BrushTools({required this.lc});
  final LocalController lc;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    // a narrow pane wraps the sizes onto a second line (targets keep 48 dp)
    return Wrap(
      alignment: WrapAlignment.center,
      crossAxisAlignment: WrapCrossAlignment.center,
      children: [
        // U2: + paints, − erases (the switch at the left end, as on every add / remove tool)
        AddRemoveSwitch(prefix: 'brush', add: !lc.erase, onChanged: (a) => lc.setErase(!a)),
        const SizedBox(width: Sp.xs),
        for (var i = 0; i < LocalController.brushRadiiDp.length; i++)
          Semantics(
            button: true,
            selected: lc.brushSize == i,
            label: kSizeNames[i],
            excludeSemantics: true,
            child: GestureDetector(
              key: ValueKey('brush-size-$i'),
              behavior: HitTestBehavior.opaque,
              onTap: () => lc.setBrushSize(i),
              child: SizedBox(
                width: kHit,
                height: kHit,
                child: Center(
                  child: Container(
                    width: 6.0 + i * 5,
                    height: 6.0 + i * 5,
                    decoration: BoxDecoration(shape: BoxShape.circle, color: lc.brushSize == i ? c.accent : c.text2.withValues(alpha: 0.55)),
                  ),
                ),
              ),
            ),
          ),
      ],
    );
  }
}

/// The four sizes of the brush and of the heal spots: one set of words for both (audit C).
const kSizeNames = ['小', '中', '大', '特大'];

/// A tone (dark -> light) or hue band with the selected range raised; drag an end to resize it, the middle to move it.
class _RangeBar extends StatefulWidget {
  const _RangeBar({required this.lc, required this.hue, this.depth = false});
  final LocalController lc;
  final bool hue;

  /// A near (left) - far (right) band of the depth plane (same lo / hi / feather as the tone band).
  final bool depth;

  @override
  State<_RangeBar> createState() => _RangeBarState();
}

class _RangeBarState extends State<_RangeBar> {
  static const double w = 196;
  int _grab = 0; // 1 lo end, 2 hi end, 3 middle
  double _x0 = 0, _a0 = 0, _b0 = 0;

  LocalController get lc => widget.lc;

  /// Current range as positions on the bar (0..1): lum lo..hi, or hue centre +- range (may wrap).
  (double, double) _ends() {
    final m = lc.comp!;
    if (!widget.hue) return (m.lo / 100, m.hi / 100);
    return ((m.hue - m.range) / 360, (m.hue + m.range) / 360);
  }

  void _down(Offset p) {
    final e = _ends();
    final t = p.dx / w;
    double dist(double a) {
      var d = (t - a).abs();
      if (widget.hue) d = math.min(d, 1 - d);
      return d * w;
    }

    _grab = dist(e.$1) < 16 ? 1 : (dist(e.$2) < 16 ? 2 : 3);
    _x0 = p.dx;
    _a0 = e.$1;
    _b0 = e.$2;
    lc.rangeBegin();
  }

  void _move(Offset p) {
    final dt = (p.dx - _x0) / w;
    var a = _a0, b = _b0;
    if (_grab == 1) a += dt;
    if (_grab == 2) b += dt;
    if (_grab == 3) {
      a += dt;
      b += dt;
    }
    if (widget.hue) {
      final centre = (a + b) / 2 * 360, range = ((b - a) / 2 * 360).abs();
      lc.setHue(centre, range);
    } else {
      if (_grab == 3) {
        final span = b - a;
        a = a.clamp(0.0, 1.0 - span);
        b = a + span;
      }
      lc.setLum((a * 100).clamp(0.0, 100.0), (b * 100).clamp(0.0, 100.0));
    }
  }

  void _end(Offset p, bool cancelled) {
    if (_grab != 0) lc.rangeEnd();
    _grab = 0;
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final m = lc.comp!;
    return Semantics(
      slider: true,
      label: widget.hue ? '颜色范围' : (widget.depth ? '远近范围' : '明暗范围'),
      value: widget.hue ? '${m.hue.round()}° ± ${m.range.round()}°' : '${m.lo.round()} – ${m.hi.round()}',
      child: SizedBox(
        width: w,
        height: kHit,
        child: ClaimSurface(
          onDown: _down,
          onMove: _move,
          onEnd: _end,
          child: CustomPaint(
            key: ValueKey(widget.hue ? 'hue-bar' : 'tone-bar'),
            painter: _RangePainter(widget.hue, m, c.text, c.surface, depth: widget.depth),
          ),
        ),
      ),
    );
  }
}

class _RangePainter extends CustomPainter {
  _RangePainter(this.hue, this.m, this.ink, this.card, {this.depth = false});
  final bool hue, depth;
  final MaskComponent m;
  final Color ink, card;

  @override
  void paint(Canvas canvas, Size size) {
    const h = 20.0;
    final bar = Rect.fromLTWH(0, (size.height - h) / 2, size.width, h);
    final rr = RRect.fromRectAndRadius(bar, const Radius.circular(10));
    canvas.save();
    canvas.clipRRect(rr);
    if (hue) {
      final colors = [for (var i = 0; i <= 12; i++) oklabHueColor(i * 30.0)];
      canvas.drawRect(bar, Paint()..shader = LinearGradient(colors: colors).createShader(bar));
    } else {
      // tones: dark -> light; distance: near (warm, strong) -> far (hazy blue), as an air-perspective hint
      final cols = depth ? Fx.depthTrack : Fx.toneTrack;
      canvas.drawRect(bar, Paint()..shader = LinearGradient(colors: cols).createShader(bar));
    }
    // dim what is not selected (the soft edge drawn as a fade)
    // what is not selected fades toward the card (light, not a dark veil)
    final dim = Paint()..color = card.withValues(alpha: 0.72);
    final w = size.width;
    void band(double a, double b, double soft) {
      // keep [a, b] clear, fade over [a - soft, a] and [b, b + soft]; everything else dimmed
      final path = Path()..addRect(bar);
      canvas.saveLayer(bar, Paint());
      canvas.drawPath(path, dim);
      final clear = Paint()..blendMode = BlendMode.dstOut;
      for (final shift in hue ? const [-1.0, 0.0, 1.0] : const [0.0]) {
        final l = (a + shift) * w, r = (b + shift) * w, s = soft * w;
        canvas.drawRect(Rect.fromLTRB(l, bar.top, r, bar.bottom), clear);
        final fadeL = Rect.fromLTRB(l - s, bar.top, l, bar.bottom), fadeR = Rect.fromLTRB(r, bar.top, r + s, bar.bottom);
        if (s > 0.5) {
          canvas.drawRect(
            fadeL,
            Paint()
              ..blendMode = BlendMode.dstOut
              ..shader = const LinearGradient(colors: [Color(0x00000000), Color(0xFF000000)]).createShader(fadeL), // token-exempt: an alpha mask, not a colour
          );
          canvas.drawRect(
            fadeR,
            Paint()
              ..blendMode = BlendMode.dstOut
              ..shader = const LinearGradient(colors: [Color(0xFF000000), Color(0x00000000)]).createShader(fadeR), // token-exempt: an alpha mask, not a colour
          );
        }
      }
      canvas.restore();
    }

    final double a, b, soft;
    if (hue) {
      a = (m.hue - m.range) / 360;
      b = (m.hue + m.range) / 360;
      soft = m.hueFeather / 360;
    } else {
      a = m.lo / 100;
      b = m.hi / 100;
      soft = m.lumFeather / 100;
    }
    band(a, b, soft);
    canvas.restore();
    canvas.drawRRect(
      rr,
      Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 1
        ..color = Fx.handleRim.withValues(alpha: 0.2),
    );
    // the two end handles
    for (final t in [a, b]) {
      var x = (t % 1 + 1) % 1 * w;
      if (!hue) x = t.clamp(0.0, 1.0) * w;
      final r = Rect.fromCenter(center: Offset(x, size.height / 2), width: 6, height: h + 8);
      canvas.drawRRect(RRect.fromRectAndRadius(r.inflate(1), const Radius.circular(4)), Paint()..color = Fx.handleRim);
      canvas.drawRRect(RRect.fromRectAndRadius(r, const Radius.circular(3)), Paint()..color = Fx.handle);
    }
  }

  @override
  bool shouldRepaint(_RangePainter o) => !o.m.same(m) || o.hue != hue || o.ink != ink;
}

/// sRGB colour of an OKLab hue angle at a moderate lightness and chroma (the hue bar matches what the colour range selects).
Color oklabHueColor(double hueDeg) {
  final h = hueDeg * math.pi / 180;
  const l = 0.72, ch = 0.13;
  final a = ch * math.cos(h), b = ch * math.sin(h);
  final l_ = l + 0.3963377774 * a + 0.2158037573 * b;
  final m_ = l - 0.1055613458 * a - 0.0638541728 * b;
  final s_ = l - 0.0894841775 * a - 1.2914855480 * b;
  final ll = l_ * l_ * l_, mm = m_ * m_ * m_, ss = s_ * s_ * s_;
  final r = 4.0767416621 * ll - 3.3077115913 * mm + 0.2309699292 * ss;
  final g = -1.2684380046 * ll + 2.6097574011 * mm - 0.3413193965 * ss;
  final bl = -0.0041960863 * ll - 0.7034186147 * mm + 1.7076147010 * ss;
  int enc(double x) {
    final v = x.clamp(0.0, 1.0);
    final e = v <= 0.0031308 ? 12.92 * v : 1.055 * math.pow(v, 1 / 2.4) - 0.055;
    return (e * 255).round().clamp(0, 255);
  }

  return Color.fromARGB(255, enc(r), enc(g), enc(bl));
}

/// 点选: the [+|−] switch (U2) and a ring while a tap is computed (the photo stays usable). Every tap is its own undo step, so the top
/// bar's 撤销 takes the last one back: the strip has no undo of its own.
class _TapTools extends StatelessWidget {
  const _TapTools({required this.lc});
  final LocalController lc;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        AddRemoveSwitch(prefix: 'tap', add: lc.tapAdd, onChanged: lc.setTapAdd),
        SizedBox(
          width: 36,
          child: lc.tapBusy
              ? Center(
                  child: SizedBox(
                    key: const ValueKey('tap-busy'),
                    width: 20,
                    height: 20,
                    child: CircularProgressIndicator(strokeWidth: 2.5, color: c.accent),
                  ),
                )
              : const SizedBox.shrink(),
        ),
      ],
    );
  }
}

/// A plane of this area is not in the store and could not be re-created on its own: one button tries again.
class _Reanalyse extends StatelessWidget {
  const _Reanalyse({required this.lc});
  final LocalController lc;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    if (lc.repairing) {
      return Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2.5, color: c.accent)),
          const SizedBox(width: 8),
          Text('正在重新分析', style: T.cap(c.text2)),
          const SizedBox(width: Sp.s),
        ],
      );
    }
    return HeaderButton(key: const ValueKey('ai-reanalyse'), label: '重新分析', icon: null, onTap: lc.reanalyse);
  }
}

// ------------------------------------------------------------------ 人像

/// The simple portrait panel: the strip of the selected chip on the left (bottom 0, middle = the recommended default, top = the recipe's
/// maximum) and the chips; a face selector only when several frontal faces exist. 分项 shows the chip's parameters.
/// The height the body of the portrait chips layer needs (the rows of tiles and the face selector), or null when the body is not that
/// layer. [innerW]: the panel's width inside its padding. Mirrors [PortraitPanel]'s layout (strip 40 + gap, up to four columns of
/// 56 dp tiles, tile height up to 64 dp).
double? portraitBodyNeed(LocalController lc, double innerW, double textScale) {
  if (lc.body != LocalBody.portrait && !(lc.body == LocalBody.pads && lc.selIsPortrait)) return null;
  if (!lc.portraitAnalysed || !lc.personFound) return null;
  final n = lc.portraitChips.length;
  if (n == 0) return null;
  const gap = Sp.s;
  final avail = innerW - 40 - Sp.m;
  final fit = ((avail + gap) / (56 + gap)).floor();
  final cols = math.max(1, math.min(math.min(4, fit), n));
  final rows = (n / cols).ceil();
  final faceRow = lc.frontalFaces.length > 1 && lc.selChip != null ? kHit + Sp.s : 0.0;
  return rows * 64.0 * textScale.clamp(1.0, 2.0) + gap * (rows - 1) + faceRow;
}

class PortraitPanel extends StatelessWidget {
  const PortraitPanel({super.key, required this.ctl});
  final EditorController ctl;

  /// The app's own line icons (guidelines 8): 眼下 = an eye with an arc under it, 眼睛 = an eye with a catch light.
  static DI iconOf(PortraitChip c) => switch (c) {
    PortraitChip.skin => DI.skin,
    PortraitChip.undereye => DI.underEye,
    PortraitChip.shine => DI.shine,
    PortraitChip.teeth => DI.teeth,
    PortraitChip.eyes => DI.eyes,
    PortraitChip.lips => DI.lips,
    PortraitChip.light => DI.faceLight,
  };

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final lc = ctl.local;
    if (!lc.portraitAnalysed) {
      if (lc.aiRunning == 0 && !lc.portraitWaiting) WidgetsBinding.instance.addPostFrameCallback((_) => lc.openPortrait());
      return Center(
        child: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            SizedBox(
              key: const ValueKey('portrait-progress'),
              width: 28,
              height: 28,
              child: CircularProgressIndicator(value: lc.aiProgress <= 0 ? null : lc.aiProgress, strokeWidth: 3, color: c.accent, backgroundColor: c.track),
            ),
            const SizedBox(width: Sp.m),
            Text('正在找人', style: T.body(c.text2)),
            const SizedBox(width: Sp.s),
            HeaderButton(
              key: const ValueKey('portrait-cancel'),
              label: '取消',
              onTap: () {
                lc.cancelAnalysis();
                lc.portraitWaiting = false;
                lc.showBody(lc.list.isEmpty ? LocalBody.addArea : LocalBody.pads);
              },
            ),
          ],
        ),
      );
    }
    if (!lc.personFound) {
      return Center(
        child: Text('没找到人', key: const ValueKey('portrait-none'), style: T.body(c.text2)),
      );
    }
    final chips = lc.portraitChips;
    final selChip = lc.selChip;
    final faces = lc.frontalFaces;
    final ts = MediaQuery.textScalerOf(context).scale(12) / 12;
    // the shared tile (audit U7): applied = soft fill ("on"), the one being adjusted = soft fill + border with the ⌄ cue (U3: a second
    // tap opens its actions, 关掉 is there)
    Widget chip(PortraitChip pc, double w, double h) {
      final on = lc.chipOn(pc), selected = on && selChip == pc;
      final spec = specOf(pc);
      return DTile(
        key: ValueKey('chip-${pc.name}'),
        label: spec.label,
        icon: iconOf(pc),
        width: w,
        height: h,
        state: selected ? TileState.selected : (on ? TileState.on : TileState.normal),
        cue: selected,
        semantics: '${spec.label}${on ? '，已开' : ''}',
        onTap: () => lc.tapChip(pc),
      );
    }

    return Row(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        StrengthStrip(
          key: ValueKey('portrait-strip-${lc.selId}'),
          ctl: ctl,
          cat: PadCat.light,
          source: lc.lightPad,
          label: selChip == null ? '强度' : '${specOf(selChip).label}强度',
        ),
        const SizedBox(width: Sp.m),
        Expanded(
          child: LayoutBuilder(
            builder: (context, cons) {
              // every chip in view: four to a row (seven chips = two rows); the tiles shrink with the panel down to their 48 dp target,
              // never below it (a narrower panel takes fewer columns instead)
              const gap = Sp.s;
              final fit = ((cons.maxWidth + gap) / (56 + gap)).floor();
              final cols = math.min(math.min(4, fit), math.max(1, chips.length));
              final rows = (chips.length / cols).ceil();
              final faceRow = faces.length > 1 && selChip != null ? kHit + Sp.s : 0.0;
              final w = TileGrid.tileWidth(cons.maxWidth, cols, max: 80);
              final h = ((cons.maxHeight - faceRow - gap * (rows - 1)) / rows).clamp(56.0, 64.0 * ts).toDouble();
              return SingleChildScrollView(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Wrap(spacing: gap, runSpacing: gap, children: [for (final pc in chips) chip(pc, w, h)]),
                    if (faces.length > 1 && selChip != null) ...[
                      const SizedBox(height: Sp.s),
                      Row(
                        children: [
                          for (final f in faces) ...[_FaceThumb(lc: lc, face: f, on: lc.selFaces.contains(f.n)), const SizedBox(width: Sp.xs)],
                        ],
                      ),
                    ],
                  ],
                ),
              );
            },
          ),
        ),
      ],
    );
  }
}

/// A detected face as a small round crop of the picture on screen; the ring says the selected chip applies to it.
class _FaceThumb extends StatelessWidget {
  const _FaceThumb({required this.lc, required this.face, required this.on});
  final LocalController lc;
  final PortraitFace face;
  final bool on;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final img = lc.ctl.engine.image;
    final box = lc.faceBox(face.n);
    return Semantics(
      button: true,
      toggled: on,
      label: '脸 ${face.n}',
      excludeSemantics: true,
      child: GestureDetector(
        key: ValueKey('face-${face.n}'),
        behavior: HitTestBehavior.opaque,
        onTap: () => lc.toggleFace(face.n),
        child: HitBox(
          child: Container(
            width: 40,
            height: 40,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              border: Border.all(color: on ? c.accent : c.sep, width: on ? 2.5 : 1),
            ),
            child: ClipOval(
              child: Opacity(
                opacity: on ? 1 : 0.45,
                child: img == null || box == null ? Center(child: Text('${face.n}', style: T.cap(c.text2))) : CustomPaint(painter: _CropPainter(img, box)),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _CropPainter extends CustomPainter {
  _CropPainter(this.img, this.box);
  final ui.Image img;
  final Rect box; // frame-normalised

  @override
  void paint(Canvas canvas, Size size) {
    final src = Rect.fromLTRB(box.left * img.width, box.top * img.height, box.right * img.width, box.bottom * img.height);
    canvas.drawImageRect(img, src, Offset.zero & size, Paint()..filterQuality = FilterQuality.medium);
  }

  @override
  bool shouldRepaint(_CropPainter o) => o.img != img || o.box != box;
}

/// 肤质's tool: 点一下祛斑 (on: taps on the photo add or remove heal spots, rings show) with the [+|−] switch at the left end (U2: − removes
/// the spot tapped; a tap never toggles) and the spot size.
class _HealTools extends StatelessWidget {
  const _HealTools({required this.lc});
  final LocalController lc;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final toggle = Semantics(
      button: true,
      toggled: lc.healMode,
      label: '点一下祛斑',
      excludeSemantics: true,
      child: GestureDetector(
        key: const ValueKey('heal-mode'),
        behavior: HitTestBehavior.opaque,
        onTap: () => lc.setHealMode(!lc.healMode),
        child: HitBox(
          // off: the words; on: a small ✕ that ends the mode (the strip then holds the [+|−] switch and the sizes on one line)
          child: Container(
            height: 32,
            width: lc.healMode ? 36 : null,
            padding: EdgeInsets.symmetric(horizontal: lc.healMode ? 0 : Sp.m),
            // on = soft fill (U7: "on", not a held press)
            decoration: BoxDecoration(color: lc.healMode ? c.accentSoft : c.bg, borderRadius: BorderRadius.circular(Rd.cardLg)),
            // shrink-wraps its word (inside the strip's wrap a free-width box would take the whole line)
            child: Center(
              widthFactor: 1,
              child: lc.healMode ? DIcon(DI.close, size: 16, color: c.accent) : Text('点一下祛斑', style: T.cap(c.text, w: FontWeight.w500)),
            ),
          ),
        ),
      ),
    );
    // a narrow photo: the parts wrap onto a second line of the strip (U10: targets keep 48 dp)
    return Wrap(
      alignment: WrapAlignment.center,
      crossAxisAlignment: WrapCrossAlignment.center,
      verticalDirection: VerticalDirection.up,
      children: [
        if (lc.healMode) ...[
          AddRemoveSwitch(prefix: 'heal', add: lc.healAdd, onChanged: lc.setHealAdd),
          for (var i = 0; i < 4; i++)
            Semantics(
              button: true,
              selected: lc.healSize == i,
              label: kSizeNames[i],
              excludeSemantics: true,
              child: GestureDetector(
                key: ValueKey('heal-size-$i'),
                behavior: HitTestBehavior.opaque,
                onTap: () => lc.setHealSize(i),
                child: SizedBox(
                  width: kHit,
                  height: kHit,
                  child: Center(
                    child: Container(
                      width: 6.0 + i * 4,
                      height: 6.0 + i * 4,
                      decoration: BoxDecoration(
                        shape: BoxShape.circle,
                        border: Border.all(color: lc.healSize == i ? c.accent : c.text2, width: 1.5),
                      ),
                    ),
                  ),
                ),
              ),
            ),
        ],
        toggle,
      ],
    );
  }
}
