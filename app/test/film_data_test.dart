// The film library as data (catalog grouping, names, swatches, no trademarks in the UI), the effective-range parser of the
// film pads, the film keys on the wire, and the licences page. (The panel itself is tested in pad_panel_test.dart.)
import 'dart:convert';
import 'dart:io';

import 'package:dapple/model/film_catalog.dart';
import 'package:dapple/model/film_names.dart';
import 'package:dapple/model/film_pad.dart';
import 'package:dapple/model/pad_model.dart' show latitudePercent;
import 'package:dapple/model/params.dart';
import 'package:dapple/screens/library/licenses_page.dart';
import 'package:dapple/screens/library/settings_page.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show phoneDpr, phonePhys, setView, settle;
import 'support/film_fixture.dart';
import 'support/harness.dart';

double latitudeFromPercent(double p) => p <= 80 ? (p - 80) / 25 : (p - 80) / 18;

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
        'effective_lo': {'film_wb': false, 'film_latitude': false},
        'effective_hi': {'film_print': false, 'film_wb': false, 'film_shoot': 'x'},
        'scene_k': 4600,
        'stock_kelvin': 5500.4,
      }));
      expect(r.hi('film_print'), isFalse);
      expect(r.lo('film_print'), isTrue, reason: 'a missing field is effective');
      expect(r.dead('film_wb'), isTrue);
      expect(r.sceneK, 4600);
      expect(r.stockK, 5500);
      expect(FilmPadRanges.parse(json({'effective_hi': {'film_wb': true}, 'effective_lo': {'film_wb': false}})).dead('film_wb'), isFalse,
          reason: 'film_wb is one-sided: only its +1 end counts');
      expect(FilmPadRanges.parse(json({'scene_k': 'warm', 'stock_k': -3})).sceneK, isNull, reason: 'garbage temperatures are ignored');
      expect(r.dead('film_latitude'), isFalse, reason: 'only the lower end is dead');
      expect(r.dead('film_print'), isFalse);
      expect(r.hi('film_shoot'), isTrue, reason: 'a non-bool is ignored');
      // the per-key object form
      final p = FilmPadRanges.parse(json({'film_black': {'effective_lo': false, 'effective_hi': false}}));
      expect(p.dead('film_black'), isTrue);
      expect(p.dead('film_shoot'), isFalse);
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

    testWidgets('RAW decoding libraries are credited: LibRaw (CDDL-1.0), libjxl + Highway (BSD-3), libomp (Apache-2.0 with LLVM exception)', (t) async {
      setView(t, phonePhys, phoneDpr);
      await loadTestFonts();
      final app = AppState.demo();
      await t.pumpWidget(MaterialApp(theme: buildTheme(Brightness.light), home: LicensesPage(app: app)));
      await settle(t, 3);
      Future<String> body(String key) async {
        await t.scrollUntilVisible(find.byKey(ValueKey(key)), 300, scrollable: find.byType(Scrollable).first);
        return t.widget<SelectableText>(find.byKey(ValueKey(key))).data!;
      }

      expect(await body('licence-libraw'), allOf(contains('LibRaw'), contains('CDDL-1.0'), contains('0.23.0')));
      expect(await body('licence-libjxl'), allOf(contains('libjxl'), contains('Highway'), contains('BSD 3-Clause'), contains('AS IS')));
      expect(await body('licence-libomp'), allOf(contains('OpenMP'), contains('Apache License 2.0')));
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

}
