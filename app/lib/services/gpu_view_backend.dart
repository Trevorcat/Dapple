import 'dart:async';
import 'dart:convert';
import 'dart:ffi' as ffi;
import 'dart:io';

import 'package:ffi/ffi.dart';
import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import '../native/dapple_bindings.dart';

/// Switch for the zero-copy display (kill switch for diagnostics: `--dart-define=DAPPLE_GPU_VIEW=0`).
const bool kGpuView = bool.fromEnvironment('DAPPLE_GPU_VIEW', defaultValue: true);

/// One frame the native render loop put on the texture. [status] is DAPPLE_VIEW_* of core/include/dapple/dapple_view.h.
class GpuFrameEvent {
  const GpuFrameEvent(this.seq, this.status, this.width, this.height, this.renderMs, this.presentMs, this.latencyMs);
  final int seq, status, width, height;
  final double renderMs, presentMs, latencyMs;
  bool get shown => status == 0;
}

typedef _FrameFnC = ffi.Void Function(ffi.Pointer<ffi.Void>, ffi.Int64, ffi.Int32, ffi.Int32, ffi.Int32, ffi.Double, ffi.Double, ffi.Double);

/// Zero-copy display of the editor picture (stage d, docs/engine/present.md): a Flutter texture (SurfaceProducer, Kotlin
/// plugin `DappleViewPlugin`) whose Surface is drawn by the native render loop (Vulkan swapchain). Dart only submits
/// parameter snapshots (`submit`) and receives small [GpuFrameEvent]s; no pixels are read back, decoded or sent
/// across isolates. The readback path in `EditorEngine` stays the permanent fallback: any failure ends in [disabled].
class GpuViewBackend with WidgetsBindingObserver {
  GpuViewBackend._(this._lib);
  static const MethodChannel _ch = MethodChannel('dapple/view');
  final ffi.DynamicLibrary _lib;

  late final _create = _lib.lookupFunction<ffi.Pointer<ffi.Void> Function(ffi.Pointer<ffi.Int32>), ffi.Pointer<ffi.Void> Function(ffi.Pointer<ffi.Int32>)>(
      'dapple_view_create');
  late final _destroy = _lib.lookupFunction<ffi.Void Function(ffi.Pointer<ffi.Void>), void Function(ffi.Pointer<ffi.Void>)>('dapple_view_destroy');
  late final _bind = _lib.lookupFunction<ffi.Void Function(ffi.Pointer<ffi.Void>, ffi.Pointer<ffi.Void>), void Function(ffi.Pointer<ffi.Void>, ffi.Pointer<ffi.Void>)>(
      'dapple_view_bind_session');
  late final _submit = _lib.lookupFunction<
      ffi.Int64 Function(ffi.Pointer<ffi.Void>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<DappleFrameRequestStruct>),
      int Function(ffi.Pointer<ffi.Void>, ffi.Pointer<Utf8>, ffi.Pointer<Utf8>, ffi.Pointer<DappleFrameRequestStruct>)>('dapple_view_submit');
  late final _setCb = _lib.lookupFunction<
      ffi.Void Function(ffi.Pointer<ffi.Void>, ffi.Pointer<ffi.NativeFunction<_FrameFnC>>, ffi.Pointer<ffi.Void>),
      void Function(ffi.Pointer<ffi.Void>, ffi.Pointer<ffi.NativeFunction<_FrameFnC>>, ffi.Pointer<ffi.Void>)>('dapple_view_set_callback');
  late final _paused = _lib.lookupFunction<ffi.Void Function(ffi.Pointer<ffi.Void>, ffi.Int32), void Function(ffi.Pointer<ffi.Void>, int)>('dapple_view_set_paused');
  late final _trim = _lib.lookupFunction<ffi.Void Function(ffi.Pointer<ffi.Void>, ffi.Int32), void Function(ffi.Pointer<ffi.Void>, int)>('dapple_view_trim');
  late final _takeAnalysis = _lib.lookupFunction<ffi.Pointer<Utf8> Function(ffi.Pointer<ffi.Void>), ffi.Pointer<Utf8> Function(ffi.Pointer<ffi.Void>)>(
      'dapple_view_take_analysis');
  late final _info = _lib.lookupFunction<ffi.Pointer<Utf8> Function(ffi.Pointer<ffi.Void>), ffi.Pointer<Utf8> Function(ffi.Pointer<ffi.Void>)>('dapple_view_info_json');
  late final _lastError = _lib.lookupFunction<ffi.Pointer<Utf8> Function(), ffi.Pointer<Utf8> Function()>('dapple_view_last_error');
  late final _freeString = _lib.lookupFunction<ffi.Void Function(ffi.Pointer<Utf8>), void Function(ffi.Pointer<Utf8>)>('dapple_free_string');

  ffi.Pointer<ffi.Void> _view = ffi.nullptr;
  ffi.NativeCallable<_FrameFnC>? _callable;
  int? _textureId;
  int texW = 0, texH = 0;
  int _boundSession = 0;
  bool _disposed = false;
  bool _resizing = false;

  /// Called for every frame event (UI isolate).
  void Function(GpuFrameEvent e)? onFrame;

  /// The texture to show with `Texture(textureId:)`; null before [start] / after failure.
  int? get textureId => _textureId;
  bool get ready => _textureId != null && !_disposed;

  /// Session address (DappleSession.address) the loop renders from.
  int get boundSession => _boundSession;

  /// True when this platform can have a GPU view at all (cheap pre-check).
  static bool get supported => kGpuView && !kIsWeb && Platform.isAndroid;

