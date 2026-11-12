// Export size options in plain words (no pixel jargon) and the picker sheet shared by Settings and Publish.
import 'package:flutter/material.dart';

import '../models.dart';
import '../theme/tokens.dart';
import 'icons.dart';

/// Long-edge values behind the three options. 0 = keep the photo's own size.
const exportSizeOriginal = 0, exportSizeLarge = 4096, exportSizeSocial = 2048;
const exportSizes = [exportSizeOriginal, exportSizeLarge, exportSizeSocial];

String exportSizeName(int le) => le <= 0
    ? '原图大小'
    : le >= 3000
        ? '大'
        : '适合社交平台';

String exportSizeDesc(int le) => le <= 0
    ? '保留拍摄的全部像素，最清晰'
    : le >= 3000
        ? '适合大屏查看'
        : '文件小，发出去更快';

/// Output pixel size when [p]'s size is known, else null.
({int w, int h})? exportDims(Photo? p, int le) {
  final w = p?.pxW, h = p?.pxH;
  if (w == null || h == null || w <= 0 || h <= 0) return null;
  final long = w > h ? w : h;
  if (le <= 0 || long <= le) return (w: w, h: h);
  final k = le / long;
  return (w: (w * k).round(), h: (h * k).round());
}

/// "6336 × 4224 · 2680 万像素".
String exportDimsText(({int w, int h}) d) {
  final mp = d.w * d.h / 1e4;
  final m = mp >= 100 ? mp.round().toString() : mp.toStringAsFixed(0);
  return '${d.w} × ${d.h} · $m 万像素';
}

/// Bottom sheet with the three options; each shows what [photo] would come out as. Returns the choice.
Future<int?> showExportSizeSheet(BuildContext context, {required int current, Photo? photo}) {
  final c = context.c;
  return showModalBottomSheet<int>(
    context: context,
    backgroundColor: c.surface,
    showDragHandle: true,
    shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(Rd.sheet))),
    constraints: const BoxConstraints(maxWidth: 480),
    builder: (ctx) => SafeArea(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
        child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
          for (final le in exportSizes)
            Semantics(
              button: true,
              checked: le == current,
              label: '${exportSizeName(le)}，${exportSizeDesc(le)}',
              excludeSemantics: true,
              child: InkWell(
                key: ValueKey('export-size-opt-$le'),
                onTap: () => Navigator.pop(ctx, le),
                child: Container(
                  constraints: const BoxConstraints(minHeight: 64),
                  padding: const EdgeInsets.symmetric(vertical: 8),
                  child: Row(children: [
                    Expanded(
                      child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
                        Text(exportSizeName(le), style: T.body17(c.text)),
                        Text(
                            () {
                              final d = exportDims(photo, le);
                              return d == null ? exportSizeDesc(le) : exportDimsText(d);
                            }(),
                            style: T.small(c.text2)),
                      ]),
                    ),
                    if (le == current) DIcon(DI.check, size: 22, color: c.accent),
                  ]),
                ),
              ),
            ),
        ]),
      ),
    ),
  );
}
