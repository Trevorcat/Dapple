// The HDR choice of the export reaches the Motion Photo cover (docs/engine/pipeline-architecture.md section 30.14): AppState.liveHdrCover asks the
// platform for an Ultra HDR cover of an HDR clip (with `force` = the export option), leaves SDR clips and every failure on the plain still.
import 'dart:convert';

import 'package:dapple/models.dart';
import 'package:dapple/services/live_edit.dart';
import 'package:dapple/state/app_state.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('dapple/live');
  final calls = <MethodCall>[];

  Photo livePhoto() => Photo(id: 'live:1', name: '20260101_000000_clip_MP.jpg', addedMs: 0, path: '/x/clip_MP.jpg', kind: PhotoKind.live)
    ..liveSrc = 'content://media/video/1'
    ..liveStartMs = 1000
    ..liveCoverMs = 2500
    ..liveSource = 0;

  void mock({required int colorKind, bool written = true, bool fail = false}) {
    calls.clear();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger.setMockMethodCallHandler(channel, (call) async {
      calls.add(call);
      if (call.method == 'probe') return <String, dynamic>{'colorKind': colorKind, 'decodable': true};
      if (call.method == 'coverHdr') {
        if (fail) throw PlatformException(code: 'boom');
        final a = call.arguments as Map;
        return <String, dynamic>{'path': a['out'], 'written': written, 'reason': written ? 0 : 1, 'peak': 2.5, 'gainMax': 1.2};
      }
      return null;
    });
  }

  tearDown(() => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger.setMockMethodCallHandler(channel, null));

  test('the HDR choice and the still\'s edit reach the platform for an HDR clip', () async {
    for (final kind in [2, 3]) {
      mock(colorKind: kind);
      final app = AppState();
      final wire = {'exposure': 0.3, 'crop': '1:1', 'rotate': 90};
      final r = await app.liveHdrCover(livePhoto(), still: '/e/still.jpg', wire: wire, le: 2048, coverMs: 2500, hdr: true, dest: '/e/cover.jpg');
      expect(r, '/e/cover.jpg');
      final c = calls.singleWhere((e) => e.method == 'coverHdr').arguments as Map;
      expect(c['force'], isTrue, reason: 'the export option reaches the engine');
      expect(c['sdr'], '/e/still.jpg');
      expect(c['out'], '/e/cover.jpg');
      expect(c['timeMs'], 2500);
      expect(c['uri'], 'content://media/video/1');
      final edit = c['edit'] as Map;
      expect(jsonDecode(edit['paramsJson'] as String), wire);
      expect(edit['refMs'], 2500);
      expect(edit['stillLongEdge'], 2048);
    }
  });

  test('an HDR clip gets its gain-map cover without the option too (the engine judges the headroom)', () async {
    mock(colorKind: 2);
    final r = await AppState().liveHdrCover(livePhoto(), still: '/e/s.jpg', wire: const {}, le: 0, coverMs: 1, hdr: false, dest: '/e/c.jpg');
    expect(r, '/e/c.jpg');
    expect((calls.singleWhere((e) => e.method == 'coverHdr').arguments as Map)['force'], isFalse);
  });

  test('the cover stays the SDR still: SDR clip, no headroom, engine error', () async {
    mock(colorKind: 0);
    expect(await AppState().liveHdrCover(livePhoto(), still: '/e/s.jpg', wire: const {}, le: 0, coverMs: 1, hdr: true, dest: '/e/c.jpg'), '/e/s.jpg');
    expect(calls.any((e) => e.method == 'coverHdr'), isFalse, reason: 'an SDR clip has nothing to put in a gain map');
    mock(colorKind: 2, written: false);
    expect(await AppState().liveHdrCover(livePhoto(), still: '/e/s.jpg', wire: const {}, le: 0, coverMs: 1, hdr: true, dest: '/e/c.jpg'), '/e/s.jpg');
    mock(colorKind: 3, fail: true);
    expect(await AppState().liveHdrCover(livePhoto(), still: '/e/s.jpg', wire: const {}, le: 0, coverMs: 1, hdr: true, dest: '/e/c.jpg'), '/e/s.jpg');
  });

  test('the policy: only HDR clips (HLG 2, PQ 3) can have a gain-map cover', () {
    for (final opt in [false, true]) {
      expect(liveCoverWantsHdr(hdrOption: opt, colorKind: 2), isTrue);
      expect(liveCoverWantsHdr(hdrOption: opt, colorKind: 3), isTrue);
      expect(liveCoverWantsHdr(hdrOption: opt, colorKind: 0), isFalse);
      expect(liveCoverWantsHdr(hdrOption: opt, colorKind: 4), isFalse);
    }
  });
}
