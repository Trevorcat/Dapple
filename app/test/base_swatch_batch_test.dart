// The batch route of the base swatches: a BatchSwatchSource gets small chunks first (2, 4, then its own size), nearest to the selection on the
// film map first; a finger that goes down cancels the chunk in flight and what it did not do is asked for again; a plain source still gets one
// call per base; the native binding degrades without the library.
import 'dart:async';
import 'dart:ui' as ui;

import 'package:dapple/native/dapple_session.dart';
import 'package:dapple/services/base_swatches.dart';
import 'package:dapple/services/base_swatch_sources.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';

Future<ui.Image> _img(WidgetTester t) async => (await t.runAsync(() async {
      final rec = ui.PictureRecorder();
      Canvas(rec).drawRect(const Rect.fromLTWH(0, 0, 4, 4), Paint()..color = const Color(0xFF336699));
      return rec.endRecording().toImage(4, 4);
    }))!;

class BatchFake implements BatchSwatchSource {
  BatchFake(this.key, this.image, {this.size = 6, this.gate});
  @override
  final String key;
  final ui.Image image;
  final int size;
  Future<void>? gate;
  final List<List<String?>> chunks = [];
  int cancels = 0;
  bool _cancelled = false;

  @override
  int get batchSize => size;

  @override
  Future<ui.Image?> render(String? baseId, int longEdge) async => (await renderBatch([baseId], longEdge)).first;

  @override
  Future<List<ui.Image?>> renderBatch(List<String?> ids, int longEdge) async {
    chunks.add(ids);
    _cancelled = false;
    if (gate != null) await gate;
    // a cancelled call returns what it had finished: here the first base only
    return [for (var i = 0; i < ids.length; i++) _cancelled && i > 0 ? null : image];
  }

  @override
  void cancel() {
    cancels++;
    _cancelled = true;
  }
}

BaseSwatches make(BatchFake src, List<String?> ids, {bool watch = false}) =>
    BaseSwatches(source: src, ids: ids, watchPointers: watch, gap: Duration.zero, settle: Duration.zero, longEdge: 96);

Future<void> pumpMs(WidgetTester t, int ms) => t.pump(Duration(milliseconds: ms));

void main() {
  tearDown(() {
    BaseSwatches.mapPositions = null;
  });
  setUp(BaseSwatches.clearCache);

  testWidgets('chunks grow 2, 4, then the source size; every base lands once', (t) async {
    final src = BatchFake('b1', await _img(t), size: 6);
    final ids = <String?>[null, for (var i = 0; i < 14; i++) 's$i'];
    final sw = make(src, ids)..start();
    for (var i = 0; i < 8; i++) {
      await pumpMs(t, 50);
    }
    expect([for (final c in src.chunks) c.length], [2, 4, 6, 3]);
    expect(src.chunks.expand((c) => c).toSet().length, 15, reason: 'no base twice');
    expect(sw.done, isTrue);
    sw.dispose();
  });

  testWidgets('nearest to the selection on the film map first', (t) async {
    BaseSwatches.mapPositions = {
      'sel': const Offset(0, 0),
      'far': const Offset(0.9, 0.9),
      'near': const Offset(0.1, 0),
      'mid': const Offset(0.4, 0.3),
    };
    final src = BatchFake('b2', await _img(t), size: 2);
    // the selection first (as the editor lists them), then the others in a fixed order
    final sw = make(src, ['sel', 'far', 'mid', 'near'])..start();
    for (var i = 0; i < 6; i++) {
      await pumpMs(t, 50);
    }
    expect(src.chunks.expand((c) => c).toList(), ['sel', 'near', 'mid', 'far']);
    sw.dispose();
  });

  testWidgets('a finger down cancels the chunk; its unfinished bases are asked for again after it is up', (t) async {
    final gate = Completer<void>();
    final src = BatchFake('b3', await _img(t), size: 6, gate: gate.future);
    await t.pumpWidget(const SizedBox());
    final sw = BaseSwatches(source: src, ids: ['a', 'b', 'c', 'd'], gap: Duration.zero, settle: const Duration(milliseconds: 100))..start();
    await pumpMs(t, 50);
    expect(src.chunks.length, 1, reason: 'the first chunk (2 bases) is in flight');
    final g = await t.startGesture(const Offset(3, 3));
    expect(src.cancels, 1);
    src.gate = null;
    gate.complete();
    await pumpMs(t, 50);
    expect(sw.imageOf('a'), isNotNull, reason: 'what the chunk finished is kept');
    expect(sw.imageOf('b'), isNull, reason: 'cancelled, not failed');
    await pumpMs(t, 300);
    expect(src.chunks.length, 1, reason: 'nothing renders while the finger is down');
    await g.up();
    await pumpMs(t, 500);
    await pumpMs(t, 500);
    expect(sw.done, isTrue, reason: 'b (and the rest) were asked for again');
    expect(src.chunks.expand((c) => c).where((x) => x == 'b').length, 2);
    sw.dispose();
  });

  testWidgets('dispose cancels the chunk in flight', (t) async {
    final gate = Completer<void>();
    final src = BatchFake('b4', await _img(t), gate: gate.future);
    final sw = make(src, ['a', 'b', 'c'])..start();
    await pumpMs(t, 50);
    sw.dispose();
    expect(src.cancels, 1);
    gate.complete();
    await pumpMs(t, 100);
    expect(sw.imageOf('a'), isNull);
  });

  test('without the native library: no batch call, swatch params keep the film key out of the batch route', () async {
    expect(DappleSession.hasBaseSwatches, isFalse);
    final closed = DappleSession.forTest(0, closed: true);
    expect(await closed.baseSwatches({}, ['a', null]), [null, null]);
    closed.cancelBaseSwatches(); // a no-op, not a crash
    expect(swatchParams({'contrast': 5, 'film': 'x', 'film_print': 1}, null), {'contrast': 5});
    expect(swatchParams({'contrast': 5}, 'kodak_portra_400'), {'contrast': 5, 'film': 'kodak_portra_400'});
  });
}
