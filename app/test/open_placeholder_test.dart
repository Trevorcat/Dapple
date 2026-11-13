import 'dart:async';

import 'package:dapple/models.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/services/editor_engine.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/native/dapple_session.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';


Photo _raw(String id) =>
    Photo(id: id, name: '$id.raf', addedMs: DateTime(2026, 11, 11, 16).millisecondsSinceEpoch, status: PhotoStatus.done);

void main() {
  testWidgets('placeholder cross-fades into the real frame in the same box', (tester) async {
    final box = ValueKey('box');
    Widget host(bool real) => Directionality(
        textDirection: TextDirection.ltr,
        child: Center(child: SizedBox(
            key: box,
            width: 300,
            height: 200,
            child: ArtCrossFade(real: real, child: ColoredBox(key: ValueKey(real ? 'r' : 'p'), color: real ? Colors.green : Colors.red)))));
    await tester.pumpWidget(host(false));
    expect(find.byKey(const ValueKey('p')), findsOneWidget);
    await tester.pumpWidget(host(true));
    await tester.pump(const Duration(milliseconds: 80));
    // mid-fade: both layers present, both fill the box (no geometry jump)
    expect(find.byKey(const ValueKey('p')), findsOneWidget);
    expect(find.byKey(const ValueKey('r')), findsOneWidget);
    expect(tester.getSize(find.byKey(const ValueKey('p'))), const Size(300, 200));
    expect(tester.getSize(find.byKey(const ValueKey('r'))), const Size(300, 200));
    await tester.pump(const Duration(milliseconds: 200));
    expect(find.byKey(const ValueKey('p')), findsNothing);
    expect(find.byKey(const ValueKey('r')), findsOneWidget);
  });

  testWidgets('the progress hint only shows after 400 ms', (tester) async {
    await tester.pumpWidget(const Directionality(textDirection: TextDirection.ltr, child: Center(child: DelayedBusy())));
    double op() => tester.widget<AnimatedOpacity>(find.byType(AnimatedOpacity)).opacity;
    await tester.pump(const Duration(milliseconds: 300));
    expect(op(), 0);
    await tester.pump(const Duration(milliseconds: 150));
    await tester.pump(const Duration(milliseconds: 200));
    expect(op(), 1);
    await tester.pumpWidget(const SizedBox()); // disposes the spinner's ticker
  });

  testWidgets('an edit made before the real frame is ready is kept and does not block', (tester) async {
    final app = AppState.demo();
    final p = _raw('a');
    app.simulatePhotoForTest(p);
    final gate = Completer<DappleSession?>();
    app.sessions.testAvailable = true;
    app.sessions.testOpener = (_) => gate.future;
    final engine = EditorEngine(app);
    engine.attach(p, neighbors: const []);
    await tester.pump(const Duration(milliseconds: 50));
    expect(engine.previewOnly, isTrue); // no real frame yet: the placeholder stays
    app.applyEdit(p.id, (e) => e.copyWith(film: 'kodak_portra_400'));
    engine.edited(); // returns at once
    expect(app.photoById(p.id)!.edit.film, 'kodak_portra_400');
    expect(engine.image, isNull);
    gate.complete(null); // the open finishes: the queued edit is picked up by the render loop, nothing throws
    await tester.runAsync(() => Future<void>.delayed(const Duration(milliseconds: 50)));
    await tester.pump(const Duration(milliseconds: 50));
    expect(app.photoById(p.id)!.edit.film, 'kodak_portra_400');
    expect(engine.rendering, isFalse);
    engine.dispose();
    await tester.pump(const Duration(seconds: 5)); // let the engine's own idle timers run out
  });

  testWidgets('finger down starts the open; an abandoned tap closes it, a tap keeps it', (tester) async {
    final app = AppState.demo();
    final a = _raw('a'), b = _raw('b');
    app.sessions.testAvailable = true;
    final opened = <String>[];
    app.sessions.testOpener = (ph) async {
      opened.add(ph.id);
      return null;
    };
    // abandoned: down, wait past the delay, cancel (scroll took the gesture)
    app.sessions.pointerDown(a);
    await tester.pump(const Duration(milliseconds: 60));
    expect(opened, ['a']);
    expect(app.sessions.has('a'), isTrue);
    app.sessions.pointerEnd(a);
    await tester.pump(const Duration(milliseconds: 300));
    expect(app.sessions.has('a'), isFalse);
    // a quick touch that ends before the delay never opens anything
    app.sessions.pointerDown(b);
    await tester.pump(const Duration(milliseconds: 20));
    app.sessions.pointerEnd(b);
    await tester.pump(const Duration(milliseconds: 300));
    expect(opened, ['a']);
    // a real tap: the editor claims the session within the grace period
    app.sessions.pointerDown(b);
    await tester.pump(const Duration(milliseconds: 60));
    app.sessions.pointerEnd(b);
    await app.sessions.get(b); // what the editor does
    await tester.pump(const Duration(milliseconds: 300));
    expect(app.sessions.has('b'), isTrue);
    expect(opened, ['a', 'b']);
  });
}
