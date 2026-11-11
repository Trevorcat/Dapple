// 同步设置 dialog (requirements 4.8): merge (not overwrite) the source's overrides.
import 'package:flutter/material.dart';

import '../../model/params.dart';
import '../../models.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/icons.dart';
import '../../widgets/photo_art.dart';

/// Medium dialog button (48 high, radius 24): secondary chip on the left, primary on the right.
class DialogButton extends StatelessWidget {
  const DialogButton(this.label, {super.key, this.onPressed, this.primary = false, this.danger = false});
  final String label;
  final VoidCallback? onPressed;
  final bool primary, danger;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final bg = danger ? c.danger : (primary ? c.accentBtn : c.chip);
    final fg = (danger || primary) ? Colors.white : c.text;
    return Semantics(
      button: true,
      enabled: onPressed != null,
      label: label,
      excludeSemantics: true,
      child: Opacity(
        opacity: onPressed == null ? 0.4 : 1,
        child: Material(
          color: bg,
          borderRadius: BorderRadius.circular(Rd.ctaMd),
          child: InkWell(
            borderRadius: BorderRadius.circular(Rd.ctaMd),
            onTap: onPressed,
            child: Container(
              height: 48,
              alignment: Alignment.center,
              child: Text(label, style: T.body(fg, w: FontWeight.w500)),
            ),
          ),
        ),
      ),
    );
  }
}

Future<void> showSyncDialog(BuildContext context, AppState app, Photo src) {
  final n = DateTime.fromMillisecondsSinceEpoch(src.addedMs);
  final targets = [
    for (final p in app.editable)
      if (p.id != src.id && p.status == PhotoStatus.done && _sameDay(DateTime.fromMillisecondsSinceEpoch(p.addedMs), n)) p
  ];
  // Material modal bottom sheet (drag handle + close icon), not an iOS-style cancel/confirm dialog.
  return showModalBottomSheet<void>(
    context: context,
    isScrollControlled: true,
    showDragHandle: true,
    backgroundColor: context.c.surface,
    barrierColor: context.c.scrim,
    constraints: const BoxConstraints(maxWidth: 560),
    shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(Rd.sheet))),
    builder: (_) => _SyncDialog(app: app, src: src, targets: targets),
  );
}

bool _sameDay(DateTime a, DateTime b) => a.year == b.year && a.month == b.month && a.day == b.day;

class _SyncDialog extends StatefulWidget {
  const _SyncDialog({required this.app, required this.src, required this.targets});
  final AppState app;
  final Photo src;
  final List<Photo> targets;
  @override
  State<_SyncDialog> createState() => _SyncDialogState();
}

class _SyncDialogState extends State<_SyncDialog> {
  late final Set<String> picked = {for (final p in widget.targets) p.id};
  final Set<SyncCat> cats = {...SyncCat.values}..remove(SyncCat.crop);

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final n = picked.length;
    final bottom = MediaQuery.paddingOf(context).bottom;
    return Semantics(
      explicitChildNodes: true,
      scopesRoute: true,
      namesRoute: true,
      label: '同步设置',
      child: SingleChildScrollView(
          padding: EdgeInsets.fromLTRB(24, 0, 24, 16 + bottom),
          child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
            Row(children: [
              Transform.translate(
                  offset: const Offset(-12, 0),
                  child: DIconButton(DI.close,
                      key: const ValueKey('sync-cancel'), label: '关闭', onPressed: () => Navigator.pop(context))),
              Expanded(
                child: Transform.translate(
                  offset: const Offset(-12, 0),
                  child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                    Text('同步设置', style: T.h2(c.text)),
                    Text(n == 0 ? '没有选中的照片' : '同步到 $n 张', key: const ValueKey('sync-subtitle'), style: T.small(c.text2)),
                  ]),
                ),
              ),
            ]),
            const SizedBox(height: 16),
            SizedBox(
              height: 48,
              child: ListView(scrollDirection: Axis.horizontal, children: [
                for (final p in widget.targets)
                  Semantics(
                    checked: picked.contains(p.id),
                    label: p.title,
                    excludeSemantics: true,
                    child: GestureDetector(
                      key: ValueKey('sync-target-${p.id}'),
                      behavior: HitTestBehavior.opaque,
                      onTap: () => setState(() => picked.contains(p.id) ? picked.remove(p.id) : picked.add(p.id)),
                      child: Padding(
                        padding: const EdgeInsets.only(right: 8, top: 2, bottom: 2),
                        child: Opacity(
                          opacity: picked.contains(p.id) ? 1 : 0.4,
                          child: SizedBox(
                            width: 44,
                            height: 44,
                            child: Stack(fit: StackFit.expand, children: [
                              ClipRRect(borderRadius: BorderRadius.circular(8), child: PhotoArt(p, cacheWidth: 120)),
                              if (picked.contains(p.id))
                                Positioned(
                                    right: 2,
                                    top: 2,
                                    child: Container(
                                        width: 14,
                                        height: 14,
                                        decoration: BoxDecoration(color: c.accentBtn, shape: BoxShape.circle, border: Border.all(color: Colors.white, width: 1.5)))),
                            ]),
                          ),
                        ),
                      ),
                    ),
                  ),
              ]),
            ),
            const SizedBox(height: 8),
            for (final k in SyncCat.values)
              Semantics(
                checked: cats.contains(k),
                label: syncCatLabels[k],
                excludeSemantics: true,
                child: GestureDetector(
                  key: ValueKey('sync-cat-${k.name}'),
                  behavior: HitTestBehavior.opaque,
                  onTap: () => setState(() => cats.contains(k) ? cats.remove(k) : cats.add(k)),
                  child: SizedBox(
                    height: 48,
                    child: Row(children: [
                      DCheckbox(value: cats.contains(k)),
                      const SizedBox(width: 16),
                      Text(syncCatLabels[k]!, style: T.body17(c.text)),
                    ]),
                  ),
                ),
              ),
            const SizedBox(height: 16),
            DialogButton('同步',
                key: const ValueKey('sync-confirm'),
                primary: true,
                onPressed: n == 0
                    ? null
                    : () {
                        Navigator.pop(context);
                        widget.app.syncSettings(widget.src.id, picked.toList(), {...cats});
                      }),
          ]),
      ),
    );
  }
}
