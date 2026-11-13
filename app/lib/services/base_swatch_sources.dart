// The engine behind the base swatches: [SessionSwatchSource] renders on the photo's session, [AssetSwatchSource] on a bundled
// picture (the 默认成像 page before the first photo has been developed).
import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:flutter/services.dart' show rootBundle;
import 'package:path_provider/path_provider.dart';

import '../models.dart';
import '../native/dapple_session.dart';
import '../state/app_state.dart';
import 'base_swatches.dart';
import 'session_cache.dart';

/// Bases per native call of the batch route (small chunks first, see BaseSwatches).
const int kSwatchBatch = 6;

/// Params of a swatch: the photo's own automatic correction (what the photo looks like when developed) and the base. No user
/// edit, no look, no tone strength: a swatch compares bases, so it is edit independent.
Map<String, dynamic> swatchParams(Map<String, dynamic>? correction, String? baseId, {bool auto = true}) {
  final out = <String, dynamic>{};
  if (auto && correction != null) {
    correction.forEach((k, v) {
      if (k != 'exposure' && !k.startsWith('film')) out[k] = v; // `exposure` is EV in the legacy schema, see AppState.wireParams
    });
  }
  if (baseId != null) out['film'] = baseId;
  return out;
}

/// Renders [ids] on [s]: one native batch call (`dapple_session_base_previews`: interactive tier, coarse film tables, background priority,
/// shared decode and scaling) when the engine has it, else one final-quality `render` per base. Null entries: failed or cancelled.
Future<List<ui.Image?>> renderSwatchBatch(DappleSession s, Map<String, dynamic>? correction, bool auto, List<String?> ids, int longEdge) async {
  if (DappleSession.hasBaseSwatches) {
    final rs = await s.baseSwatches(swatchParams(correction, null, auto: auto), ids, longEdge: longEdge);
    return [for (final r in rs) r == null ? null : await imageFromRgba(r)];
  }
  final out = <ui.Image?>[];
  for (final id in ids) {
    try {
      out.add(await imageFromRgba(await s.render(swatchParams(correction, id, auto: auto), longEdge: longEdge)));
    } catch (_) {
      out.add(null);
    }
  }
  return out;
}

// One swatch call at a time over the whole app: the engine's batch call stops when a newer one starts (base_previews' "newest wins"),
// so the base swatches and the look swatches of the same session must not overlap.
Future<void> _engineTail = Future.value();
Future<T> _engineSerial<T>(Future<T> Function() f) {
  final r = _engineTail.then((_) => f());
  _engineTail = r.then((_) {}, onError: (_) {});
  return r;
}

/// The nine looks ([lookIds]) on top of [baseId] (a film stock, a modern rendering, null = standard) of this photo: params carry
/// `look` (strength 100) and the base; the engine's `dapple_session_base_previews` keeps every other key and replaces only `film`, so a
/// look is rendered by passing it in the params and the current base as the one id (interactive tier, background priority, like the
/// base swatches). One native call per look (an id list varies the base, not the look). An engine without the call: final-quality renders.
Future<List<ui.Image?>> renderLookBatch(DappleSession s, Map<String, dynamic>? correction, bool auto, String? baseId, List<String?> lookIds, int longEdge) {
  return _engineSerial(() async {
    final out = <ui.Image?>[];
    for (final id in lookIds) {
      try {
        final params = swatchParams(correction, null, auto: auto)..['look'] = id ?? 'std'..['lookStrength'] = 100;
        if (DappleSession.hasBaseSwatches) {
          final r = (await s.baseSwatches(params, [baseId], longEdge: longEdge)).first;
          out.add(r == null ? null : await imageFromRgba(r));
        } else {
          if (baseId != null) params['film'] = baseId;
          out.add(await imageFromRgba(await s.render(params, longEdge: longEdge)));
        }
      } catch (_) {
        out.add(null);
      }
    }
    return out;
  });
}

/// Swatches of the colour looks of [photo] on its current base [baseId]; the ids of [BaseSwatches] are look ids.
class LookSwatchSource implements BatchSwatchSource {
  LookSwatchSource(this.app, this.photo, this.baseId);
  final AppState app;
  final Photo photo;
  final String? baseId;
  DappleSession? _live;

  @override
  String get key => 'looks|${photo.id}|${photo.path}|${photo.correction?.hashCode ?? 0}|${app.settings.autoProcess}|${baseId ?? ''}';

  @override
  int get batchSize => 1;

  @override
  Future<ui.Image?> render(String? lookId, int longEdge) async => (await renderBatch([lookId], longEdge)).first;

  @override
  Future<List<ui.Image?>> renderBatch(List<String?> ids, int longEdge) async {
    final s = await app.sessions.get(photo, background: true);
    if (s == null) return List.filled(ids.length, null);
    _live = s;
    return renderLookBatch(s, photo.correction, app.settings.autoProcess, baseId, ids, longEdge);
  }

  @override
  void cancel() => _live?.cancelBaseSwatches();
}

class SessionSwatchSource implements BatchSwatchSource {
  SessionSwatchSource(this.app, this.photo);
  final AppState app;
  final Photo photo;
  DappleSession? _live;

  @override
  String get key => '${photo.id}|${photo.path}|${photo.correction?.hashCode ?? 0}|${app.settings.autoProcess}';

  @override
  int get batchSize => DappleSession.hasBaseSwatches ? kSwatchBatch : 1;

  @override
  Future<ui.Image?> render(String? baseId, int longEdge) async => (await renderBatch([baseId], longEdge)).first;

  @override
  Future<List<ui.Image?>> renderBatch(List<String?> ids, int longEdge) async {
    final s = await app.sessions.get(photo, background: true);
    if (s == null) return List.filled(ids.length, null);
    _live = s;
    return _engineSerial(() => renderSwatchBatch(s, photo.correction, app.settings.autoProcess, ids, longEdge));
  }

  @override
  void cancel() => _live?.cancelBaseSwatches();
}

/// A bundled picture through its own small session (opened on first use, closed with [close]).
class AssetSwatchSource implements BatchSwatchSource {
  AssetSwatchSource(this.asset);
  final String asset;
  Future<DappleSession?>? _session;

  @override
  String get key => 'asset:$asset';

  Future<DappleSession?> _open() => _session ??= () async {
        if (!DappleSession.isAvailable) return null;
        final dir = await getTemporaryDirectory();
        final f = File('${dir.path}/${asset.split('/').last}');
        if (!f.existsSync()) await f.writeAsBytes((await rootBundle.load(asset)).buffer.asUint8List(), flush: true);
        return DappleSession.open(f.path, longEdge: 640, background: true);
      }();

  @override
  int get batchSize => DappleSession.hasBaseSwatches ? kSwatchBatch : 1;

  @override
  Future<ui.Image?> render(String? baseId, int longEdge) async => (await renderBatch([baseId], longEdge)).first;

  @override
  Future<List<ui.Image?>> renderBatch(List<String?> ids, int longEdge) async {
    final s = await _open();
    if (s == null) return List.filled(ids.length, null);
    return _engineSerial(() => renderSwatchBatch(s, null, true, ids, longEdge));
  }

  @override
  void cancel() {
    final f = _session;
    if (f != null) unawaited(f.then((s) => s?.cancelBaseSwatches(), onError: (_) {}));
  }

  Future<void> close() async {
    final f = _session;
    _session = null;
    final s = f == null ? null : await f.catchError((_) => null);
    await s?.close();
  }
}
