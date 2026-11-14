// ignore_for_file: library_private_types_in_public_api
// dart:ffi bindings for the AI-mask calls of core/include/dapple/dapple_c.h (docs/engine/pipeline-architecture.md section 41, additive):
// dapple_ai_configure / _status_json / _job_json / _job_cancel / _plane_preview / _release and dapple_session_ai_*. UI-free. The calls that
// can wait (a tap runs the decoder, a synchronous ensure may recompute) run in a background isolate; the session is passed by its native address.
// The calls exist in every build of the library; without the runtimes or the models the status says available:false and starting a job fails.
import 'dart:convert';
import 'dart:ffi' as ffi;
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'dapple_bindings.dart';

typedef _Configure = ffi.Int32 Function(ffi.Pointer<Utf8>, ffi.Pointer<Utf8>);
typedef _ConfigureD = int Function(ffi.Pointer<Utf8>, ffi.Pointer<Utf8>);
typedef _Str0 = ffi.Pointer<Utf8> Function();
typedef _SessionStr = ffi.Pointer<Utf8> Function(ffi.Pointer<DappleSessionStruct>);
typedef _Start = ffi.Int32 Function(ffi.Pointer<DappleSessionStruct>, ffi.Int32, ffi.Int32, ffi.Int32);
typedef _StartD = int Function(ffi.Pointer<DappleSessionStruct>, int, int, int);
typedef _JobStr = ffi.Pointer<Utf8> Function(ffi.Int32);
typedef _JobStrD = ffi.Pointer<Utf8> Function(int);
typedef _JobVoid = ffi.Void Function(ffi.Int32);
typedef _JobVoidD = void Function(int);
typedef _Tap = ffi.Pointer<Utf8> Function(ffi.Pointer<DappleSessionStruct>, ffi.Float, ffi.Float, ffi.Int32);
typedef _TapD = ffi.Pointer<Utf8> Function(ffi.Pointer<DappleSessionStruct>, double, double, int);
typedef _PlanePreview = ffi.Int32 Function(ffi.Pointer<Utf8>, ffi.Int32, ffi.Pointer<DappleImageStruct>);
typedef _PlanePreviewD = int Function(ffi.Pointer<Utf8>, int, ffi.Pointer<DappleImageStruct>);
typedef _Missing = ffi.Pointer<Utf8> Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>);
typedef _Ensure = ffi.Int32 Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, ffi.Int32);
typedef _EnsureD = int Function(ffi.Pointer<DappleSessionStruct>, ffi.Pointer<Utf8>, int);
typedef _Release = ffi.Void Function(ffi.Int32);
typedef _ReleaseD = void Function(int);
typedef _FreeImg = ffi.Void Function(ffi.Pointer<DappleImageStruct>);
typedef _FreeImgD = void Function(ffi.Pointer<DappleImageStruct>);
typedef _FreeStr = ffi.Void Function(ffi.Pointer<Utf8>);
typedef _FreeStrD = void Function(ffi.Pointer<Utf8>);

/// Task bits of dapple_session_ai_start (DAPPLE_AI_*).
class AiTask {
  static const person = 1; // person parts + 人物 + 背景 (only when a person is found)
  static const faces = 2; // face features of up to maxFaces faces
  static const depth = 4;
  static const sky = 8; // classical prior, "天空 (粗略)"
  static const tapPrepare = 16; // run the tap encoder now
  static const all = person | faces | depth | sky;
}

