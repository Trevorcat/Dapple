// The docked control panel (docs/design/editor-panel.md, version 0.6): three layers, no scrolling, nothing to scan.
//   Layer 1  the photo and the dock (pad_dock.dart);
//   Layer 2  tap a dock item: ONE pad plus ONE vertical strength strip (and, for 胶片, the stock button that swaps the pad for
//            the film map);
//   Layer 3  the 分项 button: the category's parameters as a character chart plus one ruler, with the complex editors
//            (curve, colour ranges, grading, paper) as extra entries.
// The panel is as tall as its category needs and never changes height within a category. It lives under the photo or beside
// it (a narrow tall pane), whichever leaves the larger photo (panel_layout.dart).
//
// Performance: the panel rebuilds on structural changes only (layer, selection, first step of a gesture, undo). While a finger
// moves, only the cursor layer, the strip, the glyphs and the header readout repaint (from the edit tick), each in its own
// RepaintBoundary.
import 'dart:math' as math;

import 'package:flutter/foundation.dart' show ValueListenable;
import 'package:flutter/material.dart';

import '../../model/bases.dart';
import '../../model/pad_model.dart';
import '../../model/params.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart' show HitBox, kHit;
import '../../widgets/icons.dart';
import 'editor_controller.dart';
import 'editor_widgets.dart' show CurvePainter, curveY;
import 'film_map_view.dart';
import 'itemised_view.dart';
import 'local_panel.dart' show LocalBodyView, LocalHeader;
import 'mini_maps.dart' show BandDots, WheelMini;
import 'pad_gestures.dart';
import 'pad_widgets.dart';
import 'tone_editor.dart';

// ------------------------------------------------------------------ metrics

const double kPanelHandleH = 18;
/// One header line: a 48 dp touch row (the buttons are 32 dp pills with a 48 x 48 target, guidelines 6).
const double kHeaderH = 48;

/// Fixed part of the panel's height: handle, header, the gaps (the rest is the content).
const double kPanelTopGap = 4, kPanelBottomPad = 12;
const double kPanelFixedH = kPanelHandleH + kHeaderH + kPanelTopGap + kPanelBottomPad;

/// The pad row's content height: comfortable, and the least a pad may shrink to (pad and strip shrink together; the pad may
/// be wider than tall).
const double kPadComfortH = 236, kPadMinH = 150;

/// The least height the photo keeps visible above the panel in a window too low for both (the panel gives way, never the photo).
const double kMinPhotoH = 80;

/// Content height of the itemised view: a radar of 80 dp radius with its names outside, plus the ruler.
const double kRadarContentH = 290;

/// Panel height under the photo, used before any layout is known (the host's default).
const double kPanelBottomH = kPanelFixedH + kPadComfortH;

/// The panel under the photo is never wider than this (a wide screen centres it).
const double kPanelMaxW = 600;

/// The header of the side pane has a second line for the readout.
const double kSideHeaderH = kHeaderH + 22;

/// From this text scale on, the header puts its buttons on a line of their own instead of shrinking their words (guidelines 12.6).
const double kHeaderWrapScale = 1.3;
bool headerWraps(double textScale) => textScale >= kHeaderWrapScale;

double headerH(double textScale, {bool side = false}) =>
    (side ? kSideHeaderH : kHeaderH) + (headerWraps(textScale) ? kHeaderH : 0) + math.max(0.0, textScale - 1) * 24;

/// Panel height when folded to the handle and the header.
double panelCollapsedH(double textScale, {bool side = false}) => kPanelHandleH + headerH(textScale, side: side) + 6;

/// Height of the panel under the photo. The rule: the photo's edge stays at least 90 % of its no-panel size whenever the pad
/// can still be [kPadMinH] tall; below that the photo shrinks further. The panel never gets taller than its comfortable
/// size, and the itemised view (the precise layer) may be taller, never beyond 46 % of the screen. Heights follow the content of the
/// layer (guidelines 15.1-7); the change between layers is animated by the editor page.
///
/// [region]: height between the top bar and the dock; [photoW]: width the photo may use; [aspect]: width / height of the photo.
///
/// [bodyNeed]: a layer whose content is shorter than the pad (the portrait chips) says how tall its body is; the panel is as tall as that
/// (never under the pad's least), so no empty band is left under the content.
double bottomPanelHeight({required double region, required double photoW, required double aspect, required double screenH, double textScale = 1, bool itemised = false, double headerExtra = 0, double? bodyNeed}) {
  // large text: the header's button line; [headerExtra]: a header with a further line (局部's area chips)
  final extra = math.max(0.0, textScale - 1) * 24 + (headerWraps(textScale) ? kHeaderH : 0) + headerExtra;
  final hRef = math.min(photoW / aspect, region);
  final allowed = region - 8 - 0.9 * hRef; // the photo keeps 90 % of its height
  final lo = kPanelFixedH + kPadMinH + extra, comfy = kPanelFixedH + (bodyNeed == null ? kPadComfortH : math.max(kPadMinH, math.min(kPadComfortH, bodyNeed))) + extra;
  var h = allowed.clamp(lo, comfy).toDouble();
  if (itemised) h = math.max(h, kPanelFixedH + kRadarContentH + extra);
  return math.min(h, math.max(screenH * 0.46, lo));
}

