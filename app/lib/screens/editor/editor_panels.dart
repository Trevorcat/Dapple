// Content of the 8 adjustment groups (requirements 4.5-4.7), shared by the phone sheet and the
// large-screen inspector (`large` switches the few layout differences).
import 'package:flutter/material.dart';

import '../../model/catalog.dart';
import '../../model/params.dart';
import '../../models.dart';
import '../../services/editor_engine.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/icons.dart';
import '../../widgets/photo_art.dart';
import 'editor_widgets.dart';

/// UI-only selections inside groups (kept by the editor so they survive resizes).
class PanelUi {
  int band = 1; // 混色 默认橙
  int curveCh = 0;
  int gradeZone = 0;
}

const groupIcons = {
  EGroup.style: DI.styles,
  EGroup.light: DI.light,
  EGroup.color: DI.color,
  EGroup.curve: DI.curve,
  EGroup.effects: DI.effects,
  EGroup.detail: DI.detail,
  EGroup.grade: DI.grade,
  EGroup.crop: DI.crop,
};

class EditorCtx {
  EditorCtx({
    required this.app,
    required this.photo,
    required this.engine,
    required this.large,
    required this.analysis,
    required this.ui,
    required this.jump,
    required this.rebuild,
  });
  final AppState app;
  final Photo photo;
  final EditorEngine engine;
  final bool large;
  final Analysis analysis;
  final PanelUi ui;
  final void Function(EGroup g) jump;
  final VoidCallback rebuild;

  EditState get edit => photo.edit;
  void update(EditState Function(EditState e) f, {String? key}) => app.applyEdit(photo.id, f, coalesce: key);
  void setVal(String k, double v) => update((e) => e.withValue(k, v), key: k);
}

Color hsl(double h, double s, double l) => HSLColor.fromAHSL(1, ((h % 360) + 360) % 360, s, l).toColor();

Widget _gap([double h = 8]) => SizedBox(height: h);

Widget _sectionTitle(BuildContext context, String t) {
  final c = context.c;
  return Padding(
    padding: const EdgeInsets.only(top: 4, bottom: 8),
    child: Text(t, style: T.body(c.accent, w: FontWeight.w500)),
  );
}

/// One slider row for a [SliderSpec] key.
Widget specSlider(EditorCtx x, String key, {List<Color>? gradient}) {
  final s = specByKey[key]!;
  final v = x.edit.value(key);
  return DSlider(
    keyPrefix: key,
    label: s.label,
    value: v,
    min: s.min,
    max: s.max,
    def: s.def,
    display: fmtSpec(s, v),
    gradient: gradient,
    onChanged: (n) => x.setVal(key, n),
  );
}

List<Color> _trackFor(TrackKind k, {double hue = 0}) {
  switch (k) {
    case TrackKind.temp:
      return Fx.tempTrack;
    case TrackKind.tint:
      return Fx.tintTrack;
    case TrackKind.hue:
    case TrackKind.gradeHue:
      return Fx.hueTrack;
    case TrackKind.mixHue:
      return [hsl(hue - 40, .8, .52), hsl(hue, .8, .52), hsl(hue + 40, .8, .52)];
    case TrackKind.mixSat:
      return [const Color(0xFF9A9A9A), hsl(hue, .8, .52)];
    case TrackKind.mixLum:
      return [hsl(hue, .8, .2), hsl(hue, .8, .52), hsl(hue, .8, .88)];
    case TrackKind.gradeSat:
      return [const Color(0xFF9A9A9A), hsl(hue, .9, .5)];
    case TrackKind.plain:
      return const [];
  }
}

