// Rendering bases (film stocks + three modern renderings) on the film map and the 默认成像 page: the swatch service, the map with
// swatches (placeholder -> image, z-order, modern shape, release closes and selects), the pad with a modern base, the default
// page, and the default base at ingest. Screenshots (widget-rendered; the swatch pixels are a stand-in tint of the bundled
// sample, not engine output) go to build/ui-looks/.
import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:dapple/model/bases.dart';
import 'package:dapple/model/catalog.dart';
import 'package:dapple/model/film_map.dart';
import 'package:dapple/model/pad_model.dart';
import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/screens/editor/film_map_view.dart';
import 'package:dapple/screens/editor/pad_widgets.dart' show LookDot;
import 'package:dapple/screens/library/styles_page.dart';
import 'package:dapple/services/base_swatches.dart';
import 'package:dapple/services/ingest.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show Finger, setView, settle;
import 'support/film_fixture.dart';
import 'support/harness.dart';

/// Hands out pre-made images per base; `gate` (when set) holds every render until it completes.
class FakeSource implements SwatchSource {
  FakeSource(this.key, this.images, {this.gate});
  @override
  final String key;
  final Map<String?, ui.Image> images;
  Future<void>? gate;
  final List<String?> calls = [];
  int sizeSeen = 0;
  final Set<int> sizesSeen = {};

  @override
  Future<ui.Image?> render(String? baseId, int longEdge) async {
    calls.add(baseId);
    sizeSeen = longEdge;
    sizesSeen.add(longEdge);
    if (gate != null) await gate;
    return images[baseId];
  }
}

/// One small image per base: the bundled sample, tinted differently per id (what the engine would do in earnest).
Future<Map<String?, ui.Image>> makeImages(WidgetTester t, Iterable<String?> ids) async {
  return (await t.runAsync(() async {
    final bytes = File('assets/style_sample/sample.jpg').readAsBytesSync();
    final codec = await ui.instantiateImageCodec(Uint8List.fromList(bytes), targetWidth: 96);
    final src = (await codec.getNextFrame()).image;
    final out = <String?, ui.Image>{};
    for (final id in ids) {
      final h = id == null ? 0.0 : (id.hashCode % 360) / 360.0;
      final rec = ui.PictureRecorder();
      final cv = Canvas(rec);
      final tint = HSVColor.fromAHSV(1, h * 360, id == null ? 0 : 0.5, 1).toColor();
      cv.drawImage(src, Offset.zero, Paint()..colorFilter = ColorFilter.mode(tint, BlendMode.modulate));
      out[id] = await rec.endRecording().toImage(src.width, src.height);
    }
    return out;
  }))!;
}

final _ids = allBaseIds(testFilmCatalog);

Future<void> shot(WidgetTester t, GlobalKey k, String name) async {
  await t.runAsync(() async {
    final b = k.currentContext!.findRenderObject() as RenderRepaintBoundary;
    final img = await b.toImage(pixelRatio: 1.5);
    final bytes = await img.toByteData(format: ui.ImageByteFormat.png);
    final f = File('build/ui-looks/$name.png');
    f.parent.createSync(recursive: true);
    f.writeAsBytesSync(bytes!.buffer.asUint8List());
  });
}

final _shotKey = GlobalKey();

/// Look swatches: remembers the base each service was made for.
final lookBases = <String?>[];
final lookSources = <FakeSource>[];

Future<(AppState, String, FakeSource)> openEditor(WidgetTester t, {Future<void>? gate}) async {
  setView(t, const Size(1140, 2616), 2.75);
  await loadTestFonts();
  final app = AppState.demo();
  app.film = testFilmCatalog;
  final id = app.photos.firstWhere((p) => p.title == '外滩夕阳' && p.id.startsWith('d')).id;
  final src = FakeSource('photo-$id', await makeImages(t, _ids), gate: gate);
  final lookImgs = await makeImages(t, [for (final l in dLooks) l.id]);
  lookBases.clear();
  lookSources.clear();
  await t.pumpWidget(RepaintBoundary(
    key: _shotKey,
    child: MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildTheme(Brightness.light),
      home: EditorPage(app: app, photoId: id, resamplePointers: false, filmPadFetcher: (_) async => null, swatchSource: (_) => src,
        lookSwatchSource: (pid, base) {
          lookBases.add(base);
          final f = FakeSource('looks|$pid|$base', lookImgs);
          lookSources.add(f);
          return f;
        },
      ),
    ),
  ));
  await settle(t);
  return (app, id, src);
}

