import 'dart:convert';
import 'dart:ffi' as ffi;
import 'dart:isolate';

import 'package:ffi/ffi.dart';

import '../native/dapple_bindings.dart';

/// Progress / cancel memory shared with the native merge: the library writes the fraction into [cell]
/// from its worker thread while the isolate is blocked in the call, and polls [cancel].
class MergeHandle {
  MergeHandle()
      : cell = calloc<DappleProgressCell>(),
        cancel = calloc<ffi.Int32>();
  final ffi.Pointer<DappleProgressCell> cell;
  final ffi.Pointer<ffi.Int32> cancel;
  bool _disposed = false;

  double get fraction => _disposed ? 1 : cell.ref.fraction.clamp(0.0, 1.0).toDouble();
  int get stage => _disposed ? 0 : cell.ref.stage;
  bool get cancelled => !_disposed && cancel.value != 0;
  bool get disposed => _disposed;

  /// Writes progress from Dart (fake engines, non-native stages).
  void report(double f) {
    if (!_disposed) cell.ref.fraction = f;
  }

  void requestCancel() {
    if (!_disposed) cancel.value = 1;
  }

  void dispose() {
    if (_disposed) return;
    _disposed = true;
    calloc.free(cell);
    calloc.free(cancel);
  }
}

class MergeRequest {
  MergeRequest({
    required this.kind,
    required this.paths,
    required this.handle,
    this.options = const {},
    this.previewJpeg,
    this.dngOut,
    this.previewEdge = 1024,
  });

  /// "hdr" | "pano"
  final String kind;
  final List<String> paths;
  final Map<String, dynamic> options;
  final MergeHandle handle;

  /// When set, the merged image is rendered (engine auto correction) into this JPEG.
  final String? previewJpeg;
  final int previewEdge;

  /// When set, the merged linear image is archived here as a linear DNG (the result becomes a normal photo).
  final String? dngOut;
}

class MergeOutcome {
  MergeOutcome.ok(this.report)
      : ok = true,
        cancelled = false,
        error = null;
  MergeOutcome.fail(this.error, {this.cancelled = false})
      : ok = false,
        report = null;
  final bool ok;
  final bool cancelled;
  final String? error;
  final Map<String, dynamic>? report;
}

/// The engine calls the HDR / panorama / Motion Photo tools need. Tests substitute a fake.
abstract class MergeEngine {
  bool get available;
  Future<MergeOutcome> merge(MergeRequest r);

  /// dapple_detect_sequences_json over [paths]; null on failure or cancel.
  Future<Map<String, dynamic>?> detect(List<String> paths, MergeHandle handle, {Map<String, dynamic> options});

  /// dapple_motion_photo_write: JPEG still + already trimmed MP4 -> out. Returns the info JSON.
  Future<Map<String, dynamic>> writeMotionPhoto(
      {required String jpeg, required String mp4, required String out, required double coverSec});

  /// dapple_motion_photo_from_video: stream-copy cutter fallback.
  Future<Map<String, dynamic>> motionFromVideo(
      {required String video,
      required String jpeg,
      required String out,
      required double start,
      required double duration,
      required double cover,
      required bool keepAudio});
}

class MergeException implements Exception {
  MergeException(this.message);
  final String message;
  @override
  String toString() => message;
}

DappleBindings _b() {
  final b = DappleBindings.instanceOrNull;
  if (b == null) throw MergeException('原生库 libdapple 未加载');
  return b;
}

String _err(DappleBindings b) => b.lastError().toDartString();

String? _take(DappleBindings b, ffi.Pointer<Utf8> p) {
  if (p == ffi.nullptr) return null;
  final s = p.toDartString();
  b.freeString(p);
  return s;
}

/// Allocates a `const char* const*` array; free with [_freeStrings].
ffi.Pointer<ffi.Pointer<Utf8>> _strings(List<String> l) {
  final arr = calloc<ffi.Pointer<Utf8>>(l.isEmpty ? 1 : l.length);
  for (var i = 0; i < l.length; i++) {
    arr[i] = l[i].toNativeUtf8();
  }
  return arr;
}

void _freeStrings(ffi.Pointer<ffi.Pointer<Utf8>> arr, int n) {
  for (var i = 0; i < n; i++) {
    malloc.free(arr[i]);
  }
  calloc.free(arr);
}

class NativeMergeEngine implements MergeEngine {
  const NativeMergeEngine();

  @override
  bool get available {
    final b = DappleBindings.instanceOrNull;
    return b != null && b.sessionOpenMerged != null && (b.mergeAvailable?.call() ?? 0) != 0;
  }

  @override
  Future<MergeOutcome> merge(MergeRequest r) => _mergeStatic(r.kind, r.paths, jsonEncode(r.options),
      r.handle.cell.address, r.handle.cancel.address, r.previewJpeg, r.dngOut, r.previewEdge);

