// The one tile of every tile grid (audit U7, guidelines 10.19): the 局部 [+] card, the area action card, the portrait chips and the
// publish presets are all this tile in a wrap. One look for the states:
//   normal   = the quiet fill, text in `text` / `text2`;
//   on       = "on but not the current one": `accentSoft` fill, no border, `accent` content (a portrait chip that is applied);
//   selected = the persistent selection: `accentSoft` fill + 1.5 `accent` border (the chip being adjusted, the chosen preset);
//   held     = solid `accentBtn` with `onAccent`, only while a finger holds it.
// Minimum 56 x 56 (a 48 dp target with room), radius 16, icon 24 (scaled with the text), label 12 (11 for the note).
import 'package:flutter/material.dart';

import '../theme/tokens.dart';
import 'icons.dart';

enum TileState { normal, on, selected }

class DTile extends StatefulWidget {
  const DTile({
    super.key,
    required this.label,
    required this.onTap,
    this.icon,
    this.iconWidget,
    this.note,
    this.state = TileState.normal,
    this.danger = false,
    this.cue = false,
    this.width,
    this.height = 72,
    this.fill,
    this.alignStart = false,
    this.semantics,
    this.overlay,
    this.iconOpacity = 1,
    this.labelStyle,
  });

  final String label;
  final VoidCallback? onTap;
  final DI? icon;

  /// A drawn picture instead of a [DI] icon (the area kinds' glyphs).
  final Widget Function(Color color, double size)? iconWidget;
  final String? note;
  final TileState state;

  /// A destructive action: `danger` content.
  final bool danger;

  /// The small ⌄ of the selected tile: a second tap opens its actions (audit U3).
  final bool cue;
  final double? width;
  final double height;

  /// The quiet fill (default `bg`, the tile sits on a `surface` panel; on a `bg` page pass `surface`).
  final Color? fill;

  /// Text tiles (presets): left aligned, label 15.
  final bool alignStart;
  final String? semantics;

  /// Drawn over the icon (an analysis progress ring).
  final Widget? overlay;
  final double iconOpacity;
  final TextStyle? labelStyle;

  @override
  State<DTile> createState() => _DTileState();
}

class _DTileState extends State<DTile> {
  bool _held = false;

  void _hold(bool v) {
    if (_held != v && mounted) setState(() => _held = v);
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final w = widget;
    final held = _held && w.onTap != null;
    final active = w.state != TileState.normal;
    final fg = held ? c.onAccent : (w.danger ? c.danger : (active ? c.accent : c.text));
    final fg2 = held ? c.onAccent : (w.danger ? c.danger : (active ? c.accent : c.text2));
    final bg = held ? c.accentBtn : (active ? c.accentSoft : (w.fill ?? c.bg));
    final ts = MediaQuery.textScalerOf(context).scale(12) / 12;
    final labelH = 16 * ts, noteH = w.note == null ? 0.0 : 14 * ts;
    final iconSize = (w.icon == null && w.iconWidget == null) ? 0.0 : (w.height - 12 - labelH - noteH).clamp(12.0, 24 * ts).toDouble();
    final label = Text(w.label,
        maxLines: 1,
        softWrap: false,
        overflow: TextOverflow.fade,
        style: w.labelStyle?.copyWith(color: fg) ?? (w.alignStart ? T.body(fg, w: FontWeight.w500) : T.cap(fg2, w: active ? FontWeight.w600 : FontWeight.w400)));
    final note = w.note == null ? null : Text(w.note!, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.mini(fg2.withValues(alpha: held || active ? 1 : 0.75)));
    final pic = w.icon != null
        ? DIcon(w.icon!, size: iconSize, color: fg)
        : (w.iconWidget != null ? w.iconWidget!(fg, iconSize) : null);
    final content = w.alignStart
        ? Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisAlignment: MainAxisAlignment.center, children: [label, if (note != null) ...[const SizedBox(height: 2), note]])
        : Column(mainAxisAlignment: MainAxisAlignment.center, children: [
            if (pic != null)
              SizedBox(
                width: iconSize + 8,
                height: iconSize,
                child: Stack(alignment: Alignment.center, clipBehavior: Clip.none, children: [Opacity(opacity: w.iconOpacity, child: pic), ?w.overlay]),
              ),
            if (pic != null) const SizedBox(height: Sp.xs),
            label,
            ?note,
          ]);
    return Semantics(
      button: true,
      selected: w.state == TileState.selected,
      toggled: w.state == TileState.on ? true : null,
      label: w.semantics ?? w.label,
      excludeSemantics: true,
      onTap: w.onTap,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: w.onTap,
        onTapDown: (_) => _hold(true),
        onTapUp: (_) => _hold(false),
        onTapCancel: () => _hold(false),
        child: Container(
          width: w.width,
          height: w.height,
          constraints: const BoxConstraints(minWidth: 56, minHeight: 56),
          padding: EdgeInsets.symmetric(horizontal: w.alignStart ? Sp.m : Sp.xs),
          alignment: w.alignStart ? Alignment.centerLeft : Alignment.center,
          decoration: BoxDecoration(
            color: bg,
            borderRadius: BorderRadius.circular(Rd.cardLg),
            border: w.state == TileState.selected && !held ? Border.all(color: c.accent, width: 1.5) : null,
          ),
          child: Stack(clipBehavior: Clip.none, children: [
            Positioned.fill(child: Align(alignment: w.alignStart ? Alignment.centerLeft : Alignment.center, child: content)),
            if (w.cue) Positioned(right: 0, bottom: Sp.xs, child: DIcon(DI.down, key: const ValueKey('tile-actions-cue'), size: 12, color: fg, stroke: 2.4)),
          ]),
        ),
      ),
    );
  }
}

/// Tiles in a wrap with the grid gap; the caller sizes the tiles (at least 56, and so at least the 48 dp target).
class TileGrid extends StatelessWidget {
  const TileGrid({super.key, required this.children, this.alignment = WrapAlignment.center});
  final List<Widget> children;
  final WrapAlignment alignment;

  static const double gap = Sp.s;

  /// Width of a tile when [perRow] share [width] (at most [max], at least the 56 dp minimum).
  static double tileWidth(double width, int perRow, {double max = 84}) {
    // whole pixels down: a row that adds up to exactly the width must not wrap on a rounding error
    final w = ((width - gap * (perRow - 1)) / perRow).floorToDouble();
    return w.clamp(56.0, max).toDouble();
  }

  /// How many tiles of at least [min] fit in [width].
  static int fit(double width, {double min = 56}) => ((width + gap) / (min + gap)).floor().clamp(1, 99);

  @override
  Widget build(BuildContext context) => Wrap(spacing: gap, runSpacing: gap, alignment: alignment, children: children);
}
