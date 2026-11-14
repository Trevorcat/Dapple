// The control pads of the editor (docs/design/editor-panel.md section 3, version 0.6): four categories, each a pad of two
// directions plus a strength strip. A pad position is a *recipe*: a pure function (x, y, strength) -> the values of the
// category's underlying edit parameters. The edit's parameters stay the source of truth for the engine; the pad state
// ([PadState] in [EditState.pads]) only lets a reopened edit restore the pad, and item offsets (the itemised view) are
// added on top of what the recipe says.
//
// Invariant (tested): at the centre (x = y = 0, strength 0.5) every recipe gives exactly the parameter defaults, so an
// untouched edit is the automatic result and opening an old edit shows its values as plain item offsets.
import 'dart:math' as math;
import 'dart:ui' show Offset;

import 'bases.dart';
import 'catalog.dart';
import 'params.dart';

enum PadCat { light, color, film, texture }

extension PadCatX on PadCat {
  String get label => const {PadCat.light: '光线', PadCat.color: '颜色', PadCat.film: '成像', PadCat.texture: '质地'}[this]!;

  /// The editor's category (window manager / deep link / sync) this pad category lives under.
  EGroup get group => const {PadCat.light: EGroup.light, PadCat.color: EGroup.color, PadCat.film: EGroup.style, PadCat.texture: EGroup.effects}[this]!;

  static PadCat? ofGroup(EGroup g) => switch (g) {
        EGroup.light => PadCat.light,
        EGroup.color => PadCat.color,
        EGroup.style => PadCat.film,
        EGroup.effects => PadCat.texture,
        _ => null,
      };

  /// Icon vocabulary of the four edges: left, right, bottom, top.
  List<PadIconKind> get edgeIcons => switch (this) {
        PadCat.light => const [PadIconKind.moon, PadIconKind.sun, PadIconKind.contrastLow, PadIconKind.contrastHigh],
        PadCat.color => const [PadIconKind.cold, PadIconKind.warm, PadIconKind.pale, PadIconKind.rich],
        PadCat.film => const [PadIconKind.pale, PadIconKind.rich, PadIconKind.moon, PadIconKind.sun],
        PadCat.texture => const [PadIconKind.soft, PadIconKind.sharp, PadIconKind.clean, PadIconKind.grain],
      };

  /// Spoken names of the edges: left, right, bottom, top.
  List<String> get edgeWords => switch (this) {
        PadCat.light => const ['暗', '亮', '反差低', '反差高'],
        PadCat.color => const ['冷', '暖', '淡', '浓'],
        PadCat.film => const ['淡', '浓', '暗', '亮'],
        PadCat.texture => const ['柔', '锐', '干净', '颗粒'],
      };
}

/// One character for an edge word, shown beside the edge icon (暗 / 亮, 冷 / 暖, 淡 / 浓, 柔 / 锐, 低 / 高 for contrast, 净 / 粒).
String edgeChar(String word) => const {'反差低': '低', '反差高': '高', '干净': '净', '颗粒': '粒', '通透': '透'}[word] ?? word.substring(0, 1);

/// The line icons of the pad edges (one vocabulary everywhere; drawn by pad_icons.dart).
enum PadIconKind { moon, sun, contrastLow, contrastHigh, cold, warm, pale, rich, soft, sharp, clean, grain, hazy, clear }

/// What the recipes need to know about the app's settings.
class PadCtx {
  const PadCtx({this.defaultTone = 60, this.defaultLookId = 'std'});
  final double defaultTone;
  final String defaultLookId;
}

enum ItemKind { value, tone, look }

/// One parameter of a category as the itemised view (radar + ruler) sees it. Values are in the stored domain.
class PadItem {
  const PadItem(this.id, this.label, this.min, this.max, this.auto, this.fmt, {this.kind = ItemKind.value, this.step = 1, this.group});
  final String id, label;
  final double min, max, auto, step;
  final String Function(double) fmt;
  final ItemKind kind;

  /// Shown under this heading in the spoken description; null = none.
  final String? group;

