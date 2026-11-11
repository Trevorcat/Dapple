import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;

import '../models.dart';
import '../native/dapple_session.dart';
import '../state/app_state.dart';

typedef _Src = ({String path, bool temp});

/// Keeps the decoded native session per photo (decode once, render many times) with a small LRU,
/// and prefetches the neighbours of the photo being edited so that next / previous is instant.
class SessionCache {
  SessionCache(this._app);
  final AppState _app;

  /// Current photo + next + previous + one spare.
  static const _max = 4;

  final Map<String, Future<DappleSession?>> _open = {};
  final Map<String, Future<_Src?>> _src = {};
  final Map<String, String> _temp = {};
  final List<String> _lru = [];

  bool get available => !_app.isDemo && DappleSession.isAvailable;

  /// True when a session for [id] is open or opening.
  bool has(String id) => _open.containsKey(id);

  /// Session for [ph] at screen resolution (2048 px). Null when the engine is unavailable or the
  /// photo has no readable source (demo photos).
  Future<DappleSession?> get(Photo ph) {
    if (!available || ph.demo != null) return Future.value(null);
    _lru.remove(ph.id);
    _lru.add(ph.id);
    final f = _open.putIfAbsent(ph.id, () => _openNew(ph));
    while (_lru.length > _max) {
      final old = _lru.removeAt(0);
      unawaited(_close(old));
    }
    return f;
  }

  /// Local readable path of the photo's source (copied out of MediaStore at most once).
  Future<_Src?> _resolve(Photo ph) => _src.putIfAbsent(ph.id, () async {
        try {
          final s = await _app.resolveSource(ph);
          if (s.temp) _temp[ph.id] = s.path;
          return s;
        } catch (_) {
          _src.remove(ph.id);
          return null;
        }
      });

  /// The camera's embedded JPEG of [ph] as an instant placeholder while the RAW decodes.
  Future<RenderedImage?> preview(Photo ph, {int longEdge = 1280}) async {
    if (!available || ph.demo != null || !ph.isRaw) return null;
    try {
      final s = await _resolve(ph);
      if (s == null) return null;
      return await DappleSession.extractPreview(s.path, longEdge: longEdge);
    } catch (_) {
      return null;
    }
  }

  /// Decodes [photos] one after the other in the background (next, then previous). Stops as soon
  /// as [stillWanted] turns false (the user navigated away), so a stale prefetch never holds the
  /// CPU against the photo that is now open.
  Future<void> prefetch(List<Photo> photos, bool Function() stillWanted) async {
    for (final ph in photos) {
      if (!stillWanted()) return;
      if (_open.containsKey(ph.id)) continue;
      await get(ph);
    }
  }

  Future<DappleSession?> _openNew(Photo ph) async {
    try {
      final src = await _resolve(ph);
      if (src == null) throw '没有源文件';
      return await DappleSession.open(src.path, full: false, longEdge: 2048);
    } catch (_) {
      _open.remove(ph.id);
      return null;
    }
  }

  Future<void> _close(String id) async {
    final f = _open.remove(id);
    _src.remove(id);
    final s = await f;
    await s?.close();
    final t = _temp.remove(id);
    if (t != null) {
      try {
        await File(t).delete();
      } catch (_) {}
    }
  }

  void dispose() {
    for (final id in _open.keys.toList()) {
      unawaited(_close(id));
    }
  }
}

/// Decodes RGBA8888 bytes into a [ui.Image].
Future<ui.Image> imageFromRgba(RenderedImage r) {
  final c = Completer<ui.Image>();
  ui.decodeImageFromPixels(r.rgba, r.width, r.height, ui.PixelFormat.rgba8888, c.complete);
  return c.future;
}
