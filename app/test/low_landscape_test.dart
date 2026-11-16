// A slab phone held in landscape (a wide, low window, docs/design/foldables.md section 2): the column layout at the holding hand's short
// edge, the photo whole on the other side, nothing over the photo but the corner buttons. Rule, geometry, then the real page at 805 x 351 and
// 873 x 393 dp, both hands, light and dark, text scale 1.0 and 1.3, and the two screenshots.
import 'dart:ui' as ui;

import 'package:dapple/editor/fold_layout.dart';
import 'package:dapple/editor/window_manager.dart';
import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show dump, settle;
import 'local_ai_test.dart' show FakeLocalAi;
import 'support/harness.dart';

Finder k(String key) => find.byKey(ValueKey(key));
Rect rectOf(WidgetTester t, String key) => t.getRect(k(key));

const _sizes = [Size(805, 351), Size(873, 393)];

Future<AppState> openLow(WidgetTester t, Size size, {int handedness = 0, bool dark = false, double scale = 1, EGroup group = EGroup.light, String title = '外滩夕阳'}) async {
  useWindow(t, size, top: 28, bottom: 16);
  t.platformDispatcher.textScaleFactorTestValue = scale;
  addTearDown(t.platformDispatcher.clearTextScaleFactorTestValue);
  await loadTestFonts();
  final app = AppState.demo();
  app.settings.handedness = handedness;
  final id = app.photos.firstWhere((p) => p.title == title && p.id.startsWith('d')).id;
  await t.pumpWidget(RepaintBoundary(
    key: shotKey,
    child: MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildTheme(Brightness.light),
      darkTheme: buildTheme(Brightness.dark),
      themeMode: dark ? ThemeMode.dark : ThemeMode.light,
      home: EditorPage(app: app, photoId: id, resamplePointers: false, initialGroup: group, localAi: FakeLocalAi()),
    ),
  ));
  await settle(t, 8);
  return app;
}

/// Nothing but the corner buttons lies over the photo: the column and the strip are beside it, no dock, no filmstrip.
void expectPhotoClear(WidgetTester t, {bool rightHand = true}) {
  final photo = rectOf(t, 'zoom-stage');
  final col = rectOf(t, 'fold-column');
  expect(photo.overlaps(col), isFalse, reason: 'column $col beside the photo $photo');
  expect(k('dock-EGroup.light'), findsNothing);
  expect(k('cluster-ball'), findsNothing);
  if (k('local-tools').evaluate().isNotEmpty) {
    final tools = rectOf(t, 'local-tools');
    expect(col.inflate(1).contains(tools.topLeft) && col.inflate(1).contains(tools.bottomRight), isTrue, reason: 'the tool strip $tools is in the column $col');
  }
}

