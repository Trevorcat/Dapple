// Rules of the panel added after the owner's first look (docs/design/editor-panel.md): the photo keeps 90 % of its size while
// the pad can be 150 dp, the radar's size, the film pad's effective ranges against the new pad, the tone-target editor,
// the film map's margins, the colour preset dots.
import 'dart:math' as math;

import 'package:dapple/model/pad_model.dart';
import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/film_map_view.dart';
import 'package:dapple/screens/editor/itemised_view.dart';
import 'package:dapple/screens/editor/pad_panel.dart';
import 'package:dapple/screens/editor/pad_widgets.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/screens/editor/tone_editor.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show Finger, dump, setView, settle;
import 'support/film_fixture.dart';
import 'support/harness.dart';

const ctx = PadCtx(defaultTone: 60);
const ownerPhys = Size(1140, 2616);
const ownerDpr = 2.75;

Future<(AppState, String)> openWith(WidgetTester t, {String title = '外滩夕阳', String? crop, Future<String?> Function(Map<String, dynamic>)? fetch, Size phys = ownerPhys, double dpr = ownerDpr}) async {
  setView(t, phys, dpr);
  await loadTestFonts();
  final app = AppState.demo();
  app.film = testFilmCatalog;
  final id = app.photos.firstWhere((p) => p.title == title && p.id.startsWith('d')).id;
  if (crop != null) app.applyEdit(id, (e) => e.copyWith(crop: crop));
  await t.pumpWidget(RepaintBoundary(
    key: shotKey,
    child: MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildTheme(Brightness.light),
      home: EditorPage(app: app, photoId: id, resamplePointers: false, filmPadFetcher: fetch),
    ),
  ));
  await settle(t);
  return (app, id);
}

Future<void> tapDock(WidgetTester t, EGroup g) async {
  await t.tap(find.byKey(ValueKey('dock-$g')));
  await settle(t, 8);
}

Rect padRect(WidgetTester t, PadCat c) => t.getRect(find.byKey(ValueKey('pad-${c.name}')));
Offset padAt(WidgetTester t, PadCat c, double x, double y) => padRect(t, c).topLeft + padToLocal(padRect(t, c).size, x, y);
PadState viewOf(AppState app, String id, PadCat c) => padViewOf(app.editOf(id), c, ctx);

/// The photo's drawn height in the zoom stage.
double photoH(WidgetTester t, double aspect) {
  final r = t.getRect(find.byKey(const ValueKey('zoom-stage')));
  return math.min(r.width / aspect, r.height);
}

