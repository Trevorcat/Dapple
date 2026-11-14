// Static data: built-in looks (requirements 4.12), publish presets (4.11.2) and
// the demo photo set used by tests / screenshots (taken from the prototype).
import 'dart:math' as math;
import 'dart:ui';

import 'package:flutter/painting.dart';

// ---------------------------------------------------------------- looks

class DLook {
  const DLook(this.id, this.name, this.tags, this.cat, this.px, this.py, this.swatch,
      {this.sat = 1, this.sepia = 0, this.contrast = 1, this.bright = 1, this.gray = 0});
  final String id, name, tags, cat;

  /// Palette default point.
  final double px, py;
  final List<Color> swatch;

  /// CSS-filter style approximations, only used when the engine is unavailable (demo / tests).
  final double sat, sepia, contrast, bright, gray;
}

const lookCats = [
  ['all', '全部'],
  ['warm', '暖色'],
  ['cool', '冷色'],
  ['neutral', '素净'],
  ['film', '胶片'],
  ['bw', '黑白'],
];

const dLooks = <DLook>[
  DLook('std', '标准', '中性 · 不加倾向', 'neutral', 0.50, 0.50, [Color(0xFFE9E4DA), Color(0xFFB9C4CF)]),
  DLook('warm', '暖阳', '暖色 · 柔和', 'warm', 0.56, 0.30, [Color(0xFFFFD29A), Color(0xFFE9A25E)], sat: 1.04),
  DLook('amber', '琥珀', '暖色 · 浓郁', 'warm', 0.66, 0.22, [Color(0xFFFFBF5E), Color(0xFFB8692A)], sepia: 0.12, sat: 1.1),
  DLook('rose', '玫瑰金', '暖粉 · 柔和', 'warm', 0.50, 0.36, [Color(0xFFF6C7C0), Color(0xFFC98A8F)], sat: 1.02),
  DLook('neutral', '淡雅', '低饱和 · 干净', 'neutral', 0.38, 0.50, [Color(0xFFE2E2E2), Color(0xFF9A9A9A)], sat: 0.92),
  DLook('cool', '冷调', '冷色 · 安静', 'cool', 0.44, 0.74, [Color(0xFFCFE2F7), Color(0xFF6F93BD)]),
  DLook('teal', '青橙', '冷暖互补 · 电影感', 'cool', 0.62, 0.62, [Color(0xFFF0A55E), Color(0xFF2F8A8F)],
      contrast: 1.04, sat: 1.08, sepia: 0.1),
  DLook('film', '胶片感', '偏暖 · 灰黑位 · 颗粒', 'film', 0.46, 0.40, [Color(0xFFEFE1C4), Color(0xFF9C8A6B)],
      sepia: 0.22, contrast: 0.94, bright: 1.03),
  DLook('bw', '黑白', '单色 · 高反差', 'bw', 0.50, 0.50, [Color(0xFFF5F5F5), Color(0xFF1C1C1C)], gray: 1, contrast: 1.12),
];

DLook lookById(String? id) => dLooks.firstWhere((l) => l.id == id, orElse: () => dLooks.first);

// ---------------------------------------------------------------- publish presets

class Preset {
  const Preset(this.id, this.name, this.spec, this.maxCount, this.defRatio, this.ratios, this.longEdge, this.app,
      {this.package, this.iosScheme});
  final String id, name, spec;
  final int? maxCount;
  final String defRatio;
  final List<String> ratios;

  /// null = the Settings default export size.
  final int? longEdge;

  /// Display name of the target app ("其他 App" has none).
  final String? app;

  /// Android package for the "open app" fallback.
  final String? package;

  /// iOS URL scheme for the same fallback (data only; the iOS service opens it with canOpenURL / openURL, and the schemes must be listed in
  /// LSApplicationQueriesSchemes). docs/design/publish-presets.md has the sources and confidence.
  final String? iosScheme;
}

// Counts / sizes / ratios: see docs/design/publish-presets.md for what is verified (Instagram) and what is still an assumption (requirements 4.11.2 [待验]).
const presets = <Preset>[
  Preset('wechat', '朋友圈', '原比例 · 九宫格', 9, 'orig', ['orig', '1:1'], 1440, '微信', package: 'com.tencent.mm', iosScheme: 'weixin://'),
  Preset('xhs', '小红书', '3:4 · 多图', 18, '3:4', ['3:4', '1:1', 'orig'], 2048, '小红书', package: 'com.xingin.xhs', iosScheme: 'xhsdiscover://'),
  Preset('weibo', '微博', '原比例 · 多图', 18, 'orig', ['orig', '1:1'], null, '微博', package: 'com.sina.weibo', iosScheme: 'sinaweibo://'),
  Preset('douyin', '抖音图文', '3:4 · 图文', null, '3:4', ['3:4', '9:16'], 2048, '抖音',
      package: 'com.ss.android.ugc.aweme', iosScheme: 'snssdk1128://'),
  Preset('ig', 'Instagram', '4:5 · 轮播', 20, '4:5', ['4:5', '1:1', 'orig'], 2048, 'Instagram',
      package: 'com.instagram.android', iosScheme: 'instagram://'),
  Preset('other', '其他 App', '系统分享', null, 'orig', ['orig'], null, null),
];

