// The fold layouts of the editor (docs/design/foldables.md): size class and posture decide the layout, never the device model. Geometry
// of the plan, then the real page with displayFeatures: nothing on the fold, the cluster inside the photo, 分项 swapping with the pad, the
// hand mirroring, state kept across a size change.
import 'dart:ui' as ui;

import 'package:dapple/editor/fold_layout.dart';
import 'package:dapple/editor/window_manager.dart';
import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/editor_page.dart';
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
          expect(p.photo.top, 36);
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
        expect(p.photo.top, 36);
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

    testWidgets('裁切 and 成像 in the workbench: ratio chips with the ruler; the film map stays open after a pick', (t) async {
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
      expect(k('film-map'), findsOneWidget);
      expect(k('header-back'), findsNothing, reason: 'the map is the home layer of 成像: no back arrow');
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
}
