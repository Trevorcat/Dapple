// Local adjustments through masks: the data model of the engine's "local" params key (docs/engine/pipeline-architecture.md section
// 40.1, core/include/dapple/local_adjust.h), mirrored key by key. Immutable; JSON read is tolerant and clamps every number to the
// engine's range (NaN -> default), JSON write uses the engine's fixed key order and only non-default ops, so a list read from the
// engine's JSON writes back byte-identical (tests/engine/local_capi_tests.cpp examples; test/model/local_adjust_test.dart).
//
// Coordinates are the photo's own normalised space (0..1 of the decoded picture, before crop / rotate / straighten / flip);
// distances (radii, brush size) are fractions of the photo's long edge. Unknown component kinds are kept verbatim.
//
// The app stores one extra key per item, "ui" (the pad state of docs/design/local-adjustments.md section 4), in its own edit
// record ([LocalList.toAppJson]); the engine never sees it ([LocalList.toEngineJson]).
import 'dart:math' as math;
import 'dart:typed_data';

/// Engine limits (local_adjust.h).
const int kMaxLocalAdjustments = 16;
const int kMaxMaskComponents = 8;
const int kMaxRasterComponents = 4;
const int kMaxBrushComponents = 8;
const int kMaxBrushPoints = 100000;

enum MaskKindId { linear, radial, luminance, color, brush, raster, unknown }

MaskKindId maskKindOf(String s) => switch (s) {
      'linear' => MaskKindId.linear,
      'radial' => MaskKindId.radial,
      'luminance' => MaskKindId.luminance,
      'color' || 'colour' => MaskKindId.color,
      'brush' => MaskKindId.brush,
      'raster' => MaskKindId.raster,
      _ => MaskKindId.unknown,
    };

enum MaskOp { add, subtract, intersect }

// ------------------------------------------------------------------------------------------------ numbers

double _fix(Object? v, double lo, double hi, double def) {
  double x = def;
  if (v is num) x = v.toDouble();
  if (v is bool) x = v ? 1 : 0;
  if (!x.isFinite) x = def;
  return x < lo ? lo : (x > hi ? hi : x);
}

double _fmod(double a, double b) => a - b * (a / b).truncateToDouble();

final Float32List _f32 = Float32List(1);
double _asF32(double v) {
  _f32[0] = v;
  return _f32[0];
}

/// The engine's number writer: the shortest of %.6g / %.9g that reads back to the same float. Integral values become ints so
/// `jsonEncode` writes `100` like the engine, not `100.0`.
Object jnum(double v) {
  final f = _asF32(v);
  var d = double.parse(f.toStringAsPrecision(6));
  if (_asF32(d) != f) d = double.parse(f.toStringAsPrecision(9));
  if (d == d.truncateToDouble() && d.abs() < 1e15) return d.toInt();
  return d;
}

bool _rdb(Object? v, bool def) => v is bool ? v : (v is num ? v != 0 : def);

// ------------------------------------------------------------------------------------------------ the op registry

/// One local op (the engine's `localOps()` row), in JSON units: exposure is EV x 20.
class LocalOpSpec {
  const LocalOpSpec(this.key, this.min, this.max, this.label, this.group);
  final String key, label;
  final double min, max;

  /// Which itemised group shows it: light, color, detail; null = not shown in v1 (kept in the data).
  final String? group;
}

