// Layouts of the editor for large near-square windows and for a window folded half open (docs/design/foldables.md). Pure geometry,
// no widgets: the editor page lays the real widgets into these rectangles and the tests check the matrix.
//
// Chosen by the window's size class and posture, never by device model:
//   bench   a large near-square window (a book-style inner screen, a wide-fold inner screen, in either rotation) and a photo that is
//           wider than tall after crop and rotation: the photo whole at the top edge, under it a workbench with two halves.
//   column  the same window and a portrait photo: the photo beside one column of controls on the holding side.
//   half    a horizontal fold, half opened (flip style, a book standing): the photo above the fold, every control below it.
//   none    everything else: the existing phone layout (tall phones, a flip opened flat, small windows).
import 'dart:math' as math;
import 'dart:ui';

import 'package:flutter/painting.dart' show EdgeInsets;

import 'window_manager.dart' show Hand;

enum FoldMode { none, bench, column, half }

/// The fold (or hinge) the window reports, in the window's own dp.
class FoldCrease {
  const FoldCrease(this.bounds, {required this.halfOpened});
  final Rect bounds;
  final bool halfOpened;

  /// A fold that lies across the window (zero-thickness folds have one dimension of 0).
  bool get horizontal => bounds.width > bounds.height;
  bool get vertical => !horizontal;

  @override
  bool operator ==(Object other) => other is FoldCrease && other.bounds == bounds && other.halfOpened == halfOpened;

  @override
  int get hashCode => Object.hash(bounds, halfOpened);
}

/// The first fold or hinge of [features] that reaches into a window of [size]; cut-outs and the like are not creases.
FoldCrease? creaseOf(Iterable<DisplayFeature> features, Size size) {
  for (final f in features) {
    if (f.type != DisplayFeatureType.fold && f.type != DisplayFeatureType.hinge) continue;
    final b = f.bounds;
    if (b.isEmpty && b.width == 0 && b.height == 0) continue;
    if (b.right < 0 || b.bottom < 0 || b.left > size.width || b.top > size.height) continue;
    // a fold that is not across the window (an edge sliver) is ignored
    final spans = b.width > b.height ? b.width : b.height;
    final full = b.width > b.height ? size.width : size.height;
    if (spans < full * 0.8) continue;
    return FoldCrease(b, halfOpened: f.state == DisplayFeatureState.postureHalfOpened);
  }
  return null;
}

/// A large, nearly square window: at least 600 dp on its short side and between 2:3 and 3:2 (the inner screens of the book-style and
/// the wide folding phones in either rotation; a tablet is taller than that, a slab phone is narrower).
bool isNearSquareLarge(Size s) {
  if (s.shortestSide < 600) return false;
  final r = s.width / s.height;
  return r >= 0.66 && r <= 1.52;
}

/// The mode for a window of [size], its [crease] and a photo of [aspect] (width / height after crop and rotation).
/// Test hook: false keeps the ordinary phone / side-pane layouts on every window (the tests of that placement logic run on fold-sized windows).
bool debugFoldLayouts = true;

FoldMode foldModeOf(Size size, FoldCrease? crease, double aspect) {
  if (!debugFoldLayouts) return FoldMode.none;
  if (crease != null && crease.horizontal && crease.halfOpened) {
    final y = crease.bounds.center.dy;
    if (y > size.height * 0.25 && y < size.height * 0.8) return FoldMode.half;
  }
  if (isNearSquareLarge(size)) return aspect >= 1 ? FoldMode.bench : FoldMode.column;
  return FoldMode.none;
}

const double kBenchMin = 236; // the least height of the workbench (the pad keeps 150 dp)
const double kBenchGap = 48; // the gap between the two halves when there is no fold to tell where it is
const double kBenchGapPlain = 16; // the gap between the halves when no vertical fold lies there
const double kBenchWorkShare = 0.58; // the pad half's share of the width without a vertical fold
const double kInfoMaxH = 300; // the histogram half stops growing here; the height left over goes to the pad half
const double kCreaseClear = 24; // what stays free beside a fold
const double kColumnMin = 272, kColumnMax = 340;
const double kBenchPhotoMin = 300; // the photo area keeps the cluster's height (ring, ball, grip) in the bench layout

