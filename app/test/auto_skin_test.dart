// 自动 (top bar switch, per photo) and 肤色保护 (film window checkbox), owner decisions 2026-11-17: the wire, the saved edit, undo, the
// copy / paste / sync carry, and the real page at the top-bar variants. Screenshots: test/goldens/editor_v2/auto_*.png, skin_*.png.
import 'dart:convert';

import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show dump, settle;
import 'support/film_fixture.dart';
import 'support/harness.dart';

Finder k(String key) => find.byKey(ValueKey(key));

Future<(AppState, String)> openAt(WidgetTester t, Size size, {double top = 36, double bottom = 24, bool dark = false, EGroup? group, bool film = false}) async {
  useWindow(t, size, top: top, bottom: bottom);
  await loadTestFonts();
  final app = AppState.demo();
  if (film) app.film = testFilmCatalog;
  final id = app.photos.firstWhere((p) => p.title == '外滩夕阳' && p.id.startsWith('d')).id;
  if (film) app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
  await t.pumpWidget(RepaintBoundary(
    key: shotKey,
    child: MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildTheme(Brightness.light),
      darkTheme: buildTheme(Brightness.dark),
      themeMode: dark ? ThemeMode.dark : ThemeMode.light,
      home: EditorPage(app: app, photoId: id, resamplePointers: false, initialGroup: group),
    ),
  ));
  await settle(t);
  return (app, id);
}

Map<String, dynamic> wireOf(EditState e) => e.toWire(defaultLook: 'natural', defaultToneStrength: 60);

