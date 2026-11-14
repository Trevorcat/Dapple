// dart:ffi bindings of the engine's memory calls (dapple_c.h, docs/engine/pipeline-architecture.md 42.9): device class, report, trim.
// Optional symbols: an older engine without them makes every call a no-op.
import 'dart:convert';
import 'dart:ffi' as ffi;
import 'dart:isolate';

import 'package:ffi/ffi.dart';

import 'dapple_bindings.dart';

typedef _Configure = ffi.Int32 Function(ffi.Pointer<Utf8>);
typedef _ConfigureD = int Function(ffi.Pointer<Utf8>);
typedef _Report = ffi.Pointer<Utf8> Function();
typedef _Trim = ffi.Int64 Function(ffi.Int32);
typedef _TrimD = int Function(int);
typedef _FreeStr = ffi.Void Function(ffi.Pointer<Utf8>);
typedef _FreeStrD = void Function(ffi.Pointer<Utf8>);

class MemoryBindings {
  MemoryBindings._(ffi.DynamicLibrary lib)
      : _configure = lib.providesSymbol('dapple_memory_configure') ? lib.lookupFunction<_Configure, _ConfigureD>('dapple_memory_configure') : null,
        _report = lib.providesSymbol('dapple_memory_report_json') ? lib.lookupFunction<_Report, _Report>('dapple_memory_report_json') : null,
        _trim = lib.providesSymbol('dapple_memory_trim') ? lib.lookupFunction<_Trim, _TrimD>('dapple_memory_trim') : null,
        _intent = lib.providesSymbol('dapple_export_intent') ? lib.lookupFunction<_Configure, _ConfigureD>('dapple_export_intent') : null,
        _free = lib.lookupFunction<_FreeStr, _FreeStrD>('dapple_free_string');

  final _ConfigureD? _configure;
  final _Report? _report;
  final _TrimD? _trim;
  final _ConfigureD? _intent;
  final _FreeStrD _free;

  static MemoryBindings? _instance;
  static bool _failed = false;
  static MemoryBindings? get instanceOrNull {
    if (_instance != null || _failed) return _instance;
    try {
      _instance = MemoryBindings._(DappleBindings.openLibrary());
    } catch (_) {
      _failed = true;
    }
    return _instance;
  }

  /// The device's memory class (Android ActivityManager figures): sets the engine's export ceiling and cache budgets.
  static void configure(Map<String, Object?> device) {
    final f = instanceOrNull?._configure;
    if (f == null) return;
    final s = jsonEncode(device).toNativeUtf8();
    try {
      f(s);
    } finally {
      malloc.free(s);
    }
  }

  /// The edit of the export about to open a FULL session on this thread (the same isolate call): the engine takes its whole-frame / bands
  /// decision at the open with the film stock known.
  static void exportIntent(String paramsJson) {
    final f = instanceOrNull?._intent;
    if (f == null) return;
    final s = paramsJson.toNativeUtf8();
    try {
      f(s);
    } finally {
      malloc.free(s);
    }
  }

  /// What the engine holds per cache / pool (bytes); null when unavailable.
  static Map<String, dynamic>? report() {
    final b = instanceOrNull;
    final f = b?._report;
    if (b == null || f == null) return null;
    final p = f();
    if (p.address == 0) return null;
    try {
      return jsonDecode(p.toDartString()) as Map<String, dynamic>;
    } finally {
      b._free(p);
    }
  }

  /// Gives cached memory back (0 idle / editor closed, 1 pressure, 2 critical); bytes released. Runs off the UI isolate: a trim waits for the
  /// GPU frame in flight and returns the allocator's pages.
  static Future<int> trim(int level) async {
    if (instanceOrNull?._trim == null) return 0;
    return Isolate.run(() => instanceOrNull?._trim?.call(level) ?? 0);
  }
}
