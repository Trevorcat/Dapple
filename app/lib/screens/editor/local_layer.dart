// The "editing an area" layer over the photo (docs/design/local-adjustments.md sections 3 and 6). It lives INSIDE the ZoomView, in the
// frame's own dp space, so the zoom / pan transform of the photo applies to it as well: handles stay on the picture under any zoom, and
// the pointer positions it gets are already frame positions. One finger edits the selected part (handles, painting, picking); a
// second finger hands over to the editor's arbiter, which zooms and pans (two fingers are always zoom / pan).
//
// Drawing: the red overlay is an engine image (dapple_session_mask_preview, tinted off the UI thread) shown with an opacity fade; the
// handles are one CustomPainter that repaints on the edit tick and the layer's own tick; handle sizes are divided by the zoom so
// they keep their screen size.
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import '../../editor/mask_geometry.dart';
import '../../model/local_adjust.dart';
import 'local_controller.dart';
import 'zoom_view.dart';

class LocalPhotoLayer extends StatefulWidget {
  const LocalPhotoLayer({super.key, required this.lc, required this.zoom});
  final LocalController lc;
  final ZoomController zoom;

  @override
  State<LocalPhotoLayer> createState() => _LocalPhotoLayerState();
}

class _LocalPhotoLayerState extends State<LocalPhotoLayer> {
  int? _a, _b;
  bool _multi = false;

  LocalController get lc => widget.lc;

  int _ms(PointerEvent e) => e.timeStamp.inMilliseconds;

  void _down(PointerDownEvent e) {
    lc.zoomScale = widget.zoom.scale;
    if (_a == null) {
      _a = e.pointer;
      _multi = false;
      lc.pointerDown(e.localPosition, _ms(e));
    } else if (_b == null) {
      _b = e.pointer;
      if (!_multi) lc.secondFinger();
      _multi = true;
    }
  }

  void _move(PointerMoveEvent e) {
    if (e.pointer == _a && !_multi) lc.pointerMove(e.localPosition, _ms(e));
  }

  void _up(PointerEvent e, {required bool cancelled}) {
    if (e.pointer == _a) {
      if (!_multi) cancelled ? lc.pointerCancel() : lc.pointerUp(e.localPosition, _ms(e));
      _a = _b;
      _b = null;
    } else if (e.pointer == _b) {
      _b = null;
    }
    if (_a == null && _b == null) _multi = false;
  }

  @override
  Widget build(BuildContext context) {
    final dpr = MediaQuery.devicePixelRatioOf(context);
    return LayoutBuilder(builder: (context, cons) {
      final size = Size(cons.maxWidth, cons.maxHeight);
      lc.frameSize = size;
      lc.frameDevicePx = math.max(size.width, size.height) * dpr;
      return Listener(
        behavior: HitTestBehavior.opaque,
        onPointerDown: _down,
        onPointerMove: _move,
        onPointerUp: (e) => _up(e, cancelled: false),
        onPointerCancel: (e) => _up(e, cancelled: true),
        child: Stack(fit: StackFit.expand, clipBehavior: Clip.none, children: [
          IgnorePointer(
            child: ListenableBuilder(
              listenable: Listenable.merge([lc.overlayOn, lc.overlayStale]),
              builder: (context, _) => AnimatedOpacity(
                opacity: lc.overlayOn.value && !lc.overlayStale.value ? 1 : 0,
                duration: const Duration(milliseconds: 150),
                child: ValueListenableBuilder<ui.Image?>(
                  valueListenable: lc.overlay,
                  builder: (context, img, _) => img == null
                      ? const SizedBox.shrink()
                      : RawImage(key: const ValueKey('local-overlay'), image: img, fit: BoxFit.fill, filterQuality: FilterQuality.medium),
                ),
              ),
            ),
          ),
          IgnorePointer(
            child: RepaintBoundary(
              child: CustomPaint(
                key: const ValueKey('local-handles'),
                painter: LocalHandlePainter(lc, widget.zoom, Listenable.merge([lc.ctl.app.editTick, lc.tick, widget.zoom, lc])),
              ),
            ),
          ),
        ]),
      );
    });
  }
}

/// The selected part's handles (and the other parts of the area, faint), the stroke being painted, the picked point.
class LocalHandlePainter extends CustomPainter {
  LocalHandlePainter(this.lc, this.zoom, Listenable repaint) : super(repaint: repaint);
  final LocalController lc;
  final ZoomController zoom;

