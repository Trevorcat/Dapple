// Swatches of the rendering bases on one photo (film map, 默认成像 page): a small thumbnail of the photo rendered by the engine
// with each base (every film stock, the three modern renderings, and none = the standard rendering).
//
// Route: one engine session per photo (the editor's own, from SessionCache), params = the photo's automatic correction + the base.
// The swatch is therefore edit independent: it shows the base at its defaults on this photo, not the user's other edits (so it
// never has to be redone while the user edits, and the cache is keyed by the photo alone). A source that can ([BatchSwatchSource]:
// the engine's `dapple_session_base_previews`) renders a few bases per call, small chunks first so that the first swatches show
// early, nearest to the selection on the film map first; any other source renders one base per call.
// Rendering is at low priority: it waits while any pointer is down (a drag owns the engine; a chunk in flight is cancelled when a
// finger goes down and its unfinished bases are asked for again), leaves a gap between calls, and stops for good when the service
// is disposed (the editor closed). Finished swatches live in a small LRU over photos, so reopening the same photo costs nothing.
import 'dart:async';
import 'dart:collection';
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';

/// Where swatch pixels come from (the engine in the app, a fake in tests).
abstract class SwatchSource {
  /// Identity of the picture the swatches show; the cache key.
  String get key;

  /// [baseId] rendered on the picture at [longEdge] px, or null when it cannot be (no engine, no file, a failure).
  Future<ui.Image?> render(String? baseId, int longEdge);
}

/// A source that renders several bases in one call (one isolate, one native call, shared decode / source scaling).
abstract class BatchSwatchSource implements SwatchSource {
  /// Most bases one [renderBatch] call takes (1 = the per-base route).
  int get batchSize;

  /// [ids] rendered in order; an entry is null when it failed or the call was interrupted by [cancel].
  Future<List<ui.Image?>> renderBatch(List<String?> ids, int longEdge);

  /// Stops the call in flight at its next picture (a finger went down, the service was closed).
  void cancel();
}

class BaseSwatches extends ChangeNotifier {
  BaseSwatches({
    required this.source,
    required List<String?> ids,
    this.longEdge = 96,
    this.gap = const Duration(milliseconds: 30),
    this.settle = const Duration(milliseconds: 250),
    bool watchPointers = true,
  })  : _queue = [...ids],
        _focus = ids.isEmpty ? null : ids.first,
        _watch = watchPointers {
    // from construction on: a finger that goes down before [start] is known to be down when the first render would begin
    if (_watch) GestureBinding.instance.pointerRouter.addGlobalRoute(_route);
  }

  /// Photos whose swatches stay cached (96 px swatches: some 25 KB each, about 0.7 MB a photo).
  static int maxPhotos = 3;

  /// Where the bases sit on the film map (set when the map's positions are loaded): chunks start with the bases nearest to the focus.
  static Map<String, ui.Offset>? mapPositions;
  static final LinkedHashMap<String, Map<String, ui.Image>> _cache = LinkedHashMap();

  /// Test hook: drop the cache.
  @visibleForTesting
  static void clearCache() {
    _cache.clear();
  }

  final SwatchSource source;
  final int longEdge;

  /// Pause between two renders (the UI isolate and the engine keep breathing room).
  final Duration gap;

  /// After the last pointer went up, wait this long before rendering again.
  final Duration settle;
  final List<String?> _queue;
  String? _focus; // the selection: what the chunks are ordered around
  bool _inFlight = false, _interrupted = false;
  int _chunks = 0;
  final bool _watch;
  final Set<int> _down = {};
  final Set<String> _failed = {};
  bool _started = false, _closed = false;
  bool _quiet = true; // no pointer went up within [settle]
  Timer? _quietTimer;
  Timer? _timer;
  Completer<void>? _sleeping;

  static String _k(String? id) => id ?? '';

  Map<String, ui.Image> get _mine {
    final m = _cache.remove(source.key) ?? <String, ui.Image>{};
    _cache[source.key] = m; // most recently used last
    while (_cache.length > maxPhotos) {
      _cache.remove(_cache.keys.first); // not disposed: a page that still paints it would throw; the GC frees the pixels
    }
    return m;
  }

  /// The finished swatch of [id] (null = standard), or null while it is not rendered.
  ui.Image? imageOf(String? id) => _closed ? null : _cache[source.key]?[_k(id)];

