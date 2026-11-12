// The parameter rows of each category window (requirements 4.15.5): what a row is, how it reads
// and writes the per-photo [EditState], and its auto position. Pure Dart; the keys, ranges, defaults
// and display formats come from model/params.dart unchanged.
import 'dart:math' as math;
import 'dart:ui' show Offset;

import '../model/catalog.dart';
import '../model/film_catalog.dart';
import '../model/film_names.dart';
import '../model/film_pad.dart';
import '../model/params.dart';
import 'gesture_arbiter.dart' show ActiveKind;

enum RowKind { oneD, discrete, twoD, action }

enum TwoD { palette, tone, curve, wheel, offset }

/// UI-only selections inside categories (not part of the photo's edit record).
class PanelSel {
  int band = 1; // colour range, default orange
  int curveCh = 0;
  int gradeZone = 0;
}

/// Everything a row needs to read itself.
class RowContext {
  const RowContext({
    required this.edit,
    required this.defaultLookId,
    required this.defaultTone,
    required this.nearestTone,
    required this.sel,
    this.film = FilmCatalog.empty,
    this.filmRanges = FilmPadRanges.all,
  });
  final EditState edit;
  final String defaultLookId;
  final double defaultTone;
  final String nearestTone;
  final PanelSel sel;

  /// The film library of the engine (empty = no pack, the film rows are not offered).
  final FilmCatalog film;

  /// Which ends of the film pad axes change the render for this photo and stock (engine answer; all effective when unknown).
  final FilmPadRanges filmRanges;
}

class ParamRow {
  const ParamRow._({
    required this.id,
    required this.label,
    required this.kind,
    required this.group,
    this.min = 0,
    this.max = 1,
    this.auto = 0,
    this.inputScale = 1,
    this.count = 0,
    this.twoD,
    this.read,
    this.write,
    this.fmt,
    this.readIndex,
    this.writeIndex,
    this.uiSet,
    this.itemLabel,
    this.autoIndex,
    this.reset,
    this.act,
    this.hint,
    this.padX,
    this.padY,
    this.padLabels = const [],
    this.padEff = const [true, true, true, true],
    this.padXMin = -1,
    this.padNotes = const [],
  });

  final String id, label;

  /// Spoken explanation (semantics only; nothing of it is drawn).
  final String? hint;
  final RowKind kind;
  final EGroup group;

  // 1D
  final double min, max, auto;

  /// Parameter step (all rows are integer steps).
  double get step => 1;

  /// Typed value * [inputScale] = stored value (EV 20, radius 10).
  final double inputScale;
  final double Function(EditState)? read;
  final EditState Function(EditState, double)? write;
  final String Function(double)? fmt;

  // discrete
  final int count;
  final int Function(RowContext)? readIndex;
  final EditState Function(EditState, int, RowContext)? writeIndex;

  /// Discrete rows backed by the UI selection instead of the edit record.
  final void Function(PanelSel, int)? uiSet;
  final String Function(int)? itemLabel;
  final int Function(RowContext)? autoIndex;

  // 2D
  final TwoD? twoD;

  /// [TwoD.offset] pads: the params-map keys of the X and Y offsets (each in [-1, +1], 0 = automatic) and the four
  /// edge labels: left, right, bottom, top.
  final String? padX, padY;
  final List<String> padLabels;

  /// [TwoD.offset]: whether the X axis' -1 and +1 ends and the Y axis' -1 (bottom) and +1 (top) ends change the render.
  /// An ineffective end is unreachable: the cursor stops at the centre line and the dead half is drawn hatched.
  final List<bool> padEff;

  /// The X axis runs from [padXMin] (left) to +1 (right); -1 = symmetric around the centre, 0 = one-sided (the default
  /// sits at the left edge, e.g. film_wb).
  final double padXMin;

  /// Small notes under the left and right edge labels (the scene's and the stock's colour temperature).
  final List<String> padNotes;

  /// Resets this row to its auto position.
  final EditState Function(EditState, RowContext)? reset;
  final EditState Function(EditState)? act;

  double get range => max - min;
  double get bigStep => math.max(step, range * 0.05);

  ActiveKind get activeKind => switch (kind) {
        RowKind.oneD => ActiveKind.oneD,
        RowKind.discrete => ActiveKind.discrete,
        RowKind.twoD => ActiveKind.twoD,
        RowKind.action => ActiveKind.none,
      };

  bool get switchable => kind != RowKind.action;

  // ---------------------------------------------------------------- reading

