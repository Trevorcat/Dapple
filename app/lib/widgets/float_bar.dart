// The floating capsule of the home shell (docs/design/guidelines.md 15.6): the editor dock's own surface (the frosted [GlassCapsule], radius 28,
// 56 dp items, a tinted pill for the selected one), centred above the bottom inset. Pages scroll underneath it and end with
// [floatBarClearance] of space, so the last row and any bottom action clear it.
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../screens/editor/editor_overlays.dart' show GlassCapsule;
import '../theme/tokens.dart';
import 'icons.dart';

/// Height of the capsule's items (the dock's), the gap between it and the bottom inset, the radius (the dock's).
const double kFloatBarH = 56, kFloatBarGap = 12, kFloatBarRadius = 28;

/// The capsule's height: 56, taller when the 12 / 16 label grows with the system text size (icon 24 + gap 2 + label + margin 4).
double floatBarH(BuildContext context) {
  final ts = MediaQuery.textScalerOf(context).scale(12) / 12;
  return ts <= 1.0 ? kFloatBarH : 32 + 16 * ts;
}

/// The bottom edge the capsule stands on: the larger of the bottom inset and the system gesture inset (guidelines 15.6: nothing a finger
/// must reach sits where the system takes the touch).
double floatBarBase(BuildContext context) {
  final mq = MediaQuery.of(context);
  return math.max(mq.padding.bottom, mq.systemGestureInsets.bottom);
}

/// Space a scrolling page keeps at its end so nothing stays under the capsule: the inset, the gap, the capsule and a little air.
double floatBarClearance(BuildContext context) => floatBarBase(context) + kFloatBarGap + floatBarH(context) + Sp.l;

/// The capsule: centred above the bottom inset; the empty space beside it passes touches through to the page.
class FloatBar extends StatelessWidget {
  const FloatBar({super.key, required this.children, this.capsuleKey});
  final List<Widget> children;

  /// Key of the capsule itself (its rect is the visible bar; the widget's own box includes the inset and the gap).
  final Key? capsuleKey;

  @override
  Widget build(BuildContext context) {
    final bottom = floatBarBase(context);
    return Padding(
      padding: EdgeInsets.only(bottom: bottom + kFloatBarGap, left: Sp.l, right: Sp.l),
      child: Center(
        heightFactor: 1,
        child: GlassCapsule(
          radius: kFloatBarRadius,
          child: SizedBox(key: capsuleKey, height: floatBarH(context), child: Row(mainAxisSize: MainAxisSize.min, children: children)),
        ),
      ),
    );
  }
}

/// One item of the capsule, drawn like a dock item: a 24 dp icon over a 12 / 16 word; the selected one on a tinted pill. [onTap] null = off.
class FloatBarItem extends StatelessWidget {
  const FloatBarItem({super.key, required this.label, required this.icon, this.selected = false, this.onTap, this.width = 80, this.semantics, this.primary = false, this.dimmed = false});
  final String label;
  final DI icon;
  final bool selected;
  final VoidCallback? onTap;
  final double width;
  final String? semantics;

  /// The capsule's one main action (发布): the same icon-over-word item on a filled primary pill, at the same width as the others.
  final bool primary;

  /// Drawn as off but still tappable: the tap says why (合成 with fewer than two photos).
  final bool dimmed;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final fg = primary ? c.onPrimaryContainer : (selected ? c.accent : c.text2);
    return Semantics(
      button: true,
      selected: selected,
      enabled: onTap != null,
      label: semantics ?? label,
      excludeSemantics: true,
      onTap: onTap,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: onTap,
        child: Opacity(
          opacity: onTap == null || dimmed ? 0.4 : 1,
          child: SizedBox(
            width: width,
            height: floatBarH(context),
            child: Container(
              margin: const EdgeInsets.all(2),
              decoration: BoxDecoration(color: primary ? c.primaryContainer : (selected ? c.accentSoft : null), borderRadius: BorderRadius.circular(26)),
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                mainAxisSize: MainAxisSize.min,
                children: [
                  DIcon(icon, size: 24, color: fg, filled: selected || primary),
                  const SizedBox(height: 2),
                  FittedBox(fit: BoxFit.scaleDown, child: Text(label, maxLines: 1, softWrap: false, style: T.cap(fg, w: selected || primary ? FontWeight.w600 : FontWeight.w400))),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}
