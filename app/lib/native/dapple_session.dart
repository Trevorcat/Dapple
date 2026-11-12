import 'dart:async';
import 'dart:convert';
import 'dart:ffi' as ffi;
import 'dart:isolate';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:ffi/ffi.dart';

import 'dapple_bindings.dart';
import '../services/perf_probe.dart';

class DappleException implements Exception {
  DappleException(this.message);
  final String message;
  @override
  String toString() => 'DappleException: $message';
}

/// A rendered RGBA8888 image (stride == width * 4).
class RenderedImage {
  RenderedImage(this.width, this.height, this.rgba);
  final int width;
  final int height;
  final Uint8List rgba;
}

DappleBindings _b() {
  final b = DappleBindings.instanceOrNull;
  if (b == null) throw DappleException('原生库 libdapple 未加载: ${DappleBindings.loadError}');
  return b;
}

String _lastError(DappleBindings b) => b.lastError().toDartString();

/// Takes ownership of a native string result (JSON) and frees it.
String? _takeString(DappleBindings b, ffi.Pointer<Utf8> p) {
  if (p == ffi.nullptr) return null;
  final s = p.toDartString();
  b.freeString(p);
  return s;
}

Map<String, dynamic>? _decodeObj(String? s) {
  if (s == null || s.isEmpty) return null;
  final v = jsonDecode(s);
  return v is Map<String, dynamic> ? v : null;
}

/// One rendered interactive frame. The pixels stay in native memory (RGBA8888, stride width * 4)
/// until [toImage] copies them once into an engine-owned buffer and releases them, or [dispose]
/// drops them. Exactly one of the two must be called.
class NativeFrame {
  NativeFrame._(this.width, this.height, this._addr, this.analysisJson, [this.nativeMs = 0]);

  /// Wall time of the native call (inside the worker isolate), ms.
  final double nativeMs;
  final int width;
  final int height;
  final int _addr;

  /// Tonal analysis JSON of this very render (same schema as [DappleSession.analyze]), when asked for.
  final String? analysisJson;
  bool _released = false;

  Map<String, dynamic>? get analysis => _decodeObj(analysisJson);

  /// Zero-copy view of the native pixels (valid until [toImage]/[dispose]).
  Uint8List get pixels => ffi.Pointer<ffi.Uint8>.fromAddress(_addr).asTypedList(width * height * 4);

  void _release() {
    if (_released) return;
    _released = true;
    final b = DappleBindings.instanceOrNull;
    if (b == null || _addr == 0) return;
    final img = calloc<DappleImageStruct>();
    img.ref
      ..width = width
      ..height = height
      ..rgba = ffi.Pointer<ffi.Uint8>.fromAddress(_addr);
    b.freeImage(img);
    calloc.free(img);
  }

  void dispose() => _release();

  /// Native pixels -> GPU-ready [ui.Image] with a single copy (ImmutableBuffer), then frees them.
  Future<ui.Image> toImage() async {
    try {
      final t0 = PerfProbe.begin();
      final buf = await ui.ImmutableBuffer.fromUint8List(pixels);
      PerfProbe.end('immbuf', t0);
      _release();
      final desc = ui.ImageDescriptor.raw(buf, width: width, height: height, pixelFormat: ui.PixelFormat.rgba8888);
      try {
        final codec = await desc.instantiateCodec();
        final frame = await codec.getNextFrame();
        codec.dispose();
        return frame.image;
      } finally {
        desc.dispose();
        buf.dispose();
      }
    } finally {
      _release();
    }
  }
}

// ---- persistent frame worker (one isolate per session) -------------------------------------------

/// Message: [replyPort, id, address, paramsJson, lutPath, longEdge, flags, bakeSize, rx0, ry0, rx1, ry1, regionLongEdge, activeParam?]
/// or [replyPort, id, 'quit']. Reply: [id, 'ok', width, height, rgbaAddress, analysisJson?] or [id, 'err', message].
void _frameWorkerMain(SendPort initPort) {
  final rp = ReceivePort();
  initPort.send(rp.sendPort);
  rp.listen((dynamic m) {
    final msg = m as List;
    final reply = msg[0] as SendPort;
    final id = msg[1] as int;
    if (msg[2] == 'quit') {
      rp.close();
      return;
    }
    reply.send(_frameNative(id, msg));
  });
}