class AiBindings {
  AiBindings(ffi.DynamicLibrary lib)
      : configure = lib.providesSymbol('dapple_ai_configure') ? lib.lookupFunction<_Configure, _ConfigureD>('dapple_ai_configure') : null,
        status = lib.providesSymbol('dapple_ai_status_json') ? lib.lookupFunction<_Str0, _Str0>('dapple_ai_status_json') : null,
        photoId = lib.providesSymbol('dapple_session_ai_photo_id') ? lib.lookupFunction<_SessionStr, _SessionStr>('dapple_session_ai_photo_id') : null,
        start = lib.providesSymbol('dapple_session_ai_start') ? lib.lookupFunction<_Start, _StartD>('dapple_session_ai_start') : null,
        jobJson = lib.providesSymbol('dapple_ai_job_json') ? lib.lookupFunction<_JobStr, _JobStrD>('dapple_ai_job_json') : null,
        jobCancel = lib.providesSymbol('dapple_ai_job_cancel') ? lib.lookupFunction<_JobVoid, _JobVoidD>('dapple_ai_job_cancel') : null,
        tap = lib.providesSymbol('dapple_session_ai_tap_json') ? lib.lookupFunction<_Tap, _TapD>('dapple_session_ai_tap_json') : null,
        planePreview = lib.providesSymbol('dapple_ai_plane_preview') ? lib.lookupFunction<_PlanePreview, _PlanePreviewD>('dapple_ai_plane_preview') : null,
        missing = lib.providesSymbol('dapple_session_ai_missing_json') ? lib.lookupFunction<_Missing, _Missing>('dapple_session_ai_missing_json') : null,
        ensure = lib.providesSymbol('dapple_session_ai_ensure') ? lib.lookupFunction<_Ensure, _EnsureD>('dapple_session_ai_ensure') : null,
        release = lib.providesSymbol('dapple_ai_release') ? lib.lookupFunction<_Release, _ReleaseD>('dapple_ai_release') : null,
        freeImage = lib.lookupFunction<_FreeImg, _FreeImgD>('dapple_free_image'),
        freeString = lib.lookupFunction<_FreeStr, _FreeStrD>('dapple_free_string');

  final _ConfigureD? configure;
  final _Str0? status;
  final _SessionStr? photoId;
  final _StartD? start;
  final _JobStrD? jobJson;
  final _JobVoidD? jobCancel;
  final _TapD? tap;
  final _PlanePreviewD? planePreview;
  final _Missing? missing;
  final _EnsureD? ensure;
  final _ReleaseD? release;
  final _FreeImgD freeImage;
  final _FreeStrD freeString;

  static AiBindings? _instance;

  /// Null when the native library is missing (tests, demo mode).
  static AiBindings? get instanceOrNull {
    if (_instance != null) return _instance;
    if (DappleBindings.instanceOrNull == null) return null;
    try {
      _instance = AiBindings(DappleBindings.openLibrary());
    } catch (_) {}
    return _instance;
  }

  /// The library has the AI-mask calls (older libraries: false; the service then reports unavailable).
  static bool get present {
    final b = AiBindings.instanceOrNull;
    return b != null && b.configure != null && b.status != null && b.start != null && b.jobJson != null;
  }
}

// ---- plain data ------------------------------------------------------------------------------------------------------------------------

/// dapple_ai_status_json.
class AiAvailability {
  const AiAvailability({
    this.compiled = false,
    this.available = false,
    this.person = false,
    this.faces = false,
    this.depth = false,
    this.tap = false,
    this.sky = false,
    this.tapEngine = '',
    this.backends = '',
    this.reason = '',
    this.residentBytes = 0,
  });
  static const none = AiAvailability(reason: 'native library without AI masks');

  final bool compiled, available, person, faces, depth, tap, sky;
  final String tapEngine, backends, reason;
  final int residentBytes;

  /// Bit mask of the AiTask values that can run.
  int get tasks => (person ? AiTask.person : 0) | (faces ? AiTask.faces : 0) | (depth ? AiTask.depth : 0) | (sky ? AiTask.sky : 0) | (tap ? AiTask.tapPrepare : 0);

  factory AiAvailability.fromJson(Map<String, Object?> j) {
    final t = j['tasks'] is Map ? Map<String, Object?>.from(j['tasks']! as Map) : const <String, Object?>{};
    bool b(Object? v) => v == true;
    return AiAvailability(
      compiled: b(j['compiled']),
      available: b(j['available']),
      person: b(t['person']),
      faces: b(t['faces']),
      depth: b(t['depth']),
      tap: b(t['tap']),
      sky: b(t['sky']),
      tapEngine: '${j['tap_engine'] ?? ''}',
      backends: '${j['backends'] ?? ''}',
      reason: '${j['reason'] ?? ''}',
      residentBytes: (j['resident_bytes'] as num?)?.toInt() ?? 0,
    );
  }
}

