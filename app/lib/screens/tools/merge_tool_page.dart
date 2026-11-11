import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:path/path.dart' as p;

import '../../services/merge_engine.dart';
import '../../services/tool_models.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/icons.dart';
import '../../widgets/photo_art.dart';
import 'pick_sheet.dart';
import 'progress_dialog.dart';
import 'tool_common.dart';

/// HDR 合成 / 全景合成 tool (requirements 4.9.2 - 4.9.4).
class MergeToolPage extends StatefulWidget {
  const MergeToolPage({super.key, required this.app, required this.draft});
  final AppState app;
  final MergeDraft draft;

  @override
  State<MergeToolPage> createState() => _MergeToolPageState();
}

class _MergeToolPageState extends State<MergeToolPage> {
  AppState get app => widget.app;
  MergeDraft get d => widget.draft;
  bool get isHdr => d.kind == ToolKind.hdr;

  // ---- live preview (a small merge, run in the background; never blocks the page)
  Timer? _debounce;
  MergeHandle? _handle;
  String _previewSig = '';
  String? _previewPath;
  int _previewRev = 0;
  bool _busy = false;
  final Map<String, ({String path, bool temp})> _resolved = {};

  @override
  void initState() {
    super.initState();
    _schedulePreview();
  }

  @override
  void dispose() {
    _debounce?.cancel();
    _handle?.requestCancel();
    for (final r in _resolved.values) {
      if (r.temp) {
        try {
          File(r.path).deleteSync();
        } catch (_) {}
      }
    }
    super.dispose();
  }

  void _changed() {
    setState(() {});
    _schedulePreview();
  }

  void _schedulePreview() {
    _debounce?.cancel();
    if (app.isDemo || !app.engine.available || d.checked.length < 2) return;
    final sig = d.signature;
    if (sig == _previewSig) return;
    _debounce = Timer(const Duration(milliseconds: 900), () => unawaited(_runPreview(sig)));
  }

  Future<void> _runPreview(String sig) async {
    _handle?.requestCancel();
    final h = MergeHandle();
    _handle = h;
    if (mounted) setState(() => _busy = true);
    try {
      final paths = <String>[];
      for (final s in d.checked) {
        final ph = app.photoById(s.photoId);
        if (ph == null) return;
        var r = _resolved[ph.id];
        if (r == null) {
          r = await app.resolveSource(ph);
          _resolved[ph.id] = r;
        }
        paths.add(r.path);
      }
      final out = p.join(app.scratchPath, 'merge_preview_${++_previewRev}.jpg');
      final res = await app.engine.merge(MergeRequest(
        kind: d.kind.engineKind,
        paths: paths,
        handle: h,
        options: d.options(quality: 'preview', longEdge: 900),
        previewJpeg: out,
        previewEdge: 900,
      ));
      if (!mounted || _handle != h) return;
      if (res.ok) {
        imageCache.clear();
        setState(() {
          _previewPath = out;
          _previewSig = sig;
        });
      } else if (!res.cancelled) {
        setState(() => _previewSig = sig); // keep the base frame; the start button reports the real failure
      }
    } catch (_) {
      // preview is best effort
    } finally {
      if (_handle == h) {
        _handle = null;
        if (mounted) setState(() => _busy = false);
      }
      h.dispose();
    }
  }

  Future<void> _pick() async {
    final changed = await showSourcePicker(context, app, d);
    if (changed) _changed();
  }

