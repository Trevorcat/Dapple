import 'dart:io';

import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/services/capture.dart';
import 'package:dapple/state/app_state.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:path/path.dart' as p;

import 'support/harness.dart';

class FakeCapture implements CaptureBackend {
  FakeCapture({this.perm = CapturePerm.granted, this.grantOnRequest = false, this.result, this.fails = false});
  CapturePerm perm;
  bool grantOnRequest;
  CaptureResult? result;
  bool fails;
  int captures = 0, requests = 0, settings = 0;
  final gallery = <String>[];

  @override
  bool get available => true;
  @override
  Future<CapturePerm> permission({bool request = false}) async {
    if (request) {
      requests++;
      if (grantOnRequest) perm = CapturePerm.granted;
    }
    return perm;
  }

  @override
  Future<bool> openSettings() async {
    settings++;
    return true;
  }

  @override
  Future<CaptureResult?> capture() async {
    captures++;
    if (fails) throw Exception('no camera');
    return result;
  }

  @override
  Future<void> saveToGallery(String path, String name, String mime) async => gallery.add('$name|$mime');
}

Directory _tmp() => Directory.systemTemp.createTempSync('dapple_cap_');

CaptureResult _files(Directory d, {bool dng = true, String stem = 'DAP_20261112_131500'}) {
  final j = File(p.join(d.path, '$stem.jpg'))..writeAsBytesSync([0xFF, 0xD8, 1, 2, 3]);
  String? r;
  if (dng) {
    r = File(p.join(d.path, '$stem.dng')).path;
    File(r).writeAsBytesSync([0x49, 0x49, 0x2A, 0x00, 9]);
  }
  return CaptureResult(jpegPath: j.path, dngPath: r);
}

Future<AppState> _open(WidgetTester tester, FakeCapture fake, {Size size = phoneSize, DemoScenario sc = DemoScenario.normal}) async {
  useWindow(tester, size);
  final app = AppState.demo(scenario: sc)..captureBackend = fake;
  await pumpDapple(tester, app);
  return app;
}