List<Object?> _frameNative(int id, List msg) {
  ffi.Pointer<Utf8>? pp, lp, ap;
  ffi.Pointer<DappleFrameRequestStruct>? rq;
  ffi.Pointer<DappleImageStruct>? img;
  ffi.Pointer<ffi.Pointer<Utf8>>? an;
  try {
    final b = _b();
    final f = b.sessionRenderFrame;
    if (f == null) return [id, 'err', '原生库缺少 dapple_session_render_frame'];
    pp = (msg[3] as String).toNativeUtf8();
    lp = ((msg[4] as String?) ?? '').toNativeUtf8();
    rq = calloc<DappleFrameRequestStruct>();
    rq.ref
      ..size = ffi.sizeOf<DappleFrameRequestStruct>()
      ..longEdge = msg[5] as int
      ..flags = msg[6] as int
      ..bakeSize = msg[7] as int
      ..rx0 = msg[8] as double
      ..ry0 = msg[9] as double
      ..rx1 = msg[10] as double
      ..ry1 = msg[11] as double
      ..regionLongEdge = msg[12] as int;
    final act = msg.length > 13 ? msg[13] as String? : null;
    if (act != null && act.isNotEmpty) {
      ap = act.toNativeUtf8();
      rq.ref.activeParam = ap;
    }
    img = calloc<DappleImageStruct>();
    an = calloc<ffi.Pointer<Utf8>>();
    final sw = Stopwatch()..start();
    final rc = f(ffi.Pointer<DappleSessionStruct>.fromAddress(msg[2] as int), pp, lp, rq, img, an);
    final nativeMs = sw.elapsedMicroseconds / 1000.0;
    if (rc != dappleOk) return [id, 'err', _lastError(b)];
    String? analysis;
    if (an.value != ffi.nullptr) {
      analysis = an.value.toDartString();
      b.freeString(an.value);
    }
    return [id, 'ok', img.ref.width, img.ref.height, img.ref.rgba.address, analysis, nativeMs];
  } catch (e) {
    return [id, 'err', e.toString()];
  } finally {
    if (pp != null) malloc.free(pp);
    if (lp != null) malloc.free(lp);
    if (ap != null) malloc.free(ap);
    if (rq != null) calloc.free(rq);
    if (img != null) calloc.free(img);
    if (an != null) calloc.free(an);
  }
}

/// Friendly wrapper around a native DappleSession.
///
/// Every heavy call runs in a short-lived background isolate (Isolate.run);
/// the native session pointer travels as an int address. Calls on one session
/// are serialised (the C API forbids concurrent use of a single session).
class DappleSession {
  DappleSession._(this._address);
  final int _address;

  /// Native session pointer (for the zero-copy view, which renders on its own thread; see GpuViewBackend).
  int get address => _address;
  Future<void> _tail = Future.value();
  bool _closed = false;

  // persistent frame worker (spawned on the first frame)
  Isolate? _worker;
  SendPort? _toWorker;
  ReceivePort? _replies;
  ReceivePort? _exits;
  final Map<int, Completer<List>> _pendingFrames = {};
  int _frameId = 0;

  static bool get isAvailable => DappleBindings.instanceOrNull != null;
  /// Engine scheduling knobs: [threadLimit] threads per job (0 = all), ADPF [targetFrameMs] (0 = off).
  static void configurePerf({int threadLimit = 0, int targetFrameMs = 0}) =>
      DappleBindings.instanceOrNull?.perfConfigure?.call(threadLimit, targetFrameMs);

  /// Thermal headroom forecast (0 cool .. 1 = throttling threshold); negative when the platform has none.
  static double thermalHeadroom([int forecastSeconds = 10]) =>
      DappleBindings.instanceOrNull?.thermalHeadroom?.call(forecastSeconds) ?? -1;

  /// Core topology / ADPF availability reported by the engine.
  static Map<String, dynamic>? perfInfo() {
    final b = DappleBindings.instanceOrNull;
    final f = b?.perfInfoJson;
    if (b == null || f == null) return null;
    final p = f();
    try {
      return _decodeObj(p.toDartString());
    } finally {
      b.freeString(p);
    }
  }

  static String get version => DappleBindings.instanceOrNull?.version().toDartString() ?? 'n/a';

