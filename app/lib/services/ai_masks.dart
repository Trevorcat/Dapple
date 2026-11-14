// AI masks service (UI-free): availability, start / cancel, a progress stream, tap select and small previews of the planes. The engine does the work
// (docs/engine/pipeline-architecture.md section 41): it computes raster planes from the neutral rendition of a photo, caches them on disk per photo
// hash and registers them in the raster store under stable ids ("ai:<photo>:<key>"). The editor puts such an id into a "raster" component
// (`ref`); because the id says what to compute, an edit opened later (cache wipe, another device) gets its planes back through [ensure].
//
//   final ai = AiMasks.instance;
//   final av = await ai.prepare();                      // models + cache dir, once; av.tasks says what this device can do
//   await for (final p in ai.run(session)) { ... }      // progress 0..1; the last event has p.result (planes, "no person found" ...)
//   final t = await ai.tap(session, 0.4, 0.6);          // tap select, accumulates; t.id is the plane
//   final img = ai.preview(plane.id);                   // 128 px gray RGBA for a list
//   await ai.ensure(session, paramsJson);               // before showing an edit that references AI planes
//
// A depth plane is a normal raster plane (0 = nearest .. 255 = farthest); select a depth band with the raster component's range
// ("lo" / "hi" / "feather", 0..100) instead of a separate plane.
import 'dart:async';
import 'dart:io';

import 'package:flutter/services.dart' show MethodChannel;
import 'package:path_provider/path_provider.dart';

import '../native/ai_bindings.dart';
import '../native/dapple_session.dart';

export '../native/ai_bindings.dart'
    show AiTask, AiAvailability, AiPlane, AiGroup, AiFace, AiPose, AiJobResult, AiProgress, AiTapResult, AiMissing, AiPlanePreview;

class AiMasks {
  AiMasks._();
  static final AiMasks instance = AiMasks._();

  static const MethodChannel _channel = MethodChannel('dapple/ai');

  AiAvailability _availability = AiAvailability.none;
  Future<AiAvailability>? _preparing;

  /// What the engine can do on this device (after [prepare]).
  AiAvailability get availability => _availability;

  /// Copies the bundled model files to app storage on first use (Android; a no-op after the first time; iOS reads them in place from the app bundle), tells the engine where the models and
  /// the cache directory are, and returns the availability. Idempotent. On desktop the models come from DAPPLE_AI_MODELS (development only).
  Future<AiAvailability> prepare({String? modelDir, String? cacheDir}) => _preparing ??= _prepare(modelDir, cacheDir);

  Future<AiAvailability> _prepare(String? modelDir, String? cacheDir) async {
    if (!AiBindings.present) return _availability = AiAvailability.none;
    var models = modelDir ?? '';
    if (models.isEmpty) {
      if (Platform.isAndroid || Platform.isIOS) {
        try {
          models = (await _channel.invokeMethod<String>('prepare')) ?? '';
        } catch (_) {}
      } else {
        models = Platform.environment['DAPPLE_AI_MODELS'] ?? '';
      }
    }
    var cache = cacheDir ?? '';
    if (cache.isEmpty) {
      try {
        cache = (await getTemporaryDirectory()).path; // the app cache directory (the GPU caches live there too)
      } catch (_) {}
    }
    AiNative.configure(models, cache);
    return _availability = AiNative.status();
  }

  /// A fresh status read (resident model bytes change).
  AiAvailability refreshStatus() => _availability = AiNative.status();

