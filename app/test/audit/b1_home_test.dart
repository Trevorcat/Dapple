import 'package:dapple/models.dart';
import 'package:dapple/state/app_state.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import '../support/fake_usb.dart';
import '../support/harness.dart';
import 'audit_support.dart';

Photo ph(String id, PhotoStatus st, {String? error}) =>
    Photo(id: id, name: '$id.jpg', addedMs: DateTime(2026, 11, 11, 16).millisecondsSinceEpoch, status: st, error: error);

void main() {
  for (final dark in [false, true]) {
    final th = dark ? 'dark' : 'light';
    testWidgets('B1 home $th', (t) async {
      win(t, 'p');
      var app = AppState.demo(scenario: DemoScenario.empty);
      await pumpDapple(t, app, dark: dark);
      await audShot(t, '30_home_empty_p_$th');

      app = AppState.demo();
      await pumpDapple(t, app, dark: dark);
      await audShot(t, '31_home_normal_p_$th');

      app = AppState.demo(scenario: DemoScenario.disconnected);
      await pumpDapple(t, app, dark: dark);
      await audShot(t, '32_home_disconnected_p_$th');

      app = AppState.demo(scenario: DemoScenario.transferFailed);
      await pumpDapple(t, app, dark: dark);
      await audShot(t, '33_home_transferfailed_p_$th');

      app = AppState.demo(scenario: DemoScenario.empty);
      app.simulatePhotoForTest(ph('q', PhotoStatus.pending));
      app.simulatePhotoForTest(ph('d', PhotoStatus.developing));
      app.simulatePhotoForTest(ph('f', PhotoStatus.failed, error: 'PlatformException(convert, cannot convert IMG_1.HEIC to JPEG)'));
      app.simulatePhotoForTest(ph('g', PhotoStatus.failed, error: 'asset not found: ph://x'));
      app.simulatePhotoForTest(ph('n', PhotoStatus.failed, error: 'network: icloud download failed'));
      await pumpDapple(t, app, dark: dark);
      await t.pump(const Duration(milliseconds: 200));
      await audShot(t, '34_home_tiles_waiting_developing_failed_p_$th');

      app = AppState.demo();
      await pumpDapple(t, app, dark: dark);
      await t.tap(find.byKey(const ValueKey('import-button')));
      await t.pump();
      await t.pump(const Duration(milliseconds: 500));
      await audShot(t, '35_home_import_sheet_p_$th');
      await t.tapAt(const Offset(20, 20));
      await t.pump(const Duration(milliseconds: 500));

      await t.tap(find.byKey(const ValueKey('nav-1')));
      await t.pump(const Duration(milliseconds: 500));
      await audShot(t, '36_styles_tab_p_$th');
      await t.tap(find.byKey(const ValueKey('nav-2')));
      await t.pump(const Duration(milliseconds: 500));
      await audShot(t, '37_nav2_tab_p_$th');
    });

    testWidgets('B1 usb $th', (t) async {
      win(t, 'p');
      final cam = FakeUsbBackend();
      final app = AppState.demo()..usbBackend = cam;
      await app.initUsb(inboxPath: '/inbox');
      app.usb.isKnown = (f) => f.name == 'DSC00001.JPG';
      await pumpDapple(t, app, dark: dark);
      Future<void> st(String n) async {
        await t.pump(const Duration(milliseconds: 100));
        await audShot(t, '4${n}_usb_p_$th');
      }
      cam.emit({'type': 'state', 'state': 'permission'});
      await st('0_permission');
      cam.emit({'type': 'state', 'state': 'error', 'reason': 'permission_denied'});
      await st('1_denied');
      cam.ready();
      await st('2_ready');
      cam.emit({'type': 'newShots', 'groups': [camGroup('DSC00009', [camFile(9, 'DSC00009.JPG')])]});
      cam.emit({'type': 'importStart', 'name': 'DSC00009.JPG', 'key': 'usb:dsc00009.jpg|100|1700000000', 'size': 100});
      cam.emit({'type': 'importProgress', 'name': 'DSC00009.JPG', 'bytes': 40, 'size': 100});
      await st('3_receiving');
      cam.emit({'type': 'state', 'state': 'error', 'reason': 'io'});
      await st('4_error');
      cam.emit({'type': 'state', 'state': 'unplugged'});
      await st('5_unplugged');
      await t.pump(const Duration(seconds: 50));
    });
  }
}
