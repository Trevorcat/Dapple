import 'dart:async';
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';

import '../model/catalog.dart';
import '../model/params.dart';
import '../models.dart';
import '../state/app_state.dart';
import 'session_cache.dart';

/// Tonal analysis of a rendered photo: 64-bin histogram + percentiles + nearest class.
class Analysis {
  Analysis(this.hist, this.p5, this.p50, this.p95, this.nearest);
  final List<double> hist;
  final double p5, p50, p95;

  /// "mid-long" style id.
  final String nearest;

  factory Analysis.fromJson(Map<String, dynamic> j) {
    final h = ((j['histogram'] as List?) ?? const []).map((e) => (e as num).toDouble()).toList();
    final key = (j['key'] as String?) ?? 'mid';
    final range = (j['range'] as String?) ?? 'mid';
    return Analysis(h.isEmpty ? List<double>.filled(64, 1 / 64) : h, (j['p5'] as num?)?.toDouble() ?? .1,
        (j['p50'] as num?)?.toDouble() ?? .5, (j['p95'] as num?)?.toDouble() ?? .9,
        (j['nearest'] as String?) ?? '$key-$range');
  }

  /// Demo / fallback analysis built from the prototype's gaussian mixtures.
  factory Analysis.demo(DemoPhotoSpec s, {double brightness = 1, double contrast = 1}) {
    final h = demoHistogram(s.hist, brightness: brightness, contrast: contrast);
    double q(double f) {
      var a = 0.0;
      for (var i = 0; i < h.length; i++) {
        a += h[i];
        if (a >= f) return (i + .5) / h.length;
      }
      return 1;
    }

    return Analysis(h, q(.05), q(.5), q(.95), toneClassId(s.key, s.range));
  }
}

/// Approximate demo rendering parameters (CSS-filter style) for an edit state; used when the
/// native engine is not available (tests / screenshots).
class DemoLook {
  const DemoLook(this.b, this.c, this.s, this.warm, this.gray, this.sepia);
  final double b, c, s, warm, gray, sepia;
}

DemoLook demoLookFor(EditState e, {required double defaultToneStrength, required String defaultLook, String? key, String? range, bool original = false}) {
  if (original) return const DemoLook(0.94, 0.86, 0.78, 0, 0, 0);
  final l = lookById(e.look ?? defaultLook);
  final ls = e.lookStrength / 100;
  var b = 1 + e.value('exposure') / 100 * 0.6 + e.value('shadows') / 100 * 0.08 - e.value('blacks') / 100 * 0.05;
  var c = 1 + e.value('contrast') / 100 * 0.4 + e.value('clarity') / 100 * 0.1;
  var s = 1 + e.value('saturation') / 100 * 0.8 + e.value('vibrance') / 100 * 0.4;
  var warm = e.value('temp') / 100;
  b *= 1 + (l.bright - 1) * ls;
  c *= 1 + (l.contrast - 1) * ls;
  s *= 1 + (l.sat - 1) * ls;
  if (e.palette != null) {
    s *= 1 + (e.palette!.dx - 0.5) * 0.9 * ls;
    warm += -(e.palette!.dy - 0.5) * ls;
  }
  if (key != null && range != null) {
    const kb = {'high': 1.2, 'mid': 1.0, 'low': 0.78};
    const rc = {'short': 0.8, 'mid': 1.0, 'long': 1.24};
    final strength = (e.toneStrength ?? defaultToneStrength) / 100;
    final amt = 0.35 + 0.65 * strength;
    final tgt = (e.toneTarget ?? '$key-$range').split('-');
    b *= _pow(kb[tgt[0]]! / kb[key]!, amt);
    c *= _pow(rc[tgt[1]]! / rc[range]!, amt);
  }
  return DemoLook(b.clamp(0.4, 1.8), c.clamp(0.5, 1.8), s.clamp(0.0, 2.2), warm.clamp(-1.0, 1.0), l.gray * ls, l.sepia * ls);
}

double _pow(double a, double e) => a <= 0 ? 1 : math.pow(a, e).toDouble();

/// Renders one photo for the editor: latest-wins (stale renders are dropped), debounced,
/// off the UI thread (DappleSession runs each call in an isolate), session cached in [AppState].
class EditorEngine extends ChangeNotifier {
  EditorEngine(this.app);
  final AppState app;

  Photo? photo;
  ui.Image? image;
  ui.Image? original;

  /// The camera's embedded JPEG, shown (labelled 预览) until the first RAW render is ready.
  ui.Image? placeholder;
  List<Photo> _neighbors = const [];
  Analysis? analysis;
  Analysis? originalAnalysis;
  final Map<String, ui.Image> tiles = {};
  bool rendering = false;

  int _gen = 0;
  Timer? _debounce;
  bool _busy = false, _dirty = false;
  Timer? _tileDebounce;
  int _tileGen = 0;

  /// True when real pixels come from the native engine.
  bool get live => app.sessions.available && photo?.demo == null;