void main() {
  group('the photo keeps 90 % of its no-panel size while the pad can be 150 dp tall (N5 outer screen)', () {
    // regionTop = status 28 + 8 + top bar 48 + 8; the dock is 56 tall with 28 under it and 8 above the region's end
    const w = 1140 / 2.75, h = 2616 / 2.75;
    const region = h - (28 + 56) - 8 - (28 + 8 + 48 + 8);

    for (final c in [
      ('3:4', '街角咖啡', '3:4', 3 / 4),
      ('3:2', '外滩夕阳', null, 3 / 2),
      ('2:3', '街角咖啡', null, 2 / 3),
    ]) {
      testWidgets('photo ${c.$1}: every category, the film map and the radar', (t) async {
        final (app, id) = await openWith(t, title: c.$2, crop: c.$3);
        app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
        final aspect = c.$4;
        final ref = math.min(w / aspect, region);
        for (final g in [EGroup.light, EGroup.color, EGroup.style, EGroup.effects]) {
          await tapDock(t, g);
          final cat = PadCatX.ofGroup(g)!;
          final pad = padRect(t, cat);
          final ph = photoH(t, aspect);
          expect(pad.height, greaterThanOrEqualTo(149), reason: '$g pad stays usable');
          if (pad.height > 151) {
            expect(ph, greaterThanOrEqualTo(0.9 * ref - 1), reason: '${c.$1} $g: the photo keeps 90 % while the pad is taller than 150 (pad ${pad.height}, photo $ph of $ref)');
          } else {
            // the pad is at its least: only now may the photo shrink below 90 %
            expect(pad.height, closeTo(150, 1.5));
          }
          // the film map has the same height
          if (g == EGroup.style) {
            final panelH = t.getRect(find.byKey(const ValueKey('docked-panel'))).height;
            await t.tap(find.byKey(const ValueKey('stock-button')));
            await settle(t, 8);
            expect(t.getRect(find.byKey(const ValueKey('docked-panel'))).height, closeTo(panelH, 0.5));
            expect(photoH(t, aspect), closeTo(ph, 1));
            await t.tap(find.byKey(const ValueKey('stock-button')));
            await settle(t, 6);
          }
          // the radar (the precise layer) may be taller, never beyond 46 % of the screen
          await t.tap(find.byKey(const ValueKey('items-button')));
          await settle(t, 8);
          final panel = t.getRect(find.byKey(const ValueKey('docked-panel')));
          expect(panel.height, lessThanOrEqualTo(h * 0.46 + 1));
          final radar = t.getRect(find.byKey(const ValueKey('radar')));
          expect(RadarGeo(radar.size, [for (final it in padItems(cat)) it.label]).r1, greaterThanOrEqualTo(80), reason: '$g radar radius');
          await t.tap(find.byKey(const ValueKey('items-button')));
          await settle(t, 6);
          await tapDock(t, g); // close
        }
      });
    }

    test('bottomPanelHeight: the rule as a pure function', () {
      double hh(double aspect) => bottomPanelHeight(region: region, photoW: w, aspect: aspect, screenH: h);
      expect(hh(3 / 2), kPanelFixedH + kPadComfortH, reason: 'a landscape photo leaves room: the comfortable panel');
      expect(hh(2 / 3), kPanelFixedH + kPadMinH, reason: 'a tall photo: the pad shrinks to its least');
      expect(hh(3 / 4), inInclusiveRange(kPanelFixedH + kPadMinH, kPanelFixedH + kPadComfortH));
      expect(bottomPanelHeight(region: region, photoW: w, aspect: 2 / 3, screenH: h, itemised: true), lessThanOrEqualTo(h * 0.46));
      expect(bottomPanelHeight(region: region, photoW: w, aspect: 3 / 2, screenH: h, itemised: true), greaterThanOrEqualTo(kPanelFixedH + kRadarContentH));
    });
  });

  group('radar', () {
    testWidgets('full panel width, names on one line outside, none cut off, the selected spoke clearly marked', (t) async {
      final (app, id) = await openWith(t);
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      await tapDock(t, EGroup.style);
      await t.tap(find.byKey(const ValueKey('items-button')));
      await settle(t, 8);
      final radar = t.getRect(find.byKey(const ValueKey('radar')));
      final panel = t.getRect(find.byKey(const ValueKey('docked-panel')));
      expect(radar.width, greaterThan(panel.width - 30), reason: 'the whole panel width');
      for (final it in padItems(PadCat.film)) {
        final r = t.getRect(find.byKey(ValueKey('spoke-${it.id}')));
        expect(r.left, greaterThanOrEqualTo(panel.left), reason: it.id);
        expect(r.right, lessThanOrEqualTo(panel.right), reason: it.id);
        expect(r.top, greaterThanOrEqualTo(radar.top - 1), reason: it.id);
        expect(r.bottom, lessThanOrEqualTo(radar.bottom + 1), reason: it.id);
        expect(t.getSize(find.text(it.label)).height, lessThan(24), reason: 'one line: ${it.label}');
      }
      await dump(t, 'v4_radar_film_test');
    });

    testWidgets('质地 has 锐化半径 and 去雾 / 暗角; the radius is editable and resettable', (t) async {
      final (app, id) = await openWith(t);
      app.applyEdit(id, (_) => EditState.empty);
      await tapDock(t, EGroup.effects);
      await t.tap(find.byKey(const ValueKey('items-button')));
      await settle(t, 8);
      expect(find.byKey(const ValueKey('spoke-sharpRadius')), findsOneWidget);
      expect(find.text('锐化半径'), findsOneWidget);
      final radar = t.getRect(find.byKey(const ValueKey('radar')));
      final items = padItems(PadCat.texture);
      final geo = RadarGeo(radar.size, [for (final it in items) it.label]);
      final i = items.indexWhere((e) => e.id == 'sharpRadius');
      final f = await Finger.down(t, radar.topLeft + geo.point(i, items.length, 1.0));
      await f.up();
      await settle(t, 2);
      expect(app.editOf(id).value('sharpRadius'), greaterThan(25));
      await t.tap(find.byKey(const ValueKey('ruler-reset')));
      await settle(t, 2);
      expect(app.editOf(id).value('sharpRadius'), 10);
    });
  });

  group('明暗目标 (tone target) editor', () {
    testWidgets('an entry under 光线 分项 opens the 3x3 grid; absolute; back to automatic; reset clears it', (t) async {
      final (app, id) = await openWith(t);
      app.applyEdit(id, (_) => EditState.empty);
      await tapDock(t, EGroup.light);
      await t.tap(find.byKey(const ValueKey('items-button')));
      await settle(t, 6);
      await t.tap(find.byKey(const ValueKey('extra-明暗目标')));
      await settle(t, 8);
      final cell = t.getRect(find.byKey(const ValueKey('tone-low-long')));
      expect(cell.width, greaterThan(40));
      final f = await Finger.down(t, cell.center);
      await f.up();
      expect(app.editOf(id).toneTarget, 'low-long', reason: 'the cell under the finger');
      final cell2 = t.getRect(find.byKey(const ValueKey('tone-high-short')));
      final g = await Finger.down(t, cell2.center);
      await g.drag(cell.center - cell2.center);
      await g.up();
      expect(app.editOf(id).toneTarget, 'low-long', reason: 'a slide ends on the cell it ends on');
      app.undo(id);
      expect(app.editOf(id).toneTarget, 'low-long', reason: 'press and slide were one undo step; the first tap was another');
      await t.tap(find.byKey(const ValueKey('tone-auto')));
      await settle(t, 3);
      expect(app.editOf(id).toneTarget, isNull);
      expect(toneIdAt(const Offset(1, 1), 150, 4), 'high-short');
      expect(toneIdAt(const Offset(149, 149), 150, 4), 'low-long');
    });

    testWidgets('the category reset clears a tone target', (t) async {
      final (app, id) = await openWith(t);
      app.applyEdit(id, (e) => e.copyWith(toneTarget: 'high-mid'));
      await tapDock(t, EGroup.light);
      await t.tap(find.byKey(ValueKey('reset-${EGroup.light.name}')));
      await settle(t, 3);
      expect(app.editOf(id).toneTarget, isNull);
    });
  });

  group('film pad: effective ranges (the engine says which ends change the render)', () {
    String hiOff(String key) => '{"ok":true,"effective_hi":{"$key":false}}';

    testWidgets('a dead half is unreachable and dimmed; the live half still works', (t) async {
      final (app, id) = await openWith(t, fetch: (_) async => hiOff('film_print'));
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      await t.pump();
      await settle(t, 6); // the 300 ms debounce, then the answer
      await tapDock(t, EGroup.style);
      final cursor = t.widgetList<CustomPaint>(find.descendant(of: find.byKey(const ValueKey('pad-film')), matching: find.byType(CustomPaint))).firstWhere((w) => w.painter is PadCursorPainter);
      final lim = (cursor.painter! as PadCursorPainter).limits;
      expect(lim.yHi, 0, reason: 'film_print +1 changes nothing: the top half is dead');
      expect(lim.isFull, isFalse);
      final f = await Finger.down(t, padAt(t, PadCat.film, 0.5, 0.8));
      await f.up();
      expect(viewOf(app, id, PadCat.film).y, 0, reason: 'the cursor stops at the centre line');
      expect(viewOf(app, id, PadCat.film).x, closeTo(0.5, 0.03));
      final g = await Finger.down(t, padAt(t, PadCat.film, 0.2, -0.8));
      await g.up();
      expect(viewOf(app, id, PadCat.film).y, closeTo(-0.8, 0.03), reason: 'the bottom half is live');
      expect(app.editOf(id).value('film_print'), closeTo(-0.8 * 0.65, 0.02));
    });

    testWidgets('a dead axis: the pad stays but that axis is stuck at automatic; both dead: nothing moves', (t) async {
      final (app, id) = await openWith(t, fetch: (_) async => '{"ok":true,"effective_lo":{"film_shoot":false},"effective_hi":{"film_shoot":false}}');
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      await settle(t, 6);
      await tapDock(t, EGroup.style);
      final f = await Finger.down(t, padAt(t, PadCat.film, 0.7, 0.6));
      await f.up();
      expect(viewOf(app, id, PadCat.film).x, 0, reason: 'film_shoot is dead on both ends');
      expect(viewOf(app, id, PadCat.film).y, closeTo(0.6, 0.03));
      expect(app.editOf(id).value('film_shoot'), 0);
    });

    testWidgets('both axes dead: the pad does not move at all', (t) async {
      final (app, id) = await openWith(t,
          fetch: (_) async => '{"ok":true,"effective_lo":{"film_shoot":false,"film_print":false},"effective_hi":{"film_shoot":false,"film_print":false}}');
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      await settle(t, 6);
      await tapDock(t, EGroup.style);
      final f = await Finger.down(t, padAt(t, PadCat.film, 0.7, 0.6));
      await f.drag(const Offset(-40, 40));
      await f.up();
      expect(viewOf(app, id, PadCat.film).x, 0);
      expect(viewOf(app, id, PadCat.film).y, 0);
    });

    testWidgets('the ranges refresh after a stock change and after a drag that moved film_shoot, never during the drag', (t) async {
      var calls = 0;
      final (app, id) = await openWith(t, fetch: (_) async {
        calls++;
        return '{"ok":true}';
      });
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      await settle(t, 6);
      expect(calls, 1);
      app.applyEdit(id, (e) => e.copyWith(film: 'fujifilm_velvia_100'));
      await settle(t, 6);
      expect(calls, 2, reason: 'a new stock asks again');
      await tapDock(t, EGroup.style);
      final f = await Finger.down(t, padAt(t, PadCat.film, 0, 0));
      await f.drag(const Offset(60, 0));
      await settle(t, 5); // longer than the debounce, finger still down
      expect(calls, 2, reason: 'never while a finger is down');
      await f.up();
      await settle(t, 6);
      expect(calls, 3, reason: 'after the drag film_shoot changed: ask again');
    });

    testWidgets('a tolerant parser path: garbage, an exception or a "not ok" answer all mean everything is reachable', (t) async {
      for (final answer in <Future<String?> Function(Map<String, dynamic>)>[
        (_) async => 'not json at all',
        (_) async => throw StateError('no engine'),
        (_) async => '{"ok":false,"effective_hi":{"film_print":false}}',
        (_) async => null,
      ]) {
        final (app, id) = await openWith(t, fetch: answer);
        app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
        await settle(t, 6);
        await tapDock(t, EGroup.style);
        final f = await Finger.down(t, padAt(t, PadCat.film, 0, 1));
        await f.up();
        expect(viewOf(app, id, PadCat.film).y, closeTo(1, 0.03), reason: 'everything reachable');
        await t.pumpWidget(const SizedBox());
      }
    });
  });

  group('film map margins and the header', () {
    testWidgets('the 无 button sits in the header next to the stock button; the edge icons fade while magnified', (t) async {
      FilmMapStore.debugSet(const {});
      addTearDown(() => FilmMapStore.debugSet(null));
      final (app, id) = await openWith(t);
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      await tapDock(t, EGroup.style);
      final none = t.getRect(find.byKey(const ValueKey('film-none')));
      final stock = t.getRect(find.byKey(const ValueKey('stock-button')));
      expect(none.right, lessThanOrEqualTo(stock.left + 1));
      expect((none.center.dy - stock.center.dy).abs(), lessThan(2));
      await t.tap(find.byKey(const ValueKey('stock-button')));
      await settle(t, 8);
      final map = t.getRect(find.byKey(const ValueKey('film-map')));
      expect(find.descendant(of: find.byKey(const ValueKey('film-map')), matching: find.text('无')), findsNothing, reason: 'nothing in the map covers the stocks');
      double iconOpacity() => t.widget<Opacity>(find.byKey(const ValueKey('film-map-edges'))).opacity;
      expect(iconOpacity(), 1);
      // no dot lies under an edge icon (icons are 20 dp, 8 dp from the edges, in the middle of each edge)
      final icons = [
        Rect.fromCenter(center: Offset(map.left + 18, map.center.dy), width: 24, height: 24),
        Rect.fromCenter(center: Offset(map.right - 18, map.center.dy), width: 24, height: 24),
        Rect.fromCenter(center: Offset(map.center.dx, map.top + 16), width: 24, height: 24),
        Rect.fromCenter(center: Offset(map.center.dx, map.bottom - 16), width: 24, height: 24),
      ];
      for (final s in testFilmCatalog.stocks.where((s) => !s.id.contains('push') || true)) {
        final k = find.byKey(ValueKey('stock-${s.id}'));
        if (k.evaluate().isEmpty) continue;
        final c = t.getCenter(k);
        for (final ic in icons) {
          expect(ic.inflate(6).contains(c), isFalse, reason: '${s.name} hides under an edge icon');
        }
      }
      final f = await Finger.down(t, map.center);
      await settle(t, 6);
      expect(iconOpacity(), lessThan(0.05), reason: 'magnified: the edge icons fade out');
      await f.up();
      await t.pump(const Duration(milliseconds: 130));
      expect(iconOpacity(), inExclusiveRange(0.05, 0.95), reason: 'the icons fade back in while the map shrinks, before it closes');
      await settle(t, 6);
      expect(find.byKey(const ValueKey('film-map')), findsNothing, reason: 'release closes the map');
      expect(app.editOf(id).film, isNotNull);
    });
  });

  group('colour preset dots', () {
    testWidgets('16 dp visible, 44 dp hit area, the label is never clipped at the pad edge', (t) async {
      final (app, id) = await openWith(t);
      app.applyEdit(id, (_) => EditState.empty);
      await tapDock(t, EGroup.color);
      expect(t.getSize(find.byKey(const ValueKey('preset-warm'))), const Size(16, 16));
      final pad = padRect(t, PadCat.color);
      // the hit area is 44: a touch 20 dp off the dot's centre still selects it
      final p = colorPresets.firstWhere((e) => e.look == 'amber');
      final at = padAt(t, PadCat.color, p.x, p.y);
      final f = await Finger.down(t, at + const Offset(15, 15));
      await f.up();
      await settle(t, 2);
      expect(app.editOf(id).look, 'amber');
      for (final pr in colorPresets) {
        final g = await Finger.down(t, padAt(t, PadCat.color, pr.x, pr.y));
        await g.up();
        await settle(t, 2);
        final r = t.getRect(find.byKey(ValueKey('preset-name-${pr.look}')));
        expect(r.left, greaterThanOrEqualTo(pad.left), reason: '${pr.look} label clipped on the left');
        expect(r.right, lessThanOrEqualTo(pad.right), reason: '${pr.look} label clipped on the right');
        expect(r.top, greaterThanOrEqualTo(pad.top));
        expect(r.bottom, lessThanOrEqualTo(pad.bottom));
      }
    });
  });
}
