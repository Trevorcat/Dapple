// The 局部 panel (docs/design/local-adjustments.md sections 3, 4, 5 and 7): the header with the area chips, the body (the strip and
// the two pads, the itemised view, the shape card, the area's actions) and the tool strip that floats on the photo's bottom edge for
// the "where" of brushes and ranges. No scrolling lists, no second-level menus: a card replaces the body in place and closes itself.
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../../model/local_adjust.dart';
import '../../model/local_kinds.dart';
import '../../model/local_pad.dart';
import '../../model/pad_model.dart';
import '../../services/local_ai.dart' show AiPlane;
import '../../theme/tokens.dart';
import 'editor_controller.dart';
import 'itemised_view.dart';
import 'local_controller.dart';
import 'local_glyphs.dart';
import 'pad_gestures.dart';
import 'pad_panel.dart' show HeaderButton, PanelChip, headerH;
import 'pad_widgets.dart';

// ------------------------------------------------------------------ header

class LocalHeader extends StatelessWidget {
  const LocalHeader({super.key, required this.ctl, required this.side, required this.textScale});
  final EditorController ctl;
  final bool side;
  final double textScale;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final lc = ctl.local;
    final a = lc.sel;
    final title = switch (lc.body) {
      LocalBody.addPart => '加一块',
      LocalBody.subPart => '减一块',
      _ => '局部',
    };
    final inCard = lc.body != LocalBody.pads && (!lc.list.isEmpty || lc.body == LocalBody.aiParts);
    final parts = lc.body == LocalBody.aiParts && lc.personFound && lc.aiRunning == 0;
    final buttons = <Widget>[
      if (inCard)
        HeaderButton(key: const ValueKey('local-back'), label: '返回', onTap: () {
          if (lc.body == LocalBody.aiParts) lc.cancelAnalysis();
          lc.showBody(LocalBody.pads);
        }),
      // the parts card's one action sits where the eye goes for "done" (the chips keep the whole body)
      if (parts)
        Opacity(
          opacity: lc.partPick.isEmpty ? 0.38 : 1,
          child: HeaderButton(
            key: const ValueKey('parts-add'),
            label: lc.partMode == LocalBody.subPart ? '减去' : '添加',
            pressed: lc.partPick.isNotEmpty,
            onTap: lc.addPickedParts,
          ),
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
              child: HeaderButton(key: const ValueKey('reset-local'), label: '复位', icon: Icons.refresh, semantics: '复位这个区域的调整', onTap: lc.resetOps),
            );
          },
        ),
        HeaderButton(key: const ValueKey('items-button'), label: '分项', pressed: ctl.itemised, onTap: () => ctl.setItemised(!ctl.itemised)),
      ],
    ];
    // the side pane's second line holds the chips (26 dp): 4 dp more than the readout line of the other categories; the body gives them up
    final h = headerH(textScale, side: side) + (side ? 4 : 0);
    final titleStyle = T.body(c.text, w: FontWeight.w600);
    final chips = LocalChips(lc: lc, compact: side);
    return SizedBox(
      height: h,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 14),
        child: side
            ? Column(mainAxisAlignment: MainAxisAlignment.center, crossAxisAlignment: CrossAxisAlignment.start, children: [
                Row(children: [Text(title, key: const ValueKey('header-title'), style: titleStyle), const Spacer(), for (final b in buttons) ...[const SizedBox(width: 6), b]]),
                SizedBox(height: 26, child: chips),
              ])
            : Row(children: [
                Text(title, key: const ValueKey('header-title'), style: titleStyle),
                const SizedBox(width: 10),
                Expanded(child: chips),
                for (final b in buttons) ...[const SizedBox(width: 6), b],
              ]),
      ),
    );
  }
}

/// [+] and one chip per area: the kind's glyph, a small "+" when the area has several parts, a half-filled corner when it is
/// inverted. Tap selects; a second tap on the selected chip shows its actions; holding a chip shows its mask.
class LocalChips extends StatelessWidget {
  const LocalChips({super.key, required this.lc, this.compact = false});
  final LocalController lc;
  final bool compact;

