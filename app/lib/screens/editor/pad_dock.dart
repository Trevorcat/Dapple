// The category dock (guidelines 15.2.2): five items, 光线 颜色 成像 质地 局部, each a static line icon over its word. The same item
// ([DockItem]) stands in every layout: the phone's dock, the fold column's row and the one-thumb cluster's rail. A category changed from
// its default carries a small accent dot at the icon's corner; 局部 shows its number of areas there instead. Nothing on the item moves
// while a value is dragged. A tap opens the panel; on the phone's dock a press-and-hold (or starting to drag on the item) pops the pad
// up above the dock and the drag moves the value relative to where it started; releasing closes the popup. 裁切 is not a category:
// its entry is the top bar's crop button (editor_overlays.dart).
import 'dart:async';

import 'package:flutter/material.dart';

import '../../model/pad_model.dart';
import '../../model/params.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart' show kHit;
import '../../widgets/float_bar.dart' show capsuleH, kFloatBarH, kFloatBarRadius;
import '../../widgets/icons.dart';
import 'editor_controller.dart';
import 'editor_overlays.dart' show GlassCapsule;
import '../../model/local_pad.dart' show localStripPercent, localUiOf;
import 'pad_widgets.dart';

/// The dock's categories, in order.
const List<EGroup> padDockOrder = [EGroup.light, EGroup.color, EGroup.style, EGroup.effects, EGroup.local];

/// The word of a category (裁切 included: the cluster's ball names the open tool).
String dockLabel(EGroup g) => g == EGroup.crop ? '裁切' : (g == EGroup.local ? '局部' : PadCatX.ofGroup(g)!.label);

/// The icon of a dock category (guidelines 8): sun, drop, three circles, a square of grain from fine to coarse, a frame with one part
/// marked.
DI dockIcon(EGroup g) => switch (g) {
      EGroup.light => DI.light,
      EGroup.color => DI.color,
      EGroup.style => DI.styles,
      EGroup.effects => DI.texture,
      EGroup.local => DI.local,
      _ => DI.crop,
    };

/// Hold time before the popup appears (a drag beyond [kHoldSlop] starts it at once).
const Duration kHoldDelay = Mo.hold;
const double kHoldSlop = 8;

class PadDock extends StatelessWidget {
  const PadDock({super.key, required this.ctl});
  final EditorController ctl;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: Listenable.merge([ctl.wm, ctl.app.editTick, ctl.hold, ctl.local]),
      builder: (context, _) => LayoutBuilder(builder: (context, cons) {
        final n = padDockOrder.length;
        // five items of at least 48 dp (the touch target, guidelines 6); a still narrower window scrolls the dock (guidelines 15.2.2)
        // rather than shrinking the items
        final iw = (cons.maxWidth / n).clamp(kHit, 76.0).toDouble();
        final dh = padDockH(MediaQuery.textScalerOf(context).scale(15) / 15);
        final row = SizedBox(
          width: iw * n,
          height: dh,
          child: Row(children: [
            for (final g in padDockOrder) SizedBox(width: iw, height: dh, child: DockItem(key: ValueKey('dock-$g'), ctl: ctl, group: g)),
          ]),
        );
        return GlassCapsule(
          radius: kFloatBarRadius,
          child: iw * n <= cons.maxWidth + 0.5 ? row : SizedBox(width: cons.maxWidth, child: SingleChildScrollView(scrollDirection: Axis.horizontal, child: row)),
        );
      }),
    );
  }
}

/// Why a greyed dock category has nothing to show right now and what to do, one plain line; null when the category is not greyed
/// (nothing is said then).
String? dockDimReason(EditorController ctl, EGroup g) {
  if (g == EGroup.local) return ctl.edit.local.isEmpty ? '还没有区域，点「+」新建' : null;
  final pc = PadCatX.ofGroup(g);
  if (pc == null) return null;
  if (pc == PadCat.film && ctl.edit.film == null) return '还没有选成像，在面板里选一个';
  if (padViewOf(ctl.edit, pc, ctl.padCtx).a < 0.02) return '现在是 0，拖动盘面开始调整';
  return null;
}

class DockItem extends StatefulWidget {
  const DockItem({super.key, required this.ctl, required this.group, this.holdToAdjust = true, this.onTap});
  final EditorController ctl;
  final EGroup group;