Widget buildGroup(BuildContext context, EGroup g, EditorCtx x) {
  switch (g) {
    case EGroup.style:
      return _StyleGroup(x);
    case EGroup.light:
      return Column(children: [
        if (!x.large) ...[MiniHistogram(analysis: x.analysis), _gap()], // wide layout pins the full histogram
        for (final s in specsOf(EGroup.light)) specSlider(x, s.key),
      ]);
    case EGroup.color:
      return _ColorGroup(x);
    case EGroup.curve:
      return _CurveGroup(x);
    case EGroup.effects:
      return Column(children: [for (final s in specsOf(EGroup.effects)) specSlider(x, s.key)]);
    case EGroup.detail:
      return Column(children: [for (final s in specsOf(EGroup.detail)) specSlider(x, s.key)]);
    case EGroup.grade:
      return _GradeGroup(x);
    case EGroup.crop:
      return _CropGroup(x);
  }
}

// ------------------------------------------------------------------ 风格

class _StyleGroup extends StatelessWidget {
  const _StyleGroup(this.x);
  final EditorCtx x;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final e = x.edit;
    final nearest = x.analysis.nearest;
    final target = e.toneTarget ?? nearest;
    final tone = e.toneStrength ?? x.app.settings.toneStrength;
    final lookId = e.look ?? x.app.settings.defaultLookId;
    final look = lookById(lookId);
    final pal = e.palette ?? Offset(look.px, look.py);
    final swatches = [
      for (final l in dLooks)
        Semantics(
          checked: l.id == lookId,
          inMutuallyExclusiveGroup: true,
          label: l.name,
          excludeSemantics: true,
          child: GestureDetector(
            key: ValueKey('look-${l.id}'),
            behavior: HitTestBehavior.opaque,
            onTap: () => x.update((s) => s.copyWith(look: l.id == x.app.settings.defaultLookId ? null : l.id, palette: null)),
            child: SizedBox(
              width: 52,
              child: Column(mainAxisSize: MainAxisSize.min, children: [
                Container(
                  width: 46,
                  height: 46,
                  alignment: Alignment.center,
                  decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      border: l.id == lookId ? Border.all(color: c.accentBtn, width: 2) : null),
                  child: Container(
                    width: 38,
                    height: 38,
                    decoration: BoxDecoration(
                        shape: BoxShape.circle,
                        gradient: LinearGradient(
                            begin: Alignment.topLeft,
                            end: Alignment.bottomRight,
                            colors: l.id == 'teal'
                                ? [l.swatch[0], l.swatch[0], l.swatch[1], l.swatch[1]]
                                : l.swatch,
                            stops: l.id == 'teal' ? const [0, .48, .52, 1] : null)),
                  ),
                ),
                const SizedBox(height: 4),
                Text(l.name,
                    style: T.mini(l.id == lookId ? c.text : c.text2, w: l.id == lookId ? FontWeight.w500 : FontWeight.w400)),
              ]),
            ),
          ),
        ),
    ];
    final pad = PalettePad(
      value: pal,
      size: x.large ? 112 : 148,
      onChanged: (p) => x.update((s) => s.copyWith(palette: p), key: 'palette'),
    );
    final nameCol = Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
      Text(look.name, key: const ValueKey('look-name'), style: T.body17(c.text, w: FontWeight.w500)),
      const SizedBox(height: 4),
      ActionChip32('回到默认', color: c.chip, onPressed: () => x.update((s) => s.copyWith(palette: null))),
    ]);
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      _sectionTitle(context, '明暗'),
      if (!x.large) ...[HistogramCard(analysis: x.analysis, target: e.toneTarget), _gap(12)],
      ToneGrid(
        photo: x.photo,
        engine: x.engine,
        demoEdit: DemoEdit.of(x.app, x.photo),
        nearest: nearest,
        selected: target,
        large: x.large,
        onSelect: (id) => x.update((s) => s.copyWith(toneTarget: id)),
      ),
      _gap(12),
      DSlider(
        keyPrefix: 'toneStrength',
        label: '明暗强度',
        value: tone,
        min: 0,
        max: 100,
        def: x.app.settings.toneStrength,
        display: fmtValue(Fmt.pct, tone),
        onChanged: (v) => x.update(
            (s) => s.copyWith(toneStrength: v == x.app.settings.toneStrength ? null : v),
            key: 'toneStrength'),
      ),
      Divider(height: 24, thickness: 1, color: context.c.sep),
      _sectionTitle(context, '风格'),
      if (x.large)
        Wrap(runSpacing: 8, children: swatches)
      else
        SizedBox(height: 74, child: ListView(scrollDirection: Axis.horizontal, children: swatches)),
      _gap(12),
      Row(crossAxisAlignment: CrossAxisAlignment.start, children: [pad, const SizedBox(width: 16), Expanded(child: nameCol)]),
      _gap(8),
      DSlider(
        keyPrefix: 'lookStrength',
        label: '风格强度',
        value: e.lookStrength,
        min: 0,
        max: 100,
        def: 100,
        display: fmtValue(Fmt.pct, e.lookStrength),
        onChanged: (v) => x.update((s) => s.copyWith(lookStrength: v), key: 'lookStrength'),
      ),
      _gap(8),
      Row(children: [
        Expanded(child: _EntryButton('颜色', () => x.jump(EGroup.color), const ValueKey('entry-color'))),
        const SizedBox(width: 12),
        Expanded(child: _EntryButton('分级', () => x.jump(EGroup.grade), const ValueKey('entry-grade'))),
      ]),
    ]);
  }
}