  bool get bipolar => min < 0;

  /// 0..1 position of [v] on its spoke (bipolar items sit in the middle at zero).
  double norm(double v) => bipolar ? (0.5 + 0.5 * v / max).clamp(0.0, 1.0) : ((v - min) / (max - min)).clamp(0.0, 1.0);

  double fromNorm(double t) => bipolar ? (t - 0.5) * 2 * max : min + t * (max - min);

  double clampV(double v) => v.clamp(min, max).toDouble();
}

String _ev(double v) => fmtValue(Fmt.ev, v);
String _sg(double v) => fmtValue(Fmt.signed, v);
String _pct(double v) => fmtValue(Fmt.pct, v);
String _pl(double v) => '${v.round()}';
String _sg100(double v) => fmtValue(Fmt.signed, v * 100);
/// Latitude ruler scale: the stored offset -1 / 0 / +1 means an in-range guarantee of 55 / 80 / 98 % (piecewise linear).
double latitudePercent(double v) => v <= 0 ? 80 + v * 25 : 80 + v * 18;

String _lat(double v) => '${latitudePercent(v).round()}%';

PadItem _s(String id, String label, String Function(double) f, {double step = 1}) {
  final s = specByKey[id]!;
  return PadItem(id, label, s.min, s.max, s.def, f, step: step);
}

/// The items (spokes of the radar) of [cat]. [printed]: the stock is a printed negative, so the print warmth / tint
/// filters exist.
List<PadItem> padItems(PadCat cat, {bool printed = true, bool modern = false}) {
  switch (cat) {
    case PadCat.light:
      return [
        _s('exposure', '曝光', _ev),
        _s('contrast', '对比度', _sg),
        _s('highlights', '高光', _sg),
        _s('shadows', '阴影', _sg),
        _s('whites', '白色色阶', _sg),
        _s('blacks', '黑色色阶', _sg),
        const PadItem('toneStrength', '明暗强度', 0, 100, 60, _pct, kind: ItemKind.tone),
        // last: with eight spokes the 3rd and 7th sit on the horizontal axis, where a long name costs radar radius
        _s('hdr', '单张 HDR', _pct),
      ];
    case PadCat.color:
      return [
        _s('temp', '色温', _sg),
        _s('tint', '色偏', _sg),
        _s('vibrance', '鲜艳度', _sg),
        _s('saturation', '饱和度', _sg),
        const PadItem('lookStrength', '风格强度', 0, 100, 100, _pct, kind: ItemKind.look),
      ];
    case PadCat.film:
      // a modern rendering has the strength only (the rest is the film pack: shoot, print, black, white balance, latitude, grain, halation)
      if (modern) return [_s('film_strength', '强度', _pct)];
      return [
        _s('film_shoot', '拍摄曝光', _sg100, step: 0.01),
        _s('film_print', '冲印深浅', _sg100, step: 0.01),
        _s('film_strength', '强度', _pct),
        _s('film_grain', '颗粒', _pct),
        _s('film_halation', '光晕', _pct),
        _s('film_black', '黑位', _sg100, step: 0.01),
        _s('film_wb', '白平衡', (v) => '${(v * 100).round()}', step: 0.01),
        _s('film_latitude', '宽容度', _lat, step: 0.01),
        if (printed) _s('film_warmth', '冲印冷暖', _sg),
        if (printed) _s('film_tint', '冲印色调', _sg),
      ];
    case PadCat.texture:
      return [
        _s('clarity', '清晰度', _sg),
        _s('texture', '纹理', _sg),
        _s('sharpen', '锐化', _pl),
        _s('nrLuma', '明亮度降噪', _pl),
        _s('nrColor', '颜色降噪', _pl),
        _s('sharpRadius', '锐化半径', (v) => fmtValue(Fmt.radius, v)),
        _s('grain', '颗粒', _pl),
        _s('dehaze', '去雾', _sg),
        _s('vignette', '暗角', _sg),
      ];
  }
}