  double value(RowContext c) => read!(c.edit);

  bool isAuto(RowContext c) {
    switch (kind) {
      case RowKind.oneD:
        return (value(c) - auto).abs() < 1e-9;
      case RowKind.discrete:
        return autoIndex == null ? false : readIndex!(c) == autoIndex!(c);
      case RowKind.twoD:
        return !isModified(c);
      case RowKind.action:
        return true;
    }
  }

  bool isModified(RowContext c) {
    switch (kind) {
      case RowKind.oneD:
        return (value(c) - auto).abs() > 1e-9;
      case RowKind.discrete:
        if (uiSet != null || autoIndex == null) return false;
        return readIndex!(c) != autoIndex!(c);
      case RowKind.twoD:
        final e = c.edit;
        switch (twoD!) {
          case TwoD.palette:
            return e.palette != null;
          case TwoD.tone:
            return e.toneTarget != null;
          case TwoD.curve:
            return (value(c) - auto).abs() > 1e-9;
          case TwoD.wheel:
            final z = c.sel.gradeZone;
            return e.values.containsKey(grKey(z, 'h')) || e.values.containsKey(grKey(z, 's'));
          case TwoD.offset:
            return e.values.containsKey(padX) || e.values.containsKey(padY);
        }
      case RowKind.action:
        return false;
    }
  }

  /// Short value text shown at the right of the row.
  String text(RowContext c) {
    switch (kind) {
      case RowKind.oneD:
        return fmt!(value(c));
      case RowKind.discrete:
        return itemLabel!(readIndex!(c));
      case RowKind.twoD:
        final e = c.edit;
        switch (twoD!) {
          case TwoD.palette:
            final look = lookById(e.look ?? c.defaultLookId);
            final p = e.palette ?? Offset(look.px, look.py);
            return e.palette == null ? '默认' : paletteShort(p);
          case TwoD.tone:
            return tonePlain(e.toneTarget ?? c.nearestTone);
          case TwoD.curve:
            return fmt!(value(c));
          case TwoD.wheel:
            final z = c.sel.gradeZone;
            final s = e.value(grKey(z, 's'));
            return s == 0 ? '无' : '${fmtValue(Fmt.hue, e.value(grKey(z, 'h')))} · ${s.round()}';
          case TwoD.offset:
            return offsetPadText(e.value(padX!), e.value(padY!), xSigned: padXMin < 0);
        }
      case RowKind.action:
        return fmt?.call(value(c)) ?? '';
    }
  }

  /// Numeric text for the "tap the value to type" path (1D rows and curve points).
  bool get typeable => kind == RowKind.oneD || (kind == RowKind.twoD && twoD == TwoD.curve);

  /// Parses typed text into a stored value, clamped and rounded to the step. Null = cancel.
  double? parseInput(String raw) {
    final t = raw.trim().replaceAll('−', '-').replaceAll('EV', '').replaceAll('°', '').replaceAll('%', '').trim();
    if (t.isEmpty) return null;
    final v = double.tryParse(t);
    if (v == null) return null;
    final stored = v * inputScale;
    return (((stored - min) / step).round() * step + min).clamp(min, max).toDouble();
  }

  /// True when [raw] parses to a value outside the range (to show "已调整到范围内").
  bool inputOutOfRange(String raw) {
    final t = raw.trim().replaceAll('−', '-').replaceAll('EV', '').replaceAll('°', '').replaceAll('%', '').trim();
    final v = double.tryParse(t);
    if (v == null) return false;
    final stored = v * inputScale;
    return stored < min - 1e-9 || stored > max + 1e-9;
  }

  /// Text used to pre-fill the numeric input.
  String inputText(RowContext c) {
    final v = value(c) / inputScale;
    if (inputScale == 1) return v.round().toString();
    return v.toStringAsFixed(inputScale >= 20 ? 2 : 1);
  }

  /// Semantics value: display text, plus ", auto" at the auto position.
  String semanticValue(RowContext c) => isAuto(c) && kind != RowKind.action ? '${text(c)}，自动' : text(c);

  EditState resetRow(EditState e, RowContext c) {
    if (reset != null) return reset!(e, c);
    if (kind == RowKind.oneD || twoD == TwoD.curve) return write!(e, auto);
    return e;
  }

  // ---------------------------------------------------------------- factories