void main() {
  group('自动: the model and the wire', () {
    test('default on, saved only when off, old edits are on, no schema bump', () {
      expect(EditState.empty.auto, isTrue);
      expect(EditState.empty.toJson().containsKey('auto'), isFalse);
      final off = EditState.empty.copyWith(auto: false);
      expect(off.toJson()['auto'], false);
      expect(EditState.fromJson(jsonDecode(jsonEncode(off.toJson())) as Map<String, dynamic>).auto, isFalse);
      expect(EditState.fromJson({'v': 2, 'lookStrength': 100, 'values': {}}).auto, isTrue);
      expect(EditState.fromJson({'lookStrength': 100, 'values': {'film_wb': 1}}).auto, isTrue, reason: 'an old (schema 1) edit');
      expect(kEditSchema, 2);
      expect(off.isAdjusted, isTrue);
      expect(off.sameAs(EditState.empty), isFalse);
    });

    test('wireParams: the correction is the base only with 自动 on (and the global setting on)', () {
      final app = AppState.demo();
      final ph = app.photos.first;
      ph.correction = {'contrast': 12.0, 'exposure': 0.4};
      expect(app.wireParams(ph)['contrast'], 12.0);
      ph.edit = ph.edit.copyWith(auto: false);
      expect(app.wireParams(ph).containsKey('contrast'), isFalse);
      ph.edit = ph.edit.copyWith(auto: true);
      app.settings.autoProcess = false;
      expect(app.wireParams(ph).containsKey('contrast'), isFalse);
    });

    test('the toggle is one undo step; 还原为自动结果 turns it back on', () {
      final app = AppState.demo();
      final id = app.photos.first.id;
      app.applyEdit(id, (e) => e.copyWith(auto: false));
      expect(app.editOf(id).auto, isFalse);
      app.undo(id);
      expect(app.editOf(id).auto, isTrue);
      app.redo(id);
      expect(app.editOf(id).auto, isFalse);
      app.restoreAuto(id);
      expect(app.editOf(id).auto, isTrue);
    });
  });

  group('肤色保护: the model and the wire', () {
    test('film_skin travels only with a stock; 1 by default, 0 when unchecked', () {
      expect(wireOf(EditState.empty).containsKey('film_skin'), isFalse);
      expect(wireOf(EditState.empty.copyWith(filmSkin: false)).containsKey('film_skin'), isFalse);
      expect(wireOf(EditState.empty.copyWith(film: 'kodak_portra_400'))['film_skin'], 1);
      expect(wireOf(EditState.empty.copyWith(film: 'kodak_portra_400', filmSkin: false))['film_skin'], 0);
    });

    test('saved only when off; old edits are on; paste and sync carry it with the film', () {
      expect(EditState.empty.toJson().containsKey('filmSkin'), isFalse);
      final e = EditState.empty.copyWith(film: 'kodak_portra_400', filmSkin: false);
      expect(EditState.fromJson(jsonDecode(jsonEncode(e.toJson())) as Map<String, dynamic>).filmSkin, isFalse);
      expect(EditState.fromJson({'v': 2, 'film': 'kodak_portra_400', 'values': {}}).filmSkin, isTrue);
      expect(EditState.empty.pasted(e.withoutCrop()).filmSkin, isFalse);
      expect(EditState.empty.merged(e, {SyncCat.style}).filmSkin, isFalse);
      expect(EditState.empty.merged(e, {SyncCat.tone}).filmSkin, isTrue);
    });

    test('the checkbox is one undo step', () {
      final app = AppState.demo();
      final id = app.photos.first.id;
      app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
      app.applyEdit(id, (e) => e.copyWith(filmSkin: false));
      app.undo(id);
      expect(app.editOf(id).filmSkin, isTrue);
      expect(app.editOf(id).film, 'kodak_portra_400');
    });
  });

  group('the page', () {
    const variants = <String, (Size, double, double)>{
      'phone360': (Size(360, 780), 36, 24),
      'phone412': (Size(412, 915), 36, 24),
      'landscape805': (Size(805, 351), 28, 16),
      'fold_inner': (Size(904, 832), 36, 24),
      'fold_half': (Size(412, 1004), 36, 24),
    };
    for (final v in variants.entries) {
      testWidgets('top bar fits with 自动 off: ${v.key}', (t) async {
        final (app, id) = await openAt(t, v.value.$1, top: v.value.$2, bottom: v.value.$3);
        expect(k('toolbar-auto'), findsOneWidget);
        await t.tap(k('toolbar-auto'));
        await settle(t, 4);
        expect(app.editOf(id).auto, isFalse);
        expect(t.takeException(), isNull);
        final r = t.getRect(k('toolbar-auto'));
        expect(r.left, greaterThanOrEqualTo(0));
        expect(r.right, lessThanOrEqualTo(v.value.$1.width));
        expect(t.getRect(k('toolbar-more')).right, lessThanOrEqualTo(v.value.$1.width));
        expect(r.right, lessThanOrEqualTo(t.getRect(k('toolbar-undo')).left + 0.5), reason: 'no overlap with the history group');
        if (const ['phone360', 'landscape805', 'fold_inner'].contains(v.key)) await dump(t, 'auto_off_${v.key}');
        await t.tap(k('toolbar-auto'));
        await settle(t, 4);
        expect(app.editOf(id).auto, isTrue);
      });
    }

    testWidgets('a cover-size window has no top bar to switch in', (t) async {
      await openAt(t, const Size(400, 400));
      expect(k('toolbar-auto'), findsNothing);
    });

    testWidgets('with the global setting off the switch stays, off and not tappable', (t) async {
      final (app, id) = await openAt(t, const Size(412, 915));
      app.settings.autoProcess = false;
      app.changed();
      await settle(t, 3);
      await t.tap(k('toolbar-auto'), warnIfMissed: false);
      await settle(t, 3);
      expect(app.editOf(id).auto, isTrue, reason: 'nothing changes');
      await dump(t, 'auto_global_off');
    });

    for (final v in const {
      'phone412': (Size(412, 915), 36.0, 24.0),
      'landscape805': (Size(805, 351), 28.0, 16.0),
      'fold_inner': (Size(904, 832), 36.0, 24.0),
    }.entries) {
      testWidgets('肤色保护 checkbox in the film window: ${v.key}', (t) async {
        final (app, id) = await openAt(t, v.value.$1, top: v.value.$2, bottom: v.value.$3, group: EGroup.style, film: true);
        await t.tap(k('items-button'));
        await settle(t, 6);
        expect(k('film-skin'), findsOneWidget);
        expect(app.editOf(id).filmSkin, isTrue);
        await t.tap(k('film-skin'));
        await settle(t, 4);
        expect(app.editOf(id).filmSkin, isFalse);
        expect(t.takeException(), isNull);
        final r = t.getRect(k('film-skin'));
        expect(r.right, lessThanOrEqualTo(v.value.$1.width));
        expect(r.left, greaterThanOrEqualTo(0));
        await dump(t, 'skin_${v.key}');
        await t.tap(k('film-skin'));
        await settle(t, 3);
        expect(app.editOf(id).filmSkin, isTrue);
      });
    }

    testWidgets('no stock, no 肤色保护', (t) async {
      await openAt(t, const Size(412, 915), group: EGroup.style);
      await t.tap(k('items-button'));
      await settle(t, 6);
      expect(k('film-skin'), findsNothing);
    });
  });
}
