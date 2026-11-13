import 'dart:io';

import 'package:dapple/editor/panel_layout.dart';
import 'package:dapple/platform/test_trigger.dart';
import 'package:dapple/services/media_permission.dart';
import 'package:dapple/screens/library/settings_page.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:permission_handler/permission_handler.dart';

import 'support/harness.dart';

const _t = Duration(milliseconds: 100);

AppState _real({required List<PermissionStatus> statuses, required PermissionStatus Function() onRequest, void Function()? onSettings}) {
  var i = 0;
  final app = AppState()
    ..ready = true
    ..permissionPlatform = true
    ..mediaPerm = MediaPermission(
      sdkInt: () async => 34,
      status: (p) async => statuses[i < statuses.length ? i++ : statuses.length - 1],
      request: (p) async => onRequest(),
      openSettings: () async {
        onSettings?.call();
        return true;
      },
    );
  return app;
}

void main() {
  group('permission flow', () {
    test('access levels', () {
      expect(MediaPermission.accessOfStatus(PermissionStatus.granted), MediaAccess.full);
      expect(MediaPermission.accessOfStatus(PermissionStatus.limited), MediaAccess.limited);
      expect(MediaPermission.accessOfStatus(PermissionStatus.denied), MediaAccess.askable);
      expect(MediaPermission.accessOfStatus(PermissionStatus.permanentlyDenied), MediaAccess.blocked);
      expect(MediaPermission.accessOfIos('notDetermined'), MediaAccess.askable);
      expect(MediaPermission.accessOfIos('denied'), MediaAccess.blocked);
      expect(MediaPermission.accessOfIos('restricted'), MediaAccess.blocked);
      expect(MediaPermission.accessOfIos('limited'), MediaAccess.limited);
      expect(MediaPermission.accessOfIos('authorized'), MediaAccess.full);
    });

    test('first ask: prompt, granted -> the card is gone at once', () async {
      final app = _real(statuses: [PermissionStatus.denied], onRequest: () => PermissionStatus.granted)..mediaPermitted = false;
      expect(app.connAction, '允许访问');
      await app.connectAction();
      expect(app.mediaPermitted, isTrue);
      expect(app.needsMediaPermission, isFalse);
      expect(app.connAction, isNot('允许访问'));
    });

    test('refused: the button becomes 打开设置 and opens Settings; resuming re-reads the state', () async {
      var opened = 0;
      final app = _real(
        statuses: [PermissionStatus.permanentlyDenied, PermissionStatus.granted],
        onRequest: () => PermissionStatus.permanentlyDenied,
        onSettings: () => opened++,
      )..mediaPermitted = false;
      await app.refreshPermission();
      expect(app.mediaBlocked, isTrue);
      expect(app.connAction, '打开设置');
      expect(app.connLine, '在系统设置里允许 Dapple 访问照片');
      await app.connectAction();
      expect(opened, 1);
      expect(app.mediaPermitted, isFalse);
      await app.refreshPermission(); // what the resume handler does after the user came back from Settings
      expect(app.mediaPermitted, isTrue);
      expect(app.needsMediaPermission, isFalse);
    });

    test('a refusal at the prompt turns the next tap into Settings', () async {
      final app = _real(statuses: [PermissionStatus.denied], onRequest: () => PermissionStatus.permanentlyDenied)..mediaPermitted = false;
      await app.connectAction();
      expect(app.connAction, '打开设置');
    });

    test('limited access lists photos and offers more', () async {
      final app = _real(statuses: [PermissionStatus.limited], onRequest: () => PermissionStatus.limited)..mediaPermitted = false;
      await app.refreshPermission();
      expect(app.mediaPermitted, isTrue);
      expect(app.mediaLimited, isTrue);
    });
  });

  testWidgets('empty after allowing: says no new photos in 7 days and offers the picker', (tester) async {
    useWindow(tester, phoneSize);
    final app = AppState()
      ..ready = true
      ..mediaPermitted = true
      ..scanDone = true;
    await pumpDapple(tester, app);
    expect(find.text('最近 7 天没有新照片'), findsOneWidget);
    expect(find.text('更早的照片可以从相册选择'), findsOneWidget);
    expect(find.byKey(const ValueKey('empty-import')), findsOneWidget);
    await shot(tester, 'home_empty_7days');
  });

  testWidgets('styles page shows previews with no photo in the library', (tester) async {
    useWindow(tester, phoneSize);
    final app = AppState()
      ..ready = true
      ..tab = 1;
    await pumpDapple(tester, app);
    await tester.pump(_t);
    expect(find.textContaining('预览为示例画面'), findsOneWidget);
    expect(find.byKey(const ValueKey('base-modern_clean')), findsOneWidget);
    expect(find.byType(Image), findsWidgets); // the bundled sample scene, not a flat placeholder
    await shot(tester, 'styles_no_photo');
  });

  group('hand side setting', () {
    test('rule', () {
      expect(sidePaneFits(559), isFalse);
      expect(sidePaneFits(560), isTrue);
      expect(handSideMatters(const Size(412, 945)), isTrue); // slab phone: landscape is 945 wide
      expect(handSideMatters(const Size(360, 640)), isTrue);
      expect(handSideMatters(const Size(840, 760)), isTrue);
      expect(handSideMatters(const Size(400, 500)), isFalse); // narrow both ways
    });

    testWidgets('hidden where no side panel can ever appear, shown otherwise', (tester) async {
      Future<AppState> open(Size size) async {
        useWindow(tester, size);
        await loadTestFonts();
        final app = AppState.demo();
        await tester.pumpWidget(MaterialApp(theme: buildTheme(Brightness.light), home: Scaffold(body: SettingsPage(app: app, wide: false))));
        await tester.pump(_t);
        return app;
      }

      await open(phoneSize);
      expect(find.byKey(const ValueKey('seg-handedness')), findsOneWidget);
      await open(const Size(400, 500));
      expect(find.byKey(const ValueKey('seg-handedness')), findsNothing);
      expect(find.byKey(const ValueKey('reset-hand')), findsNothing);
    });
  });

  group('test-run trigger files are one-shot', () {
    late Directory dir;
    setUp(() => dir = Directory.systemTemp.createTempSync('trig'));
    tearDown(() => dir.deleteSync(recursive: true));

    test('fresh file: consumed once and deleted', () {
      final f = File('${dir.path}/DAPPLE_OPEN')..writeAsStringSync('style');
      expect(TestTrigger.consume(f), 'style');
      expect(f.existsSync(), isFalse);
      expect(TestTrigger.consume(f), isNull);
    });

    test('stale file from an earlier session: ignored and removed, the launch stays a normal one', () {
      final f = File('${dir.path}/autotest.json')..writeAsStringSync('{}');
      f.setLastModifiedSync(DateTime.now().subtract(const Duration(hours: 5)));
      expect(TestTrigger.consume(f), isNull);
      expect(f.existsSync(), isFalse);
    });

    test('absent file: nothing', () {
      expect(TestTrigger.consume(File('${dir.path}/none')), isNull);
    });
  });
}
