// The kinds of area the user can add (docs/design/local-adjustments.md section 9). Data, not code paths: the shape card, the chip
// glyphs, the tool strip and the limit checks all read this table. AI masks (subject, sky, person parts, tap-to-select, depth) arrive
// as `raster` kinds: one row each plus a [LocalRasterProvider] that makes the plane; a raster kind without a provider is not shown.
import 'local_adjust.dart';

/// How a kind is placed on the photo.
enum LocalPlace {
  /// Handles on the photo (gradient, ellipse).
  handles,

  /// Painted with one finger.
  paint,

  /// A tap on the photo picks the tone / colour; the width is set on the tool strip.
  sample,

  /// Computed elsewhere (an AI model, depth) and supplied as a raster plane.
  external,
}

/// The glyph drawn for a kind (local_glyphs.dart).
enum LocalGlyph { linear, radial, brush, luminance, color, subject, sky, person, depth, raster }

class LocalKind {
  const LocalKind(this.id, this.label, this.engine, this.glyph, this.place, {this.visible = true, this.rasterTag});
  final String id;

  /// The word under the icon on the shape card (plain Chinese, no jargon).
  final String label;
  final MaskKindId engine;
  final LocalGlyph glyph;
  final LocalPlace place;

  /// False: kept in the table, not offered yet.
  final bool visible;

  /// raster kinds: the prefix of the plane id (`subject:photo:n`) that tells the glyph of a stored component.
  final String? rasterTag;

  /// A range kind (tone / colour) added to an area that already has a shape LIMITS it (intersect) instead of adding to it.
  bool get limits => place == LocalPlace.sample;
}

/// Makes the raster plane of an external kind (later round). Returns the plane id passed to dapple_mask_raster_set, or null.
abstract class LocalRasterProvider {
  Future<String?> makeRaster(String photoId, LocalKind kind, {double? u, double? v});
}

/// Providers by kind id; empty in this round (no AI kind is offered).
final Map<String, LocalRasterProvider> localRasterProviders = {};

const List<LocalKind> localKinds = [
  LocalKind('linear', '渐变', MaskKindId.linear, LocalGlyph.linear, LocalPlace.handles),
  LocalKind('radial', '圆形', MaskKindId.radial, LocalGlyph.radial, LocalPlace.handles),
  LocalKind('brush', '画笔', MaskKindId.brush, LocalGlyph.brush, LocalPlace.paint),
  LocalKind('luminance', '明暗', MaskKindId.luminance, LocalGlyph.luminance, LocalPlace.sample),
  LocalKind('color', '颜色', MaskKindId.color, LocalGlyph.color, LocalPlace.sample),
  // AI masks: rows ready, hidden until their provider is registered
  LocalKind('subject', '主体', MaskKindId.raster, LocalGlyph.subject, LocalPlace.external, visible: false, rasterTag: 'subject'),
  LocalKind('sky', '天空', MaskKindId.raster, LocalGlyph.sky, LocalPlace.external, visible: false, rasterTag: 'sky'),
  LocalKind('person', '人物', MaskKindId.raster, LocalGlyph.person, LocalPlace.external, visible: false, rasterTag: 'person'),
  LocalKind('depth', '远近', MaskKindId.raster, LocalGlyph.depth, LocalPlace.external, visible: false, rasterTag: 'depth'),
];

/// The kinds offered on the shape card.
List<LocalKind> get pickableKinds =>
    [for (final k in localKinds) if (k.visible && (k.place != LocalPlace.external || localRasterProviders.containsKey(k.id))) k];

LocalKind kindById(String id) => localKinds.firstWhere((k) => k.id == id);

/// The kind a stored component stands for (raster: by the plane id's tag).
LocalKind kindOfComponent(MaskComponent c) {
  if (c.kind == MaskKindId.raster) {
    for (final k in localKinds) {
      final t = k.rasterTag;
      if (t != null && c.ref.startsWith('$t:')) return k;
    }
    return const LocalKind('raster', '区域', MaskKindId.raster, LocalGlyph.raster, LocalPlace.external, visible: false);
  }
  for (final k in localKinds) {
    if (k.engine == c.kind) return k;
  }
  return const LocalKind('unknown', '区域', MaskKindId.unknown, LocalGlyph.raster, LocalPlace.external, visible: false);
}

/// The fold op of a new component: the first one adds; "减一块" subtracts; a range kind added to a shape limits it.
MaskOp opForNew(LocalKind k, {required bool first, required bool subtract}) {
  if (first) return MaskOp.add;
  if (subtract) return MaskOp.subtract;
  return k.limits ? MaskOp.intersect : MaskOp.add;
}

/// Why a component of [k] cannot be added to the area at [index] of [list] (the toast), or null.
String? limitFor(LocalList list, LocalKind k, {int? index}) {
  if (index == null && list.length >= kMaxLocalAdjustments) return '最多 $kMaxLocalAdjustments 个区域';
  if (index != null && list.items[index].mask.length >= kMaxMaskComponents) return '每个区域最多 $kMaxMaskComponents 块';
  if (k.engine == MaskKindId.brush && list.countKind(MaskKindId.brush) >= kMaxBrushComponents) return '画笔最多 $kMaxBrushComponents 个';
  if (k.engine == MaskKindId.raster && list.countKind(MaskKindId.raster) >= kMaxRasterComponents) return '这类区域最多 $kMaxRasterComponents 个';
  return null;
}
