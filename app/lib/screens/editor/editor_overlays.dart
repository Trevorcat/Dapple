// Floating chrome of the editor: top bar, category dock, transient readout, histogram window
// (requirements 4.15.2 / 4.15.3 / 4.15.9, guidelines 15.2).
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import '../../editor/springs.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/icons.dart';
import '../../model/params.dart' show EGroup;
import 'editor_controller.dart';
import '../../services/perf_probe.dart';

BoxDecoration glassDeco(DC c, double radius) => BoxDecoration(
  color: c.glass,
  borderRadius: BorderRadius.circular(radius),
  border: Border.all(color: c.sep),
);

/// Frosted capsule (guidelines 1.4: floating navigation bars may use glass).
class GlassCapsule extends StatelessWidget {
  const GlassCapsule({super.key, required this.radius, required this.child});
  final double radius;
  final Widget child;
  @override
  Widget build(BuildContext context) => ClipRRect(
    borderRadius: BorderRadius.circular(radius),
    child: BackdropFilter(
      filter: ui.ImageFilter.blur(sigmaX: 20, sigmaY: 20),
      child: DecoratedBox(
        decoration: glassDeco(context.c, radius),
        child: child,
      ),
    ),
  );
}

// ===================================================================================== top bar

/// Width of the top bar standing as a rail beside the photo (one 48 dp button).
const double kTopRailW = 48;

class EditorTopBar extends StatelessWidget {
  const EditorTopBar({
    super.key,
    required this.app,
    required this.photoId,
    required this.onBack,
    required this.onMore,
    this.ctl,
    this.vertical = false,
    this.immersive = false,
  });

  /// The editor: when set, the bar carries the crop button (裁切's entry; it is not a dock category).
  final EditorController? ctl;

  /// Immersive view: the action group fades out, the back button stays and works.
  final bool immersive;

  /// Beside the side pane (landscape, the unfolded inner screen): a rail on the photo column's outer edge, back on top, so the photo
  /// can take the full height (audit F25).
  final bool vertical;
  final AppState app;
  final String Function() photoId;
  final VoidCallback onBack;
  final VoidCallback onMore;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final ed = ctl;
    return ListenableBuilder(
      listenable: Listenable.merge([app, ?ed?.wm, ?ed?.edits]),
      builder: (context, _) {
        final id = photoId();
        PerfProbe.count('topbar');
        final saving = app.isSaving(id);
        final done = app.photoById(id)?.exported ?? false;
        return Flex(
          direction: vertical ? Axis.vertical : Axis.horizontal,
          mainAxisSize: vertical ? MainAxisSize.min : MainAxisSize.max,
          children: [
            GlassCapsule(
              radius: 24,
              child: DIconButton(
                DI.back,
                key: const ValueKey('toolbar-back'),
                label: '返回',
                onPressed: onBack,
              ),
            ),
            if (vertical) const SizedBox(height: 8) else const Spacer(),
            // the tools: 裁切, its own capsule beside the history and output group
            if (ed != null)
              IgnorePointer(
                ignoring: immersive,
                child: AnimatedOpacity(
                  opacity: immersive ? 0 : 1,
                  duration: const Duration(milliseconds: 150),
                  child: GlassCapsule(
                    radius: 24,
                    child: Flex(
                      direction: vertical ? Axis.vertical : Axis.horizontal,
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        CropButton(ctl: ed, app: app, photoId: id),
                        AutoToggle(ctl: ed, app: app, photoId: id, vertical: vertical),
                      ],
                    ),
                  ),
                ),
              ),
            if (ed != null) vertical ? const SizedBox(height: 8) : SizedBox(width: MediaQuery.sizeOf(context).width < 380 ? 4 : 8), // 360 dp: back, 裁切 + 自动, history just fit
            IgnorePointer(
              ignoring: immersive,
              child: AnimatedOpacity(
              opacity: immersive ? 0 : 1,
              duration: const Duration(milliseconds: 150),
              child: GlassCapsule(
              radius: 24,
              child: Flex(
                direction: vertical ? Axis.vertical : Axis.horizontal,
                mainAxisSize: MainAxisSize.min,
                children: [
                  DIconButton(
                    DI.undo,
                    key: const ValueKey('toolbar-undo'),
                    label: '撤销',
                    onPressed: app.canUndo(id) ? () => app.undo(id) : null,
                  ),
                  DIconButton(
                    DI.redo,
                    key: const ValueKey('toolbar-redo'),
                    label: '重做',
                    onPressed: app.canRedo(id) ? () => app.redo(id) : null,
                  ),
                  if (saving)
                    Semantics(
                      label: '正在保存',
                      excludeSemantics: true,
                      child: SizedBox(
                        key: const ValueKey('toolbar-save'),
                        width: 48,
                        height: 48,
                        child: Center(
                          child: SizedBox(
                            width: 22,
                            height: 22,
                            child: CircularProgressIndicator(
                              strokeWidth: 2.4,
                              color: c.accent,
                            ),
                          ),
                        ),
                      ),
                    )
                  else
                    DIconButton(
                      DI.save,
                      key: const ValueKey('toolbar-save'),
                      label: done ? '已保存到相册，再存一次' : '保存到相册',
                      color: done ? c.okIcon : null,
                      onPressed: () {
                        final p = app.photoById(id);
                        if (p != null) app.saveOneToAlbum(p);
                      },
                    ),
                  DIconButton(
                    DI.more,
                    key: const ValueKey('toolbar-more'),
                    label: '更多',
                    onPressed: onMore,
                  ),
                ],
              ),
            ),
            ),
            ),
          ],
        );
      },
    );
  }
}