  /// Creates the native view + texture of [w] x [h] pixels and attaches them. Null when anything is unavailable (the caller keeps
  /// the readback path); the reason goes to the debug log.
  static Future<GpuViewBackend?> start(int w, int h, {void Function(GpuFrameEvent e)? onFrame}) async {
    if (!supported) return null;
    GpuViewBackend? b;
    try {
      if (await _ch.invokeMethod<bool>('available') != true) return null;
      final lib = DappleBindings.openLibrary();
      if (!lib.providesSymbol('dapple_view_create')) return null;
      b = GpuViewBackend._(lib);
      b.onFrame = onFrame;
      final opt = calloc<ffi.Int32>(2);
      opt[0] = 8;
      opt[1] = 0;
      b._view = b._create(opt);
      calloc.free(opt);
      if (b._view == ffi.nullptr) {
        debugPrint('gpu view: create failed: ${b._lastError().toDartString()}');
        return null;
      }
      b._callable = ffi.NativeCallable<_FrameFnC>.listener(b._onNative);
      b._setCb(b._view, b._callable!.nativeFunction, ffi.nullptr);
      final r = await _ch.invokeMapMethod<String, dynamic>('create', {'w': w, 'h': h});
      if (r == null || r['ok'] != true) {
        b.dispose();
        return null;
      }
      b._textureId = (r['id'] as num).toInt();
      b.texW = w;
      b.texH = h;
      final a = await _ch.invokeMapMethod<String, dynamic>('bind', {'id': b._textureId, 'view': b._view.address});
      if (a == null || a['ok'] != true) {
        debugPrint('gpu view: attach failed: ${a?['error']}');
        b.dispose();
        return null;
      }
      WidgetsBinding.instance.addObserver(b);
      return b;
    } catch (e) {
      debugPrint('gpu view unavailable: $e');
      b?.dispose();
      return null;
    }
  }

  void _onNative(ffi.Pointer<ffi.Void> user, int seq, int status, int w, int h, double r, double p, double lat) {
    if (_disposed) return;
    onFrame?.call(GpuFrameEvent(seq, status, w, h, r, p, lat));
  }

  /// Renders from this session (DappleSession.address). Blocks for at most one frame in flight; call with 0 before closing a session.
  void bindSession(int address) {
    if (_disposed || _view == ffi.nullptr) return;
    _boundSession = address;
    _bind(_view, ffi.Pointer<ffi.Void>.fromAddress(address));
  }

  /// Latest wins. Returns the sequence number.
  int submit(Map<String, dynamic> params, {required int longEdge, required int flags, int bakeSize = 0}) {
    if (_disposed || _view == ffi.nullptr) return 0;
    final pj = jsonEncode(params).toNativeUtf8();
    final rq = calloc<DappleFrameRequestStruct>();
    try {
      rq.ref
        ..size = ffi.sizeOf<DappleFrameRequestStruct>()
        ..longEdge = longEdge
        ..flags = flags
        ..bakeSize = bakeSize;
      return _submit(_view, pj, ffi.nullptr, rq);
    } finally {
      malloc.free(pj);
      calloc.free(rq);
    }
  }

  /// The analysis JSON of the newest frame that had one (once).
  Map<String, dynamic>? takeAnalysis() {
    if (_disposed || _view == ffi.nullptr) return null;
    final p = _takeAnalysis(_view);
    if (p == ffi.nullptr) return null;
    try {
      return jsonDecode(p.toDartString()) as Map<String, dynamic>;
    } catch (_) {
      return null;
    } finally {
      _freeString(p);
    }
  }

  /// Native loop / presenter diagnostics (frames, drops, wakeups, timings, thermal).
  Map<String, dynamic>? info() {
    if (_disposed || _view == ffi.nullptr) return null;
    final p = _info(_view);
    if (p == ffi.nullptr) return null;
    try {
      return jsonDecode(p.toDartString()) as Map<String, dynamic>;
    } catch (_) {
      return null;
    } finally {
      _freeString(p);
    }
  }

  /// The texture should follow the size of the frames it shows. Resizes the SurfaceProducer (the plugin re-attaches).
  Future<bool> resize(int w, int h) async {
    if (!ready || _resizing || (w == texW && h == texH)) return ready;
    _resizing = true;
    try {
      final r = await _ch.invokeMapMethod<String, dynamic>('resize', {'id': _textureId, 'w': w, 'h': h});
      if (r != null && r['ok'] == true) {
        texW = w;
        texH = h;
        return true;
      }
      debugPrint('gpu view: resize failed: ${r?['error']}');
      return false;
    } catch (e) {
      debugPrint('gpu view: resize failed: $e');
      return false;
    } finally {
      _resizing = false;
    }
  }

  void trim(int level) {
    if (!_disposed && _view != ffi.nullptr) _trim(_view, level);
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (_disposed || _view == ffi.nullptr) return;
    // Nothing is shown while the app is not visible: stop rendering (the newest snapshot is kept and drawn on resume)
    final visible = state == AppLifecycleState.resumed;
    _paused(_view, visible ? 0 : 1);
  }

  void dispose() {
    if (_disposed) return;
    _disposed = true;
    WidgetsBinding.instance.removeObserver(this);
    final id = _textureId;
    _textureId = null;
    if (_view != ffi.nullptr) {
      _setCb(_view, ffi.nullptr.cast<ffi.NativeFunction<_FrameFnC>>(), ffi.nullptr);
      _bind(_view, ffi.nullptr);
    }
    _callable?.close();
    _callable = null;
    if (id != null) {
      // detach the Surface before the native view goes away
      unawaited(_ch.invokeMethod('dispose', {'id': id}).catchError((_) {}).whenComplete(() {
        if (_view != ffi.nullptr) _destroy(_view);
        _view = ffi.nullptr;
      }));
    } else if (_view != ffi.nullptr) {
      _destroy(_view);
      _view = ffi.nullptr;
    }
  }
}