  /// Decode [path]. [full] = full-resolution demosaic; [longEdge] > 0 downsizes once after decoding.
  static Future<DappleSession> open(String path, {bool full = false, int longEdge = 2048, bool background = false}) async {
    final addr = await Isolate.run(() {
      final b = _b();
      final cp = path.toNativeUtf8();
      if (background) b.threadBackground?.call(1);
      try {
        final s = b.sessionOpen(cp, full ? dappleQualityFull : dappleQualityPreview, longEdge);
        if (s == ffi.nullptr) throw DappleException(_lastError(b));
        return s.address;
      } finally {
        if (background) b.threadBackground?.call(0);
        malloc.free(cp);
      }
    });
    return DappleSession._(addr);
  }

  Future<T> _serial<T>(Future<T> Function() op) {
    final c = Completer<T>();
    _tail = _tail.then((_) async {
      try {
        c.complete(await op());
      } catch (e, st) {
        c.completeError(e, st);
      }
    });
    return c.future;
  }

  /// Metadata JSON of the opened file.
  Future<Map<String, dynamic>> info() => _serial(() => _infoIso(_address));

  // NOTE: closures handed to Isolate.run must be created in static methods; a closure created in an
  // instance method captures `this` (and its Future fields), which cannot be sent to an isolate.
  static Future<Map<String, dynamic>> _infoIso(int a) => Isolate.run(() {
        final b = _b();
        return _decodeObj(_takeString(b, b.sessionInfoJson(ffi.Pointer.fromAddress(a)))) ?? {};
      });

  /// {"correction": {...EditParams...}, "stats": {...}, "notes": [...]}
  Future<Map<String, dynamic>> auto(double strength) => _serial(() => _autoIso(_address, strength));

  static Future<Map<String, dynamic>> _autoIso(int a, double strength) => Isolate.run(() {
        final b = _b();
        final r = _decodeObj(_takeString(b, b.sessionAutoJson(ffi.Pointer.fromAddress(a), strength)));
        if (r == null) throw DappleException(_lastError(b));
        return r;
      });

  /// Tonal analysis ({"histogram":[64],"p5","p50","p95","nearest","key","range",...}) of the photo as
  /// rendered with [params] (null = untouched). Returns null when the library predates the symbol.
  Future<Map<String, dynamic>?> analyze([Map<String, dynamic>? params]) {
    final pj = params == null ? null : jsonEncode(params);
    return _serial(() => _analyzeIso(_address, pj));
  }

  static Future<Map<String, dynamic>?> _analyzeIso(int a, String? pj) => Isolate.run(() {
        final b = _b();
        final ptr = ffi.Pointer<DappleSessionStruct>.fromAddress(a);
        if (pj == null || b.sessionAnalyzeParamsJson == null) {
          final f = b.sessionAnalyzeJson;
          if (f == null) return null;
          return _decodeObj(_takeString(b, f(ptr)));
        }
        final pp = pj.toNativeUtf8();
        try {
          return _decodeObj(_takeString(b, b.sessionAnalyzeParamsJson!(ptr, pp)));
        } finally {
          malloc.free(pp);
        }
      });

  /// The nine style previews (order of [looksCatalog]) rendered on top of [params]. Null if unsupported.
  Future<List<RenderedImage>?> lookPreviews(Map<String, dynamic> params, {int longEdge = 256}) {
    final pj = jsonEncode(params);
    return _serial(() => _previewsIso(_address, pj, longEdge, null));
  }

  static Future<List<RenderedImage>?> _previewsIso(int a, String pj, int longEdge, double? ts) =>
      Isolate.run(() => _previews(a, pj, longEdge, ts));

  /// The nine tonal-class previews, order: low-short, low-mid, low-long, mid-short ... high-long.
  /// [strength] < 0 means max(60, toneStrength of params) (engine rule). Null if unsupported.
  Future<List<RenderedImage>?> tonePreviews(Map<String, dynamic> params, {int longEdge = 200, double strength = -1}) {
    final pj = jsonEncode(params);
    return _serial(() => _previewsIso(_address, pj, longEdge, strength));
  }