  @override
  Widget build(BuildContext context) {
    final items = lc.list.items;
    return LayoutBuilder(builder: (context, cons) {
      final n = items.length + 1;
      final full = compact ? 26.0 : 32.0;
      // shrink to fit (tighter gaps first), never below 26; beyond that (16 areas on a narrow screen) the row scrolls
      final gap = full * n + 6.0 * (n - 1) <= cons.maxWidth ? 6.0 : 4.0;
      final side = ((cons.maxWidth - gap * (n - 1)) / n).clamp(compact ? 24.0 : 26.0, full).toDouble();
      final row = Row(mainAxisSize: MainAxisSize.min, children: [
        _AddChip(lc: lc, size: side),
        for (var i = 0; i < items.length; i++) ...[SizedBox(width: gap), _AreaChip(lc: lc, index: i, size: side)],
      ]);
      final fits = side * n + gap * (n - 1) <= cons.maxWidth + 0.5;
      return Align(
        alignment: Alignment.centerLeft,
        child: fits ? row : SingleChildScrollView(scrollDirection: Axis.horizontal, child: row),
      );
    });
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
      label: '新区域',
      excludeSemantics: true,
      onTap: tap,
      child: GestureDetector(
        key: const ValueKey('local-add'),
        behavior: HitTestBehavior.opaque,
        onTap: tap,
        child: Opacity(
          opacity: full ? 0.38 : 1,
          child: Container(
            width: size,
            height: size,
            decoration: BoxDecoration(
              color: open ? c.accentSoft : c.chip,
              borderRadius: BorderRadius.circular(size * 0.3),
              border: open ? Border.all(color: c.accent, width: 1.5) : null,
            ),
            child: Icon(Icons.add, size: size * 0.6, color: open ? c.accent : c.text),
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
    final selected = a.id == lc.selId;
    final glyph = a.mask.isEmpty ? LocalGlyph.raster : kindOfComponent(a.mask.first).glyph;
    final fg = selected ? c.accent : c.text;
    final label = '区域 ${index + 1}，${a.mask.isEmpty ? '' : kindOfComponent(a.mask.first).label}${a.mask.length > 1 ? '，${a.mask.length} 块' : ''}${a.invert ? '，反选' : ''}';
    return Semantics(
      button: true,
      selected: selected,
      label: label,
      hint: selected ? '再点一次显示操作，按住查看范围' : null,
      excludeSemantics: true,
      onTap: () => lc.select(a.id),
      child: GestureDetector(
        key: ValueKey('local-chip-$index'),
        behavior: HitTestBehavior.opaque,
        onTap: () => lc.select(a.id),
        onLongPressStart: (_) {
          if (!selected) lc.select(a.id);
          lc.setPeek(true);
        },
        onLongPressEnd: (_) => lc.setPeek(false),
        onLongPressCancel: () => lc.setPeek(false),
        child: Opacity(
          opacity: a.on ? 1 : 0.4,
          child: Container(
            width: size,
            height: size,
            decoration: BoxDecoration(
              color: selected ? c.accentSoft : c.chip,
              borderRadius: BorderRadius.circular(size * 0.3),
              border: selected ? Border.all(color: c.accent, width: 1.5) : null,
            ),
            child: Stack(children: [
              Center(child: LocalGlyphIcon(glyph, size: size * 0.62, color: fg)),
              if (a.mask.length > 1)
                Positioned(right: 1.5, bottom: 1, child: Text('+', style: TextStyle(fontSize: size * 0.32, height: 1, fontWeight: FontWeight.w700, color: fg))),
              if (a.invert)
                Positioned(
                  left: 3,
                  bottom: 3,
                  child: Container(
                    width: size * 0.22,
                    height: size * 0.22,
                    decoration: BoxDecoration(shape: BoxShape.circle, border: Border.all(color: fg, width: 1), gradient: LinearGradient(colors: [fg, fg, Colors.transparent, Colors.transparent], stops: const [0, 0.5, 0.5, 1])),
                  ),
                ),
            ]),
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
    if (lc.list.isEmpty || lc.body == LocalBody.addArea) return ShapeCard(lc: lc, mode: LocalBody.addArea);
    switch (lc.body) {
      case LocalBody.addPart:
      case LocalBody.subPart:
        return ShapeCard(lc: lc, mode: lc.body);
      case LocalBody.actions:
        return AreaActions(lc: lc);
      case LocalBody.aiParts:
        return AiPartsCard(lc: lc);
      case LocalBody.pads:
      case LocalBody.addArea:
        break;
    }
    if (ctl.itemised) return LocalItemised(ctl: ctl);
    final strip = StrengthStrip(ctl: ctl, cat: PadCat.light, source: lc.lightPad, label: '区域强度');
    final light = PadFace(key: const ValueKey('local-pad-light'), ctl: ctl, cat: PadCat.light, source: lc.lightPad);
    final color = PadFace(key: const ValueKey('local-pad-color'), ctl: ctl, cat: PadCat.color, source: lc.colorPad);
    return Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      strip,
      const SizedBox(width: 10),
      // the two pads side by side or stacked, whichever keeps them closer to square (a squat pad crowds its edge icons)
      Expanded(
        child: LayoutBuilder(builder: (context, cons) {
          double off(double a) => (math.log(a)).abs();
          final row = off(((cons.maxWidth - 8) / 2) / cons.maxHeight), col = off(cons.maxWidth / ((cons.maxHeight - 8) / 2));
          return col < row
              ? Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [Expanded(child: light), const SizedBox(height: 8), Expanded(child: color)])
              : Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [Expanded(child: light), const SizedBox(width: 8), Expanded(child: color)]);
        }),
      ),
    ]);
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
      [for (final k in kinds) if (!k.ai) k],
      [for (final k in kinds) if (k.ai) k],
    ]..removeWhere((r) => r.isEmpty);
    return LayoutBuilder(builder: (context, cons) {
      final perRow = rows.fold<int>(1, (n, r) => math.max(n, r.length));
      final tileW = math.min(84.0, (cons.maxWidth - 8 * (perRow - 1)) / perRow);
      final tileH = math.max(56.0, math.min(84.0, (cons.maxHeight - 8 * (rows.length - 1)) / rows.length));
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
              decoration: BoxDecoration(color: c.bg, borderRadius: BorderRadius.circular(16)),
              child: Column(mainAxisAlignment: MainAxisAlignment.center, children: [
                SizedBox(
                  width: 36,
                  height: 32,
                  child: Stack(alignment: Alignment.center, children: [
                    Opacity(opacity: running ? 0.35 : 1, child: LocalGlyphIcon(k.glyph, size: 28, color: c.text)),
                    if (running)
                      SizedBox(
                        key: ValueKey('kind-progress-${k.id}'),
                        width: 26,
                        height: 26,
                        child: CircularProgressIndicator(value: lc.aiProgress <= 0 ? null : lc.aiProgress, strokeWidth: 2.5, color: c.accent, backgroundColor: c.track),
                      ),
                  ]),
                ),
                const SizedBox(height: 4),
                Text(k.label, maxLines: 1, style: T.cap(c.text2)),
                if (k.note != null) Text(k.note!, maxLines: 1, style: T.mini(c.text2.withValues(alpha: 0.7))),
              ]),
            ),
          ),
        );
      }

      return Center(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          for (var i = 0; i < rows.length; i++) ...[
            if (i > 0) const SizedBox(height: 8),
            Row(mainAxisAlignment: MainAxisAlignment.center, children: [
              for (var j = 0; j < rows[i].length; j++) ...[if (j > 0) const SizedBox(width: 8), tile(rows[i][j])],
            ]),
          ],
        ]),
      );
    });
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
        child: Row(mainAxisSize: MainAxisSize.min, children: [
          SizedBox(
            key: const ValueKey('parts-progress'),
            width: 28,
            height: 28,
            child: CircularProgressIndicator(value: lc.aiProgress <= 0 ? null : lc.aiProgress, strokeWidth: 3, color: c.accent, backgroundColor: c.track),
          ),
          const SizedBox(width: 12),
          Text('正在找人', style: T.body(c.text2)),
          const SizedBox(width: 16),
          HeaderButton(key: const ValueKey('parts-cancel'), label: '取消', onTap: () {
            lc.cancelAnalysis();
            lc.showBody(lc.list.isEmpty ? LocalBody.addArea : LocalBody.pads);
          }),
        ]),
      );
    }
    if ((lc.aiDone & AiBits.person) != 0 && !lc.personFound) {
      return Center(child: Text('没找到人', key: const ValueKey('parts-none'), style: T.body(c.text2)));
    }
    final planes = lc.partPlanes;
    final people = [for (final p in planes) if (p.face == 0) p];
    final byFace = <int, List<AiPlane>>{};
    for (final p in planes) {
      if (p.face > 0) byFace.putIfAbsent(p.face, () => []).add(p);
    }
    Widget chip(AiPlane p) {
      final on = lc.partPick.contains(p.key);
      // a chip as wide as its word (PanelChip fills a loose width)
      return IntrinsicWidth(child: PanelChip(key: ValueKey('part-${p.key}'), label: partLabel(p.key) ?? p.label, selected: on, onTap: () => lc.togglePart(p.key)));
    }

    final rows = <Widget>[
      Wrap(spacing: 6, runSpacing: 6, children: [for (final p in people) chip(p)]),
      for (final e in byFace.entries)
        Padding(
          padding: const EdgeInsets.only(top: 8),
          child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Padding(
              padding: const EdgeInsets.only(top: 9, right: 6),
              child: Text(byFace.length > 1 ? '脸 ${e.key}' : '脸', style: T.cap(c.text2)),
            ),
            Expanded(child: Wrap(spacing: 6, runSpacing: 6, children: [for (final p in e.value) chip(p)])),
          ]),
        ),
    ];
    return SingleChildScrollView(child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: rows));
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
    final acts = <(String, IconData, VoidCallback, bool)>[
      if (lc.selIsAi) ...[
        ('涂抹补充', Icons.brush_outlined, () => lc.brushFix(add: true), false),
        ('涂抹擦除', Icons.auto_fix_off_outlined, () => lc.brushFix(add: false), false),
      ],
      ('加一块', Icons.add_circle_outline, () => full ? lc.ctl.app.showToast('每个区域最多 $kMaxMaskComponents 块') : lc.showBody(LocalBody.addPart), false),
      ('减一块', Icons.remove_circle_outline, () => full ? lc.ctl.app.showToast('每个区域最多 $kMaxMaskComponents 块') : lc.showBody(LocalBody.subPart), false),
      ('反选', Icons.flip_outlined, lc.invertArea, false),
      ('复制', Icons.content_copy_outlined, lc.duplicateArea, false),
      // the selected part (chosen on the tool strip) goes; the area stays
      if (a != null && a.mask.length > 1) ('去掉这块', Icons.layers_clear_outlined, () => lc.removePart(lc.selComp), false),
      ('删除', Icons.delete_outline, lc.deleteArea, true),
    ];
    return LayoutBuilder(builder: (context, cons) {
      final perRow = acts.length > 5 ? (acts.length / 2).ceil() : acts.length;
      final w = math.min(84.0, (cons.maxWidth - 8 * (perRow - 1)) / perRow);
      return Center(
        child: Wrap(spacing: 8, runSpacing: 8, alignment: WrapAlignment.center, children: [
          for (final x in acts)
            Semantics(
              button: true,
              label: x.$1,
              excludeSemantics: true,
              child: GestureDetector(
                key: ValueKey('act-${x.$1}'),
                behavior: HitTestBehavior.opaque,
                onTap: x.$3,
                child: Container(
                  width: w,
                  height: 72 + math.max(0.0, MediaQuery.textScalerOf(context).scale(12) / 12 - 1) * 26,
                  decoration: BoxDecoration(color: c.bg, borderRadius: BorderRadius.circular(16)),
                  child: Column(mainAxisAlignment: MainAxisAlignment.center, children: [
                    Icon(x.$2, size: 24, color: x.$4 ? c.danger : c.text),
                    const SizedBox(height: 6),
                    Text(x.$1, style: T.cap(x.$4 ? c.danger : c.text2)),
                  ]),
                ),
              ),
            ),
        ]),
      );
    });
  }
}

