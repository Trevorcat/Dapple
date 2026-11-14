// The fold layouts of the editor (docs/design/foldables.md): size class and posture decide the layout, never the device model. Geometry
// of the plan, then the real page with displayFeatures: nothing on the fold, the cluster inside the photo, 分项 swapping with the pad, the
// hand mirroring, state kept across a size change.
import 'dart:ui' as ui;

import 'package:dapple/editor/fold_layout.dart';
import 'package:dapple/editor/window_manager.dart';
import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/screens/editor/fold_panels.dart' show histogramCeiling;
import 'package:dapple/screens/editor/local_panel.dart' show LocalChips;
import 'package:dapple/screens/editor/pad_dock.dart' show DockItem;
import 'package:dapple/screens/editor/pad_panel.dart' show PanelChip;
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show settle;
import 'local_ai_test.dart' show FakeLocalAi;
import 'support/harness.dart';

const _landscapePhoto = '外滩夕阳'; // 3:2
const _portraitPhoto = '街角咖啡'; // 2:3

Finder k(String key) => find.byKey(ValueKey(key));

void windowWith(WidgetTester t, Size size, {ui.Rect? fold, bool half = false, double top = 36, double bottom = 24}) {
  useWindow(t, size, top: top, bottom: bottom);
  // display features are in logical pixels
  t.view.displayFeatures = [
    if (fold != null) ui.DisplayFeature(bounds: fold, type: ui.DisplayFeatureType.fold, state: half ? ui.DisplayFeatureState.postureHalfOpened : ui.DisplayFeatureState.postureFlat),
  ];
}

ui.Rect vFold(Size s) => ui.Rect.fromLTWH(s.width / 2, 0, 0, s.height);
ui.Rect hFold(Size s) => ui.Rect.fromLTWH(0, s.height / 2, s.width, 0);

Future<(AppState, String)> openFold(WidgetTester t, Size size, {ui.Rect? fold, bool half = false, String title = _landscapePhoto, int handedness = 0, EGroup group = EGroup.light, void Function(AppState, String)? before}) async {
  windowWith(t, size, fold: fold, half: half);
  await loadTestFonts();
  final app = AppState.demo();
  app.settings.handedness = handedness;
  final id = app.photos.firstWhere((p) => p.title == title && p.id.startsWith('d')).id;
  before?.call(app, id);
  await t.pumpWidget(MaterialApp(
    debugShowCheckedModeBanner: false,
    theme: buildTheme(Brightness.light),
    home: EditorPage(app: app, photoId: id, resamplePointers: false, initialGroup: group, localAi: FakeLocalAi()),
  ));
  await settle(t, 8);
  return (app, id);
}

Rect rectOf(WidgetTester t, String key) => t.getRect(k(key));