  static ParamRow spec(String key, {String? label, String? hint}) {
    final s = specByKey[key]!;
    return ParamRow._(
      id: key,
      label: label ?? s.label,
      hint: hint,
      kind: RowKind.oneD,
      group: s.group,
      min: s.min,
      max: s.max,
      auto: s.def,
      inputScale: s.fmt == Fmt.ev ? 20 : (s.fmt == Fmt.radius ? 10 : 1),
      read: (e) => e.value(key),
      write: (e, v) => e.withValue(key, v),
      fmt: (v) => fmtSpec(s, v),
    );
  }
}

/// Latitude ruler scale: the stored offset -1 / 0 / +1 means an in-range guarantee of 55 / 80 / 98 % (piecewise linear).
double latitudePercent(double v) => v <= 0 ? 80 + v * 25 : 80 + v * 18;

/// Inverse of [latitudePercent].
double latitudeFromPercent(double p) => p <= 80 ? (p - 80) / 25 : (p - 80) / 18;

/// Offset pad readout: both offsets as signed hundredths, "自动" at the centre.
String offsetPadText(double x, double y, {bool xSigned = true}) {
  String f(double v, bool signed) => v == 0 ? '0' : '${signed ? (v > 0 ? '+' : '−') : ''}${(v.abs() * 100).round()}';
  return x == 0 && y == 0 ? '自动' : '${f(x, xSigned)} ${f(y, true)}';
}

/// "偏暖 12，偏浓 8" (requirements 4.7 aria text).
String paletteText(Offset p) {
  final n = ((p.dy - 0.5).abs() * 200).round();
  final m = ((p.dx - 0.5).abs() * 200).round();
  return '${p.dy <= 0.5 ? '偏暖' : '偏冷'} $n，${p.dx >= 0.5 ? '偏浓' : '偏淡'} $m';
}

/// Compact palette value for the window row: "暖 12 浓 8".
String paletteShort(Offset p) {
  final n = ((p.dy - 0.5).abs() * 200).round();
  final m = ((p.dx - 0.5).abs() * 200).round();
  return '${p.dy <= 0.5 ? '暖' : '冷'}$n ${p.dx >= 0.5 ? '浓' : '淡'}$m';
}

ParamRow _custom({
  required String id,
  required String label,
  required RowKind kind,
  required EGroup group,
  double min = 0,
  double max = 1,
  double auto = 0,
  int count = 0,
  TwoD? twoD,
  double Function(EditState)? read,
  EditState Function(EditState, double)? write,
  String Function(double)? fmt,
  int Function(RowContext)? readIndex,
  EditState Function(EditState, int, RowContext)? writeIndex,
  void Function(PanelSel, int)? uiSet,
  String Function(int)? itemLabel,
  int Function(RowContext)? autoIndex,
  EditState Function(EditState, RowContext)? reset,
  EditState Function(EditState)? act,
  String? hint,
  String? padX,
  String? padY,
  List<String> padLabels = const [],
  List<bool> padEff = const [true, true, true, true],
  double padXMin = -1,
  List<String> padNotes = const [],
}) =>
    ParamRow._(
      id: id,
      label: label,
      hint: hint,
      kind: kind,
      group: group,
      min: min,
      max: max,
      auto: auto,
      count: count,
      twoD: twoD,
      read: read,
      write: write,
      fmt: fmt,
      readIndex: readIndex,
      writeIndex: writeIndex,
      uiSet: uiSet,
      itemLabel: itemLabel,
      autoIndex: autoIndex,
      reset: reset,
      act: act,
      padX: padX,
      padY: padY,
      padLabels: padLabels,
      padEff: padEff,
      padXMin: padXMin,
      padNotes: padNotes,
    );