/// The itemised view of the selected area: three group chips, the radar of that group, one ruler.
class LocalItemised extends StatelessWidget {
  const LocalItemised({super.key, required this.ctl});
  final EditorController ctl;

  @override
  Widget build(BuildContext context) {
    final lc = ctl.local;
    final items = lc.itemsOf(lc.itemGroup);
    final sel = ctl.selItem.clamp(0, items.length - 1);
    const groups = [('light', '光线'), ('color', '颜色'), ('detail', '质地')];
    return Column(children: [
      Row(children: [
        for (var i = 0; i < groups.length; i++) ...[
          if (i > 0) const SizedBox(width: 6),
          Expanded(child: PanelChip(key: ValueKey('local-group-${groups[i].$1}'), label: groups[i].$2, selected: lc.itemGroup == groups[i].$1, compact: true, onTap: () => lc.setItemGroup(groups[i].$1))),
        ],
      ]),
      const SizedBox(height: 4),
      Expanded(child: RadarView(key: ValueKey('local-radar-${lc.itemGroup}'), ctl: ctl, cat: PadCat.light, items: items, selected: sel, onSelect: ctl.selectItem, source: lc.items)),
      const SizedBox(height: 6),
      ItemRuler(ctl: ctl, cat: PadCat.light, item: items[sel], source: lc.items),
    ]);
  }
}

