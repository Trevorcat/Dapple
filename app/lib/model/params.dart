// Edit parameter contract (docs/design/requirements.md chapter 6).
// Keys here are exactly the JSON keys sent to the engine through
// dapple_session_render; the engine ignores keys it does not know yet.
import 'dart:ui' show Offset;

import 'local_adjust.dart';
import 'pad_model.dart' show migratePalette;

enum EGroup { style, light, color, curve, effects, detail, grade, crop, local }

const groupLabels = {
  EGroup.style: '风格',
  EGroup.light: '光线',
  EGroup.color: '颜色',
  EGroup.curve: '曲线',
  EGroup.effects: '效果',
  EGroup.detail: '细节',
  EGroup.grade: '分级',
  EGroup.crop: '裁切',
  EGroup.local: '局部',
};

enum Fmt { signed, ev, plain, pct, deg, hue, radius }

enum TrackKind { plain, temp, tint, hue, mixHue, mixSat, mixLum, gradeHue, gradeSat }

class SliderSpec {
  const SliderSpec(this.key, this.label, this.min, this.max, this.def, this.fmt, this.group, {this.track = TrackKind.plain});
  final String key;
  final String label;
  final double min, max, def;
  final Fmt fmt;
  final EGroup group;
  final TrackKind track;

  bool get bipolar => min < 0 && def == 0;
}

const hslBands = ['red', 'orange', 'yellow', 'green', 'aqua', 'blue', 'purple', 'magenta'];
const hslBandNames = ['红', '橙', '黄', '绿', '青', '蓝', '紫', '洋红'];
const hslBandHue = [0, 28, 52, 120, 180, 220, 272, 318];
const curveChannels = ['rgb', 'r', 'g', 'b'];
const curveChannelNames = ['RGB', '红', '绿', '蓝'];
const gradeZones = ['shadows', 'mid', 'highlights', 'global'];
const gradeZoneNames = ['阴影', '中间调', '高光', '全局'];
const cropRatios = ['orig', '1:1', '4:5', '3:4', '16:9', '9:16'];
const cropRatioNames = ['原比例', '1:1', '4:5', '3:4', '16:9', '9:16'];

String hslKey(int band, String c) => 'hsl_${hslBands[band]}_$c';
String cvKey(int ch, String r) => 'cv_${curveChannels[ch]}_$r';
String grKey(int zone, String c) => 'gr_${gradeZones[zone]}_$c';

final List<SliderSpec> sliderSpecs = _buildSpecs();
final Map<String, SliderSpec> specByKey = {for (final s in sliderSpecs) s.key: s};

List<SliderSpec> specsOf(EGroup g) => sliderSpecs.where((s) => s.group == g).toList();

