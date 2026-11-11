import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'support/harness.dart';

Future<AppState> open(WidgetTester tester, Size size, {bool dark = false, String title = '外滩夕阳'}) async {
  useWindow(tester, size);
  await loadTestFonts();
  final app = AppState.demo();
  final id = app.photos.firstWhere((p) => p.title == title && p.id.startsWith('d')).id;
  await tester.pumpWidget(RepaintBoundary(
    key: shotKey,
    child: MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildTheme(Brightness.light),
      darkTheme: buildTheme(Brightness.dark),
      themeMode: dark ? ThemeMode.dark : ThemeMode.light,
      home: EditorPage(app: app, photoId: id),
    ),
  ));
  await tester.pump(const Duration(milliseconds: 300));
  return app;
}

String idOf(AppState app, String title) => app.photos.firstWhere((p) => p.title == title && p.id.startsWith('d')).id;

Future<void> tapKey(WidgetTester t, String k) async {
  final f = find.byKey(ValueKey(k));
  final r = t.getRect(f);
  if (r.bottom > 945 || r.top < 0 || r.right > 412 && r.left > 412) {
    await t.ensureVisible(f);
    await t.pump(const Duration(milliseconds: 300));
  }
  await t.tap(f);
  for (var i = 0; i < 3; i++) {
    await t.pump(const Duration(milliseconds: 300));
  }
}

Future<void> tab(WidgetTester t, EGroup g) async {
  final f = find.byKey(ValueKey('group-tab-${g.name}'));
  final r = t.getRect(f);
  if (r.left < 0 || r.right > 412) {
    final strip = find.ancestor(of: f, matching: find.byType(SingleChildScrollView)).first;
    await t.drag(strip, Offset(r.left < 0 ? 300 : -300, 0));
    for (var i = 0; i < 3; i++) {
      await t.pump(const Duration(milliseconds: 300));
    }
  }
  await tapKey(t, 'group-tab-${g.name}');
}