  static List<RenderedImage>? _previews(int addr, String pj, int longEdge, double? toneStrength) {
    final b = _b();
    final look = b.sessionLookPreviews, tone = b.sessionTonePreviews;
    if ((toneStrength == null ? look : tone) == null) return null;
    final pp = pj.toNativeUtf8();
    final out = calloc<DappleImageStruct>(9);
    b.threadBackground?.call(1); // thumbnails never compete with interactive frames
    try {
      final s = ffi.Pointer<DappleSessionStruct>.fromAddress(addr);
      final rc = toneStrength == null ? look!(s, pp, longEdge, out) : tone!(s, pp, longEdge, toneStrength, out);
      if (rc != dappleOk) throw DappleException(_lastError(b));
      final res = <RenderedImage>[];
      for (var i = 0; i < 9; i++) {
        final img = out + i;
        final w = img.ref.width, h = img.ref.height;
        res.add(RenderedImage(w, h, Uint8List.fromList(img.ref.rgba.asTypedList(w * h * 4))));
        b.freeImage(img);
      }
      return res;
    } finally {
      b.threadBackground?.call(0);
      malloc.free(pp);
      calloc.free(out);
    }
  }

  /// The JPEG embedded in a RAW file (the camera's own rendering), upright, scaled to [longEdge].
  /// Fast (no demosaic); null when the file has none or the library predates the symbol.
  static Future<RenderedImage?> extractPreview(String path, {int longEdge = 1280}) => Isolate.run(() {
        final b = _b();
        final f = b.extractPreview;
        if (f == null) return null;
        final cp = path.toNativeUtf8();
        final out = calloc<DappleImageStruct>();
        try {
          if (f(cp, longEdge, out) != dappleOk) return null;
          final w = out.ref.width, h = out.ref.height;
          final bytes = Uint8List.fromList(out.ref.rgba.asTypedList(w * h * 4));
          b.freeImage(out);
          return RenderedImage(w, h, bytes);
        } finally {
          malloc.free(cp);
          calloc.free(out);
        }
      });

  /// {"looks":[{id,name,tags,category,padX,padY}...]} from the engine, or null.
  static Map<String, dynamic>? looksCatalog() {
    final f = DappleBindings.instanceOrNull?.looksJson;
    if (f == null) return null;
    return _decodeObj(_takeString(_b(), f()));
  }

  /// Engine slider schema (keys / ranges / defaults), or null. Used to cross-check the UI table.
  static Map<String, dynamic>? paramsSchema() {
    final f = DappleBindings.instanceOrNull?.paramsSchemaJson;
    if (f == null) return null;
    return _decodeObj(_takeString(_b(), f()));
  }

  /// Installs the film pack (core/data/film/dapple.dfp, read from the app's assets) process-wide; the engine
  /// copies the bytes. False when the library has no film support or rejects the pack.
  static bool loadFilmPack(Uint8List bytes) {
    final f = DappleBindings.instanceOrNull?.filmLoadPackMemory;
    if (f == null || bytes.isEmpty) return false;
    final p = malloc<ffi.Uint8>(bytes.length);
    try {
      p.asTypedList(bytes.length).setAll(0, bytes);
      return f(p.cast<ffi.Void>(), bytes.length) == dappleOk;
    } finally {
      malloc.free(p);
    }
  }

  /// `{"loaded":bool,"stocks":[{"id","name","type":"negative|slide|paper"}]}` or null.
  static String? filmListJson() {
    final f = DappleBindings.instanceOrNull?.filmListJson;
    return f == null ? null : _takeString(_b(), f());
  }

  /// `{"<stock id>":["rrggbb" x3]}` the engine's rendering of three test patches per stock, in the order skin, sky, shadow (a
  /// pure function of the pack; null without native support). Takes a few seconds: call from a background isolate
  /// ([filmSwatchesJsonIsolate]). An engine without the band function gives its older grey / skin / sky table, reordered.
  static String? filmSwatchesJson() {
    final b = DappleBindings.instanceOrNull;
    final bands = b?.filmSwatchBandsJson;
    if (bands != null) {
      final s = _takeString(_b(), bands());
      if (s != null && s.length > 2) return s;
    }
    final f = b?.filmSwatchesJson;
    final old = f == null ? null : _takeString(_b(), f());
    if (old == null) return null;
    try {
      final m = jsonDecode(old) as Map<String, dynamic>;
      return jsonEncode({
        for (final e in m.entries) e.key: [(e.value as List)[1], (e.value as List)[2], (e.value as List)[0]]
      });
    } catch (_) {
      return null;
    }
  }

  /// [filmSwatchesJson] without blocking the UI isolate (the film pack is process-wide, the worker sees it).
  static Future<String?> filmSwatchesJsonIsolate() => Isolate.run(filmSwatchesJson);

