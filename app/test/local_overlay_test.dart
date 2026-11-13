// The red overlay of 局部 (docs/design/local-adjustments.md section 6): a stale overlay is hidden while a shape is dragged, but kept
// (and swapped directly for the fresh one) while a range band is dragged ([LocalKind.overlayFollowsStale]).
import 'dart:async';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:dapple/model/local_kinds.dart';
import 'package:dapple/screens/editor/local_layer.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show Finger, settle;
import 'local_panel_test.dart' show Rig, addViaCard, frameRect, openLocal;

Future<ui.Image> _img(WidgetTester t) async => (await t.runAsync(() {
      final c = Completer<ui.Image>();
      ui.decodeImageFromPixels(Uint8List.fromList(List.filled(16, 255)), 2, 2, ui.PixelFormat.rgba8888, c.complete);
      return c.future;
    }))!;

double _opacity(WidgetTester t) =>
    t.widget<AnimatedOpacity>(find.descendant(of: find.byType(LocalPhotoLayer), matching: find.byType(AnimatedOpacity)).first).opacity;

ui.Image? _shown(WidgetTester t) {
  final f = find.byKey(const ValueKey('local-overlay'));
  return f.evaluate().isEmpty ? null : t.widget<RawImage>(f).image;
}

Future<Rig> _withOverlay(WidgetTester t, String kind) async {
  final r = await openLocal(t);
  await addViaCard(t, kind);
  r.lc.debugDeliverOverlay(await _img(t));
  r.lc.flash(60000);
  await settle(t, 4);
  return r;
}

void main() {
  test('only the range kinds follow a stale overlay', () {
    for (final k in localKinds) {
      expect(k.overlayFollowsStale, const {'luminance', 'color', 'depth'}.contains(k.id), reason: k.id);
    }
  });

  testWidgets('range drag: the stale overlay stays, a fresh one swaps in with no blank frame, the end shows the final one', (t) async {
    final r = await _withOverlay(t, 'luminance');
    final first = _shown(t);
    expect(first, isNotNull);
    expect(_opacity(t), 1);

    r.lc.rangeBegin();
    r.lc.setLum(20, 70); // the shape on screen moves on; the overlay is older
    await settle(t, 2);
    expect(r.lc.overlayStale.value, isTrue);
    expect(_opacity(t), 1, reason: 'stale overlay kept during a range drag');
    expect(_shown(t), same(first));

    final second = await _img(t);
    r.lc.setLum(30, 80);
    r.lc.debugDeliverOverlay(second, current: false); // still older than the values on screen
    await t.pump();
    expect(_shown(t), same(second), reason: 'swapped in the same frame: no blank one');
    expect(_opacity(t), 1);

    r.lc.rangeEnd();
    await settle(t, 2);
    expect(_opacity(t), 1, reason: 'kept after the release until the exact overlay arrives');
    final last = await _img(t);
    r.lc.debugDeliverOverlay(last); // exact for the final values
    await settle(t, 4);
    expect(r.lc.overlayStale.value, isFalse);
    expect(r.lc.overlayFollow.value, isFalse);
    expect(_shown(t), same(last));
    expect(_opacity(t), 1);
  });

  testWidgets('geometry drag: a stale overlay is hidden as before', (t) async {
    final r = await _withOverlay(t, 'radial');
    expect(_opacity(t), 1);
    final fr = frameRect(t);
    final f = await Finger.down(t, fr.center);
    await f.drag(const Offset(40, 24));
    await settle(t, 2);
    expect(r.lc.shaping, isTrue);
    expect(r.lc.overlayStale.value, isTrue);
    expect(r.lc.overlayFollow.value, isFalse);
    expect(_opacity(t), 0);
    r.lc.debugDeliverOverlay(await _img(t));
    await settle(t, 2);
    expect(_opacity(t), 1, reason: 'a fresh overlay shows');
    await f.up();
  });

  testWidgets('a pad drag still hides the overlay, also after a range drag', (t) async {
    final r = await _withOverlay(t, 'luminance');
    r.lc.rangeBegin();
    r.lc.setLum(10, 60);
    r.lc.rangeEnd();
    r.lc.debugDeliverOverlay(await _img(t)); // exact: the follow ends
    r.lc.padDragging = true;
    r.lc.flash(60000); // re-evaluates the visibility
    await settle(t, 2);
    expect(r.lc.overlayOn.value, isFalse);
    expect(_opacity(t), 0);
    r.lc.padDragging = false;
  });
}