void main() {
  group('the rule', () {
    test('wide low windows get the column, whatever the photo', () {
      for (final a in [0.5, 0.667, 1.0, 1.5, 1.78]) {
        expect(foldModeOf(const Size(805, 351), null, a), FoldMode.column);
        expect(foldModeOf(const Size(873, 393), null, a), FoldMode.column);
        expect(foldModeOf(const Size(932, 430), null, a), FoldMode.column);
      }
      expect(foldModeOf(const Size(393, 873), null, 1.5), FoldMode.none);
      expect(foldModeOf(const Size(600, 560), null, 1.5), FoldMode.none, reason: 'near-square but small');
      expect(foldModeOf(const Size(400, 400), null, 1.5), FoldMode.none, reason: 'cover');
      expect(foldModeOf(const Size(520, 360), null, 1.5), FoldMode.none, reason: 'under 560 wide');
      expect(foldModeOf(const Size(904, 832), null, 1.5), FoldMode.bench);
      expect(foldModeOf(const Size(904, 832), null, 0.667), FoldMode.column);
      final half = FoldCrease(const ui.Rect.fromLTWH(0, 175, 805, 0), halfOpened: true);
      expect(foldModeOf(const Size(805, 351), half, 1.5), isNot(FoldMode.column), reason: 'a half-opened crease is not this class');
    });

    test('plan: photo whole beside the column on the holding side, both inside the insets', () {
      for (final size in _sizes) {
        for (final hand in Hand.values) {
          for (final a in [0.667, 1.5, 1.78]) {
            final p = FoldPlan.compute(mode: FoldMode.column, size: size, pad: const EdgeInsets.fromLTRB(40, 28, 24, 16), bottomInset: 16, crease: null, aspect: a, hand: hand);
            expect(p.low, isTrue);
            expect(p.photo.overlaps(p.controls), isFalse);
            expect(hand == Hand.right ? p.controls.left > p.photo.left : p.controls.left < p.photo.right, isTrue);
            expect(p.controls.width, inInclusiveRange(kColumnMin, kColumnMax));
            final pr = p.photoRect(a);
            expect(pr.left, greaterThanOrEqualTo(40));
            expect(pr.right, lessThanOrEqualTo(size.width - (hand == Hand.right ? 0 : 24)));
            expect(p.controls.right, lessThanOrEqualTo(size.width - (hand == Hand.right ? 24 : 0)));
            expect(p.controls.left, greaterThanOrEqualTo(hand == Hand.right ? 0 : 40));
            expect(p.photo.top, 28 + kFoldTopGap);
            expect(p.controls.bottom, lessThanOrEqualTo(size.height - 16));
          }
        }
      }
    });
  });

  group('the page', () {
    for (final size in _sizes) {
      for (final hand in [1, 2]) {
        for (final dark in [false, true]) {
          for (final scale in [1.0, 1.3]) {
            final name = '${size.width.toInt()}x${size.height.toInt()} ${hand == 1 ? 'left' : 'right'} ${dark ? 'dark' : 'light'} x$scale';
            testWidgets('$name: no overflow, nothing over the photo, five categories, pad, 分项', (t) async {
              await openLow(t, size, handedness: hand, dark: dark, scale: scale);
              expect(t.takeException(), isNull);
              expectPhotoClear(t);
              final col = rectOf(t, 'fold-column');
              expect(hand == 2 ? col.center.dx > size.width / 2 : col.center.dx < size.width / 2, isTrue, reason: 'the column is at the holding hand\'s edge');
              final photo = rectOf(t, 'zoom-stage');
              expect(photo.contains(rectOf(t, 'toolbar-back').center), isTrue);
              for (final g in ['light', 'color', 'style', 'effects', 'local']) {
                await t.ensureVisible(k('fold-cat-$g'));
                await t.tap(k('fold-cat-$g'));
                await settle(t, 6);
                expect(t.takeException(), isNull, reason: g);
                expectPhotoClear(t);
              }
              await t.tap(k('fold-cat-light'));
              await settle(t, 6);
              if (scale == 1.0) {
                expect(rectOf(t, 'pad-light').height, greaterThanOrEqualTo(150));
              }
              await t.ensureVisible(k('items-button'));
              await t.tap(k('items-button'));
              await settle(t, 6);
              expect(t.takeException(), isNull, reason: 'items');
              expectPhotoClear(t);
            });
          }
        }
      }
    }

    for (final size in _sizes) {
      testWidgets('${size.width.toInt()}x${size.height.toInt()}: 局部 radial region, pads and items; 成像 map opens and closes', (t) async {
        await openLow(t, size);
        await t.tap(k('fold-cat-local'));
        await settle(t, 6);
        await t.ensureVisible(k('kind-radial'));
        await t.tap(k('kind-radial'));
        await settle(t, 6);
        expect(t.takeException(), isNull, reason: 'radial added');
        expect(k('local-pad-light'), findsOneWidget);
        expectPhotoClear(t);
        await t.ensureVisible(k('items-button'));
        await t.tap(k('items-button'));
        await settle(t, 6);
        expect(t.takeException(), isNull, reason: 'items');
        await t.ensureVisible(k('header-back'));
        await t.tap(k('header-back'));
        await settle(t, 6);
        await t.ensureVisible(k('fold-cat-style'));
        await t.tap(k('fold-cat-style'));
        await settle(t, 6);
        expect(k('pad-film'), findsOneWidget);
        await t.tap(k('stock-button'));
        await settle(t, 6);
        expect(k('film-map'), findsOneWidget);
        expect(t.takeException(), isNull, reason: 'map');
        expectPhotoClear(t);
        await t.tap(k('header-back'));
        await settle(t, 6);
        expect(k('film-map'), findsNothing);
        expect(t.takeException(), isNull);
      });

      testWidgets('${size.width.toInt()}x${size.height.toInt()}: 局部 brush: the tool strip is in the column, not under the photo', (t) async {
        await openLow(t, size);
        await t.tap(k('fold-cat-local'));
        await settle(t, 6);
        await t.ensureVisible(k('kind-brush'));
        await t.tap(k('kind-brush'));
        await settle(t, 6);
        expect(t.takeException(), isNull);
        expect(k('local-tools'), findsOneWidget);
        expectPhotoClear(t);
      });
    }

    testWidgets('rotating keeps the page: category and photo', (t) async {
      await openLow(t, const Size(805, 351), group: EGroup.color);
      expect(k('pad-color'), findsOneWidget);
      useWindow(t, const Size(393, 873));
      await settle(t, 10);
      expect(k('pad-color'), findsOneWidget);
      expect(k('fold-column'), findsNothing);
      useWindow(t, const Size(873, 393), top: 28, bottom: 16);
      await settle(t, 10);
      expect(k('pad-color'), findsOneWidget);
      expect(k('fold-column'), findsOneWidget);
      expect(t.takeException(), isNull);
    });

    testWidgets('golden 805x351 light, 光线', (t) async {
      await openLow(t, const Size(805, 351), handedness: 2);
      await dump(t, 'low_landscape_805x351_light');
    });

    testWidgets('golden 873x393 dark, 成像', (t) async {
      await openLow(t, const Size(873, 393), handedness: 2, dark: true, group: EGroup.style);
      await dump(t, 'low_landscape_873x393_dark_film');
    });
  });
}
