// The 局部 panel (docs/design/local-adjustments.md sections 3, 4, 5 and 7): the header with the area chips, the body (the strip and
// the two pads, the itemised view, the shape card, the area's actions) and the tool strip that floats on the photo's bottom edge for
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
import 'editor_controller.dart';
import 'itemised_view.dart';
import 'local_controller.dart';
import 'local_glyphs.dart';
import 'pad_gestures.dart';
import 'pad_panel.dart' show HeaderButton, PanelChip, headerWraps, kHeaderH;
import 'pad_widgets.dart';

// ------------------------------------------------------------------ header

/// Area chips: a 40 dp tile in a 48 dp touch box, laid side by side without gaps (the boxes touch, the tiles keep 8 dp apart).
const double kAreaChip = 40;

class LocalHeader extends StatelessWidget {
  const LocalHeader({super.key, required this.ctl, required this.side, required this.textScale, this.collapsed = false});
  final EditorController ctl;
  final bool side;
  final double textScale;

  /// Folded to the handle and the header: only the first line shows.
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

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final lc = ctl.local;
    final a = lc.sel;
    final portrait = lc.body == LocalBody.portrait || (lc.body == LocalBody.pads && lc.selIsPortrait);
    final title = switch (lc.body) {
      LocalBody.addPart => '加一块',
      LocalBody.subPart => '减一块',
      _ => portrait ? '人像' : '局部',
    };
    final inCard = lc.body != LocalBody.pads && lc.body != LocalBody.portrait && (!lc.list.isEmpty || lc.body == LocalBody.aiParts);
    final parts = lc.body == LocalBody.aiParts && lc.personFound && lc.aiRunning == 0;
    final buttons = <Widget>[
      if (inCard)
        HeaderButton(
          key: const ValueKey('local-back'),
          label: '返回',
          onTap: () {
            if (lc.body == LocalBody.aiParts) lc.cancelAnalysis();
            lc.showBody(LocalBody.pads);
          },
        ),
      // the parts card's one action sits where the eye goes for "done" (the chips keep the whole body)
      if (parts)
        Opacity(
          opacity: lc.partPick.isEmpty ? 0.38 : 1,
          child: HeaderButton(key: const ValueKey('parts-add'), label: lc.partMode == LocalBody.subPart ? '减去' : '加上', pressed: lc.partPick.isNotEmpty, onTap: lc.addPickedParts),
        )
      else if (!inCard && a != null) ...[
        ListenableBuilder(
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
        ),
        // press and hold: the photo without the local areas (the photo's own long-press is the handles' in 局部)
        CompareButton(lc: lc),
        HeaderButton(key: const ValueKey('items-button'), label: '分项', pressed: ctl.itemised, onTap: () => ctl.setItemised(!ctl.itemised)),
      ],
    ];
    final titleStyle = T.body(c.text, w: FontWeight.w600);
    final scaler = MediaQuery.textScalerOf(context);
    final line = kHeaderH + math.max(0.0, textScale - 1) * 24;
    return LayoutBuilder(builder: (context, cons) {
      final w = cons.maxWidth - 2 * Sp.l;
      double textW(String s, TextStyle st) => (TextPainter(text: TextSpan(text: s, style: st), textDirection: TextDirection.ltr, textScaler: scaler)..layout()).width;
      // what the first line needs besides the chips: the title and the buttons at their own size (a pill is its word + 24, at least 48)
      final labels = [
        if (inCard) '返回',
        if (parts) lc.partMode == LocalBody.subPart ? '减去' : '加上',
        if (!parts && !inCard && a != null) ...['', '对比', '分项'],
      ];
      final buttonsW = labels.fold<double>(0, (s, l) => s + Sp.xs + math.max(kHit, textW(l, T.cap(c.text, w: FontWeight.w500)) + 2 * Sp.m));
      final n = chipCount(lc);
      final inline = !side && !headerWraps(textScale) && textW(title, titleStyle) + Sp.m + n * kHit + buttonsW <= w;
      final perLine = math.max(1, (w / kHit).floor());
      // crowded chips wrap onto more lines (they never shrink below their touch size); folded, only the first line shows
      final chipLines = inline || collapsed ? 0 : (n / perLine).ceil();
      final buttonRow = Row(mainAxisSize: MainAxisSize.min, children: [for (final b in buttons) ...[const SizedBox(width: Sp.xs), b]]);
      final first = Row(
        children: [
          Text(title, key: const ValueKey('header-title'), style: titleStyle),
          const SizedBox(width: Sp.m),
          Expanded(child: inline ? LocalChips(lc: lc) : const SizedBox.shrink()),
          // large text: the buttons keep their words at full size; on a narrow pane they scroll rather than shrink
          Flexible(flex: 0, child: SingleChildScrollView(scrollDirection: Axis.horizontal, reverse: true, child: buttonRow)),
        ],
      );
      return SizedBox(
        height: line + chipLines * kHit,
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: Sp.l),
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              SizedBox(height: line, child: first),
              if (chipLines > 0) SizedBox(height: chipLines * kHit, child: LocalChips(lc: lc)),
            ],
          ),
        ),
      );
    });
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
                if (a.mask.length > 1)
                  Positioned(
                    right: 1.5,
                    bottom: 1,
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
        final perRow = rows.fold<int>(1, (n, r) => math.max(n, r.length));
        final tileW = math.min(84.0, (cons.maxWidth - 8 * (perRow - 1)) / perRow);
        final tileH = math.max(56.0, math.min(84.0, (cons.maxHeight - 8 * (rows.length - 1)) / rows.length));
        // a short pane (landscape phone): a smaller glyph rather than clipped words (the words stay at their size)
        final ts = MediaQuery.textScalerOf(context).scale(12) / 12;
        final compact = tileH < (32 + 4 + 16 * ts + (kinds.any((k) => k.note != null) ? 14 * ts : 0));
        Widget tile(LocalKind k) {
          final running = lc.runningFor(k);
          return Semantics(
            button: true,
            label: '${k.label}${k.note == null ? '' : '（${k.note}）'}${running ? '，分析中，再点一次取消' : ''}',
            excludeSemantics: true,
            child: GestureDetector(
              key: ValueKey('kind-${k.id}'),
              behavior: HitTestBehavior.opaque,
              onTap: () => lc.chooseKind(k, mode),
              child: Container(
                width: tileW,
                height: tileH,
                decoration: BoxDecoration(color: c.bg, borderRadius: BorderRadius.circular(Rd.cardLg)),
                child: Column(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    SizedBox(
                      width: 36,
                      height: compact ? 24 : 32,
                      child: Stack(
                        alignment: Alignment.center,
                        children: [
                          Opacity(
                            opacity: running ? 0.35 : 1,
                            child: LocalGlyphIcon(k.glyph, size: compact ? 22 : 28, color: c.text),
                          ),
                          if (running)
                            SizedBox(
                              key: ValueKey('kind-progress-${k.id}'),
                              width: 26,
                              height: 26,
                              child: CircularProgressIndicator(value: lc.aiProgress <= 0 ? null : lc.aiProgress, strokeWidth: 2.5, color: c.accent, backgroundColor: c.track),
                            ),
                        ],
                      ),
                    ),
                    SizedBox(height: compact ? 2 : Sp.xs),
                    Text(k.label, maxLines: 1, style: T.cap(c.text2)),
                    if (k.note != null) Text(k.note!, maxLines: 1, style: T.mini(c.text2.withValues(alpha: 0.7))),
                  ],
                ),
              ),
            ),
          );
        }

        return Center(
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              for (var i = 0; i < rows.length; i++) ...[
                if (i > 0) const SizedBox(height: 8),
                Row(
                  mainAxisAlignment: MainAxisAlignment.center,
                  children: [
                    for (var j = 0; j < rows[i].length; j++) ...[if (j > 0) const SizedBox(width: 8), tile(rows[i][j])],
                  ],
                ),
              ],
            ],
          ),
        );
      },
    );
  }
}