/// What the open category needs, per placement, and how much taller it can usefully get.
class PanelNeeds {
  const PanelNeeds({required this.bottom, required this.side, required this.sideMin, required this.growBottom, required this.growSide, required this.growMin, required this.pad});

  /// Heights of the whole panel (bottom) / of the pane without the dock (side).
  final double bottom, side;

  /// The least the side pane needs.
  final double sideMin;

  /// How much taller the pad can usefully get, and the least spare height worth it.
  final double growBottom, growSide, growMin;

  /// Always false now (the photo is no longer a pad); kept for the layout solver's signature.
  final bool pad;
}

PanelNeeds panelNeeds({
  required double screenW,
  required double paneW,
  required double screenH,
  required double region,
  required double photoW,
  required double aspect,
  double textScale = 1,
  bool collapsed = false,
  bool itemised = false,
  double headerExtra = 0,
  double? bodyNeed,
}) {
  // [headerExtra]: a header with a further full line (局部: the area chips are a 48 dp line of their own), in both placements
  final extra = math.max(0.0, textScale - 1) * 24 + (headerWraps(textScale) ? kHeaderH : 0); // large text: the header's button line
  if (collapsed) {
    return PanelNeeds(bottom: panelCollapsedH(textScale), side: panelCollapsedH(textScale, side: true), sideMin: panelCollapsedH(textScale, side: true), growBottom: 0, growSide: 0, growMin: 0, pad: false);
  }
  final sideFixed = kPanelHandleH + kSideHeaderH + kPanelTopGap + kPanelBottomPad + extra + headerExtra;
  final side = sideFixed + (itemised ? kRadarContentH : 176);
  return PanelNeeds(
    bottom: bottomPanelHeight(region: region, photoW: photoW, aspect: aspect, screenH: screenH, textScale: textScale, itemised: itemised, headerExtra: headerExtra, bodyNeed: bodyNeed),
    side: side,
    // 分项 in a short side pane (landscape phone): the radar shrinks rather than the pane giving way to a bottom panel that leaves a
    // stamp-sized photo (guidelines 15.8, editor-panel 8: the side radar is small)
    sideMin: itemised ? sideFixed + 180 - headerExtra : side - 20 - headerExtra,
    growBottom: 0,
    growSide: itemised ? 0 : 220,
    growMin: 0,
    pad: false,
  );
}

// ------------------------------------------------------------------ host (frame + animated rect)

/// The panel frame at the animated [rect] (bottom panel or side pane). The content is laid out at its final size
/// ([contentSize]) and revealed from the top, so nothing reflows during the animation.
class DockedPanelHost extends StatefulWidget {
  const DockedPanelHost({super.key, required this.controller, required this.rect, required this.contentSize, required this.group, required this.collapsed, required this.side});
  final EditorController controller;

  /// Animated visible rectangle (height 0 = closed).
  final ValueListenable<Rect> rect;
  final Size contentSize;
  final EGroup? group;
  final bool collapsed, side;

  @override
  State<DockedPanelHost> createState() => _DockedPanelHostState();
}

class _DockedPanelHostState extends State<DockedPanelHost> {
  EGroup? _last;
  bool _lastCollapsed = false, _lastSide = false;
  Size _lastContent = const Size(0, kPanelBottomH);

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    if (widget.group != null) {
      _last = widget.group;
      _lastCollapsed = widget.collapsed;
      _lastSide = widget.side;
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
                borderRadius: BorderRadius.circular(Rd.panel),
                border: Border.all(color: c.sep),
                boxShadow: Fx.shadowPanel,
              ),
              clipBehavior: Clip.antiAlias,
              child: OverflowBox(
                alignment: Alignment.topCenter,
                minWidth: _lastContent.width,
                maxWidth: _lastContent.width,
                minHeight: _lastContent.height,
                maxHeight: _lastContent.height,
                child: DockedPanel(controller: widget.controller, group: g, collapsed: _lastCollapsed, side: _lastSide),
              ),
            ),
          ),
        );
      },
    );
  }
}

// ------------------------------------------------------------------ panel

/// Test hook: how often the panel body was rebuilt (a drag must not rebuild it).
int debugPanelBuilds = 0;

class DockedPanel extends StatefulWidget {
  const DockedPanel({super.key, required this.controller, required this.group, required this.collapsed, this.side = false});
  final EditorController controller;
  final EGroup group;
  final bool collapsed, side;

  @override
  State<DockedPanel> createState() => _DockedPanelState();
}