List<SliderSpec> _buildSpecs() {
  const g = EGroup.light;
  final out = <SliderSpec>[
    const SliderSpec('exposure', '曝光', -100, 100, 0, Fmt.ev, g),
    const SliderSpec('contrast', '对比度', -100, 100, 0, Fmt.signed, g),
    const SliderSpec('highlights', '高光', -100, 100, 0, Fmt.signed, g),
    const SliderSpec('shadows', '阴影', -100, 100, 0, Fmt.signed, g),
    const SliderSpec('whites', '白色色阶', -100, 100, 0, Fmt.signed, g),
    const SliderSpec('blacks', '黑色色阶', -100, 100, 0, Fmt.signed, g),
    const SliderSpec('hdr', '单张 HDR', 0, 100, 0, Fmt.pct, g),
    const SliderSpec('temp', '色温', -100, 100, 0, Fmt.signed, EGroup.color, track: TrackKind.temp),
    const SliderSpec('tint', '色偏', -100, 100, 0, Fmt.signed, EGroup.color, track: TrackKind.tint),
    const SliderSpec('vibrance', '鲜艳度', -100, 100, 0, Fmt.signed, EGroup.color),
    const SliderSpec('saturation', '饱和度', -100, 100, 0, Fmt.signed, EGroup.color),
    const SliderSpec('clarity', '清晰度', -100, 100, 0, Fmt.signed, EGroup.effects),
    const SliderSpec('texture', '纹理', -100, 100, 0, Fmt.signed, EGroup.effects),
    const SliderSpec('dehaze', '去朦胧', -100, 100, 0, Fmt.signed, EGroup.effects),
    const SliderSpec('vignette', '暗角', -100, 100, 0, Fmt.signed, EGroup.effects),
    const SliderSpec('grain', '颗粒', 0, 100, 0, Fmt.plain, EGroup.effects),
    const SliderSpec('sharpen', '锐化', 0, 150, 40, Fmt.plain, EGroup.detail),
    const SliderSpec('sharpRadius', '锐化半径', 5, 30, 10, Fmt.radius, EGroup.detail),
    const SliderSpec('nrLuma', '明亮度降噪', 0, 100, 0, Fmt.plain, EGroup.detail),
    const SliderSpec('nrColor', '颜色降噪', 0, 100, 25, Fmt.plain, EGroup.detail),
    const SliderSpec('gr_blend', '混合', 0, 100, 50, Fmt.plain, EGroup.grade),
    const SliderSpec('gr_balance', '平衡', -100, 100, 0, Fmt.signed, EGroup.grade),
    const SliderSpec('straighten', '拉直', -45, 45, 0, Fmt.deg, EGroup.crop),
    // Film simulation (docs/film/README.md). Strength / grain / halation 100 = the full effect of the stock.
    const SliderSpec('film_strength', '胶片强度', 0, 100, 100, Fmt.pct, EGroup.style),
    const SliderSpec('film_grain', '颗粒', 0, 200, 100, Fmt.pct, EGroup.style), // the engine schema (core/src/edit_params.cpp) allows 0..200
    const SliderSpec('film_halation', '光晕', 0, 200, 100, Fmt.pct, EGroup.style),
    const SliderSpec('film_warmth', '冲印冷暖', -100, 100, 0, Fmt.signed, EGroup.style),
    const SliderSpec('film_tint', '冲印色调', -100, 100, 0, Fmt.signed, EGroup.style),
    // Old edits only: the 胶片曝光 slider left the UI (replaced by the 曝光 pad: film_shoot / film_print); the field stays.
    const SliderSpec('film_exp', '胶片曝光', -60, 60, 0, Fmt.ev, EGroup.style),
    // Film exposure pad and the black-point / latitude rulers (docs/design/editor-panel.md §5): normalised offsets in [-1, +1], 0 = automatic.
    // NATIVE BINDING: the engine reads these five keys (film_wb included) from the edit JSON as plain floats, missing = 0 (they travel in
    // EditState.toWire like the other film_* keys; nothing else on the Dart side is needed). Their effective ranges per photo and stock come from dapple_session_film_pad_json (model/film_pad.dart).
    const SliderSpec('film_shoot', '拍摄曝光', -1, 1, 0, Fmt.signed, EGroup.style), // X of 曝光: 欠曝 -1 .. 过曝 +1
    const SliderSpec('film_print', '冲印深浅', -1, 1, 0, Fmt.signed, EGroup.style), // Y of 曝光: 深 -1 .. 浅 +1
    const SliderSpec('film_wb', '胶片白平衡', 0, 1, 0, Fmt.signed, EGroup.style), // X of 质感, one-sided: 0 数码白平衡 (default) .. 1 胶片白平衡 (the stock's own balance)
    const SliderSpec('film_black', '黑位', -1, 1, 0, Fmt.signed, EGroup.style), // Y of 质感: -1 抬黑 (matte lifted black), 0 paper black, +1 压黑 (deeper)
    const SliderSpec('film_latitude', '宽容度', -1, 1, 0, Fmt.signed, EGroup.style), // ruler 宽容度: -1..+1 = 55..98 % (0 = 80 %)
  ];
  for (var b = 0; b < 8; b++) {
    out.add(SliderSpec(hslKey(b, 'h'), '${hslBandNames[b]} · 色相', -100, 100, 0, Fmt.signed, EGroup.color,
        track: TrackKind.mixHue));
    out.add(SliderSpec(hslKey(b, 's'), '${hslBandNames[b]} · 饱和度', -100, 100, 0, Fmt.signed, EGroup.color,
        track: TrackKind.mixSat));
    out.add(SliderSpec(hslKey(b, 'l'), '${hslBandNames[b]} · 明亮度', -100, 100, 0, Fmt.signed, EGroup.color,
        track: TrackKind.mixLum));
  }
  for (var c = 0; c < 4; c++) {
    const names = {'H': '高光区', 'L': '亮部', 'D': '暗部', 'S': '阴影区'};
    for (final r in const ['H', 'L', 'D', 'S']) {
      out.add(SliderSpec(cvKey(c, r), names[r]!, -100, 100, 0, Fmt.signed, EGroup.curve));
    }
  }
  for (var z = 0; z < 4; z++) {
    out.add(SliderSpec(grKey(z, 'h'), '${gradeZoneNames[z]} · 色相', 0, 360, 0, Fmt.deg, EGroup.grade,
        track: TrackKind.gradeHue));
    out.add(SliderSpec(grKey(z, 's'), '${gradeZoneNames[z]} · 饱和度', 0, 100, 0, Fmt.plain, EGroup.grade,
        track: TrackKind.gradeSat));
    out.add(SliderSpec(grKey(z, 'l'), '${gradeZoneNames[z]} · 明亮度', -100, 100, 0, Fmt.signed, EGroup.grade));
  }
  return out;
}