/// 人物: the parts the analysis found, as small chips to pick one or several; one button adds them as one area. While the analysis runs: a
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
            Expanded(child: Wrap(spacing: Sp.s, children: [for (final p in e.value) chip(p)])),
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
    // labels that open the shape card end with "…" (guidelines 13.2)
    final acts = <(String, DI, VoidCallback, bool)>[
      if (lc.selIsAi) ...[('涂抹补充', DI.strokeAdd, () => lc.brushFix(add: true), false), ('涂抹擦除', DI.strokeErase, () => lc.brushFix(add: false), false)],
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
        final need = acts.fold<double>(kHit, (m, x) => math.max(m, (TextPainter(text: TextSpan(text: x.$1, style: T.cap(c.text2)), textDirection: TextDirection.ltr, textScaler: scaler)..layout()).width + Sp.l));
        final fit = math.max(1, ((cons.maxWidth + Sp.s) / (need + Sp.s)).floor());
        final rows = (acts.length / math.min(fit, acts.length > 5 ? (acts.length / 2).ceil() : acts.length)).ceil();
        final perRow = (acts.length / rows).ceil();
        final w = math.min(84.0, (cons.maxWidth - Sp.s * (perRow - 1)) / perRow);
        return Center(
          child: Wrap(
            spacing: 8,
            runSpacing: 8,
            alignment: WrapAlignment.center,
            children: [
              for (final x in acts)
                Semantics(
                  button: true,
                  label: x.$1,
                  excludeSemantics: true,
                  child: GestureDetector(
                    key: ValueKey('act-${x.$1.replaceAll('…', '')}'),
                    behavior: HitTestBehavior.opaque,
                    onTap: x.$3,
                    child: Container(
                      width: w,
                      height: 72 + math.max(0.0, MediaQuery.textScalerOf(context).scale(12) / 12 - 1) * 26,
                      decoration: BoxDecoration(color: c.bg, borderRadius: BorderRadius.circular(Rd.cardLg)),
                      child: Column(
                        mainAxisAlignment: MainAxisAlignment.center,
                        children: [
                          DIcon(x.$2, size: 24 * MediaQuery.textScalerOf(context).scale(12) / 12, color: x.$4 ? c.danger : c.text),
                          const SizedBox(height: Sp.xs),
                          Text(x.$1, maxLines: 1, softWrap: false, style: T.cap(x.$4 ? c.danger : c.text2)),
                        ],
                      ),
                    ),
                  ),
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
            child: RadarView(key: ValueKey('local-radar-${lc.selId}'), ctl: ctl, cat: PadCat.light, items: items, selected: sel, onSelect: ctl.selectItem, source: lc.items),
          ),
          const SizedBox(height: Sp.s),
          ItemRuler(ctl: ctl, cat: PadCat.light, item: items[sel], source: lc.items),
        ],
      );
    }
    final items = lc.itemsOf(lc.itemGroup);
    final sel = ctl.selItem.clamp(0, items.length - 1);
    const groups = [('light', '光线'), ('color', '颜色'), ('detail', '质地')];
    return Column(
      children: [
        Row(
          children: [
            for (var i = 0; i < groups.length; i++) ...[
              if (i > 0) const SizedBox(width: Sp.s),
              Expanded(
                child: PanelChip(key: ValueKey('local-group-${groups[i].$1}'), label: groups[i].$2, selected: lc.itemGroup == groups[i].$1, compact: true, onTap: () => lc.setItemGroup(groups[i].$1)),
              ),
            ],
          ],
        ),
        const SizedBox(height: 4),
        Expanded(
          child: RadarView(key: ValueKey('local-radar-${lc.itemGroup}'), ctl: ctl, cat: PadCat.light, items: items, selected: sel, onSelect: ctl.selectItem, source: lc.items),
        ),
        const SizedBox(height: Sp.s),
        ItemRuler(ctl: ctl, cat: PadCat.light, item: items[sel], source: lc.items),
      ],
    );
  }
}