/// Where everything goes. The rectangles are in window coordinates.
class FoldPlan {
  const FoldPlan({required this.mode, required this.photo, this.controls = Rect.zero, this.info = Rect.zero, this.work = Rect.zero, this.split});
  static const none = FoldPlan(mode: FoldMode.none, photo: Rect.zero);

  final FoldMode mode;

  /// The area the photo is fitted in (it is shown whole and sits at the top edge of it).
  final Rect photo;

  /// All the controls (bench / column / the area under the fold).
  final Rect controls;

  /// Bench: the half with the histogram and the title row; the half with the strength strip and the pad.
  final Rect info, work;

  /// Column: a horizontal fold that crosses the column splits it into an upper and a lower block here (top and bottom of the band).
  final (double, double)? split;

  bool get holdsBall => mode == FoldMode.bench;

  /// The widest the photo can be drawn in [photo] for [aspect]: the rectangle of the photo itself, top-aligned in its area.
  Rect photoRect(double aspect) {
    if (photo.isEmpty) return photo;
    final h = math.min(photo.height, photo.width / aspect);
    final w = h * aspect;
    return Rect.fromLTWH(photo.left + (photo.width - w) / 2, photo.top, w, h);
  }

  static FoldPlan compute({
    required FoldMode mode,
    required Size size,
    required EdgeInsets pad,
    required double bottomInset,
    required FoldCrease? crease,
    required double aspect,
    required Hand hand,
    double textScale = 1,
    double benchExtra = 0,
  }) {
    final w = size.width, h = size.height;
    final edge = math.max(16.0, math.max(pad.left, pad.right));
    final top = pad.top;
    switch (mode) {
      case FoldMode.none:
        return FoldPlan.none;
      case FoldMode.bench:
        final bottom = h - bottomInset;
        var photoH = math.min(w / aspect, bottom - top - (kBenchMin + benchExtra) * math.max(1.0, textScale * 0.9));
        photoH = math.max(photoH, kBenchPhotoMin);
        var benchTop = top + photoH;
        // a fold across the bench zone: the bench lies wholly below it, the photo wholly above
        if (crease != null && crease.horizontal && crease.bounds.bottom + 8 + kBenchMin + benchExtra <= bottom && crease.bounds.top > top + kBenchPhotoMin && crease.bounds.top < benchTop + 40) {
          benchTop = crease.bounds.bottom + 4;
          photoH = crease.bounds.top - 8 - top;
        }
        final c = Rect.fromLTRB(edge, benchTop + 12, w - edge, bottom - 8);
        // the two halves keep the fold in the gap between them; without a vertical fold the gap is small and the pad half (the one the
        // thumb works on) is the wider one, the histogram half stops growing past kInfoMaxH
        final holdRight = hand == Hand.right;
        final vFold = crease != null && crease.vertical;
        Rect left, right;
        if (vFold) {
          final cx = crease.bounds.center.dx;
          final half = math.max(kBenchGap / 2, crease.bounds.width / 2 + kCreaseClear);
          left = Rect.fromLTRB(c.left, c.top, cx - half, c.bottom);
          right = Rect.fromLTRB(cx + half, c.top, c.right, c.bottom);
        } else {
          final total = c.width - kBenchGapPlain;
          final workW = total * kBenchWorkShare;
          final infoW = total - workW;
          final infoR = Rect.fromLTWH(holdRight ? c.left : c.right - infoW, c.top, infoW, math.min(c.height, kInfoMaxH));
          final workR = Rect.fromLTWH(holdRight ? c.right - workW : c.left, c.top, workW, c.height);
          left = holdRight ? infoR : workR;
          right = holdRight ? workR : infoR;
        }
        if (vFold) {
          final infoR = holdRight ? left : right;
          final capped = Rect.fromLTWH(infoR.left, infoR.top, infoR.width, math.min(infoR.height, kInfoMaxH));
          if (holdRight) { left = capped; } else { right = capped; }
        }
        return FoldPlan(mode: mode, photo: Rect.fromLTWH(0, top, w, photoH), controls: Rect.fromLTRB(0, benchTop, w, bottom), info: holdRight ? left : right, work: holdRight ? right : left);
      case FoldMode.column:
        final bottom = h - bottomInset;
        final colW = (w * 0.3).clamp(kColumnMin, kColumnMax).toDouble();
        final holdRight = hand == Hand.right;
        final col = holdRight ? Rect.fromLTRB(w - colW, top + 8, w - edge + 4, bottom - 8) : Rect.fromLTRB(edge - 4, top + 8, colW, bottom - 8);
        final area = holdRight ? Rect.fromLTRB(0, top, w - colW, bottom) : Rect.fromLTRB(colW, top, w, bottom);
        (double, double)? split;
        if (crease != null && crease.horizontal && crease.bounds.bottom > col.top + 160 && crease.bounds.top < col.bottom - 200) {
          split = (crease.bounds.top - 8, crease.bounds.bottom + 8);
        }
        final photoH = math.min(area.height, area.width / aspect);
        return FoldPlan(mode: mode, photo: Rect.fromLTWH(area.left, area.top, area.width, photoH), controls: col, split: split);
      case FoldMode.half:
        final c = crease!.bounds;
        final bottom = h - bottomInset;
        final avail = math.max(120.0, c.top - 8 - top);
        // the photo gets the whole area above the fold and sits in the middle of it, so no band is left between it and the fold
        final photo = Rect.fromLTWH(0, top, w, avail);
        return FoldPlan(mode: mode, photo: photo, controls: Rect.fromLTRB(0, c.bottom + 8, w, bottom));
    }
  }
}