/// The top bar's 自动 button (per photo, default on): the engine's automatic correction under the edit. An icon button like 裁切: on is the
/// selected look (`accentSoft` disc, `accent` icon), off a plain icon. A tap says what happened in a short toast. With 照片到达后自动处理 off in
/// the settings there is no correction to switch: the control stays in place, off and dimmed like an unavailable history button, and says why
/// to a screen reader.
class AutoToggle extends StatelessWidget {
  const AutoToggle({super.key, required this.ctl, required this.app, required this.photoId, this.vertical = false});
  final EditorController ctl;
  final AppState app;
  final String photoId;
  final bool vertical;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final global = app.settings.autoProcess;
    final cur = app.photoById(photoId)?.edit.auto ?? true;
    final on = global && cur;
    return Semantics(
      button: true,
      toggled: on,
      enabled: global,
      label: global ? '自动修正' : '自动修正，已在设置中关闭',
      hint: global ? null : '在设置中打开“照片到达后自动处理”',
      excludeSemantics: true,
      child: GestureDetector(
        key: const ValueKey('toolbar-auto'),
        behavior: HitTestBehavior.opaque,
        onTap: global
            ? () {
                ctl.setAuto(!cur);
                app.showToast(cur ? '已关闭自动修正' : '已打开自动修正');
              }
            : null,
        child: SizedBox(
          width: !vertical && MediaQuery.sizeOf(context).width < 380 ? kHit - 4 : kHit, // 360 dp: back, 裁切 + 自动, history just fit
          height: kHit,
          child: Center(
            child: Opacity(
              opacity: global ? 1 : 0.4,
              child: Container(
                width: 40,
                height: 40,
                decoration: BoxDecoration(color: on ? c.accentSoft : null, shape: BoxShape.circle),
                alignment: Alignment.center,
                child: DIcon(DI.auto, color: on ? c.accent : c.text),
              ),
            ),
          ),
        ),
      ),
    );
  }
}

/// The top bar's 裁切 button. Open: the item's selected look (`accentSoft` disc, `accent` icon), the same as a selected dock item.
/// A photo whose crop, rotation, flip or straightening differs from the original: the dock's changed mark, a small `accent` dot at the
/// icon's top right corner.
class CropButton extends StatelessWidget {
  const CropButton({super.key, required this.ctl, required this.app, required this.photoId});
  final EditorController ctl;
  final AppState app;
  final String photoId;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final open = ctl.cropOpen;
    final cropped = app.photoById(photoId)?.edit.groupAdjusted(EGroup.crop) ?? false;
    return SizedBox(
      width: kHit,
      height: kHit,
      child: Stack(clipBehavior: Clip.none, children: [
        DIconButton(
          DI.crop,
          key: const ValueKey('toolbar-crop'),
          label: cropped ? '裁切，已裁切' : '裁切',
          pressed: open,
          color: open ? c.accent : null,
          bg: open ? c.accentSoft : null,
          onPressed: ctl.toggleCrop,
        ),
        if (cropped)
          Positioned(
            left: 36,
            top: 10,
            child: IgnorePointer(child: Container(key: const ValueKey('toolbar-crop-mark'), width: 6, height: 6, decoration: BoxDecoration(color: c.accent, shape: BoxShape.circle))),
          ),
      ]),
    );
  }
}

// ===================================================================================== readout

/// The transient pill over the photo: "原片" while comparing, or a short text such as the "n / N" photo counter.
class ReadoutOverlay extends StatefulWidget {
  const ReadoutOverlay({super.key, required this.controller});
  final EditorController controller;

  @override
  State<ReadoutOverlay> createState() => _ReadoutOverlayState();
}

class _ReadoutOverlayState extends State<ReadoutOverlay> with SingleTickerProviderStateMixin {
  late final SpringValue _fade = SpringValue(this);
  ReadoutModel get r => widget.controller.readout;

  @override
  void initState() {
    super.initState();
    r.addListener(_sync);
    _sync();
  }

  @override
  void dispose() {
    r.removeListener(_sync);
    _fade.dispose();
    super.dispose();
  }

  void _sync() {
    final target = r.visible ? (r.dimmed ? 0.4 : 1.0) : 0.0;
    if (target != _fade.target) {
      _fade.animateTo(target, spring: SpringTier.small.opacity);
    }
    if (mounted) setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    return IgnorePointer(
      child: AnimatedBuilder(
        animation: _fade,
        builder: (context, _) {
          final o = _fade.value.clamp(0.0, 1.0);
          if (o < 0.01) return const SizedBox.shrink();
          return RepaintBoundary(child: Opacity(opacity: o, child: _content(context)));
        },
      ),
    );
  }

  Widget _content(BuildContext context) {
    if (r.compare) {
      return Container(
        key: const ValueKey('original-pill'),
        padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 6),
        decoration: BoxDecoration(color: Fx.overlay60, borderRadius: BorderRadius.circular(14)),
        child: Text(r.compareText, style: T.body(Fx.onPhoto, w: FontWeight.w600)),
      );
    }
    return Container(
      key: const ValueKey('readout'),
      padding: const EdgeInsets.fromLTRB(14, 8, 14, 8),
      decoration: BoxDecoration(color: Fx.overlay60, borderRadius: BorderRadius.circular(14)),
      child: Text(r.name, style: T.body(Colors.white, w: FontWeight.w600)),
    );
  }
}