class _EntryButton extends StatelessWidget {
  const _EntryButton(this.label, this.onTap, this.k);
  final String label;
  final VoidCallback onTap;
  final Key k;
  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      button: true,
      label: label,
      excludeSemantics: true,
      child: GestureDetector(
        key: k,
        behavior: HitTestBehavior.opaque,
        onTap: onTap,
        child: Container(
          height: 52,
          padding: const EdgeInsets.symmetric(horizontal: 16),
          decoration: BoxDecoration(color: c.bg, borderRadius: BorderRadius.circular(Rd.tile)),
          child: Row(children: [
            Expanded(child: Text(label, style: T.body(c.text))),
            DIcon(DI.forward, size: 18, color: c.text2),
          ]),
        ),
      ),
    );
  }
}

// ------------------------------------------------------------------ 颜色

class _ColorGroup extends StatelessWidget {
  const _ColorGroup(this.x);
  final EditorCtx x;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final b = x.ui.band;
    final hue = hslBandHue[b].toDouble();
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      specSlider(x, 'temp', gradient: Fx.tempTrack),
      specSlider(x, 'tint', gradient: Fx.tintTrack),
      specSlider(x, 'vibrance'),
      specSlider(x, 'saturation'),
      Divider(height: 24, thickness: 1, color: context.c.sep),
      Text('混色', style: T.body(c.text2, w: FontWeight.w500)),
      const SizedBox(height: 4),
      Semantics(
        container: true,
        label: '色范围',
        child: Row(mainAxisAlignment: MainAxisAlignment.spaceBetween, children: [
          for (var i = 0; i < 8; i++)
            Semantics(
              checked: i == b,
              inMutuallyExclusiveGroup: true,
              label: hslBandNames[i],
              excludeSemantics: true,
              child: GestureDetector(
                key: ValueKey('band-$i'),
                behavior: HitTestBehavior.opaque,
                onTap: () {
                  x.ui.band = i;
                  x.rebuild();
                },
                child: SizedBox(
                  width: 36,
                  height: 48,
                  child: Center(
                    child: Container(
                      width: 34,
                      height: 34,
                      alignment: Alignment.center,
                      decoration: BoxDecoration(
                          shape: BoxShape.circle,
                          border: i == b ? Border.all(color: c.text, width: 2) : null),
                      child: Container(
                          width: 26,
                          height: 26,
                          decoration: BoxDecoration(shape: BoxShape.circle, color: hsl(hslBandHue[i].toDouble(), .8, .52))),
                    ),
                  ),
                ),
              ),
            ),
        ]),
      ),
      specSlider(x, hslKey(b, 'h'), gradient: _trackFor(TrackKind.mixHue, hue: hue)),
      specSlider(x, hslKey(b, 's'), gradient: _trackFor(TrackKind.mixSat, hue: hue)),
      specSlider(x, hslKey(b, 'l'), gradient: _trackFor(TrackKind.mixLum, hue: hue)),
    ]);
  }
}