/// The one-thumb cluster of the bench layout, laid out over the photo only (the photo's rectangle, never beyond it). The ball is the
/// centre; the rail of the six categories stands at the holding edge, the strength ring above the ball, the 分项 pill beside it and the
/// grip under it that moves the whole cluster along the edge.
class BallGeo {
  const BallGeo({required this.ball, required this.ring, required this.grip, required this.pill, required this.rail, required this.photo, required this.side});

  static const double ballD = 76, ringD = 52, edgeGap = 136, railW = 64, railChip = 48;
  static const double ringUp = 110, gripDown = 48;
  static const double gripW = 44, gripH = 30, pillW = 52, pillH = 36;
  static const double above = ringUp + ringD / 2 + 64, below = gripDown + gripH / 2 + 4 + 24 + 4; // reach above and below the ball's centre
  static const double railTopClear = 64; // the corner buttons on the photo keep the top of the edge

  final Rect ball, ring, grip, pill, rail, photo;
  final Hand side;

  /// The ball's centre is between these heights (top, bottom of the allowed range) for [photo].
  static (double, double) range(Rect photo) {
    final lo = photo.top + above, hi = photo.bottom - below;
    return hi >= lo ? (lo, hi) : ((lo + hi) / 2, (lo + hi) / 2);
  }

  /// [t] (0..1): where along the allowed range the cluster stands.
  factory BallGeo.of(Rect photo, Hand side, double t) {
    final (lo, hi) = range(photo);
    final by = lo + (hi - lo) * t.clamp(0.0, 1.0);
    final right = side == Hand.right;
    // a photo too short for six chips in one column gets the rail in two columns of three (every category stays on screen, none scrolls)
    final railMax = photo.bottom - 8;
    final railTop = photo.top + railTopClear;
    final fullH = 6 * railChip + 12.0;
    final cols = railMax - railTop >= fullH ? 1 : 2;
    final cx = right ? photo.right - edgeGap - (cols - 1) * railW : photo.left + edgeGap + (cols - 1) * railW;
    Rect box(double x, double y, double w, double h) => Rect.fromCenter(center: Offset(x, y), width: w, height: h);
    final railH = math.min(cols == 1 ? fullH : 3 * railChip + 12.0, math.max(railChip, railMax - railTop));
    final ry = (by - railH / 2).clamp(railTop, math.max(railTop, railMax - railH)).toDouble();
    final rw = railW * cols;
    final rx = right ? photo.right - rw - 10 : photo.left + 10;
    return BallGeo(
      ball: box(cx, by, ballD, ballD),
      ring: box(cx, by - ringUp, ringD, ringD),
      grip: box(cx, by + gripDown + gripH / 2 + 4, 48, 48),
      pill: box(right ? cx - 98 + pillW / 2 : cx + 98 - pillW / 2, by, pillW, 48),
      rail: Rect.fromLTWH(rx, ry, rw, railH),
      photo: photo,
      side: side,
    );
  }

  Iterable<Rect> get parts => [ball, ring, grip, pill, rail];
}