  static Future<MergeOutcome> _mergeStatic(String kind, List<String> paths, String options, int cellAddr,
          int cancelAddr, String? previewJpeg, String? dngOut, int previewEdge) =>
      Isolate.run(() {
        final b = _b();
        final open = b.sessionOpenMerged;
        if (open == null) return MergeOutcome.fail('此版本的引擎不含合成功能');
        final arr = _strings(paths);
        final ck = kind.toNativeUtf8();
        final oj = options.toNativeUtf8();
        final rep = calloc<ffi.Pointer<Utf8>>();
        try {
          final s = open(arr, paths.length, ck, oj, b.progressToCell ?? ffi.nullptr, ffi.Pointer.fromAddress(cellAddr),
              ffi.Pointer.fromAddress(cancelAddr), rep);
          final report = _take(b, rep.value);
          if (s == ffi.nullptr) {
            final e = _err(b);
            return MergeOutcome.fail(e, cancelled: e == 'cancelled');
          }
          try {
            if (previewJpeg != null) {
              Map<String, dynamic> corr = {};
              final aj = _take(b, b.sessionAutoJson(s, 0.6));
              if (aj != null) {
                final m = jsonDecode(aj);
                if (m is Map && m['correction'] is Map) corr = (m['correction'] as Map).cast<String, dynamic>();
              }
              corr.remove('exposure');
              final pj = jsonEncode(corr).toNativeUtf8();
              final lp = ''.toNativeUtf8();
              final op = previewJpeg.toNativeUtf8();
              try {
                if (b.sessionExportJpeg(s, pj, lp, previewEdge, 88, op) != dappleOk) return MergeOutcome.fail(_err(b));
              } finally {
                malloc.free(pj);
                malloc.free(lp);
                malloc.free(op);
              }
            }
            if (dngOut != null) {
              final save = b.sessionSaveLinear;
              if (save == null) return MergeOutcome.fail('此版本的引擎不能保存合成结果');
              final op = dngOut.toNativeUtf8();
              final fp = 'dng'.toNativeUtf8();
              try {
                if (save(s, op, fp) != dappleOk) return MergeOutcome.fail(_err(b));
              } finally {
                malloc.free(op);
                malloc.free(fp);
              }
            }
          } finally {
            b.sessionClose(s);
          }
          Map<String, dynamic>? rm;
          if (report != null) {
            final d = jsonDecode(report);
            if (d is Map) rm = d.cast<String, dynamic>();
          }
          return MergeOutcome.ok(rm);
        } finally {
          _freeStrings(arr, paths.length);
          malloc.free(ck);
          malloc.free(oj);
          calloc.free(rep);
        }
      });

  @override
  Future<Map<String, dynamic>?> detect(List<String> paths, MergeHandle handle,
          {Map<String, dynamic> options = const {}}) =>
      _detectStatic(paths, jsonEncode(options), handle.cell.address, handle.cancel.address);

  static Future<Map<String, dynamic>?> _detectStatic(List<String> paths, String options, int cellAddr, int cancelAddr) =>
      Isolate.run(() {
        final b = _b();
        final f = b.detectSequences;
        if (f == null) return null;
        final arr = _strings(paths);
        final oj = options.toNativeUtf8();
        try {
          final p = f(arr, paths.length, oj, b.progressToCell ?? ffi.nullptr, ffi.Pointer.fromAddress(cellAddr),
              ffi.Pointer.fromAddress(cancelAddr));
          final s = _take(b, p);
          if (s == null) return null;
          final d = jsonDecode(s);
          return d is Map ? d.cast<String, dynamic>() : null;
        } finally {
          _freeStrings(arr, paths.length);
          malloc.free(oj);
        }
      });

  @override
  Future<Map<String, dynamic>> writeMotionPhoto(
          {required String jpeg, required String mp4, required String out, required double coverSec}) =>
      Isolate.run(() {
        final b = _b();
        final f = b.motionPhotoWrite;
        if (f == null) throw MergeException('此版本的引擎不含动态照片功能');
        final j = jpeg.toNativeUtf8(), m = mp4.toNativeUtf8(), o = out.toNativeUtf8();
        final info = calloc<ffi.Pointer<Utf8>>();
        try {
          final rc = f(j, m, o, coverSec, info);
          final s = _take(b, info.value);
          if (rc != dappleOk) throw MergeException(_err(b));
          return s == null ? <String, dynamic>{} : (jsonDecode(s) as Map).cast<String, dynamic>();
        } finally {
          malloc.free(j);
          malloc.free(m);
          malloc.free(o);
          calloc.free(info);
        }
      });

  @override
  Future<Map<String, dynamic>> motionFromVideo(
          {required String video,
          required String jpeg,
          required String out,
          required double start,
          required double duration,
          required double cover,
          required bool keepAudio}) =>
      Isolate.run(() {
        final b = _b();
        final f = b.motionPhotoFromVideo;
        if (f == null) throw MergeException('此版本的引擎不含动态照片功能');
        final v = video.toNativeUtf8(), j = jpeg.toNativeUtf8(), o = out.toNativeUtf8();
        final opt =
            jsonEncode({'start': start, 'duration': duration, 'coverTime': cover, 'keepAudio': keepAudio}).toNativeUtf8();
        final info = calloc<ffi.Pointer<Utf8>>();
        try {
          final rc = f(v, j, o, opt, info);
          final s = _take(b, info.value);
          if (rc != dappleOk) throw MergeException(_err(b));
          return s == null ? <String, dynamic>{} : (jsonDecode(s) as Map).cast<String, dynamic>();
        } finally {
          malloc.free(v);
          malloc.free(j);
          malloc.free(o);
          malloc.free(opt);
          calloc.free(info);
        }
      });
}
