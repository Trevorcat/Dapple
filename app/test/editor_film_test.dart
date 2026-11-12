// Film simulation in the editor's 风格 window: stock strip, paper row only for negatives, relative-drag
// rows, undo / redo, the licences page, and goldens at phone and Find N5 inner-screen size.
import 'dart:convert';
import 'dart:io';

import 'package:dapple/editor/param_rows.dart' show latitudeFromPercent, latitudePercent;
import 'package:dapple/model/film_pad.dart';
import 'package:dapple/screens/editor/mini_maps.dart' show OffsetPadView;
import 'package:dapple/model/film_catalog.dart';
import 'package:dapple/model/film_names.dart';
import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/screens/editor/float_window.dart' show labelGap;
import 'package:dapple/screens/library/licenses_page.dart';
import 'package:dapple/screens/library/settings_page.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:dapple/widgets/photo_art.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show Finger, dump, n5Dpr, n5Phys, phoneDpr, phonePhys, setView, settle, tapKey;
import 'support/film_fixture.dart';
import 'support/harness.dart';

Future<(AppState, String)> openFilm(WidgetTester t, {Size phys = phonePhys, double dpr = phoneDpr, bool pack = true, String title = '外滩夕阳', Future<String?> Function(Map<String, dynamic>)? fetch}) async {
  setView(t, phys, dpr);
  await loadTestFonts();
  final app = AppState.demo();
  if (pack) app.film = testFilmCatalog;
  final id = app.photos.firstWhere((p) => p.title == title && p.id.startsWith('d')).id;
  await t.pumpWidget(RepaintBoundary(
    key: shotKey,
    child: MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildTheme(Brightness.light),
      darkTheme: buildTheme(Brightness.dark),
      themeMode: ThemeMode.light,
      home: EditorPage(app: app, photoId: id, resamplePointers: false, filmPadFetcher: fetch),
    ),
  ));
  await settle(t);
  return (app, id);
}

/// True on the phone layout (docked panel); the inner screen keeps the floating windows.
bool get docked => find.byKey(const ValueKey('docked-panel')).evaluate().isNotEmpty;

/// Selects a 风格 parameter: a chip in the docked panel, a row in the floating window.
Future<void> selectStyleRow(WidgetTester t, String id) async {
  final f = find.byKey(ValueKey(docked ? 'chip-style-$id' : 'row-style-$id'));
  await t.ensureVisible(f);
  await settle(t, 2);
  await t.tap(f);
  await settle(t, 8);
}

/// Opens the 风格 category and the film row (its stock strip shows).
Future<void> openFilmRow(WidgetTester t) async {
  await tapKey(t, 'dock-${EGroup.style}');
  await settle(t, 8);
  await selectStyleRow(t, 'film');
}

Future<void> pickStock(WidgetTester t, String id) async {
  final k = find.byKey(ValueKey('film-stock-$id'));
  await t.ensureVisible(k);
  await settle(t, 2);
  await t.tap(k);
  await settle(t, 3);
}

bool hasRow(String id) =>
    find.byKey(ValueKey('row-style-$id')).evaluate().isNotEmpty || find.byKey(ValueKey('chip-style-$id')).evaluate().isNotEmpty;

