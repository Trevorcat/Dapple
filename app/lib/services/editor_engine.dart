import 'dart:async';
import 'dart:io';
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/scheduler.dart';

import '../model/catalog.dart';
import '../model/params.dart';
import '../models.dart';
import '../state/app_state.dart';
import '../native/dapple_bindings.dart' show dappleFrameAnalysis, dappleFrameInteractive;
import '../native/dapple_session.dart';
import 'adaptive_quality.dart';
import 'edit_interaction.dart';
import 'gpu_view_backend.dart';
import 'perf_probe.dart';
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

/// Overlap the upload of drag frame N with the render of N+1 (A/B switch for measurements; default on).
const bool _kPipeline = bool.fromEnvironment('DAPPLE_PIPELINE', defaultValue: true);

double _pow(double a, double e) => a <= 0 ? 1 : math.pow(a, e).toDouble();

/// Renders one photo for the editor, off the UI thread.
///
/// Frame pipeline: every frame is ONE native call on the session's persistent worker isolate
/// ([DappleSession.renderFrame]) that returns pixels + tonal analysis; the pixels are copied once
/// into an engine image ([NativeFrame.toImage]). Point-wise stages run through a lattice baked once per
/// parameter change in the engine, so a frame is one pass over the pixels plus the neighbourhood stages.
///
/// Interaction tiers (latest wins, no backlog: while a frame renders, newer edits only mark the state
/// dirty and the newest one is rendered next; every finished frame is presented):
///  * interactive: while a slider/pad is being dragged (explicit [beginInteraction]/[endInteraction],
///    wired for [DSlider] through [EditInteraction], or inferred from a burst of [edited] calls) frames are
///    rendered at [interactiveLongEdge] (<= 1080 px) with the cheaper neighbourhood pipeline, with no
///    debounce;
///  * final: [idleDelay] (150 ms) after the last edit / when the drag ends, one frame at
///    [finalLongEdge] (default 1600 px) with everything on.
///
/// Zoom detail: see [setViewport].
class EditorEngine extends ChangeNotifier {
  EditorEngine(this.app) {
    EditInteraction.depth.addListener(_onInteractionDepth);
    PerfProbe.start();
  }
  final AppState app;

  Photo? photo;
  ui.Image? image;
  ui.Image? original;

  // ---- zoom detail ----------------------------------------------------------------------------------
  /// High-resolution render of the visible region while zoomed in; null when not available. The UI
  /// draws it over [image], stretched to [detailRect], and falls back to [image] where it is null.
  /// Contract: it is only set when the zoom is above [detailMinZoom] and the view has been still for
  /// [detailDelay]; it is cleared (null) on every edit, when the photo changes and when zooming out.
  /// While panning/pinching the previous detail stays (it is positioned by [detailRect], not by the
  /// viewport) until a fresh one replaces it.
  ui.Image? detail;

  /// Normalised (0..1, in the image's own orientation/crop space) rectangle [detail] covers. It is
  /// the visible region inflated by 12% per side so a small pan does not uncover the base image.
  ui.Rect? detailRect;

  /// Zoom above which detail is rendered (displayed pixels per preview pixel).
  static const double detailMinZoom = 1.3;

  /// Idle time of the viewport before the detail render starts.
  static const Duration detailDelay = Duration(milliseconds: 200);

  /// Tells the engine what is on screen. [normalized]: the visible part of the picture as a 0..1 rect
  /// (0,0 = top left of the picture); [zoom]: displayed (device) pixels per pixel of the current
  /// [image]. Call it whenever the view changes (it is cheap; it only restarts a timer). When
  /// zoom > [detailMinZoom] and the view is idle for [detailDelay], the visible region is rendered at
  /// screen resolution from a higher-resolution session (decoded lazily in the background for this
  /// photo, at most [SessionCache.detailLongEdge] px, then cached) and published as [detail].
  void setViewport(ui.Rect normalized, double zoom) {
    _view = normalized;
    _zoom = zoom;
    _viewVersion++;
    final w = image?.width ?? 0;
    if (w > 0) _fullWidthPx = zoom * w;
    if (!live) return;
    _detailTimer?.cancel();
    if (zoom <= detailMinZoom) {
      _clearDetail();
      return;
    }
    _scheduleDetail();
  }