void main() {
  group('home button', () {
    testWidgets('phone and fold, with and without photos, hidden while selecting', (tester) async {
      for (final size in [phoneSize, foldSize]) {
        for (final sc in [DemoScenario.normal, DemoScenario.empty]) {
          await _open(tester, FakeCapture(), size: size, sc: sc);
          expect(find.byKey(const ValueKey('capture-button')), findsOneWidget, reason: '$size $sc');
          expect(find.bySemanticsLabel('拍照'), findsOneWidget);
        }
      }
      final app = await _open(tester, FakeCapture());
      app.enterSelect();
      await tester.pump(const Duration(milliseconds: 100));
      expect(find.byKey(const ValueKey('capture-button')), findsNothing);
    });

    testWidgets('denied: toast with retry, which asks again and then captures', (tester) async {
      final tmp = _tmp();
      addTearDown(() => tmp.deleteSync(recursive: true));
      final fake = FakeCapture(perm: CapturePerm.denied, grantOnRequest: false);
      final app = await _open(tester, fake);
      app.captureInboxOverride = Directory(p.join(tmp.path, 'inbox'));
      await tester.tap(find.byKey(const ValueKey('capture-button')));
      await tester.pump(const Duration(milliseconds: 100));
      expect(fake.requests, 1);
      expect(fake.captures, 0);
      expect(app.toast?.message, '没有相机权限');
      expect(app.toast?.actionLabel, '重试');
      // the user grants it, then taps the toast action: capture starts
      fake.grantOnRequest = true;
      fake.result = null; // closed without a shot
      app.toast!.onAction!();
      await tester.pump(const Duration(milliseconds: 100));
      expect(fake.captures, 1);
      expect(find.byType(EditorPage), findsNothing);
    });

    testWidgets('blocked: toast offers system settings, no capture', (tester) async {
      final fake = FakeCapture(perm: CapturePerm.blocked);
      final app = await _open(tester, fake);
      await tester.tap(find.byKey(const ValueKey('capture-button')));
      await tester.pump(const Duration(milliseconds: 100));
      expect(fake.requests, 0);
      expect(fake.captures, 0);
      expect(app.toast?.actionLabel, '设置');
      app.toast!.onAction!();
      expect(fake.settings, 1);
    });

    testWidgets('camera cannot open: toast, nothing ingested', (tester) async {
      final fake = FakeCapture(fails: true);
      final app = await _open(tester, fake);
      final before = app.allPhotos.length;
      await tester.tap(find.byKey(const ValueKey('capture-button')));
      await tester.pump(const Duration(milliseconds: 100));
      expect(app.toast?.message, '无法打开相机');
      expect(app.allPhotos.length, before);
    });

    testWidgets('granted: shot is ingested and the editor opens on it', (tester) async {
      final tmp = _tmp();
      addTearDown(() => tmp.deleteSync(recursive: true));
      final fake = FakeCapture()..result = _files(tmp);
      final app = await _open(tester, fake);
      app.captureInboxOverride = Directory(p.join(tmp.path, 'inbox'));
      final before = app.allPhotos.length;
      await tester.runAsync(() async {
        await tester.tap(find.byKey(const ValueKey('capture-button')));
        await Future<void>.delayed(const Duration(milliseconds: 200));
      });
      for (var i = 0; i < 4; i++) {
        await tester.pump(const Duration(milliseconds: 300));
      }
      expect(app.allPhotos.length, before + 1);
      expect(find.byType(EditorPage), findsOneWidget);
    });
  });

  group('capture -> library', () {
    late Directory tmp;
    late AppState app;
    late FakeCapture fake;
    setUp(() {
      tmp = _tmp();
      fake = FakeCapture();
      app = AppState.demo(scenario: DemoScenario.empty)
        ..captureBackend = fake
        ..captureInboxOverride = Directory(p.join(tmp.path, 'inbox'));
    });
    tearDown(() {
      try {
        tmp.deleteSync(recursive: true);
      } catch (_) {} // the develop queue may still hold the file
    });

    test('RAW + JPEG: the DNG is the source, the JPEG its preview, both in the gallery', () async {
      final src = Directory(p.join(tmp.path, 'cache'))..createSync();
      final ph = await app.ingestCapture(_files(src));
      expect(ph.id, 'cap:DAP_20261112_131500');
      expect(ph.name, 'DAP_20261112_131500.dng');
      expect(ph.isRaw, isTrue);
      expect(p.extension(ph.path!), '.dng');
      expect(ph.pairedJpeg, endsWith('.jpg'));
      expect(ph.previewPath, ph.pairedJpeg);
      expect(File(ph.path!).existsSync() && File(ph.pairedJpeg!).existsSync(), isTrue);
      expect(src.listSync(), isEmpty, reason: 'moved out of the capture cache');
      expect(ph.takenMs, isNotNull);
      expect(app.photoById(ph.id), same(ph));
      await Future<void>.delayed(Duration.zero);
      expect(fake.gallery, ['DAP_20261112_131500.jpg|image/jpeg', 'DAP_20261112_131500.dng|image/x-adobe-dng']);
    });

    test('JPEG only (no RAW lens / system camera fallback): the JPEG is the source', () async {
      final src = Directory(p.join(tmp.path, 'cache'))..createSync();
      final ph = await app.ingestCapture(_files(src, dng: false, stem: 'SYS_1'));
      expect(ph.name, 'SYS_1.jpg');
      expect(ph.isRaw, isFalse);
      expect(ph.pairedJpeg, isNull);
      expect(ph.path, ph.previewPath);
      await Future<void>.delayed(Duration.zero);
      expect(fake.gallery, ['SYS_1.jpg|image/jpeg']);
    });

    test('RAW only (no camera JPEG): the DNG alone is the source, no preview yet, only the DNG goes to the gallery at once', () async {
      final src = Directory(p.join(tmp.path, 'cache'))..createSync();
      final dng = File(p.join(src.path, 'DAP_20261112_131501.dng'))..writeAsBytesSync([0x49, 0x49, 0x2A, 0x00, 9]);
      final r = CaptureResult(dngPath: dng.path);
      expect(r.stem, 'DAP_20261112_131501');
      final ph = await app.ingestCapture(r);
      expect(ph.id, 'cap:DAP_20261112_131501');
      expect(ph.name, 'DAP_20261112_131501.dng');
      expect(ph.isRaw, isTrue);
      expect(ph.pairedJpeg, isNull);
      expect(ph.previewPath, isNull, reason: 'the thumbnail comes from the engine develop of the DNG');
      expect(File(ph.path!).existsSync(), isTrue);
      expect(src.listSync(), isEmpty);
      expect(ph.openable, isTrue);
      expect(app.galleryJpegPending, contains(ph.id), reason: 'the developed JPEG follows the develop');
      await Future<void>.delayed(Duration.zero);
      expect(fake.gallery, ['DAP_20261112_131501.dng|image/x-adobe-dng']);
    });

    test('a capture with a camera JPEG needs no developed gallery copy', () async {
      final src = Directory(p.join(tmp.path, 'cache'))..createSync();
      final ph = await app.ingestCapture(_files(src));
      expect(app.galleryJpegPending, isNot(contains(ph.id)));
    });

    test('goes through the same develop queue as camera arrivals (pending until the engine runs)', () async {
      final src = Directory(p.join(tmp.path, 'cache'))..createSync();
      final ph = await app.ingestCapture(_files(src));
      expect(['pending', 'developing'], contains(ph.status.name));
      expect(ph.openable, isTrue);
      expect(app.isEmpty, isFalse);
    });
  });
}
