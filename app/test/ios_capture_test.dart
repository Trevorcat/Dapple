import 'package:dapple/platform/ios_capture.dart';
import 'package:dapple/services/capture.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const ch = MethodChannel('dapple/capture');
  final calls = <MethodCall>[];

  void handler(Future<Object?>? Function(MethodCall) f) {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger.setMockMethodCallHandler(ch, (c) async {
      calls.add(c);
      return f(c);
    });
  }

  setUp(calls.clear);
  tearDown(() => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger.setMockMethodCallHandler(ch, null));

  test('availability comes from the plugin; no handler (simulator, tests) means unavailable without throwing', () async {
    final none = IosCaptureBackend();
    expect(await none.refreshAvailability(), isFalse);
    expect(none.available, isFalse);
    handler((c) async => c.method == 'available' ? true : null);
    final b = IosCaptureBackend(probe: false);
    expect(b.available, isFalse);
    expect(await b.refreshAvailability(), isTrue);
    expect(b.available, isTrue);
  });

  test('permission maps granted / blocked / denied and passes the request flag', () async {
    final b = IosCaptureBackend(probe: false);
    for (final e in {'granted': CapturePerm.granted, 'blocked': CapturePerm.blocked, 'denied': CapturePerm.denied}.entries) {
      handler((c) async => e.key);
      expect(await b.permission(), e.value);
      expect(await b.permission(request: true), e.value);
      expect(calls.last.arguments, {'request': true});
    }
    handler((c) async => throw PlatformException(code: 'x'));
    expect(await b.permission(), CapturePerm.denied);
  });

  test('capture returns the DNG + JPEG pair, a DNG alone with its thumbnail, and null when closed', () async {
    final b = IosCaptureBackend(probe: false);
    handler((c) async => {'jpg': '/c/DAP_1.jpg', 'dng': '/c/DAP_1.dng', 'source': 'avfoundation', 'mode': 'raw+jpeg'});
    var r = (await b.capture())!;
    expect([r.jpegPath, r.dngPath, r.thumbPath, r.source, r.stem], ['/c/DAP_1.jpg', '/c/DAP_1.dng', null, 'avfoundation', 'DAP_1']);
    handler((c) async => {'dng': '/c/DAP_2.dng', 'thumb': '/c/DAP_2.thumb.jpg', 'source': 'avfoundation'});
    r = (await b.capture())!;
    expect([r.jpegPath, r.dngPath, r.thumbPath], [null, '/c/DAP_2.dng', '/c/DAP_2.thumb.jpg']);
    handler((c) async => null);
    expect(await b.capture(), isNull);
    handler((c) async => <String, dynamic>{'source': 'avfoundation'});
    expect(await b.capture(), isNull);
  });

  test('capture errors reach the caller (capture_flow shows its toast)', () async {
    final b = IosCaptureBackend(probe: false);
    handler((c) async => throw PlatformException(code: 'capture', message: 'no camera'));
    await expectLater(b.capture(), throwsA(isA<PlatformException>()));
  });

  test('capabilities and auto shutter go over the channel; failures are neutral', () async {
    final b = IosCaptureBackend(probe: false);
    handler((c) async => c.method == 'capabilities'
        ? {'device': 'iPhone10,3', 'lenses': [{'label': '1×', 'rawKind': 'bayer'}]}
        : null);
    final caps = await b.capabilities();
    expect((caps['lenses'] as List).single['rawKind'], 'bayer');
    await b.setAutoShutter(true, delayMs: 1234);
    expect(calls.last.method, 'autoShutter');
    expect(calls.last.arguments, {'on': true, 'delayMs': 1234});
    handler((c) async => throw PlatformException(code: 'x'));
    expect(await b.capabilities(), isEmpty);
    expect(await b.openSettings(), isFalse);
  });

  test('the gallery copy can be switched off for test shots', () async {
    final b = IosCaptureBackend(probe: false);
    IosCaptureBackend.saveToLibrary = false;
    await b.saveToGallery('/c/a.jpg', 'a.jpg', 'image/jpeg'); // would reach PhotoKit otherwise; here it must be a silent no-op
    expect(calls, isEmpty);
    IosCaptureBackend.saveToLibrary = true;
  });
}