// ------------------------------------------------------------------ the dock glyph

/// The dock's 局部 glyph: the selected (or last) area's kind, a count dot when there are several.
class LocalDockGlyphPainter extends CustomPainter {
  const LocalDockGlyphPainter(this.glyph, this.count, this.color);
  final LocalGlyph glyph;
  final int count;
  final Color color;

  @override
  void paint(Canvas canvas, Size size) {
    const g = 24.0;
    final box = Rect.fromLTWH(1, (size.height - g) / 2, g, g);
    paintLocalGlyph(canvas, box, glyph, color, stroke: 1.5);
    if (count > 1) {
      final tp = TextPainter(text: TextSpan(text: '$count', style: TextStyle(fontSize: 9, height: 1, fontWeight: FontWeight.w700, color: color)), textDirection: TextDirection.ltr)..layout();
      tp.paint(canvas, Offset(box.right + 2, box.top - 1));
    }
  }

  @override
  bool shouldRepaint(LocalDockGlyphPainter o) => o.glyph != glyph || o.count != count || o.color != color;
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
        if (!lc.active || a == null || m == null || lc.body != LocalBody.pads || ctl.compare.value) return const SizedBox.shrink();
        final c = context.c;
        final parts = a.mask.length > 1;
        final children = <Widget>[];
        if (parts) {
          for (var i = 0; i < a.mask.length; i++) {
            children.add(_PartChip(lc: lc, index: i));
            children.add(const SizedBox(width: 4));
          }
        }
        Widget? tool;
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
            } else if (k == 'tap') {
              tool = _TapTools(lc: lc);
            } else if (k == 'depth') {
              tool = Row(mainAxisSize: MainAxisSize.min, children: [
                Text('近', style: T.mini(c.text2)),
                const SizedBox(width: 4),
                _RangeBar(lc: lc, hue: false, depth: true),
                const SizedBox(width: 4),
                Text('远', style: T.mini(c.text2)),
              ]);
            } else {
              tool = null;
            }
          default:
            tool = null;
        }
        if (tool == null && !parts) return const SizedBox.shrink();
        if (tool != null) {
          if (parts) children.add(Container(width: 1, height: 24, color: c.sep, margin: const EdgeInsets.symmetric(horizontal: 6)));
          children.add(tool);
        }
        return Center(
          child: Container(
            key: const ValueKey('local-tools'),
            height: 48,
            padding: const EdgeInsets.symmetric(horizontal: 8),
            decoration: BoxDecoration(
              color: c.surface,
              borderRadius: BorderRadius.circular(24),
              boxShadow: const [BoxShadow(color: Color(0x33000000), blurRadius: 12, offset: Offset(0, 3))],
            ),
            // several parts plus the brush tools can exceed a narrow photo: the strip scales down rather than overflow
            child: FittedBox(fit: BoxFit.scaleDown, child: Row(mainAxisSize: MainAxisSize.min, children: children)),
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
      child: Row(mainAxisSize: MainAxisSize.min, children: [
        GestureDetector(
          key: ValueKey('local-part-$index'),
          behavior: HitTestBehavior.opaque,
          onTap: () => lc.selectPart(index),
          child: Container(
            width: 36,
            height: 36,
            decoration: BoxDecoration(color: selected ? c.accentSoft : c.chip, shape: BoxShape.circle, border: selected ? Border.all(color: c.accent, width: 1.5) : null),
            child: Stack(children: [
              Center(child: LocalGlyphIcon(k.glyph, size: 20, color: fg)),
              if (sign.isNotEmpty) Positioned(left: 4, top: 2, child: Text(sign, style: TextStyle(fontSize: 11, height: 1, fontWeight: FontWeight.w700, color: fg))),
            ]),
          ),
        ),
      ]),
    );
  }
}