/// The item that the strength strip drives (its value is part of the strip's recipe), or null.
String? strengthItemOf(PadCat cat) => switch (cat) {
      PadCat.light => 'toneStrength',
      PadCat.color => 'lookStrength',
      PadCat.film => 'film_strength',
      PadCat.texture => null,
    };

// ------------------------------------------------------------------------------------------------ recipes

double _lerp(double a, double b, double t) => a + (b - a) * t;

/// 0 at the bottom of the strip, 1 in the middle (100 %), 2 at the top.
double strengthK(double a) => a * 2;

/// Automatic-tone strength of the strip position [a]: 0 at the bottom, the user's default in the middle, 100 on top.
double toneStrengthOf(double a, double def) => a <= 0.5 ? def * a * 2 : _lerp(def, 100, a * 2 - 1);

double strengthFromTone(double v, double def) {
  if (def >= 100) return 0.5;
  return v <= def ? (def <= 0 ? 0.5 : v / def / 2) : 0.5 + 0.5 * (v - def) / (100 - def);
}

/// The values the pad position (x: left -1 .. right +1, y: bottom -1 .. top +1) and strength [a] (0..1, 0.5 = 100 %)
/// imply for every item of [cat] (also the conditional ones), in the stored domain. Pure. Centre = the defaults.
Map<String, double> padRecipe(PadCat cat, double x, double y, double a, {PadCtx ctx = const PadCtx()}) {
  final boost = math.max(0.0, a * 2 - 1); // the upper half of the strip
  final low = math.min(1.0, a * 2); // the lower half of the strip: 0 .. 1
  switch (cat) {
    case PadCat.light:
      return {
        'exposure': x * 16,
        'contrast': y * 55,
        'highlights': -x * 40 - y * 15,
        'shadows': x * 20 - y * 15,
        'whites': y * 15,
        'blacks': -y * 15,
        'hdr': 0, // not driven by the pad: the user's own control
        'toneStrength': toneStrengthOf(a, ctx.defaultTone),
      };
    case PadCat.color:
      return {
        'temp': x * 30,
        'tint': 0,
        'vibrance': y * 55 + boost * 20,
        'saturation': y * 25 + boost * 8,
        'lookStrength': 100 * low,
      };
    case PadCat.film:
      return {
        'film_shoot': x * 0.8,
        'film_print': y * 0.65,
        // the bundle: the film effect fades in over the lower half (grain and halation with it); above the middle the
        // effect is complete, grain and halation grow towards the top of their range (read from the param spec: the
        // engine schema allows 200 %) and the black lifts
        'film_strength': 100 * low,
        'film_grain': 100 * low + boost * (specByKey['film_grain']!.max - 100),
        'film_halation': 100 * low + boost * (specByKey['film_halation']!.max - 100),
        'film_black': -0.5 * boost,
        'film_wb': 0,
        'film_latitude': 0,
        'film_warmth': 0,
        'film_tint': 0,
      };
    case PadCat.texture:
      // the strength scales the default sharpening and noise reduction: 0 off, the middle the defaults, the top heavier
      final k = a <= 0.5 ? a * 2 : 1 + boost * 0.8;
      final clean = math.max(0.0, -y); // 干净: more noise reduction
      return {
        'clarity': x * 40,
        'texture': x * 20,
        'sharpen': ((40 + x * 40) * k).clamp(0.0, 150.0),
        'nrLuma': (clean * 60 + boost * 25) * (a <= 0.5 ? low : 1),
        'nrColor': ((25 + clean * 40) * k).clamp(0.0, 100.0),
        'grain': math.max(0.0, y) * 45,
        'sharpRadius': 10,
        'dehaze': 0,
        'vignette': 0,
      };
  }
}

// ------------------------------------------------------------------------------------------------ reading and writing items

double readItem(EditState e, PadItem it, PadCtx ctx) {
  switch (it.kind) {
    case ItemKind.tone:
      return e.toneStrength ?? ctx.defaultTone;
    case ItemKind.look:
      return e.lookStrength;
    case ItemKind.value:
      return e.value(it.id);
  }
}