class _DockedPanelState extends State<DockedPanel> {
  EditorController get ctl => widget.controller;
  late final Listenable _all = Listenable.merge([ctl.wm, ctl.edits, ctl.editingRow]);

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
    if (mounted) setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    debugPanelBuilds++;
    final g = widget.group;
    final cat = PadCatX.ofGroup(g);
    final local = g == EGroup.local;
    final ts = MediaQuery.textScalerOf(context).scale(15) / 15;
    return Semantics(
      container: true,
      label: '${local ? '局部' : cat?.label ?? '裁切'}调整面板',
      child: Column(key: ValueKey('panel-${g.name}'), children: [
        _Handle(ctl: ctl, group: g, collapsed: widget.collapsed),
        if (local)
          LocalHeader(ctl: ctl, side: widget.side, textScale: ts, collapsed: widget.collapsed)
        else
          _Header(ctl: ctl, group: g, cat: cat, side: widget.side, textScale: ts),
        if (!widget.collapsed)
          Expanded(
            // a window too low for photo + panel gives the photo its minimum and the body less than it needs: the body keeps its least
            // height inside a clipped, scrollable viewport instead of overflowing (docs/design/foldables.md has the real layout for it)
            child: LayoutBuilder(builder: (context, bc) {
              final body = Padding(
                padding: const EdgeInsets.fromLTRB(Sp.m, kPanelTopGap, Sp.m, kPanelBottomPad),
                child: local ? LocalBodyView(ctl: ctl, side: widget.side) : (cat == null ? CropBody(ctl: ctl) : _body(cat)),
              );
              final need = (ctl.itemised ? kRadarContentH : kPadMinH) + kPanelTopGap + kPanelBottomPad;
              if (widget.side || bc.maxHeight >= need) return body;
              return SingleChildScrollView(physics: const ClampingScrollPhysics(), child: SizedBox(height: need, child: body));
            }),
          ),
      ]),
    );
  }

  Widget _body(PadCat cat) {
    final extra = ctl.extra;
    if (extra != null) return ExtraEditor(ctl: ctl, cat: cat, extra: extra);
    if (ctl.itemised) return ItemisedView(ctl: ctl, cat: cat);
    if (cat == PadCat.film && ctl.filmMapOpen) return FilmMapView(key: const ValueKey('film-map'), ctl: ctl);
    return PadRow(ctl: ctl, cat: cat);
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

// ------------------------------------------------------------------ header

/// A pill button of the header: a 32 dp pill inside a 48 x 48 touch target. [pressed] = a layer that is open (分项, the film map):
/// `accentBtn` with `onAccent`, which reads in both themes.
class HeaderButton extends StatelessWidget {
  const HeaderButton({super.key, required this.label, required this.onTap, this.pressed = false, this.icon, this.semantics, this.chevron = false, this.trailingChevron = false});
  final String label;

  /// A small down chevron after the label (a button that opens a picker).
  final bool chevron;

  /// A right chevron instead (a button that opens a screen).
  final bool trailingChevron;
  final VoidCallback onTap;
  final bool pressed;

  /// An icon of the app's line set instead of the word (the word stays the semantics label).
  final DI? icon;
  final String? semantics;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final fg = pressed ? c.onAccent : c.text;
    final fg2 = pressed ? c.onAccent : c.text2;
    // icons grow with the text (guidelines 12.6)
    final ts = MediaQuery.textScalerOf(context).scale(16) / 16;
    return Semantics(
      button: true,
      toggled: pressed,
      label: semantics ?? label,
      excludeSemantics: true,
      onTap: onTap,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: onTap,
        child: HitBox(
          child: Container(
            constraints: const BoxConstraints(minWidth: 32, minHeight: 32),
            padding: const EdgeInsets.symmetric(horizontal: Sp.m),
            alignment: Alignment.center,
            decoration: BoxDecoration(color: pressed ? c.accentBtn : c.bg, borderRadius: BorderRadius.circular(Rd.cardLg)),
            child: icon != null
                ? DIcon(icon!, size: 18 * ts, color: fg)
                : Row(mainAxisSize: MainAxisSize.min, children: [
                    Text(label, maxLines: 1, style: T.cap(fg, w: FontWeight.w500)),
                    if (chevron && !trailingChevron) DIcon(DI.down, size: 14 * ts, color: fg2),
                    if (trailingChevron) DIcon(DI.forward, size: 14 * ts, color: fg2),
                  ]),
          ),
        ),
      ),
    );
  }
}

/// One header for every panel layer (audit U6, guidelines 15.2.8): `[‹] title · readout or subtitle …… (reset) · main action`. The title is
/// always the dock item's name; ‹ (left of the title) appears in any layer below the panel's first one and goes back one layer; the
/// right end holds at most three buttons, the last one being the layer's main action. A narrow side pane puts the readout / subtitle on
/// a second line; [lines] are further full-width lines (局部's area chips). Large text puts the buttons on a line of their own.
class PanelHeader extends StatelessWidget {
  const PanelHeader({
    super.key,
    required this.title,
    required this.side,
    required this.textScale,
    this.onBack,
    this.sub,
    this.subTappable = false,
    this.buttons = const [],
    this.below,
    this.belowLines = 0,
  }) : assert(buttons.length <= 3, 'a header holds at most three buttons (U6)');
  final String title;
  final bool side;
  final double textScale;
  final VoidCallback? onBack;