// ------------------------------------------------------------------ the dock glyph

/// The dock's 局部 glyph: the selected (or last) area's kind, a count badge when there are several (a 16 dp disc in the glyph's
/// colour with the number in 11 px [ink], the same badge as the photo grid's counts; never smaller than the type scale's 11).
class LocalDockGlyphPainter extends CustomPainter {
  const LocalDockGlyphPainter(this.glyph, this.count, this.color, {this.ink = const Color(0xFFFFFFFF), this.textScale = 1, this.base}); // token-exempt: default, callers pass a token
  final LocalGlyph glyph;
  final int count;
  final Color color, ink;
  final double textScale;

  /// The ambient text style (font family): a bare TextPainter would not inherit it.
  final TextStyle? base;

  @override
  void paint(Canvas canvas, Size size) {
    const g = 24.0;
    final box = Rect.fromLTWH(1, (size.height - g) / 2, g, g);
    paintLocalGlyph(canvas, box, glyph, color, stroke: 1.5);
    if (count > 1) {
      final tp = TextPainter(
        text: TextSpan(text: '$count', style: (base ?? const TextStyle()).merge(T.mini(ink, w: FontWeight.w600)).copyWith(height: 1, fontFeatures: const [FontFeature.tabularFigures()])),
        textDirection: TextDirection.ltr,
        textScaler: TextScaler.linear(textScale),
      )..layout();
      final d = math.max(16.0 * textScale, tp.width + 6);
      final centre = Offset(box.right + 1, box.top + 3);
      canvas.drawRRect(RRect.fromRectAndRadius(Rect.fromCenter(center: centre, width: d, height: 16 * textScale), Radius.circular(8 * textScale)), Paint()..color = color);
      tp.paint(canvas, centre - Offset(tp.width / 2, tp.height / 2));
    }
  }

