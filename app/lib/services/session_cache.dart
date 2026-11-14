import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;
import 'package:flutter/foundation.dart' show debugPrint, visibleForTesting;

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

  // zoom detail: one higher resolution session, for the photo being edited only
  String? _detailId;
  Future<DappleSession?>? _detail;

  /// Long edge of the detail session. The RAW is decoded once at its native half size (fast path, no
  /// AHD demosaic) and capped here to bound memory (4096 x 2731 float RGB = 134 MB).
  static const detailLongEdge = 4096;

  /// Tests: pretend the engine is there and open sessions through [testOpener] instead of the FFI.
  @visibleForTesting
  bool? testAvailable;
  @visibleForTesting
  Future<DappleSession?> Function(Photo ph)? testOpener;

  bool get available => testAvailable ?? (!_app.isDemo && DappleSession.isAvailable);

  /// True when a session for [id] is open or opening.
  bool has(String id) => _open.containsKey(id);

  /// Session for [ph] at screen resolution (2048 px). Null when the engine is unavailable or the
  /// photo has no readable source (demo photos).
  Future<DappleSession?> get(Photo ph, {bool background = false, bool speculative = false}) {
    if (!available || ph.demo != null) return Future.value(null);
    if (!background && !speculative) {
      _warm.remove(ph.id); // claimed by a real open: no longer speculative
      current = ph.id;
    }
    _lru.remove(ph.id);
    _lru.add(ph.id);
    final f = _open.putIfAbsent(ph.id, () => _openNew(ph, background));
    while (_lru.length > _max) {
      final old = _lru.removeAt(0);
      unawaited(_close(old));
    }
    return f;
  }

  // ---- speculative open on pointer-down ----
  final Set<String> _warm = {};

  /// Starts opening [ph] at full priority because a finger just went down on its thumbnail. Pair with
  /// [cancelWarm] when the tap is abandoned (scroll, drag away, long press). No-op when already open.
  void warm(Photo ph) {
    if (!available || ph.demo != null || !ph.isRaw || _open.containsKey(ph.id)) return;
    _warm.add(ph.id);
    _openDone.remove(ph.id);
    unawaited(get(ph, speculative: true).then((_) {
      _openDone.add(ph.id);
      if (_warm.contains(ph.id) && _abandoned.remove(ph.id)) {
        _warm.remove(ph.id);
        unawaited(_close(ph.id)); // nobody claimed it: free the memory
      }
    }));
  }

  final Set<String> _abandoned = {};
  Timer? _downTimer;

  /// Finger down on a thumbnail. The open starts after [downDelay] if the finger is still there, so a
  /// scroll that merely touches a tile does not start a 40 MP decode.
  static const downDelay = Duration(milliseconds: 40);
  void pointerDown(Photo ph) {
    _downTimer?.cancel();
    _downTimer = null;
    if (!available || ph.demo != null || !ph.isRaw || _open.containsKey(ph.id)) return;
    _downTimer = Timer(downDelay, () => warm(ph));
  }

  /// Finger up or cancelled. A tap claims the session within a frame or two; anything else (scroll,
  /// slid off, long press) leaves it unclaimed and the warm session is dropped after a short grace.
  void pointerEnd(Photo ph) {
    _downTimer?.cancel();
    _downTimer = null;
    if (_warm.isEmpty) return;
    Timer(const Duration(milliseconds: 250), () => cancelWarm(ph.id));
  }

  /// The tap that [warm]ed [id] was abandoned: the session is closed as soon as it is open, unless the
  /// editor has claimed it in the meantime. (A native decode cannot be interrupted midway.)
  void cancelWarm(String id) {
    if (!_warm.contains(id)) return;
    _abandoned.add(id);
    if (_openDone.contains(id)) {
      _abandoned.remove(id);
      _warm.remove(id);
      unawaited(_close(id));
    }
  }

  final Set<String> _openDone = {};

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
      await get(ph, background: true); // neighbours decode on the efficiency cores, never against the open photo
    }
  }

  Future<DappleSession?> _openNew(Photo ph, bool background) async {
    try {
      if (testOpener != null) return await testOpener!(ph);
      final src = await _resolve(ph);
      if (src == null) throw '没有源文件';
      return await DappleSession.open(src.path, full: false, longEdge: 2048, background: background);
    } catch (_) {
      _open.remove(ph.id);
      return null;
    }
  }

  /// Higher resolution session of [ph] (up to [detailLongEdge] px) used for pinch-zoom detail. Opened
  /// lazily on first request (decode takes about a second) and cached for this photo; asking for another
  /// photo closes the previous one. Null when unavailable.
  Future<DappleSession?> detail(Photo ph) {
    if (!available || ph.demo != null) return Future.value(null);
    if (_detailId == ph.id && _detail != null) return _detail!;
    closeDetail();
    _detailId = ph.id;
    return _detail = _openDetail(ph);
  }

  Future<DappleSession?> _openDetail(Photo ph) async {
    try {
      final src = await _resolve(ph);
      if (src == null) throw '没有源文件';
      return await DappleSession.open(src.path, full: false, longEdge: detailLongEdge, background: true);
    } catch (_) {
      if (_detailId == ph.id) {
        _detail = null;
        _detailId = null;
      }
      return null;
    }
  }

  // ---- true-resolution region detail (RAW files with a mosaic path) ----
  // The visible rectangle at the picture's native resolution, demosaiced from the cached mosaic (GPU where enabled): one region at a time, a newer request
  // cancels the one in flight and replaces the session, the memory is that of one rectangle (<= regionMaxPx on the long edge, 3200 x 2400 x 12 B = 92 MB).
  static const regionMaxPx = 3200;
  static const regionMaxEdge = 4800; // a portrait phone zoomed on a landscape photo sees a tall, narrow rectangle (1600 x 3400 at 4x): the cap is on the area, 92 MB
  final Set<String> _noRegion = {}; // photos the native side refused (JPEG, sRAW, lens-warped DNG ...): half-resolution detail only
  String? _regionId;
  Future<({DappleSession session, ui.Rect rect})?>? _region;
  final Map<String, ({int w, int h})> _fullSize = {};

  /// Full-resolution size of the oriented picture (from the session info), cached.
  Future<({int w, int h})?> fullSize(Photo ph) async {
    final c = _fullSize[ph.id];
    if (c != null) return c;
    final s = await get(ph);
    if (s == null) return null;
    final i = await s.info();
    final w = (i['width'] as num?)?.toInt() ?? 0, h = (i['height'] as num?)?.toInt() ?? 0;
    if (w <= 0 || h <= 0) return null;
    return _fullSize[ph.id] = (w: w, h: h);
  }

  /// A session whose source is [frac] (normalised rect of the picture) at 1:1, plus the normalised rect it really covers. Null when the region is larger
  /// than [regionMaxPx] (use [detail]), the file has no region path, or the open failed / was cancelled by a newer request.
  Future<({DappleSession session, ui.Rect rect})?> regionDetail(Photo ph, ui.Rect frac) async {
    if (!available || ph.demo != null || !DappleSession.hasRegions || _noRegion.contains(ph.id)) {
      debugPrint('dapple region: not offered (available $available, symbol ${DappleSession.hasRegions}, refused earlier ${_noRegion.contains(ph.id)})');
      return null;
    }
    final fs = await fullSize(ph);
    if (fs == null) return null;
    final x0 = (frac.left * fs.w).floor().clamp(0, fs.w - 1), y0 = (frac.top * fs.h).floor().clamp(0, fs.h - 1);
    final x1 = (frac.right * fs.w).ceil().clamp(x0 + 1, fs.w), y1 = (frac.bottom * fs.h).ceil().clamp(y0 + 1, fs.h);
    if (x1 - x0 > regionMaxEdge || y1 - y0 > regionMaxEdge || (x1 - x0) * (y1 - y0) > regionMaxPx * 2400) {
      debugPrint('dapple region: ${x1 - x0} x ${y1 - y0} px is larger than the region cap ($regionMaxPx x 2400 px, edge $regionMaxEdge), half-resolution detail instead');
      return null;
    }
    final src = await _resolve(ph);
    if (src == null) return null;
    DappleSession.cancelRegion(); // a pan / zoom moved on: stop the one in flight
    final old = _region;
    _regionId = ph.id;
    final rect = ui.Rect.fromLTRB(x0 / fs.w, y0 / fs.h, x1 / fs.w, y1 / fs.h);
    final f = _openRegion(ph, src.path, x0, y0, x1, y1, rect);
    _region = f;
    if (old != null) unawaited(old.then((r) => r?.session.close()));
    return f;
  }

  static bool _primedOnce = false; // the GPU demosaic and its speed verdict are per process, the mosaic and anchor are cached by the preview open itself

  /// Idle warm-up of the true-resolution path, once per process: a 256 x 256 region at the centre brings up the GPU demosaic and, on a new device, runs its
  /// one-off speed verdict (about 3 s), so the first pinch-zoom does not. The mosaic and the exposure anchor are already cached by the preview open itself.
  /// Background priority; skipped once a real region was asked for; the engine's tile loops wait while a finger is on a control (dapple_set_interaction).
  Future<void> primeRegion(Photo ph) async {
    if (!available || ph.demo != null || !ph.isRaw || !DappleSession.hasRegions || _noRegion.contains(ph.id) || _region != null) return;
    if (_primedOnce) return;
    _primedOnce = true;
    final t = Stopwatch()..start();
    try {
      final fs = await fullSize(ph);
      final src = await _resolve(ph);
      if (fs == null || src == null || _region != null) return;
      final x0 = fs.w ~/ 2 - 128, y0 = fs.h ~/ 2 - 128;
      final s = await DappleSession.openRegion(src.path, x0, y0, x0 + 256, y0 + 256);
      await s.close();
      debugPrint('dapple region: warm-up done in ${t.elapsedMilliseconds} ms');
    } catch (e) {
      _primedOnce = false; // cancelled by a real request or failed: nothing to keep
    }
  }

  Future<({DappleSession session, ui.Rect rect})?> _openRegion(Photo ph, String path, int x0, int y0, int x1, int y1, ui.Rect rect) async {
    try {
      final s = await DappleSession.openRegion(path, x0, y0, x1, y1);
      return (session: s, rect: rect);
    } catch (e) {
      final m = '$e';
      debugPrint('dapple region: open failed: $m');
      if (!m.contains('cancelled')) _noRegion.add(ph.id); // refused for this file: do not ask again
      return null;
    }
  }

  void closeRegion() {
    final f = _region;
    _region = null;
    _regionId = null;
    DappleSession.cancelRegion();
    if (f != null) unawaited(f.then((r) => r?.session.close()));
  }

  void closeDetail() {
    closeRegion();
    final f = _detail;
    _detail = null;
    _detailId = null;
    if (f != null) unawaited(f.then((s) => s?.close()));
  }

  Future<void> _close(String id) async {
    if (id == _detailId || id == _regionId) closeDetail();
    final f = _open.remove(id);
    _openDone.remove(id);
    _abandoned.remove(id);
    _warm.remove(id);
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

  /// The photo of the last foreground [get] (the one the editor shows), null when none.
  String? current;

  /// Memory (pipeline-architecture.md 42.9): closes every session but [keep]'s (neighbours, warm opens) and the 2x / 1:1 detail sessions.
  /// The cached source paths of the closed photos stay (cheap); their temporary copies are deleted.
  Future<void> closeAllExcept(String? keep) async {
    _downTimer?.cancel();
    _downTimer = null;
    closeDetail();
    final ids = _open.keys.where((id) => id != keep).toList();
    _lru.removeWhere(ids.contains);
    await Future.wait(ids.map(_close));
  }

  /// Every session closed (the editor has gone): the native pictures go, the next open decodes again (from the engine's mosaic cache when it
  /// still holds the file).
  Future<void> closeAll() async {
    await closeAllExcept(null);
    current = null;
  }

  void dispose() {
    _downTimer?.cancel();
    closeDetail();
    for (final id in _open.keys.toList()) {
      unawaited(_close(id));
    }
  }
}

/// Decodes RGBA8888 bytes into a [ui.Image]. (Frames from the engine use [NativeFrame.toImage].)
Future<ui.Image> imageFromRgba(RenderedImage r) {
  final c = Completer<ui.Image>();
  ui.decodeImageFromPixels(r.rgba, r.width, r.height, ui.PixelFormat.rgba8888, c.complete);
  return c.future;
}