/// Rows of category [g] for the current selections (requirements 4.15.5, top to bottom).
List<ParamRow> rowsFor(EGroup g, RowContext c) {
  switch (g) {
    case EGroup.light:
      return [for (final k in const ['exposure', 'contrast', 'highlights', 'shadows', 'whites', 'blacks']) ParamRow.spec(k)];
    case EGroup.color:
      final b = c.sel.band;
      final n = hslBandNames[b];
      return [
        ParamRow.spec('temp'),
        ParamRow.spec('tint'),
        ParamRow.spec('vibrance'),
        ParamRow.spec('saturation'),
        _custom(
          id: 'band',
          label: '色范围',
          kind: RowKind.discrete,
          group: g,
          count: 8,
          readIndex: (c) => c.sel.band,
          uiSet: (s, i) => s.band = i,
          itemLabel: (i) => hslBandNames[i],
        ),
        ParamRow.spec(hslKey(b, 'h'), label: '$n色相'),
        ParamRow.spec(hslKey(b, 's'), label: '$n饱和度'),
        ParamRow.spec(hslKey(b, 'l'), label: '$n明亮度'),
      ];
    case EGroup.style:
      return [
        _custom(
          id: 'look',
          label: '风格',
          kind: RowKind.discrete,
          group: g,
          count: dLooks.length,
          readIndex: (c) => math.max(0, dLooks.indexWhere((l) => l.id == (c.edit.look ?? c.defaultLookId))),
          writeIndex: (e, i, c) =>
              e.copyWith(look: dLooks[i].id == c.defaultLookId ? null : dLooks[i].id, palette: null),
          itemLabel: (i) => dLooks[i].name,
          autoIndex: (c) => math.max(0, dLooks.indexWhere((l) => l.id == c.defaultLookId)),
          reset: (e, c) => e.copyWith(look: null, palette: null),
        ),
        _custom(
          id: 'lookStrength',
          label: '风格强度',
          kind: RowKind.oneD,
          group: g,
          min: 0,
          max: 100,
          auto: 100,
          read: (e) => e.lookStrength,
          write: (e, v) => e.copyWith(lookStrength: v),
          fmt: (v) => fmtValue(Fmt.pct, v),
        ),
        _custom(
          id: 'palette',
          label: '调色盘',
          kind: RowKind.twoD,
          group: g,
          twoD: TwoD.palette,
          reset: (e, c) => e.copyWith(palette: null),
        ),
        _custom(
          id: 'toneTarget',
          label: '明暗目标',
          kind: RowKind.twoD,
          group: g,
          twoD: TwoD.tone,
          reset: (e, c) => e.copyWith(toneTarget: null),
        ),
        _toneStrengthRow(c),
        ..._filmRows(c),
      ];
    case EGroup.curve:
      final ch = c.sel.curveCh;
      const names = {'H': '高光区', 'L': '亮部', 'D': '暗部', 'S': '阴影区'};
      return [
        _custom(
          id: 'channel',
          label: '通道',
          kind: RowKind.discrete,
          group: g,
          count: 4,
          readIndex: (c) => c.sel.curveCh,
          uiSet: (s, i) => s.curveCh = i,
          itemLabel: (i) => curveChannelNames[i],
        ),
        for (final r in const ['H', 'L', 'D', 'S'])
          _custom(
            id: cvKey(ch, r),
            label: names[r]!,
            kind: RowKind.twoD,
            group: g,
            twoD: TwoD.curve,
            min: -100,
            max: 100,
            read: (e) => e.value(cvKey(ch, r)),
            write: (e, v) => e.withValue(cvKey(ch, r), v),
            fmt: (v) => fmtValue(Fmt.signed, v),
          ),
      ];
    case EGroup.effects:
      return [for (final k in const ['clarity', 'texture', 'dehaze', 'vignette', 'grain']) ParamRow.spec(k)];
    case EGroup.detail:
      return [for (final k in const ['sharpen', 'sharpRadius', 'nrLuma', 'nrColor']) ParamRow.spec(k)];
    case EGroup.grade:
      final z = c.sel.gradeZone;
      return [
        _custom(
          id: 'zone',
          label: '区域',
          kind: RowKind.discrete,
          group: g,
          count: 4,
          readIndex: (c) => c.sel.gradeZone,
          uiSet: (s, i) => s.gradeZone = i,
          itemLabel: (i) => gradeZoneNames[i],
        ),
        _custom(
          id: 'wheel',
          label: '色轮',
          kind: RowKind.twoD,
          group: g,
          twoD: TwoD.wheel,
          reset: (e, c) => e.withValue(grKey(z, 'h'), 0).withValue(grKey(z, 's'), 0),
        ),
        ParamRow.spec(grKey(z, 'l'), label: '明亮度'),
        ParamRow.spec('gr_blend'),
        ParamRow.spec('gr_balance'),
      ];
    case EGroup.crop:
      return [
        _custom(
          id: 'ratio',
          label: '比例',
          kind: RowKind.discrete,
          group: g,
          count: cropRatios.length,
          readIndex: (c) => math.max(0, cropRatios.indexOf(c.edit.crop)),
          writeIndex: (e, i, c) => e.copyWith(crop: cropRatios[i]),
          itemLabel: (i) => cropRatioNames[i],
          autoIndex: (c) => 0,
          reset: (e, c) => e.copyWith(crop: 'orig'),
        ),
        ParamRow.spec('straighten'),
        _custom(
          id: 'rotate',
          label: '旋转 90°',
          kind: RowKind.action,
          group: g,
          read: (e) => e.rot.toDouble(),
          fmt: (v) => '${v.round()}°',
          act: (e) => e.copyWith(rot: (e.rot + 90) % 360),
          reset: (e, c) => e.copyWith(rot: 0),
        ),
        _custom(
          id: 'flip',
          label: '水平翻转',
          kind: RowKind.action,
          group: g,
          read: (e) => e.flip ? 1 : 0,
          fmt: (v) => v > 0 ? '开' : '关',
          act: (e) => e.copyWith(flip: !e.flip),
          reset: (e, c) => e.copyWith(flip: false),
        ),
      ];
  }
}