  /// The licence line the About screen must show ("" without a pack).
  static String filmAttribution() {
    final f = DappleBindings.instanceOrNull?.filmAttribution;
    return f == null ? '' : (_takeString(_b(), f()) ?? '');
  }

  static bool get hasAnalyze => DappleBindings.instanceOrNull?.sessionAnalyzeJson != null;

  Future<RenderedImage> render(Map<String, dynamic> params, {String? lutPath, int longEdge = 0}) {
    final pj = jsonEncode(params);
    return _serial(() => _renderIso(_address, pj, lutPath, longEdge));
  }

  static Future<RenderedImage> _renderIso(int a, String pj, String? lutPath, int longEdge) => Isolate.run(() {
        final b = _b();
        final pp = pj.toNativeUtf8();
        final lp = (lutPath ?? '').toNativeUtf8();
        final img = calloc<DappleImageStruct>();
        try {
          final rc = b.sessionRender(ffi.Pointer.fromAddress(a), pp, lp, longEdge, img);
          if (rc != dappleOk) throw DappleException(_lastError(b));
          final w = img.ref.width, h = img.ref.height;
          final bytes = Uint8List.fromList(img.ref.rgba.asTypedList(w * h * 4));
          b.freeImage(img);
          return RenderedImage(w, h, bytes);
        } finally {
          malloc.free(pp);
          malloc.free(lp);
          calloc.free(img);
        }
      });

  static bool get hasFrames => DappleBindings.instanceOrNull?.sessionRenderFrame != null;

  /// One interactive frame: pixels + tonal analysis of the same render in a single native call, run on
  /// this session's persistent worker isolate (no isolate spawn per call, pixels are not copied across
  /// isolates: the result holds a native pointer, see [NativeFrame]).
  ///
  /// [longEdge]: long edge of the whole picture (0 = session size). [interactive]: drag quality (cheaper
  /// neighbourhood stages). [region]: normalised rect of the picture to render at [regionLongEdge]
  /// pixels (zoom detail). [bakeSize] 0 = engine default.
  Future<NativeFrame> renderFrame(Map<String, dynamic> params,
      {String? lutPath,
      int longEdge = 0,
      bool interactive = false,
      bool analysis = true,
      ui.Rect? region,
      int regionLongEdge = 0,
      int bakeSize = 0,
      bool background = false,
      bool exact = false,
      String? activeParam}) {
    final pj = jsonEncode(params);
    final flags = (interactive ? dappleFrameInteractive : 0) | (analysis ? dappleFrameAnalysis : 0) | (exact ? dappleFrameExact : 0) | (background ? dappleFrameBackground : 0);
    return _serial(() => _frame(pj, lutPath, longEdge, flags, bakeSize, region, regionLongEdge, activeParam));
  }

  Future<void> _ensureWorker() async {
    if (_toWorker != null) return;
    final replies = ReceivePort();
    final exits = ReceivePort();
    final init = ReceivePort();
    _replies = replies;
    _exits = exits;
    replies.listen((dynamic m) {
      final r = m as List;
      _pendingFrames.remove(r[0] as int)?.complete(r);
    });
    exits.listen((_) {
      for (final c in _pendingFrames.values) {
        c.completeError(DappleException('渲染线程已退出'));
      }
      _pendingFrames.clear();
      _toWorker = null;
      _worker = null;
    });
    _worker = await Isolate.spawn(_frameWorkerMain, init.sendPort, onExit: exits.sendPort, errorsAreFatal: true);
    _toWorker = await init.first as SendPort;
  }

  Future<NativeFrame> _frame(String pj, String? lutPath, int longEdge, int flags, int bakeSize, ui.Rect? region, int regionLongEdge, String? activeParam) async {
    await _ensureWorker();
    final id = ++_frameId;
    final c = Completer<List>();
    _pendingFrames[id] = c;
    _toWorker!.send([
      _replies!.sendPort,
      id,
      _address,
      pj,
      lutPath,
      longEdge,
      flags,
      bakeSize,
      region?.left ?? 0.0,
      region?.top ?? 0.0,
      region?.right ?? 0.0,
      region?.bottom ?? 0.0,
      regionLongEdge,
      activeParam,
    ]);
    final r = await c.future;
    if (r[1] != 'ok') throw DappleException(r[2] as String);
    return NativeFrame._(r[2] as int, r[3] as int, r[4] as int, r[5] as String?, (r[6] as num?)?.toDouble() ?? 0);
  }