  /// The readout (强度 N% …) or the subtitle (the card, the editor, the layer); [subTappable]: it holds buttons (48 dp line).
  final Widget? sub;
  final bool subTappable;
  final List<Widget> buttons;

  /// Full-width content under the header, [belowLines] lines of [kHeaderH] (局部's area chips).
  final Widget? below;
  final int belowLines;

  /// Height of the header: one line (two in a side pane: 22 dp readout, 48 dp when it holds buttons), the large-text button line, the
  /// extra lines.
  static double heightOf({required double textScale, required bool side, bool subTappable = false, int lines = 0}) {
    final extra = math.max(0.0, textScale - 1) * 24;
    final first = kHeaderH + extra;
    final second = side ? (subTappable ? kHeaderH : kSideHeaderH - kHeaderH) : 0.0;
    return first + second + (headerWraps(textScale) ? kHeaderH : 0) + lines * kHeaderH;
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final titleStyle = T.body(c.text, w: FontWeight.w600);
    final wrap = headerWraps(textScale) && buttons.length > 1;
    final extra = math.max(0.0, textScale - 1) * 24;
    final row = Row(mainAxisSize: MainAxisSize.min, children: [for (final b in buttons) ...[const SizedBox(width: Sp.xs), b]]);
    final back = onBack == null
        ? null
        : Semantics(
            button: true,
            label: '返回',
            excludeSemantics: true,
            onTap: onBack,
            child: GestureDetector(
              key: const ValueKey('header-back'),
              behavior: HitTestBehavior.opaque,
              onTap: onBack,
              child: SizedBox(width: 36, height: kHit, child: Align(alignment: Alignment.centerLeft, child: DIcon(DI.back, size: 22 * textScale.clamp(1.0, 2.0), color: c.text))),
            ),
          );
    final titleW = Row(mainAxisSize: MainAxisSize.min, children: [
      ?back,
      Text(title, key: const ValueKey('header-title'), style: titleStyle),
    ]);
    final h = heightOf(textScale: textScale, side: side, subTappable: subTappable, lines: below == null ? 0 : belowLines);
    final first = Row(children: [
      titleW,
      const SizedBox(width: Sp.m),
      Expanded(child: side || sub == null ? const SizedBox.shrink() : Align(alignment: Alignment.centerLeft, child: sub)),
      if (!wrap) Flexible(flex: 0, child: SingleChildScrollView(scrollDirection: Axis.horizontal, reverse: true, child: row)),
    ]);
    return SizedBox(
      height: h,
      child: Padding(
        padding: EdgeInsets.only(left: onBack == null ? Sp.l : Sp.s, right: Sp.l),
        child: Column(mainAxisAlignment: MainAxisAlignment.center, crossAxisAlignment: CrossAxisAlignment.start, children: [
          SizedBox(height: kHeaderH + extra, child: first),
          if (side) SizedBox(height: subTappable ? kHeaderH : kSideHeaderH - kHeaderH, child: Align(alignment: Alignment.centerLeft, child: sub ?? const SizedBox.shrink())),
          // large text: the buttons keep their words at full size on a line of their own (guidelines 12.6)
          if (headerWraps(textScale))
            SizedBox(height: kHeaderH, child: wrap ? Align(alignment: Alignment.centerRight, child: SingleChildScrollView(scrollDirection: Axis.horizontal, reverse: true, child: row)) : null),
          if (below != null && belowLines > 0) SizedBox(height: belowLines * kHeaderH, child: below),
        ]),
      ),
    );
  }
}

/// The readout text of a header (强度 N% · …), repainted from the edit tick only.
class HeaderReadout extends StatelessWidget {
  const HeaderReadout({super.key, required this.listenable, required this.text});
  final Listenable listenable;
  final String Function() text;

  @override
  Widget build(BuildContext context) => ListenableBuilder(
        listenable: listenable,
        builder: (context, _) => LayoutBuilder(builder: (context, cons) {
          final style = T.cap(context.c.text2, tabular: true);
          var t = text();
          // "强度 N%" is the readout (U1) and always stays whole; the pad's directions after it ("· 淡 90 · 亮 64") give way when the
          // header is narrow (a 360 dp phone with the 成像 button) and stay in the semantics label
          if (cons.maxWidth.isFinite) {
            final cut = t.indexOf(' · ');
            final tp = TextPainter(text: TextSpan(text: t, style: style), maxLines: 1, textDirection: TextDirection.ltr, textScaler: MediaQuery.textScalerOf(context))..layout();
            if (cut > 0 && tp.width > cons.maxWidth) t = t.substring(0, cut);
          }
          return Text(t, key: const ValueKey('header-readout'), maxLines: 1, softWrap: false, overflow: TextOverflow.ellipsis, style: style);
        }),
      );
}