/// Registry order = the engine's slot order (and the JSON key order of "adjust").
const List<LocalOpSpec> localOpSpecs = [
  LocalOpSpec('exposure', -80, 80, '曝光', 'light'),
  LocalOpSpec('contrast', -100, 100, '对比度', 'light'),
  LocalOpSpec('highlights', -100, 100, '高光', 'light'),
  LocalOpSpec('shadows', -100, 100, '阴影', 'light'),
  LocalOpSpec('whites', -100, 100, '白色色阶', 'light'),
  LocalOpSpec('blacks', -100, 100, '黑色色阶', 'light'),
  LocalOpSpec('temp', -100, 100, '色温', 'color'),
  LocalOpSpec('tint', -100, 100, '色偏', 'color'),
  LocalOpSpec('saturation', -100, 100, '饱和度', 'color'),
  LocalOpSpec('vibrance', -100, 100, '鲜艳度', 'color'),
  LocalOpSpec('color_hue', 0, 360, '色相', null),
  LocalOpSpec('color_sat', 0, 100, '加色', null),
  LocalOpSpec('clarity', -100, 100, '清晰度', 'detail'),
  LocalOpSpec('texture', -100, 100, '纹理', 'detail'),
  LocalOpSpec('sharpen', -100, 100, '锐化', 'detail'),
  LocalOpSpec('noise', 0, 100, '降噪', 'detail'),
  LocalOpSpec('dehaze', -100, 100, '去雾', 'detail'),
];
final Map<String, LocalOpSpec> localOpByKey = {for (final s in localOpSpecs) s.key: s};

// ------------------------------------------------------------------------------------------------ brush

class BrushStroke {
  BrushStroke({this.size = 0.05, this.hardness = 50, this.flow = 100, this.erase = false, Float32List? pts}) : pts = pts ?? Float32List(0);

  /// Radius, fraction of the photo's long edge (0.0005..0.5).
  final double size;
  final double hardness, flow;
  final bool erase;

  /// x0, y0, x1, y1, ... photo-normalised. Never mutated once the stroke is in an edit.
  final Float32List pts;

  int get count => pts.length ~/ 2;

  BrushStroke withPoints(Float32List p) => BrushStroke(size: size, hardness: hardness, flow: flow, erase: erase, pts: p);

  /// A copy with [more] (x, y pairs) appended.
  BrushStroke appended(List<double> more) {
    final p = Float32List(pts.length + more.length)
      ..setAll(0, pts)
      ..setAll(pts.length, more);
    return withPoints(p);
  }

  static BrushStroke? fromJson(Object? j) {
    if (j is! Map) return null;
    final p = j['pts'];
    final xs = <double>[];
    if (p is List) {
      for (var i = 0; i + 1 < p.length; i += 2) {
        final x = p[i], y = p[i + 1];
        if (x is num && y is num) xs..add(_fix(x, -4, 5, 0))..add(_fix(y, -4, 5, 0));
      }
    }
    if (xs.isEmpty) return null;
    return BrushStroke(
      size: _fix(j['size'], 0.0005, 0.5, 0.05),
      hardness: _fix(j['hardness'], 0, 100, 50),
      flow: _fix(j['flow'], 0, 100, 100),
      erase: _rdb(j['erase'], false),
      pts: Float32List.fromList(xs),
    );
  }

  Map<String, Object> toJson() => {
        'size': jnum(size),
        'hardness': jnum(hardness),
        'flow': jnum(flow),
        if (erase) 'erase': true,
        'pts': [for (final v in pts) jnum(v)],
      };

  bool same(BrushStroke o) {
    if (identical(this, o)) return true;
    if (size != o.size || hardness != o.hardness || flow != o.flow || erase != o.erase || pts.length != o.pts.length) return false;
    for (var i = 0; i < pts.length; i++) {
      if (pts[i] != o.pts[i]) return false;
    }
    return true;
  }
}

// ------------------------------------------------------------------------------------------------ mask component

class MaskComponent {
  const MaskComponent({
    required this.kind,
    this.op = MaskOp.add,
    this.invert = false,
    this.x0 = 0.5,
    this.y0 = 0,
    this.x1 = 0.5,
    this.y1 = 0.5,
    this.cx = 0.5,
    this.cy = 0.5,
    this.rx = 0.25,
    this.ry = 0.25,
    this.angle = 0,
    this.feather = 50,
    this.lo = 0,
    this.hi = 100,
    this.lumFeather = 10,
    this.hue = 0,
    this.range = 30,
    this.hueFeather = 20,
    this.satMin = 10,
    this.strokes = const [],
    this.ref = '',
    this.refine = 50,
    this.raw,
  });

  final MaskKindId kind;
  final MaskOp op;
  final bool invert;

  /// linear: full effect at (x0, y0), none at (x1, y1).
  final double x0, y0, x1, y1;