/// Film simulation rows of the 风格 window: the stock; the paper only for a printed (negative) stock; then the
/// knobs. Print warmth / tint filter the enlarger, so they exist only when the film is printed on paper.
List<ParamRow> _filmRows(RowContext c) {
  final cat = c.film;
  if (cat.isEmpty) return const [];
  const g = EGroup.style;
  final stock = cat.stock(c.edit.film);
  final rows = <ParamRow>[
    _custom(
      id: 'film',
      label: '胶片',
      kind: RowKind.discrete,
      group: g,
      hint: '模拟胶片的色彩、影调和颗粒',
      count: cat.stocks.length + 1,
      readIndex: (c) => cat.stocks.indexWhere((s) => s.id == c.edit.film) + 1,
      writeIndex: (e, i, c) => i == 0 ? clearFilm(e) : e.copyWith(film: cat.stocks[i - 1].id, filmPaper: null),
      itemLabel: (i) => i == 0 ? '无' : cat.stocks[i - 1].name,
      autoIndex: (c) => 0,
      reset: (e, c) => clearFilm(e),
    ),
  ];
  if (stock == null) return rows;
  var scanned = false;
  if (stock.printed) {
    final ids = cat.paperIds;
    final def = cat.defaultPaperOf(stock);
    final cur = c.edit.filmPaper ?? def;
    scanned = cur == kPaperNone;
    rows.add(_custom(
      id: 'filmPaper',
      label: '相纸',
      kind: RowKind.discrete,
      group: g,
      hint: '负片冲印用的相纸；直接扫描则不经冲印',
      count: ids.length,
      readIndex: (c) => math.max(0, ids.indexOf(c.edit.filmPaper ?? def)),
      writeIndex: (e, i, c) => e.copyWith(filmPaper: ids[i] == def ? null : ids[i]),
      itemLabel: (i) => cat.paperName(ids[i]),
      autoIndex: (c) => math.max(0, ids.indexOf(def)),
      reset: (e, c) => e.copyWith(filmPaper: null),
    ));
  }
  rows.addAll([
    ParamRow.spec('film_strength', hint: '胶片效果与原图的混合比例'),
    ?_filmPadRow(c,
        id: 'film_expo',
        label: '曝光',
        x: 'film_shoot',
        y: 'film_print',
        labels: const ['欠曝', '过曝', '深', '浅'],
        hint: '横向：拍摄时欠曝或过曝；纵向：冲印的深浅。中心为自动'),
    ?_filmPadRow(c,
        id: 'film_texture',
        label: '质感',
        x: 'film_wb',
        xMin: 0,
        y: 'film_black',
        labels: const ['数码', '胶片', '抬黑', '压黑'],
        notes: [if (c.filmRanges.sceneK != null) '${c.filmRanges.sceneK}K' else '', if (c.filmRanges.stockK != null) '${c.filmRanges.stockK}K' else ''],
        hint: '横向：数码白平衡到胶片白平衡（胶片自己的白平衡看到真实的光）；纵向：黑位，抬黑到压黑。左边缘为自动'),
    // 宽容度: hidden when neither end changes the render, restricted to the effective side otherwise
    if (!c.filmRanges.dead('film_latitude'))
      _custom(
        id: 'film_latitude',
        label: '宽容度',
        kind: RowKind.oneD,
        group: g,
        min: c.filmRanges.lo('film_latitude') ? 55 : 80,
        max: c.filmRanges.hi('film_latitude') ? 98 : 80,
        auto: 80,
        hint: '保证不溢出的亮度范围，80% 为自动',
        read: (e) => latitudePercent(e.values['film_latitude'] ?? 0),
        write: (e, v) => e.withValue('film_latitude', latitudeFromPercent(v)),
        fmt: (v) => '${v.round()}%',
      ),
    ParamRow.spec('film_grain', hint: '颗粒量，100 为该胶片的标准颗粒'),
    ParamRow.spec('film_halation', hint: '高光向周围的红色渗光'),
    if (stock.printed && !scanned) ...[
      ParamRow.spec('film_warmth', hint: '冲印时的黄色滤光，正值偏暖'),
      ParamRow.spec('film_tint', hint: '冲印时的品红滤光，正值偏品红'),
    ],
  ]);
  return rows;
}