String _signed(num v) => v == 0 ? '0' : (v > 0 ? '+${v.round()}' : '−${(-v).round()}');

/// Display text (requirements 4.5). Minus sign is U+2212.
String fmtValue(Fmt f, double v) {
  switch (f) {
    case Fmt.signed:
      return _signed(v);
    case Fmt.ev:
      final e = v / 20;
      if (e == 0) return '0.00 EV';
      return '${e > 0 ? '+' : '−'}${e.abs().toStringAsFixed(2)} EV';
    case Fmt.plain:
      return '${v.round()}';
    case Fmt.pct:
      return '${v.round()}%';
    case Fmt.deg:
      if (v == 0) return '0°';
      return '${v > 0 ? '+' : '−'}${v.abs().round()}°';
    case Fmt.hue:
      return '${v.round()}°';
    case Fmt.radius:
      return (v / 10).toStringAsFixed(1);
  }
}

/// Grade hue is an absolute angle (0..360), shown without sign.
String fmtSpec(SliderSpec s, double v) => s.track == TrackKind.gradeHue ? fmtValue(Fmt.hue, v) : fmtValue(s.fmt, v);

// ---------------------------------------------------------------- tone classes

const toneKeys = ['high', 'mid', 'low'];
const toneRanges = ['short', 'mid', 'long'];
const keyPlain = {'high': '明亮', 'mid': '均衡', 'low': '深沉'};
const rangePlain = {'short': '柔和', 'mid': '自然', 'long': '强烈'};
const keyPro = {'high': '高', 'mid': '中', 'low': '低'};
const rangePro = {'short': '短调', 'mid': '中调', 'long': '长调'};

String toneClassId(String key, String range) => '$key-$range';
String tonePlain(String id) {
  final p = id.split('-');
  return '${keyPlain[p[0]]} · ${rangePlain[p[1]]}';
}

String tonePro(String id) {
  final p = id.split('-');
  return '${keyPro[p[0]]}${rangePro[p[1]]}';
}

// ---------------------------------------------------------------- sync categories

/// Keys of the film simulation (numeric ones; the stock and the paper are [EditState.film] / [EditState.filmPaper]).
const filmValueKeys = [
  'film_strength', 'film_grain', 'film_halation', 'film_warmth', 'film_tint', 'film_exp', //
  'film_shoot', 'film_print', 'film_black', 'film_wb', 'film_latitude',
];

enum SyncCat { style, tone, light, color, detail, crop }

const syncCatLabels = {
  SyncCat.style: '风格',
  SyncCat.tone: '明暗',
  SyncCat.light: '光线',
  SyncCat.color: '颜色',
  SyncCat.detail: '效果与细节',
  SyncCat.crop: '裁切',
};

/// Numeric slider keys belonging to a sync category (requirements 4.8).
SyncCat? syncCatOfKey(String key) {
  final s = specByKey[key];
  if (s == null) return null;
  switch (s.group) {
    case EGroup.light:
      return SyncCat.light;
    case EGroup.color:
    case EGroup.curve:
    case EGroup.grade:
      return SyncCat.color;
    case EGroup.effects:
    case EGroup.detail:
      return SyncCat.detail;
    case EGroup.crop:
      return SyncCat.crop;
    case EGroup.style:
      return SyncCat.style;
    case EGroup.local:
      return null;
  }
}

// ---------------------------------------------------------------- edit state

/// The state of one control pad (docs/design/editor-panel.md section 3): where the cursor sits (x, y in [-1, +1], 0 =
/// automatic), the strength strip ([a] in 0..1, 0.5 = 100 %) and the item offsets the itemised view adds on top of what
/// the pad position implies (key -> offset in the item's own units). The edit's slider values stay the source of truth for
/// the engine; this record only lets a reopened edit restore the pad. The engine never sees it.
class PadState {
  const PadState({this.x = 0, this.y = 0, this.a = 0.5, this.off = const {}});
  final double x, y, a;
  final Map<String, double> off;