  /// Called on every touch that ends as a tap, before the category opens (the cluster wakes up on it).
  final VoidCallback? onTap;

  /// False where the hold popup has no place to rise from (the fold layouts' column): a tap still opens the category.
  final bool holdToAdjust;

  @override
  State<DockItem> createState() => _DockItemState();
}

class _DockItemState extends State<DockItem> {
  Timer? _timer;
  Offset _start = Offset.zero, _startGlobal = Offset.zero;
  bool _down = false, _holding = false;
  int? _pointer;

  EditorController get ctl => widget.ctl;
  PadCat? get cat => PadCatX.ofGroup(widget.group);

  @override
  void dispose() {
    _timer?.cancel();
    if (_holding) ctl.holdEnd();
    super.dispose();
  }

  void _begin() {
    final c = cat;
    if (c == null || !_down || _holding || !widget.holdToAdjust) return;
    _holding = true;
    ctl.holdBegin(c, _startGlobal.dx);
  }

  void _onDown(PointerDownEvent e) {
    if (_pointer != null) return;
    _pointer = e.pointer;
    _down = true;
    _holding = false;
    _start = e.position;
    _startGlobal = e.position;
    _timer?.cancel();
    if (cat != null && widget.holdToAdjust) _timer = Timer(kHoldDelay, _begin);
  }

  void _onMove(PointerMoveEvent e) {
    if (e.pointer != _pointer) return;
    final d = e.position - _start;
    if (!_holding && cat != null && widget.holdToAdjust && d.distance > kHoldSlop) {
      _timer?.cancel();
      _begin();
    }
    if (_holding) ctl.holdMove(d.dx, d.dy);
  }

  void _onUp(PointerEvent e, {required bool cancelled}) {
    if (e.pointer != _pointer) return;
    _pointer = null;
    _timer?.cancel();
    final wasHolding = _holding;
    _down = false;
    _holding = false;
    if (wasHolding) {
      ctl.holdEnd();
    } else if (!cancelled) {
      widget.onTap?.call();
      final why = dockDimReason(ctl, widget.group);
      ctl.openCategory(widget.group);
      // after the switch: a tab change clears the previous page's toast
      if (why != null) ctl.app.showToast(why);
    }
  }

  @override
  Widget build(BuildContext context) {
    final g = widget.group;
    final selected = ctl.wm.windowOf(g) != null;
    final pressed = ctl.hold.value?.cat == cat && cat != null;
    final on = selected || pressed;
    final label = dockLabel(g);
    final ed = ctl.edit;
    final pc = cat;
    final PadState s = pc == null ? PadState.centre : padViewOf(ed, pc, ctl.padCtx);
    final local = g == EGroup.local;
    final areas = ed.local;
    final la = local && !areas.isEmpty ? (areas.byId(ctl.local.selId ?? '') ?? areas.items.last) : null;
    // greyed: nothing to show yet (strength 0, no stock chosen, no area); a tap still opens it and says why
    final dim = local ? areas.isEmpty : pc != null && (s.a < 0.02 || (pc == PadCat.film && ed.film == null));
    final adjusted = pc != null && padCatAdjusted(ed, pc, ctl.padCtx);
    return Semantics(
      selected: on,
      button: true,
      label: local ? '$label${areas.isEmpty ? '' : '，${areas.length} 个区域'}${la == null ? '' : '，强度 ${localStripPercent(la, localUiOf(la).a).round()}%'}' : (pc == null ? label : '$label，${padReadout(pc, s, ctx: ctl.padCtx)}${adjusted ? '，已调整' : ''}${widget.holdToAdjust ? '，按住拖动直接调整' : ''}'),
      excludeSemantics: true,
      onTap: () => ctl.openCategory(g),
      child: Listener(
        behavior: HitTestBehavior.opaque,
        onPointerDown: _onDown,
        onPointerMove: _onMove,
        onPointerUp: (e) => _onUp(e, cancelled: false),
        onPointerCancel: (e) => _onUp(e, cancelled: true),
        child: DockItemFace(
          icon: dockIcon(g),
          label: label,
          on: on,
          dim: dim,
          changed: adjusted,
          count: local && !areas.isEmpty ? areas.length : null,
          iconKey: ValueKey('glyph-${pc?.name ?? g.name}'),
          markKey: ValueKey('dock-mark-${pc?.name ?? g.name}'),
        ),
      ),
    );
  }
}

