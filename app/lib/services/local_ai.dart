// What 局部 needs from the AI masks service (docs/engine/pipeline-architecture.md section 41, docs/design/local-adjustments.md section 9):
// availability, an analysis job with progress, tap select, the missing-plane check and repair, plane previews and memory release. The
// editor talks to this seam only, so tests drive the whole flow with a fake; [EngineLocalAi] is the real one over [AiMasks].
import 'dart:async';

import '../models.dart' show Photo;
import '../state/app_state.dart';
import 'ai_masks.dart';

export 'ai_masks.dart' show AiTask, AiAvailability, AiPlane, AiGroup, AiFace, AiPose, AiJobResult, AiProgress, AiTapResult, AiMissing, AiPlanePreview;

abstract class LocalAi {
  /// What this device can do (models bundled, runtimes present). Never throws.
  Future<AiAvailability> prepare();

  /// The analysis of [p] for [tasks] (AiTask bits): progress events, the last one carries the result. Cancelling the subscription cancels the job.
  Stream<AiProgress> run(Photo p, int tasks);

  /// Tap select on [p] at photo (u, v): the object is added to (or, [add] false, subtracted from) the photo's accumulated plane.
  Future<AiTapResult> tap(Photo p, double u, double v, {bool add = true});
  Future<AiTapResult> undoTap(Photo p);
  Future<AiTapResult> resetTaps(Photo p);

  /// Runs the tap encoder in the background (the first tap is then fast).
  void prepareTap(Photo p);

  /// The AI raster ids of [paramsJson] that are not in the raster store.
  Future<List<AiMissing>> missing(Photo p, String paramsJson);

  /// Re-creates the missing planes (disk cache, else recomputation); returns how many are still missing (-1 = could not run).
  Future<int> ensure(Photo p, String paramsJson);

  AiPlanePreview? preview(String id, {int longEdge = 128});

  /// Frees model memory ([all]: everything, memory pressure / leaving the editor).
  void release({bool all = false});
}

/// No AI (the native library lacks it, demo photos, tests that do not care).
class NoLocalAi implements LocalAi {
  const NoLocalAi();
  static const _none = AiTapResult(ok: false, message: 'unavailable', id: '', taps: [], w: 0, h: 0, ms: 0);
  @override
  Future<AiAvailability> prepare() async => AiAvailability.none;
  @override
  Stream<AiProgress> run(Photo p, int tasks) => const Stream.empty();
  @override
  Future<AiTapResult> tap(Photo p, double u, double v, {bool add = true}) async => _none;
  @override
  Future<AiTapResult> undoTap(Photo p) async => _none;
  @override
  Future<AiTapResult> resetTaps(Photo p) async => _none;
  @override
  void prepareTap(Photo p) {}
  @override
  Future<List<AiMissing>> missing(Photo p, String paramsJson) async => const [];
  @override
  Future<int> ensure(Photo p, String paramsJson) async => -1;
  @override
  AiPlanePreview? preview(String id, {int longEdge = 128}) => null;
  @override
  void release({bool all = false}) {}
}

/// The engine's service on the photo's session.
class EngineLocalAi implements LocalAi {
  EngineLocalAi(this.app);
  final AppState app;
  AiMasks get _ai => AiMasks.instance;
  StreamSubscription<AiProgress>? _tapPrep;

  static const _noSession = AiTapResult(ok: false, message: 'no session', id: '', taps: [], w: 0, h: 0, ms: 0);

  @override
  Future<AiAvailability> prepare() async {
    if (!app.sessions.available) return AiAvailability.none;
    try {
      return await _ai.prepare();
    } catch (_) {
      return AiAvailability.none;
    }
  }

  @override
  Stream<AiProgress> run(Photo p, int tasks) async* {
    final s = await app.sessions.get(p);
    if (s == null) return;
    yield* _ai.run(s, tasks: tasks);
  }

  @override
  Future<AiTapResult> tap(Photo p, double u, double v, {bool add = true}) async {
    final s = await app.sessions.get(p);
    return s == null ? _noSession : _ai.tap(s, u, v, add: add);
  }

  @override
  Future<AiTapResult> undoTap(Photo p) async {
    final s = await app.sessions.get(p);
    return s == null ? _noSession : _ai.undoTap(s);
  }

  @override
  Future<AiTapResult> resetTaps(Photo p) async {
    final s = await app.sessions.get(p);
    return s == null ? _noSession : _ai.resetTaps(s);
  }

  @override
  void prepareTap(Photo p) {
    _tapPrep?.cancel();
    () async {
      final s = await app.sessions.get(p);
      if (s == null) return;
      _tapPrep = _ai.prepareTap(s).listen((_) {});
    }();
  }

  @override
  Future<List<AiMissing>> missing(Photo p, String paramsJson) async {
    final s = await app.sessions.get(p);
    return s == null ? const [] : _ai.missing(s, paramsJson);
  }

  @override
  Future<int> ensure(Photo p, String paramsJson) async {
    final s = await app.sessions.get(p);
    return s == null ? -1 : _ai.ensure(s, paramsJson);
  }

  @override
  AiPlanePreview? preview(String id, {int longEdge = 128}) => _ai.preview(id, longEdge: longEdge);

  @override
  void release({bool all = false}) => _ai.release(all: all);
}

/// The key of an "ai:" plane id (`ai:photo:key`, the key may hold a colon), or null.
String? aiKeyOf(String id) {
  if (!id.startsWith('ai:')) return null;
  final i = id.indexOf(':', 3);
  return i < 0 ? null : id.substring(i + 1);
}

/// The taps of a tap-select id ("...:tap:+412,531;-200,700", thousandths): (u, v, add).
List<(double, double, bool)> tapsOfId(String id) {
  final k = aiKeyOf(id);
  if (k == null || !k.startsWith('tap:')) return const [];
  final out = <(double, double, bool)>[];
  for (final t in k.substring(4).split(';')) {
    if (t.length < 4) continue;
    final add = t[0] != '-';
    final xy = t.substring(1).split(',');
    if (xy.length != 2) continue;
    final x = int.tryParse(xy[0]), y = int.tryParse(xy[1]);
    if (x == null || y == null) continue;
    out.add((x / 1000, y / 1000, add));
  }
  return out;
}