  Future<void> _start() async {
    _debounce?.cancel();
    _handle?.requestCancel();
    final job = await app.startMerge(d);
    if (!mounted) return;
    final r = await showToolProgress(context, app, job);
    if (!mounted) return;
    switch (r) {
      case ToolDialogResult.done:
        Navigator.of(context).pop();
        app.showToast('${d.kind.resultName}已加入「今天」');
        break;
      case ToolDialogResult.background:
        Navigator.of(context).pop();
        break;
      case ToolDialogResult.changeBase:
        d.nextBase(); // not retried automatically
        _changed();
        break;
      case ToolDialogResult.cancelled:
      case ToolDialogResult.closedFailure:
        break;
    }
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final n = d.checked.length;
    final size = MediaQuery.sizeOf(context);
    final base = app.photoById(d.effectiveBaseId ?? '');
    final boxW = size.width - 32;
    final aspect = isHdr ? 1.5 : 3.0;
    final maxH = size.height * 0.30;
    var w = boxW, h = boxW / aspect;
    if (h > maxH) {
      h = maxH;
      w = h * aspect;
    }
    Widget preview;
    if (_previewPath != null && n >= 2 && _previewSig == d.signature) {
      preview = Image.file(File(_previewPath!), fit: BoxFit.cover, gaplessPlayback: true, errorBuilder: (_, _, _) => const SizedBox.shrink());
    } else if (base != null) {
      preview = PhotoArt(base, cacheWidth: 800);
    } else {
      preview = const SizedBox.shrink();
    }
    return ToolScaffold(
      title: isHdr ? 'HDR 合成' : '全景合成',
      onCancel: () {
        _handle?.requestCancel();
        Navigator.of(context).pop();
      },
      mainLabel: n < 2 ? '至少选 2 张' : (isHdr ? '开始合成' : '开始拼接'),
      onMain: n < 2 ? null : _start,
      body: ListView(padding: const EdgeInsets.only(bottom: Sp.l), children: [
        Center(
          child: SizedBox(
            key: const ValueKey('tool-preview'),
            width: w,
            height: h,
            child: PreviewBox(busy: _busy, child: preview),
          ),
        ),
        const SizedBox(height: Sp.l),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: Sp.l),
          child: Row(children: [
            Expanded(
                child: Text('${isHdr ? '参与合成' : '参与拼接'} · $n 张',
                    key: const ValueKey('tool-count'), style: T.body(c.text2, w: FontWeight.w500, tabular: true))),
            KeyedSubtree(
                key: const ValueKey('tool-pick'),
                child: TextAction('从全部照片中选…', size: 15, bold: true, onPressed: _pick)),
          ]),
        ),
        if (d.note != null)
          Padding(
            padding: const EdgeInsets.fromLTRB(Sp.l, 0, Sp.l, Sp.s),
            child: Text(d.note!, key: const ValueKey('tool-note'), style: T.small(c.text2)),
          ),
        SizedBox(
          height: 66 + 8 + 20 + 6,
          child: ListView.separated(
            key: const ValueKey('tool-strip'),
            scrollDirection: Axis.horizontal,
            padding: const EdgeInsets.symmetric(horizontal: Sp.l),
            itemCount: d.sources.length,
            separatorBuilder: (_, _) => const SizedBox(width: Sp.s),
            itemBuilder: (_, i) => _SourceTile(
              app: app,
              draft: d,
              source: d.sources[i],
              onChanged: _changed,
            ),
          ),
        ),
        const SizedBox(height: Sp.m),
        ToolBlock(children: [
          if (isHdr) ...[
            Text('去鬼影', style: T.body17(c.text)),
            const SizedBox(height: Sp.s),
            KeyedSubtree(
              key: const ValueKey('tool-deghost'),
              child: DSegmented(
                semanticsLabel: '去鬼影',
                labels: const ['关', '低', '中', '高'],
                index: const ['off', 'low', 'mid', 'high'].indexOf(d.deghost),
                onChanged: (i) {
                  d.deghost = const ['off', 'low', 'mid', 'high'][i];
                  _changed();
                },
              ),
            ),
            ToolSwitchRow(
                switchKey: const ValueKey('tool-align'),
                label: '自动对齐',
                value: d.align,
                onChanged: (v) {
                  d.align = v;
                  _changed();
                }),
          ] else ...[
            Text('投影', style: T.body17(c.text)),
            const SizedBox(height: Sp.s),
            KeyedSubtree(
              key: const ValueKey('tool-projection'),
              child: DSegmented(
                semanticsLabel: '投影',
                labels: const ['球面', '圆柱', '透视'],
                index: const ['sph', 'cyl', 'persp'].indexOf(d.projection),
                onChanged: (i) {
                  d.projection = const ['sph', 'cyl', 'persp'][i];
                  _changed();
                },
              ),
            ),
            ToolSwitchRow(
                switchKey: const ValueKey('tool-crop'),
                label: '自动裁切',
                value: d.crop,
                onChanged: (v) {
                  d.crop = v;
                  _changed();
                }),
            ToolSwitchRow(
                switchKey: const ValueKey('tool-fill'),
                label: '填充边缘',
                value: d.fill,
                onChanged: (v) {
                  d.fill = v;
                  _changed();
                }),
          ],
        ]),
      ]),
    );
  }
}