  /// radial: centre, outer radii (fractions of the long edge), rotation (degrees, clockwise in the photo), feather 0..100 (the outer
  /// fraction over which it fades; full effect inside 1 - feather / 100).
  final double cx, cy, rx, ry, angle, feather;

  /// luminance: L* range 0..100 with a soft edge 0..50.
  final double lo, hi, lumFeather;

  /// color: OKLab hue centre (degrees), half width 0..180, feather 0..90, minimum saturation 0..100.
  final double hue, range, hueFeather, satMin;

  final List<BrushStroke> strokes;

  /// raster: plane id (dapple_mask_raster_set) and refinement 0..100.
  final String ref;
  final double refine;

  /// Unknown kinds: the component's JSON as read (written back verbatim).
  final Map<String, Object?>? raw;

  MaskComponent copyWith({
    MaskOp? op,
    bool? invert,
    double? x0,
    double? y0,
    double? x1,
    double? y1,
    double? cx,
    double? cy,
    double? rx,
    double? ry,
    double? angle,
    double? feather,
    double? lo,
    double? hi,
    double? lumFeather,
    double? hue,
    double? range,
    double? hueFeather,
    double? satMin,
    List<BrushStroke>? strokes,
    String? ref,
    double? refine,
  }) =>
      MaskComponent(
        kind: kind,
        op: op ?? this.op,
        invert: invert ?? this.invert,
        x0: x0 ?? this.x0,
        y0: y0 ?? this.y0,
        x1: x1 ?? this.x1,
        y1: y1 ?? this.y1,
        cx: cx ?? this.cx,
        cy: cy ?? this.cy,
        rx: rx ?? this.rx,
        ry: ry ?? this.ry,
        angle: angle ?? this.angle,
        feather: feather ?? this.feather,
        lo: lo ?? this.lo,
        hi: hi ?? this.hi,
        lumFeather: lumFeather ?? this.lumFeather,
        hue: hue ?? this.hue,
        range: range ?? this.range,
        hueFeather: hueFeather ?? this.hueFeather,
        satMin: satMin ?? this.satMin,
        strokes: strokes ?? this.strokes,
        ref: ref ?? this.ref,
        refine: refine ?? this.refine,
        raw: raw,
      );

  /// Brush points of all strokes.
  int get brushPoints => strokes.fold(0, (n, s) => n + s.count);

  static MaskComponent? fromJson(Object? j) {
    if (j is! Map) return null;
    final kindStr = j['kind'] is String ? j['kind'] as String : '';
    final kind = maskKindOf(kindStr);
    if (kind == MaskKindId.unknown) return MaskComponent(kind: kind, raw: Map<String, Object?>.from(j));
    final ops = j['op'];
    final op = ops == 'subtract' ? MaskOp.subtract : (ops == 'intersect' ? MaskOp.intersect : MaskOp.add);
    double c(String k, double lo, double hi, double def) => _fix(j[k], lo, hi, def);
    var lo = c('lo', 0, 100, 0), hi = c('hi', 0, 100, 100);
    if (hi < lo) {
      final t = lo;
      lo = hi;
      hi = t;
    }
    final strokes = <BrushStroke>[];
    final ss = j['strokes'];
    if (kind == MaskKindId.brush && ss is List) {
      for (final s in ss) {
        final b = BrushStroke.fromJson(s);
        if (b != null) strokes.add(b);
      }
    }
    final isLum = kind == MaskKindId.luminance, isCol = kind == MaskKindId.color;
    final hue0 = _fmod(_fmod(c('hue', -3600, 3600, 0), 360) + 360, 360);
    return MaskComponent(
      kind: kind,
      op: op,
      invert: _rdb(j['invert'], false),
      x0: c('x0', -4, 5, 0.5),
      y0: c('y0', -4, 5, 0),
      x1: c('x1', -4, 5, 0.5),
      y1: c('y1', -4, 5, 0.5),
      cx: c('cx', -4, 5, 0.5),
      cy: c('cy', -4, 5, 0.5),
      rx: c('rx', 0.001, 4, 0.25),
      ry: c('ry', 0.001, 4, 0.25),
      angle: _fmod(c('angle', -3600, 3600, 0), 360),
      // "feather" means a different thing per kind (as in the engine's reader)
      feather: isLum || isCol ? 50 : c('feather', 0, 100, 50),
      lo: lo,
      hi: hi,
      lumFeather: isLum ? c('feather', 0, 50, 10) : 10,
      hue: hue0,
      range: c('range', 0, 180, 30),
      hueFeather: isCol ? c('feather', 0, 90, 20) : 20,
      satMin: c('satMin', 0, 100, 10),
      strokes: strokes,
      ref: j['ref'] is String ? (j['ref'] as String).substring(0, math.min(128, (j['ref'] as String).length)) : '',
      refine: c('refine', 0, 100, 50),
    );
  }

