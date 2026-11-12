// Film simulation in the editor's 风格 window: stock strip, paper row only for negatives, relative-drag
// rows, undo / redo, the licences page, and goldens at phone and Find N5 inner-screen size.
import 'dart:io';

import 'package:dapple/model/film_catalog.dart';
import 'package:dapple/model/film_names.dart';
import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/screens/editor/float_window.dart' show labelGap;
import 'package:dapple/screens/library/licenses_page.dart';
import 'package:dapple/screens/library/settings_page.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show Finger, dump, n5Dpr, n5Phys, phoneDpr, phonePhys, setView, settle, tapKey;
import 'support/film_fixture.dart';
import 'support/harness.dart';

Future<(AppState, String)> openFilm(WidgetTester t, {Size phys = phonePhys, double dpr = phoneDpr, bool pack = true}) async {
  setView(t, phys, dpr);
  await loadTestFonts();
  final app = AppState.demo();
  if (pack) app.film = testFilmCatalog;
  final id = app.photos.firstWhere((p) => p.title == '外滩夕阳' && p.id.startsWith('d')).id;
  await t.pumpWidget(RepaintBoundary(
    key: shotKey,
    child: MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildTheme(Brightness.light),
      darkTheme: buildTheme(Brightness.dark),
      themeMode: ThemeMode.light,
      home: EditorPage(app: app, photoId: id, resamplePointers: false),
    ),
  ));
  await settle(t);
  return (app, id);
}

/// Opens the 风格 window and the film row (its stock strip unfolds).
Future<void> openFilmRow(WidgetTester t) async {
  await tapKey(t, 'dock-${EGroup.style}');
  await t.ensureVisible(find.byKey(const ValueKey('row-style-film')));
  await tapKey(t, 'row-style-film');
}

Future<void> pickStock(WidgetTester t, String id) async {
  final k = find.byKey(ValueKey('film-stock-$id'));
  await t.ensureVisible(k);
  await settle(t, 2);
  await t.tap(k);
  await settle(t, 3);
}

bool hasRow(String id) => find.byKey(ValueKey('row-style-$id')).evaluate().isNotEmpty;

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

  group('edit state', () {
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
      expect(find.byKey(const ValueKey('extra-film')), findsOneWidget);
      expect(find.byKey(const ValueKey('film-strip')), findsOneWidget);
      for (final g in ['负片', '反转片', '电影']) {
        expect(find.text(g), findsWidgets, reason: g);
      }
      expect(app.editOf(id).film, isNull);
      // before a stock is chosen only the stock row exists
      for (final k in ['filmPaper', 'film_strength', 'film_grain', 'film_halation', 'film_warmth', 'film_tint', 'film_exp']) {
        expect(hasRow(k), isFalse, reason: k);
      }

      // a negative: default paper automatically, paper row and print filtration appear
      await pickStock(t, 'kodak_portra_400');
      expect(app.editOf(id).film, 'kodak_portra_400');
      expect(app.editOf(id).filmPaper, isNull, reason: 'the default paper is the engine default, not an override');
      expect(app.wireParams(app.photos.firstWhere((p) => p.id == id))['film'], 'kodak_portra_400');
      for (final k in ['filmPaper', 'film_strength', 'film_grain', 'film_halation', 'film_warmth', 'film_tint', 'film_exp']) {
        expect(hasRow(k), isTrue, reason: k);
      }
      expect(find.text(kFilmDisplayNames['kodak_portra_endura']!), findsWidgets, reason: 'the row shows the default paper');

      // choose another paper from the paper row
      await t.ensureVisible(find.byKey(const ValueKey('row-style-filmPaper')));
      await tapKey(t, 'row-style-filmPaper');
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
      await t.ensureVisible(find.byKey(const ValueKey('row-style-film')));
      await tapKey(t, 'row-style-film');
      await pickStock(t, 'fujifilm_velvia_100');
      expect(app.editOf(id).film, 'fujifilm_velvia_100');
      expect(app.editOf(id).filmPaper, isNull);
      expect(hasRow('filmPaper'), isFalse);
      expect(hasRow('film_warmth'), isFalse);
      expect(hasRow('film_tint'), isFalse);
      expect(hasRow('film_grain'), isTrue);
      expect(hasRow('film_exp'), isTrue);

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

      await t.ensureVisible(find.byKey(const ValueKey('row-style-film_grain')));
      await tapKey(t, 'row-style-film_grain');
      final f = await Finger.down(t, const Offset(200, 300));
      await f.drag(const Offset(-120, 3));
      await t.pump(const Duration(milliseconds: 250));
      expect(find.byKey(const ValueKey('readout')), findsOneWidget);
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
        for (final k in ['filmPaper', 'film', 'film_exp', 'film_strength']) {
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
      final row = find.byKey(const ValueKey('row-style-film_halation'));
      await t.ensureVisible(row);
      await settle(t, 2);
      await t.tap(row);
      await settle(t, 3);
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
      expect(find.byKey(const ValueKey('readout')), findsOneWidget, reason: 'the readout names the row that is being changed');
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
      await t.ensureVisible(find.byKey(const ValueKey('row-style-film_grain')));
      await tapKey(t, 'row-style-film_grain');
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
      await t.ensureVisible(find.byKey(const ValueKey('row-style-film_exp')));
      await tapKey(t, 'row-style-film_exp');
      var f = await Finger.down(t, const Offset(60, 300));
      await f.drag(const Offset(100, 2));
      await f.up();
      await settle(t);
      final ev = app.editOf(id).value('film_exp');
      expect(ev, greaterThan(0));
      expect(ev, lessThanOrEqualTo(60));

      await t.ensureVisible(find.byKey(const ValueKey('row-style-film_warmth')));
      await tapKey(t, 'row-style-film_warmth');
      f = await Finger.down(t, const Offset(60, 300));
      await f.drag(const Offset(-90, 2));
      await f.up();
      await settle(t);
      expect(app.editOf(id).value('film_warmth'), lessThan(0));

      // strength: starts at the default (100) and the row is not marked modified
      await t.ensureVisible(find.byKey(const ValueKey('row-style-film_strength')));
      await tapKey(t, 'row-style-film_strength');
      expect(app.editOf(id).value('film_strength'), 100);
      f = await Finger.down(t, const Offset(200, 300));
      await f.drag(const Offset(-80, 2));
      await f.up();
      await settle(t);
      expect(app.editOf(id).value('film_strength'), lessThan(100));
      // double tap on the row resets it to the default
      await t.ensureVisible(find.byKey(const ValueKey('row-style-film_strength')));
      await t.tap(find.byKey(const ValueKey('row-style-film_strength')));
      await t.pump(const Duration(milliseconds: 50));
      await t.tap(find.byKey(const ValueKey('row-style-film_strength')));
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