  /// True while the picture shown is a placeholder, not yet the engine's render.
  bool get previewOnly => live && image == null;

  /// [neighbors] (next, previous) are decoded in the background once this photo is on screen.
  void attach(Photo p, {List<Photo> neighbors = const []}) {
    _gen++;
    _tileGen++;
    photo = p;
    _neighbors = neighbors;
    _disposeImages();
    analysis = null;
    originalAnalysis = null;
    notifyListeners();
    if (!live) return;
    _dirty = true;
    _kick();
    unawaited(_loadPlaceholder(p, _gen));
    unawaited(_loadOriginal(p, _gen));
  }

  Future<void> _loadPlaceholder(Photo ph, int gen) async {
    final r = await app.sessions.preview(ph);
    if (r == null || gen != _gen || image != null) return;
    final img = await imageFromRgba(r);
    if (gen != _gen || image != null) {
      img.dispose();
      return;
    }
    placeholder?.dispose();
    placeholder = img;
    notifyListeners();
  }

  /// Call after the photo's edit state changed.
  void edited() {
    if (!live) return;
    _debounce?.cancel();
    _debounce = Timer(const Duration(milliseconds: 40), () {
      _dirty = true;
      _kick();
    });
  }

  Future<void> _kick() async {
    if (_busy) return;
    _busy = true;
    rendering = true;
    final myGen = _gen;
    try {
      while (_dirty && myGen == _gen) {
        _dirty = false;
        final ph = photo!;
        final s = await app.sessions.get(ph);
        if (s == null || myGen != _gen) break;
        final params = app.wireParams(ph);
        final r = await s.render(params, longEdge: 1600);
        final an = await s.analyze(params);
        if (myGen != _gen) break;
        if (_dirty) continue; // stale: a newer edit is waiting, skip presenting this frame
        final img = await imageFromRgba(r);
        if (myGen != _gen) {
          img.dispose();
          break;
        }
        final first = image == null;
        image?.dispose();
        image = img;
        if (first) {
          placeholder?.dispose();
          placeholder = null;
          if (_neighbors.isNotEmpty) {
            final g = _gen;
            unawaited(app.sessions.prefetch(_neighbors, () => g == _gen));
          }
        }
        if (an != null) {
          analysis = Analysis.fromJson(an);
          ph.nearest ??= analysis!.nearest;
        }
        notifyListeners();
      }
    } catch (e) {
      debugPrint('render failed: $e');
    } finally {
      _busy = false;
      rendering = false;
      notifyListeners();
    }
  }

  Future<void> _loadOriginal(Photo ph, int gen) async {
    try {
      final s = await app.sessions.get(ph);
      if (s == null || gen != _gen) return;
      final r = await s.render({'original': true}, longEdge: 1600);
      final an = await s.analyze({'original': true});
      if (gen != _gen) return;
      original = await imageFromRgba(r);
      if (an != null) originalAnalysis = Analysis.fromJson(an);
      notifyListeners();
    } catch (e) {
      debugPrint('original render failed: $e');
    }
  }

  /// Renders the nine 明暗 targets for this photo at thumbnail size (strength max(0.6, toneStrength)).
  void refreshTiles() {
    if (!live) return;
    _tileDebounce?.cancel();
    _tileDebounce = Timer(const Duration(milliseconds: 250), () => unawaited(_renderTiles()));
  }

  Future<void> _renderTiles() async {
    final ph = photo;
    if (ph == null) return;
    final gen = ++_tileGen;
    final s = await app.sessions.get(ph);
    if (s == null) return;
    final base = app.wireParams(ph);
    try {
      // one engine call renders all nine tonal classes (strength = max(60, toneStrength))
      final out = await s.tonePreviews(base, longEdge: 200);
      if (out == null || gen != _tileGen) return;
      for (var i = 0; i < out.length && i < 9; i++) {
        final img = await imageFromRgba(out[i]);
        if (gen != _tileGen) {
          img.dispose();
          return;
        }
        final id = _engineToneOrder[i];
        tiles.remove(id)?.dispose();
        tiles[id] = img;
      }
      notifyListeners();
    } catch (e) {
      debugPrint('tone previews failed: $e');
    }
  }

  /// Order of the engine's tone_previews output (tone.h toneClasses()).
  static const _engineToneOrder = [
    'low-short', 'low-mid', 'low-long', 'mid-short', 'mid-mid', 'mid-long', 'high-short', 'high-mid', 'high-long'
  ];

  void _disposeImages() {
    image?.dispose();
    original?.dispose();
    for (final t in tiles.values) {
      t.dispose();
    }
    placeholder?.dispose();
    image = null;
    original = null;
    placeholder = null;
    tiles.clear();
  }

  @override
  void dispose() {
    _gen++;
    _tileGen++;
    _debounce?.cancel();
    _tileDebounce?.cancel();
    _disposeImages();
    super.dispose();
  }
}