  /// Runs [tasks] (AiTask bits) on [session]: yields a progress event about every 120 ms and finally one with `result`. Cancelling the
  /// subscription cancels the job. Planes already in the disk cache are published at once (`cached`), [force] recomputes. The events carry
  /// state "error" with a message when the engine cannot start (see [AiAvailability.reason]).
  Stream<AiProgress> run(DappleSession session, {int tasks = AiTask.all, bool force = false, int maxFaces = 4}) {
    late StreamController<AiProgress> c;
    var job = -1;
    Timer? timer;
    void stop() {
      timer?.cancel();
      timer = null;
    }

    c = StreamController<AiProgress>(
      onListen: () async {
        await prepare();
        if (c.isClosed) return;
        job = AiNative.start(session.address, tasks: tasks, force: force, maxFaces: maxFaces);
        if (job <= 0) {
          final msg = AiNative.lastError();
          c.add(AiProgress(job, 'error', 0, '', AiJobResult(state: 'error', message: msg.isEmpty ? _availability.reason : msg, planes: const [], groups: const [], personFound: false, faces: 0, skyFound: false, totalMs: 0)));
          await c.close();
          return;
        }
        timer = Timer.periodic(const Duration(milliseconds: 120), (_) {
          final p = AiNative.poll(job);
          if (p == null || c.isClosed) return;
          c.add(p);
          if (p.finished || p.state == 'unknown') {
            stop();
            c.close();
          }
        });
      },
      onCancel: () {
        stop();
        if (job > 0) AiNative.cancel(job);
      },
    );
    return c.stream;
  }

  /// Convenience: runs [tasks] and returns the result.
  Future<AiJobResult> runToEnd(DappleSession session, {int tasks = AiTask.all, bool force = false, int maxFaces = 4}) async {
    AiProgress? last;
    await for (final p in run(session, tasks: tasks, force: force, maxFaces: maxFaces)) {
      last = p;
    }
    return last?.result ?? const AiJobResult(state: 'error', message: 'no result', planes: [], groups: [], personFound: false, faces: 0, skyFound: false, totalMs: 0);
  }

  /// Cancels a job by id (the stream's own cancel does the same).
  void cancel(int job) => AiNative.cancel(job);

  /// Tap select at the photo's normalised point (u, v), in the same space as every mask (before crop / rotate): maps a screen point with the
  /// editor's frame map first. The object under the tap is added to the one accumulated plane (`t.id`); [add] false subtracts it. The first
  /// tap of a photo also runs the encoder (about 0.7 s on a phone: call [prepareTap] when the tool opens to hide it).
  Future<AiTapResult> tap(DappleSession session, double u, double v, {bool add = true}) => AiNative.tap(session.address, u, v, add ? 0 : 1);

  /// Removes the last tap; the plane is re-made from the earlier ones (`id` empty when none is left).
  Future<AiTapResult> undoTap(DappleSession session) => AiNative.tap(session.address, 0, 0, 2);

  /// Forgets all taps of the photo.
  Future<AiTapResult> resetTaps(DappleSession session) => AiNative.tap(session.address, 0, 0, 3);

  /// Runs the tap encoder now as a background job.
  Stream<AiProgress> prepareTap(DappleSession session) => run(session, tasks: AiTask.tapPrepare);

  /// A small gray RGBA preview of a registered plane (null when the id is not in the store).
  AiPlanePreview? preview(String id, {int longEdge = 128}) => AiNative.planePreview(id, longEdge: longEdge);

  /// The raster ids of the enabled local adjustments of [paramsJson] that are not in the store (after an app restart, a cache wipe ...).
  Future<List<AiMissing>> missing(DappleSession session, String paramsJson) => AiNative.missing(session.address, paramsJson);

  /// Re-creates the missing "ai:" planes of an edit: disk cache first (milliseconds), else recomputation. [background] true returns a job id
  /// (0 = nothing to do) to follow with [AiNative.poll]; false waits and returns how many ids are still missing (0 = all there).
  /// An export does this itself and FAILS rather than writing a picture whose mask is empty; interactive frames render a missing plane as empty.
  Future<int> ensure(DappleSession session, String paramsJson, {bool background = false}) => AiNative.ensure(session.address, paramsJson, background: background);

  /// Frees model memory now (memory pressure); the engine frees idle models by itself after 30 s.
  void release({bool all = false}) => AiNative.release(all: all);
}
