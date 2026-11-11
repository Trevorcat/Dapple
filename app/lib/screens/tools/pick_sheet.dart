import 'package:flutter/material.dart';

import '../../models.dart';
import '../../services/tool_models.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/photo_art.dart';

/// "从全部照片中选…": large bottom sheet over the processed photos and sequence frames (requirements 4.9.2).
/// 取消 restores the selection from before the sheet opened; 完成 applies it. Returns true when applied.
Future<bool> showSourcePicker(BuildContext context, AppState app, MergeDraft draft) async {
  final r = await showModalBottomSheet<bool>(
    context: context,
    isScrollControlled: true,
    useSafeArea: true,
    backgroundColor: context.c.bg,
    showDragHandle: false,
    shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(Rd.sheet))),
    builder: (ctx) => _PickSheet(app: app, draft: draft),
  );
  return r ?? false;
}

class _PickSheet extends StatefulWidget {
  const _PickSheet({required this.app, required this.draft});
  final AppState app;
  final MergeDraft draft;

  @override
  State<_PickSheet> createState() => _PickSheetState();
}

class _PickSheetState extends State<_PickSheet> {
  /// Selection order (the number in the circle).
  late final List<String> _sel = [for (final s in widget.draft.checked) s.photoId];
  late final List<Photo> _cands = _candidates();

  List<Photo> _candidates() {
    final list = widget.app.pickerCandidates;
    // sources of the draft that are not "done" photos (should not happen) stay selectable
    for (final s in widget.draft.sources) {
      if (!list.any((e) => e.id == s.photoId)) {
        final ph = widget.app.photoById(s.photoId);
        if (ph != null) list.add(ph);
      }
    }
    list.sort((a, b) => a.sortMs.compareTo(b.sortMs));
    return list;
  }

  void _toggle(String id) {
    setState(() {
      if (!_sel.remove(id)) _sel.add(id);
    });
  }

  void _apply() {
    final d = widget.draft;
    final old = {for (final s in d.sources) s.photoId: s};
    final merged = <DraftSource>[];
    final ids = {...old.keys, ..._sel};
    for (final id in ids) {
      final ph = widget.app.photoById(id);
      if (ph == null) continue;
      final o = old[id] ?? DraftSource(photoId: id, label: widget.app.candidateLabel(ph), timeMs: ph.sortMs);
      o.checked = _sel.contains(id);
      merged.add(o);
    }
    merged.sort((a, b) => a.timeMs.compareTo(b.timeMs));
    d.sources
      ..clear()
      ..addAll(merged);
    d.note = null;
    Navigator.of(context).pop(true);
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Column(children: [
      SizedBox(
        height: 64,
        child: Row(children: [
          const SizedBox(width: 4),
          SizedBox(
              width: 80,
              child: Align(
                  alignment: Alignment.centerLeft,
                  child: KeyedSubtree(
                      key: const ValueKey('pick-cancel'), child: TextAction('取消', size: 17, onPressed: () => Navigator.of(context).pop(false))))),
          Expanded(
              child: Text('已选 ${_sel.length} 张',
                  key: const ValueKey('pick-title'), textAlign: TextAlign.center, style: T.body17(c.text, w: FontWeight.w600))),
          SizedBox(
              width: 80,
              child: Align(
                  alignment: Alignment.centerRight,
                  child: KeyedSubtree(key: const ValueKey('pick-done'), child: TextAction('完成', size: 17, bold: true, onPressed: _apply)))),
          const SizedBox(width: 4),
        ]),
      ),
      Expanded(
        child: GridView.builder(
          key: const ValueKey('pick-grid'),
          padding: EdgeInsets.only(bottom: MediaQuery.paddingOf(context).bottom),
          gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
              crossAxisCount: 3, mainAxisSpacing: Sp.grid, crossAxisSpacing: Sp.grid),
          itemCount: _cands.length,
          itemBuilder: (_, i) {
            final ph = _cands[i];
            final n = _sel.indexOf(ph.id) + 1;
            return GestureDetector(
              key: ValueKey('pick-${ph.id}'),
              behavior: HitTestBehavior.opaque,
              onTap: () => _toggle(ph.id),
              child: Semantics(
                button: true,
                selected: n > 0,
                label: '${widget.app.candidateLabel(ph)}${n > 0 ? '，已选第 $n 张' : ''}',
                excludeSemantics: true,
                child: Stack(fit: StackFit.expand, children: [
                  PhotoArt(ph, cacheWidth: 400),
                  if (n > 0) const ColoredBox(color: Color(0x38FFFFFF)),
                  Positioned(right: 6, top: 6, child: SelectDot(number: n)),
                  Positioned(
                    left: 6,
                    bottom: 6,
                    child: Container(
                      height: 20,
                      padding: const EdgeInsets.symmetric(horizontal: 8),
                      alignment: Alignment.center,
                      decoration: BoxDecoration(color: Fx.overlay55, borderRadius: BorderRadius.circular(8)),
                      child: Text(widget.app.candidateLabel(ph), style: T.mini(Colors.white, w: FontWeight.w500)),
                    ),
                  ),
                ]),
              ),
            );
          },
        ),
      ),
    ]);
  }
}