  void _shutdownWorker() {
    final to = _toWorker;
    if (to != null && _replies != null) to.send([_replies!.sendPort, 0, 'quit']);
    _worker?.kill(priority: Isolate.beforeNextEvent);
    _replies?.close();
    _exits?.close();
    _toWorker = null;
    _worker = null;
  }

  Future<void> exportJpeg(Map<String, dynamic> params,
      {String? lutPath, int longEdge = 0, int quality = 92, required String outPath}) {
    final pj = jsonEncode(params);
    return _serial(() => _exportIso(_address, pj, lutPath, longEdge, quality, outPath));
  }

  static Future<void> _exportIso(int a, String pj, String? lut, int longEdge, int quality, String outPath) =>
      Isolate.run(() => _exportNative(a, pj, lut, longEdge, quality, outPath));

  /// Latest-wins cancellation (`dapple_session_cancel`): the render in flight on this session (any isolate) returns early
  /// with a "cancelled" error; renders submitted afterwards are unaffected. Synchronous and lock-free. False when the
  /// session is closed or the library predates the call.
  bool cancel() {
    final f = DappleBindings.instanceOrNull?.sessionCancel;
    if (f == null || _closed) return false;
    return f(ffi.Pointer.fromAddress(_address)) == dappleOk;
  }

  Future<void> close() => _serial(() async {
        if (_closed) return;
        _closed = true;
        _shutdownWorker();
        await _closeIso(_address);
      });

  static Future<void> _closeIso(int a) => Isolate.run(() => _b().sessionClose(ffi.Pointer.fromAddress(a)));

  // ---- one-shot helpers (no session object) ----

  /// Open (full quality by default), export a JPEG, close. Used for lazy full-quality save/share.
  static Future<void> exportOneShot(String srcPath, Map<String, dynamic> params,
      {String? lutPath,
      bool full = true,
      int decodeLongEdge = 0,
      int outLongEdge = 0,
      int quality = 92,
      required String outPath}) {
    final pj = jsonEncode(params);
    return Isolate.run(() {
      final b = _b();
      final cp = srcPath.toNativeUtf8();
      final s = b.sessionOpen(cp, full ? dappleQualityFull : dappleQualityPreview, decodeLongEdge);
      malloc.free(cp);
      if (s == ffi.nullptr) throw DappleException(_lastError(b));
      try {
        _exportNative(s.address, pj, lutPath, outLongEdge, quality, outPath);
      } finally {
        b.sessionClose(s);
      }
    });
  }

  static void _exportNative(int addr, String paramsJson, String? lut, int longEdge, int quality, String outPath) {
    final b = _b();
    final pp = paramsJson.toNativeUtf8();
    final lp = (lut ?? '').toNativeUtf8();
    final op = outPath.toNativeUtf8();
    try {
      final rc = b.sessionExportJpeg(ffi.Pointer.fromAddress(addr), pp, lp, longEdge, quality, op);
      if (rc != dappleOk) throw DappleException(_lastError(b));
    } finally {
      malloc.free(pp);
      malloc.free(lp);
      malloc.free(op);
    }
  }

  static Future<Map<String, dynamic>?> probe(String path) => Isolate.run(() {
        final b = _b();
        final cp = path.toNativeUtf8();
        try {
          return _decodeObj(_takeString(b, b.probeJson(cp)));
        } finally {
          malloc.free(cp);
        }
      });

  /// {"name","params","unsupported"} from a Lightroom .xmp preset.
  static Future<Map<String, dynamic>?> xmpToJson(String xmpPath) => Isolate.run(() {
        final b = _b();
        final cp = xmpPath.toNativeUtf8();
        try {
          final r = _decodeObj(_takeString(b, b.xmpToJson(cp)));
          if (r == null) throw DappleException(_lastError(b));
          return r;
        } finally {
          malloc.free(cp);
        }
      });

  static bool isSupportedFile(String path) {
    final b = DappleBindings.instanceOrNull;
    if (b == null) return false;
    final cp = path.toNativeUtf8();
    try {
      return b.isSupportedFile(cp) != 0;
    } finally {
      malloc.free(cp);
    }
  }
}
