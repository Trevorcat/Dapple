// Fixes from the second real-device pass (build/samsung2/results.md, bugs 5-9 and the film map's press-slide-release): the 成像 readout on a
// 360 dp phone, a panel height that is constant within a category, the portrait chip that is selected again after reopening / undo, the
// tool strip on a reserved line under the photo, the 发布 sheet's 分享… button, and the 选成像 chip that opens the map under a sliding
// finger. With --dart-define=SHOTS=true [--dart-define=OUT=<dir>] the steps are also written as PNGs (build/ui-samsung2).
import 'package:dapple/editor/fold_layout.dart' show debugFoldLayouts;
import 'package:dapple/model/film_map.dart';
import 'package:dapple/model/pad_model.dart';
import 'package:dapple/model/params.dart';
import 'package:dapple/model/portrait.dart';
import 'package:dapple/screens/editor/film_map_view.dart';
import 'package:dapple/screens/editor/pad_panel.dart' show kPanelBottomPad;
import 'package:dapple/screens/editor/local_controller.dart';
import 'package:dapple/screens/publish/publish_page.dart' show openPublish;
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';

import '../audit/audit_support.dart';
import '../editor_v2_test.dart' show Finger, settle;
import '../local_ai_test.dart' show FakeLocalAi;
import '../local_panel_test.dart' show addViaCard, tapKey;
import '../local_portrait_test.dart' show portraitResult;
import '../pad_panel_rules_test.dart' show padAt;
import '../support/film_fixture.dart';
import '../support/harness.dart';

Finder key(String k) => find.byKey(ValueKey(k));
Rect panelRect(WidgetTester t) => t.getRect(key('docked-panel'));
Rect stageRect(WidgetTester t) => t.getRect(key('zoom-stage'));

Future<void> dock(WidgetTester t, EGroup g) async {
  await t.tap(find.byKey(ValueKey('dock-$g')));
  await settle(t, 8);
}

/// The layer changed: the panel and the photo area did not move (guidelines 15.1-7).
(Rect, Rect) frame(WidgetTester t) => (panelRect(t), stageRect(t));