  static const centre = PadState();

  bool get isCentre => x == 0 && y == 0 && a == 0.5 && off.isEmpty;

  PadState copyWith({double? x, double? y, double? a, Map<String, double>? off}) =>
      PadState(x: x ?? this.x, y: y ?? this.y, a: a ?? this.a, off: off ?? this.off);

  bool same(PadState o) =>
      x == o.x && y == o.y && a == o.a && off.length == o.off.length && off.entries.every((e) => o.off[e.key] == e.value);

  Map<String, dynamic> toJson() => {'x': x, 'y': y, 'a': a, if (off.isNotEmpty) 'off': off};

  static PadState? fromJson(Object? j) {
    if (j is! Map) return null;
    double n(Object? v, double d) => v is num && v.isFinite ? v.toDouble() : d;
    final off = <String, double>{};
    final o = j['off'];
    if (o is Map) {
      o.forEach((k, v) {
        if (k is String && v is num && v.isFinite) off[k] = v.toDouble();
      });
    }
    return PadState(x: n(j['x'], 0).clamp(-1.0, 1.0), y: n(j['y'], 0).clamp(-1.0, 1.0), a: n(j['a'], 0.5).clamp(0.0, 1.0), off: off);
  }
}

/// Per-photo edit record (requirements 6.2). Immutable; everything that equals
/// its default is simply absent, so "adjusted" == "has any override".
class EditState {
  const EditState({
    this.look,
    this.lookStrength = 100,
    this.palette,
    this.toneTarget,
    this.toneStrength,
    this.film,
    this.filmPaper,
    this.values = const {},
    this.crop = 'orig',
    this.rot = 0,
    this.flip = false,
    this.pads = const {},
    this.local = LocalList.empty,
  });

  /// Local adjustments (docs/design/local-adjustments.md): the engine's "local" list plus each area's pad state.
  final LocalList local;

  /// Control pad states by category name (`light`, `color`, `film`, `texture`); absent = pad at the centre.
  final Map<String, PadState> pads;

  /// null = follow the global default look.
  final String? look;
  final double lookStrength;

  /// null = the look's default point.
  final Offset? palette;

  /// null = the photo's nearest class (no override).
  final String? toneTarget;

  /// null = the global default tone strength.
  final double? toneStrength;

  /// Film stock id (the `film` key); null = no film simulation.
  final String? film;

  /// Print paper override (`film_paper`: paper id or 'none'); null = the stock's default paper.
  final String? filmPaper;

  /// Only non-default slider values (including `straighten`).
  final Map<String, double> values;
  final String crop;
  final int rot;
  final bool flip;

  static const empty = EditState();

  double value(String key) => values[key] ?? specByKey[key]!.def;

  EditState copyWith({
    Object? look = _keep,
    double? lookStrength,
    Object? palette = _keep,
    Object? toneTarget = _keep,
    Object? toneStrength = _keep,
    Object? film = _keep,
    Object? filmPaper = _keep,
    Map<String, double>? values,
    String? crop,
    int? rot,
    bool? flip,
    Map<String, PadState>? pads,
    LocalList? local,
  }) =>
      EditState(
        look: identical(look, _keep) ? this.look : look as String?,
        lookStrength: lookStrength ?? this.lookStrength,
        palette: identical(palette, _keep) ? this.palette : palette as Offset?,
        toneTarget: identical(toneTarget, _keep) ? this.toneTarget : toneTarget as String?,
        toneStrength: identical(toneStrength, _keep) ? this.toneStrength : toneStrength as double?,
        film: identical(film, _keep) ? this.film : film as String?,
        filmPaper: identical(filmPaper, _keep) ? this.filmPaper : filmPaper as String?,
        values: values ?? this.values,
        crop: crop ?? this.crop,
        rot: rot ?? this.rot,
        flip: flip ?? this.flip,
        pads: pads ?? this.pads,
        local: local ?? this.local,
      );

  static const Object _keep = Object();

  EditState withValue(String key, double v) {
    final s = specByKey[key]!;
    final m = Map<String, double>.of(values);
    if ((v - s.def).abs() < 1e-9) {
      m.remove(key);
    } else {
      m[key] = v.clamp(s.min, s.max).toDouble();
    }
    return copyWith(values: m);
  }