class _Header extends StatelessWidget {
  const _Header({required this.ctl, required this.group, required this.cat, required this.side, required this.textScale});
  final EditorController ctl;
  final EGroup group;
  final PadCat? cat;
  final bool side;
  final double textScale;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final cat = this.cat;
    final name = cat?.label ?? '裁切';
    final extra = ctl.extra;
    final inItems = ctl.itemised && extra == null;
    final film = cat == PadCat.film;
    final mapOpen = film && ctl.filmMapOpen && !ctl.itemised;
    final reset = ListenableBuilder(
      listenable: ctl.app.editTick,
      builder: (context, _) {
        final adjusted = cat == null ? ctl.edit.crop != 'orig' || ctl.edit.rot != 0 || ctl.edit.flip || ctl.edit.values.containsKey('straighten') : padCatAdjusted(ctl.edit, cat, ctl.padCtx);
        // hidden but in place when nothing is changed: the header does not jump (guidelines 15.8)
        return Visibility(
          visible: adjusted,
          maintainSize: true,
          maintainAnimation: true,
          maintainState: true,
          child: HeaderButton(key: ValueKey('reset-${group.name}'), label: '复位', icon: DI.reset, semantics: '复位$name', onTap: () => cat == null ? ctl.resetCrop() : ctl.resetPad(cat)),
        );
      },
    );
    final readout = cat == null ? null : HeaderReadout(listenable: ctl.app.editTick, text: () => padReadout(cat, ctl.padView(cat), ctx: ctl.padCtx));

    // 成像, the map: ‹ back to the pad; the subtitle is what is chosen; the main action is 无 (only when something is chosen)
    if (mapOpen) {
      return ListenableBuilder(
        listenable: ctl.app.editTick,
        builder: (context, _) {
          final none = ctl.edit.film == null;
          return PanelHeader(
            title: name,
            side: side,
            textScale: textScale,
            onBack: () => ctl.setFilmMap(false),
            sub: Text(none ? '选成像' : baseNameOf(ctl.app.film, ctl.edit.film), key: const ValueKey('header-sub'), maxLines: 1, overflow: TextOverflow.ellipsis, style: T.cap(c.text2)),
            buttons: [if (!none) HeaderButton(key: const ValueKey('film-none'), label: '无', semantics: '不用成像', onTap: ctl.stockNone)],
          );
        },
      );
    }
    // a complex editor: ‹ back to 分项; the subtitle names the editor
    if (extra != null) {
      final sub = switch (extra) {
        PanelExtra.curve => '曲线',
        PanelExtra.hsl => '分色',
        PanelExtra.grade => '分级',
        PanelExtra.paper => '相纸',
        PanelExtra.tone => '明暗目标',
      };
      return PanelHeader(
        title: name,
        side: side,
        textScale: textScale,
        onBack: () => ctl.openExtra(null),
        sub: Text(sub, key: const ValueKey('header-sub'), maxLines: 1, style: T.cap(c.text2)),
        buttons: [reset],
      );
    }
    // 分项: ‹ back to the pad; the complex editors are entries where the readout was
    if (inItems) {
      final extras = <(String, PanelExtra)>[
        if (cat == PadCat.light) ...[('明暗目标', PanelExtra.tone), ('曲线', PanelExtra.curve)],
        if (cat == PadCat.color) ...[('分色', PanelExtra.hsl), ('分级', PanelExtra.grade)],
        if (cat == PadCat.film && (ctl.app.film.stock(ctl.edit.film)?.printed ?? false)) ('相纸', PanelExtra.paper),
      ];
      final entries = extras.isEmpty
          ? readout
          : FittedBox(
              fit: BoxFit.scaleDown,
              alignment: Alignment.centerLeft,
              child: Row(mainAxisSize: MainAxisSize.min, children: [
                for (final x in extras) ...[
                  HeaderButton(key: ValueKey('extra-${x.$1}'), label: x.$1, trailingChevron: true, onTap: () => ctl.openExtra(x.$2)),
                  const SizedBox(width: Sp.xs),
                ],
              ]),
            );
      return PanelHeader(
        title: name,
        side: side,
        textScale: textScale,
        onBack: () => ctl.setItemised(false),
        sub: entries,
        subTappable: extras.isNotEmpty,
        buttons: [reset],
      );
    }
    // the pad layer: title · (成像: the chosen base) readout …… (reset) · 分项
    final stock = !film
        ? null
        : ListenableBuilder(
            listenable: ctl.app.editTick,
            builder: (context, _) {
              final none = ctl.edit.film == null;
              final label = none ? '选成像' : baseNameOf(ctl.app.film, ctl.edit.film);
              // a press opens the map at once and the same finger can slide into it (press - slide - release selects and closes);
              // a plain tap leaves it open for a second tap
              return Listener(
                  behavior: HitTestBehavior.translucent,
                  onPointerDown: (e) => ctl.filmPressDown(e.pointer, e.position),
                  child: HeaderButton(
                      key: const ValueKey('stock-button'),
                      label: label,
                      chevron: true,
                      semantics: none ? '选成像' : '选择成像，当前$label',
                      onTap: () => ctl.setFilmMap(true)));
            },
          );
    final sub = stock == null ? readout : Row(mainAxisSize: MainAxisSize.min, children: [stock, const SizedBox(width: Sp.s), Flexible(child: readout!)]);
    return PanelHeader(
      title: name,
      side: side,
      textScale: textScale,
      sub: sub,
      subTappable: stock != null,
      buttons: [
        reset,
        if (cat != null) HeaderButton(key: const ValueKey('items-button'), label: '分项', onTap: () => ctl.setItemised(true)),
      ],
    );
  }
}