// ------------------------------------------------------------------ 曲线

class _CurveGroup extends StatelessWidget {
  const _CurveGroup(this.x);
  final EditorCtx x;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final ch = x.ui.curveCh;
    const colors = [Fx.chRgb, Fx.chR, Fx.chG, Fx.chB];
    double v(String r) => x.edit.value(cvKey(ch, r));
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Semantics(
        container: true,
        label: '通道',
        child: SingleChildScrollView(
          scrollDirection: Axis.horizontal,
          child: Row(children: [
            for (var i = 0; i < 4; i++)
              Semantics(
                checked: i == ch,
                inMutuallyExclusiveGroup: true,
                label: curveChannelNames[i],
                excludeSemantics: true,
                child: GestureDetector(
                  key: ValueKey('channel-${curveChannels[i]}'),
                  behavior: HitTestBehavior.opaque,
                  onTap: () {
                    x.ui.curveCh = i;
                    x.rebuild();
                  },
                  child: Container(
                    height: 44,
                    margin: const EdgeInsets.only(right: 8),
                    padding: const EdgeInsets.symmetric(horizontal: 16),
                    decoration: BoxDecoration(color: i == ch ? c.chip : null, borderRadius: BorderRadius.circular(22)),
                    child: Row(mainAxisSize: MainAxisSize.min, children: [
                      Container(width: 10, height: 10, decoration: BoxDecoration(color: colors[i], shape: BoxShape.circle)),
                      const SizedBox(width: 10),
                      Text(curveChannelNames[i], style: T.body(c.text, w: i == ch ? FontWeight.w500 : FontWeight.w400)),
                    ]),
                  ),
                ),
              ),
          ]),
        ),
      ),
      const SizedBox(height: 8),
      ClipRRect(
        borderRadius: BorderRadius.circular(Rd.card),
        child: Container(
          height: 120,
          width: double.infinity,
          color: c.bg,
          child: CustomPaint(
              painter: CurvePainter(x.analysis, v('H'), v('L'), v('D'), v('S'), ch == 0 ? c.text : colors[ch], c.histFill,
                  c.sep, c.text2.withValues(alpha: .5))),
        ),
      ),
      const SizedBox(height: 8),
      for (final r in const ['H', 'L', 'D', 'S']) specSlider(x, cvKey(ch, r)),
    ]);
  }
}

// ------------------------------------------------------------------ 分级

class _GradeGroup extends StatelessWidget {
  const _GradeGroup(this.x);
  final EditorCtx x;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final z = x.ui.gradeZone;
    final hue = x.edit.value(grKey(z, 'h'));
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Semantics(
        container: true,
        label: '区域',
        child: Row(children: [
          for (var i = 0; i < 4; i++) ...[
            if (i > 0) const SizedBox(width: 8),
            Expanded(
              child: Semantics(
                checked: i == z,
                inMutuallyExclusiveGroup: true,
                label: gradeZoneNames[i],
                excludeSemantics: true,
                child: GestureDetector(
                  key: ValueKey('zone-${gradeZones[i]}'),
                  behavior: HitTestBehavior.opaque,
                  onTap: () {
                    x.ui.gradeZone = i;
                    x.rebuild();
                  },
                  child: Container(
                    height: 56,
                    decoration: BoxDecoration(
                      color: c.chip,
                      borderRadius: BorderRadius.circular(Rd.tile),
                      border: i == z ? Border.all(color: c.accentBtn, width: 2) : null,
                    ),
                    child: Column(mainAxisAlignment: MainAxisAlignment.center, children: [
                      Container(
                        width: 18,
                        height: 18,
                        decoration: BoxDecoration(
                          shape: BoxShape.circle,
                          color: () {
                            final s = x.edit.value(grKey(i, 's'));
                            return s == 0 ? Colors.white : hsl(x.edit.value(grKey(i, 'h')), s / 100, .5);
                          }(),
                          border: Border.all(color: c.sep),
                        ),
                      ),
                      const SizedBox(height: 4),
                      Text(gradeZoneNames[i], style: T.small(c.text, w: FontWeight.w500)),
                    ]),
                  ),
                ),
              ),
            ),
          ],
        ]),
      ),
      const SizedBox(height: 8),
      specSlider(x, grKey(z, 'h'), gradient: Fx.hueTrack),
      specSlider(x, grKey(z, 's'), gradient: _trackFor(TrackKind.gradeSat, hue: hue)),
      specSlider(x, grKey(z, 'l')),
      specSlider(x, 'gr_blend'),
      specSlider(x, 'gr_balance'),
    ]);
  }
}