/// One raster plane the service made: [id] goes into a "raster" component (`ref`), [label] is the Chinese name for the list.
class AiPlane {
  const AiPlane({required this.id, required this.key, required this.label, required this.group, required this.face, required this.w, required this.h, required this.cached});
  final String id, key, label, group; // group: person | faces | depth | sky | tap
  final int face; // 1-based face index for the face features, else 0
  final int w, h;
  final bool cached;

  factory AiPlane.fromJson(Map<String, Object?> j) => AiPlane(
        id: '${j['id']}',
        key: '${j['key']}',
        label: '${j['label']}',
        group: '${j['group']}',
        face: (j['face'] as num?)?.toInt() ?? 0,
        w: (j['w'] as num?)?.toInt() ?? 0,
        h: (j['h'] as num?)?.toInt() ?? 0,
        cached: j['cached'] == true,
      );
}

/// Outcome of one group (person | faces | depth | sky): state ok | cached | none | unavailable | error | cancelled.
class AiGroup {
  const AiGroup(this.group, this.state, this.message, this.ms);
  final String group, state, message;
  final double ms;
}

/// One face of a job's `face_list` (the engine's faces meta): 1-based index (largest first), box and centre (frame-normalised), yaw in
/// degrees, frontal, how open the mouth is (0..1), whether teeth show, iod (fraction of the long edge) and the feature planes it has.
class AiFace {
  const AiFace({required this.index, this.box = const [], this.yaw = 0, this.frontal = false, this.mouthOpen = 0, this.teeth = false, this.iod = 0, this.center = const [], this.planes = const [], this.shape = const []});

  /// The reshaping anchors (54 points, photo-normalised x, y pairs; empty from an older engine or cache), docs section 45.
  final List<double> shape;
  final int index;
  final List<double> box, center;
  final double yaw, mouthOpen, iod;
  final bool frontal, teeth;
  final List<String> planes;

  static List<double> _nums(Object? v) => [for (final x in (v as List? ?? const [])) if (x is num) x.toDouble()];

  factory AiFace.fromJson(Map<String, Object?> j) => AiFace(
        index: (j['index'] as num?)?.toInt() ?? 0,
        box: _nums(j['box']),
        yaw: (j['yaw'] as num?)?.toDouble() ?? 0,
        frontal: j['frontal'] == true,
        mouthOpen: (j['mouth_open'] as num?)?.toDouble() ?? 0,
        teeth: j['teeth'] == true,
        iod: (j['iod'] as num?)?.toDouble() ?? 0,
        center: _nums(j['center']),
        planes: [for (final p in (j['planes'] as List? ?? const [])) '$p'],
        shape: _nums(j['shape']),
      );
}

class AiJobResult {
  const AiJobResult({required this.state, required this.message, required this.planes, required this.groups, required this.personFound, required this.faces, required this.skyFound, required this.totalMs, this.faceList = const []});

  /// The faces of the analysis (`face_list`); empty from an engine / cache that does not write it (the app then estimates the scale).
  final List<AiFace> faceList;
  final String state; // done | cancelled | error
  final String message;
  final List<AiPlane> planes;
  final List<AiGroup> groups;
  final bool personFound; // false: "no person found", the person planes do not exist
  final int faces; // faces with a mesh
  final bool skyFound;
  final double totalMs;

  AiGroup? group(String name) {
    for (final g in groups) {
      if (g.group == name) return g;
    }
    return null;
  }

  factory AiJobResult.fromJson(Map<String, Object?> j) {
    final planes = <AiPlane>[
      for (final p in (j['planes'] as List? ?? const [])) if (p is Map) AiPlane.fromJson(Map<String, Object?>.from(p)),
    ];
    final groups = <AiGroup>[
      for (final g in (j['groups'] as List? ?? const []))
        if (g is Map) AiGroup('${g['group']}', '${g['state']}', '${g['message'] ?? ''}', (g['ms'] as num?)?.toDouble() ?? 0),
    ];
    return AiJobResult(
      state: '${j['state']}',
      message: '${j['message'] ?? ''}',
      planes: planes,
      groups: groups,
      personFound: j['person_found'] == true,
      faces: (j['faces'] as num?)?.toInt() ?? 0,
      skyFound: j['sky_found'] == true,
      totalMs: (j['total_ms'] as num?)?.toDouble() ?? 0,
      faceList: [for (final f in (j['face_list'] as List? ?? const [])) if (f is Map) AiFace.fromJson(Map<String, Object?>.from(f))],
    );
  }
}

