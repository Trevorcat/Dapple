import 'dart:io';
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../model/catalog.dart';
import '../model/params.dart';
import '../models.dart';
import '../services/editor_engine.dart';
import '../state/app_state.dart';
import '../theme/tokens.dart';

/// Paints a [DemoScene] (procedural stand-in for a photo: tests, screenshots).
class DemoPainter extends CustomPainter {
  DemoPainter(this.scene);
  final DemoScene scene;

  @override
  void paint(Canvas canvas, Size size) {
    final rect = Offset.zero & size;
    canvas.clipRect(rect);
    final a = scene.angle * math.pi / 180;
    final dx = math.sin(a), dy = -math.cos(a);
    canvas.drawRect(
        rect,
        Paint()
          ..shader = LinearGradient(
            begin: Alignment(-dx, -dy),
            end: Alignment(dx, dy),
            colors: scene.colors,
            stops: scene.stops,
          ).createShader(rect));
    for (final v in scene.vbands) {
      canvas.drawRect(Rect.fromLTRB(v.$1 * size.width, 0, v.$2 * size.width, size.height), Paint()..color = v.$3);
    }
    for (final e in scene.ellipses) {
      canvas.drawOval(
          Rect.fromCenter(
              center: Offset(e.$1 * size.width, e.$2 * size.height),
              width: e.$3 * 2 * size.width,
              height: e.$4 * 2 * size.height),
          Paint()..color = e.$5);
    }
    for (final d in scene.dots) {
      final r = d.$3 * size.width;
      canvas.drawCircle(Offset(d.$1 * size.width, d.$2 * size.height), r, Paint()..color = d.$4);
    }
  }

  @override
  bool shouldRepaint(DemoPainter o) => o.scene != scene;
}

/// Edit state bound to the global defaults, for the demo approximation.
class DemoEdit {
  const DemoEdit(this.edit, this.defaultLook, this.defaultTone);
  final EditState edit;
  final String defaultLook;
  final double defaultTone;

  factory DemoEdit.of(AppState app, Photo p) => DemoEdit(p.edit, app.settings.defaultLookId, app.settings.toneStrength);

  DemoLook look(DemoPhotoSpec spec, bool original) => demoLookFor(edit,
      defaultToneStrength: defaultTone, defaultLook: defaultLook, key: spec.key, range: spec.range, original: original);
}

/// The picture of a photo: preview file > demo scene > placeholder. In the editor the engine image
/// is drawn by the editor itself; [demoEdit] only drives the approximate demo filter.
class PhotoArt extends StatelessWidget {
  const PhotoArt(this.photo,
      {super.key, this.fit = BoxFit.cover, this.cacheWidth, this.demoEdit, this.original = false});
  final Photo photo;
  final BoxFit fit;
  final int? cacheWidth;

  /// Demo only: render with this edit state's approximation (editor).
  final DemoEdit? demoEdit;
  final bool original;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    if (photo.status == PhotoStatus.arriving) return ColoredBox(color: c.placeholder);
    final path = photo.previewPath;
    if (path != null) {
      return Image.file(File(path),
          fit: fit,
          cacheWidth: cacheWidth,
          gaplessPlayback: true,
          errorBuilder: (_, _, _) => ColoredBox(color: c.placeholder));
    }
    final di = photo.demo;
    if (di != null) {
      final spec = demoPhotos[di];
      Widget w = CustomPaint(painter: DemoPainter(spec.scene), size: Size.infinite);
      if (demoEdit != null || original) {
        final l = (demoEdit ?? const DemoEdit(EditState.empty, 'std', 60)).look(spec, original);
        w = ColorFiltered(
            colorFilter: approxFilter(b: l.b, c: l.c, s: l.s, warm: l.warm, gray: l.gray, sepia: l.sepia), child: w);
      }
      return w;
    }
    return ColoredBox(color: c.placeholder);
  }
}