Future<void> openMap(WidgetTester t) async {
  await t.tap(find.byKey(ValueKey('dock-${EGroup.style}')));
  await settle(t, 8);
  await t.tap(find.byKey(const ValueKey('stock-button')));
  await settle(t, 8);
}

Offset mapPoint(WidgetTester t, FilmDot d) {
  final m = t.getRect(find.byKey(const ValueKey('film-map')));
  final f = dotFraction(d);
  return m.topLeft + Offset(f.dx * m.width, f.dy * m.height);
}

void main() {
  setUp(() {
    BaseSwatches.clearCache();
    // the real asset (measured stocks, the modern cluster at its place)
    FilmMapStore.debugSet(parseFilmMap(File('assets/film/film_map.json').readAsStringSync()));
  });
  tearDown(() {
    FilmMapStore.debugSet(null);
    EditState.defaultBase = null;
  });

  group('swatch service', () {
    testWidgets('renders every base once, the prioritised one first; a second service for the same photo costs nothing', (t) async {
      final imgs = await makeImages(t, [null, 'a', 'b', 'c']);
      final src = FakeSource('p1', imgs);
      final sw = BaseSwatches(source: src, ids: [null, 'a', 'b', 'c'], watchPointers: false, gap: Duration.zero, settle: Duration.zero)..prioritise('c');
      var notes = 0;
      sw.addListener(() => notes++);
      expect(sw.imageOf('a'), isNull);
      sw.start();
      await settle(t, 3);
      expect(src.calls, ['c', null, 'a', 'b']);
      expect(src.sizeSeen, 96);
      expect(sw.done, isTrue);
      expect(sw.imageOf('b'), imgs['b']);
      expect(notes, greaterThanOrEqualTo(4));
      sw.dispose();
      final again = FakeSource('p1', imgs);
      final sw2 = BaseSwatches(source: again, ids: [null, 'a', 'b', 'c'], watchPointers: false, gap: Duration.zero, settle: Duration.zero)..start();
      await settle(t, 2);
      expect(again.calls, isEmpty, reason: 'cached per photo');
      expect(sw2.imageOf('a'), imgs['a']);
      sw2.dispose();
    });

    testWidgets('waits while a pointer is down (a drag owns the engine) and carries on after it is up', (t) async {
      final imgs = await makeImages(t, [null, 'a', 'b', 'c']);
      final src = FakeSource('p2', imgs);
      await t.pumpWidget(const SizedBox());
      final sw = BaseSwatches(source: src, ids: [null, 'a', 'b', 'c'], gap: const Duration(milliseconds: 10), settle: const Duration(milliseconds: 100));
      final g = await t.startGesture(const Offset(5, 5));
      sw.start();
      await t.pump(const Duration(seconds: 1));
      expect(src.calls, isEmpty, reason: 'no render while a finger is down');
      await g.up();
      await t.pump(const Duration(milliseconds: 50));
      expect(src.calls, isEmpty, reason: 'and a moment after it');
      await t.pump(const Duration(seconds: 1));
      expect(src.calls.length, 4);
      sw.dispose();
    });

    testWidgets('dispose stops it: the render in flight is dropped, nothing new starts', (t) async {
      final imgs = await makeImages(t, [null, 'a', 'b']);
      final gate = Completer<void>();
      final src = FakeSource('p3', imgs, gate: gate.future);
      final sw = BaseSwatches(source: src, ids: [null, 'a', 'b'], watchPointers: false, gap: Duration.zero, settle: Duration.zero)..start();
      await settle(t, 2);
      expect(src.calls.length, 1);
      sw.dispose();
      gate.complete();
      await settle(t, 3);
      expect(src.calls.length, 1);
      expect(sw.imageOf(null), isNull);
    });

    testWidgets('memory is bounded: only the last few photos are kept', (t) async {
      final imgs = await makeImages(t, [null]);
      for (var i = 0; i < 6; i++) {
        final sw = BaseSwatches(source: FakeSource('m$i', imgs), ids: [null], watchPointers: false, gap: Duration.zero, settle: Duration.zero)..start();
        await settle(t, 1);
        sw.dispose();
      }
      final probe = FakeSource('m0', imgs);
      final sw = BaseSwatches(source: probe, ids: [null], watchPointers: false, gap: Duration.zero, settle: Duration.zero)..start();
      await settle(t, 1);
      expect(probe.calls, [null], reason: 'the oldest photo was evicted and is rendered again');
      sw.dispose();
    });
  });

  group('film map with swatches', () {
    testWidgets('a swatch is a tinted placeholder until its picture arrives, then fades in; the size does not jump', (t) async {
      final gate = Completer<void>();
      final (app, id, src) = await openEditor(t, gate: gate.future);
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      await openMap(t);
      final tile = find.byKey(const ValueKey('stock-kodak_ektar_100'));
      expect(tile, findsOneWidget);
      expect(find.descendant(of: tile, matching: find.byKey(const ValueKey('swatch-placeholder'))), findsOneWidget);
      expect(find.descendant(of: tile, matching: find.byKey(const ValueKey('swatch-image'))), findsNothing);
      final before = t.getSize(tile);
      await shot(t, _shotKey, 'map_placeholders');
      gate.complete();
      await settle(t, 40); // 30 renders, one at a time, 30 ms apart
      expect(find.descendant(of: tile, matching: find.byKey(const ValueKey('swatch-image'))), findsOneWidget);
      expect(t.getSize(tile), before, reason: 'no layout jump when the image arrives');
      expect(src.calls.first, 'kodak_portra_400', reason: 'the current base is rendered first');
      await shot(t, _shotKey, 'map_overview');
    });

    testWidgets('film swatches are rounded squares, the modern ones circles in a labelled cluster', (t) async {
      await openEditor(t);
      await openMap(t);
      await settle(t, 40);
      expect(find.descendant(of: find.byKey(const ValueKey('stock-kodak_ektar_100')), matching: find.byType(ClipRRect)), findsOneWidget);
      expect(find.descendant(of: find.byKey(const ValueKey('stock-kodak_ektar_100')), matching: find.byType(ClipOval)), findsNothing);
      for (final m in kModernBases) {
        expect(find.descendant(of: find.byKey(ValueKey('stock-${m.id}')), matching: find.byType(ClipOval)), findsOneWidget, reason: m.name);
      }
      expect(find.byKey(const ValueKey('modern-plate')), findsNothing, reason: 'measured positions are spread over the map: no plate over unrelated stocks');
      final sizes = {for (final m in kModernBases) t.getSize(find.byKey(ValueKey('stock-${m.id}'))).width};
      expect(sizes.single, kSwatchMagnified, reason: 'laid out at the magnified size, scaled down in the overview');
    });

    testWidgets('a tight cluster of modern renderings gets the labelled plate', (t) async {
      FilmMapStore.debugSet({
        'modern_clean': const Offset(-0.80, 0.50),
        'modern_vivid': const Offset(-0.62, 0.64),
        'modern_soft': const Offset(-0.62, 0.36),
      });
      await openEditor(t);
      await openMap(t);
      await settle(t, 10);
      expect(find.byKey(const ValueKey('modern-plate')), findsOneWidget);
      expect(find.descendant(of: find.byKey(const ValueKey('modern-plate')), matching: find.text('现代')), findsOneWidget);
    });

    test('paint order: nearest the finger on top, the selected one always last', () {
      final dots = layoutFilmMap(testFilmCatalog, const {});
      const size = Size(340, 210);
      final target = dots[3];
      final u = dotFraction(target);
      final order = paintOrder(dots, size, u, null);
      expect(order.last.id, target.id);
      // furthest first
      double d(FilmDot x) => Offset((dotFraction(x).dx - u.dx) * size.width, (dotFraction(x).dy - u.dy) * size.height).distanceSquared;
      for (var i = 1; i < order.length; i++) {
        expect(d(order[i - 1]), greaterThanOrEqualTo(d(order[i]) - 1e-12));
      }
      final sel = dots[10].id == target.id ? dots[11] : dots[10];
      expect(paintOrder(dots, size, u, sel.id).last.id, sel.id);
      expect(paintOrder(dots, size, null, sel.id).last.id, sel.id);
    });

    testWidgets('while magnified the swatch nearest the finger is the last one painted', (t) async {
      final (app, id, _) = await openEditor(t);
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      await openMap(t);
      await settle(t, 40);
      final dots = layoutFilmMap(testFilmCatalog, FilmMapStore.positions ?? const {});
      final target = dots.firstWhere((d) => d.id == 'kodak_ektar_100');
      final f = await Finger.down(t, mapPoint(t, target));
      await settle(t, 4);
      expect(app.editOf(id).film, 'kodak_ektar_100');
      // the selected one is last (it is the nearest); slide to another one: that one is the nearest but the selected stays on top
      final tiles = t.widgetList<BaseSwatchTile>(find.byType(BaseSwatchTile)).toList();
      expect(tiles.last.selected, isTrue);
      expect(tiles.where((x) => x.selected).length, 1);
      await shot(t, _shotKey, 'map_magnified');
      await f.up();
      await settle(t, 8);
    });

    testWidgets('pressing a modern rendering selects it and the map closes on release; the button names it', (t) async {
      final (app, id, _) = await openEditor(t);
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400', values: {...e.values, 'film_grain': 150, 'film_shoot': 0.3}));
      await openMap(t);
      final dots = layoutFilmMap(testFilmCatalog, FilmMapStore.positions ?? const {});
      final vivid = dots.firstWhere((d) => d.id == 'modern_vivid');
      final f = await Finger.down(t, mapPoint(t, vivid));
      await settle(t, 4);
      expect(app.editOf(id).film, 'modern_vivid');
      await shot(t, _shotKey, 'map_modern_pressed');
      await f.up();
      await settle(t, 8);
      expect(find.byKey(const ValueKey('film-map')), findsNothing);
      expect(app.editOf(id).film, 'modern_vivid');
      expect(find.descendant(of: find.byKey(const ValueKey('stock-button')), matching: find.text('鲜明')), findsOneWidget);
      // film-pack controls are back to their defaults for a modern base
      final v = app.editOf(id).values;
      expect(v.containsKey('film_grain'), isFalse);
      expect(v.containsKey('film_shoot'), isFalse);
    });
  });

  group('the film pad with a modern base', () {
    testWidgets('both pad axes are dead, the strength strip stays, the radar has the strength only, no paper', (t) async {
      final (app, id, _) = await openEditor(t);
      app.applyEdit(id, (e) => e.copyWith(film: 'modern_clean'));
      await t.tap(find.byKey(ValueKey('dock-${EGroup.style}')));
      await settle(t, 10);
      expect(find.descendant(of: find.byKey(const ValueKey('stock-button')), matching: find.text('通透')), findsOneWidget);
      expect(find.byKey(const ValueKey('pad-film')), findsOneWidget);
      expect(find.byKey(const ValueKey('strip-film')), findsOneWidget);
      // the strength strip drives film_strength and never writes grain / halation / black (the strip's upper half would)
      final strip = t.getRect(find.byKey(const ValueKey('strip-film')));
      final f = await Finger.down(t, strip.center);
      await f.drag(Offset(0, -strip.height * 0.45));
      await f.up();
      await settle(t, 4);
      final e = app.editOf(id);
      expect(e.film, 'modern_clean');
      expect(e.values.keys.where((k) => filmValueKeys.contains(k) && k != 'film_strength'), isEmpty);
      // the pad: both axes dead -> the cursor cannot leave the centre
      final pad = t.getRect(find.byKey(const ValueKey('pad-film')));
      final g = await Finger.down(t, pad.center);
      await g.drag(Offset(pad.width * 0.3, -pad.height * 0.3));
      await g.up();
      await settle(t, 4);
      expect(app.editOf(id).value('film_shoot'), 0);
      expect(app.editOf(id).value('film_print'), 0);
      // layer 3: one spoke, and no 相纸
      await t.tap(find.byKey(const ValueKey('items-button')));
      await settle(t, 6);
      expect(find.byKey(const ValueKey('extra-相纸')), findsNothing);
      expect(padItems(PadCat.film, modern: true).map((i) => i.id), ['film_strength']);
      await shot(t, _shotKey, 'pad_modern_items');
    });

    test('switching to a modern base and back to a stock', () {
      var e = const EditState(film: 'kodak_portra_400', filmPaper: 'kodak_portra_endura', values: {'film_grain': 120, 'film_strength': 70, 'film_warmth': 5, 'exposure': 10});
      e = withModernBase(e, 'modern_soft');
      expect(e.film, 'modern_soft');
      expect(e.filmPaper, isNull);
      expect(e.values, {'film_strength': 70, 'exposure': 10});
      e = e.copyWith(film: 'fujifilm_velvia_100');
      expect(e.film, 'fujifilm_velvia_100');
      expect(isModernId('modern_soft'), isTrue);
      expect(isModernId('kodak_portra_400'), isFalse);
      expect(isModernId('modern_other'), isFalse);
    });
  });

  group('colour looks as real swatches, on top of the current base', () {
    testWidgets('the colour pad dots are thumbnails of the photo with each look, rendered on the current base', (t) async {
      final (app, id, _) = await openEditor(t);
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      await t.tap(find.byKey(ValueKey('dock-${EGroup.color}')));
      await settle(t, 10);
      expect(lookBases, ['kodak_portra_400'], reason: 'one look service, for the current base');
      for (final p in colorPresets) {
        final dot = find.byKey(ValueKey('preset-${p.look}'));
        expect(dot, findsOneWidget);
        expect(t.getSize(dot), const Size(26, 26));
        expect(find.descendant(of: dot, matching: find.byKey(const ValueKey('look-image'))), findsOneWidget, reason: p.look);
      }
      expect(lookSources.single.calls.toSet(), containsAll({'warm', 'amber', 'rose', 'neutral', 'cool', 'teal', 'bw'}));
      expect(lookSources.single.sizeSeen, 96);
      await shot(t, _shotKey, 'color_pad_looks_film');
      // a modern base: a new service for that base (the old pictures show the other base)
      app.applyEdit(id, (e) => e.copyWith(film: 'modern_vivid'));
      await t.pump(const Duration(milliseconds: 300));
      await t.tap(find.byKey(ValueKey('dock-${EGroup.style}')));
      await settle(t, 4);
      await t.tap(find.byKey(ValueKey('dock-${EGroup.color}')));
      await settle(t, 10);
      expect(lookBases.last, 'modern_vivid');
      await shot(t, _shotKey, 'color_pad_looks_modern');
    });

    testWidgets('until the picture arrives a dot shows the look own colour, the size does not change', (t) async {
      final img = (await makeImages(t, ['x']))['x'];
      Widget dot(ui.Image? i) => MaterialApp(home: Center(child: LookDot(image: i, tint: Colors.orange, selected: false)));
      await t.pumpWidget(dot(null));
      final size = t.getSize(find.byType(LookDot));
      expect(find.byKey(const ValueKey('look-placeholder')), findsOneWidget);
      expect(find.byKey(const ValueKey('look-image')), findsNothing);
      await t.pumpWidget(dot(img));
      await t.pump(const Duration(milliseconds: 300));
      expect(find.byKey(const ValueKey('look-image')), findsOneWidget);
      expect(t.getSize(find.byType(LookDot)), size);
    });
  });

  group('map layout', () {
    test('the modern renderings are in the layout, a separate cluster, not touching a stock or each other', () {
      final dots = layoutFilmMap(testFilmCatalog, const {});
      expect(dots.where((d) => d.isModern).map((d) => d.id), kModernBases.map((m) => m.id));
      expect(minDotDistance(dots), greaterThanOrEqualTo(kFilmDotGap - 1.0));
      expect(dots.length, testFilmCatalog.stocks.length + 3);
    });
    test('positions from the asset win over the built-in cluster', () {
      final dots = layoutFilmMap(testFilmCatalog, {'modern_clean': const Offset(0.9, -0.9)});
      final d = dots.firstWhere((d) => d.id == 'modern_clean');
      expect(d.placed, isTrue);
      expect((d.character - const Offset(0.9, -0.9)).distance, 0);
    });
  });

  group('默认成像 page', () {
    Future<(AppState, FakeSource)> openPage(WidgetTester t, {AppState? with_, Future<void>? gate}) async {
      useWindow(t, phoneSize);
      await loadTestFonts();
      final app = with_ ?? AppState.demo();
      app.film = testFilmCatalog;
      app.tab = 1;
      final src = FakeSource('sample', await makeImages(t, _ids), gate: gate);
      await t.pumpWidget(RepaintBoundary(
        key: _shotKey,
        child: MaterialApp(
          debugShowCheckedModeBanner: false,
          theme: buildTheme(Brightness.light),
          home: Scaffold(body: ListenableBuilder(listenable: app, builder: (_, _) => StylesPage(app: app, wide: false, swatchSource: (_) => src))),
        ),
      ));
      await t.pump(const Duration(milliseconds: 100));
      return (app, src);
    }

    testWidgets('groups 胶片 / 现代 / 标准, placeholders then engine swatches, tap sets the default and the card follows', (t) async {
      final gate = Completer<void>();
      final (app, src) = await openPage(t, gate: gate.future);
      for (final k in ['group-negative', 'group-slide', 'group-modern', 'group-none']) {
        await t.scrollUntilVisible(find.byKey(ValueKey(k)), 200, scrollable: find.byType(Scrollable).first);
        expect(find.byKey(ValueKey(k)), findsOneWidget, reason: k);
      }
      await t.scrollUntilVisible(find.byKey(const ValueKey('base-modern_soft')), -200, scrollable: find.byType(Scrollable).first);
      expect(find.descendant(of: find.byKey(const ValueKey('base-modern_soft')), matching: find.byKey(const ValueKey('swatch-placeholder'))), findsOneWidget);
      gate.complete();
      await t.pump(const Duration(seconds: 3));
      await t.pump(const Duration(seconds: 3));
      expect(src.calls, contains('modern_soft'));
      expect(find.descendant(of: find.byKey(const ValueKey('base-modern_soft')), matching: find.byKey(const ValueKey('swatch-image'))), findsOneWidget);
      await t.tap(find.byKey(const ValueKey('base-modern_soft')));
      await t.pump(const Duration(milliseconds: 300));
      expect(app.settings.defaultBaseId, 'modern_soft');
      expect(EditState.defaultBase, 'modern_soft');
      await t.drag(find.byType(Scrollable).first, const Offset(0, 4000));
      await t.pump(const Duration(milliseconds: 600));
      expect(find.descendant(of: find.byKey(const ValueKey('default-card')), matching: find.text('柔和')), findsOneWidget);
      expect(src.calls.where((c) => c == 'modern_soft').length, greaterThanOrEqualTo(2), reason: 'the large preview was rendered for the new default');
      await t.scrollUntilVisible(find.byKey(const ValueKey('base-none')), 200, scrollable: find.byType(Scrollable).first);
      await t.ensureVisible(find.byKey(const ValueKey('base-none')));
      await t.pump(const Duration(milliseconds: 300));
      await t.tap(find.byKey(const ValueKey('base-none')));
      await t.pump(const Duration(milliseconds: 300));
      expect(app.settings.defaultBaseId, isNull);
      await t.pump(const Duration(seconds: 6));
    });

    testWidgets('the card describes the default in one line instead of repeating the name', (t) async {
      final (app, _) = await openPage(t);
      Finder line(String x) => find.descendant(of: find.byKey(const ValueKey('default-card')), matching: find.text(x));
      expect(line('标准'), findsOneWidget, reason: 'the name once');
      expect(line('不加倾向'), findsOneWidget);
      app.setDefaultBase('modern_clean');
      await t.pump(const Duration(milliseconds: 300));
      expect(line('通透'), findsOneWidget);
      expect(line('中性干净'), findsOneWidget);
      app.setDefaultBase('modern_vivid');
      await t.pump(const Duration(milliseconds: 300));
      expect(line('浓郁有力'), findsOneWidget);
      app.setDefaultBase('modern_soft');
      await t.pump(const Duration(milliseconds: 300));
      expect(line('轻盈低反差'), findsOneWidget);
      app.setDefaultBase('kodak_portra_400');
      await t.pump(const Duration(milliseconds: 300));
      expect(line('N400C'), findsOneWidget);
      expect(line('负片'), findsOneWidget);
      await t.pump(const Duration(seconds: 6));
    });

    testWidgets('grid swatches are asked for at 2x (192 px), the large preview at 640', (t) async {
      final (_, src) = await openPage(t);
      await t.pump(const Duration(seconds: 3));
      expect(src.calls, isNotEmpty);
      expect(src.sizesSeen, containsAll([192, 640]));
      expect(src.sizesSeen, isNot(contains(96)));
    });

    testWidgets('a film default is visible without scrolling when the tab opens', (t) async {
      final app = AppState.demo();
      app.settings.defaultBaseId = 'kodak_vision3_500t';
      await openPage(t, with_: app);
      await t.pump(const Duration(milliseconds: 500));
      final r = t.getRect(find.byKey(const ValueKey('base-kodak_vision3_500t')));
      expect(r.top, greaterThan(0));
      expect(r.bottom, lessThan(phoneSize.height), reason: 'inside the window');
      await shot(t, _shotKey, 'default_page_film_default');
      await t.pump(const Duration(seconds: 6));
    });

    testWidgets('screenshot', (t) async {
      await openPage(t);
      await t.pump(const Duration(seconds: 4));
      await t.pump(const Duration(seconds: 4));
      await shot(t, _shotKey, 'default_page_top');
      await t.drag(find.byType(Scrollable).first, const Offset(0, -520));
      await t.pump(const Duration(milliseconds: 400));
      await shot(t, _shotKey, 'default_page_scrolled');
    });
  });

  group('default base at ingest', () {
    IncomingFile f(String n, int t) => IncomingFile(id: 'ftp:$n', name: n, addedMs: t, path: '/inbox/$n', size: 10);

    test('a new photo starts on the default base; existing photos and the none default are untouched; restore keeps the base', () {
      final a = AppState.demo();
      a.nativeOk = false;
      a.ingestFtpFiles([f('A.JPG', 1000)]);
      expect(a.photos.firstWhere((p) => p.name == 'A.JPG').edit.film, isNull, reason: 'default 标准: nothing stamped');
      a.setDefaultBase('modern_vivid');
      expect(EditState.defaultBase, 'modern_vivid');
      a.ingestFtpFiles([f('B.JPG', 2000)]);
      final b = a.photos.firstWhere((p) => p.name == 'B.JPG');
      expect(b.edit.film, 'modern_vivid');
      expect(a.photos.firstWhere((p) => p.name == 'A.JPG').edit.film, isNull);
      expect(b.edit.isAdjusted, isFalse, reason: 'the default base alone is not an adjustment');
      a.applyEdit(b.id, (e) => e.copyWith(film: 'kodak_portra_400'));
      expect(a.photos.firstWhere((p) => p.name == 'B.JPG').edit.isAdjusted, isTrue);
      a.restoreAuto(b.id);
      expect(a.editOf(b.id).film, 'modern_vivid', reason: 'back to the automatic result = the default base');
      a.setDefaultBase('kodak_portra_400');
      a.ingestFtpFiles([f('C.JPG', 3000)]);
      expect(a.photos.firstWhere((p) => p.name == 'C.JPG').edit.film, 'kodak_portra_400');
    });
  });
}
