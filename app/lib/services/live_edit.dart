// A Motion Photo's clip follows the still's edit (requirements 4.9.5a, docs/engine/pipeline-architecture.md section 30): the pure decision and
// the payload for LiveBridge.trim. No Flutter, no AppState: unit tested on its own.
import 'dart:convert';

import 'live_bridge.dart';

/// True when the clip must go through the node graph: the still has an edit (user edit, the automatic correction applied to it, or a destination
/// crop). False = the clip is exported as recorded (pass-through, the pre-wave-8 behaviour).
bool liveClipEdited({required bool userEdited, required bool autoCorrectionApplied, String? cropOverride}) =>
    userEdited || autoCorrectionApplied || (cropOverride != null && cropOverride.isNotEmpty && cropOverride != 'orig');

/// The payload for [LiveBridge.trim]: exactly the parameters the still is rendered with ([wire], the editor's wire map), the cover time as the
/// reference frame, and the long edge of the exported still (film grain scale). Null when the clip is not edited.
EditedClip? editedClipFor({
  required Map<String, dynamic> wire,
  required bool edited,
  required int coverMs,
  int stillLongEdge = 0,
  String? lutPath,
}) {
  if (!edited) return null;
  return EditedClip(paramsJson: jsonEncode(wire), lutPath: lutPath, refMs: coverMs, stillLongEdge: stillLongEdge);
}