  @override
  bool shouldRepaint(LocalDockGlyphPainter o) => o.glyph != glyph || o.count != count || o.color != color || o.ink != ink || o.textScale != textScale;
}

// ------------------------------------------------------------------ tool strip (on the photo)

/// The "where" fine controls on the photo's bottom edge: the area's parts (when several), the brush sizes and eraser, the tone band
/// or the hue band. Hidden for a single gradient / circle.
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
        if (!lc.active || a == null || m == null || (lc.body != LocalBody.pads && !(lc.body == LocalBody.portrait && lc.selIsPortrait)) || ctl.compare.value) {
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
          if (parts) children.add(Container(width: 1, height: 24, color: c.sep, margin: const EdgeInsets.symmetric(horizontal: Sp.s, vertical: Sp.m)));
          children.add(tool);
        }
        // every item is a 48 dp target: when the parts and the tool do not fit the photo's width the strip wraps onto a second line
        // (above the first, the strip stays on the photo's bottom edge); it never scales its targets down
        return Align(
          alignment: Alignment.bottomCenter,
          child: Container(
            key: const ValueKey('local-tools'),
            padding: const EdgeInsets.symmetric(horizontal: Sp.s),
            decoration: BoxDecoration(color: c.surface, borderRadius: BorderRadius.circular(Rd.float), boxShadow: Fx.shadowFloat),
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

class _BrushTools extends StatelessWidget {
  const _BrushTools({required this.lc});
  final LocalController lc;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
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
        Container(width: 1, height: 24, color: c.sep, margin: const EdgeInsets.symmetric(horizontal: Sp.xs)),
        Semantics(
          button: true,
          toggled: lc.erase,
          label: '擦除',
          excludeSemantics: true,
          child: GestureDetector(
            key: const ValueKey('brush-erase'),
            behavior: HitTestBehavior.opaque,
            onTap: () => lc.setErase(!lc.erase),
            child: HitBox(
              child: Container(
                width: 40,
                height: 32,
                alignment: Alignment.center,
                decoration: BoxDecoration(color: lc.erase ? c.accentBtn : null, borderRadius: BorderRadius.circular(Rd.cardLg)),
                child: DIcon(DI.eraser, size: 20, color: lc.erase ? c.onAccent : c.text),
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

/// 点选: add / subtract the next tap, take the last tap back; a ring while a tap is computed (the photo stays usable).
class _TapTools extends StatelessWidget {
  const _TapTools({required this.lc});
  final LocalController lc;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    // two 48 dp halves on one 36 dp track (the track is drawn behind, the targets are the full halves)
    Widget seg(bool add) {
      final on = lc.tapAdd == add;
      return Semantics(
        button: true,
        selected: on,
        label: add ? '加上' : '去掉',
        excludeSemantics: true,
        child: GestureDetector(
          key: ValueKey(add ? 'tap-add' : 'tap-sub'),
          behavior: HitTestBehavior.opaque,
          onTap: () => lc.setTapAdd(add),
          child: HitBox(
            child: Container(
              width: 44,
              height: 32,
              alignment: Alignment.center,
              decoration: BoxDecoration(color: on ? c.accentBtn : null, borderRadius: BorderRadius.circular(Rd.cardLg)),
              child: DIcon(add ? DI.plus : DI.minus, size: 20, color: on ? c.onAccent : c.text),
            ),
          ),
        ),
      );
    }

    final hasTaps = (lc.comp?.ref ?? '').isNotEmpty;
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Stack(
          alignment: Alignment.center,
          children: [
            Container(width: 2 * 44 + 4, height: 36, decoration: BoxDecoration(color: c.bg, borderRadius: BorderRadius.circular(Rd.chip))),
            Row(mainAxisSize: MainAxisSize.min, children: [seg(true), seg(false)]),
          ],
        ),
        Semantics(
          button: true,
          enabled: hasTaps,
          label: '撤销上一下',
          excludeSemantics: true,
          child: GestureDetector(
            key: const ValueKey('tap-undo'),
            behavior: HitTestBehavior.opaque,
            onTap: hasTaps ? lc.undoTap : null,
            child: SizedBox(width: kHit, height: kHit, child: Center(child: DIcon(DI.undo, size: 20, color: hasTaps ? c.text : c.text2.withValues(alpha: 0.4)))),
          ),
        ),
        SizedBox(
          width: 28,
          child: lc.tapBusy
              ? SizedBox(
                  key: const ValueKey('tap-busy'),
                  width: 20,
                  height: 20,
                  child: CircularProgressIndicator(strokeWidth: 2.5, color: c.accent),
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

/// Press and hold: the photo without the local areas, released: back. One control for every area (the photo's own long-press belongs to
/// the handles while 局部 is open).
class CompareButton extends StatelessWidget {
  const CompareButton({super.key, required this.lc});
  final LocalController lc;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      button: true,
      label: '按住对比',
      excludeSemantics: true,
      onTap: () {
        lc.setComparing(true);
        Future<void>.delayed(Mo.peek, () => lc.setComparing(false));
      },
      child: Listener(
        key: const ValueKey('local-compare'),
        behavior: HitTestBehavior.opaque,
        onPointerDown: (_) => lc.setComparing(true),
        onPointerUp: (_) => lc.setComparing(false),
        onPointerCancel: (_) => lc.setComparing(false),
        child: HitBox(
          child: Container(
            height: 32,
            padding: const EdgeInsets.symmetric(horizontal: Sp.m),
            alignment: Alignment.center,
            decoration: BoxDecoration(color: lc.comparing ? c.accentBtn : c.bg, borderRadius: BorderRadius.circular(Rd.cardLg)),
            child: Text('对比', maxLines: 1, style: T.cap(lc.comparing ? c.onAccent : c.text, w: FontWeight.w500)),
          ),
        ),
      ),
    );
  }
}

/// The simple portrait panel: the strip of the selected chip on the left (bottom 0, middle = the recommended default, top = the recipe's
/// maximum) and the chips; a face selector only when several frontal faces exist. 分项 shows the chip's parameters.
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
      return Center(child: Text('没找到人', key: const ValueKey('portrait-none'), style: T.body(c.text2)));
    }
    final chips = lc.portraitChips;
    final selChip = lc.selChip;
    final faces = lc.frontalFaces;
    final ts = MediaQuery.textScalerOf(context).scale(12) / 12;
    Widget chip(PortraitChip pc, double w, double h) {
      final on = lc.chipOn(pc), selected = on && selChip == pc;
      final spec = specOf(pc);
      return Semantics(
        button: true,
        toggled: on,
        selected: selected,
        label: spec.label,
        hint: selected ? '再点一次去掉' : null,
        excludeSemantics: true,
        child: GestureDetector(
          key: ValueKey('chip-${pc.name}'),
          behavior: HitTestBehavior.opaque,
          onTap: () => lc.tapChip(pc),
          child: Container(
            width: w,
            height: h,
            decoration: BoxDecoration(
              color: on ? c.accentSoft : c.bg,
              borderRadius: BorderRadius.circular(Rd.block),
              border: selected ? Border.all(color: c.accent, width: 1.5) : null,
            ),
            child: Column(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                // a short tile (large text, landscape) keeps the word at its size and gives up icon size
                DIcon(iconOf(pc), size: (h - 8 - 16 * ts).clamp(12.0, 22 * ts).toDouble(), color: on ? c.accent : c.text2),
                const SizedBox(height: 2),
                Text(spec.label, maxLines: 1, style: T.cap(on ? c.accent : c.text2, w: on ? FontWeight.w600 : FontWeight.w400)),
              ],
            ),
          ),
        ),
      );
    }

    return Row(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        StrengthStrip(key: ValueKey('portrait-strip-${lc.selId}'), ctl: ctl, cat: PadCat.light, source: lc.lightPad, label: selChip == null ? '强度' : '${specOf(selChip).label}强度'),
        const SizedBox(width: Sp.m),
        Expanded(
          child: LayoutBuilder(
            builder: (context, cons) {
              // every chip in view: four to a row (seven chips = two rows); the tiles shrink with the panel down to their 48 dp target,
              // never below it (a narrower panel takes fewer columns instead)
              const gap = Sp.s;
              final fit = ((cons.maxWidth + gap) / (kHit + gap)).floor();
              final cols = math.min(math.min(4, fit), math.max(1, chips.length));
              final rows = (chips.length / cols).ceil();
              final faceRow = faces.length > 1 && selChip != null ? kHit + Sp.s : 0.0;
              final w = ((cons.maxWidth - gap * (cols - 1)) / cols).clamp(kHit, 80.0).toDouble();
              final h = ((cons.maxHeight - faceRow - gap * (rows - 1)) / rows).clamp(kHit, 60.0 * ts).toDouble();
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

/// 肤质's tool: 点一下祛斑 (taps on the photo add / remove heal spots; rings show while it is on) and the spot size.
class _HealTools extends StatelessWidget {
  const _HealTools({required this.lc});
  final LocalController lc;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Semantics(
          button: true,
          toggled: lc.healMode,
          label: '点一下祛斑',
          excludeSemantics: true,
          child: GestureDetector(
            key: const ValueKey('heal-mode'),
            behavior: HitTestBehavior.opaque,
            onTap: () => lc.setHealMode(!lc.healMode),
            child: HitBox(
              child: Container(
                height: 32,
                padding: const EdgeInsets.symmetric(horizontal: Sp.m),
                alignment: Alignment.center,
                decoration: BoxDecoration(color: lc.healMode ? c.accentBtn : c.bg, borderRadius: BorderRadius.circular(Rd.cardLg)),
                child: Text('点一下祛斑', style: T.cap(lc.healMode ? c.onAccent : c.text, w: FontWeight.w500)),
              ),
            ),
          ),
        ),
        if (lc.healMode) ...[
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
      ],
    );
  }
}