  /// The rendering base new photos start with (默认成像, set by the app): a photo on it is not "adjusted" by that alone.
  static String? defaultBase;

  bool get styleAdjusted => look != null || lookStrength != 100 || palette != null || (film != null && film != defaultBase);
  bool get toneAdjusted => toneTarget != null || toneStrength != null;

  bool groupAdjusted(EGroup g) {
    switch (g) {
      case EGroup.style:
        return styleAdjusted || toneAdjusted || values.keys.any(filmValueKeys.contains);
      case EGroup.crop:
        return crop != 'orig' || rot != 0 || flip || values.containsKey('straighten');
      default:
        return values.keys.any((k) => specByKey[k]!.group == g);
    }
  }

  bool get isAdjusted => styleAdjusted || toneAdjusted || values.isNotEmpty || crop != 'orig' || rot != 0 || flip || !local.isEmpty;

  bool sameAs(EditState o) =>
      look == o.look &&
      lookStrength == o.lookStrength &&
      palette == o.palette &&
      toneTarget == o.toneTarget &&
      toneStrength == o.toneStrength &&
      film == o.film &&
      filmPaper == o.filmPaper &&
      crop == o.crop &&
      rot == o.rot &&
      flip == o.flip &&
      values.length == o.values.length &&
      values.entries.every((e) => o.values[e.key] == e.value) &&
      pads.length == o.pads.length &&
      pads.entries.every((e) => o.pads[e.key]?.same(e.value) ?? false) &&
      local.same(o.local);

  /// Copy settings (clipboard): everything except crop / rot / flip / straighten.
  EditState withoutCrop() {
    final m = Map<String, double>.of(values)..remove('straighten');
    return EditState(
        look: look,
        lookStrength: lookStrength,
        palette: palette,
        toneTarget: toneTarget,
        toneStrength: toneStrength,
        film: film,
        filmPaper: filmPaper,
        values: m,
        pads: pads);
  }

  /// Paste = replace everything except the target's own crop.
  EditState pasted(EditState clip) {
    final m = Map<String, double>.of(clip.values)..remove('straighten');
    if (values.containsKey('straighten')) m['straighten'] = values['straighten']!;
    return EditState(
        look: clip.look,
        lookStrength: clip.lookStrength,
        palette: clip.palette,
        toneTarget: clip.toneTarget,
        toneStrength: clip.toneStrength,
        film: clip.film,
        filmPaper: clip.filmPaper,
        values: m,
        crop: crop,
        rot: rot,
        flip: flip,
        pads: clip.pads,
        local: local);
  }

  /// Sync = merge: only what the source overrides, only the chosen categories,
  /// everything else on the target stays (requirements 4.8).
  EditState merged(EditState src, Set<SyncCat> cats) {
    var out = this;
    if (cats.contains(SyncCat.style) && src.styleAdjusted) {
      out = out.copyWith(look: src.look, lookStrength: src.lookStrength, palette: src.palette);
    }
    if (cats.contains(SyncCat.style) && src.film != null) {
      out = out.copyWith(film: src.film, filmPaper: src.filmPaper);
    }
    if (cats.contains(SyncCat.tone) && src.toneAdjusted) {
      out = out.copyWith(toneTarget: src.toneTarget, toneStrength: src.toneStrength);
    }
    final m = Map<String, double>.of(out.values);
    for (final e in src.values.entries) {
      final c = syncCatOfKey(e.key);
      if (c != null && cats.contains(c)) m[e.key] = e.value;
    }
    out = out.copyWith(values: m);
    // the pad states travel with the categories they belong to
    final np = Map<String, PadState>.of(out.pads);
    for (final e in const {'light': SyncCat.light, 'color': SyncCat.color, 'film': SyncCat.style, 'texture': SyncCat.detail}.entries) {
      if (!cats.contains(e.value)) continue;
      final sp = src.pads[e.key];
      if (sp != null) {
        np[e.key] = sp;
      } else if (src.values.keys.any((k) => syncCatOfKey(k) == e.value)) {
        np.remove(e.key);
      }
    }
    out = out.copyWith(pads: np);
    if (cats.contains(SyncCat.crop)) {
      if (src.crop != 'orig') out = out.copyWith(crop: src.crop);
      if (src.rot != 0) out = out.copyWith(rot: src.rot);
      if (src.flip) out = out.copyWith(flip: true);
    }
    return out;
  }