EditState writeItem(EditState e, PadItem it, double v, PadCtx ctx) {
  final c = it.clampV(v);
  switch (it.kind) {
    case ItemKind.tone:
      return e.copyWith(toneStrength: (c - ctx.defaultTone).abs() < 1e-6 ? null : c);
    case ItemKind.look:
      return e.copyWith(lookStrength: c);
    case ItemKind.value:
      return e.withValue(it.id, c);
  }
}

/// The items of [cat] for the edit's film (printed stocks have the print warmth / tint).
List<PadItem> itemsFor(PadCat cat, EditState e, {bool? printed}) {
  if (cat != PadCat.film) return padItems(cat);
  return padItems(cat, printed: printed ?? true);
}

PadItem? itemById(PadCat cat, String id) {
  for (final it in padItems(cat)) {
    if (it.id == id) return it;
  }
  return null;
}

/// The strip position that an existing value of the strength item stands for (old edits).
double _stripFromEdit(PadCat cat, EditState e, PadCtx ctx) {
  switch (cat) {
    case PadCat.light:
      return strengthFromTone(e.toneStrength ?? ctx.defaultTone, ctx.defaultTone);
    case PadCat.color:
      return (e.lookStrength / 200).clamp(0.0, 0.5);
    case PadCat.film:
      return (e.value('film_strength') / 200).clamp(0.0, 0.5);
    case PadCat.texture:
      return 0.5;
  }
}

/// The pad state of [cat] for [e]: the stored one, or for an edit that has none (an old edit, or an untouched one) the
/// centre with the existing values shown as item offsets.
PadState padStateOf(EditState e, PadCat cat, PadCtx ctx) {
  final s = e.pads[cat.name];
  if (s != null) return s;
  final a = _stripFromEdit(cat, e, ctx);
  final r = padRecipe(cat, 0, 0, a, ctx: ctx);
  final off = <String, double>{};
  for (final it in padItems(cat)) {
    final d = readItem(e, it, ctx) - (r[it.id] ?? it.auto);
    if (d.abs() > 1e-6) off[it.id] = d;
  }
  return PadState(a: a, off: off);
}

/// Reads only the cursor and the strip (cheap; for the dock glyphs and the cursor). Does not derive item offsets.
PadState padViewOf(EditState e, PadCat cat, PadCtx ctx) => e.pads[cat.name] ?? PadState(a: _stripFromEdit(cat, e, ctx));

/// Writes the recipe of [s] plus its offsets into the edit's parameters and stores the pad state.
EditState applyPadState(EditState e, PadCat cat, PadState s, PadCtx ctx) {
  final r = padRecipe(cat, s.x, s.y, s.a, ctx: ctx);
  var out = e;
  for (final it in padItems(cat)) {
    if (it.id == 'film_warmth' || it.id == 'film_tint') {
      if (r[it.id] == 0 && !s.off.containsKey(it.id)) continue; // never touch what the recipe does not move
    }
    out = writeItem(out, it, (r[it.id] ?? it.auto) + (s.off[it.id] ?? 0), ctx);
  }
  if (cat == PadCat.film && isModernId(out.film)) out = dropFilmOnlyValues(out); // the strip's upper half would write grain etc.
  return withPad(out, cat, s);
}

/// The film-pack controls mean nothing for a modern rendering: back to their defaults (film_strength stays).
EditState dropFilmOnlyValues(EditState e) {
  if (!e.values.keys.any((k) => filmValueKeys.contains(k) && k != 'film_strength')) return e;
  final m = Map<String, double>.of(e.values)..removeWhere((k, v) => filmValueKeys.contains(k) && k != 'film_strength');
  return e.copyWith(values: m);
}

/// [e] with the modern rendering [id] as its base: the film-pack controls and the paper go back to their defaults; the strength
/// stays, and the film pad keeps its strength strip but not its cursor (both its axes are dead for a modern base).
EditState withModernBase(EditState e, String id) {
  final p = Map<String, PadState>.of(e.pads);
  final pad = p[PadCat.film.name];
  if (pad != null) p[PadCat.film.name] = PadState(a: pad.a);
  return dropFilmOnlyValues(e).copyWith(film: id, filmPaper: null, pads: p);
}