  @override
  void paint(Canvas canvas, Size size) {
    if (!lc.active || lc.ctl.compare.value) return;
    final z = math.max(1.0, zoom.scale);
    final a = lc.sel;
    final s = HandleSpace(lc.map, size);
    final diag = size.longestSide * 3;

    // a white line over a soft dark one: readable on any photo
    void both(void Function(Paint p) draw, {double alpha = 1}) {
      draw(Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 4 / z
        ..strokeCap = StrokeCap.round
        ..color = Color.fromRGBO(0, 0, 0, 0.35 * alpha));
      draw(Paint()
        ..style = PaintingStyle.stroke
        ..strokeWidth = 2 / z
        ..strokeCap = StrokeCap.round
        ..color = Colors.white.withValues(alpha: alpha));
    }

    void dashed(Path p, Paint paint) {
      for (final m in p.computeMetrics()) {
        var d = 0.0;
        while (d < m.length) {
          canvas.drawPath(m.extractPath(d, math.min(d + 7 / z, m.length)), paint);
          d += 12 / z;
        }
      }
    }

    void dot(Offset c, double r, {bool ring = false}) {
      canvas.drawCircle(c + Offset(0, 1 / z), r + 1 / z, Paint()..color = const Color(0x66000000));
      canvas.drawCircle(c, r, Paint()..color = ring ? const Color(0x33FFFFFF) : Colors.white);
      if (ring) canvas.drawCircle(c, r, Paint()..style = PaintingStyle.stroke..strokeWidth = 2 / z..color = Colors.white);
    }

    if (a != null) {
      for (var i = 0; i < a.mask.length; i++) {
        final m = a.mask[i];
        final selected = i == lc.selComp.clamp(0, a.mask.length - 1);
        final alpha = selected ? 1.0 : 0.45;
        switch (m.kind) {
          case MaskKindId.linear:
            final h = LinearHandles(s, m);
            // the lines end at the picture's edge; the knobs may sit beyond it
            canvas.save();
            canvas.clipRect(Offset.zero & size);
            both((p) => canvas.drawLine(h.p0 - h.along * diag, h.p0 + h.along * diag, p), alpha: alpha);
            final far = Path()
              ..moveTo((h.p1 - h.along * diag).dx, (h.p1 - h.along * diag).dy)
              ..lineTo((h.p1 + h.along * diag).dx, (h.p1 + h.along * diag).dy);
            both((p) => dashed(far, p), alpha: alpha);
            canvas.restore();
            if (selected) {
              both((p) => canvas.drawLine(h.p0, h.p1, p..strokeWidth = p.strokeWidth * 0.6), alpha: 0.7);
              dot(h.mid, 6 / z);
              final k = h.knob(56 / z);
              dot(k, 9 / z, ring: true);
              _turnArrow(canvas, k, 5 / z, z);
            }
          case MaskKindId.radial:
            final h = RadialHandles(s, m);
            Path ell(double scale) {
              final p = Path();
              for (var t = 0; t <= 72; t++) {
                final q = h.at(t / 72 * 2 * math.pi, scale: scale);
                t == 0 ? p.moveTo(q.dx, q.dy) : p.lineTo(q.dx, q.dy);
              }
              return p..close();
            }

            canvas.save();
            canvas.clipRect(Offset.zero & size);
            both((p) => canvas.drawPath(ell(h.inner.clamp(0.02, 1.0)), p), alpha: alpha);
            both((p) => dashed(ell(1), p), alpha: alpha);
            canvas.restore();
            if (selected) {
              for (final q in h.sizeHandles()) {
                dot(q, 6 / z);
              }
              final k = h.knob(28 / z);
              both((p) => canvas.drawLine(h.centre - h.v * h.ryDp, k, p..strokeWidth = p.strokeWidth * 0.6));
              dot(k, 9 / z, ring: true);
              _turnArrow(canvas, k, 5 / z, z);
              dot(h.centre, 3.5 / z);
            }
          default:
            break;
        }
      }
    }
    // the stroke being painted (before the engine's overlay has it)
    final st = lc.liveStroke;
    if (st.isNotEmpty) {
      final p = Path()..moveTo(st.first.dx, st.first.dy);
      for (final q in st.skip(1)) {
        p.lineTo(q.dx, q.dy);
      }
      if (st.length == 1) p.lineTo(st.first.dx + 0.01, st.first.dy);
      canvas.drawPath(
          p,
          Paint()
            ..style = PaintingStyle.stroke
            ..strokeWidth = lc.liveRadius * 2
            ..strokeCap = StrokeCap.round
            ..strokeJoin = StrokeJoin.round
            ..color = lc.liveErase ? const Color(0x80FFFFFF) : const Color(0x73FF3B30));
      // the brush ring under the finger
      final last = st.last;
      canvas.drawCircle(last, lc.liveRadius, Paint()..style = PaintingStyle.stroke..strokeWidth = 3 / z..color = const Color(0x59000000));
      canvas.drawCircle(last, lc.liveRadius, Paint()..style = PaintingStyle.stroke..strokeWidth = 1.5 / z..color = Colors.white);
    }
    final pk = lc.pickedAt;
    final c = lc.comp;
    if (pk != null && c != null && (c.kind == MaskKindId.luminance || c.kind == MaskKindId.color)) {
      canvas.drawCircle(pk, 13 / z, Paint()..style = PaintingStyle.stroke..strokeWidth = 4 / z..color = const Color(0x59000000));
      canvas.drawCircle(pk, 13 / z, Paint()..style = PaintingStyle.stroke..strokeWidth = 2 / z..color = Colors.white);
      canvas.drawCircle(pk, 2.5 / z, Paint()..color = Colors.white);
    }
  }

  /// A small circular arrow inside the rotate knob.
  void _turnArrow(Canvas canvas, Offset c, double r, double z) {
    final p = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.6 / z
      ..strokeCap = StrokeCap.round
      ..color = Colors.white;
    canvas.drawArc(Rect.fromCircle(center: c, radius: r), -math.pi * 0.9, math.pi * 1.4, false, p);
    final end = c + Offset(math.cos(math.pi * 0.5), math.sin(math.pi * 0.5)) * r;
    canvas.drawLine(end, end + Offset(-2.5 / z, -0.5 / z), p);
    canvas.drawLine(end, end + Offset(0.5 / z, -2.6 / z), p);
  }

  @override
  bool shouldRepaint(LocalHandlePainter o) => o.lc != lc;
}
