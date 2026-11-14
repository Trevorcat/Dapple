// System back in the editor (guidelines 15.2.8): at most one transient layer per press; zoom is not a layer; with nothing open one back
// leaves the editor, and the grid thumbnail commit is not repeated per press.
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/screens/editor/zoom_view.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/model/params.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'support/harness.dart';

const _phys = Size(1080, 2400);
const _dpr = 2.75;

Future<AppState> _openOnTopOfHome(WidgetTester t) async {
  t.view.devicePixelRatio = _dpr;
  t.view.physicalSize = _phys;
  t.view.padding = FakeViewPadding(top: 28 * _dpr, bottom: 20 * _dpr);
  t.view.viewPadding = FakeViewPadding(top: 28 * _dpr, bottom: 20 * _dpr);
  addTearDown(t.view.reset);
  await loadTestFonts();
  final app = AppState.demo();
  final id = app.photos.firstWhere((p) => p.title == '外滩夕阳' && p.id.startsWith('d')).id;
  late BuildContext home;
  await t.pumpWidget(MaterialApp(
    theme: buildTheme(Brightness.light),
    home: Builder(builder: (c) {
      home = c;
      return const Scaffold(body: Text('home'));
    }),
  ));
  Navigator.of(home).push(MaterialPageRoute<void>(builder: (_) => EditorPage(app: app, photoId: id, resamplePointers: false)));
  await _settle(t);
  return app;
}

Future<void> _settle(WidgetTester t) async {
  for (var i = 0; i < 8; i++) {
    await t.pump(const Duration(milliseconds: 100));
  }
}

Future<void> _back(WidgetTester t) async {
  await t.binding.handlePopRoute();
  await _settle(t);
}

bool _inEditor() => find.byType(EditorPage).evaluate().isNotEmpty;
ZoomController _zoom(WidgetTester t) => t.widget<ZoomView>(find.byKey(const ValueKey('zoom-stage'))).controller;
Future<void> _dock(WidgetTester t, EGroup g) async {
  await t.tap(find.byKey(ValueKey('dock-$g')));
  await _settle(t);
}

void main() {
  testWidgets('a tap on the photo never takes the back button away; it still leaves', (t) async {
    await _openOnTopOfHome(t);
    await t.tapAt(t.getCenter(find.byKey(const ValueKey('zoom-stage'))));
    await t.pump(const Duration(milliseconds: 600));
    await _settle(t);
    final back = find.byKey(const ValueKey('toolbar-back'));
    expect(back.hitTestable(), findsOneWidget);
    // no ancestor hides it or ignores pointers
    final ignoring = find.ancestor(of: back, matching: find.byWidgetPredicate((w) => w is IgnorePointer && w.ignoring));
    expect(ignoring, findsNothing);
    final faded = find.ancestor(of: back, matching: find.byWidgetPredicate((w) => w is AnimatedOpacity && w.opacity < 1));
    expect(faded, findsNothing);
    await t.tap(back);
    await _settle(t);
    expect(_inEditor(), isFalse);
  });

  testWidgets('nothing open: one back leaves the editor', (t) async {
    await _openOnTopOfHome(t);
    expect(_inEditor(), isTrue);
    await _back(t);
    expect(_inEditor(), isFalse);
    expect(find.text('home'), findsOneWidget);
  });

  testWidgets('zoomed with nothing open: zoom is not a layer, one back leaves', (t) async {
    await _openOnTopOfHome(t);
    _zoom(t).zoomTo(3);
    await _settle(t);
    expect(_zoom(t).zoomed, isTrue);
    await _back(t);
    expect(_inEditor(), isFalse);
  });

  testWidgets('a tool open: back closes the tool only, the next back leaves', (t) async {
    await _openOnTopOfHome(t);
    await _dock(t, EGroup.light);
    expect(find.byKey(const ValueKey('docked-panel')), findsOneWidget);
    await _back(t);
    expect(_inEditor(), isTrue);
    expect(find.byKey(const ValueKey('docked-panel')), findsNothing);
    await _back(t);
    expect(_inEditor(), isFalse);
  });

  testWidgets('zoomed with a tool open: tool closes, zoom stays, then leave (two backs)', (t) async {
    await _openOnTopOfHome(t);
    await _dock(t, EGroup.color);
    _zoom(t).zoomTo(3);
    await _settle(t);
    await _back(t);
    expect(_inEditor(), isTrue);
    expect(find.byKey(const ValueKey('docked-panel')), findsNothing);
    expect(_zoom(t).zoomed, isTrue, reason: 'back never only resets zoom');
    await _back(t);
    expect(_inEditor(), isFalse);
  });

  testWidgets('itemised view in a tool: itemised closes, tool closes, then leave (three backs)', (t) async {
    await _openOnTopOfHome(t);
    await _dock(t, EGroup.color);
    await t.tap(find.byKey(const ValueKey('items-button')));
    await _settle(t);
    expect(find.byKey(const ValueKey('radar')), findsOneWidget);
    await _back(t);
    expect(find.byKey(const ValueKey('radar')), findsNothing);
    expect(find.byKey(const ValueKey('docked-panel')), findsOneWidget);
    expect(_inEditor(), isTrue);
    await _back(t);
    expect(find.byKey(const ValueKey('docked-panel')), findsNothing);
    expect(_inEditor(), isTrue);
    await _back(t);
    expect(_inEditor(), isFalse);
  });

  testWidgets('the grid thumbnail commit does not run on a back that only closes a layer', (t) async {
    final app = await _openOnTopOfHome(t);
    var commits = 0;
    app.addListener(() {}); // keep the app alive
    await _dock(t, EGroup.light);
    final before = app.commitCount;
    await _back(t); // closes the tool, editor stays
    commits = app.commitCount - before;
    expect(commits, 0);
    await _back(t); // leaves: one commit
    await t.pump();
    expect(app.commitCount - before, 1);
  });
}