/// Stores [s] (dropping offsets that are zero; a pad at the centre is simply absent).
EditState withPad(EditState e, PadCat cat, PadState s) {
  final off = {for (final en in s.off.entries) if (en.value.abs() > 1e-6) en.key: en.value};
  final n = s.copyWith(off: off);
  final m = Map<String, PadState>.of(e.pads);
  if (n.isCentre) {
    m.remove(cat.name);
  } else {
    m[cat.name] = n;
  }
  return e.copyWith(pads: m);
}

/// Sets one item to [v] (the itemised view): the value is written as is, the pad keeps its position and the difference to
/// what the pad implies becomes the item's offset.
EditState setItemValue(EditState e, PadCat cat, PadItem it, double v, PadCtx ctx) {
  final s = padStateOf(e, cat, ctx);
  final r = padRecipe(cat, s.x, s.y, s.a, ctx: ctx);
  final c = it.clampV(v);
  final off = Map<String, double>.of(s.off);
  off[it.id] = c - (r[it.id] ?? it.auto);
  return withPad(writeItem(e, it, c, ctx), cat, s.copyWith(off: off));
}

/// Everything the pad implies for [cat] at its current state (the dashed polygon of the radar).
Map<String, double> padImplied(EditState e, PadCat cat, PadCtx ctx) {
  final s = padStateOf(e, cat, ctx);
  return padRecipe(cat, s.x, s.y, s.a, ctx: ctx);
}

/// Resets the category: the pad to the centre, the strip to 100 %, the offsets away (the film stays: the 无 button clears it).
EditState resetPadCat(EditState e, PadCat cat, PadCtx ctx) {
  var out = e;
  final r = padRecipe(cat, 0, 0, 0.5, ctx: ctx);
  for (final it in padItems(cat)) {
    out = writeItem(out, it, r[it.id] ?? it.auto, ctx);
  }
  final m = Map<String, PadState>.of(out.pads)..remove(cat.name);
  out = out.copyWith(pads: m);
  switch (cat) {
    case PadCat.light:
      return out.copyWith(toneTarget: null);
    case PadCat.color:
      return out.copyWith(look: null, palette: null);
    case PadCat.film:
      return out.copyWith(filmPaper: null);
    case PadCat.texture:
      return out;
  }
}

/// Whether the category carries any edit (the dock glyph's lit state, the header reset).
bool padCatAdjusted(EditState e, PadCat cat, PadCtx ctx) {
  if (e.pads.containsKey(cat.name)) return true;
  switch (cat) {
    case PadCat.light:
      return e.toneTarget != null || e.toneStrength != null || e.groupAdjusted(EGroup.light) || e.groupAdjusted(EGroup.curve);
    case PadCat.color:
      return e.look != null || e.palette != null || e.lookStrength != 100 || e.groupAdjusted(EGroup.color) || e.groupAdjusted(EGroup.grade);
    case PadCat.film:
      return e.film != null || e.values.keys.any(filmValueKeys.contains);
    case PadCat.texture:
      return e.groupAdjusted(EGroup.effects) || e.groupAdjusted(EGroup.detail);
  }
}

// ------------------------------------------------------------------------------------------------ the old palette

/// Pad coordinates of a look palette point (x: pale -> rich, y: warm -> cool, both 0..1): the colour pad's warm / cool is x and
/// pale / rich is y. The same mapping places the style presets, which sit at their looks' own palette points.
Offset palettePadPosition(Offset palette) =>
    Offset(((0.5 - palette.dy) * 2.4).clamp(-1.0, 1.0).toDouble(), ((palette.dx - 0.5) * 2.4).clamp(-1.0, 1.0).toDouble());

