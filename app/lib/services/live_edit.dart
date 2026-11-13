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

/// Does the Motion Photo cover get an HDR gain map (docs/engine/pipeline-architecture.md section 30.14)? Only an HDR clip (colour kind 2 HLG, 3 PQ of the
/// video core) has highlights above SDR white to put into it; the engine then decides from the cover frame's headroom (peak >= 1.5 x SDR white, or >= 1.1
/// when the user chose HDR in the export options: [hdrOption] lowers the threshold, it cannot create headroom an SDR clip does not have).
bool liveCoverWantsHdr({required bool hdrOption, required int colorKind}) => colorKind == 2 || colorKind == 3;

/// The payload for [LiveBridge.coverHdr]: the edit the still was rendered with (also when the clip itself is exported as recorded: the cover frame is
/// then rendered with the identity of the still's wire), the cover time as the reference frame, the long edge of the still.
EditedClip coverEditFor({required Map<String, dynamic> wire, required int coverMs, int stillLongEdge = 0, String? lutPath}) =>
    EditedClip(paramsJson: jsonEncode(wire), lutPath: lutPath, refMs: coverMs, stillLongEdge: stillLongEdge);