  /// The engine's writer (local_adjust.cpp writeComponent): kind, op, invert when set, then the kind's own fields.
  Map<String, Object?> toJson() {
    if (kind == MaskKindId.unknown) return Map<String, Object?>.from(raw ?? const {});
    final o = <String, Object?>{
      'kind': switch (kind) {
        MaskKindId.linear => 'linear',
        MaskKindId.radial => 'radial',
        MaskKindId.luminance => 'luminance',
        MaskKindId.color => 'color',
        MaskKindId.brush => 'brush',
        MaskKindId.raster => 'raster',
        MaskKindId.unknown => 'unknown',
      },
      'op': switch (op) {
        MaskOp.add => 'add',
        MaskOp.subtract => 'subtract',
        MaskOp.intersect => 'intersect',
      },
      if (invert) 'invert': true,
    };
    switch (kind) {
      case MaskKindId.linear:
        o.addAll({'x0': jnum(x0), 'y0': jnum(y0), 'x1': jnum(x1), 'y1': jnum(y1)});
      case MaskKindId.radial:
        o.addAll({'cx': jnum(cx), 'cy': jnum(cy), 'rx': jnum(rx), 'ry': jnum(ry), 'angle': jnum(angle), 'feather': jnum(feather)});
      case MaskKindId.luminance:
        o.addAll({'lo': jnum(lo), 'hi': jnum(hi), 'feather': jnum(lumFeather)});
      case MaskKindId.color:
        o.addAll({'hue': jnum(hue), 'range': jnum(range), 'feather': jnum(hueFeather), 'satMin': jnum(satMin)});
      case MaskKindId.brush:
        o['strokes'] = [for (final s in strokes) s.toJson()];
      case MaskKindId.raster:
        o['ref'] = ref;
        o['refine'] = jnum(refine);
      case MaskKindId.unknown:
        break;
    }
    return o;
  }

  bool same(MaskComponent o) {
    if (identical(this, o)) return true;
    if (kind != o.kind || op != o.op || invert != o.invert) return false;
    if (kind == MaskKindId.unknown) return _deepEq(raw, o.raw);
    if (x0 != o.x0 || y0 != o.y0 || x1 != o.x1 || y1 != o.y1) return false;
    if (cx != o.cx || cy != o.cy || rx != o.rx || ry != o.ry || angle != o.angle || feather != o.feather) return false;
    if (lo != o.lo || hi != o.hi || lumFeather != o.lumFeather) return false;
    if (hue != o.hue || range != o.range || hueFeather != o.hueFeather || satMin != o.satMin) return false;
    if (ref != o.ref || refine != o.refine || strokes.length != o.strokes.length) return false;
    for (var i = 0; i < strokes.length; i++) {
      if (!strokes[i].same(o.strokes[i])) return false;
    }
    return true;
  }
}

bool _deepEq(Object? a, Object? b) {
  if (a is Map && b is Map) {
    if (a.length != b.length) return false;
    for (final k in a.keys) {
      if (!b.containsKey(k) || !_deepEq(a[k], b[k])) return false;
    }
    return true;
  }
  if (a is List && b is List) {
    if (a.length != b.length) return false;
    for (var i = 0; i < a.length; i++) {
      if (!_deepEq(a[i], b[i])) return false;
    }
    return true;
  }
  return a == b;
}

// ------------------------------------------------------------------------------------------------ pad state of an area (app only)

