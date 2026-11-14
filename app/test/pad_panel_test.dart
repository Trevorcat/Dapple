// The control panel of the editor, version 0.6 (docs/design/editor-panel.md): the dock glyphs, the hold popup, the pad and the
// strength strip (absolute mapping), the film map, the itemised view (radar + ruler), the complex editors, old edits, the
// adaptive placement, and that nothing in the panel scrolls sideways. Screenshots go to test/goldens/editor_v2/v3_*.png.
import 'package:dapple/editor/fold_layout.dart' show debugFoldLayouts;
import 'package:dapple/model/film_map.dart';
import 'package:dapple/model/pad_model.dart';
import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/film_map_view.dart';
import 'package:dapple/screens/editor/itemised_view.dart';
import 'package:dapple/screens/editor/pad_dock.dart';
import 'package:dapple/screens/editor/pad_panel.dart';
import 'package:dapple/screens/editor/pad_widgets.dart';
import 'package:dapple/state/app_state.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show Finger, dump, n5Dpr, n5Phys, open, setView, settle;
import 'support/film_fixture.dart';

const ctx = PadCtx(defaultTone: 60);
const ownerPhys = Size(1140, 2616); // the owner's foldable outer screen
const ownerDpr = 2.75;

Future<(AppState, String)> openFilm(WidgetTester t, {Size phys = ownerPhys, double dpr = ownerDpr, bool dark = false, String title = '外滩夕阳'}) async {
  final r = await open(t, phys: phys, dpr: dpr, dark: dark, title: title);
  r.$1.film = testFilmCatalog;
  return r;
}

Future<void> tapDock(WidgetTester t, EGroup g) async {
  await t.tap(find.byKey(ValueKey('dock-$g')));
  await settle(t, 8);
}

Rect padRect(WidgetTester t, PadCat c) => t.getRect(find.byKey(ValueKey('pad-${c.name}')));
Rect stripRect(WidgetTester t, PadCat c) => t.getRect(find.byKey(ValueKey('strip-${c.name}')));

/// The screen position of pad coordinates (x, y) on the pad of [c] (the inset keeps the cursor inside the field).
Offset padAt(WidgetTester t, PadCat c, double x, double y) {
  final r = padRect(t, c);
  final p = padToLocal(r.size, x, y);
  return r.topLeft + p;
}

PadState viewOf(AppState app, String id, PadCat c) => padViewOf(app.editOf(id), c, ctx);