// ---------------------------------------------------------------- demo scenes

/// A procedurally drawn stand-in for a photo, used when no real image file exists.
class DemoScene {
  const DemoScene({
    required this.stops,
    required this.colors,
    this.angle = 180,
    this.hardStops = false,
    this.dots = const [],
    this.ellipses = const [],
    this.vbands = const [],
  });
  final List<double> stops;
  final List<Color> colors;

  /// CSS-style angle in degrees (180 = top to bottom, 90 = left to right).
  final double angle;
  final bool hardStops;
  final List<(double, double, double, Color)> dots; // cx, cy, r (fractions of width)
  final List<(double, double, double, double, Color)> ellipses; // cx, cy, rx, ry
  final List<(double, double, Color)> vbands; // x0, x1
}

class DemoPhotoSpec {
  const DemoPhotoSpec(this.name, this.ratio, this.exif, this.scene, this.key, this.range, this.hist,
      {this.flag = false});
  final String name;

  /// width / height
  final double ratio;
  final String exif;
  final DemoScene scene;
  final String key, range;
  final bool flag;

  /// (mean, sd, weight) gaussians over 0..1
  final List<List<double>> hist;
}

const demoPhotos = <DemoPhotoSpec>[
  DemoPhotoSpec(
      '晨雾湖面', 1.5, 'ISO 200 · 1/500 s · f/8 · 24mm',
      DemoScene(stops: [0, .47, .5, 1], colors: [Color(0xFFC9D6DF), Color(0xFFE9EEF1), Color(0xFF8199A8), Color(0xFF4F6577)], hardStops: false),
      'high', 'short', [[0.78, 0.07, 0.75], [0.55, 0.08, 0.25]]),
  DemoPhotoSpec(
      '外滩夕阳', 1.5, 'ISO 100 · 1/1000 s · f/5.6 · 70mm',
      DemoScene(stops: [0, .38, .41, 1], colors: [Color(0xFFF2A65A), Color(0xFFF7CF8F), Color(0xFF6A5A7A), Color(0xFF2E2B40)]),
      'mid', 'long', [[0.18, 0.08, 0.45], [0.72, 0.1, 0.45], [0.96, 0.02, 0.1]]),
  DemoPhotoSpec(
      '街角咖啡', 2 / 3, 'ISO 800 · 1/125 s · f/2 · 35mm',
      DemoScene(stops: [0, 1], colors: [Color(0xFF8A5D3F), Color(0xFF3B2A20)], angle: 160, dots: [(0.38, 0.58, 0.16, Color(0xFFEAD0AE))]),
      'low', 'mid', [[0.2, 0.09, 0.75], [0.62, 0.08, 0.25]]),
  DemoPhotoSpec(
      '窗边人像', 2 / 3, 'ISO 400 · 1/200 s · f/1.8 · 85mm',
      DemoScene(stops: [0, 1], colors: [Color(0xFFDDD6CB), Color(0xFFA39A8D)],
          ellipses: [(0.5, 0.38, 0.2, 0.24, Color(0xFFE5B697)), (0.5, 0.86, 0.36, 0.30, Color(0xFF5B6B7D))]),
      'high', 'mid', [[0.7, 0.1, 0.7], [0.4, 0.08, 0.3]]),
  DemoPhotoSpec(
      '老街', 1.5, 'ISO 320 · 1/250 s · f/4 · 28mm',
      DemoScene(stops: [0, 1], colors: [Color(0xFF474C54), Color(0xFF474C54)], vbands: [(0.28, 0.72, Color(0xFFBBA78B))]),
      'mid', 'mid', [[0.3, 0.08, 0.5], [0.62, 0.08, 0.5]]),
  DemoPhotoSpec(
      '雪山', 1.5, 'ISO 100 · 1/1600 s · f/8 · 100mm',
      DemoScene(stops: [0, .44, .46, .7, 1], colors: [Color(0xFF94BFE6), Color(0xFFD8E8F5), Color(0xFFFFFFFF), Color(0xFFC9D4DD), Color(0xFF6B7A86)], angle: 172),
      'high', 'long', [[0.85, 0.07, 0.7], [0.35, 0.1, 0.3]]),
  DemoPhotoSpec(
      '霓虹夜街', 2 / 3, 'ISO 3200 · 1/60 s · f/1.8 · 35mm',
      DemoScene(stops: [0, 1], colors: [Color(0xFF151B34), Color(0xFF2B203E)], dots: [(0.70, 0.36, 0.06, Color(0xFFFF7AA2)), (0.28, 0.58, 0.05, Color(0xFF5AD1FF))]),
      'low', 'long', [[0.1, 0.06, 0.85], [0.9, 0.06, 0.15]],
      flag: true),
  DemoPhotoSpec(
      '森林小径', 1.5, 'ISO 640 · 1/160 s · f/2.8 · 35mm',
      DemoScene(stops: [0, .45, 1], colors: [Color(0xFFA9C9A4), Color(0xFF5F8A5A), Color(0xFF2E4A2C)]),
      'low', 'mid', [[0.22, 0.08, 0.65], [0.55, 0.1, 0.35]]),
  DemoPhotoSpec(
      '海边礁石', 1.5, 'ISO 100 · 1/800 s · f/7.1 · 24mm',
      DemoScene(stops: [0, .45, .6, 1], colors: [Color(0xFFBFE0EF), Color(0xFF7FBCD6), Color(0xFF2F6F8A), Color(0xFF3B3A35)]),
      'mid', 'long', [[0.2, 0.07, 0.4], [0.5, 0.1, 0.3], [0.82, 0.07, 0.3]]),
  DemoPhotoSpec(
      '花市', 1.5, 'ISO 250 · 1/320 s · f/2.8 · 50mm',
      DemoScene(stops: [0, 1], colors: [Color(0xFF8FBF7A), Color(0xFF4F7A43)], dots: [(0.30, 0.40, 0.12, Color(0xFFF06D7A)), (0.65, 0.60, 0.10, Color(0xFFF7C14A))]),
      'mid', 'mid', [[0.45, 0.13, 1]]),
  DemoPhotoSpec(
      '地铁站', 1.5, 'ISO 1600 · 1/100 s · f/2 · 35mm',
      DemoScene(stops: [0, .35, .35, .7, .7, 1], colors: [Color(0xFFD9DDE0), Color(0xFFD9DDE0), Color(0xFF9AA3AA), Color(0xFF9AA3AA), Color(0xFF5B636A), Color(0xFF5B636A)], hardStops: true),
      'mid', 'short', [[0.5, 0.07, 1]]),
  DemoPhotoSpec(
      '黄昏剪影', 1.5, 'ISO 100 · 1/640 s · f/8 · 50mm',
      DemoScene(stops: [0, .4, .75, 1], colors: [Color(0xFFFFB36B), Color(0xFFFF8A5C), Color(0xFF3A2A35), Color(0xFF1E1820)]),
      'low', 'long', [[0.08, 0.05, 0.6], [0.72, 0.08, 0.4]]),
];

