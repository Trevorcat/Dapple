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

/// The bottom edge the capsule stands on: the larger of the bottom inset and the system gesture inset (guidelines 15.6: nothing a finger
/// must reach sits where the system takes the touch).
double floatBarBase(BuildContext context) {
  final mq = MediaQuery.of(context);
  return math.max(mq.padding.bottom, mq.systemGestureInsets.bottom);
}

/// Space a scrolling page keeps at its end so nothing stays under the capsule: the inset, the gap, the capsule and a little air.
double floatBarClearance(BuildContext context) => floatBarBase(context) + kFloatBarGap + kFloatBarH + Sp.l;

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
          child: SizedBox(key: capsuleKey, height: kFloatBarH, child: Row(mainAxisSize: MainAxisSize.min, children: children)),
        ),
      ),
    );
  }
}

/// One item of the capsule, drawn like a dock item: a 24 dp icon over a 11 px word; the selected one on a tinted pill. [onTap] null = off.
class FloatBarItem extends StatelessWidget {
  const FloatBarItem({super.key, required this.label, required this.icon, this.selected = false, this.onTap, this.width = 80, this.semantics});
  final String label;
  final DI icon;
  final bool selected;
  final VoidCallback? onTap;
  final double width;
  final String? semantics;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final fg = selected ? c.accent : c.text2;
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
          opacity: onTap == null ? 0.4 : 1,
          child: SizedBox(
            width: width,
            height: kFloatBarH,
            child: Container(
              margin: const EdgeInsets.all(2),
              decoration: BoxDecoration(color: selected ? c.accentSoft : null, borderRadius: BorderRadius.circular(26)),
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                mainAxisSize: MainAxisSize.min,
                children: [
                  DIcon(icon, size: 24, color: fg, filled: selected),
                  const SizedBox(height: 2),
                  Text(label, style: T.mini(fg, w: selected ? FontWeight.w600 : FontWeight.w400)),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}
