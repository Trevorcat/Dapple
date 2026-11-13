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

class SessionSwatchSource implements SwatchSource {
  SessionSwatchSource(this.app, this.photo);
  final AppState app;
  final Photo photo;

  @override
  String get key => '${photo.id}|${photo.path}|${photo.correction?.hashCode ?? 0}|${app.settings.autoProcess}';

  @override
  Future<ui.Image?> render(String? baseId, int longEdge) async {
    final s = await app.sessions.get(photo, background: true);
    if (s == null) return null;
    final r = await s.render(swatchParams(photo.correction, baseId, auto: app.settings.autoProcess), longEdge: longEdge);
    return imageFromRgba(r);
  }
}

/// A bundled picture through its own small session (opened on first use, closed with [close]).
class AssetSwatchSource implements SwatchSource {
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
  Future<ui.Image?> render(String? baseId, int longEdge) async {
    final s = await _open();
    if (s == null) return null;
    return imageFromRgba(await s.render(swatchParams(null, baseId), longEdge: longEdge));
  }

  Future<void> close() async {
    final f = _session;
    _session = null;
    final s = f == null ? null : await f.catchError((_) => null);
    await s?.close();
  }
}