void main() {
  // these tests are about the ordinary panel placement (side pane on a wide window); the fold layouts have their own tests (fold_layout_test.dart)
  setUp(() => debugFoldLayouts = false);
  tearDown(() => debugFoldLayouts = true);
  group('dock', () {
    testWidgets('five items; the first four are live glyphs (dot = pad position, bar = strength), 裁切 is an icon', (t) async {
      final (app, id) = await openFilm(t);
      for (final l in ['光线', '颜色', '成像', '质地', '裁切']) {
        expect(find.text(l), findsWidgets, reason: l);
      }
      expect(find.byKey(const ValueKey('docked-panel')), findsNothing);
      for (final c in PadCat.values) {
        expect(find.byKey(ValueKey('glyph-${c.name}')), findsOneWidget);
      }
      GlyphPainter glyph(PadCat c) => (t.widget<CustomPaint>(find.byKey(ValueKey('glyph-${c.name}'))).painter! as GlyphPainter);
      expect(glyph(PadCat.light).x, 0);
      expect(glyph(PadCat.light).a, 0.5);
      app.applyEdit(id, (e) => applyPadState(e, PadCat.light, const PadState(x: 0.6, y: -0.4, a: 0.8), ctx));
      await settle(t, 2);
      expect(glyph(PadCat.light).x, 0.6);
      expect(glyph(PadCat.light).y, -0.4);
      expect(glyph(PadCat.light).a, 0.8);
      expect(glyph(PadCat.color).x, 0, reason: 'the other glyphs stay');
      // film glyph is dim without a stock
      expect(t.widget<Opacity>(find.ancestor(of: find.byKey(const ValueKey('glyph-film')), matching: find.byType(Opacity)).first).opacity, lessThan(0.6));
      await dump(t, 'v3_dock_glyphs');
    });

    testWidgets('a tap opens the panel with ONE pad and ONE strength strip and nothing to scroll', (t) async {
      await openFilm(t);
      for (final g in [EGroup.light, EGroup.color, EGroup.style, EGroup.effects]) {
        await tapDock(t, g);
        final cat = PadCatX.ofGroup(g)!;
        expect(find.byKey(const ValueKey('docked-panel')), findsOneWidget);
        expect(find.byKey(ValueKey('pad-${cat.name}')), findsOneWidget);
        expect(find.byKey(ValueKey('strip-${cat.name}')), findsOneWidget);
        expect(find.byKey(const ValueKey('items-button')), findsOneWidget);
        _noHorizontalScroll(t);
        // strength strip on the left, the pad filling the rest of the row, both as tall as the row
        final pad = padRect(t, cat), strip = stripRect(t, cat);
        expect(strip.right, lessThan(pad.left));
        expect(strip.height, closeTo(pad.height, 0.5));
        expect(strip.width, 40);
      }
      await tapDock(t, EGroup.effects);
      expect(find.byKey(const ValueKey('docked-panel')), findsNothing, reason: 'the open category toggles closed');
    });

    testWidgets('the panel keeps one height within a category (pad, film map, complex editors); the itemised view may be taller, never beyond 46 % of the screen', (t) async {
      await openFilm(t);
      await tapDock(t, EGroup.style);
      final h0 = t.getRect(find.byKey(const ValueKey('docked-panel'))).height;
      await t.tap(find.byKey(const ValueKey('stock-button')));
      await settle(t, 8);
      expect(t.getRect(find.byKey(const ValueKey('docked-panel'))).height, closeTo(h0, 0.5));
      await t.tap(find.byKey(const ValueKey('header-back')));
      await settle(t, 8);
      await t.tap(find.byKey(const ValueKey('items-button')));
      await settle(t, 8);
      final h1 = t.getRect(find.byKey(const ValueKey('docked-panel'))).height;
      expect(h1, greaterThanOrEqualTo(h0 - 0.5));
      expect(h1, lessThanOrEqualTo(951.3 * 0.46 + 1));
      _noHorizontalScroll(t);
    });
  });

  group('pad (absolute mapping)', () {
    testWidgets('the cursor is under the finger: the position of the touch is the pad position', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (_) => EditState.empty);
      await tapDock(t, EGroup.light);
      final r = padRect(t, PadCat.light);
      final target = padAt(t, PadCat.light, 0.5, -0.5);
      final f = await Finger.down(t, target);
      await f.up();
      final s = viewOf(app, id, PadCat.light);
      expect(s.x, closeTo(0.5, 0.02));
      expect(s.y, closeTo(-0.5, 0.02));
      // a jump to another place goes straight there (no relative drag-anywhere mapping)
      final g = await Finger.down(t, padAt(t, PadCat.light, -0.8, 0.9));
      await g.up();
      expect(viewOf(app, id, PadCat.light).x, closeTo(-0.8, 0.02));
      expect(viewOf(app, id, PadCat.light).y, closeTo(0.9, 0.02));
      // the field fills the whole pad: the pad starts at the strip's right edge plus the gap, no inner padding
      expect(r.left - stripRect(t, PadCat.light).right, 12);
      // the recipe wrote the parameters
      expect(app.editOf(id).value('exposure'), closeTo(-0.8 * 16, 0.6));
      // the edges clamp
      final h = await Finger.down(t, r.topRight - const Offset(1, -1));
      await h.up();
      expect(viewOf(app, id, PadCat.light).x, 1);
      expect(viewOf(app, id, PadCat.light).y, 1);
    });

    testWidgets('a drag follows the finger absolutely, with a little snap to the centre; one gesture = one undo step', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (_) => EditState.empty);
      await tapDock(t, EGroup.light);
      final f = await Finger.down(t, padAt(t, PadCat.light, 0, 0));
      await f.drag(const Offset(60, -40));
      final s = viewOf(app, id, PadCat.light);
      final want = padFromLocal(padRect(t, PadCat.light).size, padToLocal(padRect(t, PadCat.light).size, 0, 0) + const Offset(60, -40));
      expect(s.x, closeTo(want.dx, 0.03));
      expect(s.y, closeTo(want.dy, 0.03));
      await f.drag(Offset(-60, 40));
      await f.up();
      expect(viewOf(app, id, PadCat.light).x, 0, reason: 'back at the centre it snaps to automatic');
      expect(viewOf(app, id, PadCat.light).y, 0);
      expect(app.editOf(id).value('exposure'), 0);
      app.undo(id);
      expect(app.canUndo(id), isTrue, reason: 'only the test setup is left');
      expect(app.editOf(id).sameAs(EditState.empty), isTrue, reason: 'one gesture = one undo step');
    });

    testWidgets('the pad only reacts inside its drawn area (a touch beside it does nothing)', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (_) => EditState.empty);
      await tapDock(t, EGroup.light);
      final r = padRect(t, PadCat.light);
      final f = await Finger.down(t, Offset(r.left - 6, r.center.dy)); // in the gap between strip and pad
      await f.drag(const Offset(80, 0));
      await f.up();
      expect(viewOf(app, id, PadCat.light).isCentre, isTrue);
      final header = t.getRect(find.byKey(const ValueKey('header-title')));
      final g = await Finger.down(t, Offset(r.center.dx, header.center.dy));
      await g.drag(const Offset(0, 120));
      await g.up();
      expect(viewOf(app, id, PadCat.light).isCentre, isTrue);
    });

    testWidgets('a drag does not rebuild the panel', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (_) => EditState.empty);
      await tapDock(t, EGroup.color);
      final f = await Finger.down(t, padAt(t, PadCat.color, 0, 0));
      await f.drag(const Offset(10, 0)); // the first step may announce the edit
      final n = debugPanelBuilds;
      await f.drag(const Offset(120, -60));
      await f.drag(const Offset(-60, 90));
      expect(debugPanelBuilds, n, reason: 'only the cursor, the glyphs and the readout repaint');
      await f.up();
    });

    testWidgets('colour pad: preset dots; a tap moves the cursor there and selects the style; the selected one shows its name', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (_) => EditState.empty);
      await tapDock(t, EGroup.color);
      expect(find.byKey(const ValueKey('preset-warm')), findsOneWidget);
      expect(find.byKey(const ValueKey('preset-name-warm')), findsNothing);
      final p = colorPresets.firstWhere((e) => e.look == 'amber');
      final f = await Finger.down(t, padAt(t, PadCat.color, p.x, p.y));
      await t.pump(const Duration(milliseconds: 50));
      expect(find.byKey(const ValueKey('preset-name-amber')), findsOneWidget, reason: 'the pressed preset names itself');
      await f.up();
      await settle(t, 2);
      expect(app.editOf(id).look, 'amber');
      expect(viewOf(app, id, PadCat.color).x, closeTo(p.x, 1e-9));
      expect(find.byKey(const ValueKey('preset-name-amber')), findsOneWidget, reason: 'the selected one keeps its name');
      await dump(t, 'v3_color_presets');
    });
  });

  group('strength strip', () {
    testWidgets('absolute: bottom 0, middle 100 % (snaps), top heavier; the strength item follows', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (_) => EditState.empty);
      await tapDock(t, EGroup.light);
      final r = stripRect(t, PadCat.light);
      Offset at(double a) => Offset(r.center.dx, r.top + stripToLocal(r.height, a));
      final f = await Finger.down(t, at(1));
      await f.up();
      expect(viewOf(app, id, PadCat.light).a, closeTo(1, 0.01));
      expect(app.editOf(id).toneStrength, closeTo(100, 1));
      final g = await Finger.down(t, at(0));
      await g.up();
      expect(viewOf(app, id, PadCat.light).a, closeTo(0, 0.01));
      expect(app.editOf(id).toneStrength, closeTo(0, 1e-6));
      final h = await Finger.down(t, at(0.51));
      await h.up();
      expect(viewOf(app, id, PadCat.light).a, 0.5, reason: 'snaps to 100 %');
      expect(app.editOf(id).toneStrength, isNull);
      // below the strip's bottom edge it clamps
      final k = await Finger.down(t, Offset(r.center.dx, r.bottom - 1));
      await k.drag(const Offset(0, -30));
      await k.up();
      expect(viewOf(app, id, PadCat.light).a, greaterThan(0));
    });

    testWidgets('film: the strip is the strength of the film bundle; a stock is chosen when none is', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (_) => EditState.empty);
      await tapDock(t, EGroup.style);
      expect(app.editOf(id).film, isNull);
      final r = stripRect(t, PadCat.film);
      final f = await Finger.down(t, Offset(r.center.dx, r.top + stripToLocal(r.height, 0.25)));
      await f.up();
      expect(app.editOf(id).film, isNotNull);
      expect(app.editOf(id).value('film_strength'), closeTo(50, 2));
      final g = await Finger.down(t, Offset(r.center.dx, r.top + stripToLocal(r.height, 0.95)));
      await g.up();
      expect(app.editOf(id).value('film_strength'), 100);
      expect(app.editOf(id).value('film_black'), lessThan(0), reason: 'above the middle the black lifts');
    });
  });

  group('hold popup', () {
    testWidgets('press and hold a glyph: the pad pops up above the dock, the photo keeps its size, the drag is relative, release closes it', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (_) => EditState.empty);
      final photoBefore = t.getRect(find.byKey(const ValueKey('zoom-stage')));
      final glyph = t.getCenter(find.byKey(ValueKey('dock-${EGroup.light}')));
      final f = await Finger.down(t, glyph);
      await t.pump(const Duration(milliseconds: 400));
      expect(find.byKey(const ValueKey('hold-popup')), findsOneWidget);
      expect(find.byKey(const ValueKey('docked-panel')), findsNothing);
      final pop = t.getRect(find.byKey(const ValueKey('hold-popup')));
      expect(pop.bottom, lessThan(t.getTopLeft(find.byKey(ValueKey('dock-${EGroup.light}'))).dy));
      expect(pop.width, closeTo(216, 1));
      await f.drag(const Offset(47, -47)); // half a pad range right and up
      expect(viewOf(app, id, PadCat.light).x, closeTo(0.5, 0.06));
      expect(viewOf(app, id, PadCat.light).y, closeTo(0.5, 0.06));
      await dump(t, 'v3_hold_light');
      await f.up();
      await settle(t, 3);
      expect(find.byKey(const ValueKey('hold-popup')), findsNothing);
      expect(find.byKey(const ValueKey('docked-panel')), findsNothing, reason: 'a hold never opens the panel');
      expect(t.getRect(find.byKey(const ValueKey('zoom-stage'))).size, photoBefore.size, reason: 'the photo stays full size');
      expect(viewOf(app, id, PadCat.light).x, closeTo(0.5, 0.06), reason: 'release keeps the value');
      app.undo(id);
      expect(app.editOf(id).sameAs(EditState.empty), isTrue, reason: 'one gesture = one undo step');
    });

    testWidgets('starting to drag on a glyph pops the pad up at once; a short tap still opens the panel', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (_) => EditState.empty);
      final f = await Finger.down(t, t.getCenter(find.byKey(ValueKey('dock-${EGroup.color}'))));
      await f.drag(const Offset(20, 0));
      expect(find.byKey(const ValueKey('hold-popup')), findsOneWidget);
      expect(viewOf(app, id, PadCat.color).x, greaterThan(0));
      await f.up();
      await settle(t, 3);
      expect(find.byKey(const ValueKey('hold-popup')), findsNothing);
      await tapDock(t, EGroup.color);
      expect(find.byKey(const ValueKey('docked-panel')), findsOneWidget);
      expect(find.byKey(const ValueKey('hold-popup')), findsNothing);
    });
  });

  group('film map', () {
    setUp(() {
      // a hand-made map: three stocks spread out, the rest on the automatic grid
      FilmMapStore.debugSet({
        'kodak_portra_400': const Offset(-0.5, -0.3),
        'kodak_ektar_100': const Offset(0.6, 0.5),
        'fujifilm_velvia_100': const Offset(0.0, 0.8),
      });
    });
    tearDown(() => FilmMapStore.debugSet(null));

    testWidgets('the stock button shows the current stock and opens the map in place of the pad', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      await tapDock(t, EGroup.style);
      expect(find.descendant(of: find.byKey(const ValueKey('stock-button')), matching: find.text('N400C')), findsOneWidget);
      await t.tap(find.byKey(const ValueKey('stock-button')));
      await settle(t, 6);
      expect(find.byKey(const ValueKey('film-map')), findsOneWidget);
      expect(find.byKey(const ValueKey('pad-film')), findsNothing);
      expect(find.byKey(const ValueKey('strip-film')), findsNothing);
      expect(find.byKey(const ValueKey('film-none')), findsOneWidget);
      // every catalog stock is a dot; without magnification only the selected one is named
      expect(find.byKey(const ValueKey('stock-kodak_ektar_100')), findsOneWidget);
      expect(t.widget<Text>(find.byKey(const ValueKey('header-sub'))).data, 'N400C', reason: 'the header names the chosen stock');
      expect(find.text('N400C'), findsNWidgets(2), reason: 'the subtitle and the selected dot');
      expect(find.byKey(const ValueKey('header-title')), findsOneWidget);
      expect(t.widget<Text>(find.byKey(const ValueKey('header-title'))).data, '成像');
      expect(find.text('N100'), findsNothing);
      await dump(t, 'v3_film_map_idle');
      await t.tap(find.byKey(const ValueKey('header-back')));
      await settle(t, 6);
      expect(find.byKey(const ValueKey('pad-film')), findsOneWidget, reason: 'the back chevron returns to the pad');
      expect(find.byKey(const ValueKey('header-back')), findsNothing, reason: 'the pad is the first layer');
      await t.tap(find.byKey(const ValueKey('stock-button')));
      await settle(t, 6);
      expect(find.byKey(const ValueKey('film-map')), findsOneWidget, reason: 'the stock button only opens the map');
      await t.tap(find.byKey(const ValueKey('header-back')));
      await settle(t, 6);
    });

    testWidgets('pressing magnifies about 2.4x around the finger and names appear; sliding picks the nearest stock live; release keeps it', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      await tapDock(t, EGroup.style);
      await t.tap(find.byKey(const ValueKey('stock-button')));
      await settle(t, 6);
      final map = t.getRect(find.byKey(const ValueKey('film-map')));
      double zoomNow() => t.widget<Transform>(find.byKey(const ValueKey('film-map-zoom'))).transform.getMaxScaleOnAxis();
      expect(zoomNow(), closeTo(1, 1e-6));
      // the dot of N100 (ektar) in map coordinates
      Offset dotPos(double x, double y) => map.topLeft + Offset((0.5 + x * kFilmMapX) * map.width, (0.5 - y * kFilmMapY) * map.height);
      final dots = layoutFilmMap(testFilmCatalog, FilmMapStore.positions!);
      final ektar = dots.firstWhere((d) => d.id == 'kodak_ektar_100');
      final f = await Finger.down(t, map.topLeft + Offset(dotFraction(ektar).dx * map.width, dotFraction(ektar).dy * map.height));
      await settle(t, 5);
      expect(zoomNow(), closeTo(kMagnify, 0.05));
      expect(app.editOf(id).film, 'kodak_ektar_100', reason: 'the press itself picks the stock under the finger');
      expect(find.text('N100'), findsWidgets);
      expect(find.text('N160'), findsWidgets, reason: 'while magnified every stock shows its name');
      await dump(t, 'v3_film_map_magnified');
      // slide: the finger travels 2.4x less in map terms; move towards the velvia dot and the photo follows live
      final velvia = dots.firstWhere((d) => d.id == 'fujifilm_velvia_100');
      final from = dotFraction(ektar), to = dotFraction(velvia);
      final d = Offset((to.dx - from.dx) * map.width, (to.dy - from.dy) * map.height) * kMagnify;
      await f.drag(d, step: 4);
      expect(app.editOf(id).film, 'fujifilm_velvia_100', reason: 'sliding moves to the nearest stock, live');
      await f.up();
      await t.pump(const Duration(milliseconds: 130));
      expect(zoomNow(), lessThan(kMagnify - 0.1), reason: 'the map is shrinking back before it closes');
      await settle(t, 6);
      expect(find.byKey(const ValueKey('film-map')), findsNothing, reason: 'release is final: the map closes by itself');
      expect(find.byKey(const ValueKey('pad-film')), findsOneWidget);
      expect(app.editOf(id).film, 'fujifilm_velvia_100', reason: 'release keeps it');
      app.undo(id);
      expect(app.editOf(id).film, 'kodak_portra_400', reason: 'the whole press was one undo step');
      expect(dotPos(0, 0), isNotNull);
    });

    testWidgets('a pointer cancel keeps the map open; releasing on the selected stock closes it; the button reopens', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      await tapDock(t, EGroup.style);
      await t.tap(find.byKey(const ValueKey('stock-button')));
      await settle(t, 6);
      final map = t.getRect(find.byKey(const ValueKey('film-map')));
      final dots = layoutFilmMap(testFilmCatalog, FilmMapStore.positions!);
      Offset at(String sid) {
        final fr = dotFraction(dots.firstWhere((d) => d.id == sid));
        return map.topLeft + Offset(fr.dx * map.width, fr.dy * map.height);
      }

      final f = await Finger.down(t, at('kodak_ektar_100'));
      await settle(t, 3);
      await t.sendEventToBinding(f.p.cancel());
      await settle(t, 6);
      expect(find.byKey(const ValueKey('film-map')), findsOneWidget, reason: 'a cancelled gesture does not close the map');
      // press and release on the stock that is selected now
      final sel = app.editOf(id).film!;
      final g = await Finger.down(t, at(sel));
      await g.up();
      await settle(t, 6);
      expect(find.byKey(const ValueKey('film-map')), findsNothing);
      expect(app.editOf(id).film, sel);
      await t.tap(find.byKey(const ValueKey('stock-button')));
      await settle(t, 6);
      expect(find.byKey(const ValueKey('film-map')), findsOneWidget, reason: 'the stock button reopens the map');
    });

    testWidgets('the 无 pill clears the film', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400').withValue('film_grain', 40));
      await tapDock(t, EGroup.style);
      await t.tap(find.byKey(const ValueKey('stock-button')));
      await settle(t, 6);
      await t.tap(find.byKey(const ValueKey('film-none')));
      await settle(t, 3);
      expect(app.editOf(id).film, isNull);
      expect(app.editOf(id).values.containsKey('film_grain'), isFalse);
      // nothing chosen: the subtitle says so and there is no 无 (audit F44); back to the pad: the stock button says what it does
      expect(t.widget<Text>(find.byKey(const ValueKey('header-sub'))).data, '选成像');
      expect(find.byKey(const ValueKey('film-none')), findsNothing);
      await t.tap(find.byKey(const ValueKey('header-back')));
      await settle(t, 6);
      expect(find.descendant(of: find.byKey(const ValueKey('stock-button')), matching: find.text('选成像')), findsOneWidget);
      expect(find.byKey(const ValueKey('film-none')), findsNothing);
    });

    test('pickDot: the finger stands for a point 2.4x closer to the press point', () {
      final dots = layoutFilmMap(testFilmCatalog, FilmMapStore.positions ?? const {});
      const size = Size(340, 200);
      final a = dots[0];
      final fa = dotFraction(a);
      expect(pickDot(dots, size, fa, kMagnify, fa)!.id, a.id);
      // moving the finger by 2.4 * (b - a) from the press point lands on b's neighbourhood
      final b = dots[5];
      final fb = dotFraction(b);
      final at = Offset(fa.dx + (fb.dx - fa.dx) * kMagnify, fa.dy + (fb.dy - fa.dy) * kMagnify);
      expect(pickDot(dots, size, fa, kMagnify, at)!.id, b.id);
    });
  });

  group('itemised view', () {
    testWidgets('the radar selects by direction and sets by distance; the outer ring only selects; the ruler shows the selected one', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (_) => EditState.empty);
      await tapDock(t, EGroup.light);
      await t.tap(find.byKey(const ValueKey('items-button')));
      await settle(t, 6);
      final radar = t.getRect(find.byKey(const ValueKey('radar')));
      final items = padItems(PadCat.light);
      final geo = RadarGeo(radar.size, [for (final it in items) it.label]);
      final n = items.length;
      Offset spoke(int i, double t01) => radar.topLeft + geo.point(i, n, t01);
      // press inside the chart on the contrast spoke (index 1) at 90 % of its length: the value follows the distance
      final f = await Finger.down(t, spoke(1, 0.9));
      await f.up();
      await settle(t, 2);
      expect(app.editOf(id).value('contrast'), closeTo(items[1].fromNorm(0.9), 4), reason: 'distance from the centre = value');
      expect(find.text('对比度  ${items[1].fmt(app.editOf(id).value('contrast'))}'), findsOneWidget, reason: 'the ruler follows the selection');
      expect(app.editOf(id).pads['light']!.off['contrast'], isNotNull, reason: 'an item edit is an offset on the pad');
      // the spoke stays locked for the whole gesture: dragging towards another spoke's direction only changes the same item
      final before = app.editOf(id).value('exposure');
      final g = await Finger.down(t, spoke(1, 0.5));
      await g.drag(spoke(3, 0.8) - spoke(1, 0.5));
      await g.up();
      expect(app.editOf(id).value('exposure'), before);
      // the outer ring (where the names are) only selects: no value changes
      final contrast = app.editOf(id).value('contrast');
      final ring = radar.topLeft + geo.label(3, n);
      final h = await Finger.down(t, ring);
      await h.up();
      await settle(t, 2);
      expect(app.editOf(id).value('shadows'), 0);
      expect(app.editOf(id).value('contrast'), contrast);
      expect(find.textContaining('阴影  '), findsOneWidget, reason: 'the ruler now shows the selected shadows');
      await dump(t, 'v3_light_radar');
    });

    testWidgets('parameter names stay as text; 去雾 and 暗角 live in 质地; the film radar has the print filters for a printed stock', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      await tapDock(t, EGroup.effects);
      await t.tap(find.byKey(const ValueKey('items-button')));
      await settle(t, 6);
      for (final k in ['dehaze', 'vignette', 'clarity', 'sharpen', 'grain']) {
        expect(find.byKey(ValueKey('spoke-$k')), findsOneWidget, reason: k);
      }
      expect(find.text('去雾'), findsOneWidget);
      await tapDock(t, EGroup.style);
      await tapDock(t, EGroup.style);
      await tapDock(t, EGroup.style);
      await t.tap(find.byKey(const ValueKey('items-button')));
      await settle(t, 6);
      expect(find.byKey(const ValueKey('spoke-film_warmth')), findsOneWidget);
      expect(find.byKey(const ValueKey('spoke-film_shoot')), findsOneWidget);
    });

    testWidgets('the ruler tape follows the finger: drag right and the tape moves right (the value goes down)', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (_) => EditState.empty);
      await tapDock(t, EGroup.light);
      await t.tap(find.byKey(const ValueKey('items-button')));
      await settle(t, 6);
      final ruler = t.getRect(find.byKey(const ValueKey('tape-ruler')));
      final tape = t.widget<CustomPaint>(find.byKey(const ValueKey('tape'))).painter! as TapePainter;
      final it = padItems(PadCat.light).first; // exposure
      final x0 = tapeOffset(tape.v.value, it.min, it.max);
      final f = await Finger.down(t, ruler.center + const Offset(0, 14));
      await f.drag(const Offset(70, 0));
      final v1 = tape.v.value;
      expect(v1, lessThan(0), reason: 'the tape followed the finger to the right: the value under the pointer went down');
      expect(tapeOffset(v1, it.min, it.max) - x0, closeTo(70, 8), reason: 'the tape moved with the finger, one to one');
      await f.drag(const Offset(-140, 0));
      expect(tape.v.value, greaterThan(0));
      await f.up();
      expect(app.editOf(id).value('exposure'), closeTo(tape.v.value, 1e-6));
      expect(app.editOf(id).pads['light']!.off['exposure'], closeTo(tape.v.value, 1e-6));
      // numeric entry: tap the value
      await t.tap(find.byKey(const ValueKey('ruler-value')));
      await settle(t, 3);
      expect(find.byKey(const ValueKey('ruler-entry')), findsOneWidget);
      await t.enterText(find.byType(TextField), '0.5');
      await t.testTextInput.receiveAction(TextInputAction.done);
      await settle(t, 3);
      expect(app.editOf(id).value('exposure'), closeTo(10, 1e-6), reason: '0.5 EV');
    });

    testWidgets('the complex editors stay reachable: 曲线 from 光线, 分色 and 分级 from 颜色, 相纸 from 胶片; back returns', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      await tapDock(t, EGroup.light);
      await t.tap(find.byKey(const ValueKey('items-button')));
      await settle(t, 6);
      await t.tap(find.byKey(const ValueKey('extra-曲线')));
      await settle(t, 6);
      expect(find.byKey(const ValueKey('curve-pad')), findsOneWidget);
      // absolute: the curve passes through the finger's height at the point's x
      final cp = t.getRect(find.byKey(const ValueKey('curve-pad')));
      final f = await Finger.down(t, Offset(cp.left + cp.width * 0.88, cp.top + cp.height * 0.03));
      await f.up();
      expect(app.editOf(id).value('cv_rgb_H'), greaterThan(40), reason: 'a finger above the diagonal raises the highlights');
      final g = await Finger.down(t, Offset(cp.left + cp.width * 0.88, cp.top + cp.height * 0.8));
      await g.up();
      expect(app.editOf(id).value('cv_rgb_H'), lessThan(-40));
      await t.tap(find.byKey(const ValueKey('header-back'))); // back from the editor to 分项
      await settle(t, 4);
      expect(find.byKey(const ValueKey('curve-pad')), findsNothing);
      expect(find.byKey(const ValueKey('radar')), findsOneWidget);

      await tapDock(t, EGroup.color);
      await t.tap(find.byKey(const ValueKey('items-button')));
      await settle(t, 6);
      await t.tap(find.byKey(const ValueKey('extra-分色')));
      await settle(t, 6);
      expect(find.byKey(const ValueKey('band-3')), findsOneWidget);
      await t.tap(find.byKey(const ValueKey('band-3')));
      await settle(t, 3);
      _noHorizontalScroll(t);
      await t.tap(find.byKey(const ValueKey('header-back')));
      await settle(t, 4);
      await t.tap(find.byKey(const ValueKey('extra-分级')));
      await settle(t, 6);
      final wheel = t.getRect(find.byKey(const ValueKey('grade-wheel')));
      // absolute: the finger's angle is the hue, its distance from the centre the saturation
      final w = await Finger.down(t, wheel.center + Offset(wheel.width / 2 * 0.5, 0));
      await w.up();
      expect(app.editOf(id).value('gr_shadows_h'), closeTo(0, 3));
      expect(app.editOf(id).value('gr_shadows_s'), closeTo(52, 8));
      final w2 = await Finger.down(t, wheel.center + Offset(0, wheel.height / 2 * 0.5));
      await w2.up();
      expect(app.editOf(id).value('gr_shadows_h'), closeTo(90, 3), reason: 'straight down is 90 degrees');
      _noHorizontalScroll(t);

      await tapDock(t, EGroup.style);
      await t.tap(find.byKey(const ValueKey('items-button')));
      await settle(t, 6);
      await t.tap(find.byKey(const ValueKey('extra-相纸')));
      await settle(t, 6);
      expect(find.byKey(const ValueKey('paper-kodak_ektacolor_edge')), findsOneWidget);
      await t.tap(find.byKey(const ValueKey('paper-none')));
      await settle(t, 3);
      expect(app.editOf(id).filmPaper, 'none');
    });

    testWidgets('back: extra editor, itemised view, close the tool, in that order (a fold is not a layer)', (t) async {
      final (app, id) = await openFilm(t);
      await tapDock(t, EGroup.color);
      await t.tap(find.byKey(const ValueKey('items-button')));
      await settle(t, 6);
      await t.tap(find.byKey(const ValueKey('extra-分色')));
      await settle(t, 6);
      Future<void> back() async {
        await t.binding.handlePopRoute();
        await settle(t, 6);
      }

      await back();
      expect(find.byKey(const ValueKey('radar')), findsOneWidget);
      await back();
      expect(find.byKey(const ValueKey('pad-color')), findsOneWidget);
      await back();
      expect(find.byKey(const ValueKey('docked-panel')), findsNothing);
      expect(app.editOf(id), isNotNull);
    });
  });

  group('old edits and persistence', () {
    testWidgets('an old edit opens with the pad at the centre and its values as item offsets; the first move keeps them', (t) async {
      final (app, id) = await openFilm(t);
      final old = app.editOf(id); // the demo photo has exposure 12, contrast 8 and no pad state
      expect(old.pads, isEmpty);
      expect(old.value('exposure'), 12);
      await tapDock(t, EGroup.light);
      final s = padStateOf(old, PadCat.light, ctx);
      expect(s.x, 0);
      expect(s.off['exposure'], 12);
      final f = await Finger.down(t, padAt(t, PadCat.light, 1, 0));
      await f.up();
      expect(app.editOf(id).value('exposure'), closeTo(28, 0.6), reason: 'recipe + the old value as an offset');
      expect(app.editOf(id).value('contrast'), closeTo(8, 0.01));
      // reopening: the pad state is in the persisted record
      final reopened = EditState.fromJson(app.editOf(id).toJson());
      expect(reopened.pads['light']!.x, closeTo(1, 0.01));
      expect(app.editOf(id).toWire(defaultLook: 'std', defaultToneStrength: 60).containsKey('pads'), isFalse);
    });

    testWidgets('the header reset puts the category back to automatic; undo restores', (t) async {
      final (app, id) = await openFilm(t);
      await tapDock(t, EGroup.light);
      expect(find.byKey(ValueKey('reset-${EGroup.light.name}')), findsOneWidget);
      await t.tap(find.byKey(ValueKey('reset-${EGroup.light.name}')));
      await settle(t, 3);
      expect(app.editOf(id).value('exposure'), 0);
      expect(app.editOf(id).pads, isEmpty);
      app.undo(id);
      expect(app.editOf(id).value('exposure'), 12);
    });
  });

  group('crop', () {
    testWidgets('裁切: ratios, rotate, flip and the straighten ruler, no chip strip, no scrolling', (t) async {
      final (app, id) = await openFilm(t);
      await tapDock(t, EGroup.crop);
      expect(find.byKey(const ValueKey('crop-1:1')), findsOneWidget);
      await t.tap(find.byKey(const ValueKey('crop-1:1')));
      await settle(t, 3);
      expect(app.editOf(id).crop, '1:1');
      await t.tap(find.byKey(const ValueKey('crop-rotate')));
      await settle(t, 3);
      expect(app.editOf(id).rot, 90);
      final ruler = t.getRect(find.byKey(const ValueKey('tape-ruler')));
      final f = await Finger.down(t, ruler.center + const Offset(0, 14));
      await f.drag(const Offset(-70, 0));
      await f.up();
      expect(app.editOf(id).value('straighten'), greaterThan(0), reason: 'the tape followed the finger left: the value went up');
      _noHorizontalScroll(t);
    });
  });

  group('placement', () {
    testWidgets('N5 outer screen: the panel sits under the photo, above the dock', (t) async {
      await openFilm(t);
      await tapDock(t, EGroup.light);
      final panel = t.getRect(find.byKey(const ValueKey('docked-panel')));
      final photo = t.getRect(find.byKey(const ValueKey('zoom-stage')));
      expect(panel.top, greaterThan(photo.center.dy));
      expect(panel.width, greaterThan(300));
      expect(panel.bottom, lessThan(t.getTopLeft(find.byKey(ValueKey('dock-${EGroup.light}'))).dy));
      _noHorizontalScroll(t);
    });

    testWidgets('N5 inner screen: a narrow tall side pane with the pad, the strip and a two-line header', (t) async {
      await openFilm(t, phys: n5Phys, dpr: n5Dpr, title: '街角咖啡');
      await tapDock(t, EGroup.style);
      final panel = t.getRect(find.byKey(const ValueKey('docked-panel')));
      final screen = t.view.physicalSize / t.view.devicePixelRatio;
      expect(panel.left, greaterThan(screen.width * 0.5), reason: 'beside the photo');
      expect(panel.width, inInclusiveRange(280, 410));
      final pad = padRect(t, PadCat.film), strip = stripRect(t, PadCat.film);
      expect(pad.height, greaterThan(pad.width * 0.9), reason: 'the pad is tall in the pane');
      expect(strip.height, closeTo(pad.height, 0.5));
      final readout = t.getRect(find.byKey(const ValueKey('header-readout')));
      final title = t.getRect(find.byKey(const ValueKey('header-title')));
      expect(readout.top, greaterThanOrEqualTo(title.bottom - 2), reason: 'the readout has its own line in the pane');
      expect(panel.bottom, lessThan(t.getTopLeft(find.byKey(ValueKey('dock-${EGroup.light}'))).dy));
      _noHorizontalScroll(t);
      await dump(t, 'v3_inner_pane');
    });

    testWidgets('landscape phone: side pane; tablet: side pane', (t) async {
      await openFilm(t, phys: const Size(2400, 1080), dpr: 2.75);
      await tapDock(t, EGroup.light);
      var panel = t.getRect(find.byKey(const ValueKey('docked-panel')));
      expect(panel.left, greaterThan(300));
      _noHorizontalScroll(t);
    });

    testWidgets('tablet', (t) async {
      await openFilm(t, phys: const Size(2560, 1600), dpr: 2);
      await tapDock(t, EGroup.light);
      final panel = t.getRect(find.byKey(const ValueKey('docked-panel')));
      expect(panel.left, greaterThan(640 * 0.5));
      _noHorizontalScroll(t);
    });

    testWidgets('dark mode and large text: no overflow anywhere in the panel', (t) async {
      setView(t, ownerPhys, ownerDpr);
      t.platformDispatcher.textScaleFactorTestValue = 2.0;
      addTearDown(t.platformDispatcher.clearTextScaleFactorTestValue);
      final (app, id) = await openFilm(t, dark: true);
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      for (final g in [EGroup.light, EGroup.color, EGroup.style, EGroup.effects, EGroup.crop]) {
        await tapDock(t, g);
        if (g != EGroup.crop) {
          await t.tap(find.byKey(const ValueKey('items-button')));
          await settle(t, 6);
        }
        expect(t.takeException(), isNull, reason: '$g');
      }
      await dump(t, 'v3_dark_large_text');
    });
  });
}

/// Nothing inside the panel scrolls sideways (no strips, no horizontally scrolling pickers).
void _noHorizontalScroll(WidgetTester t) {
  final panel = find.byKey(const ValueKey('docked-panel'));
  for (final e in find.descendant(of: panel, matching: find.byType(Scrollable)).evaluate()) {
    final s = (e.widget as Scrollable);
    // the header's button row is a one-line safety valve against overflow (PanelHeader), not a scrolling picker
    if (find.ancestor(of: find.byWidget(s), matching: find.byType(PanelHeader)).evaluate().isNotEmpty) continue;
    expect(s.axis, isNot(Axis.horizontal), reason: 'horizontal scrolling in the panel');
  }
  expect(find.descendant(of: panel, matching: find.byType(ListView)), findsNothing);
}

