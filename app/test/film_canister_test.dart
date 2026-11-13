// The base icons (widgets/film_canister.dart): they draw at every size without overflow, the kinds and the data-driven colours
// make different pictures, and the comparison sheet of the three styles goes to build/ui-canister/styles_sheet.png.
import 'dart:io';
import 'dart:ui' as ui;

import 'package:dapple/model/bases.dart';
import 'package:dapple/model/film_catalog.dart';
import 'package:dapple/model/film_map.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:dapple/widgets/film_canister.dart';
import 'package:dapple/widgets/film_canister_palette.dart';
import 'dart:math' as math;
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';

import 'support/film_fixture.dart';
import 'support/harness.dart';

const _sizes = [23.0, 40.0, 72.0];
final _pos = parseFilmMap(File('assets/film/film_map.json').readAsStringSync());

BaseIconData _icon(String? id) => baseIconOf(testFilmCatalog, id, character: _pos[id]);

Widget _host(Widget child, {bool dark = false, Key? key}) => MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildTheme(dark ? Brightness.dark : Brightness.light),
      home: Scaffold(body: Center(child: RepaintBoundary(key: key, child: SizedBox(width: 96, height: 96, child: ColoredBox(color: dark ? DC.darkTheme.bg : DC.light.bg, child: Center(child: child)))))),
    );

Future<List<int>> _pixels(WidgetTester t, GlobalKey k) async {
  return (await t.runAsync(() async {
    final b = k.currentContext!.findRenderObject() as RenderRepaintBoundary;
    final img = await b.toImage(pixelRatio: 2);
    return (await img.toByteData(format: ui.ImageByteFormat.rawRgba))!.buffer.asUint8List().toList();
  }))!;
}

Future<List<int>> _render(WidgetTester t, BaseIconData d, double size, CanisterStyle style, {bool dark = false}) async {
  final k = GlobalKey();
  await t.pumpWidget(_host(BaseIcon(data: d, size: size, style: style), dark: dark, key: k));
  return _pixels(t, k);
}

double _diff(List<int> a, List<int> b) {
  var d = 0;
  for (var i = 0; i < a.length; i++) {
    d += (a[i] - b[i]).abs();
  }
  return d / a.length;
}

Future<void> _savePng(WidgetTester t, GlobalKey k, String name, {double ratio = 2}) async {
  await t.runAsync(() async {
    final b = k.currentContext!.findRenderObject() as RenderRepaintBoundary;
    final img = await b.toImage(pixelRatio: ratio);
    final bytes = await img.toByteData(format: ui.ImageByteFormat.png);
    final f = File('build/ui-canister/$name.png');
    f.parent.createSync(recursive: true);
    f.writeAsBytesSync(bytes!.buffer.asUint8List());
  });
}

