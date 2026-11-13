import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';

import '../../model/catalog.dart';
import '../../model/params.dart';
import '../../models.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/icons.dart';
import '../../widgets/photo_art.dart';
import '../editor/editor_page.dart';

/// One square grid cell with the per-photo state badges of requirements 3.1 / guidelines 10.12.
class PhotoTile extends StatelessWidget {
  const PhotoTile({super.key, required this.app, required this.photo});
  final AppState app;
  final Photo photo;

  String get _tone {
    final id = photo.nearest ?? (photo.demo != null ? toneClassId(demoPhotos[photo.demo!].key, demoPhotos[photo.demo!].range) : null);
    return id == null ? '' : tonePlain(id).replaceAll(' · ', '');
  }

  String _label(int n) {
    final b = StringBuffer(photo.title);
    if (_tone.isNotEmpty) b.write('，$_tone');
    switch (photo.status) {
      case PhotoStatus.arriving:
        b.write('，接收中');
        break;
      case PhotoStatus.pending:
        b.write('，等待处理');
        break;
      case PhotoStatus.developing:
        b.write('，处理中');
        break;
      case PhotoStatus.interrupted:
      case PhotoStatus.failed:
        b.write(photo.developFailed ? '，无法处理，${photo.failReason}' : '，传输中断');
        break;
      case PhotoStatus.done:
        break;
    }
    if (photo.edit.isAdjusted) b.write('，已调整');
    if (photo.flag) b.write('，建议检查');
    if (n > 0) b.write('，已选第 $n 张');
    return b.toString();
  }