// ------------------------------------------------------------------ layer 2

/// The strip on the left, the pad filling the rest.
class PadRow extends StatelessWidget {
  const PadRow({super.key, required this.ctl, required this.cat});
  final EditorController ctl;
  final PadCat cat;

  @override
  Widget build(BuildContext context) {
    return Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      StrengthStrip(ctl: ctl, cat: cat),
      const SizedBox(width: Sp.m),
      Expanded(child: PadFace(ctl: ctl, cat: cat, presets: cat == PadCat.color)),
    ]);
  }
}

// ------------------------------------------------------------------ chips

class PanelChip extends StatelessWidget {
  const PanelChip({super.key, required this.label, required this.selected, required this.onTap, this.dot, this.compact = false});
  final String label;

  /// Narrow padding (chips that share a row equally).
  final bool compact;
  final bool selected;
  final VoidCallback onTap;
  final Color? dot;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      button: true,
      selected: selected,
      label: label,
      excludeSemantics: true,
      onTap: onTap,
      // a 36 dp chip in a 48 dp touch row (guidelines 6, 10.2)
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: onTap,
        child: Padding(
          // the opaque detector takes the padding too: 6 + 36 + 6 = 48
          padding: const EdgeInsets.symmetric(vertical: (kHit - 36) / 2),
          child: Center(
            heightFactor: 1,
            widthFactor: compact ? null : 1,
            child: Container(
              height: 36,
              constraints: const BoxConstraints(minWidth: kHit),
              padding: EdgeInsets.symmetric(horizontal: compact ? Sp.xs : Sp.m),
              alignment: Alignment.center,
              decoration: BoxDecoration(color: selected ? c.accentSoft : c.chip, borderRadius: BorderRadius.circular(Rd.chip), border: selected ? Border.all(color: c.accent, width: 1.5) : null),
              child: Row(mainAxisSize: MainAxisSize.min, children: [
                if (dot != null) ...[Container(width: 10, height: 10, decoration: BoxDecoration(color: dot, shape: BoxShape.circle)), const SizedBox(width: Sp.s)],
                Flexible(child: Text(label, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.cap(selected ? c.accent : c.text, w: selected ? FontWeight.w600 : FontWeight.w400))),
              ]),
            ),
          ),
        ),
      ),
    );
  }
}

/// A ruler bound to one slider key of the edit (crop straightening, colour ranges, grading, curve).
class SpecRuler extends StatelessWidget {
  const SpecRuler({super.key, required this.ctl, required this.valueKey, required this.label});
  final EditorController ctl;
  final String valueKey, label;

  @override
  Widget build(BuildContext context) {
    final s = specByKey[valueKey]!;
    return ListenableBuilder(
      listenable: ctl.app.editTick,
      builder: (context, _) => TapeRuler(
        key: ValueKey('ruler-$valueKey'),
        label: label,
        value: ctl.edit.value(valueKey),
        min: s.min,
        max: s.max,
        auto: s.def,
        format: (v) => fmtSpec(s, v),
        haptics: ctl.haptics,
        onBegin: () => ctl.valueBegin(valueKey),
        onChanged: (v) => ctl.valueSet(valueKey, v),
        onEnd: ctl.valueEnd,
      ),
    );
  }
}

// ------------------------------------------------------------------ crop

class CropBody extends StatelessWidget {
  const CropBody({super.key, required this.ctl});
  final EditorController ctl;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: Listenable.merge([ctl.app.editTick, ctl.edits]),
      builder: (context, _) {
        final e = ctl.edit;
        Widget ratio(int i) => Expanded(
            child: PanelChip(key: ValueKey('crop-${cropRatios[i]}'), label: cropRatioNames[i], selected: e.crop == cropRatios[i], compact: true, onTap: () => ctl.setCropRatio(cropRatios[i])));
        return LayoutBuilder(builder: (context, cons) {
          // a narrow pane puts the six ratios on two rows of three instead of clipping the words (audit F46)
          final perRow = (cons.maxWidth - Sp.s * 5) / 6 < 56 * MediaQuery.textScalerOf(context).scale(13) / 13 ? 3 : 6;
          final rows = [
            for (var r = 0; r < cropRatios.length; r += perRow)
              Row(children: [
                for (var i = r; i < r + perRow; i++) ...[if (i > r) const SizedBox(width: Sp.s), ratio(i)],
              ]),
          ];
          final top = Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
            ...rows,
            Row(children: [
              Expanded(child: PanelChip(key: const ValueKey('crop-rotate'), label: '旋转 90°', selected: false, compact: true, onTap: ctl.rotate90)),
              const SizedBox(width: Sp.s),
              Expanded(child: PanelChip(key: const ValueKey('crop-flip'), label: '水平翻转', selected: e.flip, compact: true, onTap: ctl.toggleFlip)),
            ]),
          ]);
          return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            // the chips give way (scroll) before the ruler does when the pane is short
            Flexible(child: SingleChildScrollView(child: top)),
            const SizedBox(height: Sp.s),
            SpecRuler(ctl: ctl, valueKey: 'straighten', label: '拉直'),
          ]);
        });
      },
    );
  }
}

