// Adaptive placement of the control panel (docs/design/editor-panel.md section 6): two placements are computed for the
// screen, the photo's aspect and the selected control, and the one that leaves the larger photo wins.
//
//  bottom: the panel sits under the photo, above the category dock (tall phones).
//  side:   the panel is a pane on the trailing (or leading) edge with the dock at the pane's bottom (a phone held
//          in landscape, the unfolded inner screen with portrait photos, tablets).
//
// Pure geometry, no widgets: the editor page animates between layouts and the tests check the matrix.
import 'dart:math' as math;
import 'dart:ui';

enum PanelPlace { bottom, side }

/// Side pane width: 36 % of the screen, 300 to 400 dp.
double sidePaneWidth(double screenWidth) => (screenWidth * 0.36).clamp(300.0, 400.0).toDouble();

/// A side pane fits at this logical width (the photo column keeps at least 260 dp). Below it the panel is always under the photo.
bool sidePaneFits(double screenWidth) => screenWidth - sidePaneWidth(screenWidth) >= 260;

/// Whether the screen can ever show the side panel in some orientation, i.e. whether the hand side (which edge the pane takes)
/// has a visible effect: the long edge must reach the side-pane width. Slab phones qualify in landscape (long edge >= 640 dp);
/// only a window that is narrow both ways (a small split-screen / pop-up window) never does.
bool handSideMatters(Size screen) => sidePaneFits(math.max(screen.width, screen.height));

/// The side must win by this factor (area) to be chosen; avoids flapping at the boundary.
const double kSideMustWin = 1.08;

class PanelLayout {
  const PanelLayout({
    required this.place,
    required this.photoRegion,
    required this.photo,
    required this.panel,
    required this.dock,
    required this.fill,
    required this.fits,
  });

  final PanelPlace place;

  /// Where the photo is fitted (the photo is centred in it); the zoom view uses this as its viewport.
  final Rect photoRegion;

  /// The photo at fit zoom inside [photoRegion].
  final Rect photo;
  final Rect panel;
  final Rect dock;

  /// Photo edge length relative to its size without any panel (best of the two placements).
  final double fill;

  /// The controls fit this placement (with at least a usable photo).
  final bool fits;

  bool get side => place == PanelPlace.side;
}

Size _fit(double bw, double bh, double aspect) {
  bw = math.max(0.0, bw);
  bh = math.max(0.0, bh);
  final h = math.min(bw / aspect, bh);
  return Size(h * aspect, h);
}

/// Solves both placements and returns the chosen one.
///
/// [needBottom] / [needSide] are the heights the selected control needs (handle, chips, control) in the bottom
/// panel and in the side pane; [growBottom] / [growSide] are how much taller each may usefully get (a pad, the
/// histogram). [regionTop] is the first y below the top bar ([paneTop]: where the side pane may start, the top bar then
/// sits in the photo column only; defaults to [regionTop]); the dock is [dockH] tall with [dockBottom] under it.
PanelLayout solvePanelLayout({
  required Size size,
  required double regionTop,
  required double dockH,
  required double dockBottom,
  required double aspect,
  required double needBottom,
  required double needSide,
  double? needSideMin,
  double growBottom = 0,
  double growSide = 0,
  double? paneTop,
  double growMin = 0,
  PanelPlace? force,
  bool trailing = true,
  double photoInsetH = 0,
  double margin = 12,
}) {
  final w = size.width, h = size.height;
  final dockTop = h - dockBottom - dockH;
  final regionBottom = dockTop - 8;
  final a = regionBottom - regionTop;

  // no-panel references (the 80 % photo-as-pad rule and `fill`)
  final refBottom = _fit(w - 2 * photoInsetH, a, aspect);
  final sw = sidePaneWidth(w);
  final sideOk = sidePaneFits(w);
  final sideRegionH = h - regionTop - margin;
  final refSideFull = sideOk ? _fit(w - 16, sideRegionH, aspect) : Size.zero;
  final ref = math.max(refBottom.width * refBottom.height, math.max(refSideFull.width * refSideFull.height, 0.0));

  // bottom
  final pb = _fit(w - 2 * photoInsetH, a - needBottom - 8, aspect);
  final spareB = math.max(0.0, a - 8 - pb.height - needBottom);
  final panelH = needBottom + (spareB >= growMin ? math.min(spareB, growBottom) : 0);
  final regB = Rect.fromLTWH(0, regionTop, w, math.max(0.0, a - panelH - 8));
  final bottom = PanelLayout(
    place: PanelPlace.bottom,
    photoRegion: regB,
    photo: Rect.fromCenter(center: regB.center, width: pb.width, height: pb.height),
    panel: Rect.fromLTWH(margin, regionBottom - panelH, w - 2 * margin, panelH),
    dock: Rect.fromLTWH(margin, dockTop, w - 2 * margin, dockH),
    fill: 0,
    fits: a - needBottom >= 60,
  );

  PanelLayout? sidePl;
  if (sideOk) {
    final paneX = trailing ? w - sw : 0.0;
    final regS = Rect.fromLTWH(trailing ? 0 : sw, regionTop, w - sw, sideRegionH);
    final ps = _fit(regS.width - 16, regS.height, aspect);
    final col = regionBottom - (paneTop ?? regionTop); // the pane column above the dock
    final spareS = math.max(0.0, col - needSide);
    final panelHs = needSide + (spareS >= growMin ? math.min(spareS, growSide) : 0); // clipped to the column below
    final left = paneX + (trailing ? 0 : margin);
    final pw = sw - margin;
    sidePl = PanelLayout(
      place: PanelPlace.side,
      photoRegion: regS,
      photo: Rect.fromCenter(center: regS.center, width: ps.width, height: ps.height),
      panel: Rect.fromLTWH(left, regionBottom - math.min(panelHs, col), pw, math.min(panelHs, col)),
      dock: Rect.fromLTWH(left, dockTop, pw, dockH),
      fill: 0,
      fits: col >= (needSideMin ?? needSide),
    );
  }

  double area(PanelLayout? l) => l == null ? 0 : l.photo.width * l.photo.height;
  PanelLayout pick;
  final sideWins = sidePl != null && sidePl.fits && (!bottom.fits || area(sidePl) > area(bottom) * kSideMustWin);
  if (force == PanelPlace.side ? (sidePl != null && sidePl.fits) : (force == PanelPlace.bottom ? false : sideWins)) {
    pick = sidePl;
  } else {
    pick = bottom;
  }
  final fill = ref <= 0 ? 1.0 : math.sqrt(area(pick) / ref);
  return PanelLayout(place: pick.place, photoRegion: pick.photoRegion, photo: pick.photo, panel: pick.panel, dock: pick.dock, fill: fill, fits: pick.fits);
}
