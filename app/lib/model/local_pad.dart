// The pads of an area (docs/design/local-adjustments.md section 4): the SAME pad + strength idiom as the global categories. A pad
// position is a recipe (pure function of the light pad, the colour pad and the strip) -> the area's ops; the itemised view adds
// offsets on top. Centre + strip at 0.5 = no ops and strength 100.
import 'dart:math' as math;

import 'local_adjust.dart';
import 'portrait.dart';

/// Engine strength (0..100) of the strip position [a]: 0 at the bottom, 100 from the middle up.
double localStrengthOf(double a) => 100 * math.min(1.0, a * 2);

/// Above the middle the recipe grows to twice its size at the top.
double localBoost(double a) => math.max(1.0, a * 2);

/// The ops the pads imply (JSON units: exposure EV x 20). Pure; centre = all zero.
Map<String, double> localRecipe(LocalUi u) {
  final k = localBoost(u.a);
  return {
    'exposure': 30 * u.lx * k, // +-1.5 EV at the edge (x2 at the top of the strip)
    'contrast': 40 * u.ly * k,
    'whites': 10 * u.ly * k,
    'blacks': -10 * u.ly * k,
    'temp': 40 * u.cx * k,
    'saturation': 30 * u.cy * k,
    'vibrance': 15 * u.cy * k,
  };
}

double _clampOp(String key, double v) {
  final s = localOpByKey[key]!;
  return v.clamp(s.min, s.max).toDouble();
}

/// The pad state of [a]: the stored one, or for an area that has none (made elsewhere, an older build) the centre with every op as an
/// offset and the strip where its strength puts it, so the first touch changes nothing.
LocalUi localUiOf(LocalAdjustment a) {
  final u = a.ui;
  if (u != null) return u;
  final pt = portraitOf(a);
  if (pt != null) {
    // a portrait area without pad state: the strip in the middle, what differs from the default recipe as offsets
    final r = portraitRecipe(pt.$2, 0.5);
    final off = <String, double>{};
    for (final k in {...a.ops.keys, ...r.keys}) {
      final d = a.op(k) - (r[k] ?? 0);
      if (d.abs() > 1e-9) off[k] = d;
    }
    return LocalUi(off: off);
  }
  return LocalUi(a: (a.strength / 200).clamp(0.0, 0.5), off: Map<String, double>.of(a.ops));
}

/// The recipe of [a] at the pad state [u]: a portrait area's chip recipe (the strip scales it), else the light / colour pads.
Map<String, double> recipeOf(LocalAdjustment a, LocalUi u) {
  final pt = portraitOf(a);
  return pt == null ? localRecipe(u) : portraitRecipe(pt.$2, u.a);
}

/// [a] with the pad state [u]: ops = recipe + offsets (clamped, zeros dropped), strength from the strip.
LocalAdjustment applyLocalUi(LocalAdjustment a, LocalUi u) {
  final pt = portraitOf(a);
  final r = recipeOf(a, u);
  final ops = <String, double>{};
  for (final s in localOpSpecs) {
    var v = _clampOp(s.key, (r[s.key] ?? 0) + (u.off[s.key] ?? 0));
    if (pt != null) v = v.clamp(localOpByKey[s.key]!.min, portraitMax(pt.$2, s.key)).toDouble(); // the recipe's maxima
    if (v.abs() > 1e-9) ops[s.key] = _round(v);
  }
  final off = {for (final e in u.off.entries) if (e.value.abs() > 1e-9) e.key: e.value};
  // a portrait chip's strip scales its recipe; the engine strength stays 100
  return a.copyWith(ops: ops, strength: pt != null ? 100 : _round(localStrengthOf(u.a)), ui: u.copyWith(off: off));
}

/// Values sent to the engine are kept to 0.01 (the JSON stays short; no visible step).
double _round(double v) => (v * 100).roundToDouble() / 100;

/// One op set to [v] from the itemised view: the value is written as is, the pads stay, the difference to the recipe becomes the
/// op's offset.
LocalAdjustment setLocalItem(LocalAdjustment a, String key, double v) {
  final u = localUiOf(a);
  final r = recipeOf(a, u);
  final pt = portraitOf(a);
  final c = pt == null ? _clampOp(key, v) : v.clamp(localOpByKey[key]!.min, portraitMax(pt.$2, key)).toDouble();
  final off = Map<String, double>.of(u.off)..[key] = c - (r[key] ?? 0);
  return applyLocalUi(a, u.copyWith(off: off));
}

/// Back to no adjustment (the shape stays).
LocalAdjustment resetLocalOps(LocalAdjustment a) => a.copyWith(ops: const {}, strength: 100, ui: LocalUi.centre);

bool localOpsAdjusted(LocalAdjustment a) => a.ops.isNotEmpty || a.strength != 100 || !(a.ui?.same(LocalUi.centre) ?? true);

/// Header readout of the area's pads: 强度 n% · 亮 n · 暖 n (only the directions that are away from the centre).
String localReadout(LocalUi u) {
  String dir(double v, String neg, String pos) => v == 0 ? '' : ' · ${v > 0 ? pos : neg} ${(v.abs() * 100).round()}';
  return '强度 ${(u.a * 200).round()}%${dir(u.lx, '暗', '亮')}${dir(u.cx, '冷', '暖')}';
}
