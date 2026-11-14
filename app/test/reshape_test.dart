// 形体 (docs/design/local-adjustments.md section 9.2; engine: docs/engine/pipeline-architecture.md section 45): the model (JSON as the engine
// reads it), and the tiles in the 人像 panel on the real editor: groups, first tap = on at the recommended amount for the main face, the strip,
// second tap = 关掉, the face row with several faces, 推 / 恢复 on the photo, 对比 hides the reshaping, no body tiles without a pose.
import 'package:dapple/editor/fold_layout.dart' show debugFoldLayouts;
import 'package:dapple/model/params.dart';
import 'package:dapple/model/reshape.dart';
import 'package:dapple/native/ai_bindings.dart' show AiAvailability, AiFace, AiJobResult, AiPose;
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

/// A standing figure: 33 landmarks (x, y, visibility), everything visible except the indices in [hidden].
List<double> poseOf({Set<int> hidden = const {}, double cx = 0.5}) {
  final p = List<double>.filled(99, 0);
  void put(int k, double dx, double y) {
    p[3 * k] = cx + dx;
    p[3 * k + 1] = y;
    p[3 * k + 2] = hidden.contains(k) ? 0.1 : 0.95;
  }
  put(0, 0, 0.12);
  put(7, -0.03, 0.11);
  put(8, 0.03, 0.11);
  put(11, -0.09, 0.22);
  put(12, 0.09, 0.22);
  put(13, -0.10, 0.34);
  put(14, 0.10, 0.34);
  put(15, -0.10, 0.45);
  put(16, 0.10, 0.45);
  put(23, -0.05, 0.48);
  put(24, 0.05, 0.48);
  put(25, -0.05, 0.68);
  put(26, 0.05, 0.68);
  put(27, -0.05, 0.88);
  put(28, 0.05, 0.88);
  for (final k in [29, 30, 31, 32]) {
    put(k, k.isEven ? 0.05 : -0.05, 0.92);
  }
  return p;
}