class _BrushTools extends StatelessWidget {
  const _BrushTools({required this.lc});
  final LocalController lc;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Row(mainAxisSize: MainAxisSize.min, children: [
      for (var i = 0; i < LocalController.brushRadiiDp.length; i++)
        Semantics(
          button: true,
          selected: lc.brushSize == i,
          label: const ['细', '中', '粗', '很粗'][i],
          excludeSemantics: true,
          child: GestureDetector(
            key: ValueKey('brush-size-$i'),
            behavior: HitTestBehavior.opaque,
            onTap: () => lc.setBrushSize(i),
            child: SizedBox(
              width: 40,
              height: 44,
              child: Center(
                child: Container(
                  width: 6.0 + i * 5,
                  height: 6.0 + i * 5,
                  decoration: BoxDecoration(
                    shape: BoxShape.circle,
                    color: lc.brushSize == i ? c.accent : c.text2.withValues(alpha: 0.55),
                  ),
                ),
              ),
            ),
          ),
        ),
      Container(width: 1, height: 24, color: c.sep, margin: const EdgeInsets.symmetric(horizontal: 4)),
      Semantics(
        button: true,
        toggled: lc.erase,
        label: '擦除',
        excludeSemantics: true,
        child: GestureDetector(
          key: const ValueKey('brush-erase'),
          behavior: HitTestBehavior.opaque,
          onTap: () => lc.setErase(!lc.erase),
          child: Container(
            width: 40,
            height: 32,
            alignment: Alignment.center,
            decoration: BoxDecoration(color: lc.erase ? c.accent : null, borderRadius: BorderRadius.circular(16)),
            child: CustomPaint(size: const Size(20, 20), painter: _EraserPainter(lc.erase ? Colors.white : c.text)),
          ),
        ),
      ),
    ]);
  }
}

