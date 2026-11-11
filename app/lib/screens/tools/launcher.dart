import 'package:flutter/material.dart';

import '../../services/live_bridge.dart';
import '../../services/tool_models.dart';
import '../../state/app_state.dart';
import 'live_tool_page.dart';
import 'merge_tool_page.dart';

/// Opens the HDR / panorama tool with a prepared draft (detected set, multi-select, or a failed job's draft).
Future<void> openMergeTool(BuildContext context, AppState app, MergeDraft draft) {
  return Navigator.of(context).push<void>(MaterialPageRoute(
    fullscreenDialog: true,
    builder: (_) => MergeToolPage(app: app, draft: draft),
  ));
}

/// Opens the video -> Motion Photo tool ([video] preselected, else the picker is offered).
Future<void> openLiveTool(BuildContext context, AppState app, {VideoItem? video}) {
  return Navigator.of(context).push<void>(MaterialPageRoute(
    fullscreenDialog: true,
    builder: (_) => LiveToolPage(app: app, video: video),
  ));
}

/// Multi-select -> 合成 (requirements 4.10): HDR or panorama with the selected photos, oldest first.
Future<void> openComposeFromSelection(BuildContext context, AppState app, ToolKind kind) {
  final draft = app.draftForPhotos(kind, app.selectedPhotos);
  return openMergeTool(context, app, draft);
}

/// "查看" on a failed background job: reopen the tool with the same selection and parameters.
void reopenToolForJob(AppState app, ToolJob job) {
  final nav = app.navigatorKey.currentState;
  if (nav == null) return;
  if (job.draft != null) {
    nav.push<void>(MaterialPageRoute(fullscreenDialog: true, builder: (_) => MergeToolPage(app: app, draft: job.draft!)));
  } else if (job.live != null) {
    nav.push<void>(MaterialPageRoute(fullscreenDialog: true, builder: (_) => LiveToolPage(app: app, video: job.live!.video)));
  }
}
