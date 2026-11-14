// The kinds of area the user can add (docs/design/local-adjustments.md section 9). Data, not code paths: the shape card, the chip
// glyphs, the tool strip and the limit checks all read this table. The AI kinds (人物, 背景, 点选, 天空, 远近) are rows like the others: what
// the device can do ([LocalKind.aiTasks] against the service's availability) and what the analysis found decide whether a row is offered.
import 'local_adjust.dart';

/// How a kind is placed on the photo.
enum LocalPlace {
  /// Handles on the photo (gradient, ellipse).
  handles,

  /// Painted with one finger.
  paint,

  /// A tap on the photo picks the tone / colour; the width is set on the tool strip.
  sample,

  /// AI: the analysis finds parts (person parts, face features); the user picks one or several.
  aiParts,

  /// AI: one plane of the analysis (背景, 天空).
  aiPlane,

  /// AI: taps on the photo select objects (more taps add, a toggle subtracts).
  aiTap,

  /// AI: the depth plane; a near-far band is chosen on the tool strip.
  aiDepth,

  /// AI: 人像, the portrait chips (one-tap retouch areas on the face planes).
  aiPortrait,
}

/// The glyph drawn for a kind (local_glyphs.dart).
enum LocalGlyph { linear, radial, brush, luminance, color, subject, sky, person, depth, raster, background, tap, portrait }

/// AiTask bits (app/lib/native/ai_bindings.dart), repeated here so the model needs no native import.
abstract final class AiBits {
  static const person = 1, faces = 2, depth = 4, sky = 8, tapPrepare = 16;
}

class LocalKind {
  const LocalKind(this.id, this.label, this.engine, this.glyph, this.place, {this.visible = true, this.aiKey, this.aiTasks = 0, this.note, this.overlayFollowsStale = false});
  final String id;

  /// The word under the icon on the shape card (plain Chinese, no jargon).
  final String label;

  /// A second, smaller line on the tile (an honest qualifier: 粗略).
  final String? note;
  final MaskKindId engine;
  final LocalGlyph glyph;
  final LocalPlace place;

  /// False: kept in the table, not offered.
  final bool visible;

  /// AI kinds: the plane key ("background", "sky", "depth"; "tap" for tap select; null for the parts picker).
  final String? aiKey;

  /// AI kinds: the AiTask bits the analysis must run (and the device must support) for this kind.
  final int aiTasks;

  /// Range kinds (tone, hue, near-far band): while the band is dragged the last red overlay stays up (stale) until the fresh one replaces
  /// it, so the selection visibly follows the drag. Shape kinds hide a stale overlay (their handles show the shape).
  final bool overlayFollowsStale;

  bool get ai => place == LocalPlace.aiParts || place == LocalPlace.aiPlane || place == LocalPlace.aiTap || place == LocalPlace.aiDepth || place == LocalPlace.aiPortrait;

  /// A range kind (tone / colour) added to an area that already has a shape LIMITS it (intersect) instead of adding to it. AI planes behave
  /// as shapes.
  bool get limits => place == LocalPlace.sample;
}

const List<LocalKind> localKinds = [
  LocalKind('linear', '渐变', MaskKindId.linear, LocalGlyph.linear, LocalPlace.handles),
  LocalKind('radial', '圆形', MaskKindId.radial, LocalGlyph.radial, LocalPlace.handles),
  LocalKind('brush', '画笔', MaskKindId.brush, LocalGlyph.brush, LocalPlace.paint),
  LocalKind('luminance', '明暗', MaskKindId.luminance, LocalGlyph.luminance, LocalPlace.sample, overlayFollowsStale: true),
  LocalKind('color', '颜色', MaskKindId.color, LocalGlyph.color, LocalPlace.sample, overlayFollowsStale: true),
  LocalKind('portrait', '人像', MaskKindId.raster, LocalGlyph.portrait, LocalPlace.aiPortrait, aiTasks: AiBits.person | AiBits.faces),
  LocalKind('person', '人物', MaskKindId.raster, LocalGlyph.person, LocalPlace.aiParts, aiTasks: AiBits.person | AiBits.faces),
  LocalKind('background', '背景', MaskKindId.raster, LocalGlyph.background, LocalPlace.aiPlane, aiKey: 'background', aiTasks: AiBits.person),
  LocalKind('tap', '点选', MaskKindId.raster, LocalGlyph.tap, LocalPlace.aiTap, aiKey: 'tap', aiTasks: AiBits.tapPrepare),
  LocalKind('sky', '天空', MaskKindId.raster, LocalGlyph.sky, LocalPlace.aiPlane, aiKey: 'sky', aiTasks: AiBits.sky, note: '粗略'),
  LocalKind('depth', '远近', MaskKindId.raster, LocalGlyph.depth, LocalPlace.aiDepth, aiKey: 'depth', aiTasks: AiBits.depth, overlayFollowsStale: true),
];

