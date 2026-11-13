// The base icons (widgets/film_canister.dart): they draw at every size without overflow, the kinds and the data-driven colours
// make different pictures, and the comparison sheet of the three styles goes to build/ui-canister/styles_sheet.png.
import 'dart:io';
import 'dart:ui' as ui;

import 'package:dapple/model/bases.dart';
import 'package:dapple/model/film_map.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:dapple/widgets/film_canister.dart';
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
}

void useWindowSized(WidgetTester t, Size s) {
  t.view.devicePixelRatio = 1.0;
  t.view.physicalSize = s;
  addTearDown(t.view.reset);
}