  ui.Rect _view = const ui.Rect.fromLTWH(0, 0, 1, 1);
  double _zoom = 1;
  double _fullWidthPx = 0; // displayed device pixels across the whole picture's width
  Timer? _detailTimer;
  int _detailGen = 0;
  int _viewVersion = 0;
  bool _detailBusy = false;

  // ---- interaction ----------------------------------------------------------------------------------
  /// Dynamic resolution of drag-time frames (see [AdaptiveQuality]): long edge <= 1080, stepping down when the
  /// measured frame time exceeds the budget, back up when there is headroom.
  final AdaptiveQuality quality = AdaptiveQuality();

  /// Long edge of drag-time frames (current rung of [quality]).
  int get interactiveLongEdge => quality.rung.longEdge;

  /// Long edge of idle (full quality) frames.
  int finalLongEdge = 1600;

  /// Idle time after the last edit before the full-quality frame is rendered.
  static const Duration idleDelay = Duration(milliseconds: 150);

  /// Adapts the frame sizes to the picture's on-screen size in device pixels (long edge): drag frames
  /// are capped at 1080 and never smaller than 640, idle frames sit between 1080 and the source's 2048.
  void setDisplayLongEdge(int devicePx) {
    quality.setDisplayLongEdge(devicePx);
    finalLongEdge = devicePx.clamp(1080, 2048);
  }

  int _drag = 0;
  int _lastDepth = 0;
  Timer? _idle;
  bool _burst = false; // edits arrive faster than idleDelay -> a drag, even without begin/endInteraction
  bool _needFinal = false;
  final Stopwatch _clock = Stopwatch()..start();
  int _lastEditMs = -100000;
  int _editUs = 0; // time of the newest edit (latency probe)
  int _releaseMs = -1; // when the last drag ended (final-frame latency probe)
  int _openMs = 0; // attach() time (time-to-first-render probe)
  bool _perfConfigured = false;
  bool _perfLogged = false;

  /// A control is being dragged: render interactive frames until [endInteraction].
  void beginInteraction() {
    _drag++;
    _idle?.cancel();
    if (_drag == 1) PerfProbe.dragging = true;
  }

  // Thermal headroom (AThermal_getThermalHeadroom, a binder call that the platform rate-limits to ~1/s) is
  // polled off the drag path: after a drag ends, at most every [_thermalEvery], and applied to the next drag
  // so the interactive resolution steps down before the SoC throttles. No polling while the editor is idle.
  static const int _thermalEvery = 5000;
  int _thermalAtMs = -100000;
  void _pollThermal() {
    final now = _clock.elapsedMilliseconds;
    if (now - _thermalAtMs < _thermalEvery) return;
    _thermalAtMs = now;
    final h = DappleSession.thermalHeadroom(10);
    if (h >= 0) quality.setThermalHeadroom(h);
  }

  /// The drag ended: the full-quality frame follows after [idleDelay].
  void endInteraction() {
    if (_drag > 0) _drag--;
    if (_drag == 0) {
      _releaseMs = _clock.elapsedMilliseconds;
      PerfProbe.dragging = false;
      _scheduleFinal(delay: releaseDelay);
      _schedulePrefetch();
      _pollThermal();
      if (kPerfProbe && !_perfLogged) {
        _perfLogged = true; // after the first drag: is an ADPF session actually running?
        PerfProbe.log('perfinfo2', DappleSession.perfInfo() ?? const {});
      }
    }
  }

  void _onInteractionDepth() {
    final d = EditInteraction.depth.value;
    if (d > 0 && _lastDepth == 0) beginInteraction();
    if (d == 0 && _lastDepth > 0) endInteraction();
    _lastDepth = d;
  }

  bool get _interactiveNow => _drag > 0 || (_burst && _clock.elapsedMilliseconds - _lastEditMs < idleDelay.inMilliseconds);