void main() {
  // these tests are about the ordinary panel placement (side pane on a wide window); the fold layouts have their own tests (fold_layout_test.dart)
  setUp(() => debugFoldLayouts = false);
  tearDown(() => debugFoldLayouts = true);
  setUp(() => FilmMapStore.debugSet({
        'kodak_portra_400': const Offset(-0.5, -0.3),
        'kodak_ektar_100': const Offset(0.6, 0.5),
        'fujifilm_velvia_100': const Offset(0.0, 0.8),
      }));
  tearDown(() => FilmMapStore.debugSet(null));

  // ------------------------------------------------------------------------------------------------ 1 readout
  for (final s in ['s', 'p', 'l', 'f']) {
    testWidgets('1 成像 header: the readout "强度 N%" is whole, the chip and 分项 stay ($s)', (t) async {
      final r = await audOpen(t, s, false, group: EGroup.style, before: (app, id) {
        app.film = testFilmCatalog;
        app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      });
      await audShot(t, '01_film_header_${s}_stock');
      // move the pad off centre so that the directions are in the full readout as well
      final f = await Finger.down(t, padAt(t, PadCat.film, 0.6, -0.5));
      await f.up();
      await settle(t, 4);
      final text = t.widget<Text>(key('header-readout')).data!;
      expect(text, startsWith('强度 '), reason: 'the unified readout leads');
      expect(text.contains('%'), isTrue);
      final ro = t.renderObject<RenderParagraph>(key('header-readout'));
      expect(ro.didExceedMaxLines, isFalse, reason: 'not ellipsised: "$text"');
      final box = t.getRect(key('header-readout'));
      final chip = t.getRect(key('stock-button')), items = t.getRect(key('items-button'));
      expect(box.left, greaterThanOrEqualTo(chip.right - 0.5), reason: 'the readout sits after the chip');
      expect(box.right, lessThanOrEqualTo(items.left + 0.5), reason: 'and ends before 分项 / 复位');
      await audShot(t, '02_film_header_${s}_moved');
      // no stock: 选成像 is the chip
      r.app.applyEdit(r.id, (e) => e.copyWith(film: null));
      await settle(t, 4);
      await audShot(t, '03_film_header_${s}_none');
      expect(t.takeException(), isNull);
    });
  }

  // ------------------------------------------------------------------------------------------------ 2 heights follow content
  // Heights follow the content of the layer; a change between layers of one category is animated (guidelines 15.1-7).
  for (final s in ['p', 'l', 'f']) {
    testWidgets('2 pad categories: no empty band under the pad, and the change to 分项 is animated, not a jump ($s)', (t) async {
      await audOpen(t, s, false, group: EGroup.light, before: (app, id) {
        app.film = testFilmCatalog;
        app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      });
      for (final g in [EGroup.light, EGroup.color, EGroup.style, EGroup.effects]) {
        if (g != EGroup.light) await dock(t, g); // 光线 is open already
        final cat = PadCatX.ofGroup(g)!;
        final pn = panelRect(t);
        final pad = t.getRect(key('pad-${cat.name}'));
        expect(pn.bottom - pad.bottom, lessThanOrEqualTo(kPanelBottomPad + 8), reason: '$g: nothing empty under the pad');
        await audShot(t, '10_${g.name}_pad_$s');
        final before = frame(t);
        await t.tap(key('items-button'));
        await t.pump();
        await t.pump(const Duration(milliseconds: 30));
        await t.pump(const Duration(milliseconds: 30));
        final mid = frame(t);
        await settle(t, 8);
        final after = frame(t);
        if ((after.$1.height - before.$1.height).abs() > 1 || after.$1.top != before.$1.top) {
          final moved = (mid.$1.top - before.$1.top).abs() > 0.5 || (mid.$1.top - after.$1.top).abs() > 0.5;
          expect(moved, isTrue, reason: '$g: the panel is in motion shortly after the tap');
          expect((mid.$1.top - after.$1.top).abs(), greaterThan(0.5), reason: '$g: it has not jumped to the end');
          expect((mid.$1.top - before.$1.top).abs(), lessThan((after.$1.top - before.$1.top).abs() + 0.5), reason: '$g: the panel moves towards the new height');
        }
        await audShot(t, '11_${g.name}_items_$s');
        await t.tap(key('header-back'));
        await settle(t, 8);
        if (g == EGroup.style) {
          await t.tap(key('stock-button'));
          await settle(t, 8);
          await audShot(t, '12_film_map_$s');
          await t.tap(key('header-back'));
          await settle(t, 6);
        }
      }
      expect(t.takeException(), isNull);
    });

    testWidgets('2 局部 portrait chips: the panel is as tall as the chips need, no empty band under them ($s)', (t) async {
      final fake = FakeLocalAi();
      fake.setFace(1, iod: 0.125);
      await audOpen(t, s, false, ai: fake);
      await audShot(t, '20_local_shape_card_$s');
      await tapKey(t, 'kind-portrait');
      fake.finish(portraitResult());
      await settle(t, 4);
      final pn = panelRect(t);
      var lowest = 0.0;
      for (final c in PortraitChip.values) {
        final f = key('chip-${c.name}');
        if (f.evaluate().isNotEmpty) lowest = lowest > t.getRect(f).bottom ? lowest : t.getRect(f).bottom;
      }
      expect(lowest, greaterThan(0));
      // the side pane is as tall as the screen lets it be: the rule is for the panel under the photo
      if (pn.width > t.view.physicalSize.width / t.view.devicePixelRatio * 0.7) {
        expect(pn.bottom - lowest, lessThanOrEqualTo(kPanelBottomPad + 40), reason: 'under the chips: ${pn.bottom - lowest} dp');
      }
      await audShot(t, '21_local_portrait_$s');
      final before = frame(t);
      await t.tap(key('items-button'));
      await t.pump();
      await t.pump(const Duration(milliseconds: 30));
      await t.pump(const Duration(milliseconds: 30));
      final mid = frame(t);
      await settle(t, 8);
      final after = frame(t);
      if ((after.$1.top - before.$1.top).abs() > 1) {
        expect((mid.$1.top - after.$1.top).abs(), greaterThan(0.5), reason: 'not a jump');
        expect((mid.$1.top - before.$1.top).abs(), greaterThan(0.01), reason: 'already moving');
      }
      await audShot(t, '22_local_portrait_items_$s');
      await t.tap(key('header-back'));
      await settle(t, 8);
      await tapKey(t, 'local-add');
      await addViaCard(t, 'radial');
      await audShot(t, '23_local_region_$s');
      expect(t.takeException(), isNull);
    });
  }

  // ------------------------------------------------------------------------------------------------ 3 selection
  group('3 the selected portrait chip', () {
    Future<LocalController> openPortrait(WidgetTester t, AppState app, FakeLocalAi fake) async {
      final r = await audOpen(t, 'p', false, ai: fake, useApp: app);
      return r.lc!;
    }

    testWidgets('comes back after reopening the photo, and after 撤销 of 关掉', (t) async {
      final fake = FakeLocalAi();
      fake.setFace(1, iod: 0.125);
      final app = AppState.demo();
      var lc = await openPortrait(t, app, fake);
      await tapKey(t, 'kind-portrait');
      fake.finish(portraitResult());
      await settle(t, 4);
      expect(lc.selChip, PortraitChip.skin, reason: 'the first chip when it opens');
      lc.tapChip(PortraitChip.lips);
      await settle(t, 3);
      expect(lc.selChip, PortraitChip.lips);
      await audShot(t, '30_portrait_lips_selected');
      final id = lc.ctl.id;
      // reopen
      await t.pumpWidget(const SizedBox());
      lc = await openPortrait(t, app, fake);
      await settle(t, 4);
      expect(lc.selChip, PortraitChip.lips, reason: 'reopened: what was selected before');
      await audShot(t, '31_portrait_reopened');
      // 关掉 and 撤销
      lc.tapChip(PortraitChip.lips); // second tap: action card
      lc.turnOffChip(PortraitChip.lips);
      await settle(t, 4);
      expect(lc.chipOn(PortraitChip.lips), isFalse);
      app.undo(id);
      await settle(t, 6);
      expect(lc.chipOn(PortraitChip.lips), isTrue);
      expect(lc.selChip, PortraitChip.lips, reason: 'after 撤销 of 关掉: the same chip, not the last one');
    });

    testWidgets('else the first chip (no memory of a selection)', (t) async {
      final fake = FakeLocalAi();
      fake.setFace(1, iod: 0.125);
      final a1 = AppState.demo();
      final lc1 = await openPortrait(t, a1, fake);
      await tapKey(t, 'kind-portrait');
      fake.finish(portraitResult());
      await settle(t, 4);
      lc1.tapChip(PortraitChip.lips);
      final id = lc1.ctl.id;
      final local = a1.editOf(id).local;
      await t.pumpWidget(const SizedBox());
      // a new app run: the same areas, nothing remembered
      final a2 = AppState.demo();
      a2.applyEdit(id, (e) => e.copyWith(local: local));
      final lc2 = await openPortrait(t, a2, FakeLocalAi());
      await settle(t, 4);
      expect(lc2.selChip, PortraitChip.skin, reason: 'the first chip, not 嘴唇 (the last area of the list)');
    });
  });

  // ------------------------------------------------------------------------------------------------ 4 tool strip
  for (final s in ['p', 'l', 'f', 's']) {
    testWidgets('4 the tools have a line under the photo only while a tool shows, the same place for every tool ($s)', (t) async {
      final fake = FakeLocalAi();
      fake.setFace(1, iod: 0.125);
      final r = await audOpen(t, s, false, ai: fake);
      await tapKey(t, 'kind-portrait');
      fake.finish(portraitResult());
      await settle(t, 4);
      final lc = r.lc!;
      expect(lc.selChip, PortraitChip.skin);
      final withTool = stageRect(t);
      void tools(String what) {
        final tr = t.getRect(key('local-tools'));
        final pn = panelRect(t);
        // the stage keeps the symmetric system insets around the photo (photoPad): the drawn photo ends that far above its bottom
        // (the photo is centred in the region above the strip's line and the strip hangs directly under the photo's lower edge)
        final photoBottom = t.getRect(key('local-handles')).bottom;
        expect(tr.top, greaterThanOrEqualTo(photoBottom - 0.5), reason: '$what: not over the photo (photo ends $photoBottom, strip ${tr.top})');
        expect(tr.top - photoBottom, lessThan(16), reason: '$what: attached to the photo, no detached gap');
        expect(tr.overlaps(pn.deflate(1)), isFalse, reason: '$what: not over the panel');
        expect(tr.height, greaterThanOrEqualTo(48));
        expect(tr.left, greaterThanOrEqualTo(0));
        expect(tr.right, lessThanOrEqualTo(t.view.physicalSize.width / t.view.devicePixelRatio));
      }

      // a chip with no tool: the line is not reserved, the photo gets its 48 dp back (animated, settled here)
      lc.tapChip(PortraitChip.shine);
      await settle(t, 10);
      expect(key('local-tools'), findsNothing);
      expect(stageRect(t).height, greaterThan(withTool.height + 10), reason: 'the photo area grew by the strip line (less the system inset it already keeps)');
      await audShot(t, '44_no_tool_$s');
      lc.tapChip(PortraitChip.skin);
      await settle(t, 10);
      expect(key('local-tools'), findsOneWidget);
      expect(stageRect(t).height, closeTo(withTool.height, 3));
      // 点一下祛斑 (portrait skin)
      tools('点一下祛斑 off');
      final healTop = t.getRect(key('local-tools')).top;
      await audShot(t, '40_heal_off_$s');
      await tapKey(t, 'heal-mode');
      tools('点一下祛斑 on, [+|−] switch');
      expect(key('heal-add'), findsOneWidget);
      expect(t.getRect(key('local-tools')).top, closeTo(healTop, 0.5), reason: 'the strip does not move when the tool opens');
      await audShot(t, '41_heal_on_$s');
      await tapKey(t, 'heal-mode');
      // brush area
      await tapKey(t, 'local-add');
      await addViaCard(t, 'brush');
      tools('brush');
      expect(key('brush-add'), findsOneWidget);
      await audShot(t, '42_brush_$s');
      // a luminance range
      await tapKey(t, 'local-add');
      await addViaCard(t, 'luminance');
      tools('luminance range');
      await audShot(t, '43_range_$s');
      expect(t.takeException(), isNull);
    });
  }

  // ------------------------------------------------------------------------------------------------ 5 publish
  for (final inset in [(47.0, 0.0), (0.0, 47.0), (24.0, 24.0)]) {
    testWidgets('5 分享…: every point of the drawn button opens the one sheet, a second tap adds none (padding ${inset.$1}, gesture ${inset.$2})', (t) async {
      useWindow(t, phoneSize, top: 36, bottom: inset.$1);
      t.view.systemGestureInsets = FakeViewPadding(bottom: inset.$2 * 2);
      await loadTestFonts();
      final app = AppState.demo();
      final ids = app.editable.take(1).map((e) => e.id).toList();
      await t.pumpWidget(RepaintBoundary(
        key: shotKey,
        child: MaterialApp(
          theme: buildTheme(Brightness.light),
          home: Scaffold(body: Center(child: Builder(builder: (ctx) => TextButton(onPressed: () => openPublish(ctx, app, ids), child: const Text('open'))))),
        ),
      ));
      await t.tap(find.text('open'));
      await t.pump();
      await t.pump(const Duration(milliseconds: 600));
      final btn = t.getRect(key('publish-share'));
      final screen = t.view.physicalSize / t.view.devicePixelRatio;
      final guard = inset.$1 > inset.$2 ? inset.$1 : inset.$2;
      expect(btn.height, 52);
      expect(btn.bottom, lessThanOrEqualTo(screen.height - guard + 0.5), reason: 'clear of the system bar / gesture zone');
      final sem = t.getSemantics(key('publish-share'));
      expect(sem.rect.height, closeTo(52, 1), reason: 'the semantics node is as tall as the drawn button');
      await audShot(t, '50_publish_${inset.$1.round()}_${inset.$2.round()}');
      for (final dy in [26.0, 2.0, 50.0]) {
        await t.tapAt(Offset(btn.center.dx, btn.top + dy));
        await t.pump();
        await t.pump(const Duration(milliseconds: 600));
        expect(key('handoff-system'), findsOneWidget, reason: 'tap at +$dy opens the sheet');
        expect(find.byType(BottomSheet), findsOneWidget);
        // the scrim above the sheet closes it
        await t.tapAt(const Offset(200, 80));
        for (var i = 0; i < 6; i++) {
          await t.pump(const Duration(milliseconds: 150)); // the exit animation needs a frame to start and one to finish
        }
        expect(key('handoff-system'), findsNothing, reason: 'the scrim closes it (after the tap at +$dy)');
        await t.pump(const Duration(milliseconds: 300));
      }
      // a double tap: one sheet, not two (one tap = one effect)
      await t.tapAt(btn.center);
      await t.tapAt(btn.center);
      await t.pump();
      await t.pump(const Duration(milliseconds: 600));
      expect(find.byType(BottomSheet), findsOneWidget);
      expect(t.takeException(), isNull);
    });
  }

  // ------------------------------------------------------------------------------------------------ 6 film map from the chip
  group('6 the 选成像 chip', () {
    Future<(AppState, String)> openFilm(WidgetTester t, String s) async {
      final r = await audOpen(t, s, false, group: EGroup.style, before: (app, id) {
        app.film = testFilmCatalog;
        app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      });
      return (r.app, r.id);
    }

    testWidgets('press opens the map at once; slide to a stock and release selects it and closes', (t) async {
      final (app, id) = await openFilm(t, 'p');
      final chip = t.getCenter(key('stock-button'));
      final f = await Finger.down(t, chip);
      await t.pump();
      await t.pump(const Duration(milliseconds: 50));
      expect(key('film-map'), findsOneWidget, reason: 'the press opened the map while the finger is still down');
      expect(app.editOf(id).film, 'kodak_portra_400');
      final map = t.getRect(key('film-map'));
      final into = map.topLeft + Offset(map.width * 0.8, map.height * 0.6);
      // the finger leaves the chip towards the map (a few steps), then slides inside it
      await f.drag(into - chip, step: 12);
      await settle(t, 3);
      final dots = layoutFilmMap(testFilmCatalog, FilmMapStore.positions!);
      await f.by(const Offset(0, 30));
      await f.by(const Offset(-20, 10));
      await settle(t, 6);
      await audShot(t, '60_film_map_slide');
      await f.up();
      await settle(t, 10);
      expect(key('film-map'), findsNothing, reason: 'release closes the map');
      final chosen = app.editOf(id).film;
      expect(chosen, isNot('kodak_portra_400'), reason: 'a stock was chosen by the slide');
      expect(dots.any((d) => d.id == chosen), isTrue);
      await audShot(t, '61_film_after_slide');
      expect(t.takeException(), isNull);
    });

    testWidgets('a plain tap opens the map and leaves it open for a tap on a stock', (t) async {
      final (app, id) = await openFilm(t, 'p');
      await t.tap(key('stock-button'));
      await settle(t, 10);
      expect(key('film-map'), findsOneWidget, reason: 'open after the tap, still open afterwards');
      expect(app.editOf(id).film, 'kodak_portra_400');
      await audShot(t, '62_film_map_open_after_tap');
      await t.tap(key('stock-kodak_ektar_100'));
      await settle(t, 10);
      expect(app.editOf(id).film, 'kodak_ektar_100');
      expect(key('film-map'), findsNothing);
    });

    testWidgets('pressed and slid, lifted before the map is reached: the map stays open', (t) async {
      final (app, id) = await openFilm(t, 'p');
      final chip = t.getCenter(key('stock-button'));
      final f = await Finger.down(t, chip);
      await f.drag(const Offset(30, 4), step: 6);
      await f.up();
      await settle(t, 10);
      expect(key('film-map'), findsOneWidget);
      expect(app.editOf(id).film, 'kodak_portra_400');
    });
  });
}