void main() {
  group('the plan', () {
    test('size classes: near-square large windows only', () {
      expect(isNearSquareLarge(const Size(984, 1092)), isTrue);
      expect(isNearSquareLarge(const Size(904, 832)), isTrue);
      expect(isNearSquareLarge(const Size(626, 890)), isTrue);
      expect(isNearSquareLarge(const Size(890, 626)), isTrue);
      expect(isNearSquareLarge(const Size(412, 915)), isFalse, reason: 'ordinary phone');
      expect(isNearSquareLarge(const Size(412, 1004)), isFalse, reason: 'tall phone (flip opened flat)');
      expect(isNearSquareLarge(const Size(915, 412)), isFalse, reason: 'phone in landscape');
      expect(isNearSquareLarge(const Size(400, 400)), isFalse, reason: 'cover screen');
      expect(isNearSquareLarge(const Size(800, 1280)), isFalse, reason: 'a tablet is taller than 2:3');
    });

    test('mode: by orientation of the photo, and the half-opened fold wins', () {
      const big = Size(904, 832);
      expect(foldModeOf(big, null, 1.5), FoldMode.bench);
      expect(foldModeOf(big, null, 0.667), FoldMode.column);
      expect(foldModeOf(const Size(412, 915), null, 1.5), FoldMode.none);
      final half = FoldCrease(hFold(const Size(412, 1004)), halfOpened: true);
      expect(foldModeOf(const Size(412, 1004), half, 1.5), FoldMode.half);
      // flat: the flip opened flat is a tall phone
      expect(foldModeOf(const Size(412, 1004), FoldCrease(hFold(const Size(412, 1004)), halfOpened: false), 1.5), FoldMode.none);
      // a vertical fold half opened (a book held like a book) keeps the near-square layouts
      expect(foldModeOf(big, FoldCrease(vFold(big), halfOpened: true), 1.5), FoldMode.bench);
    });

    test('creaseOf reads folds and hinges only, and ignores slivers', () {
      const s = Size(904, 832);
      expect(creaseOf([ui.DisplayFeature(bounds: vFold(s), type: ui.DisplayFeatureType.cutout, state: ui.DisplayFeatureState.unknown)], s), isNull);
      expect(creaseOf([ui.DisplayFeature(bounds: vFold(s), type: ui.DisplayFeatureType.hinge, state: ui.DisplayFeatureState.postureFlat)], s)!.vertical, isTrue);
      expect(creaseOf([ui.DisplayFeature(bounds: ui.Rect.fromLTWH(0, 0, 0, 40), type: ui.DisplayFeatureType.fold, state: ui.DisplayFeatureState.postureFlat)], s), isNull);
    });

    for (final size in const [Size(984, 1092), Size(904, 832), Size(626, 890), Size(890, 626)]) {
      for (final hand in Hand.values) {
        test('bench $size $hand: halves keep clear of the fold, pad on the holding side, photo whole at the top', () {
          final crease = size.width >= size.height ? FoldCrease(vFold(size), halfOpened: false) : FoldCrease(hFold(size), halfOpened: false);
          final p = FoldPlan.compute(mode: FoldMode.bench, size: size, pad: const EdgeInsets.only(top: 36, bottom: 24), bottomInset: 24, crease: crease, aspect: 1.5, hand: hand);
          expect(p.photo.top, 36 + kFoldTopGap);
          expect(p.photo.height, greaterThanOrEqualTo(kBenchPhotoMin));
          for (final r in [p.info, p.work]) {
            expect(r.width, greaterThan(200), reason: 'a half is usable');
            expect(r.height, greaterThanOrEqualTo(180));
            expect(r.overlaps(crease.bounds.inflate(1)), isFalse, reason: 'no control on the fold');
            expect(r.bottom, lessThanOrEqualTo(size.height - 24));
          }
          expect(p.info.overlaps(p.work), isFalse);
          // the pad is on the holding-hand side
          expect(hand == Hand.right ? p.work.left > p.info.left : p.work.left < p.info.left, isTrue);
          // the bench is under the photo's region
          expect(p.info.top, greaterThanOrEqualTo(p.photo.bottom));
        });
      }
    }

    test('bench: the cluster stays inside the photo for every position and both sides, never over the bench', () {
      const photo = Rect.fromLTWH(60, 36, 780, 520);
      for (final side in Hand.values) {
        for (final t in [0.0, 0.25, 0.5, 1.0]) {
          final g = BallGeo.of(photo, side, t);
          for (final part in g.parts) {
            expect(photo.contains(part.topLeft) && photo.contains(part.bottomRight), isTrue, reason: '$side $t $part');
          }
          expect(g.ball.overlaps(g.rail), isFalse);
          expect(g.ball.overlaps(g.ring), isFalse);
          expect(g.ball.overlaps(g.pill), isFalse);
          expect(g.ball.overlaps(g.grip), isFalse);
          // at the holding edge
          expect(side == Hand.right ? g.rail.right > photo.center.dx : g.rail.left < photo.center.dx, isTrue);
          // the rail keeps clear of the corner buttons
          expect(g.rail.top, greaterThanOrEqualTo(photo.top + BallGeo.railTopClear));
        }
      }
    });

    test('bench: a horizontal fold across the bench zone puts the bench wholly below it', () {
      const size = Size(626, 890);
      final crease = FoldCrease(hFold(size), halfOpened: false);
      final p = FoldPlan.compute(mode: FoldMode.bench, size: size, pad: const EdgeInsets.only(top: 36, bottom: 24), bottomInset: 24, crease: crease, aspect: 1.5, hand: Hand.right);
      expect(p.info.top, greaterThan(crease.bounds.bottom));
      expect(p.photo.bottom, lessThan(crease.bounds.top));
    });

    test('bench: the two cards are as tall as each other and reach the bottom margin (no strip left under either)', () {
      for (final size in const [Size(984, 1092), Size(904, 832), Size(1100, 900)]) {
        for (final hand in Hand.values) {
          final p = FoldPlan.compute(mode: FoldMode.bench, size: size, pad: const EdgeInsets.only(top: 36, bottom: 24), bottomInset: 24, crease: FoldCrease(vFold(size), halfOpened: false), aspect: 1.5, hand: hand);
          expect(p.info.height, p.work.height, reason: '$size');
          expect(p.info.bottom, p.work.bottom);
          expect(p.work.bottom, size.height - 24 - 8);
          expect(p.photo.top, greaterThan(36), reason: 'the photo keeps a little under the status bar');
        }
      }
    });

    test('column: the column takes the width the photo leaves; no empty strip beside the photo unless the column is at its widest', () {
      for (final size in const [Size(904, 832), Size(984, 1092), Size(832, 904)]) {
        final p = FoldPlan.compute(mode: FoldMode.column, size: size, pad: const EdgeInsets.only(top: 36, bottom: 24), bottomInset: 24, crease: null, aspect: 0.667, hand: Hand.right);
        final shown = p.photoRect(0.667);
        final gap = p.controls.left - shown.right;
        expect(p.controls.width, inInclusiveRange(kColumnMin - 12, kColumnMax));
        if (p.controls.width < kColumnMax - 1) expect(gap, lessThan(40), reason: '$size: the photo and the column meet');
      }
    });

    test('half open and wide (a book standing): the photo above, the workbench halves below the fold, none across it; narrow keeps the phone panel', () {
      const size = Size(904, 832);
      final crease = FoldCrease(hFold(size), halfOpened: true);
      for (final hand in Hand.values) {
        final p = FoldPlan.compute(mode: FoldMode.half, size: size, pad: const EdgeInsets.only(top: 36, bottom: 24), bottomInset: 24, crease: crease, aspect: 1.5, hand: hand);
        expect(p.hasHalves, isTrue);
        expect(p.holdsBall, isTrue);
        expect(p.photo.bottom, lessThan(crease.bounds.top));
        for (final r in [p.info, p.work]) {
          expect(r.top, greaterThan(crease.bounds.bottom));
          expect(r.height, greaterThanOrEqualTo(180));
          expect(r.width, greaterThan(200));
        }
        expect(p.info.overlaps(p.work), isFalse);
        expect(p.info.height, p.work.height);
      }
      const narrow = Size(412, 1004);
      final p = FoldPlan.compute(mode: FoldMode.half, size: narrow, pad: const EdgeInsets.only(top: 36, bottom: 24), bottomInset: 24, crease: FoldCrease(hFold(narrow), halfOpened: true), aspect: 1.5, hand: Hand.right);
      expect(p.hasHalves, isFalse);
    });

    test('裁切: the cluster keeps all four corners of the photo free for every position and both sides', () {
      const photo = Rect.fromLTWH(60, 42, 780, 520);
      for (final photo in [photo, const Rect.fromLTWH(0, 42, 904, 420), const Rect.fromLTWH(0, 42, 904, 330)]) {
        for (final side in Hand.values) {
          for (final t in [0.0, 0.3, 0.5, 1.0]) {
            final g = BallGeo.of(photo, side, t, clearCorners: true);
            for (final part in g.parts) {
              for (final corner in BallGeo.corners(photo)) {
                expect(part.overlaps(corner), isFalse, reason: '$photo $side $t $part covers $corner');
              }
            }
          }
        }
      }
    });

    test('cover size: small windows only', () {
      expect(isCoverSize(const Size(360, 380)), isTrue);
      expect(isCoverSize(const Size(400, 400)), isTrue);
      expect(isCoverSize(const Size(412, 915)), isFalse);
      expect(isCoverSize(const Size(915, 412)), isFalse);
      expect(isCoverSize(const Size(904, 832)), isFalse);
    });

    test('column: beside the photo on the holding side, split around a horizontal fold', () {
      const size = Size(832, 904);
      final crease = FoldCrease(hFold(size), halfOpened: false);
      for (final hand in Hand.values) {
        final p = FoldPlan.compute(mode: FoldMode.column, size: size, pad: const EdgeInsets.only(top: 36, bottom: 24), bottomInset: 24, crease: crease, aspect: 0.667, hand: hand);
        expect(hand == Hand.right ? p.controls.left > p.photo.left : p.controls.left < p.photo.right, isTrue);
        expect(p.photo.overlaps(p.controls), isFalse);
        expect(p.split, isNotNull);
        expect(p.split!.$1, lessThan(crease.bounds.top));
        expect(p.split!.$2, greaterThan(crease.bounds.bottom));
        expect(p.photo.top, 36 + kFoldTopGap);
      }
    });
  });

  group('the page', () {
    testWidgets('book inner, landscape photo: bench with cluster; nothing on the fold; the cluster inside the photo', (t) async {
      const size = Size(904, 832);
      await openFold(t, size, fold: vFold(size));
      final info = rectOf(t, 'fold-info'), work = rectOf(t, 'fold-work');
      expect(info.right, lessThan(size.width / 2));
      expect(work.left, greaterThan(size.width / 2));
      expect(k('pad-light'), findsOneWidget);
      expect(rectOf(t, 'pad-light').left, greaterThan(size.width / 2), reason: 'the pad is on the right for the right hand');
      expect(k('histogram'), findsOneWidget);
      expect(k('dock-EGroup.light'), findsNothing, reason: 'no dock in the workbench layout');
      final photo = rectOf(t, 'zoom-stage');
      expect(photo.bottom, lessThanOrEqualTo(info.top));
      for (final key in ['cluster-ball', 'cluster-ring', 'cluster-rail', 'cluster-grip']) {
        final r = rectOf(t, key);
        expect(photo.contains(r.topLeft) && photo.contains(r.bottomRight), isTrue, reason: '$key $r is inside the photo $photo');
      }
      // back floats on the photo's corner
      expect(photo.contains(rectOf(t, 'toolbar-back').center), isTrue);
    });

    testWidgets('half open and wide: the workbench under the fold, the cluster on the photo above, no dock', (t) async {
      const size = Size(904, 832);
      await openFold(t, size, fold: hFold(size), half: true);
      final info = rectOf(t, 'fold-info'), work = rectOf(t, 'fold-work');
      expect(info.top, greaterThan(size.height / 2));
      expect(work.top, greaterThan(size.height / 2));
      expect(k('dock-EGroup.light'), findsNothing);
      expect(k('cluster-ball'), findsOneWidget);
      expect(rectOf(t, 'cluster-ball').bottom, lessThan(size.height / 2));
      expect(k('pad-light'), findsOneWidget);
    });

    testWidgets('portrait column: the categories are the dock own items, and the header text lines up with the histogram', (t) async {
      const size = Size(904, 832);
      await openFold(t, size, fold: vFold(size), title: _portraitPhoto);
      expect(find.descendant(of: k('fold-cat-light'), matching: find.byType(DockItem)), findsOneWidget);
      expect(find.descendant(of: k('fold-column'), matching: find.byType(PanelChip)), findsNothing, reason: 'no text chips for the categories');
      for (final g in ['light', 'color', 'style', 'effects', 'local', 'crop']) {
        final r = rectOf(t, 'fold-cat-$g');
        expect(r.height, greaterThanOrEqualTo(48));
        expect(r.width, greaterThanOrEqualTo(48));
      }
      final col = rectOf(t, 'fold-column');
      expect(rectOf(t, 'histogram').left - col.left, closeTo(Sp.l, 1.5));
      await t.tap(k('fold-cat-color'));
      await settle(t, 6);
      expect(k('pad-color'), findsOneWidget);
    });

    testWidgets('裁切: the cluster fades out while a crop handle is dragged and keeps off the photo corners', (t) async {
      const size = Size(904, 832);
      await openFold(t, size, fold: vFold(size), group: EGroup.crop);
      final photo = rectOf(t, 'zoom-stage');
      for (final corner in BallGeo.corners(photo)) {
        for (final key in ['cluster-ball', 'cluster-ring', 'cluster-rail', 'cluster-grip', 'cluster-items']) {
          if (k(key).evaluate().isEmpty) continue;
          expect(rectOf(t, key).overlaps(corner.deflate(1)), isFalse, reason: '$key covers a corner $corner of $photo');
        }
      }
      expect(k('fold-cluster'), findsOneWidget);
      expect(t.widget<AnimatedOpacity>(k('fold-cluster')).opacity, greaterThan(0));
    });

    testWidgets('分项 swaps with the pad in the same place (bench), and back returns', (t) async {
      const size = Size(904, 832);
      await openFold(t, size, fold: vFold(size));
      final padBox = rectOf(t, 'fold-work');
      await t.tap(k('items-button'));
      await settle(t, 6);
      expect(k('pad-light'), findsNothing);
      expect(k('header-back'), findsOneWidget);
      expect(rectOf(t, 'fold-work'), padBox);
      await t.tap(k('header-back'));
      await settle(t, 6);
      expect(k('pad-light'), findsOneWidget);
    });

    testWidgets('left hand mirrors: pad on the left, cluster at the left edge', (t) async {
      const size = Size(904, 832);
      await openFold(t, size, fold: vFold(size), handedness: 1);
      expect(rectOf(t, 'fold-work').right, lessThan(size.width / 2));
      expect(rectOf(t, 'cluster-rail').center.dx, lessThan(size.width / 2));
      expect(rectOf(t, 'fold-info').left, greaterThan(size.width / 2));
    });

    testWidgets('portrait photo: one column on the holding side, categories, pad, swap', (t) async {
      const size = Size(904, 832);
      await openFold(t, size, fold: vFold(size), title: _portraitPhoto);
      final col = rectOf(t, 'fold-column');
      expect(col.left, greaterThan(size.width / 2 - 1));
      expect(k('cluster-ball'), findsNothing, reason: 'no ball beside a portrait photo');
      expect(k('histogram'), findsOneWidget);
      expect(k('pad-light'), findsOneWidget);
      for (final g in ['light', 'color', 'style', 'effects', 'local', 'crop']) {
        expect(k('fold-cat-$g'), findsOneWidget);
      }
      final photo = rectOf(t, 'zoom-stage');
      expect(photo.right, lessThanOrEqualTo(col.left));
      await t.drag(k('fold-swap'), const Offset(-150, 0));
      await settle(t, 8);
      expect(rectOf(t, 'fold-column').right, lessThan(size.width / 2 + 1));
    });

    testWidgets('portrait photo with a horizontal fold: two blocks, nothing across the fold', (t) async {
      const size = Size(832, 904);
      await openFold(t, size, fold: hFold(size), title: _portraitPhoto);
      final low = rectOf(t, 'fold-column-low'), up = rectOf(t, 'fold-column');
      expect(up.bottom, lessThan(size.height / 2));
      expect(low.top, greaterThan(size.height / 2));
    });

    testWidgets('state kept across a fold / unfold: photo, category, layer, selection', (t) async {
      const size = Size(904, 832);
      windowWith(t, size, fold: vFold(size));
      await loadTestFonts();
      final app = AppState.demo();
      final id = app.photos.firstWhere((p) => p.title == _landscapePhoto && p.id.startsWith('d')).id;
      await t.pumpWidget(MaterialApp(theme: buildTheme(Brightness.light), home: EditorPage(app: app, photoId: id, resamplePointers: false, initialGroup: EGroup.color, localAi: FakeLocalAi())));
      await settle(t, 8);
      await t.tap(k('items-button'));
      await settle(t, 6);
      expect(k('header-back'), findsOneWidget);
      // fold to the ordinary phone shape
      windowWith(t, const Size(412, 915));
      await t.pump();
      await settle(t, 10);
      expect(k('dock-EGroup.color'), findsOneWidget, reason: 'the phone layout has its dock again');
      expect(k('header-back'), findsOneWidget, reason: 'still in 分项 of 颜色');
      expect(find.text('颜色'), findsWidgets);
      // unfold again: the bench, same layer
      windowWith(t, size, fold: vFold(size));
      await t.pump();
      await settle(t, 10);
      expect(k('fold-info'), findsOneWidget);
      expect(k('header-back'), findsOneWidget);
    });

    testWidgets('system back closes one layer at a time, then leaves; the open category is not a layer', (t) async {
      const size = Size(904, 832);
      final key = GlobalKey<NavigatorState>();
      windowWith(t, size, fold: vFold(size));
      await loadTestFonts();
      final app = AppState.demo();
      final id = app.photos.firstWhere((p) => p.title == _landscapePhoto && p.id.startsWith('d')).id;
      await t.pumpWidget(MaterialApp(
        navigatorKey: key,
        theme: buildTheme(Brightness.light),
        home: Builder(builder: (c) => TextButton(onPressed: () => openEditor(c, app, id), child: const Text('go'))),
      ));
      await t.tap(find.text('go'));
      await settle(t, 10);
      expect(k('fold-info'), findsOneWidget);
      await t.tap(k('items-button'));
      await settle(t, 6);
      await t.binding.handlePopRoute();
      await settle(t, 6);
      expect(k('pad-light'), findsOneWidget, reason: 'the first back closed 分项 only');
      expect(k('fold-info'), findsOneWidget);
      await t.binding.handlePopRoute();
      await settle(t, 10);
      expect(k('fold-info'), findsNothing, reason: 'the second back left the editor; the open category did not need closing');
    });

    testWidgets('the ball: push moves the pad relatively, hold compares, the ring sets the strength', (t) async {
      const size = Size(904, 832);
      final (app, id) = await openFold(t, size, fold: vFold(size));
      final ball = t.getCenter(k('cluster-ball'));
      final before = app.editOf(id).values['exposure'] ?? 0;
      final g = await t.startGesture(ball);
      await g.moveBy(const Offset(40, 0));
      await g.moveBy(const Offset(20, 0));
      await t.pump();
      await g.up();
      await settle(t, 4);
      expect(app.editOf(id).values['exposure'] ?? 0, isNot(before), reason: 'a push to the right brightens');
      // hold still: compare
      final h = await t.startGesture(ball);
      await t.pump(const Duration(milliseconds: 500));
      await t.pump(const Duration(milliseconds: 300));
      expect(find.text('原片'), findsWidgets);
      await h.up();
      await settle(t, 4);
      expect(find.text('原片'), findsNothing);
      // the ring
      final ring = t.getCenter(k('cluster-ring'));
      final r = await t.startGesture(ring);
      await r.moveBy(const Offset(0, 30));
      await r.moveBy(const Offset(0, 30));
      await r.up();
      await settle(t, 4);
      expect(find.text('100'), findsNothing, reason: 'the strength moved off 100 %');
    });

    testWidgets('the grip moves the cluster along the edge, and past the middle changes sides', (t) async {
      const size = Size(904, 832);
      await openFold(t, size, fold: vFold(size));
      final y0 = rectOf(t, 'cluster-ball').center.dy;
      final g = await t.startGesture(t.getCenter(k('cluster-grip')));
      await g.moveBy(const Offset(0, -60));
      await g.moveBy(const Offset(0, -40));
      await t.pump();
      await g.up();
      await settle(t, 6);
      expect(rectOf(t, 'cluster-ball').center.dy, lessThan(y0 - 40));
      // across the middle
      final s = await t.startGesture(t.getCenter(k('cluster-grip')));
      for (var i = 0; i < 12; i++) {
        await s.moveBy(const Offset(-50, 0));
      }
      await s.up();
      await settle(t, 8);
      expect(rectOf(t, 'cluster-rail').center.dx, lessThan(size.width / 2));
      expect(rectOf(t, 'fold-work').right, lessThan(size.width / 2));
    });

    testWidgets('裁切 and 成像 in the workbench: ratio chips with the ruler; 成像 opens on its pad; the chip opens the map', (t) async {
      const size = Size(904, 832);
      await openFold(t, size, fold: vFold(size), before: (app, id) {
        app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      });
      await t.tap(k('cluster-cat-crop'));
      await settle(t, 8);
      expect(k('crop-4:5'), findsOneWidget);
      expect(k('ruler-straighten'), findsOneWidget);
      await t.tap(k('cluster-cat-style'));
      await settle(t, 8);
      expect(k('pad-film'), findsOneWidget, reason: 'the pad is the home of 成像, as on the phone');
      expect(k('header-back'), findsNothing);
      await t.tap(k('stock-button'));
      await settle(t, 8);
      expect(k('film-map'), findsOneWidget);
      expect(k('header-back'), findsOneWidget, reason: 'the map is a layer the chip opens');
    });

    testWidgets('局部 in the workbench: the tiles first, then strip + pad with the switch', (t) async {
      const size = Size(904, 832);
      await openFold(t, size, fold: vFold(size), group: EGroup.local);
      expect(k('kind-radial'), findsOneWidget);
      expect(k('pad-light'), findsNothing);
    });

    testWidgets('half-opened flip: photo above the fold, every control below it', (t) async {
      const size = Size(412, 1004);
      await openFold(t, size, fold: hFold(size), half: true);
      final photo = rectOf(t, 'zoom-stage');
      expect(photo.bottom, lessThanOrEqualTo(size.height / 2));
      final panel = rectOf(t, 'docked-panel');
      expect(panel.top, greaterThanOrEqualTo(size.height / 2));
      expect(rectOf(t, 'dock-EGroup.light').top, greaterThanOrEqualTo(size.height / 2));
      expect(rectOf(t, 'toolbar-back').bottom, lessThanOrEqualTo(size.height / 2), reason: 'the corner buttons are on the photo');
    });

    testWidgets('half-opened flip: the photo is centred in the whole area above the fold (no band under it)', (t) async {
      const size = Size(412, 1004);
      await openFold(t, size, fold: hFold(size), half: true);
      final photo = rectOf(t, 'zoom-stage');
      final fold = hFold(size);
      final free = fold.top - 8 - 36;
      final above = photo.top - 36, under = fold.top - 8 - photo.bottom;
      expect((above - under).abs(), lessThan(free * 0.3 + 40), reason: 'the leftover is shared, not all under the photo');
    });

    test('short bench photo: the rail takes two columns so all six categories show', () {
      final g = BallGeo.of(const Rect.fromLTWH(0, 0, 890, 330), Hand.right, 0.5);
      expect(g.rail.width, greaterThan(BallGeo.railW + 1));
      expect(g.rail.height, greaterThanOrEqualTo(3 * BallGeo.railChip));
      expect(const Rect.fromLTWH(0, 0, 890, 330).contains(g.rail.topLeft) && const Rect.fromLTWH(0, 0, 890, 330).contains(g.rail.bottomRight), isTrue);
      expect(g.rail.overlaps(g.ball), isFalse);
    });

    testWidgets('flat flip, tall phone and ordinary phone keep the phone layout', (t) async {
      for (final c in [(const Size(412, 1004), true), (const Size(412, 915), false)]) {
        await openFold(t, c.$1, fold: c.$2 ? hFold(c.$1) : null);
        expect(k('fold-info'), findsNothing);
        expect(k('dock-EGroup.light'), findsOneWidget);
        await t.pumpWidget(const SizedBox());
      }
    });
  });

  // ---------------------------------------------------------------------------------------------- the second round (real device)
  const sizes3 = [Size(984, 1092), Size(904, 832), Size(626, 890)];
  const ratios = {'2:3': 2 / 3, '3:4': 3 / 4, '4:5': 4 / 5, '9:16': 9 / 16};

  group('column photo size', () {
    for (final size in sizes3) {
      for (final e in ratios.entries) {
        for (final hand in Hand.values) {
          test('plan ${size.width.toInt()}x${size.height.toInt()} ${e.key} $hand: as large as the column minimum allows, at the top', () {
            final p = FoldPlan.compute(mode: FoldMode.column, size: size, pad: const EdgeInsets.only(top: 36, bottom: 24), bottomInset: 24, crease: null, aspect: e.value, hand: hand);
            final r = p.photoRect(e.value);
            final availH = size.height - 24 - (36 + kFoldTopGap);
            expect(r.top, 36 + kFoldTopGap, reason: 'anchored at the top');
            expect((r.width / r.height - e.value).abs(), lessThan(0.002), reason: 'contain, not cropped');
            expect(r.height, lessThanOrEqualTo(availH + 0.5));
            expect(p.controls.width, greaterThanOrEqualTo(kColumnMin - 16), reason: 'the column keeps its usable width');
            expect(r.overlaps(p.controls), isFalse);
            // no band that the aspect ratio does not need: the photo takes the whole height, or the whole width the column leaves
            final fullH = (r.height - availH).abs() < 1;
            final fullW = (r.width - (size.width - kColumnMin)).abs() < 1;
            expect(fullH || fullW, isTrue, reason: 'photo $r in ${size.width}x${size.height}');
            if (!fullH) expect(r.bottom, lessThan(size.height - 24), reason: 'the band under it is the aspect ratio\'s');
          });
        }
      }
    }

    for (final size in sizes3) {
      for (final e in ratios.entries) {
        testWidgets('page ${size.width.toInt()}x${size.height.toInt()} ${e.key}: the photo is laid into the plan\'s photo area', (t) async {
          await openFold(t, size, title: _portraitPhoto, before: (a, i) {
            if (e.key != '2:3') a.applyEdit(i, (x) => x.copyWith(crop: e.key));
          });
          final plan = FoldPlan.compute(mode: FoldMode.column, size: size, pad: const EdgeInsets.only(top: 36, bottom: 24), bottomInset: 24, crease: null, aspect: e.value, hand: Hand.right);
          final stage = rectOf(t, 'zoom-stage');
          expect(stage.top, plan.photo.top);
          expect(stage.height, closeTo(plan.photo.height, 1));
          expect(stage.right, lessThanOrEqualTo(rectOf(t, 'fold-column').left + 0.5));
          expect(stage.width / stage.height, greaterThanOrEqualTo(e.value - 0.01));
        });
      }
    }
  });

  group('second level: 成像 as on the phone', () {
    Future<void> toFilm(WidgetTester t) async {
      await t.tap(k('cluster-cat-style').evaluate().isNotEmpty ? k('cluster-cat-style') : k('fold-cat-style'));
      await settle(t, 8);
    }

    void expectFilmHome(WidgetTester t) {
      expect(k('pad-film'), findsOneWidget, reason: 'the pad is the default layer');
      expect(k('film-map'), findsNothing);
      expect(k('stock-button'), findsOneWidget);
      expect(k('header-title'), findsOneWidget);
      expect(k('header-readout'), findsOneWidget);
      expect(t.widget<Text>(k('header-readout')).data, startsWith('强度'));
      expect(k('items-button'), findsOneWidget);
      expect(k('header-back'), findsNothing);
    }

    for (final column in [false, true]) {
      testWidgets('${column ? 'column' : 'workbench'}: the pad first; the chip swaps in the map in the same place, a choice returns to the pad', (t) async {
        const size = Size(904, 832);
        await openFold(t, size, fold: column ? null : vFold(size), title: column ? _portraitPhoto : _landscapePhoto, before: (a, i) => a.applyEdit(i, (e) => e.copyWith(film: 'kodak_portra_400')));
        await toFilm(t);
        expectFilmHome(t);
        final padBox = rectOf(t, 'pad-film');
        await t.tap(k('stock-button'));
        await settle(t, 8);
        expect(k('film-map'), findsOneWidget);
        expect(k('pad-film'), findsNothing);
        expect(k('header-back'), findsOneWidget);
        expect(k('stock-button'), findsNothing);
        final mapBox = rectOf(t, 'film-map');
        expect((mapBox.center - padBox.center).distance, lessThan(30), reason: 'the map takes the pad\'s place');
        await t.tap(k('header-back'));
        await settle(t, 8);
        expectFilmHome(t);
        await t.tap(k('stock-button'));
        await settle(t, 8);
        await t.tapAt(rectOf(t, 'film-map').center);
        await settle(t, 12);
        expectFilmHome(t);
      });
    }

    testWidgets('the ball drags the film pad by default and steps through stocks only while the map is open', (t) async {
      const size = Size(904, 832);
      final (app, id) = await openFold(t, size, fold: vFold(size), before: (a, i) => a.applyEdit(i, (e) => e.copyWith(film: 'kodak_portra_400')));
      await toFilm(t);
      final before = app.editOf(id);
      final g = await t.startGesture(t.getCenter(k('cluster-ball')));
      await g.moveBy(const Offset(40, 0));
      await g.moveBy(const Offset(60, -30));
      await t.pump();
      await g.up();
      await settle(t, 4);
      final after = app.editOf(id);
      expect(after.film, before.film, reason: 'the pad moved, the stock did not change');
      expect(after.values.toString(), isNot(before.values.toString()), reason: 'a film pad value changed');
      await t.tap(k('stock-button'));
      await settle(t, 8);
      final g2 = await t.startGesture(t.getCenter(k('cluster-ball')));
      await g2.moveBy(const Offset(60, 0));
      await g2.moveBy(const Offset(120, 0));
      await t.pump();
      await g2.up();
      await settle(t, 4);
      expect(app.editOf(id).film, isNot(before.film), reason: 'with the map open the push steps through the stocks');
    });

    testWidgets('the map is a layer for the system back, in the column too', (t) async {
      const size = Size(904, 832);
      await openFold(t, size, title: _portraitPhoto);
      await toFilm(t);
      await t.tap(k('stock-button'));
      await settle(t, 8);
      expect(k('film-map'), findsOneWidget);
      await t.binding.handlePopRoute();
      await settle(t, 8);
      expectFilmHome(t);
    });
  });

  group('column order, header and histogram', () {
    for (final size in sizes3) {
      final tag = '${size.width.toInt()}x${size.height.toInt()}';
      final fold = size == const Size(626, 890) ? null : vFold(size);
      testWidgets('column $tag: histogram, categories, header, second-level rows, pad; the readout is whole', (t) async {
        await openFold(t, size, fold: fold, title: _portraitPhoto, before: (a, i) => a.applyEdit(i, (e) => e.copyWith(film: 'kodak_portra_400')));
        double top(Finder f) => t.getRect(f).top;
        double bottom(Finder f) => t.getRect(f).bottom;
        expect(bottom(k('histogram')), lessThanOrEqualTo(top(k('fold-cat-light'))));
        expect(bottom(k('fold-cat-style')), lessThanOrEqualTo(top(k('header-title'))));
        expect(bottom(k('header-readout')), lessThanOrEqualTo(top(k('pad-light'))));
        await t.tap(k('fold-cat-style'));
        await settle(t, 8);
        expect(bottom(k('fold-cat-style')), lessThanOrEqualTo(top(k('header-title'))));
        expect(top(k('stock-button')), greaterThanOrEqualTo(bottom(k('header-title')) - 12));
        expect(bottom(k('stock-button')), lessThanOrEqualTo(top(k('pad-film')) + 1));
        expect(bottom(k('header-readout')), lessThanOrEqualTo(top(k('pad-film')) + 1));
        await t.tap(k('fold-cat-local'));
        await settle(t, 8);
        await t.tap(k('kind-radial'));
        await settle(t, 10);
        final card = t.getRect(k('fold-column-low').evaluate().isNotEmpty ? k('fold-column-low') : k('fold-column'));
        expect(bottom(k('fold-cat-local')), lessThanOrEqualTo(top(k('header-title'))));
        final readout = t.getRect(k('header-readout'));
        final chips = t.getRect(find.byType(LocalChips).first);
        expect(readout.bottom, lessThanOrEqualTo(chips.top + 1), reason: 'the readout is not overlapped by the chips row');
        expect(readout.top, greaterThanOrEqualTo(bottom(k('header-title')) - 12));
        expect(card.contains(readout.topLeft) && card.contains(readout.bottomRight), isTrue, reason: 'the readout is inside the card');
        expect(t.widget<Text>(k('header-readout')).data, startsWith('强度'));
        expect(readout.height, greaterThanOrEqualTo(14), reason: 'a whole line, not a clipped sliver');
      });

      testWidgets('column $tag: title, 复位 and 分项 share one row with the card\'s top padding above', (t) async {
        await openFold(t, size, fold: fold, title: _portraitPhoto);
        final card = t.getRect(k('fold-column-low').evaluate().isNotEmpty ? k('fold-column-low') : k('fold-column'));
        final title = t.getRect(k('header-title')), items = t.getRect(k('items-button'));
        expect(items.top, greaterThanOrEqualTo(card.top + Sp.s), reason: 'normal padding above the buttons');
        expect(title.top, greaterThanOrEqualTo(card.top + Sp.s));
        expect((title.center.dy - items.center.dy).abs(), lessThan(2), reason: 'one centre line');
        final reset = find.byKey(const ValueKey('reset-light'));
        if (reset.evaluate().isNotEmpty) expect((t.getRect(reset).center.dy - title.center.dy).abs(), lessThan(2));
        final up = t.getRect(k('fold-column'));
        expect(t.getRect(k('fold-swap')).top, closeTo(up.top, 1));
        expect(t.getRect(k('histogram')).top, greaterThanOrEqualTo(up.top + 12), reason: 'the handle has its own band above the histogram');
      });

      testWidgets('column $tag: undo and redo once, on the photo, and the column has none', (t) async {
        await openFold(t, size, title: _portraitPhoto);
        expect(k('toolbar-undo'), findsOneWidget);
        expect(k('toolbar-redo'), findsOneWidget);
        final stage = rectOf(t, 'zoom-stage'), col = rectOf(t, 'fold-column');
        for (final key in ['toolbar-back', 'toolbar-undo', 'toolbar-redo']) {
          final r = rectOf(t, key);
          expect(r.top, greaterThanOrEqualTo(stage.top));
          expect(r.bottom, lessThanOrEqualTo(stage.top + 8 + 56), reason: 'one slim bar at the photo\'s top, as on the phone');
          expect(r.right, lessThanOrEqualTo(col.left + 0.5), reason: key);
        }
      });
    }

    testWidgets('workbench 904x832: the header row has padding above and one centre line', (t) async {
      const size = Size(904, 832);
      await openFold(t, size, fold: vFold(size));
      final card = rectOf(t, 'fold-info');
      final title = rectOf(t, 'header-title'), items = rectOf(t, 'items-button');
      expect(items.top, greaterThanOrEqualTo(card.top + Sp.s));
      expect((title.center.dy - items.center.dy).abs(), lessThan(2));
    });

    test('histogram scale: a pile-up at an end does not flatten the rest', () {
      final h = List<double>.generate(64, (i) => i == 63 ? 40.0 : 1 + (i % 7) / 7);
      expect(histogramCeiling(h), lessThan(3), reason: 'the body sets the scale');
      expect(histogramCeiling(List<double>.filled(64, 0)..[63] = 5), 5, reason: 'only ends: the tallest');
      expect(histogramCeiling(const [1, 2]), 2);
    });

    testWidgets('the histogram card at the column has real size and shows the current frame', (t) async {
      const size = Size(904, 832);
      await openFold(t, size, title: _portraitPhoto);
      final r = rectOf(t, 'histogram');
      expect(r.width, greaterThan(200));
      expect(r.height, greaterThan(80));
    });
  });
}