/// A poll of a running job; [result] is set on the last event.
class AiProgress {
  const AiProgress(this.job, this.state, this.progress, this.stage, [this.result]);
  final int job;
  final String state; // queued | running | done | cancelled | error | unknown
  final double progress; // 0..1
  final String stage;
  final AiJobResult? result;
  bool get finished => result != null;
}

class AiTapResult {
  const AiTapResult({required this.ok, required this.message, required this.id, required this.taps, required this.w, required this.h, required this.ms});
  final bool ok;
  final String message;
  final String id; // the accumulated plane (registered); empty after a reset or when no tap is left
  final List<(double u, double v, bool add)> taps;
  final int w, h;
  final double ms;
}

class AiMissing {
  const AiMissing(this.id, {required this.ai, required this.thisPhoto});
  final String id;
  final bool ai; // an "ai:" id the service can re-create
  final bool thisPhoto; // the id carries this session's photo hash
}

/// A plane as a small RGBA image (gray) for lists.
class AiPlanePreview {
  const AiPlanePreview(this.width, this.height, this.rgba);
  final int width, height;
  final Uint8List rgba;
}

// ---- calls -------------------------------------------------------------------------------------------------------------------------------

class AiNative {
  static String? _take(AiBindings b, ffi.Pointer<Utf8> p) {
    if (p == ffi.nullptr) return null;
    final s = p.toDartString();
    b.freeString(p);
    return s;
  }

  static Map<String, Object?>? _map(String? s) {
    if (s == null) return null;
    try {
      final j = jsonDecode(s);
      return j is Map ? Map<String, Object?>.from(j) : null;
    } catch (_) {
      return null;
    }
  }

  /// Tells the engine where the model files and the cache directory are. Safe to call again.
  static bool configure(String modelDir, String cacheDir) {
    final b = AiBindings.instanceOrNull;
    if (b?.configure == null) return false;
    final m = modelDir.toNativeUtf8(), c = cacheDir.toNativeUtf8();
    try {
      return b!.configure!(m, c) == dappleOk;
    } finally {
      malloc.free(m);
      malloc.free(c);
    }
  }

  static AiAvailability status() {
    final b = AiBindings.instanceOrNull;
    if (b?.status == null) return AiAvailability.none;
    final j = _map(_take(b!, b.status!()));
    return j == null ? AiAvailability.none : AiAvailability.fromJson(j);
  }

  /// The 16 hex digits of this photo's plane ids.
  static Future<String?> photoId(int session) {
    final b = AiBindings.instanceOrNull;
    if (b?.photoId == null || session == 0) return Future.value(null);
    return Isolate.run(() {
      final bb = AiBindings.instanceOrNull!; // not captured: the function pointers are not sendable
      return _take(bb, bb.photoId!(ffi.Pointer.fromAddress(session)));
    });
  }

  /// Starts a job; returns its id, or -1 (see [lastError]).
  static int start(int session, {int tasks = AiTask.all, bool force = false, int maxFaces = 4}) {
    final b = AiBindings.instanceOrNull;
    if (b?.start == null || session == 0) return -1;
    return b!.start!(ffi.Pointer.fromAddress(session), tasks, force ? 1 : 0, maxFaces);
  }

  static String lastError() {
    final b = DappleBindings.instanceOrNull;
    if (b == null) return '';
    final p = b.lastError();
    return p == ffi.nullptr ? '' : p.toDartString();
  }

  static AiProgress? poll(int job) {
    final b = AiBindings.instanceOrNull;
    if (b?.jobJson == null) return null;
    final j = _map(_take(b!, b.jobJson!(job)));
    if (j == null) return null;
    final state = '${j['state']}';
    final done = state != 'queued' && state != 'running' && state != 'unknown';
    AiJobResult? r;
    if (done && j['result'] is Map) r = AiJobResult.fromJson(Map<String, Object?>.from(j['result']! as Map));
    return AiProgress(job, state, (j['progress'] as num?)?.toDouble() ?? 0, '${j['stage'] ?? ''}', r);
  }

