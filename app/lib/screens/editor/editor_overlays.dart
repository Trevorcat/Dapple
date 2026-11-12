// Floating chrome of the editor: top bar, category dock, transient readout, histogram window
// (requirements 4.15.2 / 4.15.3 / 4.15.9, guidelines 15.2).
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import '../../editor/springs.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/icons.dart';
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

class EditorTopBar extends StatelessWidget {
  const EditorTopBar({
    super.key,
    required this.app,
    required this.photoId,
    required this.onBack,
    required this.onMore,
  });
  final AppState app;
  final String Function() photoId;
  final VoidCallback onBack;
  final VoidCallback onMore;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return ListenableBuilder(
      listenable: app,
      builder: (context, _) {
        final id = photoId();
        PerfProbe.count('topbar');
        final saving = app.isSaving(id);
        final done = app.photoById(id)?.exported ?? false;
        return Row(
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
            const Spacer(),
            GlassCapsule(
              radius: 24,
              child: Row(
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
          ],
        );
      },
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
        child: Text('原片', style: T.body(Colors.white, w: FontWeight.w600)),
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
