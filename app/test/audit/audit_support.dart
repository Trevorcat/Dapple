// The screenshot sweep (from the 2026-11 design audit, docs/design/audit-2026-11.md section G): every new surface in phone portrait
// (light and dark), phone landscape and the unfolded inner screen. A normal `flutter test` runs the flows as layout smoke tests (no
// overflow, no exception) without writing files; with `--dart-define=SHOTS=true` each step is also written to ../build/ui-audit/<name>.png
// (2x) for review:   flutter test test/audit --dart-define=SHOTS=true
import 'dart:io';
import 'dart:ui' as ui;

import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/screens/editor/local_controller.dart';
import 'package:dapple/screens/editor/local_layer.dart';
import 'package:dapple/services/local_ai.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';

import '../editor_v2_test.dart' show settle;
import '../support/harness.dart';

const sizes = <String, (Size, double, double)>{
  'p': (Size(412, 945), 36, 24),
  'l': (Size(945, 412), 0, 0),
  'f': (Size(840, 760), 36, 24),
  's': (Size(360, 780), 36, 24), // the narrowest common phone (Samsung 2 pass: header readout at 360 dp)
};

void win(WidgetTester t, String s) {
  final v = sizes[s]!;
  useWindow(t, v.$1, top: v.$2, bottom: v.$3);
}

/// Write the PNGs (else the sweep only checks that every step lays out without an exception).
const bool kShots = bool.fromEnvironment('SHOTS');

/// Where the PNGs go (--dart-define=OUT=... for the before / after pairs of build/ui-samsung2).
const String kOut = String.fromEnvironment('OUT', defaultValue: '../build/ui-audit');

Future<void> audShot(WidgetTester t, String name) async {
  await settle(t, 6);
  expect(t.takeException(), isNull, reason: name);
  if (!kShots) return;
  await t.runAsync(() async {
    final b = shotKey.currentContext!.findRenderObject() as RenderRepaintBoundary;
    final img = await b.toImage(pixelRatio: 2.0);
    final bytes = await img.toByteData(format: ui.ImageByteFormat.png);
    final f = File('$kOut/$name.png');
    f.parent.createSync(recursive: true);
    f.writeAsBytesSync(bytes!.buffer.asUint8List());
  });
}

/// The editor on a demo photo ([title]: 外滩夕阳 is 3:2 landscape, 街角咖啡 is 2:3 portrait).
Future<({AppState app, String id, LocalController? lc})> audOpen(WidgetTester t, String size, bool dark,
    {EGroup group = EGroup.local, LocalAi? ai, void Function(AppState, String)? before, String title = '外滩夕阳', AppState? useApp}) async {
  win(t, size);
  await loadTestFonts();
  final app = useApp ?? AppState.demo();
  final id = app.photos.firstWhere((p) => p.title == title && p.id.startsWith('d')).id;
  before?.call(app, id);
  await t.pumpWidget(RepaintBoundary(
    key: shotKey,
    child: MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: buildTheme(Brightness.light),
      darkTheme: buildTheme(Brightness.dark),
      themeMode: dark ? ThemeMode.dark : ThemeMode.light,
      home: EditorPage(app: app, photoId: id, resamplePointers: false, initialGroup: group, localAi: ai),
    ),
  ));
  await settle(t, 8);
  LocalController? lc;
  final f = find.byType(LocalPhotoLayer);
  if (f.evaluate().isNotEmpty) lc = t.widget<LocalPhotoLayer>(f).lc;
  return (app: app, id: id, lc: lc);
}
