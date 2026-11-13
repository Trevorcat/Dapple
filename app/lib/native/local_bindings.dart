// ignore_for_file: library_private_types_in_public_api
// dart:ffi bindings for the local-adjustment calls of core/include/dapple/dapple_c.h (docs/engine/pipeline-architecture.md section
// 40.5, additive): dapple_session_mask_preview, dapple_session_map_point, dapple_session_mask_sample_json, dapple_mask_raster_set /
// _has / _remove. Kept in their own file (the AI-mask bindings live in another one). Every call that touches a session runs in a
// background isolate: the engine locks the session for them, so a call can wait for a frame in flight and must not block the UI.
import 'dart:convert';
import 'dart:ffi' as ffi;
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import '../editor/mask_geometry.dart';
import 'dapple_bindings.dart';

typedef _MaskPreview = ffi.Int32 Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Int32, ffi.Int32, ffi.Int32, ffi.Pointer<DappleImageStruct>);
typedef _MaskPreviewD = int Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, int, int, int, ffi.Pointer<DappleImageStruct>);
typedef _MapPoint = ffi.Int32 Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Int32, ffi.Float, ffi.Float, ffi.Int32, ffi.Pointer<ffi.Float>, ffi.Pointer<ffi.Float>);
typedef _MapPointD = int Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, int, double, double, int, ffi.Pointer<ffi.Float>, ffi.Pointer<ffi.Float>);
typedef _MaskSample = ffi.Pointer<Utf8> Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Float, ffi.Float);
typedef _MaskSampleD = ffi.Pointer<Utf8> Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, double, double);
typedef _RasterSet = ffi.Int32 Function(ffi.Pointer<Utf8>, ffi.Int32, ffi.Int32, ffi.Pointer<ffi.Uint8>, ffi.Int32);
typedef _RasterSetD = int Function(ffi.Pointer<Utf8>, int, int, ffi.Pointer<ffi.Uint8>, int);
typedef _RasterId = ffi.Int32 Function(ffi.Pointer<Utf8>);
typedef _RasterIdD = int Function(ffi.Pointer<Utf8>);
typedef _FreeImg = ffi.Void Function(ffi.Pointer<DappleImageStruct>);
typedef _FreeImgD = void Function(ffi.Pointer<DappleImageStruct>);
typedef _FreeStr = ffi.Void Function(ffi.Pointer<Utf8>);
typedef _FreeStrD = void Function(ffi.Pointer<Utf8>);

class LocalBindings {
  LocalBindings(ffi.DynamicLibrary lib)
      : maskPreview = lib.providesSymbol('dapple_session_mask_preview') ? lib.lookupFunction<_MaskPreview, _MaskPreviewD>('dapple_session_mask_preview') : null,
        mapPoint = lib.providesSymbol('dapple_session_map_point') ? lib.lookupFunction<_MapPoint, _MapPointD>('dapple_session_map_point') : null,
        maskSample = lib.providesSymbol('dapple_session_mask_sample_json') ? lib.lookupFunction<_MaskSample, _MaskSampleD>('dapple_session_mask_sample_json') : null,
        rasterSet = lib.providesSymbol('dapple_mask_raster_set') ? lib.lookupFunction<_RasterSet, _RasterSetD>('dapple_mask_raster_set') : null,
        rasterHas = lib.providesSymbol('dapple_mask_raster_has') ? lib.lookupFunction<_RasterId, _RasterIdD>('dapple_mask_raster_has') : null,
        rasterRemove = lib.providesSymbol('dapple_mask_raster_remove') ? lib.lookupFunction<_RasterId, _RasterIdD>('dapple_mask_raster_remove') : null,
        freeImage = lib.lookupFunction<_FreeImg, _FreeImgD>('dapple_free_image'),
        freeString = lib.lookupFunction<_FreeStr, _FreeStrD>('dapple_free_string');

  final _MaskPreviewD? maskPreview;
  final _MapPointD? mapPoint;
  final _MaskSampleD? maskSample;
  final _RasterSetD? rasterSet;
  final _RasterIdD? rasterHas;
  final _RasterIdD? rasterRemove;
  final _FreeImgD freeImage;
  final _FreeStrD freeString;

  static LocalBindings? _instance;

  /// Null when the native library is missing (tests, demo mode).
  static LocalBindings? get instanceOrNull {
    if (_instance != null) return _instance;
    if (DappleBindings.instanceOrNull == null) return null;
    try {
      _instance = LocalBindings(DappleBindings.openLibrary());
    } catch (_) {}
    return _instance;
  }

  /// The library has the local-adjustment calls.
  static bool get available {
    final b = instanceOrNull;
    return b != null && b.maskPreview != null && b.mapPoint != null;
  }
}

/// The red overlay of one area: premultiplied RGBA, red with alpha = 0.45 x mask, ready for decodeImageFromPixels.
class MaskOverlayPixels {
  const MaskOverlayPixels(this.width, this.height, this.rgba);
  final int width, height;
  final Uint8List rgba;
}

/// What the range components see at a photo point: L* 0..100, OKLab hue 0..360, saturation (the satMin units).
class MaskSample {
  const MaskSample(this.l, this.hue, this.sat);
  final double l, hue, sat;
}

/// The local-adjustment calls on a session (by its native address).
class LocalNative {
  static const int _probe = 2048;

