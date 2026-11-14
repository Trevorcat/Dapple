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
        b.write(photo.developFailed ? '，${photo.failReason}' : '，传输中断');
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
          final act = _failAct();
          app.showToast(photo.failReason, actionLabel: act?.$1, onAction: act?.$2);
        } else {
          app.showToast('传输中断', actionLabel: '重试', onAction: () => app.retry(photo));
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

  /// The step that can help, by what happened (audit F06): 重试 when reading again can work, 移除 when the original is gone or broken,
  /// nothing when the format is not supported (the tile says so).
  (String, VoidCallback)? _failAct() {
    if (!photo.developFailed) return ('重试', () => app.retry(photo));
    return switch (photo.failAction) {
      FailAction.retry => ('重试', () => app.retry(photo)),
      FailAction.remove => ('移除', () => app.removeFromLibrary(photo)),
      FailAction.none => null,
    };
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
              () => LongPressGestureRecognizer(duration: Mo.multiSelect),
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
            ColoredBox(color: Fx.amber.withValues(alpha: 0.18)),
            Positioned.fill(child: DecoratedBox(decoration: BoxDecoration(border: Border.all(color: Fx.amber, width: 3)))),
            // what happened, on the amber pill's own solid ground (audit F18): never bare text on the photo
            Positioned(
              left: 6,
              top: 6,
              right: 6,
              child: Align(
                alignment: Alignment.topLeft,
                child: Container(
                  key: photo.developFailed ? ValueKey('reason-${photo.id}') : null,
                  constraints: const BoxConstraints(minHeight: 20),
                  padding: const EdgeInsets.symmetric(horizontal: Sp.s, vertical: 2),
                  decoration: BoxDecoration(color: Fx.amber, borderRadius: BorderRadius.circular(Rd.thumb)),
                  child: Text(photo.developFailed ? photo.failReason : '传输中断',
                      maxLines: 2, overflow: TextOverflow.ellipsis, style: T.cap(Fx.onAmber, w: FontWeight.w500)),
                ),
              ),
            ),
            if (_failAct() case final act?)
              Center(
                child: Semantics(
                  button: true,
                  label: '${photo.title}，${photo.developFailed ? act.$1 : '重试传输'}',
                  excludeSemantics: true,
                  child: GestureDetector(
                    key: ValueKey('${act.$1 == '移除' ? 'remove' : 'retry'}-${photo.id}'),
                    behavior: HitTestBehavior.opaque,
                    onTap: act.$2,
                    child: Container(
                      width: 64,
                      height: 48,
                      alignment: Alignment.center,
                      decoration: BoxDecoration(color: Fx.amber, borderRadius: BorderRadius.circular(Rd.thumb)),
                      child: Text(act.$1, style: T.body(Fx.onAmber, w: FontWeight.w500)),
                    ),
                  ),
                ),
              ),
          ],
          if (sel) ColoredBox(color: Fx.onPhoto.withValues(alpha: 0.22)),
          // waiting / developing (guidelines 10.12, audit F17): the hollow dot bottom-left says "not final yet"; the spinner bottom-right
          // says it is being worked on now. No word pill, no edge bar.
          // bottom-left: state dot or kind badge
          Positioned(left: 6, bottom: 6, child: _bottomLeft(context)),
          // bottom-right: processing spinner
          if (st == PhotoStatus.developing)
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
          decoration: BoxDecoration(color: accent ? c.accentBtn : Fx.overlay55, borderRadius: BorderRadius.circular(Rd.thumb)),
          child: Text(t, style: T.cap(Fx.onPhoto, w: FontWeight.w500)),
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
              child: const Center(child: DIcon(DI.play, size: 18, color: Fx.onPhoto))),
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
    if (photo.processing) return const Dot(Fx.onPhoto, size: 10, hollow: true);
    if (photo.flag) return const Dot(Fx.amber, size: 10, ring: true);
    if (photo.edit.isAdjusted) return const Dot(Fx.adjusted, size: 10, ring: true);
    return const SizedBox.shrink();
  }
}
