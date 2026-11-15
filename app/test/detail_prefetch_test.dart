import 'dart:convert';
import 'dart:typed_data';

import 'package:dapple/model/local_adjust.dart';
import 'package:dapple/model/wire_json.dart';
import 'package:dapple/models.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/services/editor_engine.dart';
import 'package:dapple/state/app_state.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

Photo _raw(String id) => Photo(id: id, name: '$id.arw', addedMs: DateTime(2026, 11, 15).millisecondsSinceEpoch, status: PhotoStatus.done);

String _meminfo(int totalKb, int availKb) => 'MemTotal:       $totalKb kB\nMemFree:  1 kB\nMemAvailable:   $availKb kB\n';

void main() {
  test('the zoom session is prefetched only with enough memory', () {
    expect(EditorEngine.detailPrefetchFits(_meminfo(11800000, 6000000)), isTrue); // 12 GB phone
    expect(EditorEngine.detailPrefetchFits(_meminfo(11800000, 2000000)), isFalse); // memory is tight right now
    expect(EditorEngine.detailPrefetchFits(_meminfo(4000000, 3500000)), isFalse); // small device
    expect(EditorEngine.detailPrefetchFits('garbage'), isFalse);
    EditorEngine.meminfoForTest = () => throw 'unreadable';
    expect(EditorEngine.detailPrefetchAllowed(), isFalse);
    EditorEngine.meminfoForTest = null;
  });

  group('SessionCache.prefetchDetail', () {
    late AppState app;
    late Photo p;
    var opened = 0;
    setUp(() {
      app = AppState.demo();
      p = _raw('a');
      app.sessions.testAvailable = true;
      app.sessions.testDetailOpener = (_) async {
        opened++;
        return null;
      };
      opened = 0;
    });

    test('opens once for a large picture, and the zoom finds it', () async {
      app.sessions.testFullSize = (_) async => (w: 6240, h: 4168);
      await app.sessions.prefetchDetail(p, () => true);
      await app.sessions.prefetchDetail(p, () => true);
      await app.sessions.detail(p);
      expect(opened, 1);
    });

    test('not for a picture the 1:1 region path serves, and not when a finger went down meanwhile', () async {
      app.sessions.testFullSize = (_) async => (w: 3000, h: 2000);
      await app.sessions.prefetchDetail(p, () => true);
      expect(opened, 0);
      app.sessions.testFullSize = (_) async => (w: 9568, h: 6376);
      await app.sessions.prefetchDetail(p, () => false);
      expect(opened, 0);
    });
  });

  testWidgets('the session open starts at the tap, before the editor page is built', (tester) async {
    final app = AppState.demo();
    final p = _raw('a');
    app.simulatePhotoForTest(p);
    app.sessions.testAvailable = true;
    final opened = <String>[];
    app.sessions.testOpener = (ph) async {
      opened.add(ph.id);
      return null;
    };
    late BuildContext ctx;
    await tester.pumpWidget(MaterialApp(home: Builder(builder: (c) {
      ctx = c;
      return const SizedBox();
    })));
    openEditor(ctx, app, 'a');
    expect(opened, ['a']); // synchronously: the route has not been built yet
    expect(app.sessions.has('a'), isTrue);
  });

  group('encodeWire', () {
    test('is jsonEncode for plain parameter maps', () {
      final m = <String, dynamic>{
        'look': 'std',
        'exposure': 12.5,
        'n': 3,
        'ok': true,
        'none': null,
        'nested': {
          'a': [1, 2.5, 'x"y\n']
        },
        'e': 'ü'
      };
      expect(encodeWire(m), jsonEncode(m));
      expect(() => encodeWire({'x': double.nan}), throwsA(isA<JsonUnsupportedObjectError>()));
    });

    test('brush points are written once and read back the same', () {
      final pts = Float32List.fromList([for (var i = 0; i < 600; i++) (i * 0.00137) % 1]);
      final stroke = BrushStroke(pts: pts);
      final list = LocalList([
        LocalAdjustment(id: 'l', mask: [MaskComponent(kind: MaskKindId.brush, strokes: [stroke])], ops: const {'exposure': 20})
      ]);
      final m = <String, dynamic>{'look': 'std', 'local': list.toEngineJson()};
      expect(encodeWire(m), contains(stroke.ptsText));
      expect(identical(stroke.ptsText, stroke.ptsText), isTrue); // written once per stroke
      expect(jsonDecode(encodeWire(m)), jsonDecode(jsonEncode(m))); // a plain jsonEncode reads RawJson back through toJson
      final app = jsonDecode(jsonEncode(list.toAppJson())) as Map;
      final eng = jsonDecode(encodeWire(m))['local'] as Map;
      final strokeApp = (((app['items'] as List)[0] as Map)['mask'] as List)[0]['strokes'][0];
      final strokeEng = (((eng['items'] as List)[0] as Map)['mask'] as List)[0]['strokes'][0];
      expect(strokeEng['pts'], strokeApp['pts']); // the persisted form still holds plain numbers
    });

    test('a brush edit costs less per frame than rebuilding the points (measured)', () {
      final pts = Float32List.fromList([for (var i = 0; i < 6000; i++) (i * 0.00031) % 1]);
      final list = LocalList([
        LocalAdjustment(id: 'l', mask: [MaskComponent(kind: MaskKindId.brush, strokes: [BrushStroke(pts: pts), BrushStroke(pts: pts)])], ops: const {'exposure': 20})
      ]);
      encodeWire({'local': list.toEngineJson()}); // fills the stroke caches
      final sw = Stopwatch()..start();
      for (var i = 0; i < 200; i++) {
        encodeWire({'look': 'std', 'exposure': i, 'local': list.toEngineJson()});
      }
      final cached = sw.elapsedMicroseconds / 200;
      sw.reset();
      for (var i = 0; i < 200; i++) {
        jsonEncode({'look': 'std', 'exposure': i, 'local': list.toAppJson()});
      }
      final plain = sw.elapsedMicroseconds / 200;
      debugPrint('brush edit, 2 x 3000 points: cached ${cached.toStringAsFixed(0)} us, rebuilt ${plain.toStringAsFixed(0)} us per frame');
      expect(cached, lessThan(plain));
    });
  });
}