// ------------------------------------------------------------------ complex editors (layer 3 extras)

/// One of the complex editors, filling the panel's content area. Their pads are absolute as well.
class ExtraEditor extends StatelessWidget {
  const ExtraEditor({super.key, required this.ctl, required this.cat, required this.extra});
  final EditorController ctl;
  final PadCat cat;
  final PanelExtra extra;

  @override
  Widget build(BuildContext context) {
    switch (extra) {
      case PanelExtra.curve:
        return CurveEditor(ctl: ctl);
      case PanelExtra.hsl:
        return HslEditor(ctl: ctl);
      case PanelExtra.grade:
        return GradeEditor(ctl: ctl);
      case PanelExtra.paper:
        return PaperEditor(ctl: ctl);
      case PanelExtra.tone:
        return ToneEditor(ctl: ctl);
    }
  }
}

class PaperEditor extends StatelessWidget {
  const PaperEditor({super.key, required this.ctl});
  final EditorController ctl;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: Listenable.merge([ctl.app.editTick, ctl.edits]),
      builder: (context, _) {
        final cat = ctl.app.film;
        final stock = cat.stock(ctl.edit.film);
        if (stock == null) return const SizedBox.shrink();
        final def = cat.defaultPaperOf(stock);
        final cur = ctl.edit.filmPaper ?? def;
        return Align(
          alignment: Alignment.topLeft,
          child: Wrap(spacing: 6, runSpacing: 6, children: [
            for (final id in cat.paperIds) PanelChip(key: ValueKey('paper-$id'), label: cat.paperName(id), selected: id == cur, onTap: () => ctl.selectPaper(id == def ? null : id)),
          ]),
        );
      },
    );
  }
}

class CurveEditor extends StatefulWidget {
  const CurveEditor({super.key, required this.ctl});
  final EditorController ctl;
  @override
  State<CurveEditor> createState() => _CurveEditorState();
}

class _CurveEditorState extends State<CurveEditor> {
  static const xs = [0.88, 0.62, 0.37, 0.12];
  static const regions = ['H', 'L', 'D', 'S'];
  Size _size = Size.zero;
  int? _lock;
  EditorController get ctl => widget.ctl;

  String _key(int point) => cvKey(ctl.sel.curveCh, regions[point]);

  /// Absolute: the curve passes through the finger's height at the point's x.
  double _valueAt(int i, double y) {
    final ch = ctl.sel.curveCh;
    double v(String r) => r == regions[i] ? 0 : ctl.edit.value(cvKey(ch, r));
    final base = curveY(xs[i], v('H'), v('L'), v('D'), v('S'));
    final want = ((_size.height - 3 - y) / (_size.height - 6)).clamp(0.0, 1.0);
    return ((want - base) / 0.18 * 100).clamp(-100.0, 100.0).toDouble();
  }

  void _down(Offset p) {
    var best = 0;
    for (var i = 1; i < 4; i++) {
      if ((xs[i] * _size.width - p.dx).abs() < (xs[best] * _size.width - p.dx).abs()) best = i;
    }
    _lock = best;
    ctl.sel.curvePoint = best;
    ctl.valueBegin(_key(best));
    ctl.valueSet(_key(best), _valueAt(best, p.dy));
  }

  void _move(Offset p) {
    final i = _lock;
    if (i != null) ctl.valueSet(_key(i), _valueAt(i, p.dy));
  }

  void _end(Offset p, bool c) {
    if (_lock != null) ctl.valueEnd();
    _lock = null;
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Column(children: [
      Row(children: [
        for (var i = 0; i < 4; i++) ...[
          if (i > 0) const SizedBox(width: 6),
          PanelChip(key: ValueKey('channel-$i'), label: curveChannelNames[i], selected: ctl.sel.curveCh == i, dot: const [Fx.chRgb, Fx.chR, Fx.chG, Fx.chB][i], onTap: () {
            ctl.sel.curveCh = i;
            ctl.edits.value++;
          }),
        ],
      ]),
      const SizedBox(height: 8),
      Expanded(
        child: ClipRRect(
          borderRadius: BorderRadius.circular(Rd.block),
          child: ColoredBox(
            color: c.bg,
            child: LayoutBuilder(builder: (context, cons) {
              _size = Size(cons.maxWidth, cons.maxHeight);
              return ClaimSurface(
                onDown: _down,
                onMove: _move,
                onEnd: _end,
                child: RepaintBoundary(
                  child: ListenableBuilder(
                    listenable: Listenable.merge([ctl.app.editTick, ctl.edits]),
                    builder: (context, _) {
                      final e = ctl.edit;
                      final ch = ctl.sel.curveCh;
                      double v(String r) => e.value(cvKey(ch, r));
                      const colors = [Fx.chRgb, Fx.chR, Fx.chG, Fx.chB];
                      return SizedBox.expand(
                        key: const ValueKey('curve-pad'),
                        child: CustomPaint(painter: CurvePainter(ctl.analysis(), v('H'), v('L'), v('D'), v('S'), ch == 0 ? c.text : colors[ch], c.histFill, c.sep, c.text2.withValues(alpha: .5), sel: ctl.sel.curvePoint, dot: c.surface, showPoints: true)),
                      );
                    },
                  ),
                ),
              );
            }),
          ),
        ),
      ),
    ]);
  }
}

