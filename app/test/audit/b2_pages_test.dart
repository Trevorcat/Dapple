import 'package:dapple/models.dart';
import 'package:dapple/screens/library/licenses_page.dart';
import 'package:dapple/screens/library/settings_page.dart';
import 'package:dapple/screens/publish/publish_page.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import '../support/fake_usb.dart';
import '../support/harness.dart';
import 'audit_support.dart';
import 'package:dapple/platform/usb_camera.dart';

class _FailingCard extends FakeUsbBackend {
  @override
  Future<UsbPage> list({int cursor = 0, int limit = 60, bool refresh = false}) async => throw StateError('card read failed');
}

class _App extends AppState {
  _App() : super.demo();
  @override
  Future<double> hdrHeadroomOf(Photo ph) async => 2.0;
  @override
  Future<double> wideGamutOf(Photo ph) async => 0.2;
}

Widget themed(bool dark, Widget home) => RepaintBoundary(
    key: shotKey,
    child: MaterialApp(
        debugShowCheckedModeBanner: false,
        theme: buildTheme(Brightness.light),
        darkTheme: buildTheme(Brightness.dark),
        themeMode: dark ? ThemeMode.dark : ThemeMode.light,
        home: home));

void main() {
  for (final dark in [false, true]) {
    final th = dark ? 'dark' : 'light';
    testWidgets('B2 picker $th', (t) async {
      win(t, 'p');
      await loadTestFonts();
      final cam = FakeUsbBackend();
      final app = AppState.demo()..usbBackend = cam;
      await app.initUsb(inboxPath: '/inbox');
      app.usb.isKnown = (f) => f.name == 'DSC00001.JPG';
      cam.card = [
        for (var i = 20; i > 3; i--) UsbGroup.fromMap(camGroup('DSC000$i', [camFile(i, 'DSC000$i.JPG'), camFile(100 + i, 'DSC000$i.ARW', size: 900)])),
        UsbGroup.fromMap(camGroup('DSC00001', [camFile(1, 'DSC00001.JPG')])),
      ];
      await pumpDapple(t, app, dark: dark);
      cam.ready();
      await t.pump();
      await t.tap(find.byKey(const ValueKey('conn-card-tap')));
      await t.pumpAndSettle();
      await audShot(t, '50_usb_picker_p_$th');
      await t.tap(find.byKey(const ValueKey('usb-tile-100msdcf/dsc00020')));
      await t.pump();
      await t.tap(find.byKey(const ValueKey('usb-tile-100msdcf/dsc00019')));
      await t.pump();
      await audShot(t, '51_usb_picker_selected_p_$th');
    });

    testWidgets('B2 picker failed $th', (t) async {
      win(t, 'p');
      await loadTestFonts();
      final cam = _FailingCard();
      final app = AppState.demo()..usbBackend = cam;
      await app.initUsb(inboxPath: '/inbox');
      await pumpDapple(t, app, dark: dark);
      cam.ready();
      await t.pump();
      await t.tap(find.byKey(const ValueKey('conn-card-tap')));
      await t.pumpAndSettle();
      expect(find.byKey(const ValueKey('usb-retry')), findsOneWidget, reason: 'an empty list that failed offers 重试 (audit F04)');
      await audShot(t, '52_usb_picker_failed_p_$th');
    });

    for (final s in ['p', 'l', 'f']) {
      if (s != 'p' && dark) continue;
      testWidgets('B3 publish $s $th', (t) async {
        win(t, s);
        await loadTestFonts();
        final app = _App();
        final ids = app.editable.where((p) => p.status == PhotoStatus.done).take(3).map((e) => e.id).toList();
        await t.pumpWidget(themed(
            dark,
            Scaffold(
                body: Center(
                    child: Builder(
                        builder: (ctx) => TextButton(key: const ValueKey('host-open'), onPressed: () => openPublish(ctx, app, ids), child: const Text('open')))))));
        await t.tap(find.byKey(const ValueKey('host-open')));
        await t.pump();
        await t.pump(const Duration(milliseconds: 800));
        await audShot(t, '60_publish_${s}_$th');
        final sw = find.byKey(const ValueKey('switch-hdr'));
        if (sw.evaluate().isNotEmpty) {
          await t.ensureVisible(sw);
          await t.tap(sw);
          await t.pump(const Duration(milliseconds: 300));
          await audShot(t, '61_publish_hdr_on_${s}_$th');
        }
        final xhs = find.byKey(const ValueKey('preset-xhs'));
        if (xhs.evaluate().isNotEmpty) {
          await t.tap(xhs);
          await t.pump(const Duration(milliseconds: 300));
          await audShot(t, '62_publish_preset_xhs_${s}_$th');
        }
      });
    }

    testWidgets('B4 settings $th', (t) async {
      win(t, 'p');
      await loadTestFonts();
      final app = AppState.demo();
      await t.pumpWidget(themed(dark, Scaffold(body: SettingsPage(app: app, wide: false))));
      await t.pump(const Duration(milliseconds: 300));
      await audShot(t, '70_settings_p_$th');
      final sc = find.byType(Scrollable).first;
      await t.drag(sc, const Offset(0, -700));
      await t.pump(const Duration(milliseconds: 300));
      await audShot(t, '71_settings_scrolled_p_$th');
      await t.drag(sc, const Offset(0, -900));
      await t.pump(const Duration(milliseconds: 300));
      await audShot(t, '72_settings_end_p_$th');
    });

    testWidgets('B4 licences $th', (t) async {
      win(t, 'p');
      await loadTestFonts();
      final app = AppState.demo();
      await t.pumpWidget(themed(dark, LicensesPage(app: app)));
      await t.pump(const Duration(milliseconds: 500));
      await audShot(t, '73_licences_p_$th');
    });
  }
}