/// The pad state of one area (docs/design/local-adjustments.md section 4): light pad (lx, ly), colour pad (cx, cy), the strength
/// strip [a] (0..1, 0.5 = 100 %) and the item offsets of the itemised view (op key -> offset in JSON units). Kept by the app only.
class LocalUi {
  const LocalUi({this.lx = 0, this.ly = 0, this.cx = 0, this.cy = 0, this.a = 0.5, this.off = const {}});
  final double lx, ly, cx, cy, a;
  final Map<String, double> off;

  static const centre = LocalUi();

  LocalUi copyWith({double? lx, double? ly, double? cx, double? cy, double? a, Map<String, double>? off}) =>
      LocalUi(lx: lx ?? this.lx, ly: ly ?? this.ly, cx: cx ?? this.cx, cy: cy ?? this.cy, a: a ?? this.a, off: off ?? this.off);

  bool same(LocalUi o) =>
      lx == o.lx && ly == o.ly && cx == o.cx && cy == o.cy && a == o.a && off.length == o.off.length && off.entries.every((e) => o.off[e.key] == e.value);

  Map<String, Object> toJson() => {'lx': lx, 'ly': ly, 'cx': cx, 'cy': cy, 'a': a, if (off.isNotEmpty) 'off': off};

  static LocalUi? fromJson(Object? j) {
    if (j is! Map) return null;
    double n(String k, double d, double lo, double hi) => _fix(j[k], lo, hi, d);
    final off = <String, double>{};
    final o = j['off'];
    if (o is Map) {
      o.forEach((k, v) {
        if (k is String && v is num && v.isFinite && localOpByKey.containsKey(k)) off[k] = v.toDouble();
      });
    }
    return LocalUi(lx: n('lx', 0, -1, 1), ly: n('ly', 0, -1, 1), cx: n('cx', 0, -1, 1), cy: n('cy', 0, -1, 1), a: n('a', 0.5, 0, 1), off: off);
  }
}

// ------------------------------------------------------------------------------------------------ adjustment

class LocalAdjustment {
  const LocalAdjustment({
    required this.id,
    this.name = '',
    this.on = true,
    this.invert = false,
    this.strength = 100,
    this.mask = const [],
    this.ops = const {},
    this.ui,
  });

  final String id, name;
  final bool on, invert;

  /// 0..100.
  final double strength;
  final List<MaskComponent> mask;

  /// Non-zero ops only, JSON units (exposure EV x 20).
  final Map<String, double> ops;

  /// The app's pad state; null = derived from the ops ([localUiOf]).
  final LocalUi? ui;

  double op(String key) => ops[key] ?? 0;

  bool get hasEffect => strength > 0 && ops.entries.any((e) => e.key != 'color_hue' && e.value != 0);

  LocalAdjustment copyWith({String? id, String? name, bool? on, bool? invert, double? strength, List<MaskComponent>? mask, Map<String, double>? ops, Object? ui = _keep}) =>
      LocalAdjustment(
        id: id ?? this.id,
        name: name ?? this.name,
        on: on ?? this.on,
        invert: invert ?? this.invert,
        strength: strength ?? this.strength,
        mask: mask ?? this.mask,
        ops: ops ?? this.ops,
        ui: identical(ui, _keep) ? this.ui : ui as LocalUi?,
      );
  static const Object _keep = Object();

  /// [i]-th component replaced.
  LocalAdjustment withComponent(int i, MaskComponent c) => copyWith(mask: [...mask]..[i] = c);