class HslEditor extends StatelessWidget {
  const HslEditor({super.key, required this.ctl});
  final EditorController ctl;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: ctl.edits,
      builder: (context, _) {
        final b = ctl.sel.band, part = ctl.sel.hslPart;
        const parts = ['h', 's', 'l'];
        const names = ['色相', '饱和度', '明亮度'];
        return Column(children: [
          Center(child: BandDots(index: b, onSelect: (i) {
            ctl.sel.band = i;
            ctl.edits.value++;
          })),
          const SizedBox(height: 4),
          Row(children: [
            for (var i = 0; i < 3; i++) ...[
              if (i > 0) const SizedBox(width: 6),
              PanelChip(key: ValueKey('hsl-${parts[i]}'), label: names[i], selected: part == i, onTap: () {
                ctl.sel.hslPart = i;
                ctl.edits.value++;
              }),
            ],
          ]),
          const Spacer(),
          SpecRuler(key: ValueKey('hsl-ruler-$b-$part'), ctl: ctl, valueKey: hslKey(b, parts[part]), label: '${hslBandNames[b]}${names[part]}'),
        ]);
      },
    );
  }
}

class GradeEditor extends StatefulWidget {
  const GradeEditor({super.key, required this.ctl});
  final EditorController ctl;
  @override
  State<GradeEditor> createState() => _GradeEditorState();
}

class _GradeEditorState extends State<GradeEditor> {
  EditorController get ctl => widget.ctl;
  double _side = 150;
  bool _begun = false;

  /// Absolute: the hue is the finger's angle and the saturation its distance from the centre.
  void _set(Offset p) {
    final r = _side / 2 - 2;
    final d = p - Offset(_side / 2, _side / 2);
    var sat = (d.distance / r * 100).clamp(0.0, 100.0).toDouble();
    if (sat < 4) sat = 0;
    var hue = (math.atan2(d.dy, d.dx) * 180 / math.pi) % 360;
    if (hue < 0) hue += 360;
    ctl.wheelSet(ctl.sel.gradeZone, hue.roundToDouble() % 360, sat.roundToDouble());
  }

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: ctl.edits,
      builder: (context, _) {
        final z = ctl.sel.gradeZone, part = ctl.sel.gradePart;
        final keys = [grKey(z, 'l'), 'gr_blend', 'gr_balance'];
        const names = ['明亮度', '混合', '平衡'];
        return Column(children: [
          Row(children: [
            for (var i = 0; i < 4; i++) ...[
              if (i > 0) const SizedBox(width: 6),
              PanelChip(key: ValueKey('zone-$i'), label: gradeZoneNames[i], selected: z == i, onTap: () {
                ctl.sel.gradeZone = i;
                ctl.edits.value++;
              }),
            ],
          ]),
          const SizedBox(height: 8),
          Expanded(
            child: LayoutBuilder(builder: (context, cons) {
              _side = math.min(cons.maxHeight, 170.0);
              return Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
                SizedBox(
                  width: _side,
                  height: _side,
                  child: ClaimSurface(
                    onDown: (p) {
                      ctl.valueBegin(grKey(ctl.sel.gradeZone, 's'));
                      _begun = true;
                      _set(p);
                    },
                    onMove: _set,
                    onEnd: (p, c) {
                      if (_begun) ctl.valueEnd();
                      _begun = false;
                    },
                    child: ListenableBuilder(
                      listenable: ctl.app.editTick,
                      builder: (context, _) => Center(
                        key: const ValueKey('grade-wheel'),
                        child: WheelMini(hue: ctl.edit.value(grKey(z, 'h')), sat: ctl.edit.value(grKey(z, 's')), size: _side),
                      ),
                    ),
                  ),
                ),
                const SizedBox(width: 10),
                Expanded(
                  child: Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
                    Wrap(spacing: 6, runSpacing: 6, children: [
                      for (var i = 0; i < 3; i++)
                        PanelChip(key: ValueKey('grade-${keys[i]}'), label: names[i], selected: part == i, onTap: () {
                          ctl.sel.gradePart = i;
                          ctl.edits.value++;
                        }),
                    ]),
                    const Spacer(),
                    SpecRuler(key: ValueKey('grade-ruler-$z-$part'), ctl: ctl, valueKey: keys[part], label: names[part]),
                  ]),
                ),
              ]);
            }),
          ),
        ]);
      },
    );
  }
}
