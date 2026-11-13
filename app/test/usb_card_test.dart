import 'package:dapple/platform/usb_camera.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/widgets/controls.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'support/fake_usb.dart';
import 'support/harness.dart';

void main() {
  Future<(AppState, FakeUsbBackend)> open(WidgetTester tester) async {
    useWindow(tester, phoneSize);
    final cam = FakeUsbBackend();
    final app = AppState.demo()..usbBackend = cam;
    await app.initUsb(inboxPath: '/inbox');
    app.usb.isKnown = (f) => f.name == 'DSC00001.JPG';
    await pumpDapple(tester, app);
    return (app, cam);
  }

  testWidgets('connection card follows the cable: permission, connected, receiving, unplugged', (tester) async {
    final (app, cam) = await open(tester);
    expect(find.text('Sony α7R V · 已连接'), findsOneWidget, reason: 'demo card until a camera is plugged in');

    cam.emit({'type': 'state', 'state': 'permission'});
    await tester.pump();
    expect(find.text('相机已插入'), findsOneWidget);
    expect(find.text('等待允许访问'), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey('conn-action')));
    await tester.pump();
    expect(cam.permissionAsks, 1);

    cam.ready();
    await tester.pump();
    expect(find.text('Sony ILCE-7RM5 · 已连接'), findsOneWidget);
    expect(find.text('导入照片…'), findsOneWidget);
    await shot(tester, 'usb_card_ready');

    cam.emit({'type': 'newShots', 'groups': [camGroup('DSC00009', [camFile(9, 'DSC00009.JPG')])]});
    cam.emit({'type': 'importStart', 'name': 'DSC00009.JPG', 'key': 'usb:dsc00009.jpg|100|1700000000', 'size': 100});
    cam.emit({'type': 'importProgress', 'name': 'DSC00009.JPG', 'bytes': 40, 'size': 100});
    await tester.pump();
    expect(find.text('正在接收第 1 张 · 共 1 张'), findsOneWidget);
    expect(find.descendant(of: find.byKey(const ValueKey('conn-card')), matching: find.byType(DProgress)), findsOneWidget);

    cam.emit({'type': 'state', 'state': 'unplugged'});
    await tester.pump();
    expect(find.text('Sony ILCE-7RM5 · 已拔出'), findsOneWidget);
    expect(find.byKey(const ValueKey('conn-action')), findsNothing);
    expect(app.connState.name, 'disconnected');
    await tester.pump(const Duration(seconds: 50));
  });

  testWidgets('picker: lazy card list, known photos locked, import only what is picked, diagnostics copy', (tester) async {
    final (app, cam) = await open(tester);
    cam.card = [
      UsbGroup.fromMap(camGroup('DSC00003', [camFile(3, 'DSC00003.JPG'), camFile(2, 'DSC00003.ARW', size: 900)])),
      UsbGroup.fromMap(camGroup('DSC00002', [camFile(4, 'DSC00002.JPG')])),
      UsbGroup.fromMap(camGroup('DSC00001', [camFile(1, 'DSC00001.JPG')])), // already in the library
    ];
    String? copied;
    tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(SystemChannels.platform, (call) async {
      if (call.method == 'Clipboard.setData') copied = (call.arguments as Map)['text'] as String?;
      return null;
    });
    addTearDown(() => tester.binding.defaultBinaryMessenger.setMockMethodCallHandler(SystemChannels.platform, null));

    cam.ready();
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey('conn-card-tap')));
    await tester.pumpAndSettle();
    expect(find.byKey(const ValueKey('usb-picker')), findsOneWidget);
    expect(find.byKey(const ValueKey('usb-tile-100msdcf/dsc00003')), findsOneWidget);
    expect(find.text('已导入'), findsOneWidget);
    expect(find.text('视频暂不导入'), findsOneWidget);
    expect(cam.thumbRequests, containsAll([3, 4]), reason: 'JPEG of each exposure serves the thumbnail');
    expect(cam.imports, isEmpty, reason: 'nothing is imported without a pick');
    await shot(tester, 'usb_picker');

    await tester.tap(find.byKey(const ValueKey('usb-tile-100msdcf/dsc00001'))); // known: not selectable
    await tester.pump();
    expect(find.text('选择照片'), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey('usb-tile-100msdcf/dsc00003')));
    await tester.pump();
    expect(find.text('导入 1 张'), findsOneWidget);

    await tester.tap(find.text('复制诊断信息'));
    await tester.pump();
    expect(copied, 'DIAG TEXT');

    await tester.tap(find.byKey(const ValueKey('usb-import')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 600));
    expect(cam.imports.single, [3, 2]);
    expect(find.byKey(const ValueKey('usb-picker')), findsNothing);
    expect(app.usb.queuedPhotos, 1);
  });

  testWidgets('permission refused: the card offers to ask again', (tester) async {
    final (app, cam) = await open(tester);
    cam.emit({'type': 'state', 'state': 'error', 'reason': 'permission_denied'});
    await tester.pump();
    expect(find.text('未允许访问相机'), findsOneWidget);
    await tester.tap(find.text('允许'));
    await tester.pump();
    expect(cam.permissionAsks, 1);
    expect(app.usbCardActive, isTrue);
  });
}