/// The engine's palette (the look's 2D colour steering, warm / cool by pale / rich) is the colour pad's job now: an old edit
/// that carries one opens with the cursor where the palette was (its existing values stay as offsets), and the palette
/// itself is dropped (it is not sent any more).
EditState migratePalette(EditState e, {PadCtx ctx = const PadCtx()}) {
  final p = e.palette;
  if (p == null) return e;
  var n = e.copyWith(palette: null);
  if (!n.pads.containsKey('color')) {
    final pos = palettePadPosition(p);
    final s = padStateOf(n, PadCat.color, ctx);
    n = applyPadState(n, PadCat.color, s.copyWith(x: pos.dx, y: pos.dy), ctx);
  }
  return n;
}

// ------------------------------------------------------------------------------------------------ colour presets

/// A style preset dot on the colour pad: the look it selects and where its dot sits (pad coordinates).
class ColorPreset {
  const ColorPreset(this.look, this.x, this.y);
  final String look;
  final double x, y;
  String get name => lookById(look).name;
}

/// Positions follow the looks' own character (warm .. cold from the palette y, pale .. rich from the palette x).
final List<ColorPreset> colorPresets = [
  for (final id in const ['warm', 'amber', 'rose', 'neutral', 'cool', 'teal', 'bw'])
    () {
      final l = lookById(id);
      if (id == 'bw') return const ColorPreset('bw', 0.0, -0.6);
      final pos = palettePadPosition(Offset(l.px, l.py));
      return ColorPreset(id, pos.dx.clamp(-0.9, 0.9), pos.dy.clamp(-0.9, 0.9));
    }(),
];

// ------------------------------------------------------------------------------------------------ film pad limits

/// Reachable pad coordinates of the film pad (an end that does not change the render is unreachable).
class PadLimits {
  const PadLimits({this.xLo = -1, this.xHi = 1, this.yLo = -1, this.yHi = 1});
  static const full = PadLimits();
  final double xLo, xHi, yLo, yHi;

  Offset clamp(double x, double y) => Offset(x.clamp(xLo, xHi).toDouble(), y.clamp(yLo, yHi).toDouble());

  bool get isFull => xLo == -1 && xHi == 1 && yLo == -1 && yHi == 1;
}

// ------------------------------------------------------------------------------------------------ readouts

/// The line in the panel header: strength, then the two directions.
String padReadout(PadCat cat, PadState s, {PadCtx ctx = const PadCtx()}) {
  final w = cat.edgeWords;
  String dir(double v, String neg, String pos) => v == 0 ? '' : ' · ${v > 0 ? pos : neg} ${(v.abs() * 100).round()}';
  return '${stripReadout(stripPercent(cat, s.a, ctx: ctx))}${dir(s.x, w[0], w[1])}${dir(s.y, w[2], w[3])}';
}

/// The strength strip, one meaning everywhere (guidelines 15.1-5, audit U1): the bottom is no effect, the middle is the amount Dapple
/// recommends and reads 100 %, the top is the most this control gives, read as its real percentage of the recommended amount. The
/// strip's mapping to the parameters is unchanged (saved edits look the same); only what it reads changed:
///  光线  middle = the user's default tone strength (settings, 60 unless changed), top = tone strength 100: 100 / default (167 %);
///  颜色  middle = style strength 100; the upper half adds vividness and saturation on top: 200 % at the top;
///  成像  middle = the full film effect (film strength 100); the upper half grows grain and halation to their range maxima (2x) and
///        lifts the black: 200 % at the top;
///  质地  middle = the default sharpening and noise reduction, top = 1.8x of them: 180 %.
double stripPercent(PadCat cat, double a, {PadCtx ctx = const PadCtx()}) {
  final t = a.clamp(0.0, 1.0);
  switch (cat) {
    case PadCat.light:
      final def = ctx.defaultTone;
      return def <= 0 ? t * 200 : toneStrengthOf(t, def) / def * 100;
    case PadCat.color:
    case PadCat.film:
      return t * 200;
    case PadCat.texture:
      return (t <= 0.5 ? t * 2 : 1 + (t * 2 - 1) * 0.8) * 100;
  }
}

/// The header readout and the strip's screen-reader value: the same words (guidelines 12.3).
String stripReadout(double percent) => '强度 ${percent.round()}%';