  static LocalAdjustment? fromJson(Object? j, {bool readUi = true}) {
    if (j is! Map) return null;
    final mask = <MaskComponent>[];
    final m = j['mask'];
    if (m is List) {
      for (final c in m) {
        final mc = MaskComponent.fromJson(c);
        if (mc != null) mask.add(mc);
      }
    }
    final ops = <String, double>{};
    final ad = j['adjust'];
    if (ad is Map) {
      for (final s in localOpSpecs) {
        final v = ad[s.key];
        if (v is num) {
          final x = _fix(v, s.min, s.max, 0);
          if (x != 0) ops[s.key] = x;
        }
      }
    }
    String str(Object? v, int cap) => v is String ? (v.length > cap ? v.substring(0, cap) : v) : '';
    return LocalAdjustment(
      id: str(j['id'], 64),
      name: str(j['name'], 128),
      on: _rdb(j['on'], true),
      invert: _rdb(j['invert'], false),
      strength: _fix(j['strength'], 0, 100, 100),
      mask: mask.length > kMaxMaskComponents ? mask.sublist(0, kMaxMaskComponents) : mask,
      ops: ops,
      ui: readUi ? LocalUi.fromJson(j['ui']) : null,
    );
  }

  /// Engine key order: id, name (when set), on (when off), invert (when set), strength, mask, adjust (non-zero, registry order).
  Map<String, Object?> toJson({bool withUi = false}) => {
        'id': id,
        if (name.isNotEmpty) 'name': name,
        if (!on) 'on': false,
        if (invert) 'invert': true,
        'strength': jnum(strength),
        'mask': [for (final c in mask) c.toJson()],
        'adjust': {
          for (final s in localOpSpecs)
            if ((ops[s.key] ?? 0) != 0) s.key: jnum(ops[s.key]!),
        },
        if (withUi && ui != null) 'ui': ui!.toJson(),
      };

  bool same(LocalAdjustment o) {
    if (identical(this, o)) return true;
    if (id != o.id || name != o.name || on != o.on || invert != o.invert || strength != o.strength) return false;
    if (mask.length != o.mask.length || ops.length != o.ops.length) return false;
    if (!ops.entries.every((e) => o.ops[e.key] == e.value)) return false;
    if ((ui == null) != (o.ui == null) || (ui != null && !ui!.same(o.ui!))) return false;
    for (var i = 0; i < mask.length; i++) {
      if (!mask[i].same(o.mask[i])) return false;
    }
    return true;
  }
}

// ------------------------------------------------------------------------------------------------ the list

class LocalList {
  const LocalList([this.items = const []]);
  final List<LocalAdjustment> items;

  static const empty = LocalList();

  bool get isEmpty => items.isEmpty;
  int get length => items.length;

  int indexOf(String id) => items.indexWhere((a) => a.id == id);
  LocalAdjustment? byId(String id) {
    final i = indexOf(id);
    return i < 0 ? null : items[i];
  }

  /// Components of a kind over the whole edit (the engine's per-edit caps).
  int countKind(MaskKindId k) => items.fold(0, (n, a) => n + a.mask.where((c) => c.kind == k).length);

  LocalList replace(int i, LocalAdjustment a) => LocalList([...items]..[i] = a);
  LocalList removeAt(int i) => LocalList([...items]..removeAt(i));
  LocalList insert(int i, LocalAdjustment a) => LocalList([...items]..insert(i, a));
  LocalList add(LocalAdjustment a) => LocalList([...items, a]);

  /// Accepts the engine's object ({"v":1,"items":[...]}) or a bare array of items; tolerant (bad entries skipped), clamped, capped.
  static LocalList fromJson(Object? j) {
    Object? items = j;
    if (j is Map) items = j['items'];
    if (items is! List) return empty;
    final out = <LocalAdjustment>[];
    for (final it in items) {
      final a = LocalAdjustment.fromJson(it);
      if (a != null) out.add(a);
      if (out.length == kMaxLocalAdjustments) break;
    }
    return LocalList(out);
  }

  /// What the engine reads (no "ui"); null when the list is empty (the key is then left out).
  Map<String, Object?>? toEngineJson() => items.isEmpty ? null : {'v': 1, 'items': [for (final a in items) a.toJson()]};

  /// What the app persists (with each item's pad state).
  Map<String, Object?>? toAppJson() => items.isEmpty ? null : {'v': 1, 'items': [for (final a in items) a.toJson(withUi: true)]};

  bool same(LocalList o) {
    if (identical(this, o) || identical(items, o.items)) return true;
    if (items.length != o.items.length) return false;
    for (var i = 0; i < items.length; i++) {
      if (!items[i].same(o.items[i])) return false;
    }
    return true;
  }
}
