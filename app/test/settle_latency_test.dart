import 'dart:async';

import 'package:dapple/models.dart';
import 'package:dapple/native/dapple_session.dart';
import 'package:dapple/services/editor_engine.dart';
import 'package:dapple/state/app_state.dart';
import 'package:flutter_test/flutter_test.dart';

// The sharp frame after a drag must follow the finger-up, not a timer (owner report: ~1.5 s on a foldable after a 局部 pad drag).
void main() {
  Photo raw(String id) => Photo(id: id, name: '$id.raf', addedMs: DateTime(2026, 11, 14).millisecondsSinceEpoch, status: PhotoStatus.done);

  testWidgets('finger-up requests the settled frame in the same call, without a timer', (tester) async {
    final app = AppState.demo();
    final p = raw('a');
    app.simulatePhotoForTest(p);
    app.sessions.testAvailable = true;
    final gate = Completer<DappleSession?>();
    app.sessions.testOpener = (_) => gate.future; // the session never opens: only the scheduling is under test
    final engine = EditorEngine(app);
    engine.attach(p, neighbors: const []);

    engine.beginInteraction(activeParam: 'temp');
    for (var i = 0; i < 3; i++) {
      app.applyEdit(p.id, (e) => e.copyWith(film: i.isEven ? 'kodak_portra_400' : null), coalesce: 'drag', gesture: true);
      engine.edited();
    }
    expect(engine.debugSettleRequests, 0); // nothing settles while the finger is down
    engine.endInteraction();
    expect(engine.debugSettleRequests, 1); // synchronous: no pump, no timer elapsed

    // a plain tap edit (no drag) is never a settle request: it renders at full quality straight away
    engine.edited();
    expect(engine.debugSettleRequests, 1);

    gate.complete(null);
    await tester.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 20)));
    engine.dispose();
    await tester.pump(const Duration(seconds: 5)); // the engine's idle timers run out
  });
}