void main() {
  group('catalog', () {
    test('groups the engine list into negative / slide / cinema and finds default papers', () {
      final c = testFilmCatalog;
      expect(c.inGroup(FilmGroup.negative).map((s) => s.id), contains('kodak_portra_400'));
      expect(c.inGroup(FilmGroup.slide).map((s) => s.id), containsAll(['fujifilm_velvia_100', 'kodak_ektachrome_100']));
      expect(c.inGroup(FilmGroup.cinema).map((s) => s.id), everyElement(contains('vision3')));
      expect(c.inGroup(FilmGroup.cinema).length, 4);
      expect(c.papers.length, 8, reason: '6 papers + 2 print films');
      expect(c.stocks.length, 20);
      expect(c.defaultPaperOf(c.stock('kodak_portra_400')!), 'kodak_portra_endura');
      expect(c.defaultPaperOf(c.stock('kodak_vision3_500t')!), 'kodak_2383');
      expect(c.stock('kodak_portra_400')!.printed, isTrue);
      expect(c.stock('fujifilm_velvia_100')!.printed, isFalse);
      // names come from the single table; an id the table does not know shows a neutral label, never the engine's (trademarked) name
      expect(c.stock('kodak_portra_400')!.name, kFilmDisplayNames['kodak_portra_400']);
      expect(filmDisplayName('x_new', 'Engine Name'), kFilmUnnamed);
    });

    test('no pack: the catalog is empty and bad JSON is tolerated', () {
      expect(FilmCatalog.fromJson('{"loaded":false,"stocks":[]}').isEmpty, isTrue);
      expect(FilmCatalog.fromJson('nonsense').isEmpty, isTrue);
      expect(FilmCatalog.fromJson(null).isEmpty, isTrue);
    });

    test('every stock of the engine has a table name; names are unique neutral codes, no trademark', () {
      for (final s in testFilmCatalog.stocks) {
        expect(kFilmDisplayNames.containsKey(s.id), isTrue, reason: s.id);
      }
      final names = kFilmDisplayNames.values.toList();
      expect(names.toSet().length, names.length, reason: 'codes are unique');
      final code = RegExp(r'^(N\d{3}[A-D]?(\+[12])?|C\d{2,3}[DT]|R\d{2,3}[A-C]?|C?P\d)$');
      for (final e in kFilmDisplayNames.entries) {
        expect(code.hasMatch(e.value), isTrue, reason: '${e.key} -> ${e.value}');
      }
      const marks = ['kodak', 'fuji', 'portra', 'ektar', 'ektachrome', 'kodachrome', 'velvia', 'provia', 'gold', 'ultramax', 'vision', 'endura', 'verita', 'xtra', 'x-tra', 'crystal', 'supra', 'ektacolor', 'pro 400'];
      for (final n in [...names, kFilmUnnamed, ...filmGroupNames.values]) {
        for (final m in marks) {
          expect(n.toLowerCase().contains(m), isFalse, reason: '$n contains $m');
        }
      }
      // the engine's own names (trademarks) never reach the UI
      for (final s in testFilmCatalog.stocks) {
        expect(s.name, isNot(s.engineName));
      }
      for (final p in testFilmCatalog.papers) {
        expect(testFilmCatalog.paperName(p.id), isNot(p.engineName));
      }
    });

    test('swatches come from the engine table; without it the neutral fallback; bad entries are dropped', () {
      final sw = parseFilmSwatches('{"kodak_portra_400":["c49770","5997ca","151717"],"bad":["zz","1","2"],"short":["000000"]}');
      expect(sw.keys, ['kodak_portra_400']);
      // engine order: skin, sky, shadow
      expect(sw['kodak_portra_400'], const [Color(0xFFC49770), Color(0xFF5997CA), Color(0xFF151717)]);
      final c = testFilmCatalog.withSwatches(sw);
      // one stock only: nothing to deviate from, the bands are shown as rendered (each twice, banded)
      expect(c.swatch('kodak_portra_400'), [for (final x in sw['kodak_portra_400']!) ...[x, x]]);
      expect(c.swatch('kodak_gold_200'), kFilmSwatchFallback);
      expect(testFilmCatalog.swatch('kodak_portra_400'), kFilmSwatchFallback);
      expect(parseFilmSwatches(null), isEmpty);
      expect(parseFilmSwatches('nonsense'), isEmpty);
      expect(c.stocks.length, testFilmCatalog.stocks.length);
    });

    test('no trademark in the UI sources (licence attribution aside)', () {
      final bad = RegExp(r'Kodak|Fujifilm|Portra(?!it)|Ektar|Velvia|Provia|Kodachrome|Ektachrome|富士|柯达', caseSensitive: false);
      final hits = <String>[];
      for (final f in Directory('lib').listSync(recursive: true).whereType<File>().where((f) => f.path.endsWith('.dart'))) {
        if (f.path.endsWith('film_names.dart') || f.path.endsWith('film_fixture.dart')) continue;
        var n = 0;
        for (final line in f.readAsLinesSync()) {
          n++;
          final t = line.trimLeft();
          if (t.startsWith('//')) continue; // comments are not user-visible
          if (bad.hasMatch(line)) hits.add('${f.path}:$n $t');
        }
      }
      expect(hits, isEmpty);
    });
  });

  group('film pad and the black-point / latitude rulers', () {
    testWidgets('chips 曝光 / 宽容度 / 黑位 replace 胶片曝光; the pad is big, centred at auto, with its mark', (t) async {
      final (app, id) = await openFilm(t, phys: const Size(1140, 2616), dpr: 2.75);
      await openFilmRow(t);
      await pickStock(t, 'kodak_portra_400');
      for (final k in ['film_expo', 'film_texture', 'film_latitude']) {
        expect(hasRow(k), isTrue, reason: k);
      }
      expect(hasRow('film_exp'), isFalse);
      expect(hasRow('film_black'), isFalse, reason: 'the black point lives in the 质感 pad');
      await selectStyleRow(t, 'film_expo');
      final pad = find.byKey(const ValueKey('pad-film_expo'));
      expect(pad, findsOneWidget);
      final r = t.getRect(pad);
      expect(r.height, greaterThanOrEqualTo(1140 / 2.75 * 0.4 - 0.5));
      expect(r.width, greaterThan(300));
      expect(find.byKey(const ValueKey('offset-pad-paint')), findsOneWidget, reason: 'the pad with its auto ring');
      expect(find.descendant(of: find.byKey(const ValueKey('chip-style-film_expo')), matching: find.text('自动')), findsOneWidget, reason: 'the chip reads auto at the centre');
      expect(app.editOf(id).values.containsKey('film_shoot'), isFalse);
      expect(find.ancestor(of: pad, matching: find.byType(Scrollable)), findsNothing);
      await dump(t, 'phone_film_pad');
    });

    testWidgets('曝光 pad: right and up = 过曝 and 浅, values land in the params map in [-1, 1], one undo step, on the wire; double tap resets', (t) async {
      final (app, id) = await openFilm(t, phys: const Size(1140, 2616), dpr: 2.75);
      await openFilmRow(t);
      await pickStock(t, 'kodak_portra_400');
      await selectStyleRow(t, 'film_expo');
      final r = t.getRect(find.byKey(const ValueKey('pad-film_expo')));
      final f = await Finger.down(t, r.center);
      await f.drag(const Offset(60, -50), step: 6);
      await t.pump(const Duration(milliseconds: 80));
      expect(find.descendant(of: find.byKey(const ValueKey('chip-style-film_expo')), matching: find.textContaining('+')), findsOneWidget, reason: 'readout in the chip');
      await f.up();
      await settle(t);
      final e = app.editOf(id);
      expect(e.value('film_shoot'), greaterThan(0));
      expect(e.value('film_print'), greaterThan(0));
      expect(e.value('film_shoot'), lessThanOrEqualTo(1));
      expect(e.value('film_print'), lessThanOrEqualTo(1));
      final w = app.photos.firstWhere((p) => p.id == id);
      expect(app.wireParams(w)['film_shoot'], e.value('film_shoot'));
      expect(app.wireParams(w)['film_print'], e.value('film_print'));
      app.undo(id);
      expect(app.editOf(id).values.containsKey('film_shoot'), isFalse, reason: 'one gesture = one undo step');
      app.redo(id);
      await settle(t, 3);
      final c = t.getCenter(find.byKey(const ValueKey('pad-film_expo')));
      await t.tapAt(c);
      await t.pump(const Duration(milliseconds: 60));
      await t.tapAt(c);
      await settle(t);
      expect(app.editOf(id).values.containsKey('film_shoot'), isFalse);
      expect(app.editOf(id).values.containsKey('film_print'), isFalse);
    });

    testWidgets('质感 pad: right = 本色 (+), up = 压黑 (+); left = 中性 (-), down = 抬黑 (-); on the wire; 无 clears them', (t) async {
      final (app, id) = await openFilm(t, phys: const Size(1140, 2616), dpr: 2.75);
      await openFilmRow(t);
      await pickStock(t, 'kodak_portra_400');
      await selectStyleRow(t, 'film_texture');
      final pad = find.byKey(const ValueKey('pad-film_texture'));
      expect(pad, findsOneWidget);
      expect(hasRow('film_black'), isFalse, reason: 'the black point lives in the pad now');
      final r = t.getRect(pad);
      var f = await Finger.down(t, r.center);
      await f.drag(const Offset(70, -60), step: 6);
      await f.up();
      await settle(t);
      var e = app.editOf(id);
      expect(e.value('film_cast'), greaterThan(0), reason: 'right = 本色');
      expect(e.value('film_black'), greaterThan(0), reason: 'up = 压黑');
      expect(app.wireParams(app.photos.firstWhere((p) => p.id == id))['film_cast'], e.value('film_cast'));
      expect(app.wireParams(app.photos.firstWhere((p) => p.id == id))['film_black'], e.value('film_black'));
      app.applyEdit(id, (e) => e.withValue('film_cast', 0).withValue('film_black', 0));
      await settle(t, 3);
      f = await Finger.down(t, r.center);
      await f.drag(const Offset(-70, 60), step: 6);
      await f.up();
      await settle(t);
      e = app.editOf(id);
      expect(e.value('film_cast'), inInclusiveRange(-1.0, -0.02), reason: 'left = 中性');
      expect(e.value('film_black'), inInclusiveRange(-1.0, -0.02), reason: 'down = 抬黑');
      await selectStyleRow(t, 'film');
      await t.ensureVisible(find.byKey(const ValueKey('film-none')));
      await t.tap(find.byKey(const ValueKey('film-none')));
      await settle(t, 3);
      expect(app.editOf(id).values.keys.where((k) => k.startsWith('film_')), isEmpty, reason: '无 clears the film keys');
    });

    testWidgets('宽容度 is a ruler in percent (55 .. 98, auto 80); a drag stores a normalised value; double tap on the ruler resets', (t) async {
      final (app, id) = await openFilm(t, phys: const Size(1140, 2616), dpr: 2.75);
      await openFilmRow(t);
      await pickStock(t, 'kodak_portra_400');
      await selectStyleRow(t, 'film_latitude');
      expect(find.byKey(const ValueKey('panel-ruler')), findsOneWidget);
      final r = t.getRect(find.byKey(const ValueKey('panel-ruler')));
      final f = await Finger.down(t, r.center);
      await f.drag(const Offset(60, 2));
      await f.up();
      await settle(t);
      final v = app.editOf(id).value('film_latitude');
      expect(v, inInclusiveRange(0.05, 1.0));
      final shown = t.widget<Text>(find.descendant(of: find.byKey(const ValueKey('value-film_latitude')), matching: find.byType(Text))).data!;
      expect(shown, endsWith('%'));
      expect(int.parse(shown.replaceAll('%', '')), inInclusiveRange(81, 98), reason: 'right of auto: 80 % .. 98 %');
      final c = t.getCenter(find.byKey(const ValueKey('panel-ruler')));
      await t.tapAt(c);
      await t.pump(const Duration(milliseconds: 60));
      await t.tapAt(c);
      await settle(t);
      expect(app.editOf(id).values.containsKey('film_latitude'), isFalse);
    });

    testWidgets('the panel never covers the photo with a film pad, and an old film_exp edit still reaches the wire', (t) async {
      final (app, id) = await openFilm(t, title: '街角咖啡');
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400').withValue('film_exp', 10));
      await openFilmRow(t);
      await selectStyleRow(t, 'film_expo');
      await settle(t, 20);
      final panel = t.getRect(find.byKey(const ValueKey('docked-panel')));
      expect(t.getRect(find.byType(PhotoArt)).bottom, lessThanOrEqualTo(panel.top + 0.5));
      expect(app.wireParams(app.photos.firstWhere((p) => p.id == id))['film_exp'], 10);
    });

    testWidgets('inner screen: the 曝光 pad lives in the floating window and claims its drag', (t) async {
      final (app, id) = await openFilm(t, phys: n5Phys, dpr: n5Dpr);
      await openFilmRow(t);
      await pickStock(t, 'kodak_portra_400');
      await selectStyleRow(t, 'film_expo');
      final pad = find.byKey(const ValueKey('pad-film_expo'));
      await t.ensureVisible(pad);
      await settle(t, 5);
      final r = t.getRect(pad);
      final f = await Finger.down(t, r.center);
      await f.drag(const Offset(30, -30), step: 5);
      await f.up();
      await settle(t, 3);
      expect(app.editOf(id).value('film_shoot'), greaterThan(0));
    });
  });

  group('effective ranges of the film pads', () {
    String json(Map<String, dynamic> m) => jsonEncode({'ok': true, ...m});

    test('the parser is tolerant: missing fields, wrong types, garbage and an old engine all mean effective', () {
      for (final s in [null, '', 'nonsense', '[]', '{}', '{"ok":false}', '{"ok":true}', '{"effective_lo":3,"effective_hi":"no"}']) {
        final r = FilmPadRanges.parse(s);
        for (final k in filmPadKeys) {
          expect(r.lo(k) && r.hi(k), isTrue, reason: '$s $k');
        }
        expect(r, FilmPadRanges.all);
      }
      final r = FilmPadRanges.parse(json({
        'effective_lo': {'film_cast': false},
        'effective_hi': {'film_print': false, 'film_cast': false, 'film_shoot': 'x'},
      }));
      expect(r.hi('film_print'), isFalse);
      expect(r.lo('film_print'), isTrue, reason: 'a missing field is effective');
      expect(r.dead('film_cast'), isTrue);
      expect(r.dead('film_print'), isFalse);
      expect(r.hi('film_shoot'), isTrue, reason: 'a non-bool is ignored');
      // the per-key object form
      final p = FilmPadRanges.parse(json({'film_black': {'effective_lo': false, 'effective_hi': false}}));
      expect(p.dead('film_black'), isTrue);
      expect(p.dead('film_shoot'), isFalse);
    });

    testWidgets('a dead half is not reachable: the cursor stops at the centre line and the half is drawn hatched', (t) async {
      final calls = <Map<String, dynamic>>[];
      final (app, id) = await openFilm(t, phys: const Size(1140, 2616), dpr: 2.75, fetch: (p) async {
        calls.add(p);
        return json({'effective_hi': {'film_print': false}}); // film_print +1 (up) does nothing
      });
      await openFilmRow(t);
      await pickStock(t, 'kodak_portra_400');
      await settle(t, 8);
      expect(calls, isNotEmpty);
      expect(calls.last['film'], 'kodak_portra_400');
      await selectStyleRow(t, 'film_expo');
      expect(t.widget<OffsetPadView>(find.byType(OffsetPadView)).eff, [true, true, true, false]);
      final r = t.getRect(find.byKey(const ValueKey('pad-film_expo')));
      var f = await Finger.down(t, r.center);
      await f.drag(const Offset(60, -120), step: 6); // right and far up: into the dead half
      await f.up();
      await settle(t);
      expect(app.editOf(id).value('film_shoot'), greaterThan(0), reason: 'the live axis moves');
      expect(app.editOf(id).value('film_print'), 0, reason: 'the dead half cannot be entered');
      app.applyEdit(id, (e) => e.withValue('film_shoot', 0));
      await settle(t, 3);
      f = await Finger.down(t, r.center);
      await f.drag(const Offset(0, 90), step: 6); // down: the live half of film_print
      await f.up();
      await settle(t);
      expect(app.editOf(id).value('film_print'), lessThan(0));
    });

    testWidgets('a dead axis turns the pad into a ruler for the live axis; no dead end on that ruler', (t) async {
      final (app, id) = await openFilm(t, phys: const Size(1140, 2616), dpr: 2.75, fetch: (p) async {
        return json({'effective_lo': {'film_cast': false}, 'effective_hi': {'film_cast': false, 'film_black': false}});
      });
      await openFilmRow(t);
      await pickStock(t, 'kodak_portra_400');
      await settle(t, 8);
      expect(hasRow('film_texture'), isTrue, reason: 'film_black still has its lower half');
      await selectStyleRow(t, 'film_texture');
      expect(find.byKey(const ValueKey('pad-film_texture')), findsNothing);
      expect(find.byKey(const ValueKey('panel-ruler')), findsOneWidget, reason: 'a single-axis ruler for film_black');
      final r = t.getRect(find.byKey(const ValueKey('panel-ruler')));
      var f = await Finger.down(t, r.center);
      await f.drag(const Offset(120, 2));
      await f.up();
      await settle(t);
      expect(app.editOf(id).value('film_black'), 0, reason: '+ (压黑) is dead: the ruler cannot go there');
      f = await Finger.down(t, r.center);
      await f.drag(const Offset(-80, 2));
      await f.up();
      await settle(t);
      expect(app.editOf(id).value('film_black'), lessThan(0));
      expect(app.editOf(id).values.containsKey('film_cast'), isFalse);
    });

    testWidgets('both axes dead hides the chip; a slide (R100B) keeps a usable pad where only half is dead', (t) async {
      final (app, id) = await openFilm(t, phys: const Size(1140, 2616), dpr: 2.75, fetch: (p) async {
        return json({
          'effective_lo': {'film_shoot': false, 'film_print': false},
          'effective_hi': {'film_shoot': false, 'film_print': false, 'film_cast': false},
        });
      });
      await openFilmRow(t);
      await pickStock(t, 'fujifilm_velvia_100');
      await settle(t, 8);
      expect(app.editOf(id).film, 'fujifilm_velvia_100');
      expect(hasRow('film_expo'), isFalse, reason: 'film_shoot and film_print both dead: no chip');
      expect(hasRow('film_texture'), isTrue, reason: 'film_black is alive, film_cast only half dead');
      expect(hasRow('film_latitude'), isTrue);
    });

    testWidgets('the ranges refresh when the stock changes and never during a drag', (t) async {
      var n = 0;
      final (app, id) = await openFilm(t, phys: const Size(1140, 2616), dpr: 2.75, fetch: (p) async {
        n++;
        return json({});
      });
      await openFilmRow(t);
      await pickStock(t, 'kodak_portra_400');
      await settle(t, 8);
      final afterStock = n;
      expect(afterStock, greaterThanOrEqualTo(1));
      await selectStyleRow(t, 'film_expo');
      final r = t.getRect(find.byKey(const ValueKey('pad-film_expo')));
      final f = await Finger.down(t, r.center);
      await f.drag(const Offset(60, 0), step: 6);
      await t.pump(const Duration(milliseconds: 900)); // longer than the debounce, finger still down
      expect(n, afterStock, reason: 'no query while a finger is down');
      await f.up();
      await settle(t, 10);
      expect(n, afterStock + 1, reason: 'film_shoot moved: one refresh after the drag');
      expect(app.editOf(id).value('film_shoot'), greaterThan(0));
    });
  });

  group('review renders (written to ../build/emu/panel_4x_*.png)', () {
    Future<void> shot(WidgetTester t, String name) async {
      await dump(t, name);
      File('test/goldens/editor_v2/$name.png').copySync('../build/emu/$name.png');
    }

    testWidgets('a negative: the 质感 pad with a drag; a slide: the 曝光 pad with its dead half and 质感 as a ruler', (t) async {
      final (app, id) = await openFilm(t, title: '街角咖啡', phys: const Size(1140, 2616), dpr: 2.75);
      await openFilmRow(t);
      await pickStock(t, 'kodak_portra_400');
      await selectStyleRow(t, 'film_texture');
      final r = t.getRect(find.byKey(const ValueKey('pad-film_texture')));
      final f = await Finger.down(t, r.center);
      await f.drag(const Offset(-60, 50), step: 6);
      await f.up();
      await settle(t, 3);
      await shot(t, 'panel_4x_negative_texture');
      expect(app.editOf(id).value('film_cast'), lessThan(0));
    });

    testWidgets('a slide (R100B), the engine reports film_print +1 and film_cast as ineffective', (t) async {
      final (app, id) = await openFilm(t, title: '街角咖啡', phys: const Size(1140, 2616), dpr: 2.75, fetch: (p) async {
        return jsonEncode({
          'ok': true,
          'effective_hi': {'film_print': false, 'film_cast': false},
          'effective_lo': {'film_cast': false},
        });
      });
      await openFilmRow(t);
      await pickStock(t, 'fujifilm_velvia_100');
      await settle(t, 8);
      await selectStyleRow(t, 'film_expo');
      final r = t.getRect(find.byKey(const ValueKey('pad-film_expo')));
      final f = await Finger.down(t, r.center);
      await f.drag(const Offset(60, -150), step: 6);
      await f.up();
      await settle(t, 3);
      await shot(t, 'panel_4x_slide_expo');
      expect(app.editOf(id).value('film_print'), 0);
      await selectStyleRow(t, 'film_texture');
      await settle(t, 6);
      await shot(t, 'panel_4x_slide_texture_ruler');
    });
  });

  group('edit state', () {
    test('latitude scale: -1 / 0 / +1 = 55 / 80 / 98 %, piecewise linear, and back', () {
      expect(latitudePercent(-1), 55);
      expect(latitudePercent(0), 80);
      expect(latitudePercent(1), 98);
      expect(latitudePercent(0.5), 89);
      expect(latitudeFromPercent(55), -1);
      expect(latitudeFromPercent(80), 0);
      expect(latitudeFromPercent(98), 1);
      for (final v in [-0.7, -0.2, 0.3, 0.9]) {
        expect(latitudeFromPercent(latitudePercent(v)), closeTo(v, 1e-9));
      }
    });

    test('film keys reach the wire only with a stock; defaults stay absent; sync and clipboard carry them', () {
      var e = EditState.empty;
      expect(e.toWire(defaultLook: 'std', defaultToneStrength: 60).keys.any((k) => k.startsWith('film')), isFalse);
      e = e.copyWith(film: 'kodak_portra_400').withValue('film_grain', 60).withValue('film_exp', 10).withValue('film_strength', 100);
      final w = e.toWire(defaultLook: 'std', defaultToneStrength: 60);
      expect(w['film'], 'kodak_portra_400');
      expect(w['film_grain'], 60);
      expect(w['film_exp'], 10);
      expect(w.containsKey('film_strength'), isFalse, reason: 'the default value is absent');
      expect(w.containsKey('film_paper'), isFalse, reason: 'default paper = engine default');
      e = e.copyWith(filmPaper: 'kodak_supra_endura');
      expect(e.toWire(defaultLook: 'std', defaultToneStrength: 60)['film_paper'], 'kodak_supra_endura');
      // values without a stock are not sent
      final orphan = EditState.empty.withValue('film_grain', 10);
      expect(orphan.toWire(defaultLook: 'std', defaultToneStrength: 60).containsKey('film_grain'), isFalse);
      // json round trip
      final back = EditState.fromJson(e.toJson());
      expect(back.film, 'kodak_portra_400');
      expect(back.filmPaper, 'kodak_supra_endura');
      expect(back.value('film_grain'), 60);
      expect(back.sameAs(e), isTrue);
      // paste / sync
      expect(EditState.empty.pasted(e).film, 'kodak_portra_400');
      expect(EditState.empty.merged(e, {SyncCat.style}).value('film_grain'), 60);
      expect(EditState.empty.merged(e, {SyncCat.light}).film, isNull);
      expect(e.groupAdjusted(EGroup.style), isTrue);
    });
  });

  group('phone', () {
    testWidgets('no pack: the 风格 window has no film rows', (t) async {
      await openFilm(t, pack: false);
      await tapKey(t, 'dock-${EGroup.style}');
      expect(hasRow('look'), isTrue);
      expect(hasRow('film'), isFalse);
    });

    testWidgets('selecting a stock: strip grouped, swatches not photos, paper row only for negatives', (t) async {
      final (app, id) = await openFilm(t);
      await openFilmRow(t);
      expect(find.byKey(const ValueKey('film-strip')), findsOneWidget);
      for (final g in ['负片', '反转片', '电影']) {
        expect(find.text(g), findsWidgets, reason: g);
      }
      expect(app.editOf(id).film, isNull);
      // before a stock is chosen only the stock row exists
      for (final k in ['filmPaper', 'film_strength', 'film_expo', 'film_texture', 'film_latitude', 'film_grain', 'film_halation', 'film_warmth', 'film_tint']) {
        expect(hasRow(k), isFalse, reason: k);
      }

      // a negative: default paper automatically, paper row and print filtration appear
      await pickStock(t, 'kodak_portra_400');
      expect(app.editOf(id).film, 'kodak_portra_400');
      expect(app.editOf(id).filmPaper, isNull, reason: 'the default paper is the engine default, not an override');
      expect(app.wireParams(app.photos.firstWhere((p) => p.id == id))['film'], 'kodak_portra_400');
      for (final k in ['filmPaper', 'film_strength', 'film_expo', 'film_texture', 'film_latitude', 'film_grain', 'film_halation', 'film_warmth', 'film_tint']) {
        expect(hasRow(k), isTrue, reason: k);
      }
      expect(find.text(kFilmDisplayNames['kodak_portra_endura']!), findsWidgets, reason: 'the row / chip shows the default paper');

      // choose another paper from the paper row
      await selectStyleRow(t, 'filmPaper');
      expect(find.byKey(const ValueKey('extra-filmPaper')), findsOneWidget);
      final papers = testFilmCatalog.paperIds;
      final strip = find.descendant(of: find.byKey(const ValueKey('film-paper-strip')), matching: find.byType(Scrollable));
      final supra = find.byKey(ValueKey('film-paper-${papers.indexOf('kodak_supra_endura')}'));
      await t.scrollUntilVisible(supra, 120, scrollable: strip);
      await t.ensureVisible(supra);
      await settle(t, 2);
      await t.tap(supra);
      await settle(t, 3);
      expect(app.editOf(id).filmPaper, 'kodak_supra_endura');
      expect(app.wireParams(app.photos.firstWhere((p) => p.id == id))['film_paper'], 'kodak_supra_endura');
      // scanning the film directly: no enlarger, so no print filtration rows
      final none = find.byKey(ValueKey('film-paper-${papers.indexOf(kPaperNone)}'));
      await t.scrollUntilVisible(none, -120, scrollable: strip);
      await t.ensureVisible(none);
      await settle(t, 2);
      await t.tap(none);
      await settle(t, 3);
      expect(app.editOf(id).filmPaper, kPaperNone);
      expect(hasRow('film_warmth'), isFalse);
      expect(hasRow('film_tint'), isFalse);
      expect(hasRow('film_grain'), isTrue);

      // a slide: no paper row, no print filtration, paper override dropped
      await selectStyleRow(t, 'film');
      await pickStock(t, 'fujifilm_velvia_100');
      expect(app.editOf(id).film, 'fujifilm_velvia_100');
      expect(app.editOf(id).filmPaper, isNull);
      expect(hasRow('filmPaper'), isFalse);
      expect(hasRow('film_warmth'), isFalse);
      expect(hasRow('film_tint'), isFalse);
      expect(hasRow('film_grain'), isTrue);
      expect(hasRow('film_expo'), isTrue);
      expect(hasRow('film_exp'), isFalse, reason: 'the old exposure slider left the UI');

      // a cinema negative gets the print film by default
      await pickStock(t, 'kodak_vision3_500t');
      expect(hasRow('filmPaper'), isTrue);
      expect(find.text(kFilmDisplayNames['kodak_2383']!), findsWidgets);

      // 无 clears the film and its knobs
      app.applyEdit(id, (e) => e.withValue('film_grain', 40));
      await settle(t, 2);
      await t.ensureVisible(find.byKey(const ValueKey('film-none')));
      await t.tap(find.byKey(const ValueKey('film-none')));
      await settle(t, 3);
      expect(app.editOf(id).film, isNull);
      expect(app.editOf(id).values.keys.where((k) => k.startsWith('film_')), isEmpty);
      expect(hasRow('film_grain'), isFalse);
    });

    testWidgets('a drag changes film_grain; it is one undo step; undo / redo restore the stock', (t) async {
      final (app, id) = await openFilm(t);
      await openFilmRow(t);
      await pickStock(t, 'kodak_portra_400');
      expect(app.editOf(id).value('film_grain'), 100);

      await selectStyleRow(t, 'film_grain');
      final f = await Finger.down(t, const Offset(200, 250));
      await f.drag(const Offset(-120, 3));
      await t.pump(const Duration(milliseconds: 250));
      expect(find.byKey(ValueKey(docked ? 'panel-ruler' : 'readout')), findsOneWidget, reason: 'the panel stays; the window shows its readout');
      await f.up();
      await settle(t);
      final g = app.editOf(id).value('film_grain');
      expect(g, lessThan(100));
      expect(g, greaterThan(0));
      expect(app.editOf(id).film, 'kodak_portra_400', reason: 'dragging a knob keeps the stock');
      expect(app.wireParams(app.photos.firstWhere((p) => p.id == id))['film_grain'], g);

      // undo: only the drag goes; redo brings it back
      await tapKey(t, 'toolbar-undo');
      expect(app.editOf(id).value('film_grain'), 100);
      expect(app.editOf(id).film, 'kodak_portra_400');
      await tapKey(t, 'toolbar-redo');
      expect(app.editOf(id).value('film_grain'), g);
      // undo twice more: the stock choice is its own step
      await tapKey(t, 'toolbar-undo');
      await tapKey(t, 'toolbar-undo');
      expect(app.editOf(id).film, isNull);
      await tapKey(t, 'toolbar-redo');
      expect(app.editOf(id).film, 'kodak_portra_400');
    });

    for (final big in [false, true]) {
      testWidgets('row label and value never touch (paper row, ${big ? 'inner screen' : 'phone window'}, large text)', (t) async {
        final (app, id) = await (big ? openFilm(t, phys: n5Phys, dpr: n5Dpr) : openFilm(t));
        t.platformDispatcher.textScaleFactorTestValue = 1.6;
        addTearDown(t.platformDispatcher.clearTextScaleFactorTestValue);
        await openFilmRow(t);
        await pickStock(t, 'kodak_portra_400');
        expect(app.editOf(id).film, 'kodak_portra_400');
        for (final k in ['filmPaper', 'film', 'film_expo', 'film_strength']) {
          if (docked) {
            // phone: a chip holds the name over the value; both stay inside the chip at large text
            final chip = find.byKey(ValueKey('chip-style-$k'));
            await t.ensureVisible(chip);
            await settle(t, 2);
            final texts = find.descendant(of: chip, matching: find.byType(Text));
            expect(texts, findsNWidgets(2), reason: k);
            final cr = t.getRect(chip);
            for (var i = 0; i < 2; i++) {
              final r = t.getRect(texts.at(i));
              expect(r.left, greaterThanOrEqualTo(cr.left - 0.5), reason: k);
              expect(r.right, lessThanOrEqualTo(cr.right + 0.5), reason: k);
              expect(r.bottom, lessThanOrEqualTo(cr.bottom + 0.5), reason: '$k: inside the chip');
            }
            expect(t.takeException(), isNull, reason: '$k: no overflow');
            continue;
          }
          final row = find.byKey(ValueKey('row-style-$k'));
          await t.ensureVisible(row);
          await settle(t, 2);
          final texts = find.descendant(of: row, matching: find.byType(Text));
          expect(texts, findsNWidgets(2), reason: k);
          final label = t.getRect(texts.at(0)), value = t.getRect(texts.at(1));
          final rowRect = t.getRect(row);
          expect(value.left - label.right, greaterThanOrEqualTo(labelGap - 0.5), reason: '$k: gap between label and value');
          expect(value.right, lessThanOrEqualTo(rowRect.right), reason: k);
          expect(t.takeException(), isNull, reason: '$k: no overflow');
        }
      });
    }

    testWidgets('a tap on a row selects it; dragging on the row itself changes nothing; the photo is the adjustment surface (guidelines 15.1)', (t) async {
      final (app, id) = await openFilm(t);
      await openFilmRow(t);
      await pickStock(t, 'kodak_portra_400');
      final row = find.byKey(ValueKey(docked ? 'chip-style-film_halation' : 'row-style-film_halation'));
      await t.ensureVisible(row);
      await settle(t, 2);
      await t.tap(row);
      await settle(t, 8);
      final before = app.editOf(id).value('film_halation');
      // a horizontal drag that starts on the row: the row is not a slider, the value stays
      final f = await Finger.down(t, t.getCenter(row));
      await f.drag(const Offset(-60, 0));
      await f.up();
      await settle(t, 3);
      expect(app.editOf(id).value('film_halation'), before);
      // the same drag on the photo, outside the window, changes the active row
      final g = await Finger.down(t, const Offset(60, 200));
      await g.drag(const Offset(-50, 2));
      await t.pump(const Duration(milliseconds: 250));
      expect(find.byKey(ValueKey(docked ? 'panel-ruler' : 'readout')), findsOneWidget, reason: 'the readout / the panel names the row that is being changed');
      await g.up();
      await settle(t, 3);
      expect(app.editOf(id).value('film_halation'), lessThan(before));
    });

    testWidgets('scrolling the 风格 window after a drag keeps its place and the active row visible (accept2_08)', (t) async {
      final (app, id) = await openFilm(t, phys: n5Phys, dpr: n5Dpr);
      await openFilmRow(t);
      await pickStock(t, 'kodak_portra_400');
      final sc = find.descendant(of: find.byKey(const ValueKey('window-scroll')), matching: find.byType(Scrollable));
      double off() => t.state<ScrollableState>(sc.first).position.pixels;
      await selectStyleRow(t, 'film_grain');
      await settle(t, 3);
      final before = off();
      expect(before, greaterThan(0), reason: 'the window is scrolled down to the grain row');
      // drag on the photo (the adjustment surface)
      final f = await Finger.down(t, const Offset(60, 200));
      await f.drag(const Offset(-50, 2));
      await t.pump(const Duration(milliseconds: 250));
      await f.up();
      await settle(t, 3);
      expect(app.editOf(id).value('film_grain'), lessThan(100));
      expect(off(), closeTo(before, 1.0), reason: 'a drag does not move the window content');
      expect(t.getRect(find.byKey(const ValueKey('window-scroll'))).contains(t.getCenter(find.byKey(const ValueKey('row-style-film_grain')))),
          isTrue, reason: 'the active row stays in view');
      // the user scrolls the window up by 120 dp: it follows the finger and stays there (also after the 360 ms settle)
      final rect = t.getRect(find.byKey(const ValueKey('window-scroll')));
      final g = await Finger.down(t, rect.center);
      await g.drag(const Offset(0, 120));
      await g.up();
      await settle(t, 3);
      await t.pump(const Duration(milliseconds: 600));
      expect(off(), closeTo(before - 120, 40), reason: 'scrolled by the user, not reset to the top');
      expect(off(), greaterThan(0));
    });

    testWidgets('drag rows: exposure in EV, warmth signed, strength has a magnetic default at 100', (t) async {
      final (app, id) = await openFilm(t);
      await openFilmRow(t);
      await pickStock(t, 'kodak_portra_400');
      await selectStyleRow(t, 'film_latitude');
      var f = await Finger.down(t, const Offset(60, 300));
      await f.drag(const Offset(100, 2));
      await f.up();
      await settle(t);
      final lat = app.editOf(id).value('film_latitude');
      expect(lat, greaterThan(0));
      expect(lat, lessThanOrEqualTo(1), reason: 'stored normalised');

      await selectStyleRow(t, 'film_warmth');
      f = await Finger.down(t, const Offset(60, 300));
      await f.drag(const Offset(-90, 2));
      await f.up();
      await settle(t);
      expect(app.editOf(id).value('film_warmth'), lessThan(0));

      // strength: starts at the default (100) and the row is not marked modified
      await selectStyleRow(t, 'film_strength');
      expect(app.editOf(id).value('film_strength'), 100);
      f = await Finger.down(t, const Offset(200, 300));
      await f.drag(const Offset(-80, 2));
      await f.up();
      await settle(t);
      expect(app.editOf(id).value('film_strength'), lessThan(100));
      // double tap on the row resets it to the default
      final strengthRow = find.byKey(ValueKey(docked ? 'chip-style-film_strength' : 'row-style-film_strength'));
      await t.ensureVisible(strengthRow);
      await t.tap(strengthRow);
      await t.pump(const Duration(milliseconds: 50));
      await t.tap(strengthRow);
      await settle(t, 3);
      expect(app.editOf(id).value('film_strength'), 100);
    });

    testWidgets('golden: phone with a stock chosen', (t) async {
      final (app, id) = await openFilm(t);
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400').withValue('film_grain', 70));
      await openFilmRow(t);
      expect(find.byKey(const ValueKey('film-stock-kodak_portra_400')), findsOneWidget);
      await dump(t, 'phone_film');
      expect(File('test/goldens/editor_v2/phone_film.png').existsSync(), isTrue);
    });
  });

  group('licences', () {
    testWidgets('settings 关于 > 开源许可 shows the attribution from the engine and the notices', (t) async {
      setView(t, phonePhys, phoneDpr);
      await loadTestFonts();
      final app = AppState.demo();
      app.film = testFilmCatalog;
      await t.pumpWidget(MaterialApp(
        theme: buildTheme(Brightness.light),
        home: Scaffold(body: SettingsPage(app: app, wide: false)),
      ));
      await settle(t, 2);
      await t.scrollUntilVisible(find.byKey(const ValueKey('licenses')), 300, scrollable: find.byType(Scrollable).first);
      await t.tap(find.byKey(const ValueKey('licenses')));
      await settle(t, 5);
      expect(find.byType(LicensesPage), findsOneWidget);
      final text = t.widget<SelectableText>(find.byKey(const ValueKey('licence-attribution'))).data!;
      expect(text, contains('spektrafilm'));
      expect(text, contains('Andrea Volpato'));
      expect(text, contains('CC BY-SA 4.0'));
      expect(text, app.film.attribution);
      expect(find.textContaining('creativecommons.org/licenses/by-sa/4.0'), findsOneWidget);
    });

    testWidgets('without a pack the page still carries the credit line', (t) async {
      setView(t, phonePhys, phoneDpr);
      await loadTestFonts();
      final app = AppState.demo();
      await t.pumpWidget(MaterialApp(theme: buildTheme(Brightness.light), home: LicensesPage(app: app)));
      await settle(t, 3);
      final text = t.widget<SelectableText>(find.byKey(const ValueKey('licence-attribution'))).data!;
      expect(text, LicensesPage.fallbackAttribution);
    });
  });

  group('Find N5 inner', () {
    testWidgets('golden: inner screen with a stock chosen', (t) async {
      final (app, id) = await openFilm(t, phys: n5Phys, dpr: n5Dpr);
      app.applyEdit(id, (e) => e.copyWith(film: 'fujifilm_velvia_100'));
      await openFilmRow(t);
      await dump(t, 'n5_film');
      expect(find.byKey(const ValueKey('film-stock-fujifilm_velvia_100')), findsOneWidget);
    });
  });
}