/// What a dock item shows: the icon over the word; selected = `accentSoft` ground and `accent` (guidelines 10.2); greyed = the icon at
/// 38 % (the word stays readable); at the icon's top right corner a 6 dp `accent` dot when the category is changed from its default, or
/// the number of areas ([count], 局部). Static: nothing in it follows a value while it is dragged.
class DockItemFace extends StatelessWidget {
  const DockItemFace({super.key, required this.icon, required this.label, this.on = false, this.dim = false, this.changed = false, this.count, this.iconSize = 24, this.iconKey, this.markKey});
  final DI icon;
  final String label;
  final bool on, dim, changed;
  final int? count;
  final double iconSize;
  final Key? iconKey, markKey;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final fg = on ? c.accent : c.text2;
    final k = iconSize / 24;
    Widget? mark;
    if (count != null) {
      mark = Text('$count', key: markKey, maxLines: 1, textScaler: TextScaler.noScaling,
          style: _scaled(T.mini(c.accent, w: FontWeight.w600), k).copyWith(height: 1, fontFeatures: const [FontFeature.tabularFigures()]));
    } else if (changed) {
      mark = Container(key: markKey, width: 6 * k, height: 6 * k, decoration: BoxDecoration(color: c.accent, shape: BoxShape.circle));
    }
    return Container(
      margin: const EdgeInsets.symmetric(horizontal: 2, vertical: 2),
      decoration: BoxDecoration(color: on ? c.accentSoft : null, borderRadius: BorderRadius.circular((kFloatBarH / 2 - 2) * k)),
      // the column sits 1 dp below the centre (10 dp above the icon, 8 below the label), as in the float bar
      child: Column(mainAxisAlignment: MainAxisAlignment.center, mainAxisSize: MainAxisSize.min, children: [
        SizedBox(height: 2 * k),
        SizedBox(
          width: iconSize,
          height: iconSize,
          child: Stack(clipBehavior: Clip.none, children: [
            Opacity(opacity: dim && !on ? 0.38 : 1, child: DIcon(icon, key: iconKey, size: iconSize, color: fg)),
            if (mark != null) Positioned(left: iconSize + k, top: -1 * k, child: mark),
          ]),
        ),
        SizedBox(height: 2 * k),
        // one line at any text size: a long word shrinks to the item rather than wrapping under it (text scale 2.0)
        FittedBox(fit: BoxFit.scaleDown, child: Text(label, maxLines: 1, softWrap: false, style: _scaled(T.cap(fg, w: on ? FontWeight.w600 : FontWeight.w400), k))),
      ]),
    );
  }
}

TextStyle _scaled(TextStyle s, double k) => k == 1 ? s : s.copyWith(fontSize: (s.fontSize ?? 12) * k);

/// Dock height: the float bar's 60, taller when the label (12 / 16, guidelines 3.2) grows with the system text size (one formula, [capsuleH]).
double padDockH(double textScale) => capsuleH(textScale);

/// The popup above the dock while an item is held: the pad at full size, the photo stays where it is.
class HoldPopupLayer extends StatelessWidget {
  const HoldPopupLayer({super.key, required this.ctl, required this.dockTop, required this.screenW});
  final EditorController ctl;
  final double dockTop, screenW;

  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<HoldPopup?>(
      valueListenable: ctl.hold,
      builder: (context, h, _) {
        if (h == null) return const SizedBox.shrink();
        final c = context.c;
        const side = 216.0;
        final left = (h.anchorX - side / 2).clamp(8.0, screenW - side - 8).toDouble();
        return Positioned(
          left: left,
          top: dockTop - 10 - side,
          width: side,
          height: side,
          child: IgnorePointer(
            child: Container(
              key: const ValueKey('hold-popup'),
              padding: const EdgeInsets.all(Sp.s),
              decoration: BoxDecoration(color: c.surface, borderRadius: BorderRadius.circular(Rd.panel), boxShadow: Fx.shadowFloat),
              child: PadFace(ctl: ctl, cat: h.cat, interactive: false, presets: h.cat == PadCat.color, radius: Rd.cardLg),
            ),
          ),
        );
      },
    );
  }
}