void main() {
  testWidgets('phone: group switching shows each group', (t) async {
    await open(t, phoneSize);
    expect(find.text('明暗强度'), findsOneWidget);
    final expect_ = {
      EGroup.light: '曝光',
      EGroup.color: '色温',
      EGroup.curve: '高光区',
      EGroup.effects: '清晰度',
      EGroup.detail: '锐化',
      EGroup.grade: '混合',
      EGroup.crop: '拉直',
    };
    for (final e in expect_.entries) {
      await tab(t, e.key);
      expect(find.text(e.value), findsOneWidget, reason: groupLabels[e.key]);
    }
    await tab(t, EGroup.style);
    expect(find.text('明暗强度'), findsOneWidget);
    await shot(t, 'editor_phone_style');
    for (final g in [EGroup.light, EGroup.color, EGroup.curve, EGroup.grade, EGroup.crop]) {
      await tab(t, g);
      await shot(t, 'editor_phone_${g.name}');
    }
  });

  testWidgets('slider edit marks the tab, undo / redo work per photo', (t) async {
    final app = await open(t, phoneSize, title: '雪山');
    final id = idOf(app, '雪山');
    await tab(t, EGroup.light);
    expect(find.byKey(const ValueKey('dot-light')), findsNothing);
    final s = find.byKey(const ValueKey('exposure-slider'));
    final r = t.getRect(s);
    await t.tapAt(Offset(r.left + r.width * 0.75, r.center.dy));
    await t.pump(const Duration(milliseconds: 100));
    expect(app.editOf(id).values['exposure'], greaterThan(30));
    expect(find.byKey(const ValueKey('dot-light')), findsOneWidget);
    expect(find.byKey(const ValueKey('exposure-reset')), findsOneWidget);
    await tapKey(t, 'toolbar-undo');
    expect(app.editOf(id).values.containsKey('exposure'), isFalse);
    expect(find.byKey(const ValueKey('dot-light')), findsNothing);
    await tapKey(t, 'toolbar-redo');
    expect(app.editOf(id).values.containsKey('exposure'), isTrue);
    await tapKey(t, 'exposure-reset');
    expect(app.editOf(id).values.containsKey('exposure'), isFalse);
    // other photos keep their own stacks
    expect(app.canUndo(idOf(app, '花市')), isFalse);
  });

  testWidgets('tone grid selection sets toneTarget, header shows arrow', (t) async {
    final app = await open(t, phoneSize);
    final id = idOf(app, '外滩夕阳');
    await tapKey(t, 'tone-low-long');
    expect(app.editOf(id).toneTarget, 'low-long');
    expect(find.byKey(const ValueKey('hist-target')), findsOneWidget);
    expect(find.byKey(const ValueKey('dot-style')), findsOneWidget);
  });

  testWidgets('restore needs confirm and can be undone from the toast', (t) async {
    final app = await open(t, phoneSize);
    final id = idOf(app, '外滩夕阳'); // has demo edits
    expect(app.editOf(id).isAdjusted, isTrue);
    await tapKey(t, 'toolbar-more');
    await shot(t, 'editor_phone_menu');
    await tapKey(t, 'menu-restore');
    expect(find.text('还原为自动结果？'), findsWidgets);
    await tapKey(t, 'restore-cancel');
    expect(app.editOf(id).isAdjusted, isTrue);
    await tapKey(t, 'toolbar-more');
    await tapKey(t, 'menu-restore');
    await tapKey(t, 'restore-confirm');
    expect(app.editOf(id).isAdjusted, isFalse);
    expect(find.text('已还原为自动结果'), findsOneWidget);
    await tapKey(t, 'toast-action');
    expect(app.editOf(id).isAdjusted, isTrue);
  });

  testWidgets('copy, paste and sync settings', (t) async {
    final app = await open(t, phoneSize);
    final id = idOf(app, '外滩夕阳');
    final other = idOf(app, '雪山');
    await tapKey(t, 'toolbar-more');
    expect(app.hasClipboard, isFalse);
    await tapKey(t, 'menu-copy');
    expect(app.hasClipboard, isTrue);
    expect(find.text('已复制设置'), findsOneWidget);
    // sync
    await tapKey(t, 'toolbar-more');
    await tapKey(t, 'menu-sync');
    expect(find.text('同步设置'), findsOneWidget);
    expect(find.textContaining('同步到'), findsOneWidget);
    await tapKey(t, 'sync-cat-light');
    await tapKey(t, 'sync-confirm');
    expect(find.textContaining('已同步'), findsOneWidget);
    expect(app.editOf(other).values.containsKey('exposure'), isFalse); // light was unchecked
    expect(app.editOf(other).values.containsKey('contrast'), isFalse);
    await tapKey(t, 'toast-action');
    // paste back onto another edited photo keeps nothing of its own
    app.applyEdit(id, (e) => e.withValue('shadows', 40));
    await tapKey(t, 'toolbar-more');
    await tapKey(t, 'menu-paste');
    expect(app.editOf(id).values.containsKey('shadows'), isFalse);
    expect(find.text('已粘贴设置'), findsOneWidget);
    await tapKey(t, 'toast-action');
    expect(app.editOf(id).values['shadows'], 40);
  });

  testWidgets('sync merges the chosen categories into targets', (t) async {
    final app = await open(t, phoneSize);
    final id = idOf(app, '外滩夕阳');
    final other = idOf(app, '雪山');
    app.applyEdit(other, (e) => e.withValue('shadows', 25));
    await tapKey(t, 'toolbar-more');
    await tapKey(t, 'menu-sync');
    await tapKey(t, 'sync-confirm');
    final o = app.editOf(other);
    expect(o.values['exposure'], 12);
    expect(o.values['shadows'], 25); // merged, not overwritten
    expect(app.editOf(id).values['exposure'], 12);
  });

  testWidgets('prev / next cycle and compare toggle', (t) async {
    await open(t, phoneSize);
    await tapKey(t, 'compare-toggle');
    expect(find.byKey(const ValueKey('original-pill')), findsOneWidget);
    await shot(t, 'editor_phone_compare');
    await tapKey(t, 'next-photo');
    expect(find.byKey(const ValueKey('original-pill')), findsNothing);
    await tapKey(t, 'toolbar-more');
    await tapKey(t, 'menu-scrim');
  });

  testWidgets('sheet grabber toggles detents', (t) async {
    await open(t, phoneSize);
    final before = t.getRect(find.byKey(const ValueKey('sheet-grabber'))).top;
    await tapKey(t, 'sheet-grabber');
    final after = t.getRect(find.byKey(const ValueKey('sheet-grabber'))).top;
    expect(after, lessThan(before));
  });

  testWidgets('phone dark', (t) async {
    await open(t, phoneSize, dark: true);
    await shot(t, 'editor_phone_dark');
  });

  testWidgets('large: histogram pinned, two groups open, collapsible', (t) async {
    final app = await open(t, foldSize, title: '霓虹夜街');
    expect(find.byKey(const ValueKey('hist-nearest')), findsOneWidget);
    expect(find.byKey(const ValueKey('group-body-style')), findsOneWidget);
    expect(find.byKey(const ValueKey('group-body-light')), findsOneWidget);
    expect(find.byKey(const ValueKey('group-body-crop')), findsNothing);
    await tapKey(t, 'group-header-crop');
    expect(find.byKey(const ValueKey('group-body-crop')), findsOneWidget);
    expect(find.byKey(const ValueKey('group-body-style')), findsOneWidget);
    await tapKey(t, 'group-header-crop');
    // histogram stays while collapsing the style group
    await tapKey(t, 'group-header-style');
    expect(find.byKey(const ValueKey('group-body-style')), findsNothing);
    expect(find.byKey(const ValueKey('hist-nearest')), findsOneWidget);
    await tapKey(t, 'group-header-style');
    expect(find.byKey(const ValueKey('film-${''}')), findsNothing);
    expect(app.editable.isNotEmpty, isTrue);
    await shot(t, 'editor_fold_edit');
    await tapKey(t, 'sidebyside-toggle');
    await shot(t, 'editor_fold_compare');
  });

  testWidgets('large dark', (t) async {
    await open(t, foldSize, dark: true, title: '霓虹夜街');
    await shot(t, 'editor_fold_dark');
  });
}