// ------------------------------------------------------------------ 裁切

class _CropGroup extends StatelessWidget {
  const _CropGroup(this.x);
  final EditorCtx x;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final e = x.edit;
    Widget btn(String label, DI icon, VoidCallback onTap, Key k, {bool on = false}) => Expanded(
          child: Semantics(
            button: true,
            toggled: on,
            label: label,
            excludeSemantics: true,
            child: GestureDetector(
              key: k,
              behavior: HitTestBehavior.opaque,
              onTap: onTap,
              child: Container(
                height: 48,
                decoration: BoxDecoration(
                    color: on ? c.accentSoft : c.chip, borderRadius: BorderRadius.circular(Rd.ctaMd)),
                alignment: Alignment.center,
                child: Row(mainAxisSize: MainAxisSize.min, children: [
                  DIcon(icon, size: 18, color: on ? c.accent : c.text),
                  const SizedBox(width: 6),
                  Flexible(
                      child: Text(label,
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: T.small(on ? c.accent : c.text, w: FontWeight.w500))),
                ]),
              ),
            ),
          ),
        );
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Semantics(
        container: true,
        label: '裁切比例',
        child: Column(children: [
          for (var r = 0; r < 2; r++)
            Padding(
              padding: EdgeInsets.only(bottom: r == 0 ? 8 : 0),
              child: Row(children: [
                for (var i = 0; i < 3; i++) ...[
                  if (i > 0) const SizedBox(width: 8),
                  Expanded(
                    child: Builder(builder: (context) {
                      final idx = r * 3 + i;
                      final on = e.crop == cropRatios[idx];
                      return Semantics(
                        checked: on,
                        inMutuallyExclusiveGroup: true,
                        label: cropRatioNames[idx],
                        excludeSemantics: true,
                        child: GestureDetector(
                          key: ValueKey('crop-${cropRatios[idx]}'),
                          behavior: HitTestBehavior.opaque,
                          onTap: () => x.update((s) => s.copyWith(crop: cropRatios[idx])),
                          child: Container(
                            height: 44,
                            alignment: Alignment.center,
                            decoration: BoxDecoration(
                              color: on ? c.accentSoft : c.chip,
                              borderRadius: BorderRadius.circular(Rd.tile),
                              border: on ? Border.all(color: c.accent, width: 2) : null,
                            ),
                            child: Text(cropRatioNames[idx],
                                style: T.body(on ? c.accent : c.text, w: on ? FontWeight.w500 : FontWeight.w400)),
                          ),
                        ),
                      );
                    }),
                  ),
                ],
              ]),
            ),
        ]),
      ),
      const SizedBox(height: 8),
      specSlider(x, 'straighten'),
      const SizedBox(height: 8),
      Row(children: [
        btn('向左旋转', DI.rotateL, () => x.update((s) => s.copyWith(rot: (s.rot + 270) % 360)), const ValueKey('rotate-left')),
        const SizedBox(width: 8),
        btn('向右旋转', DI.rotateR, () => x.update((s) => s.copyWith(rot: (s.rot + 90) % 360)), const ValueKey('rotate-right')),
        const SizedBox(width: 8),
        btn('水平翻转', DI.flip, () => x.update((s) => s.copyWith(flip: !s.flip)), const ValueKey('flip'), on: e.flip),
      ]),
    ]);
  }
}
