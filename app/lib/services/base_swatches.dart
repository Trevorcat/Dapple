// Swatches of the rendering bases on one photo (film map, 默认成像 page): a small thumbnail of the photo rendered by the engine
// with each base (every film stock, the three modern renderings, and none = the standard rendering).
//
// Route: one engine session per photo (the editor's own, from SessionCache), one `render(params, longEdge: 96)` call per base
// with params = the photo's automatic correction + {film: id}. The swatch is therefore edit independent: it shows the base at its
// defaults on this photo, not the user's other edits (so it never has to be redone while the user edits, and the cache is
// keyed by the photo alone). The film tables of all stocks are prebaked at first launch, so a 96 px render is cheap.
// Rendering is one at a time, at low priority: it waits while any pointer is down (a drag owns the engine), leaves a gap
// between renders, and stops for good when the service is disposed (the editor closed). Finished swatches live in a small
// LRU over photos, so reopening the same photo costs nothing.
import 'dart:async';
import 'dart:collection';
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

class BaseSwatches extends ChangeNotifier {
  BaseSwatches({
    required this.source,
    required List<String?> ids,
    this.longEdge = 96,
    this.gap = const Duration(milliseconds: 30),
    this.settle = const Duration(milliseconds: 250),
    bool watchPointers = true,
  })  : _queue = [...ids],
        _watch = watchPointers {
    // from construction on: a finger that goes down before [start] is known to be down when the first render would begin
    if (_watch) GestureBinding.instance.pointerRouter.addGlobalRoute(_route);
  }

  /// Photos whose swatches stay cached (96 px swatches: some 25 KB each, about 0.7 MB a photo).
  static int maxPhotos = 3;
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

  /// Renders next: [id] moves to the front of the queue.
  void prioritise(String? id) {
    if (_queue.remove(id)) _queue.insert(0, id);
  }

  void _route(PointerEvent e) {
    if (e is PointerDownEvent) {
      _down.add(e.pointer);
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

  String? _next() {
    final have = _cache[source.key];
    for (final id in _queue) {
      if (have?[_k(id)] == null && !_failed.contains(_k(id))) return _k(id);
    }
    return null;
  }

  Future<void> _run() async {
    var changed = false;
    while (!_closed) {
      final k = _next();
      if (k == null) break;
      await _idle();
      if (_closed) break;
      ui.Image? img;
      try {
        img = await source.render(k.isEmpty ? null : k, longEdge);
      } catch (e) {
        debugPrint('swatch $k failed: $e');
      }
      if (_closed) {
        img?.dispose();
        break;
      }
      if (img == null) {
        _failed.add(k);
      } else {
        _mine[k] = img;
        changed = true;
        notifyListeners();
      }
      await _sleep(gap);
    }
    if (changed && !_closed) notifyListeners();
  }

  /// Stops: the render in flight is dropped when it lands, nothing new starts. Finished swatches stay in the LRU.
  @override
  void dispose() {
    if (_closed) return;
    _closed = true;
    _timer?.cancel();
    _quietTimer?.cancel();
    final s = _sleeping;
    if (s != null && !s.isCompleted) s.complete();
    if (_watch) GestureBinding.instance.pointerRouter.removeGlobalRoute(_route);
    super.dispose();
  }
}