/// The classic (non-AI) kinds: always offered.
List<LocalKind> get classicKinds => [for (final k in localKinds) if (k.visible && !k.ai) k];

/// The kinds offered on the shape card: the classic ones, plus the AI kinds the device can do ([deviceTasks], AiTask bits) that the
/// analysis did not rule out for this photo ([absent]: kind ids known to have nothing, e.g. 背景 when there is no person, 天空 without sky).
/// 人物 stays when no person was found: its card then says so plainly.
List<LocalKind> offeredKinds({int deviceTasks = 0, Set<String> absent = const {}}) => [
      for (final k in localKinds)
        if (k.visible && (!k.ai || ((k.aiTasks & deviceTasks) == k.aiTasks && !absent.contains(k.id)))) k,
    ];

/// Without AI (tests, an engine without models): the classic kinds.
List<LocalKind> get pickableKinds => offeredKinds();

LocalKind kindById(String id) => localKinds.firstWhere((k) => k.id == id);

/// The plane key of an `ai:photo:key` id (the key may contain ':'), or null.
String? aiPlaneKey(String ref) {
  if (!ref.startsWith('ai:')) return null;
  final i = ref.indexOf(':', 3);
  return i < 0 ? null : ref.substring(i + 1);
}

/// The kind a stored component stands for (raster: by the plane id's key).
LocalKind kindOfComponent(MaskComponent c) {
  if (c.kind == MaskKindId.raster) {
    final key = aiPlaneKey(c.ref) ?? (c.ref.isEmpty ? 'tap' : '');
    if (key == 'background') return kindById('background');
    if (key == 'sky') return kindById('sky');
    if (key == 'depth') return kindById('depth');
    if (key.startsWith('tap')) return kindById('tap');
    if (const {'person', 'hair', 'faceskin', 'bodyskin', 'clothes'}.contains(key) || RegExp(r'^f\d+:').hasMatch(key)) return kindById('person');
    return const LocalKind('raster', '区域', MaskKindId.raster, LocalGlyph.raster, LocalPlace.aiPlane, visible: false);
  }
  for (final k in localKinds) {
    if (k.engine == c.kind && !k.ai) return k;
  }
  return const LocalKind('unknown', '区域', MaskKindId.unknown, LocalGlyph.raster, LocalPlace.aiPlane, visible: false);
}

/// The plain word of a person / face plane key (`ai:photo:key`): 整个人, 脸部皮肤 ... (null for keys that are not offered).
String? partLabel(String key) {
  const parts = {'person': '整个人', 'faceskin': '脸部皮肤', 'bodyskin': '身体皮肤', 'hair': '头发', 'clothes': '衣服'};
  const face = {'eyes': '眼睛', 'iris': '虹膜', 'sclera': '眼白', 'lips': '嘴唇', 'brows': '眉毛', 'teeth': '牙齿'};
  if (parts.containsKey(key)) return parts[key];
  final m = RegExp(r'^f(\d+):(\w+)$').firstMatch(key);
  if (m != null) return face[m.group(2)];
  return null;
}

/// The fold op of a new component: the first one adds; "减一块" subtracts; a range kind added to a shape limits it.
MaskOp opForNew(LocalKind k, {required bool first, required bool subtract}) {
  if (first) return MaskOp.add;
  if (subtract) return MaskOp.subtract;
  return k.limits ? MaskOp.intersect : MaskOp.add;
}

/// Why [n] components of [k] cannot be added to the area at [index] of [list] (the toast), or null.
String? limitFor(LocalList list, LocalKind k, {int? index, int n = 1}) {
  if (index == null && list.length >= kMaxLocalAdjustments) return '最多 $kMaxLocalAdjustments 个区域';
  if (index != null && list.items[index].mask.length + n > kMaxMaskComponents) return '每个区域最多 $kMaxMaskComponents 块';
  if (k.engine == MaskKindId.brush && list.countKind(MaskKindId.brush) + n > kMaxBrushComponents) return '画笔最多 $kMaxBrushComponents 块';
  if (k.engine == MaskKindId.raster && list.rasterPlanes + n > kMaxRasterComponents) return '人物、天空、点选最多 $kMaxRasterComponents 块';
  return null;
}
