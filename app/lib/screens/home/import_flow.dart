import 'dart:io' show Platform;

import 'package:flutter/material.dart';

import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/icons.dart';
import '../../services/tool_models.dart';
import '../editor/editor_page.dart';
import '../tools/launcher.dart';

bool _busy = false;

/// "从相册选…" / "从文件选…": system picker -> originals copied in -> developing in the list; exactly one pick opens the editor on it.
Future<void> importAndEdit(BuildContext context, AppState app, {String source = 'photos'}) async {
  if (_busy) return;
  _busy = true;
  try {
    final id = await app.importFromPicker(source: source);
    if (id != null && context.mounted) openEditor(context, app, id);
  } finally {
    _busy = false;
  }
}

/// Only Android has a second route (the document picker, for RAW types the photo picker does not list).
bool get canPickFiles => Platform.isAndroid;

/// Top-bar "+": the sources (album, files on Android) and, as the manual way in that does not depend on detection, the three tools
/// (HDR 合成 / 全景拼接 / 视频 → 动态照片; each offers the picker of all photos / videos inside). With nothing to choose from
/// (iOS today: one source, no tools) it goes straight to the album.
Future<void> importMenu(BuildContext context, AppState app) async {
  final merge = app.engine.available, live = app.videoLayer;
  if (!canPickFiles && !merge && !live) return importAndEdit(context, app);
  final source = await showModalBottomSheet<String>(
    context: context,
    backgroundColor: context.c.surface,
    showDragHandle: true,
    shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(Rd.sheet))),
    builder: (ctx) {
      final c = ctx.c;
      // one line per source, no explanation (audit F24): the system picker opens next, hence the "…"
      Widget row(String key, DI icon, String title, String v) => InkWell(
            key: ValueKey(key),
            onTap: () => Navigator.of(ctx).pop(v),
            child: Container(
              constraints: const BoxConstraints(minHeight: 56),
              padding: const EdgeInsets.symmetric(horizontal: Sp.l, vertical: Sp.s),
              child: Row(children: [
                DIcon(icon, size: 24, color: c.text2),
                const SizedBox(width: Sp.l),
                Expanded(child: Text(title, style: T.body17(c.text))),
              ]),
            ),
          );
      return SafeArea(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          row('import-photos', DI.photos, '从相册选…', 'photos'),
          if (canPickFiles) row('import-files', DI.folder, '从文件选…', 'files'),
          if (merge || live) Divider(height: Sp.l, color: c.sep),
          if (merge) row('tool-hdr', DI.merge, 'HDR 合成…', 'tool:hdr'),
          if (merge) row('tool-pano', DI.merge, '全景拼接…', 'tool:pano'),
          if (live) row('tool-live', DI.play, '视频 → 动态照片…', 'tool:live'),
          const SizedBox(height: Sp.s),
        ]),
      );
    },
  );
  if (source == null || !context.mounted) return;
  switch (source) {
    case 'tool:hdr':
      return openMergeTool(context, app, MergeDraft(kind: ToolKind.hdr, sources: []));
    case 'tool:pano':
      return openMergeTool(context, app, MergeDraft(kind: ToolKind.pano, sources: []));
    case 'tool:live':
      return openLiveTool(context, app);
    default:
      await importAndEdit(context, app, source: source);
  }
}