/// [r] with a pose list (the main person first) and the face anchors of [n] faces.
AiJobResult withPoses(AiJobResult r, int n, List<List<double>> poses) {
  final f = withFaces(r, n);
  return AiJobResult(
    state: f.state,
    message: f.message,
    planes: f.planes,
    groups: f.groups,
    personFound: f.personFound,
    faces: f.faces,
    skyFound: f.skyFound,
    totalMs: f.totalMs,
    faceList: f.faceList,
    poseList: [for (var i = 0; i < poses.length; i++) AiPose(index: i + 1, score: 0.9, pts: poses[i])],
  );
}

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

  group('身体', () {
    setUp(() => debugFoldLayouts = false);
    tearDown(() => debugFoldLayouts = true);

    const withPose = AiAvailability(compiled: true, available: true, person: true, faces: true, depth: true, tap: true, sky: true, pose: true);

    test('what each operator needs: the engine rules on the landmarks', () {
      final full = poseOf();
      for (final op in [ReshapeOp.waist, ReshapeOp.arms, ReshapeOp.legs, ReshapeOp.longLegs, ReshapeOp.head]) {
        expect(bodyBlockFor(op, full), isNull, reason: '$op');
        expect(bodyBlockFor(op, null)?.note, '没找到');
      }
      // no legs in the picture (a half-body shot): 瘦腿 and 长腿 are off, 瘦腰 / 瘦手臂 / 小头 stay
      final half = poseOf(hidden: {25, 26, 27, 28, 29, 30, 31, 32});
      expect(bodyBlockFor(ReshapeOp.legs, half)?.note, '没拍到腿');
      expect(bodyBlockFor(ReshapeOp.longLegs, half)?.note, '没拍到脚');
      expect(bodyBlockFor(ReshapeOp.waist, half), isNull);
      expect(bodyBlockFor(ReshapeOp.head, half), isNull);
      // a head-and-shoulders shot: no hips, so no 小头 (it would be the whole picture) and no 瘦腰
      final bust = poseOf(hidden: {23, 24, 25, 26, 27, 28, 29, 30, 31, 32});
      expect(bodyBlockFor(ReshapeOp.head, bust)?.note, '没拍到腰');
      expect(bodyBlockFor(ReshapeOp.waist, bust)?.note, '没拍到腰');
      expect(bodyBlockFor(ReshapeOp.arms, bust), isNull);
      // ankles doubtful but the feet are not: 长腿 still has a foot
      expect(bodyBlockFor(ReshapeOp.longLegs, poseOf(hidden: {27, 28})), isNull);
      // lying down: not upright
      final lying = poseOf();
      for (var k = 0; k < 33; k++) {
        final x = lying[3 * k], y = lying[3 * k + 1];
        lying[3 * k] = y;
        lying[3 * k + 1] = x;
      }
      expect(bodyBlockFor(ReshapeOp.longLegs, lying)?.note, '要站直');
      // the person plane misses the body: the protected operators are off, 长腿 is not (no protection to lose)
      expect(bodyBlockFor(ReshapeOp.waist, full, covered: false)?.note, '轮廓不清');
      expect(bodyBlockFor(ReshapeOp.longLegs, full, covered: false), isNull);
    });

    testWidgets('身体: five tiles after 脸型; a first tap = on at 50 for the main person; 长腿 and 瘦腿 greyed with a hint on a half-body shot', (t) async {
      final fake = FakeLocalAi()..avail = withPose;
      final r = await openPortrait(t, fake, withPoses(portraitResult(), 1, [poseOf(hidden: {25, 26, 27, 28, 29, 30, 31, 32}), poseOf(cx: 0.2)]));
      expect(fake.runs.first & 32, 32, reason: 'the portrait analysis asks for the pose');
      expect(key('shape-group-身体'), findsOneWidget);
      for (final op in [ReshapeOp.waist, ReshapeOp.arms, ReshapeOp.legs, ReshapeOp.longLegs, ReshapeOp.head]) {
        expect(key('shape-${op.name}'), findsOneWidget);
      }
      expect(key('shape-grey-legs'), findsOneWidget);
      expect(key('shape-grey-longLegs'), findsOneWidget);
      expect(key('shape-grey-waist'), findsNothing);
      expect(find.text('没拍到腿'), findsOneWidget);
      expect(t.getSize(key('shape-waist')).height, greaterThanOrEqualTo(48));
      // a tap on a greyed tile: the reason, nothing turns on
      await t.ensureVisible(key('shape-longLegs'));
      await tapKey(t, 'shape-longLegs');
      expect(find.text('要拍到脚才能拉长腿'), findsOneWidget);
      expect(shapeOf(r).on(ReshapeOp.longLegs), isFalse);
      // 瘦腰: on at 50 for the main person (the first pose), the 33 points travel with the edit
      await t.ensureVisible(key('shape-waist'));
      await tapKey(t, 'shape-waist');
      final s = shapeOf(r);
      expect(s.value(ReshapeOp.waist), kReshapeDefault);
      expect(s.body!.pts.length, 99);
      expect(s.body!.pts[3 * 25 + 2], closeTo(0.1, 1e-9), reason: 'the main person (half-body) is the one that is reshaped');
      expect(s.matte, isNotNull);
      final j = s.toEngineJson()!;
      expect((j['bodies'] as List).single['waist'], 50);
      // 分项: only the operators that can work here
      await tapKey(t, 'items-button');
      expect([for (final i in r.lc.reshapeTool.items()) i.id], ['waist', 'arms', 'head']);
    });

    testWidgets('no pose found: the 身体 tiles are greyed with 没找到; without the pose model the group is not there', (t) async {
      final fake = FakeLocalAi()..avail = withPose;
      final r = await openPortrait(t, fake, withPoses(portraitResult(), 1, const []));
      expect(key('shape-group-身体'), findsOneWidget);
      expect(find.text('没找到'), findsNWidgets(5));
      await t.ensureVisible(key('shape-head'));
      await tapKey(t, 'shape-head');
      expect(shapeOf(r).body, isNull);
      expect(find.text('没找到身体，试试拍到半身或全身的照片'), findsOneWidget);
    });

    testWidgets('a device without the pose model: no 身体 group (and the portrait analysis does not wait for it)', (t) async {
      final fake = FakeLocalAi();
      await openPortrait(t, fake, withFaces(portraitResult(), 1));
      expect(fake.runs.first & 32, 0);
      expect(key('shape-group-身体'), findsNothing);
    });
  });
}
