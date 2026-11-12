// Adaptive placement of the control panel (lib/editor/panel_layout.dart): the device x photo matrix of the design
// mock, the "never covered" invariant, the size of the photo, the 8 % rule and the photo-as-pad threshold.
import 'dart:math' as math;
import 'dart:ui';

import 'package:dapple/editor/panel_layout.dart';
import 'package:flutter_test/flutter_test.dart';

class Dev {
  const Dev(this.name, this.size, {this.safeTop = 28, this.bottomInset = 20});
  final String name;
  final Size size;
  final double safeTop, bottomInset;
  double get regionTop => safeTop + 8 + 48 + 8;
  double get dockBottom => math.max(bottomInset, 16) + 8;
}

const devices = [
  Dev('N5 outer', Size(414.5, 951.3)),
  Dev('iPhone-like 440x956', Size(440, 956), safeTop: 47, bottomInset: 34),
  Dev('compact 360x780', Size(360, 780)),
  Dev('short 360x640', Size(360, 640)),
  Dev('N5 inner 817x902', Size(817, 902)),
  Dev('phone landscape 951x414', Size(951, 414), safeTop: 24, bottomInset: 0),
  Dev('tablet 1280x800', Size(1280, 800), safeTop: 24, bottomInset: 0),
];

const aspects = {'9:16': 9 / 16, '2:3': 2 / 3, '3:4': 3 / 4, '1:1': 1.0, '4:3': 4 / 3, '3:2': 3 / 2, '16:9': 16 / 9, '3:1': 3.0};

// a slider: handle + strip + ruler tier (bottom); chip grid + ruler tier (pane), the grid shrinks to one row at worst
PanelLayout solve(Dev d, double aspect, {double needBottom = 192, double needSide = 228, double needSideMin = 166, PanelPlace? force, double growBottom = 0, double growSide = 0}) =>
    solvePanelLayout(
      size: d.size,
      regionTop: d.regionTop,
      paneTop: d.safeTop + 8,
      dockH: 56,
      dockBottom: d.dockBottom,
      aspect: aspect,
      needBottom: needBottom,
      needSide: needSide,
      needSideMin: needSideMin,
      growBottom: growBottom,
      growSide: growSide,
      force: force,
    );