/// A film pad row for the current photo and stock: a 2D pad when both axes change the render, a one-axis ruler for the
/// live axis when the other is dead, nothing when both are dead. A half-dead axis keeps the pad but makes that end unreachable.
ParamRow? _filmPadRow(RowContext c,
    {required String id,
    required String label,
    required String x,
    required String y,
    required List<String> labels,
    double xMin = -1,
    List<String> notes = const [],
    String? hint}) {
  final r = c.filmRanges;
  final xDead = r.dead(x), yDead = r.dead(y);
  if (xDead && yDead) return null;
  if (xDead || yDead) {
    final key = xDead ? y : x;
    final oneSided = filmOneSidedKeys.contains(key);
    return _custom(
      id: id,
      label: label,
      kind: RowKind.oneD,
      group: EGroup.style,
      min: oneSided || !r.lo(key) ? 0 : -100,
      max: r.hi(key) ? 100 : 0,
      auto: 0,
      hint: hint,
      read: (e) => (e.values[key] ?? 0) * 100,
      write: (e, v) => e.withValue(key, v / 100),
      fmt: (v) => fmtValue(Fmt.signed, v),
    );
  }
  return _offsetPad(
      id: id, label: label, x: x, y: y, labels: labels, hint: hint, eff: [r.lo(x), r.hi(x), r.lo(y), r.hi(y)], xMin: xMin, notes: notes);
}

/// A film offset pad: two params-map keys, both normalised offsets in [-1, +1], 0 = automatic (the pad's centre).
ParamRow _offsetPad(
        {required String id,
        required String label,
        required String x,
        required String y,
        required List<String> labels,
        String? hint,
        List<bool> eff = const [true, true, true, true],
        double xMin = -1,
        List<String> notes = const []}) =>
    _custom(
      id: id,
      label: label,
      kind: RowKind.twoD,
      group: EGroup.style,
      twoD: TwoD.offset,
      padX: x,
      padY: y,
      padLabels: labels,
      padEff: eff,
      padXMin: xMin,
      padNotes: notes,
      hint: hint,
      reset: (e, c) => e.withValue(x, 0).withValue(y, 0),
    );

/// Film off: the stock, the paper and every film knob go back to their defaults.
EditState clearFilm(EditState e) {
  final m = Map<String, double>.of(e.values)..removeWhere((k, v) => filmValueKeys.contains(k));
  return e.copyWith(film: null, filmPaper: null, values: m);
}

ParamRow _toneStrengthRow(RowContext c) => _custom(
      id: 'toneStrength',
      label: '明暗强度',
      kind: RowKind.oneD,
      group: EGroup.style,
      min: 0,
      max: 100,
      auto: c.defaultTone,
      read: (e) => e.toneStrength ?? c.defaultTone,
      write: (e, v) => e.copyWith(toneStrength: v == c.defaultTone ? null : v),
      fmt: (v) => fmtValue(Fmt.pct, v),
    );

/// Resets every override of category [g] (one undo step for the caller).
EditState resetCategory(EGroup g, EditState e) {
  switch (g) {
    case EGroup.style:
      return clearFilm(e.copyWith(look: null, lookStrength: 100, palette: null, toneTarget: null, toneStrength: null));
    case EGroup.crop:
      final m = Map<String, double>.of(e.values)..remove('straighten');
      return e.copyWith(crop: 'orig', rot: 0, flip: false, values: m);
    default:
      final m = Map<String, double>.of(e.values)..removeWhere((k, v) => specByKey[k]!.group == g);
      return e.copyWith(values: m);
  }
}

/// Wheel point -> (hue, saturation) helpers shared with the mini map.
Offset wheelPoint(double hueDeg, double sat) {
  final a = hueDeg * math.pi / 180;
  return Offset(math.cos(a) * sat / 100, math.sin(a) * sat / 100);
}
