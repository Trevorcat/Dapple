// 形体 (docs/design/local-adjustments.md section 9.2; engine: docs/engine/pipeline-architecture.md section 45): the model (JSON as the engine
// reads it), and the tiles in the 人像 panel on the real editor: groups, first tap = on at the recommended amount for the main face, the strip,
// second tap = 关掉, the face row with several faces, 推 / 恢复 on the photo, 对比 hides the reshaping, no body tiles without a pose.
import 'package:dapple/editor/fold_layout.dart' show debugFoldLayouts;
import 'package:dapple/model/params.dart';
import 'package:dapple/model/reshape.dart';
import 'package:dapple/native/ai_bindings.dart' show AiFace, AiJobResult;
import 'package:dapple/screens/editor/local_controller.dart' show ReshapeItems;
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show Finger, settle;
import 'local_ai_test.dart' show FakeLocalAi;
import 'local_panel_test.dart' show Rig, frameRect, key, tapKey;
import 'local_portrait_test.dart' show openPortrait, portraitResult;

List<double> shapeAt(double cx) => [for (var k = 0; k < 54; k++) ...[cx + 0.001 * k, 0.3 + 0.002 * k]];

AiJobResult withFaces(AiJobResult r, int n) => AiJobResult(
      state: r.state,
      message: r.message,
      planes: r.planes,
      groups: r.groups,
      personFound: r.personFound,
      faces: r.faces,
      skyFound: r.skyFound,
      totalMs: r.totalMs,
      faceList: [for (var i = 1; i <= n; i++) AiFace(index: i, iod: 0.1 / i, frontal: true, shape: shapeAt(i / (n + 1)))],
    );

Reshape shapeOf(Rig r) => r.app.editOf(r.id).reshape;