void main() {
  // The expected placement per device and photo aspect (rows: devices, columns: aspects in the order above).
  const expected = {
    'N5 outer': 'BBBBBBBB',
    'iPhone-like 440x956': 'BBBBBBBB',
    'compact 360x780': 'BBBBBBBB',
    'short 360x640': 'BBBBBBBB',
    'N5 inner 817x902': 'SSSBBBBB',
    'phone landscape 951x414': 'SSSSSSSS',
    'tablet 1280x800': 'SSSSSSSB',
  };

  test('placement matrix: tall phones under the photo; landscape phone, inner screen with portrait photos, tablets beside', () {
    for (final d in devices) {
      final got = StringBuffer();
      for (final a in aspects.values) {
        got.write(solve(d, a).side ? 'S' : 'B');
      }
      expect(got.toString(), expected[d.name], reason: d.name);
    }
  });

  test('the photo is never covered, stays inside its region and the screen, and the dock never overlaps the panel', () {
    for (final d in devices) {
      for (final e in aspects.entries) {
        final l = solve(d, e.value);
        final r = '${d.name} ${e.key} ${l.place}';
        final screen = Offset.zero & d.size;
        expect(screen.contains(l.photo.topLeft) && screen.contains(l.photo.bottomRight - const Offset(0.01, 0.01)), isTrue, reason: '$r photo ${l.photo}');
        expect(l.photoRegion.inflate(0.5).contains(l.photo.topLeft) && l.photoRegion.inflate(0.5).contains(l.photo.bottomRight), isTrue, reason: r);
        if (l.fits) {
          expect(l.photo.overlaps(l.panel), isFalse, reason: '$r photo ${l.photo} panel ${l.panel}');
        }
        expect(l.photo.overlaps(l.dock), isFalse, reason: '$r photo ${l.photo} dock ${l.dock}');
        expect(l.panel.overlaps(l.dock), isFalse, reason: '$r panel ${l.panel} dock ${l.dock}');
        expect((l.photo.width / l.photo.height - e.value).abs(), lessThan(0.01), reason: '$r aspect kept');
      }
    }
  });

  test('the photo size matches the rule: the larger of the two placements, side only if it wins by 8 %', () {
    for (final d in devices) {
      for (final e in aspects.entries) {
        final b = solve(d, e.value, force: PanelPlace.bottom), s = solve(d, e.value, force: PanelPlace.side);
        final chosen = solve(d, e.value);
        double area(PanelLayout l) => l.photo.width * l.photo.height;
        if (chosen.side) {
          expect(s.place, PanelPlace.side, reason: '${d.name} ${e.key}');
          expect(!b.fits || area(s) > area(b) * 1.08, isTrue, reason: '${d.name} ${e.key}: side must win by 8 %');
          expect(chosen.photo.size.width, closeTo(s.photo.size.width, 3));
        } else {
          expect(chosen.photo.size.width, closeTo(b.photo.size.width, 3));
          expect(!s.fits || s.place == PanelPlace.bottom || area(s) <= area(b) * 1.08, isTrue, reason: '${d.name} ${e.key}');
        }
      }
    }
  });

  test('beside: the pane is 36 % of the width clamped to 300..400, on the trailing or the leading edge, the dock at its bottom', () {
    for (final d in devices.where((d) => solve(d, 1).side)) {
      final w = d.size.width;
      final sw = sidePaneWidth(w);
      expect(sw, inInclusiveRange(300, 400));
      final tr = solve(d, 0.75, force: PanelPlace.side);
      expect(tr.panel.right, closeTo(w - 0, 12.5), reason: d.name);
      expect(tr.dock.top, greaterThanOrEqualTo(tr.panel.bottom));
      expect(tr.panel.width, closeTo(sw - 12, 0.01));
      final lead = solvePanelLayout(
          size: d.size, regionTop: d.regionTop, paneTop: d.safeTop + 8, dockH: 56, dockBottom: d.dockBottom, aspect: 0.75, needBottom: 192, needSide: 228, trailing: false, force: PanelPlace.side);
      expect(lead.panel.left, closeTo(12, 0.01), reason: d.name);
      expect(lead.photoRegion.left, closeTo(sw, 0.01));
    }
    expect(sidePaneWidth(1280), 400);
    expect(sidePaneWidth(817), 300);
    expect(sidePaneWidth(951), closeTo(342.4, 0.1));
  });

  test('the 8 % rule: a side layout that wins by less than 8 % loses', () {
    // scan the 817 x 902 screen for an aspect where the side area is between 1.0 and 1.08 times the bottom area
    final d = devices[4];
    var seen = false;
    for (var a = 0.7; a < 1.2; a += 0.002) {
      final b = solve(d, a, force: PanelPlace.bottom), s = solve(d, a, force: PanelPlace.side);
      final ratio = (s.photo.width * s.photo.height) / (b.photo.width * b.photo.height);
      if (ratio > 1.0 && ratio <= 1.08) {
        seen = true;
        expect(solve(d, a).side, isFalse, reason: 'aspect $a ratio $ratio');
      }
      if (ratio > 1.081) expect(solve(d, a).side, isTrue, reason: 'aspect $a ratio $ratio');
    }
    expect(seen, isTrue);
  });

  test('photo-as-pad threshold: a pad that leaves less than 80 % of the no-panel edge is not shown', () {
    final owner = devices[0];
    // pad need under the photo: handle 24 + strip 56 + pad (40 % of the width, 166) + 16
    final double padBottom = 24 + 56 + math.max(owner.size.width * 0.4, 168.0) + 16;
    double fillOf(double aspect) => solve(owner, aspect, needBottom: padBottom, needSide: 400).fill;
    expect(fillOf(3 / 2), greaterThanOrEqualTo(0.8), reason: 'a landscape photo: the big pad fits');
    expect(fillOf(2 / 3), lessThan(0.8), reason: 'a tall photo: the pad would cost more than 20 % of its edge');
    expect(fillOf(9 / 16), lessThan(0.8));
    // beside, on the inner screen, a landscape photo loses most of its width to the pane: below 80 %
    final inner = devices[4];
    expect(solve(inner, 3 / 2, needBottom: 426, needSide: 300, force: PanelPlace.side).fill, lessThan(0.8));
    expect(solve(inner, 3 / 4, needBottom: 426, needSide: 300).fill, greaterThanOrEqualTo(0.8), reason: 'a portrait photo beside the pane keeps its size');
  });
}