  void _tap(BuildContext context) {
    switch (photo.status) {
      case PhotoStatus.arriving:
        app.showToast('正在接收');
        return;
      case PhotoStatus.interrupted:
      case PhotoStatus.failed:
        if (photo.developFailed) {
          app.showToast(photo.failReason, actionLabel: '重试', onAction: () => app.retry(photo));
        } else {
          app.showToast('传输中断，点「重试」');
        }
        return;
      default:
        break;
    }
    if (app.selectMode) {
      app.toggleSelect(photo.id);
    } else {
      openEditor(context, app, photo.id);
    }
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final n = app.selectionNumber(photo.id);
    final sel = n > 0;
    final st = photo.status;
    final broken = st == PhotoStatus.interrupted || st == PhotoStatus.failed;
    final selectable = app.selectable(photo);
    return Semantics(
      label: _label(n),
      button: true,
      selected: sel,
      excludeSemantics: true,
      child: Listener(
        behavior: HitTestBehavior.translucent,
        onPointerDown: (_) {
          if (photo.status == PhotoStatus.done && !app.selectMode) app.sessions.pointerDown(photo);
        },
        onPointerUp: (_) => app.sessions.pointerEnd(photo),
        onPointerCancel: (_) => app.sessions.pointerEnd(photo),
        child: RawGestureDetector(
        key: ValueKey('tile-${photo.id}'),
        behavior: HitTestBehavior.opaque,
        gestures: {
          TapGestureRecognizer: GestureRecognizerFactoryWithHandlers<TapGestureRecognizer>(
              TapGestureRecognizer.new, (r) => r.onTap = () => _tap(context)),
          LongPressGestureRecognizer: GestureRecognizerFactoryWithHandlers<LongPressGestureRecognizer>(
              () => LongPressGestureRecognizer(duration: const Duration(milliseconds: 450)),
              (r) => r.onLongPress = () {
                    if (selectable) app.enterSelect(photo.id);
                  }),
        },
        child: Stack(fit: StackFit.expand, children: [
          PhotoArt(photo, cacheWidth: 400, onMissing: () => app.previewMissing(photo)),
          if (st == PhotoStatus.arriving)
            Center(
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 16),
                child: Column(mainAxisSize: MainAxisSize.min, children: [
                  Text('接收中 ${(photo.progress * 100).round()}%', style: T.small(c.text2, tabular: true)),
                  const SizedBox(height: 8),
                  DProgress(photo.progress, height: 3),
                ]),
              ),
            ),
          if (broken) ...[
            const ColoredBox(color: Color(0x2EF0A020)),
            Positioned.fill(child: DecoratedBox(decoration: BoxDecoration(border: Border.all(color: Fx.amber, width: 3)))),
            Positioned(
              left: 6,
              top: 6,
              child: Container(
                height: 20,
                padding: const EdgeInsets.symmetric(horizontal: 8),
                alignment: Alignment.center,
                decoration: BoxDecoration(color: Fx.amber, borderRadius: BorderRadius.circular(8)),
                child: Text(photo.developFailed ? '无法处理' : '传输中断', style: T.mini(Fx.onAmber, w: FontWeight.w500)),
              ),
            ),
            if (photo.developFailed)
              Positioned(
                left: 8,
                right: 8,
                bottom: 8,
                child: Text(photo.failReason,
                    key: ValueKey('reason-${photo.id}'), maxLines: 2, overflow: TextOverflow.ellipsis, textAlign: TextAlign.center, style: T.mini(Colors.black87)),
              ),
            Center(
              child: Semantics(
                button: true,
                label: '${photo.title}，${photo.developFailed ? '重试' : '重试传输'}',
                excludeSemantics: true,
                child: GestureDetector(
                  key: ValueKey('retry-${photo.id}'),
                  behavior: HitTestBehavior.opaque,
                  onTap: () => app.retry(photo),
                  child: Container(
                    width: 64,
                    height: 48,
                    alignment: Alignment.center,
                    decoration: BoxDecoration(color: Fx.amber, borderRadius: BorderRadius.circular(8)),
                    child: Text('重试', style: T.body(Fx.onAmber, w: FontWeight.w500)),
                  ),
                ),
              ),
            ),
          ],
          if (sel) const ColoredBox(color: Color(0x38FFFFFF)),
          // developing: a thin moving bar along the bottom edge, with the word on the tile (queued tiles say so too)
          if (photo.processing && photo.kind == PhotoKind.photo) ...[
            Positioned(
              left: 0,
              right: 0,
              bottom: 0,
              child: LinearProgressIndicator(
                  key: ValueKey('progress-${photo.id}'),
                  minHeight: 3,
                  color: c.accent,
                  backgroundColor: Colors.black12,
                  value: st == PhotoStatus.pending ? 0 : null),
            ),
            Positioned(
              left: 6,
              top: 6,
              child: Container(
                height: 20,
                padding: const EdgeInsets.symmetric(horizontal: 8),
                alignment: Alignment.center,
                decoration: BoxDecoration(color: Fx.overlay55, borderRadius: BorderRadius.circular(8)),
                child: Text(st == PhotoStatus.pending ? '等待处理' : '处理中', style: T.mini(Colors.white, w: FontWeight.w500)),
              ),
            ),
          ],
          // bottom-left: state dot or kind badge
          Positioned(left: 6, bottom: 6, child: _bottomLeft(context)),
          // bottom-right: processing spinner
          if (photo.processing)
            Positioned(
              right: 6,
              bottom: 6,
              child: Container(
                width: 22,
                height: 22,
                alignment: Alignment.center,
                decoration: const BoxDecoration(color: Fx.overlay60, shape: BoxShape.circle),
                child: const Spinner(size: 16),
              ),
            ),
          if (app.selectMode && selectable) Positioned(right: 6, top: 6, child: SelectDot(number: n)),
        ]),
      ),
      ),
    );
  }

  Widget _bottomLeft(BuildContext context) {
    final c = context.c;
    Widget pill(String t, {bool accent = false}) => Container(
          height: 20,
          padding: const EdgeInsets.symmetric(horizontal: 8),
          alignment: Alignment.center,
          decoration: BoxDecoration(color: accent ? c.accentBtn : Fx.overlay55, borderRadius: BorderRadius.circular(8)),
          child: Text(t, style: T.mini(Colors.white, w: FontWeight.w500)),
        );
    switch (photo.kind) {
      case PhotoKind.bracket:
      case PhotoKind.frame:
        return pill(photo.badge ?? '');
      case PhotoKind.video:
        return Row(mainAxisSize: MainAxisSize.min, children: [
          Container(
              width: 36,
              height: 36,
              decoration: const BoxDecoration(color: Fx.overlay55, shape: BoxShape.circle),
              child: const Center(child: DIcon(DI.play, size: 18, color: Colors.white))),
          const SizedBox(width: 6),
          pill(photo.badge ?? ''),
        ]);
      case PhotoKind.hdr:
      case PhotoKind.pano:
      case PhotoKind.live:
        return pill(photo.badge ?? '', accent: true);
      case PhotoKind.photo:
        break;
    }
    if (photo.processing) return const Dot(Colors.white, size: 10, hollow: true);
    if (photo.flag) return const Dot(Fx.amber, size: 10, ring: true);
    if (photo.edit.isAdjusted) return const Dot(Fx.adjusted, size: 10, ring: true);
    return const SizedBox.shrink();
  }
}