  /// The frame <-> photo map of [paramsJson] (its geometry) and the photo's aspect; null when the library lacks the calls or fails.
  static Future<FrameMap?> frameMap(int session, String paramsJson, String strippedJson) {
    if (!LocalBindings.available || session == 0) return Future.value(null);
    return Isolate.run(() => _frameMapNative(session, paramsJson, strippedJson));
  }

  static FrameMap? _frameMapNative(int session, String pj, String sj) {
    final b = LocalBindings.instanceOrNull!;
    final s = ffi.Pointer<DappleSessionStruct>.fromAddress(session);
    final p = pj.toNativeUtf8(), q = sj.toNativeUtf8();
    final u = calloc<ffi.Float>(), v = calloc<ffi.Float>();
    try {
      (double, double)? map(ffi.Pointer<Utf8> js, double x, double y, int dir) {
        if (b.mapPoint!(s, js, _probe, x, y, dir, u, v) != dappleOk) return null;
        return (u.value, v.value);
      }

      final o = map(p, 0, 0, 0), x = map(p, _probe.toDouble(), 0, 0), y = map(p, 0, _probe.toDouble(), 0);
      final c = map(p, 0.5, 0.5, 1); // the photo centre is the frame centre (the engine's crops are centred)
      final pc = map(q, 0.5, 0.5, 1); // without geometry: the photo's own size
      if (o == null || x == null || y == null || c == null || pc == null || c.$1 <= 0 || c.$2 <= 0 || pc.$2 <= 0) return null;
      return FrameMap.fromProbes(
        u0: o.$1,
        v0: o.$2,
        ux: x.$1,
        vx: x.$2,
        uy: y.$1,
        vy: y.$2,
        probe: _probe.toDouble(),
        w: c.$1 * 2,
        h: c.$2 * 2,
        photoAspect: pc.$1 / pc.$2,
      );
    } finally {
      malloc.free(p);
      malloc.free(q);
      calloc.free(u);
      calloc.free(v);
    }
  }

  /// The mask of item [index] of the "local" list in [paramsJson], over the output frame, long edge [longEdge], tinted red.
  static Future<MaskOverlayPixels?> overlay(int session, String paramsJson, int index, int longEdge) {
    if (!LocalBindings.available || session == 0) return Future.value(null);
    return Isolate.run(() => _overlayNative(session, paramsJson, index, longEdge));
  }

  static MaskOverlayPixels? _overlayNative(int session, String pj, int index, int longEdge) {
    final b = LocalBindings.instanceOrNull!;
    final p = pj.toNativeUtf8();
    final img = calloc<DappleImageStruct>();
    try {
      final rc = b.maskPreview!(ffi.Pointer.fromAddress(session), p, index, longEdge, 0, img);
      if (rc != dappleOk || img.ref.rgba == ffi.nullptr) return null;
      final w = img.ref.width, h = img.ref.height;
      final src = img.ref.rgba.asTypedList(w * h * 4);
      final out = Uint8List(w * h * 4);
      for (var i = 0; i < w * h; i++) {
        final m = src[i * 4];
        if (m == 0) continue;
        // premultiplied (decodeImageFromPixels takes premultiplied alpha): red #FF3B30 at 0.45 x mask
        final a = (m * 115) >> 8;
        out[i * 4] = a;
        out[i * 4 + 1] = (a * 0x3B) >> 8;
        out[i * 4 + 2] = (a * 0x30) >> 8;
        out[i * 4 + 3] = a;
      }
      b.freeImage(img);
      return MaskOverlayPixels(w, h, out);
    } finally {
      malloc.free(p);
      calloc.free(img);
    }
  }

  /// L*, hue and saturation at photo point (u, v) as the range components see it.
  static Future<MaskSample?> sample(int session, String paramsJson, double u, double v) {
    final bs = LocalBindings.instanceOrNull;
    if (bs?.maskSample == null || session == 0) return Future.value(null);
    return Isolate.run(() {
      final b = LocalBindings.instanceOrNull!;
      final p = paramsJson.toNativeUtf8();
      try {
        final r = b.maskSample!(ffi.Pointer.fromAddress(session), p, u, v);
        if (r == ffi.nullptr) return null;
        final text = r.toDartString();
        b.freeString(r);
        final j = jsonDecode(text);
        if (j is! Map) return null;
        double n(Object? x) => x is num ? x.toDouble() : 0;
        return MaskSample(n(j['L']), n(j['hue']), n(j['sat']));
      } finally {
        malloc.free(p);
      }
    });
  }

  /// Raster planes (AI masks, later round): the process-wide store by id.
  static bool rasterSet(String id, int w, int h, Uint8List bytes) {
    final f = LocalBindings.instanceOrNull?.rasterSet;
    if (f == null) return false;
    final idp = id.toNativeUtf8();
    final buf = malloc<ffi.Uint8>(bytes.length);
    try {
      buf.asTypedList(bytes.length).setAll(0, bytes);
      return f(idp, w, h, buf, w) == dappleOk;
    } finally {
      malloc.free(idp);
      malloc.free(buf);
    }
  }

  static bool rasterHas(String id) => _rasterId(LocalBindings.instanceOrNull?.rasterHas, id) == 1;
  static bool rasterRemove(String id) => _rasterId(LocalBindings.instanceOrNull?.rasterRemove, id) == dappleOk;

  static int _rasterId(_RasterIdD? f, String id) {
    if (f == null) return -1;
    final p = id.toNativeUtf8();
    try {
      return f(p);
    } finally {
      malloc.free(p);
    }
  }
}