/// Builds the 64-bin histogram of a demo spec (sum = 1).
List<double> demoHistogram(List<List<double>> g, {double brightness = 1, double contrast = 1}) {
  final h = List<double>.filled(64, 0);
  for (var i = 0; i < 64; i++) {
    final x = (i + .5) / 64;
    for (final c in g) {
      // brightness shifts the mean; contrast scales the spread
      final m = (c[0] * brightness).clamp(0.0, 1.0);
      final sd = c[1] * contrast;
      h[i] += c[2] * math.exp(-math.pow((x - m) / sd, 2) / 2) / sd;
    }
  }
  final sum = h.fold<double>(0, (a, b) => a + b);
  return [for (final v in h) v / sum];
}

// ---------------------------------------------------------------- approximate filter (demo only)

/// CSS-filter-like colour matrix: brightness, contrast, saturation, grayscale, sepia, warm shift.
ColorFilter approxFilter({double b = 1, double c = 1, double s = 1, double gray = 0, double sepia = 0, double warm = 0}) {
  // saturation matrix
  const lr = 0.2126, lg = 0.7152, lb = 0.0722;
  final sat = s * (1 - gray);
  final sm = <double>[
    lr + (1 - lr) * sat, lg - lg * sat, lb - lb * sat,
    lr - lr * sat, lg + (1 - lg) * sat, lb - lb * sat,
    lr - lr * sat, lg - lg * sat, lb + (1 - lb) * sat,
  ];
  final t = 128 * (1 - c) + 0; // contrast pivot
  final k = b * c;
  final wr = 1 + warm * 0.10 + sepia * 0.2, wb = 1 - warm * 0.12 - sepia * 0.2;
  return ColorFilter.matrix(<double>[
    sm[0] * k * wr, sm[1] * k * wr, sm[2] * k * wr, 0, t,
    sm[3] * k, sm[4] * k, sm[5] * k, 0, t,
    sm[6] * k * wb, sm[7] * k * wb, sm[8] * k * wb, 0, t,
    0, 0, 0, 1, 0,
  ]);
}
