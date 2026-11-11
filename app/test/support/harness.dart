// Test harness: loads a CJK font (flutter_test renders Ahem boxes otherwise), pins the window
// size / safe areas, and saves screenshots under build/app_shots/.
import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:dapple/app.dart';
import 'package:dapple/state/app_state.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart' show FontLoader;
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';

bool _fontsLoaded = false;

/// Registers a CJK-capable font as "Roboto" (the default Android family). Reads a font from the
/// host OS purely for screenshots; nothing is bundled with the app.
Future<void> loadTestFonts() async {
  if (_fontsLoaded) return;
  const candidates = [
    r'C:\Windows\Fonts\NotoSansSC-VF.ttf',
    r'C:\Windows\Fonts\simhei.ttf',
    '/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc',
  ];
  for (final p in candidates) {
    final f = File(p);
    if (f.existsSync()) {
      final loader = FontLoader('Roboto')..addFont(Future.value(ByteData.view(f.readAsBytesSync().buffer)));
      await loader.load();
      break;
    }
  }
  _fontsLoaded = true;
}

const phoneSize = Size(412, 945);
const foldSize = Size(840, 760);

/// Sets the logical window size (dpr 2) and Android-like safe areas (status 36, gesture 24).
void useWindow(WidgetTester tester, Size size, {double top = 36, double bottom = 24}) {
  const dpr = 2.0;
  tester.view.devicePixelRatio = dpr;
  tester.view.physicalSize = size * dpr;
  tester.view.padding = FakeViewPadding(top: top * dpr, bottom: bottom * dpr);
  tester.view.viewPadding = FakeViewPadding(top: top * dpr, bottom: bottom * dpr);
  addTearDown(tester.view.reset);
}

final GlobalKey shotKey = GlobalKey();

Future<void> pumpDapple(WidgetTester tester, AppState app, {bool dark = false}) async {
  await loadTestFonts();
  await tester.pumpWidget(RepaintBoundary(
    key: shotKey,
    child: DappleApp(state: app, forceBrightness: dark ? Brightness.dark : Brightness.light),
  ));
  await tester.pump(const Duration(milliseconds: 100));
}

/// Writes build/app_shots/[name].png (relative to the app directory).
Future<void> shot(WidgetTester tester, String name) async {
  await tester.runAsync(() async {
    final boundary = shotKey.currentContext!.findRenderObject() as RenderRepaintBoundary;
    final image = await boundary.toImage(pixelRatio: 2.0);
    final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
    final f = File('build/app_shots/$name.png');
    f.parent.createSync(recursive: true);
    f.writeAsBytesSync(bytes!.buffer.asUint8List());
  });
}
