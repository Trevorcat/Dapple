import 'dart:async';
import 'dart:convert';
import 'dart:ffi' as ffi;
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'dapple_bindings.dart';

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

/// Friendly wrapper around a native DappleSession.
///
/// Every heavy call runs in a short-lived background isolate (Isolate.run);
/// the native session pointer travels as an int address. Calls on one session
/// are serialised (the C API forbids concurrent use of a single session).
class DappleSession {
  DappleSession._(this._address);
  final int _address;
  Future<void> _tail = Future.value();
  bool _closed = false;

  static bool get isAvailable => DappleBindings.instanceOrNull != null;
  static String get version => DappleBindings.instanceOrNull?.version().toDartString() ?? 'n/a';

  /// Decode [path]. [full] = full-resolution demosaic; [longEdge] > 0 downsizes once after decoding.
  static Future<DappleSession> open(String path, {bool full = false, int longEdge = 2048}) async {
    final addr = await Isolate.run(() {
      final b = _b();
      final cp = path.toNativeUtf8();
      try {
        final s = b.sessionOpen(cp, full ? dappleQualityFull : dappleQualityPreview, longEdge);
        if (s == ffi.nullptr) throw DappleException(_lastError(b));
        return s.address;
      } finally {
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

  Future<void> exportJpeg(Map<String, dynamic> params,
      {String? lutPath, int longEdge = 0, int quality = 92, required String outPath}) {
    final pj = jsonEncode(params);
    return _serial(() => _exportIso(_address, pj, lutPath, longEdge, quality, outPath));
  }

  static Future<void> _exportIso(int a, String pj, String? lut, int longEdge, int quality, String outPath) =>
      Isolate.run(() => _exportNative(a, pj, lut, longEdge, quality, outPath));

  Future<void> close() => _serial(() async {
        if (_closed) return;
        _closed = true;
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
