// 肤色保护 (film window): with a film stock and film_skin on, the engine caps the film's chroma gain inside the AI face / body skin planes. The planes
// come from the AI person task of the photo's session; this starts it once per session (a disk-cache hit is milliseconds), never blocks a frame, and
// never fails: a photo without people, a device without the model or a closed session simply yields no planes.
import 'dart:async';

import 'package:flutter/foundation.dart';

import '../native/dapple_session.dart';
import 'ai_masks.dart';

class SkinPlanes {
  SkinPlanes._();

  static final Expando<Future<bool>> _jobs = Expando<Future<bool>>('skin planes');

  /// Counts the sessions whose planes have just landed: the editor re-renders (the planes are part of the engine's cache key).
  static final ValueNotifier<int> landed = ValueNotifier<int>(0);

  /// True when [params] (wire JSON) ask for the skin cap: a film stock and film_skin 1.
  static bool wanted(Map<String, dynamic> params) => params['film'] != null && params['film_skin'] == 1;

  /// Starts the person task on [s] when [params] want it (once per session). Completes true when planes landed that were not there before.
  static Future<bool> ensure(DappleSession s, Map<String, dynamic> params) {
    if (!wanted(params)) return Future.value(false);
    return _jobs[s] ??= _run(s);
  }

  static Future<bool> _run(DappleSession s) async {
    try {
      final av = await AiMasks.instance.prepare();
      if (av.tasks & AiTask.person == 0) return false;
      final r = await AiMasks.instance.runToEnd(s, tasks: AiTask.person);
      final ok = r.personFound;
      if (ok) landed.value++;
      return ok;
    } catch (_) {
      return false;
    }
  }
}