  static void cancel(int job) => AiBindings.instanceOrNull?.jobCancel?.call(job);

  /// Tap select (op 0 add, 1 subtract, 2 undo, 3 reset), (u, v) in the photo's normalised space. Runs in an isolate.
  static Future<AiTapResult> tap(int session, double u, double v, int op) {
    final b = AiBindings.instanceOrNull;
    if (b?.tap == null || session == 0) return Future.value(const AiTapResult(ok: false, message: 'unavailable', id: '', taps: [], w: 0, h: 0, ms: 0));
    return Isolate.run(() {
      final bb = AiBindings.instanceOrNull!;
      final j = _map(_take(bb, bb.tap!(ffi.Pointer.fromAddress(session), u, v, op)));
      if (j == null) return AiTapResult(ok: false, message: lastError(), id: '', taps: const [], w: 0, h: 0, ms: 0);
      final taps = <(double, double, bool)>[
        for (final t in (j['taps'] as List? ?? const []))
          if (t is List && t.length >= 3) ((t[0] as num).toDouble(), (t[1] as num).toDouble(), (t[2] as num) != 0),
      ];
      return AiTapResult(
        ok: j['ok'] == true,
        message: '${j['message'] ?? ''}',
        id: '${j['id'] ?? ''}',
        taps: taps,
        w: (j['w'] as num?)?.toInt() ?? 0,
        h: (j['h'] as num?)?.toInt() ?? 0,
        ms: (j['ms'] as num?)?.toDouble() ?? 0,
      );
    });
  }

  /// The plane [id] (in the raster store) as a gray RGBA image of long edge [longEdge].
  static AiPlanePreview? planePreview(String id, {int longEdge = 128}) {
    final b = AiBindings.instanceOrNull;
    if (b?.planePreview == null) return null;
    final p = id.toNativeUtf8();
    final img = calloc<DappleImageStruct>();
    try {
      if (b!.planePreview!(p, longEdge, img) != dappleOk || img.ref.rgba == ffi.nullptr) return null;
      final w = img.ref.width, h = img.ref.height;
      final out = Uint8List.fromList(img.ref.rgba.asTypedList(w * h * 4));
      b.freeImage(img);
      return AiPlanePreview(w, h, out);
    } finally {
      malloc.free(p);
      calloc.free(img);
    }
  }

  /// The raster ids of the enabled local adjustments of [paramsJson] that are not in the store.
  static Future<List<AiMissing>> missing(int session, String paramsJson) {
    final b = AiBindings.instanceOrNull;
    if (b?.missing == null || session == 0) return Future.value(const []);
    return Isolate.run(() {
      final bb = AiBindings.instanceOrNull!;
      final p = paramsJson.toNativeUtf8();
      try {
        final j = _map(_take(bb, bb.missing!(ffi.Pointer.fromAddress(session), p)));
        return <AiMissing>[
          for (final m in (j?['missing'] as List? ?? const []))
            if (m is Map) AiMissing('${m['id']}', ai: m['ai'] == true, thisPhoto: m['photo'] == true),
        ];
      } finally {
        malloc.free(p);
      }
    });
  }

  /// Re-creates the missing "ai:" planes of [paramsJson] (disk cache, else recomputation). background = true starts a job and returns its id
  /// (0 = nothing to do; tap planes are re-created before it returns); false: waits and returns the number of ids still missing.
  static Future<int> ensure(int session, String paramsJson, {bool background = false}) {
    final b = AiBindings.instanceOrNull;
    if (b?.ensure == null || session == 0) return Future.value(-1);
    return Isolate.run(() {
      final bb = AiBindings.instanceOrNull!;
      final p = paramsJson.toNativeUtf8();
      try {
        return bb.ensure!(ffi.Pointer.fromAddress(session), p, background ? 1 : 0);
      } finally {
        malloc.free(p);
      }
    });
  }

  /// Frees model memory (all = false: only what is idle; the service does that by itself every few seconds).
  static void release({bool all = false}) => AiBindings.instanceOrNull?.release?.call(all ? 1 : 0);
}