void main() {
  testWidgets('every base draws at 23 / 40 / 72 px in every style and both themes without overflow', (t) async {
    await loadTestFonts();
    for (final dark in [false, true]) {
      for (final style in CanisterStyle.values) {
        for (final size in _sizes) {
          await t.pumpWidget(_host(
            Wrap(children: [for (final id in allBaseIds(testFilmCatalog)) BaseIcon(data: _icon(id), size: size, style: style, selected: id == 'kodak_portra_400')]),
            dark: dark,
          ));
          expect(t.takeException(), isNull, reason: '$style $size dark=$dark');
        }
      }
    }
  });

  testWidgets('the three kinds, a modern rendering and 标准 all look different', (t) async {
    await loadTestFonts();
    for (final style in CanisterStyle.values) {
      for (final size in _sizes) {
        BaseIconData film(CanisterKind k) => BaseIconData.film(CanisterSpec.of(kind: k, richness: 0.3, contrast: 0.3, code: 'N400A'));
        final px = [
          await _render(t, film(CanisterKind.negative), size, style),
          await _render(t, film(CanisterKind.slide), size, style),
          await _render(t, film(CanisterKind.cinema), size, style),
          await _render(t, const BaseIconData.modern('modern_clean'), size, style),
          await _render(t, const BaseIconData.none(), size, style),
        ];
        for (var i = 0; i < px.length; i++) {
          for (var j = i + 1; j < px.length; j++) {
            expect(_diff(px[i], px[j]), greaterThan(0.5), reason: '$style $size: $i vs $j');
          }
        }
      }
    }
  });

  testWidgets('colours follow the data: richer stocks are more saturated, pale ones lighter; modern renderings differ', (t) async {
    final pale = CanisterSpec.of(kind: CanisterKind.negative, richness: -0.9, contrast: 0);
    final rich = CanisterSpec.of(kind: CanisterKind.negative, richness: 0.9, contrast: 0);
    expect(HSLColor.fromColor(rich.body).saturation, greaterThan(HSLColor.fromColor(pale.body).saturation + 0.2));
    expect(HSLColor.fromColor(pale.body).lightness, greaterThan(HSLColor.fromColor(rich.body).lightness + 0.1));
    final flat = CanisterSpec.of(kind: CanisterKind.negative, richness: 0, contrast: -0.9);
    final harsh = CanisterSpec.of(kind: CanisterKind.negative, richness: 0, contrast: 0.9);
    expect(HSLColor.fromColor(harsh.cap).lightness, lessThan(HSLColor.fromColor(flat.cap).lightness));
    await loadTestFonts();
    final a = await _render(t, BaseIconData.film(pale), 40, CanisterStyle.soft);
    final b = await _render(t, BaseIconData.film(rich), 40, CanisterStyle.soft);
    expect(_diff(a, b), greaterThan(1));
    final m = [for (final k in kModernBases) await _render(t, BaseIconData.modern(k.id), 40, CanisterStyle.soft)];
    expect(_diff(m[0], m[1]), greaterThan(0.2));
    expect(_diff(m[1], m[2]), greaterThan(0.2));
    expect(_diff(m[0], m[2]), greaterThan(0.2));
  });

  testWidgets('the code shows from 40 px on only', (t) async {
    await loadTestFonts();
    final base = CanisterSpec.of(kind: CanisterKind.negative, richness: 0, contrast: 0, code: 'N400A');
    final bare = CanisterSpec.of(kind: CanisterKind.negative, richness: 0, contrast: 0);
    expect(_diff(await _render(t, BaseIconData.film(base), 23, CanisterStyle.flat), await _render(t, BaseIconData.film(bare), 23, CanisterStyle.flat)), 0);
    expect(_diff(await _render(t, BaseIconData.film(base), 40, CanisterStyle.flat), await _render(t, BaseIconData.film(bare), 40, CanisterStyle.flat)), greaterThan(0));
  });

  testWidgets('comparison sheet of the three styles', (t) async {
    await loadTestFonts();
    final film = ['fujifilm_pro_400h', 'fujifilm_xtra_400', 'kodak_portra_160', 'kodak_ektar_100', 'kodak_kodachrome_64', 'fujifilm_velvia_100', 'kodak_vision3_50d', 'kodak_vision3_500t'];
    final cols = <String?>[...film, ...kModernBases.map((m) => m.id), null];
    Widget panel(bool dark) {
      final c = dark ? DC.darkTheme : DC.light;
      Text label(String s) => Text(s, style: TextStyle(fontSize: 11, color: c.text2));
      return Container(
        color: c.bg,
        padding: const EdgeInsets.all(12),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            SizedBox(width: 56, child: label(dark ? 'dark' : 'light')),
            for (final id in cols) SizedBox(width: 84, child: Center(child: label(baseNameOf(testFilmCatalog, id)))),
          ]),
          for (final style in CanisterStyle.values)
            Padding(
              padding: const EdgeInsets.only(top: 10),
              child: Row(crossAxisAlignment: CrossAxisAlignment.center, children: [
                SizedBox(width: 56, child: label(style.name)),
                for (final id in cols)
                  SizedBox(
                    width: 84,
                    child: Column(children: [
                      for (final s in _sizes) Padding(padding: const EdgeInsets.symmetric(vertical: 3), child: BaseIcon(data: _icon(id), size: s, style: style)),
                    ]),
                  ),
              ]),
            ),
        ]),
      );
    }

    useWindowSized(t, const Size(1100, 1060));
    final k = GlobalKey();
    await t.pumpWidget(RepaintBoundary(
      key: k,
      child: MaterialApp(
        debugShowCheckedModeBanner: false,
        theme: buildTheme(Brightness.light),
        home: Scaffold(
          body: SingleChildScrollView(
            child: Column(mainAxisSize: MainAxisSize.min, children: [
              Theme(data: buildTheme(Brightness.light), child: panel(false)),
              Theme(data: buildTheme(Brightness.dark), child: panel(true)),
            ]),
          ),
        ),
      ),
    ));
    expect(t.takeException(), isNull);
    await _savePng(t, k, 'styles_sheet');
  });

  group('designed palette', () {
    List<double> lab(Color c) {
      double lin(double v) => v <= 0.04045 ? v / 12.92 : math.pow((v + 0.055) / 1.055, 2.4).toDouble();
      final r = lin(c.r), g = lin(c.g), b = lin(c.b);
      final l = math.pow(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b, 1 / 3), m = math.pow(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b, 1 / 3), s = math.pow(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b, 1 / 3);
      return [0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s, 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s];
    }

    double de(Color a, Color b) {
      final x = lab(a), y = lab(b);
      return 100 * math.sqrt(math.pow(x[0] - y[0], 2) + math.pow(x[1] - y[1], 2) + math.pow(x[2] - y[2], 2));
    }

    test('every stock of the catalog has designed colours (a new stock fails here until someone designs it)', () {
      for (final s in testFilmCatalog.stocks) {
        if (s.group == FilmGroup.negative || s.group == FilmGroup.slide || s.group == FilmGroup.cinema) {
          if (_pos.containsKey(s.id)) expect(kCanisterPalette.containsKey(s.name), isTrue, reason: '${s.id} (${s.name}) has no entry in kCanisterPalette');
        }
      }
      final codes = {for (final s in testFilmCatalog.stocks) s.name};
      for (final code in kCanisterPalette.keys) {
        expect(codes, contains(code), reason: '$code is not a stock of the catalog');
      }
    });

    test('a stock without an entry still gets the data-driven icon', () {
      const s = FilmStock('future_stock', 'Future', FilmGroup.negative);
      final a = CanisterSpec.forStock(s, character: const Offset(0.8, 0.4));
      final b = CanisterSpec.forStock(s, character: const Offset(-0.8, -0.4));
      expect(a.body, isNot(b.body));
      expect(a.kind, CanisterKind.negative);
    });

    test('bodies are told apart by OKLab distance: everywhere at least 9, map neighbours at least 11; clear of the modern icons', () {
      final byCode = {for (final s in testFilmCatalog.stocks) s.name: s.id};
      final ids = kCanisterPalette.keys.toList();
      Offset ref(String code) => Offset((0.5 + _pos[byCode[code]!]!.dx * kFilmMapX) * kFilmMapRef.width, (0.5 - _pos[byCode[code]!]!.dy * kFilmMapY) * kFilmMapRef.height);
      var minAll = 999.0, minNb = 999.0;
      String pairAll = '', pairNb = '';
      for (var i = 0; i < ids.length; i++) {
        for (var j = i + 1; j < ids.length; j++) {
          final d = de(kCanisterPalette[ids[i]]!.body, kCanisterPalette[ids[j]]!.body);
          if (d < minAll) {
            minAll = d;
            pairAll = '${ids[i]}/${ids[j]}';
          }
          if ((ref(ids[i]) - ref(ids[j])).distance < 55 && d < minNb) {
            minNb = d;
            pairNb = '${ids[i]}/${ids[j]}';
          }
        }
      }
      // ignore: avoid_print
      print('palette: min body separation $minAll ($pairAll), min among map neighbours $minNb ($pairNb)');
      expect(minAll, greaterThanOrEqualTo(9), reason: pairAll);
      expect(minNb, greaterThanOrEqualTo(11), reason: pairNb);
      // moderate chroma, mid lightness: nothing fluorescent, nothing invisible on either theme
      for (final e in kCanisterPalette.entries) {
        final l3 = lab(e.value.body);
        expect(math.sqrt(l3[1] * l3[1] + l3[2] * l3[2]), lessThan(0.17), reason: '${e.key} chroma');
        final L = l3[0];
        expect(L, inInclusiveRange(0.32, 0.86), reason: e.key);
      }
      for (final c in [const Color(0xFFA3AEBB), const Color(0xFFE0D8E8), const Color(0xFFA3A8B1)]) {
        for (final e in kCanisterPalette.entries) {
          expect(de(e.value.body, c), greaterThan(6), reason: '${e.key} vs modern/standard icon colour');
        }
      }
    });
  });

  testWidgets('palette sheet: every stock, round-1 colours above, soft style at 23 / 40 / 72 on light and dark', (t) async {
    await loadTestFonts();
    final byCode = {for (final s in testFilmCatalog.stocks) s.name: s.id};
    final ids = [for (final code in kCanisterPalette.keys) byCode[code]!]; // the table is in family order
    Widget panel(bool dark) {
      final c = dark ? DC.darkTheme : DC.light;
      Text label(String s, {double fs = 10}) => Text(s, style: TextStyle(fontSize: fs, color: c.text2));
      return Container(
        color: c.bg,
        padding: const EdgeInsets.all(10),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Row(children: [
            SizedBox(width: 50, child: label(dark ? 'dark' : 'light')),
            for (final id in ids) SizedBox(width: 62, child: Center(child: label(baseNameOf(testFilmCatalog, id)))),
          ]),
          Row(crossAxisAlignment: CrossAxisAlignment.center, children: [
            SizedBox(width: 50, child: label('round 1')),
            for (final id in ids)
              SizedBox(width: 62, child: Center(child: BaseIcon(data: BaseIconData.film(CanisterSpec.forStock(testFilmCatalog.stock(id)!, character: _pos[id], designed: false)), size: 40))),
          ]),
          const SizedBox(height: 6),
          Row(crossAxisAlignment: CrossAxisAlignment.center, children: [
            SizedBox(width: 50, child: label('palette')),
            for (final id in ids)
              SizedBox(width: 62, child: Column(children: [for (final s in _sizes) Padding(padding: const EdgeInsets.symmetric(vertical: 3), child: BaseIcon(data: _icon(id), size: s))])),
          ]),
        ]),
      );
    }

    useWindowSized(t, const Size(1340, 520));
    final k = GlobalKey();
    await t.pumpWidget(RepaintBoundary(
      key: k,
      child: MaterialApp(
        debugShowCheckedModeBanner: false,
        theme: buildTheme(Brightness.light),
        home: Scaffold(body: Column(children: [
          Theme(data: buildTheme(Brightness.light), child: panel(false)),
          Theme(data: buildTheme(Brightness.dark), child: panel(true)),
        ])),
      ),
    ));
    expect(t.takeException(), isNull);
    await _savePng(t, k, 'palette_sheet');
  });
}

void useWindowSized(WidgetTester t, Size s) {
  t.view.devicePixelRatio = 1.0;
  t.view.physicalSize = s;
  addTearDown(t.view.reset);
}