  bool get closed => _closed;

  /// Everything asked for is rendered (or failed).
  bool get done => _queue.every((id) => imageOf(id) != null || _failed.contains(_k(id)));

  /// Renders next: [id] moves to the front of the queue and becomes the focus the following ones are ordered around.
  void prioritise(String? id) {
    _focus = id;
    if (_queue.remove(id)) _queue.insert(0, id);
  }

  void _route(PointerEvent e) {
    if (e is PointerDownEvent) {
      _down.add(e.pointer);
      final s = source;
      if (_inFlight && s is BatchSwatchSource) {
        _interrupted = true; // the chunk stops at its next picture; what it did not do is asked for again
        s.cancel();
      }
    } else if (e is PointerUpEvent || e is PointerCancelEvent) {
      if (_down.remove(e.pointer)) {
        _quiet = false;
        _quietTimer?.cancel();
        _quietTimer = Timer(settle, () => _quiet = true);
      }
    }
  }

  /// Begins rendering in the background (idempotent).
  void start() {
    if (_started || _closed) return;
    _started = true;
    unawaited(_run());
  }

  Future<void> _sleep(Duration d) {
    final c = _sleeping = Completer<void>();
    _timer = Timer(d, () {
      if (!c.isCompleted) c.complete();
    });
    return c.future;
  }

  Future<void> _idle() async {
    while (!_closed) {
      if (_down.isEmpty && _quiet) return;
      await _sleep(const Duration(milliseconds: 60));
    }
  }

  /// Up to [max] bases still to render: the focus first, then by distance on the film map from it (queue order where a position is unknown).
  List<String> _nextBatch(int max) {
    final have = _cache[source.key];
    final todo = [for (final id in _queue) if (have?[_k(id)] == null && !_failed.contains(_k(id))) _k(id)];
    final pos = mapPositions, f = _focus;
    final fp = pos == null || f == null ? null : pos[f];
    if (fp != null && todo.length > 1) {
      double d(String id) => id == f ? -1 : (pos![id] == null ? double.infinity : (pos[id]! - fp).distance);
      final order = {for (var i = 0; i < todo.length; i++) todo[i]: i};
      todo.sort((a, b) {
        final c = d(a).compareTo(d(b));
        return c != 0 ? c : order[a]!.compareTo(order[b]!);
      });
    }
    return todo.take(max).toList();
  }

  Future<void> _run() async {
    var changed = false;
    while (!_closed) {
      final src = source;
      // small chunks first (2, 4, then the source's own size) so that the first swatches appear early
      final cap = src is BatchSwatchSource ? math.min(src.batchSize, 2 << _chunks) : 1;
      final ks = _nextBatch(math.max(1, cap));
      if (ks.isEmpty) break;
      await _idle();
      if (_closed) break;
      List<ui.Image?> imgs = List.filled(ks.length, null);
      _inFlight = true;
      _interrupted = false;
      try {
        final ids = [for (final k in ks) k.isEmpty ? null : k];
        imgs = src is BatchSwatchSource ? await src.renderBatch(ids, longEdge) : [await src.render(ids.first, longEdge)];
      } catch (e) {
        debugPrint('swatch ${ks.join(',')} failed: $e');
      }
      _inFlight = false;
      if (_closed) {
        for (final i in imgs) {
          i?.dispose();
        }
        break;
      }
      _chunks++;
      for (var i = 0; i < ks.length; i++) {
        final img = i < imgs.length ? imgs[i] : null;
        if (img == null) {
          if (!_interrupted) _failed.add(ks[i]); // an interrupted chunk's missing pictures are asked for again
        } else {
          _mine[ks[i]] = img;
          changed = true;
        }
      }
      notifyListeners();
      await _sleep(gap);
    }
    if (changed && !_closed) notifyListeners();
  }

  /// Stops: the render in flight is dropped when it lands, nothing new starts. Finished swatches stay in the LRU.
  @override
  void dispose() {
    if (_closed) return;
    _closed = true;
    final src = source;
    if (_inFlight && src is BatchSwatchSource) src.cancel();
    _timer?.cancel();
    _quietTimer?.cancel();
    final s = _sleeping;
    if (s != null && !s.isCompleted) s.complete();
    if (_watch) GestureBinding.instance.pointerRouter.removeGlobalRoute(_route);
    super.dispose();
  }
}
