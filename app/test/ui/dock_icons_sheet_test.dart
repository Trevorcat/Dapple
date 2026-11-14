// Review sheet of the dock's category icons and the top bar's crop icon (guidelines 8, 15.2.2): the five categories and 裁切 at 24, 32 and
// 96 dp, light and dark, each normal / selected / changed / greyed. Not a regression test: a plain `flutter test` only builds it.
//   flutter test test/ui/dock_icons_sheet_test.dart --dart-define=PAGES=true
// writes ../build/ui-dock/icons.png (2x).
import 'dart:io';
import 'dart:ui' as ui;

import 'package:dapple/model/params.dart';
import 'package:dapple/screens/editor/pad_dock.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:dapple/widgets/icons.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter_test/flutter_test.dart';

import '../support/harness.dart';

const bool kPages = bool.fromEnvironment('PAGES');

enum _St { normal, selected, changed, greyed }

Widget _sheet(Brightness b) => Theme(
      data: buildTheme(b),
      child: Builder(builder: (context) {
        final c = context.c;
        final items = [for (final g in padDockOrder) (dockIcon(g), dockLabel(g), g == EGroup.local), (DI.crop, '裁切', false)];
        Widget row(double size, _St st) => Row(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.center, children: [
              SizedBox(width: 96, child: Text('${size.round()} ${st.name}', style: T.mini(c.text2))),
              for (final (icon, label, local) in items)
                SizedBox(
                  width: size * 2.2 + 8,
                  height: size * 2.2 + 8,
                  child: Center(
                    child: SizedBox(
                      width: size * 2.2,
                      height: size * 56 / 24,
                      child: DockItemFace(
                        icon: icon,
                        label: label,
                        iconSize: size,
                        on: st == _St.selected,
                        dim: st == _St.greyed,
                        changed: st == _St.changed,
                        count: st == _St.changed && local ? 3 : null,
                      ),
                    ),
                  ),
                ),
            ]);
        return ColoredBox(
          color: c.bg,
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text(b == Brightness.light ? 'light' : 'dark', style: T.cap(c.text)),
              for (final s in const [24.0, 32.0, 96.0])
                for (final st in _St.values) row(s, st),
            ]),
          ),
        );
      }),
    );

void main() {
  testWidgets('dock icon sheet', (t) async {
    await loadTestFonts();
    useWindow(t, const Size(1460, 2960), top: 0, bottom: 0);
    await t.pumpWidget(RepaintBoundary(
      key: shotKey,
      child: MaterialApp(
        debugShowCheckedModeBanner: false,
        theme: buildTheme(Brightness.light),
        home: Material(
          child: SingleChildScrollView(
            child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [_sheet(Brightness.light), _sheet(Brightness.dark)]),
          ),
        ),
      ),
    ));
    await t.pump();
    expect(t.takeException(), isNull);
    if (!kPages) return;
    await t.runAsync(() async {
      final b = shotKey.currentContext!.findRenderObject() as RenderRepaintBoundary;
      final img = await b.toImage(pixelRatio: 1.0);
      final bytes = await img.toByteData(format: ui.ImageByteFormat.png);
      File('../build/ui-dock/icons.png')
        ..parent.createSync(recursive: true)
        ..writeAsBytesSync(bytes!.buffer.asUint8List());
    });
  });
}