  /// Engine JSON: exactly the chapter-6 keys, only where different from the
  /// default (a missing key means "default").
  Map<String, dynamic> toWire({required String defaultLook, required double defaultToneStrength}) {
    final m = <String, dynamic>{
      'look': look ?? defaultLook,
      'lookStrength': lookStrength,
      'toneStrength': toneStrength ?? defaultToneStrength,
    };
    if (toneTarget != null) m['toneTarget'] = toneTarget;
    if (film != null) {
      m['film'] = film;
      if (filmPaper != null) m['film_paper'] = filmPaper;
    }
    values.forEach((k, v) {
      if (film == null && filmValueKeys.contains(k)) return; // film knobs act only with a stock
      m[k] = v;
    });
    if (crop != 'orig') m['crop'] = crop;
    if (rot != 0) m['rot'] = rot;
    if (flip) m['flip'] = true;
    final l = local.toEngineJson();
    if (l != null) m['local'] = l;
    return m;
  }

  Map<String, dynamic> toJson() => {
        if (look != null) 'look': look,
        'lookStrength': lookStrength,
        if (palette != null) 'palette': [palette!.dx, palette!.dy],
        if (toneTarget != null) 'toneTarget': toneTarget,
        if (toneStrength != null) 'toneStrength': toneStrength,
        if (film != null) 'film': film,
        if (filmPaper != null) 'filmPaper': filmPaper,
        'values': values,
        'crop': crop,
        'rot': rot,
        'flip': flip,
        if (pads.isNotEmpty) 'pads': {for (final e in pads.entries) e.key: e.value.toJson()},
        if (!local.isEmpty) 'local': local.toAppJson(),
      };

  factory EditState.fromJson(Map<String, dynamic> j) {
    final pal = j['palette'];
    final e = EditState(
      look: j['look'] as String?,
      lookStrength: ((j['lookStrength'] as num?) ?? 100).toDouble(),
      palette: pal is List && pal.length == 2 ? Offset((pal[0] as num).toDouble(), (pal[1] as num).toDouble()) : null,
      toneTarget: j['toneTarget'] as String?,
      toneStrength: (j['toneStrength'] as num?)?.toDouble(),
      film: j['film'] as String?,
      filmPaper: j['filmPaper'] as String?,
      values: ((j['values'] as Map?) ?? {})
          .map((k, v) => MapEntry(k as String, (v as num).toDouble()))
        ..removeWhere((k, v) => !specByKey.containsKey(k)),
      crop: (j['crop'] as String?) ?? 'orig',
      rot: (j['rot'] as num?)?.toInt() ?? 0,
      flip: (j['flip'] as bool?) ?? false,
      pads: _padsFromJson(j['pads']),
      local: LocalList.fromJson(j['local']),
    );
    return e.palette == null ? e : migratePalette(e);
  }
}

Map<String, PadState> _padsFromJson(Object? j) {
  final out = <String, PadState>{};
  if (j is Map) {
    j.forEach((k, v) {
      final p = PadState.fromJson(v);
      if (k is String && p != null) out[k] = p;
    });
  }
  return out;
}

/// Per-photo undo / redo (requirements 3.2): 800 ms coalescing, depth 80.
class UndoStack {
  final List<EditState> _past = [];
  final List<EditState> _future = [];
  String? _lastKey;
  int _lastMs = 0;

  bool get canUndo => _past.isNotEmpty;
  bool get canRedo => _future.isNotEmpty;

  /// Records [before] as an undo point unless [coalesceKey] repeats within 800 ms.
  void record(EditState before, {String? coalesceKey, int? nowMs, bool forceMerge = false}) {
    final now = nowMs ?? DateTime.now().millisecondsSinceEpoch;
    final merge = coalesceKey != null && coalesceKey == _lastKey && (forceMerge || now - _lastMs < 800) && _past.isNotEmpty;
    _lastKey = coalesceKey;
    _lastMs = now;
    _future.clear();
    if (merge) return;
    _past.add(before);
    if (_past.length > 80) _past.removeAt(0);
  }

  EditState? undo(EditState current) {
    if (_past.isEmpty) return null;
    _future.add(current);
    _lastKey = null;
    return _past.removeLast();
  }

  EditState? redo(EditState current) {
    if (_future.isEmpty) return null;
    _past.add(current);
    _lastKey = null;
    return _future.removeLast();
  }
}