class _EraserPainter extends CustomPainter {
  const _EraserPainter(this.color);
  final Color color;
  @override
  void paint(Canvas canvas, Size s) {
    canvas.save();
    canvas.translate(s.width / 2, s.height / 2);
    canvas.rotate(-math.pi / 4);
    final r = Rect.fromCenter(center: Offset.zero, width: s.width * 0.9, height: s.height * 0.45);
    final rr = RRect.fromRectAndRadius(r, const Radius.circular(3));
    canvas.drawRRect(rr, Paint()..style = PaintingStyle.stroke..strokeWidth = 1.5..color = color);
    canvas.drawRRect(RRect.fromRectAndRadius(Rect.fromLTRB(r.left, r.top, r.left + r.width * 0.4, r.bottom), const Radius.circular(3)), Paint()..color = color);
    canvas.restore();
  }

  @override
  bool shouldRepaint(_EraserPainter o) => o.color != color;
}

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
        height: 44,
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
      final cols = depth ? const [Color(0xFF6B4A33), Color(0xFFB9A58F), Color(0xFFCFDCE8)] : const [Color(0xFF0B0B0C), Color(0xFFF7F7F5)];
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
          canvas.drawRect(fadeL, Paint()
            ..blendMode = BlendMode.dstOut
            ..shader = const LinearGradient(colors: [Color(0x00000000), Color(0xFF000000)]).createShader(fadeL));
          canvas.drawRect(fadeR, Paint()
            ..blendMode = BlendMode.dstOut
            ..shader = const LinearGradient(colors: [Color(0xFF000000), Color(0x00000000)]).createShader(fadeR));
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
    canvas.drawRRect(rr, Paint()..style = PaintingStyle.stroke..strokeWidth = 1..color = const Color(0x33000000));
    // the two end handles
    for (final t in [a, b]) {
      var x = (t % 1 + 1) % 1 * w;
      if (!hue) x = t.clamp(0.0, 1.0) * w;
      final r = Rect.fromCenter(center: Offset(x, size.height / 2), width: 6, height: h + 8);
      canvas.drawRRect(RRect.fromRectAndRadius(r.inflate(1), const Radius.circular(4)), Paint()..color = const Color(0x59000000));
      canvas.drawRRect(RRect.fromRectAndRadius(r, const Radius.circular(3)), Paint()..color = Colors.white);
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
    Widget seg(bool add) {
      final on = lc.tapAdd == add;
      return Semantics(
        button: true,
        selected: on,
        label: add ? '点一下加上' : '点一下去掉',
        excludeSemantics: true,
        child: GestureDetector(
          key: ValueKey(add ? 'tap-add' : 'tap-sub'),
          behavior: HitTestBehavior.opaque,
          onTap: () => lc.setTapAdd(add),
          child: Container(
            width: 44,
            height: 32,
            alignment: Alignment.center,
            decoration: BoxDecoration(color: on ? c.accent : null, borderRadius: BorderRadius.circular(16)),
            child: Icon(add ? Icons.add : Icons.remove, size: 20, color: on ? Colors.white : c.text),
          ),
        ),
      );
    }

    final hasTaps = (lc.comp?.ref ?? '').isNotEmpty;
    return Row(mainAxisSize: MainAxisSize.min, children: [
      Container(
        padding: const EdgeInsets.all(2),
        decoration: BoxDecoration(color: c.bg, borderRadius: BorderRadius.circular(18)),
        child: Row(mainAxisSize: MainAxisSize.min, children: [seg(true), seg(false)]),
      ),
      const SizedBox(width: 8),
      Semantics(
        button: true,
        enabled: hasTaps,
        label: '撤销上一下',
        excludeSemantics: true,
        child: GestureDetector(
          key: const ValueKey('tap-undo'),
          behavior: HitTestBehavior.opaque,
          onTap: hasTaps ? lc.undoTap : null,
          child: SizedBox(width: 40, height: 44, child: Icon(Icons.undo, size: 20, color: hasTaps ? c.text : c.text2.withValues(alpha: 0.4))),
        ),
      ),
      SizedBox(
        width: 28,
        child: lc.tapBusy
            ? SizedBox(key: const ValueKey('tap-busy'), width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2.5, color: c.accent))
            : const SizedBox.shrink(),
      ),
    ]);
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
      return Row(mainAxisSize: MainAxisSize.min, children: [
        SizedBox(width: 18, height: 18, child: CircularProgressIndicator(strokeWidth: 2.5, color: c.accent)),
        const SizedBox(width: 8),
        Text('正在重新分析', style: T.cap(c.text2)),
        const SizedBox(width: 6),
      ]);
    }
    return HeaderButton(key: const ValueKey('ai-reanalyse'), label: '重新分析', icon: null, onTap: lc.reanalyse);
  }
}