void main() {
  group('model', () {
    test('engine JSON: keys, only non-zero values, round trip', () {
      final r = Reshape.empty
          .withAnchors(faces: [(1, shapeAt(0.4)), (2, shapeAt(0.7))], matte: 'ai:abc:person')
          .withValue(ReshapeOp.slim, 50, sel: {1})
          .withValue(ReshapeOp.chin, -30, sel: {1, 2})
          .copyWith(push: [const PushStroke(r: 0.04, pts: [0.1, 0.1, 0.2, 0.2]), const PushStroke(r: 0.03, pts: [0.2, 0.2, 0.3, 0.3], restore: true)]);
      final j = r.toEngineJson()!;
      expect(j['v'], 1);
      expect(j['matte'], 'ai:abc:person');
      final faces = j['faces'] as List;
      expect(faces.length, 2);
      expect((faces[0] as Map)['slim'], 50);
      expect((faces[1] as Map).containsKey('slim'), isFalse);
      expect((faces[1] as Map)['chin'], -30);
      expect(((faces[0] as Map)['pts'] as List).length, 108);
      expect((j['push'] as List)[1], {'r': 0.03, 'restore': true, 'pts': [0.2, 0.2, 0.3, 0.3]});
      final back = Reshape.fromJson(j);
      expect(back.same(r), isTrue);
      expect(r.on(ReshapeOp.slim), isTrue);
      expect(r.facesOf(ReshapeOp.slim), {1});
      expect(r.value(ReshapeOp.chin), -30);
    });
    test('the edit carries it: engine params, persisted JSON, isAdjusted, sameAs, paste keeps the target\'s own', () {
      final r = Reshape.empty.withAnchors(faces: [(1, shapeAt(0.5))]).withValue(ReshapeOp.eyes, 40);
      final e = EditState.empty.copyWith(reshape: r);
      expect(e.isAdjusted, isTrue);
      expect(e.toWire(defaultLook: 'std', defaultToneStrength: 50)['reshape'], isNotNull);
      final back = EditState.fromJson(e.toJson());
      expect(back.reshape.same(r), isTrue);
      expect(back.sameAs(e), isTrue);
      expect(EditState.empty.toWire(defaultLook: 'std', defaultToneStrength: 50).containsKey('reshape'), isFalse);
      // anchors without any strength: kept (re-opening keeps the faces), nothing moves
      final anchors = EditState.empty.copyWith(reshape: Reshape.empty.withAnchors(faces: [(1, shapeAt(0.5))]));
      expect(anchors.isAdjusted, isFalse);
      // copy / paste settings never carries a reshaping (per photo): the target keeps its own
      expect(anchors.pasted(e).reshape.same(anchors.reshape), isTrue);
    });
    test('the strip: 0 .. 100 with the recommended amount in the middle', () {
      expect(reshapeFromStrip(0.5), kReshapeDefault);
      expect(reshapeFromStrip(1), 100);
      expect(reshapeToStrip(25), 0.25);
      expect(faceOpsFor(anyFrontal: true).length, 5);
      expect(faceOpsFor(anyFrontal: false), [ReshapeOp.slim, ReshapeOp.small]);
    });
  });

  group('panel', () {
    setUp(() => debugFoldLayouts = false);
    tearDown(() => debugFoldLayouts = true);

    testWidgets('脸型: five tiles, no 身体 without a pose; first tap = on at 50 for the main face with the person plane; strip; second tap = 关掉',
        (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, withFaces(portraitResult(), 1));
      expect(key('shape-group-脸型'), findsOneWidget);
      expect(key('shape-group-身体'), findsNothing);
      for (final op in [ReshapeOp.slim, ReshapeOp.small, ReshapeOp.chin, ReshapeOp.eyes, ReshapeOp.nose]) {
        expect(key('shape-${op.name}'), findsOneWidget);
      }
      // 48 dp targets
      expect(t.getSize(key('shape-slim')).height, greaterThanOrEqualTo(48));
      final localBefore = r.local.items.length;
      await tapKey(t, 'shape-slim');
      final s = shapeOf(r);
      expect(s.value(ReshapeOp.slim), kReshapeDefault);
      expect(s.facesOf(ReshapeOp.slim), {1});
      expect(s.matte, isNotNull);
      expect(s.faces.single.pts.length, 108);
      expect(r.local.items.length, localBefore, reason: 'a reshaping is not a local area');
      expect(r.lc.reshapeTool.sel, ReshapeOp.slim);
      expect(key('portrait-strip-slim'), findsOneWidget);
      // the strip: dragging to the top = 100
      final strip = t.getRect(key('portrait-strip-slim'));
      final f = await Finger.down(t, strip.center);
      await f.drag(strip.topCenter + const Offset(0, 2) - strip.center);
      await f.up();
      await settle(t, 3);
      expect(shapeOf(r).value(ReshapeOp.slim), closeTo(100, 0.5));
      // second tap: the action card with 关掉; 关掉 turns it off (撤销 on the toast)
      await tapKey(t, 'shape-slim');
      expect(key('act-关掉'), findsOneWidget);
      await tapKey(t, 'act-关掉');
      expect(shapeOf(r).on(ReshapeOp.slim), isFalse);
      expect(find.text('已关掉瘦脸'), findsOneWidget);
    });

    testWidgets('a retouch chip and a 形体 tile share the strip: tapping a chip hands the strip back', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, withFaces(portraitResult(), 1));
      await tapKey(t, 'shape-eyes');
      expect(r.lc.reshapeTool.sel, ReshapeOp.eyes);
      await t.ensureVisible(key('chip-skin'));
      await tapKey(t, 'chip-skin');
      expect(r.lc.reshapeTool.sel, isNull);
      expect(shapeOf(r).on(ReshapeOp.eyes), isTrue, reason: 'selecting something else turns nothing off');
    });

    testWidgets('two faces: the main face by default, the face row adds the other', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, withFaces(portraitResult(faces: 2), 2), faces: 2);
      await t.ensureVisible(key('shape-small'));
      await tapKey(t, 'shape-small');
      expect(shapeOf(r).facesOf(ReshapeOp.small), {1});
      await t.ensureVisible(key('face-2'));
      await tapKey(t, 'face-2');
      expect(shapeOf(r).facesOf(ReshapeOp.small), {1, 2});
      expect(shapeOf(r).faces.firstWhere((f) => f.index == 2).v['small'], kReshapeDefault);
    });

    testWidgets('推: the tool under the photo, a drag adds one stroke in one undo step; 恢复 adds a restore stroke; 对比 hides the reshaping',
        (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, withFaces(portraitResult(), 1));
      await tapKey(t, 'shape-slim');
      expect(key('push-mode'), findsOneWidget);
      await tapKey(t, 'push-mode');
      expect(r.lc.reshapeTool.pushMode, isTrue);
      final c = frameRect(t).center;
      var f = await Finger.down(t, c);
      await f.drag(const Offset(60, 4));
      await f.up();
      await settle(t, 3);
      final s = shapeOf(r);
      expect(s.push.length, 1);
      expect(s.push.single.restore, isFalse);
      expect(s.push.single.pts.length, greaterThanOrEqualTo(4));
      expect(s.push.single.pts[2], greaterThan(s.push.single.pts[0]), reason: 'a drag to the right moves right in the photo');
      // one undo step
      r.app.undo(r.id);
      await settle(t, 3);
      expect(shapeOf(r).push, isEmpty);
      await tapKey(t, 'push-sub');
      f = await Finger.down(t, c);
      await f.drag(const Offset(0, 40));
      await f.up();
      await settle(t, 3);
      expect(shapeOf(r).push.single.restore, isTrue);
      // the compare hold renders without the reshaping (as without the local areas)
      r.lc.setComparing(true);
      expect(r.lc.ctl.engine.hideLocal, isTrue);
      r.lc.setComparing(false);
    });

    testWidgets('分项: the group operators on one radar; 下巴 goes both ways', (t) async {
      final fake = FakeLocalAi();
      final r = await openPortrait(t, fake, withFaces(portraitResult(), 1));
      await tapKey(t, 'shape-chin');
      await tapKey(t, 'items-button');
      expect(key('shape-radar-chin'), findsOneWidget);
      final items = r.lc.reshapeTool.items();
      expect([for (final i in items) i.id], ['slim', 'small', 'chin', 'eyes', 'nose']);
      final chin = items.firstWhere((i) => i.id == 'chin');
      expect(chin.min, -100);
      final src = r.lc.reshapeTool.itemSource;
      src.begin(chin);
      src.set(chin, -40);
      src.end();
      await settle(t, 3);
      expect(shapeOf(r).value(ReshapeOp.chin), -40);
      expect(shapeOf(r).facesOf(ReshapeOp.chin), {1});
    });

    testWidgets('an analysis without anchors (older engine / cache): no 形体 tiles', (t) async {
      final fake = FakeLocalAi();
      await openPortrait(t, fake, portraitResult());
      expect(key('shape-group-脸型'), findsNothing);
      expect(key('shape-slim'), findsNothing);
    });
  });
}
