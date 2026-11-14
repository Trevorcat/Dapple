import 'package:dapple/editor/fold_layout.dart' show debugFoldLayouts;
import 'package:dapple/screens/editor/film_map_view.dart';
import 'package:dapple/model/params.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import '../editor_v2_test.dart' show settle;
import '../support/film_fixture.dart';
import 'audit_support.dart';

Future<void> dock(WidgetTester t, EGroup g) async {
  await t.tap(find.byKey(ValueKey('dock-$g')));
  await settle(t, 8);
}

void main() {
  // these tests are about the ordinary panel placement (side pane on a wide window); the fold layouts have their own tests (fold_layout_test.dart)
  setUp(() => debugFoldLayouts = false);
  tearDown(() => debugFoldLayouts = true);
  for (final s in ['p', 'l', 'f']) {
    for (final dark in [false, true]) {
      final th = dark ? 'dark' : 'light';
      if (s != 'p' && dark) continue;
      testWidgets('A3 $s $th', (t) async {
        FilmMapStore.debugSet({
          'kodak_portra_400': const Offset(-0.5, -0.3),
          'kodak_ektar_100': const Offset(0.6, 0.5),
          'fujifilm_velvia_100': const Offset(0.0, 0.8),
        });
        addTearDown(() => FilmMapStore.debugSet(null));
        final r = await audOpen(t, s, dark, group: EGroup.light, before: (app, id) => app.film = testFilmCatalog);
        await audShot(t, '20_light_pad_${s}_$th');
        if (find.byKey(const ValueKey('items-button')).evaluate().isNotEmpty) {
          await t.tap(find.byKey(const ValueKey('items-button')));
          await settle(t, 6);
          await audShot(t, '21_light_items_${s}_$th');
          await t.tap(find.byKey(const ValueKey('header-back')));
          await settle(t, 6);
        }
        await dock(t, EGroup.color);
        await audShot(t, '22_color_pad_${s}_$th');
        await dock(t, EGroup.style);
        await audShot(t, '23_film_pad_${s}_$th');
        if (find.byKey(const ValueKey('stock-button')).evaluate().isNotEmpty) {
          await t.tap(find.byKey(const ValueKey('stock-button')));
          await settle(t, 8);
          await audShot(t, '24_film_map_${s}_$th');
        }
        await dock(t, EGroup.effects);
        await audShot(t, '25_texture_pad_${s}_$th');
        await dock(t, EGroup.crop);
        await audShot(t, '26_crop_${s}_$th');
        expect(r.id, isNotEmpty);
      });
    }
  }
}