  /// After an explicit finger-up ([endInteraction]) the full-quality frame starts sooner: the drag is known to be
  /// over, the delay only absorbs an immediate re-grab. [idleDelay] stays for drags inferred from edit bursts.
  static const Duration releaseDelay = Duration(milliseconds: 50);

  void _scheduleFinal({Duration delay = idleDelay}) {
    _idle?.cancel();
    if (!_needFinal) return;
    _idle = Timer(delay, () {
      _burst = false;
      if (_drag > 0 || !live) return;
      _needFinal = false;
      _dirty = true;
      _kick();
    });
  }

  /// The camera's embedded JPEG, shown (labelled 预览) until the first RAW render is ready.
  ui.Image? placeholder;
  List<Photo> _neighbors = const [];
  Analysis? analysis;
  Analysis? originalAnalysis;
  final Map<String, ui.Image> tiles = {};
  bool rendering = false;

  int _gen = 0;
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
    _detailGen++;
    _detailTimer?.cancel();
    _originalTimer?.cancel();
    _prefetchTimer?.cancel();
    _idle?.cancel();
    _needFinal = false;
    _burst = false;
    _openMs = _clock.elapsedMilliseconds;
    if (!_perfConfigured) {
      _perfConfigured = true;
      // ADPF target = the drag frame budget; threads: all performance cores (the engine picks them on big.LITTLE)
      DappleSession.configurePerf(targetFrameMs: quality.budgetMs.round());
      PerfProbe.log('perfinfo', DappleSession.perfInfo() ?? const {});
      PerfProbe.textBench();
    }
    _pollThermal();
    if (photo?.id != p.id) app.sessions.closeDetail(); // the high-resolution session belongs to one photo
    photo = p;
    _neighbors = neighbors;
    _gpuDetachSession();
    _disposeImages();
    analysis = null;
    originalAnalysis = null;
    notifyListeners();
    if (!live) return;
    _dirty = true;
    _placeholderDone = Completer<void>();
    unawaited(_loadPlaceholder(p, _gen));
    _kick();
  }

  /// Completes when the placeholder is on screen (or given up); the first RAW decode waits for it at most
  /// [placeholderHead], so on a slow phone the decode (all cores) does not starve the placeholder.
  Completer<void>? _placeholderDone;
  static const Duration placeholderHead = Duration(milliseconds: 800);

  Future<void> _loadPlaceholder(Photo ph, int gen) async {
    final done = _placeholderDone;
    try {
      // 1. the photo's developed preview (the JPEG the library grid shows): a plain JPEG decode on the
      //    engine's IO thread, no isolate, no RAW parsing; 2. else the camera's embedded JPEG.
      ui.Image? img = await _developedPreview(ph);
      if (img == null) {
        final r = await app.sessions.preview(ph);
        if (r == null || gen != _gen || image != null) return;
        img = await imageFromRgba(r);
      }
      _showPlaceholder(img, gen);
    } finally {
      if (done != null && !done.isCompleted) done.complete();
    }
  }

  Future<ui.Image?> _developedPreview(Photo ph) async {
    final path = ph.previewPath;
    if (path == null) return null;
    try {
      final bytes = await File(path).readAsBytes();
      final buf = await ui.ImmutableBuffer.fromUint8List(bytes);
      final desc = await ui.ImageDescriptor.encoded(buf);
      // 1280 px long edge is plenty for a placeholder and halves the decode
      final landscape = desc.width >= desc.height;
      final codec = await desc.instantiateCodec(
          targetWidth: landscape ? math.min(1280, desc.width) : null, targetHeight: landscape ? null : math.min(1280, desc.height));
      final f = await codec.getNextFrame();
      codec.dispose();
      desc.dispose();
      buf.dispose();
      return f.image;
    } catch (_) {
      return null;
    }
  }

  void _showPlaceholder(ui.Image img, int gen) {
    if (gen != _gen || image != null) {
      img.dispose();
      return;
    }
    placeholder?.dispose();
    placeholder = img;
    PerfProbe.log('open_placeholder', {'ms': _clock.elapsedMilliseconds - _openMs});
    notifyListeners();
  }

  /// Call after the photo's edit state changed. Renders immediately (no debounce); a burst of calls
  /// (or an explicit drag) switches to interactive frames, the full-quality frame follows when it stops.
  void edited() {
    if (!live) return;
    final now = _clock.elapsedMilliseconds;
    _burst = now - _lastEditMs < 200;
    _lastEditMs = now;
    _editUs = _clock.elapsedMicroseconds;
    _detailTimer?.cancel();
    _clearDetail();
    _dirty = true;
    if (_drag > 0 || _burst) _needFinal = true;
    // a single discrete edit (tap, look change) is rendered at final quality straight away
    if (!_interactiveNow) _needFinal = false;
    _scheduleFinal();
    _kick();
  }

  Future<void> _kick() async {
    if (_busy) return;
    _busy = true;
    rendering = true;
    final myGen = _gen;
    // Presentation (pixels -> GPU image -> on screen) of the previous interactive frame. It overlaps the native
    // render of the next one, so a drag frame costs max(render, upload) instead of render + upload.
    Future<void>? presenting;
    try {
      // the first decode of a photo waits briefly for the placeholder: on a slow phone the decode takes every
      // core and the placeholder (a JPEG decode) would otherwise show up seconds late
      final ph0 = photo;
      final pd = _placeholderDone;
      if (ph0 != null && pd != null && !pd.isCompleted && !app.sessions.has(ph0.id)) {
        await pd.future.timeout(placeholderHead, onTimeout: () {});
      }
      while (_dirty && myGen == _gen) {
        _dirty = false;
        final ph = photo!;
        final s = await app.sessions.get(ph);
        if (s == null || myGen != _gen) break;
        final interactive = _interactiveNow;
        final params = app.wireParams(ph);
        final rung = quality.rung;
        final editUs = _editUs; // newest edit this frame contains
        if (_gpuActive) {
          _submitGpu(s, params, interactive, rung); // zero-copy: the native loop renders and presents (latest wins)
          continue;
        }
        final t0 = _clock.elapsedMicroseconds;
        final frame = await s.renderFrame(params,
            longEdge: interactive ? rung.longEdge : finalLongEdge,
            interactive: interactive,
            bakeSize: interactive ? rung.bake : 0);
        final t1 = _clock.elapsedMicroseconds;
        if (myGen != _gen) {
          frame.dispose();
          break;
        }
        final prev = presenting;
        final p = _present(frame, myGen, interactive, editUs, t0, t1, prev);
        if (!interactive || !_kPipeline) {
          await p; // final frames: in order, nothing to overlap with
          presenting = null;
          continue;
        }
        presenting = p;
        // pacing / latest wins: at most one frame uploading while the next renders, and the next render starts
        // only once the frame before this one is on screen (never more frames than the display shows; every edit
        // that arrives meanwhile is coalesced into the newest state)
        if (prev != null && _dirty) await prev;
      }
      if (presenting != null) await presenting;
    } catch (e) {
      debugPrint('render failed: $e');
    } finally {
      _busy = false;
      rendering = false;
      notifyListeners();
    }
    // edits that arrived while the last presentation finished
    if (_dirty && myGen == _gen) {
      unawaited(_kick());
      return;
    }
    if (myGen == _gen && !_dirty && !_interactiveNow && _zoom > detailMinZoom) _scheduleDetail();
    // the before/after picture is not needed to show or edit: render it once things are idle (it shares the
    // session with the drag frames, so it must never start in the middle of a drag)
    if (myGen == _gen && !_dirty && !_interactiveNow && original == null) {
      _originalTimer?.cancel();
      final g = _gen, ph = photo;
      _originalTimer = Timer(const Duration(milliseconds: 600), () {
        if (g == _gen && ph != null && original == null && !_busy && !_dirty && !_interactiveNow && !_gpuInFlight) unawaited(_loadOriginal(ph, g));
      });
    }
  }

  /// Uploads and shows one rendered frame, after [after] (the previous frame's presentation) so frames appear in
  /// order. Interactive frames complete when the Flutter frame that shows them has been built.
  Future<void> _present(NativeFrame frame, int myGen, bool interactive, int editUs, int t0, int t1, Future<void>? after) async {
    try {
      final img = await frame.toImage();
      final tUp = _clock.elapsedMicroseconds; // upload done (before waiting for the previous frame to show)
      if (after != null) await after;
      if (myGen != _gen) {
        img.dispose();
        return;
      }
      final t2 = _clock.elapsedMicroseconds;
      final first = image == null;
      image?.dispose();
      image = img;
      if (first) {
        placeholder?.dispose();
        placeholder = null;
        _schedulePrefetch();
        _maybeStartGpu(img);
      }
      final an = frame.analysis;
      if (an != null) {
        analysis = Analysis.fromJson(an);
        photo?.nearest ??= analysis!.nearest;
      }
      notifyListeners();
      if (kPerfProbe) {
        final kind = interactive ? 'i' : 'f', after = _releaseMs;
        final w = img.width, nat = frame.nativeMs;
        SchedulerBinding.instance.addPostFrameCallback((_) {
          final nowUs = _clock.elapsedMicroseconds;
          PerfProbe.log('frame', {
            'k': kind,
            'e2p': editUs > 0 ? (nowUs - editUs) / 1000.0 : -1,
            'wait': editUs > 0 ? (t0 - editUs) / 1000.0 : -1, // edit -> render start
            'native': nat,
            'dart': (t1 - t0) / 1000.0 - nat, // isolate hops + JSON + queueing
            'toimg': (t2 - t1) / 1000.0, // upload (+ waiting for the previous frame to be shown)
            'w': w,
            'first': first,
            if (!interactive && after >= 0) 'rel2final': _clock.elapsedMilliseconds - after,
          });
        });
        if (!interactive && after >= 0) _releaseMs = -1;
        if (first) PerfProbe.log('open_first_frame', {'ms': _clock.elapsedMilliseconds - _openMs});
      }
      if (interactive) {
        // the drag frame rate is bounded by the slower of the two overlapped stages
        final renderMs = (t1 - t0) / 1000.0, uploadMs = (tUp - t1) / 1000.0;
        if (_skipRecord) {
          _skipRecord = false; // first frame at a new size also builds the scaled source: not representative
        } else if (quality.record(math.max(renderMs, uploadMs))) {
          _skipRecord = true;
          PerfProbe.log('rung', {'to': quality.rung.toString(), 'ema': quality.emaMs});
        }
        await SchedulerBinding.instance.endOfFrame;
      }
    } catch (e) {
      frame.dispose();
      debugPrint('present failed: $e');
    }
  }
  bool _skipRecord = false;


  // ---- zero-copy display (stage d) -----------------------------------------------------------------------
  // After the first frame of a photo has been shown through the readback path, drag and final frames go to the native
  // render loop (GpuViewBackend), which draws them straight onto a Flutter texture: no decodeImageFromPixels, no pixels
  // across isolates. The readback path above stays the permanent fallback (any failure ends in _disableGpu).
  GpuViewBackend? _gpu;
  bool _gpuStarting = false, _gpuDisabled = false, _gpuLive = false, _closed = false;
  int _gpuW = 0, _gpuH = 0, _gpuFails = 0, _gpuSubmitted = 0, _gpuDone = 0, _gpuSubmitMs = 0;
  final Map<int, ({bool interactive, int editUs})> _gpuMeta = {};

  /// Texture showing the current photo's frames, or null while the readback image is what is on screen.
  int? get gpuTextureId => _gpuLive ? _gpu?.textureId : null;

  /// Aspect (w / h) of the picture the texture shows, null when the texture is not live.
  double? get gpuAspect => _gpuLive && _gpuH > 0 ? _gpuW / _gpuH : null;

  /// Width of the newest frame in pixels (0 when not live).
  int get gpuFrameWidth => _gpuLive ? _gpuW : 0;

  bool get _gpuActive => _gpu != null && _gpu!.ready && !_gpuDisabled && image != null;
  bool get _gpuInFlight => _gpuSubmitted > _gpuDone;

  void _maybeStartGpu(ui.Image first) {
    if (_gpu != null || _gpuStarting || _gpuDisabled || !GpuViewBackend.supported) return;
    _gpuStarting = true;
    GpuViewBackend.start(first.width, first.height, onFrame: _onGpuFrame).then((b) {
      _gpuStarting = false;
      if (b == null) {
        _gpuDisabled = true;
        PerfProbe.log('gpu_view', {'enabled': false});
        return;
      }
      if (_closed) {
        b.dispose();
        return;
      }
      _gpu = b;
      PerfProbe.log('gpu_view', {'enabled': true, ...?b.info()});
    });
  }

  void _submitGpu(DappleSession s, Map<String, dynamic> params, bool interactive, QualityRung rung) {
    final g = _gpu!;
    if (_gpuInFlight && _clock.elapsedMilliseconds - _gpuSubmitMs > 3000) {
      _disableGpu('no frame for 3 s'); // the loop stalled: take the readback path again
      return;
    }
    if (g.boundSession != s.address) g.bindSession(s.address);
    final seq = g.submit(params,
        longEdge: interactive ? rung.longEdge : finalLongEdge,
        flags: (interactive ? dappleFrameInteractive : 0) | dappleFrameAnalysis,
        bakeSize: interactive ? rung.bake : 0);
    if (seq <= 0) {
      _disableGpu('submit failed');
      return;
    }
    if (!_gpuInFlight) _gpuSubmitMs = _clock.elapsedMilliseconds;
    _gpuSubmitted = seq;
    _gpuMeta[seq] = (interactive: interactive, editUs: _editUs);
  }

  void _onGpuFrame(GpuFrameEvent e) {
    final g = _gpu;
    if (g == null || _closed) return;
    if (e.seq > _gpuDone) _gpuDone = e.seq;
    final meta = _gpuMeta.remove(e.seq);
    _gpuMeta.removeWhere((k, _) => k < e.seq);
    if (_gpuSubmitMs != 0 && _gpuInFlight) _gpuSubmitMs = _clock.elapsedMilliseconds;
    switch (e.status) {
      case 0: // shown
        _gpuFails = 0;
        final firstLive = !_gpuLive;
        _gpuLive = true;
        _gpuW = e.width;
        _gpuH = e.height;
        final interactive = meta?.interactive ?? false;
        final an = g.takeAnalysis();
        if (an != null) {
          analysis = Analysis.fromJson(an);
          photo?.nearest ??= analysis!.nearest;
        }
        // the texture follows the picture: crop changes alter the aspect, a new screen size alters the final frame size
        final aspectOff = ((e.width / e.height) / (g.texW / g.texH) - 1).abs() > 0.015;
        final sizeOff = !interactive && (e.width - g.texW).abs() > g.texW * 0.1;
        if (aspectOff || sizeOff) {
          if (aspectOff && interactive) {
            final k = math.max(g.texW, g.texH) / math.max(e.width, e.height);
            unawaited(g.resize((e.width * k).round(), (e.height * k).round()));
          } else {
            unawaited(g.resize(e.width, e.height));
          }
        }
        if (interactive) {
          final ms = math.max(e.renderMs, e.presentMs);
          if (_skipRecord) {
            _skipRecord = false;
          } else if (quality.record(ms)) {
            _skipRecord = true;
            PerfProbe.log('rung', {'to': quality.rung.toString(), 'ema': quality.emaMs});
          }
        }
        if (kPerfProbe) {
          final editUs = meta?.editUs ?? -1;
          PerfProbe.log('frame', {
            'k': interactive ? 'g' : 'gf', // zero-copy frame
            'e2p': editUs > 0 ? (_clock.elapsedMicroseconds - editUs) / 1000.0 : -1,
            'native': e.renderMs,
            'present': e.presentMs,
            'lat': e.latencyMs,
            'w': e.width,
            'first': firstLive,
          });
        }
        if (an != null || firstLive || !interactive) notifyListeners();
        if (!interactive && !_gpuInFlight && !_dirty) _gpuIdleTail();
        break;
      case 1: // render failed
        if (++_gpuFails >= 3) _disableGpu('render failed ${e.status}');
        break;
      case 2: // present failed: surface / device problems
        if (++_gpuFails >= 2) _disableGpu('present failed');
        break;
      default: // no window / session right now (background): nothing to do, the loop re-renders on attach
        break;
    }
  }

  /// What `_kick` does once things are idle (zoom detail, before/after picture), for frames that finish natively.
  void _gpuIdleTail() {
    if (_interactiveNow || _busy) return;
    if (_zoom > detailMinZoom) _scheduleDetail();
    if (original == null) {
      _originalTimer?.cancel();
      final g = _gen, ph = photo;
      _originalTimer = Timer(const Duration(milliseconds: 600), () {
        if (g == _gen && ph != null && original == null && !_busy && !_dirty && !_interactiveNow && !_gpuInFlight) unawaited(_loadOriginal(ph, g));
      });
    }
  }

  /// Back to the readback path for the rest of the process (diagnostics in the perf log).
  void _disableGpu(String why) {
    if (_gpuDisabled && _gpu == null) return;
    debugPrint('gpu view disabled: $why');
    PerfProbe.log('gpu_view', {'enabled': false, 'why': why, ...?_gpu?.info()});
    _gpuDisabled = true;
    _gpuLive = false;
    final g = _gpu;
    _gpu = null;
    g?.dispose();
    _gpuMeta.clear();
    _gpuSubmitted = _gpuDone = 0;
    // the readback image is stale: render the current state through the old path
    if (!_closed && live) {
      _dirty = true;
      unawaited(_kick());
    }
    notifyListeners();
  }

  void _gpuDetachSession() {
    _gpuLive = false;
    _gpuMeta.clear();
    _gpuSubmitted = _gpuDone = 0;
    final g = _gpu;
    if (g != null && g.boundSession != 0) g.bindSession(0); // before the session can be closed
  }
  // ---- neighbour prefetch ----------------------------------------------------------------------------
  // Decoding the next / previous RAW takes ~10 s of CPU and ~350 MB transient memory on a Snapdragon 680 class
  // phone. Started right after the first frame it ran under the user's first drags (no drag frame for 3 s, the
  // guest swapping at 1.1 GB PSS). So it waits until the editor has been idle for [prefetchIdle], stops between
  // photos as soon as a drag starts (resumed after it), and on devices with <= 5 GB of RAM only the next photo is
  // decoded ahead.
  static const Duration prefetchIdle = Duration(milliseconds: 2500);
  Timer? _prefetchTimer;
  bool _prefetching = false;

  void _schedulePrefetch() {
    if (_neighbors.isEmpty || !live) return;
    _prefetchTimer?.cancel();
    final g = _gen;
    _prefetchTimer = Timer(prefetchIdle, () async {
      if (g != _gen || _prefetching) return;
      if (_drag > 0 || _interactiveNow || _busy || _dirty) {
        _schedulePrefetch();
        return;
      }
      final want = _lowRam ? _neighbors.take(1).toList() : _neighbors;
      if (want.every((n) => app.sessions.has(n.id))) return;
      _prefetching = true;
      try {
        await app.sessions.prefetch(want, () => g == _gen && _drag == 0);
      } finally {
        _prefetching = false;
      }
    });
  }

  static final bool _lowRam = () {
    try {
      final m = RegExp(r'MemTotal:\s+(\d+)').firstMatch(File('/proc/meminfo').readAsStringSync());
      return m != null && int.parse(m.group(1)!) <= 5 * 1024 * 1024;
    } catch (_) {
      return false;
    }
  }();
  Timer? _originalTimer;

  // ---- zoom detail ----------------------------------------------------------------------------------
  void _scheduleDetail() {
    _detailTimer?.cancel();
    _detailTimer = Timer(detailDelay, () => unawaited(_renderDetail()));
  }

  void _clearDetail() {
    _detailGen++;
    if (detail == null && detailRect == null) return;
    detail?.dispose();
    detail = null;
    detailRect = null;
    notifyListeners();
  }

  Future<void> _renderDetail() async {
    final ph = photo;
    final img = image;
    if (ph == null || img == null || !live || _zoom <= detailMinZoom || _fullWidthPx <= 0) return;
    if (_busy || _dirty || _interactiveNow || _gpuInFlight) {
      _scheduleDetail(); // edits in flight: try again once the view is still
      return;
    }
    if (_detailBusy) return;
    final vis = _view;
    // inflate the visible region so a small pan keeps being covered
    final dx = vis.width * 0.12, dy = vis.height * 0.12;
    final region = ui.Rect.fromLTRB(
        (vis.left - dx).clamp(0.0, 1.0), (vis.top - dy).clamp(0.0, 1.0), (vis.right + dx).clamp(0.0, 1.0), (vis.bottom + dy).clamp(0.0, 1.0));
    if (region.width <= 0.01 || region.height <= 0.01) return;
    final cur = detailRect;
    if (cur != null && detail != null && cur.left <= vis.left && cur.top <= vis.top && cur.right >= vis.right && cur.bottom >= vis.bottom) {
      // covers the view; sharp enough unless the user zoomed in much further since (pixels per picture width)
      final have = detail!.width / cur.width;
      final want = math.min(_fullWidthPx, SessionCache.detailLongEdge * 1.25);
      if (have >= 0.85 * want) return;
    }
    // device pixels the region occupies on screen (capped: no point beyond the detail source resolution)
    final aspect = img.height / img.width;
    final wPx = region.width * _fullWidthPx, hPx = region.height * _fullWidthPx * aspect;
    final longFrac = aspect > 1 ? region.height : region.width; // extent along the picture's long edge
    final srcCap = longFrac * SessionCache.detailLongEdge * 1.25;
    final out = math.min(math.max(wPx, hPx), math.min(2800.0, srcCap)).round().clamp(256, 2800);
    final gen = ++_detailGen, myGen = _gen, version = _viewVersion;
    _detailBusy = true;
    try {
      final ds = await app.sessions.detail(ph); // decodes lazily the first time (about a second)
      if (ds == null || gen != _detailGen || myGen != _gen) return;
      final frame = await ds.renderFrame(app.wireParams(ph), region: region, regionLongEdge: out, analysis: false, background: true);
      if (gen != _detailGen || myGen != _gen) {
        frame.dispose();
        return;
      }
      final im = await frame.toImage();
      if (gen != _detailGen || myGen != _gen) {
        im.dispose();
        return;
      }
      detail?.dispose();
      detail = im;
      detailRect = region;
      notifyListeners();
    } catch (e) {
      debugPrint('detail render failed: $e');
    } finally {
      _detailBusy = false;
    }
    // the view moved (or an edit cleared the detail) while rendering: look again once it is still
    if (myGen == _gen && _zoom > detailMinZoom && (version != _viewVersion || gen != _detailGen)) _scheduleDetail();
  }

  Future<void> _loadOriginal(Photo ph, int gen) async {
    try {
      final s = await app.sessions.get(ph);
      if (s == null || gen != _gen) return;
      final frame = await s.renderFrame({'original': true}, longEdge: 1600);
      if (gen != _gen) {
        frame.dispose();
        return;
      }
      final an = frame.analysis;
      final img = await frame.toImage();
      if (gen != _gen) {
        img.dispose();
        return;
      }
      original = img;
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
    detail?.dispose();
    detail = null;
    detailRect = null;
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
    _closed = true;
    _gpuLive = false;
    _gpu?.bindSession(0); // before the sessions are closed
    _gpu?.dispose();
    _gpu = null;
    _gen++;
    _tileGen++;
    _detailGen++;
    EditInteraction.depth.removeListener(_onInteractionDepth);
    _idle?.cancel();
    _originalTimer?.cancel();
    _prefetchTimer?.cancel();
    _detailTimer?.cancel();
    _tileDebounce?.cancel();
    _disposeImages();
    super.dispose();
  }
}