/// One candidate of the strip: 88 x 66, check toggle, 识别 tag, base-frame star, bottom label.
class _SourceTile extends StatelessWidget {
  const _SourceTile({required this.app, required this.draft, required this.source, required this.onChanged});
  final AppState app;
  final MergeDraft draft;
  final DraftSource source;
  final VoidCallback onChanged;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final ph = app.photoById(source.photoId);
    final isBase = source.checked && draft.effectiveBaseId == source.photoId;
    return SizedBox(
      width: 88,
      child: Column(children: [
        SizedBox(
          width: 88,
          height: 66,
          child: Stack(children: [
            Positioned.fill(
              child: Opacity(
                opacity: source.checked ? 1 : 0.45,
                child: GestureDetector(
                  key: ValueKey('src-${source.photoId}'),
                  behavior: HitTestBehavior.opaque,
                  onTap: () {
                    source.checked = !source.checked;
                    onChanged();
                  },
                  child: Semantics(
                    checked: source.checked,
                    label: '${source.label}，${source.checked ? '已选' : '未选'}',
                    excludeSemantics: true,
                    child: Container(
                      decoration: BoxDecoration(
                          borderRadius: BorderRadius.circular(Rd.thumb),
                          border: source.checked ? Border.all(color: c.accentBtn, width: 2) : null),
                      clipBehavior: Clip.antiAlias,
                      child: ph == null ? ColoredBox(color: c.placeholder) : PhotoArt(ph, cacheWidth: 240),
                    ),
                  ),
                ),
              ),
            ),
            Positioned(
              right: 4,
              top: 4,
              child: IgnorePointer(
                child: Container(
                  width: 20,
                  height: 20,
                  decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      color: source.checked ? c.accentBtn : Fx.overlay35,
                      border: Border.all(color: Colors.white, width: 1.5)),
                  child: source.checked ? const Center(child: DIcon(DI.check, size: 12, color: Colors.white, stroke: 3)) : null,
                ),
              ),
            ),
            if (source.detected)
              Positioned(
                left: 4,
                top: 4,
                child: IgnorePointer(
                  child: Container(
                    padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                    decoration: BoxDecoration(color: Fx.overlay55, borderRadius: BorderRadius.circular(8)),
                    child: Text('识别', style: T.mini(Colors.white, w: FontWeight.w500)),
                  ),
                ),
              ),
            if (source.checked)
              Positioned(
                left: 0,
                bottom: 0,
                child: Semantics(
                  button: true,
                  selected: isBase,
                  label: isBase ? '基准帧' : '设为基准帧',
                  excludeSemantics: true,
                  child: GestureDetector(
                    key: ValueKey('star-${source.photoId}'),
                    behavior: HitTestBehavior.opaque,
                    onTap: () {
                      draft.baseId = source.photoId;
                      onChanged();
                    },
                    child: SizedBox(
                      key: isBase ? ValueKey('base-${source.photoId}') : null,
                      width: 44,
                      height: 44,
                      child: Center(
                        child: Container(
                          width: 26,
                          height: 26,
                          decoration: BoxDecoration(shape: BoxShape.circle, color: isBase ? Fx.star : Fx.overlay55),
                          child: Center(
                              child: DIcon(DI.star, size: 14, color: isBase ? Fx.onStar : Colors.white, filled: isBase, stroke: 1.8)),
                        ),
                      ),
                    ),
                  ),
                ),
              ),
          ]),
        ),
        const SizedBox(height: Sp.s),
        Text(source.label, maxLines: 1, style: T.small(c.text, w: FontWeight.w500, tabular: true)),
      ]),
    );
  }
}
