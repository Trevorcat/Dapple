// The AI part of 局部 on the real editor, driven by a fully controllable [LocalAi] fake: the shape card's AI row, 人物 (progress, cancel,
// parts, one area from several picks), no person, 天空, 远近, 点选 (taps, subtract, undo, session replay), the brush correction actions,
// missing planes and their repair, memory release, and the model JSON of an AI area (docs/design/local-adjustments.md section 9).
import 'dart:async';
import 'dart:convert';

import 'package:dapple/model/local_adjust.dart';
import 'package:dapple/model/local_kinds.dart';
import 'package:dapple/model/params.dart';
import 'package:dapple/models.dart' show Photo;
import 'package:dapple/screens/editor/local_controller.dart';
import 'package:dapple/services/local_ai.dart';
import 'package:dapple/state/app_state.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'editor_v2_test.dart' show Finger, n5Dpr, n5Phys, phoneDpr, phonePhys, settle;
import 'local_panel_test.dart' show Rig, addViaCard, area, back, frameRect, key, openLocal, ownerDpr, ownerPhys, tapKey;

const photoHash = '0123456789abcdef';
String planeId(String k) => 'ai:$photoHash:$k';

AiPlane plane(String k, {int face = 0, String group = 'person'}) => AiPlane(id: planeId(k), key: k, label: k, group: group, face: face, w: 64, h: 64, cached: false);

AiJobResult result({bool person = true, bool sky = false, int faces = 0, List<AiPlane> planes = const [], String state = 'done'}) => AiJobResult(
      state: state,
      message: '',
      planes: planes,
      groups: const [],
      personFound: person,
      faces: faces,
      skyFound: sky,
      totalMs: 1,
    );

/// A person with one face: the parts a portrait analysis returns (no body skin, no teeth).
AiJobResult portrait() => result(
      faces: 1,
      planes: [
        plane('person'),
        plane('faceskin'),
        plane('hair'),
        plane('clothes'),
        plane('background', group: 'person'),
        plane('f1:eyes', face: 1, group: 'faces'),
        plane('f1:lips', face: 1, group: 'faces'),
      ],
    );

class FakeLocalAi implements LocalAi {
  AiAvailability avail = const AiAvailability(compiled: true, available: true, person: true, faces: true, depth: true, tap: true, sky: true);

  /// Task bits of every run() call.
  final runs = <int>[];
  int cancels = 0;
  final _ctrls = <StreamController<AiProgress>>[];

  /// The tap session of the "engine": taps in the id format, and a log of what was called.
  final engineTaps = <String>[];
  final log = <String>[];
  final taps = <({double u, double v, bool add})>[];
  Completer<void>? tapHold;
  bool tapOk = true;

  /// Scripted missing planes: ids that [missing] reports until [ensure] fixes them (when [ensureFixes]).
  final missingIds = <String>{};
  bool ensureFixes = false;
  int ensureCalls = 0, missingCalls = 0, prepareTaps = 0;
  final releases = <bool>[];

  StreamController<AiProgress> get last => _ctrls.last;

  void progress(double v, {String stage = 'person'}) => last.add(AiProgress(runs.length, 'running', v, stage));
  void finish(AiJobResult r) {
    last.add(AiProgress(runs.length, r.state, 1, '', r));
    last.close();
  }

  @override
  Future<AiAvailability> prepare() async => avail;

  @override
  Stream<AiProgress> run(Photo p, int tasks) {
    runs.add(tasks);
    late StreamController<AiProgress> c;
    c = StreamController<AiProgress>(onCancel: () => cancels++);
    _ctrls.add(c);
    return c.stream;
  }

  String get _tapId => engineTaps.isEmpty ? '' : planeId('tap:${engineTaps.join(';')}');
  AiTapResult _res(bool ok) => AiTapResult(ok: ok, message: ok ? '' : 'no object', id: ok ? _tapId : '', taps: const [], w: 64, h: 64, ms: 1);

  @override
  Future<AiTapResult> tap(Photo p, double u, double v, {bool add = true}) async {
    log.add('tap');
    taps.add((u: u, v: v, add: add));
    if (tapHold != null) await tapHold!.future;
    if (!tapOk) return _res(false);
    engineTaps.add('${add ? '+' : '-'}${(u * 1000).round()},${(v * 1000).round()}');
    return _res(true);
  }

  @override
  Future<AiTapResult> undoTap(Photo p) async {
    log.add('undo');
    if (engineTaps.isNotEmpty) engineTaps.removeLast();
    return _res(true);
  }

  @override
  Future<AiTapResult> resetTaps(Photo p) async {
    log.add('reset');
    engineTaps.clear();
    return _res(true);
  }

  @override
  void prepareTap(Photo p) => prepareTaps++;

  @override
  Future<List<AiMissing>> missing(Photo p, String paramsJson) async {
    missingCalls++;
    return [for (final id in missingIds) if (paramsJson.contains(id)) AiMissing(id, ai: true, thisPhoto: true)];
  }

  @override
  Future<int> ensure(Photo p, String paramsJson) async {
    ensureCalls++;
    if (ensureFixes) missingIds.clear();
    return missingIds.length;
  }

  @override
  AiPlanePreview? preview(String id) => null;

  @override
  void release({bool all = false}) => releases.add(all);
}

Future<void> tapPhoto(WidgetTester t, {Offset by = Offset.zero}) async {
  final f = await Finger.down(t, frameRect(t).center + by);
  await f.up();
  await settle(t, 3);
}

LocalAdjustment aiArea(String id, String key, {bool on = true}) =>
    LocalAdjustment(id: id, on: on, mask: [MaskComponent(kind: MaskKindId.raster, ref: planeId(key), refine: 50)], ui: LocalUi.centre);

MaskComponent rasterOf(LocalAdjustment a, [int i = 0]) => a.mask[i];

Future<Rig> open(WidgetTester t, FakeLocalAi fake, {bool dark = false, void Function(AppState app, String id)? before, Size? phys, double? dpr}) =>
    openLocal(t, ai: fake, dark: dark, phys: phys ?? phonePhys, dpr: dpr ?? phoneDpr, before: before == null ? null : (app, id) => before(app, id));

void main() {
  group('the shape card', () {
    for (final dark in [false, true]) {
      testWidgets('${dark ? 'dark' : 'light'}: five shapes, then 人物 背景 点选 天空(粗略) 远近 on a second row', (t) async {
        final fake = FakeLocalAi();
        await open(t, fake, dark: dark);
        final shapes = ['linear', 'radial', 'brush', 'luminance', 'color'], ai = ['person', 'background', 'tap', 'sky', 'depth'];
        for (final k in [...shapes, ...ai]) {
          expect(key('kind-$k'), findsOneWidget, reason: k);
        }
        final y1 = t.getCenter(key('kind-linear')).dy, y2 = t.getCenter(key('kind-person')).dy;
        expect(y2, greaterThan(y1 + 20), reason: 'second row');
        for (final k in shapes) {
          expect(t.getCenter(key('kind-$k')).dy, y1, reason: k);
        }
        var x = -1.0;
        for (final k in ai) {
          final c = t.getCenter(key('kind-$k'));
          expect(c.dy, y2, reason: k);
          expect(c.dx, greaterThan(x), reason: 'order $k');
          x = c.dx;
        }
        for (final w in ['人物', '背景', '点选', '天空', '远近', '粗略']) {
          expect(find.text(w), findsOneWidget, reason: w);
        }
        expect(find.descendant(of: key('kind-sky'), matching: find.text('粗略')), findsOneWidget);
        expect(find.descendant(of: key('kind-depth'), matching: find.text('粗略')), findsNothing);
        expect(t.takeException(), isNull);
      });
    }

    testWidgets('a device without AI offers the five shapes only', (t) async {
      final fake = FakeLocalAi()..avail = const AiAvailability();
      await open(t, fake);
      expect(key('kind-radial'), findsOneWidget);
      for (final k in ['person', 'background', 'tap', 'sky', 'depth']) {
        expect(key('kind-$k'), findsNothing, reason: k);
      }
    });
  });

  group('人物', () {
    testWidgets('progress, cancel, again, parts, two picks make ONE area with two raster parts', (t) async {
      final fake = FakeLocalAi();
      final r = await open(t, fake);
      await tapKey(t, 'kind-person');
      expect(fake.runs, [AiBits.person | AiBits.faces]);
      expect(key('parts-progress'), findsOneWidget);
      fake.progress(0.4);
      await settle(t, 2);
      expect(t.widget<CircularProgressIndicator>(find.descendant(of: key('parts-progress'), matching: find.byType(CircularProgressIndicator))).value, closeTo(0.4, 1e-9));
      expect(r.lc.aiProgress, closeTo(0.4, 1e-9));

      await tapKey(t, 'parts-cancel');
      expect(fake.cancels, 1);
      expect(key('parts-progress'), findsNothing);
      expect(r.lc.aiRunning, 0);
      expect(key('kind-person'), findsOneWidget, reason: 'back on the shape card');
      expect(r.local.isEmpty, isTrue);

      await tapKey(t, 'kind-person');
      expect(fake.runs, hasLength(2));
      expect(key('parts-progress'), findsOneWidget);
      fake.finish(portrait());
      await settle(t, 3);
      expect(key('parts-progress'), findsNothing);
      for (final (k, label) in [('person', '整个人'), ('faceskin', '脸部皮肤'), ('hair', '头发'), ('clothes', '衣服'), ('f1:eyes', '眼睛'), ('f1:lips', '嘴唇')]) {
        expect(key('part-$k'), findsOneWidget, reason: k);
        expect(find.descendant(of: key('part-$k'), matching: find.text(label)), findsOneWidget, reason: label);
      }
      expect(key('part-bodyskin'), findsNothing, reason: 'only planes that exist');
      expect(key('part-f1:teeth'), findsNothing);
      expect(key('part-background'), findsNothing, reason: 'background is not a part');
      expect(find.text('脸'), findsOneWidget);
      expect(key('parts-none'), findsNothing);

      await tapKey(t, 'part-faceskin');
      await t.ensureVisible(key('part-f1:lips'));
      await tapKey(t, 'part-f1:lips');
      expect(r.lc.partPick, {'faceskin', 'f1:lips'});
      await tapKey(t, 'parts-add');
      expect(r.local.length, 1);
      final a = area(r);
      expect(a.mask, hasLength(2));
      expect([for (final m in a.mask) m.kind], [MaskKindId.raster, MaskKindId.raster]);
      expect([for (final m in a.mask) m.op], [MaskOp.add, MaskOp.add]);
      expect([for (final m in a.mask) m.ref], [planeId('faceskin'), planeId('f1:lips')]);
      expect(key('part-faceskin'), findsNothing, reason: 'the card closed');
      expect(key('local-pad-light'), findsOneWidget);
      expect(r.lc.selId, a.id);
      r.app.undo(r.id);
      expect(r.local.isEmpty, isTrue, reason: 'one undo step');
      await settle(t, 3);

      // the analysis is cached: 背景 needs no new run
      await tapKey(t, 'kind-background');
      expect(fake.runs, hasLength(2));
      expect(r.local.length, 1);
      expect(area(r).mask.single.ref, planeId('background'));
      expect(area(r).mask.single.op, MaskOp.add);
      expect(r.lc.debugAiRuns, 1, reason: 'one finished analysis');
      expect(t.takeException(), isNull);
    });

    testWidgets('a second person pick on an existing area adds parts; 减去 subtracts', (t) async {
      final fake = FakeLocalAi();
      final r = await open(t, fake, before: (app, id) => app.applyEdit(id, (e) => e.copyWith(local: LocalList([aiArea('s1', 'sky')]))));
      await tapKey(t, 'local-chip-0');
      await tapKey(t, 'act-减一块');
      await tapKey(t, 'kind-person');
      fake.finish(portrait());
      await settle(t, 3);
      expect(find.descendant(of: key('parts-add'), matching: find.text('减去')), findsOneWidget);
      await tapKey(t, 'part-hair');
      await tapKey(t, 'parts-add');
      expect(r.local.length, 1);
      expect(area(r).mask, hasLength(2));
      expect(area(r).mask[1].op, MaskOp.subtract);
      expect(area(r).mask[1].ref, planeId('hair'));
    });

    testWidgets('no person: 没找到人 and the 背景 tile disappears', (t) async {
      final fake = FakeLocalAi();
      final r = await open(t, fake);
      await tapKey(t, 'kind-person');
      fake.finish(result(person: false));
      await settle(t, 3);
      expect(key('parts-none'), findsOneWidget);
      expect(find.text('没找到人'), findsOneWidget);
      expect(key('parts-add'), findsNothing);
      await back(t);
      expect(key('parts-none'), findsNothing);
      expect(key('kind-background'), findsNothing);
      expect(key('kind-person'), findsOneWidget, reason: 'stays: its card says why');
      expect(key('kind-sky'), findsOneWidget);
      expect(r.local.isEmpty, isTrue);
      expect(t.takeException(), isNull);
    });
  });

  group('天空', () {
    testWidgets('tap while not analysed: the tile shows its ring; the result makes a sky area', (t) async {
      final fake = FakeLocalAi();
      final r = await open(t, fake);
      expect(key('kind-progress-sky'), findsNothing);
      await tapKey(t, 'kind-sky');
      expect(fake.runs, [AiBits.sky]);
      expect(key('kind-progress-sky'), findsOneWidget);
      expect(key('kind-progress-person'), findsNothing);
      fake.progress(0.5, stage: 'sky');
      await settle(t, 2);
      expect(t.widget<CircularProgressIndicator>(find.descendant(of: key('kind-progress-sky'), matching: find.byType(CircularProgressIndicator))).value, closeTo(0.5, 1e-9));
      fake.finish(result(person: false, sky: true, planes: [plane('sky', group: 'sky')]));
      await settle(t, 3);
      expect(key('kind-progress-sky'), findsNothing);
      expect(r.local.length, 1);
      expect(area(r).mask.single.kind, MaskKindId.raster);
      expect(area(r).mask.single.ref, planeId('sky'));
      expect(area(r).mask.single.op, MaskOp.add);
      expect(r.lc.selId, area(r).id);
      expect(key('local-pad-light'), findsOneWidget);
      r.app.undo(r.id);
      expect(r.local.isEmpty, isTrue);
      await settle(t, 3);
      // analysed now: the next 天空 needs no run
      await tapKey(t, 'kind-sky');
      expect(fake.runs, hasLength(1));
      expect(r.local.length, 1);
    });

    testWidgets('no sky: toast 没找到天空 and the tile disappears', (t) async {
      final fake = FakeLocalAi();
      final r = await open(t, fake);
      await tapKey(t, 'kind-sky');
      fake.finish(result(person: false, sky: false));
      await settle(t, 3);
      expect(r.app.toast?.message, '没找到天空');
      expect(r.local.isEmpty, isTrue);
      expect(key('kind-sky'), findsNothing);
      expect(key('kind-depth'), findsOneWidget);
      expect(t.takeException(), isNull);
    });

    testWidgets('a second tap on a running 天空 tile cancels', (t) async {
      final fake = FakeLocalAi();
      final r = await open(t, fake);
      await tapKey(t, 'kind-sky');
      expect(key('kind-progress-sky'), findsOneWidget);
      await tapKey(t, 'kind-sky');
      expect(fake.cancels, 1);
      expect(fake.runs, hasLength(1));
      expect(key('kind-progress-sky'), findsNothing);
      expect(r.lc.aiRunning, 0);
      expect(r.local.isEmpty, isTrue);
      expect(key('kind-sky'), findsOneWidget);
    });

    testWidgets('an analysis error toasts and leaves the card usable', (t) async {
      final fake = FakeLocalAi();
      final r = await open(t, fake);
      await tapKey(t, 'kind-sky');
      fake.finish(result(state: 'error'));
      await settle(t, 3);
      expect(r.app.toast?.message, '没能分析这张照片');
      expect(r.local.isEmpty, isTrue);
      expect(key('kind-progress-sky'), findsNothing);
      expect(key('kind-sky'), findsOneWidget);
    });
  });

  group('远近', () {
    testWidgets('a raster part with lo 0 hi 30; the range bar is on the strip; dragging changes lo/hi in one undo step', (t) async {
      final fake = FakeLocalAi();
      final r = await open(t, fake);
      await tapKey(t, 'kind-depth');
      expect(fake.runs, [AiBits.depth]);
      expect(key('kind-progress-depth'), findsOneWidget);
      fake.finish(result(person: false, planes: [plane('depth', group: 'depth')]));
      await settle(t, 3);
      expect(r.local.length, 1);
      final m = area(r).mask.single;
      expect(m.ref, planeId('depth'));
      expect(m.lo, 0);
      expect(m.hi, 30);
      final j = m.toJson();
      expect(j['lo'], 0);
      expect(j['hi'], 30);
      expect(j['feather'], 10);
      expect(j['kind'], 'raster');
      expect(key('local-tools'), findsOneWidget);
      expect(key('tone-bar'), findsOneWidget);
      expect(find.text('近'), findsOneWidget);
      expect(find.text('远'), findsOneWidget);

      final bar = t.getRect(key('tone-bar'));
      final f = await Finger.down(t, Offset(bar.left + 0.3 * bar.width, bar.center.dy));
      await f.drag(const Offset(40, 0), step: 5);
      await f.up();
      await settle(t, 2);
      final n = area(r).mask.single;
      expect(n.lo, 0);
      expect(n.hi, greaterThan(40));
      expect(n.hi, lessThan(60));
      expect(n.toJson()['hi'], n.hi);
      r.app.undo(r.id);
      expect(r.local.length, 1);
      expect(area(r).mask.single.hi, 30, reason: 'one undo took the whole drag back');
      expect(area(r).mask.single.lo, 0);
      expect(t.takeException(), isNull);
    });
  });

  group('点选', () {
    testWidgets('an area that is OFF with an empty ref; a tap makes it ON with the returned id, one undo step per tap', (t) async {
      final fake = FakeLocalAi();
      final r = await open(t, fake);
      await tapKey(t, 'kind-tap');
      expect(r.local.length, 1);
      expect(area(r).on, isFalse);
      expect(area(r).mask.single.kind, MaskKindId.raster);
      expect(area(r).mask.single.ref, '');
      expect(fake.prepareTaps, 1);
      expect(fake.log, ['reset']);
      expect(key('tap-add'), findsOneWidget);
      expect(key('tap-sub'), findsOneWidget);
      expect(key('tap-undo'), findsOneWidget);
      expect(key('tap-busy'), findsNothing);

      await tapPhoto(t);
      expect(fake.taps, hasLength(1));
      expect(fake.taps.single.u, inInclusiveRange(0, 1));
      expect(fake.taps.single.v, inInclusiveRange(0, 1));
      expect(fake.taps.single.u, closeTo(0.5, 0.1));
      expect(fake.taps.single.add, isTrue);
      expect(area(r).on, isTrue);
      final first = area(r).mask.single.ref;
      expect(first, startsWith('ai:$photoHash:tap:+'));
      final parsed = tapsOfId(first);
      expect(parsed, hasLength(1));
      expect(parsed.single.$1, closeTo(fake.taps.single.u, 0.001));
      expect(parsed.single.$3, isTrue);

      await tapPhoto(t, by: const Offset(30, 20));
      expect(fake.taps, hasLength(2));
      expect(tapsOfId(area(r).mask.single.ref), hasLength(2));
      r.app.undo(r.id);
      expect(area(r).mask.single.ref, first, reason: 'one undo step per tap');
      expect(area(r).on, isTrue);
      r.app.undo(r.id);
      expect(area(r).mask.single.ref, '');
      expect(area(r).on, isFalse);
      r.app.undo(r.id);
      expect(r.local.isEmpty, isTrue);
      expect(t.takeException(), isNull);
    });

    testWidgets('tap-sub makes the next tap a subtract; tap-undo calls undoTap and takes the last tap off the ref', (t) async {
      final fake = FakeLocalAi();
      final r = await open(t, fake);
      await tapKey(t, 'kind-tap');
      await tapPhoto(t);
      await tapKey(t, 'tap-sub');
      expect(r.lc.tapAdd, isFalse);
      await tapPhoto(t, by: const Offset(40, -20));
      expect(fake.taps.last.add, isFalse);
      expect(tapsOfId(area(r).mask.single.ref).map((e) => e.$3), [true, false]);
      await tapKey(t, 'tap-add');
      expect(r.lc.tapAdd, isTrue);
      await tapPhoto(t, by: const Offset(-40, 30));
      expect(fake.taps.last.add, isTrue);
      expect(tapsOfId(area(r).mask.single.ref), hasLength(3));

      await tapKey(t, 'tap-undo');
      expect(fake.log.last, 'undo');
      expect(tapsOfId(area(r).mask.single.ref), hasLength(2));
      await tapKey(t, 'tap-undo');
      await tapKey(t, 'tap-undo');
      expect(fake.log.where((e) => e == 'undo'), hasLength(3));
      expect(area(r).mask.single.ref, '');
      expect(area(r).on, isFalse, reason: 'no tap left: no plane');
      expect(t.takeException(), isNull);
    });

    testWidgets('tap-busy shows while a tap future is pending', (t) async {
      final fake = FakeLocalAi();
      await open(t, fake);
      await tapKey(t, 'kind-tap');
      fake.tapHold = Completer<void>();
      await tapPhoto(t);
      expect(key('tap-busy'), findsOneWidget);
      fake.tapHold!.complete();
      await settle(t, 3);
      expect(key('tap-busy'), findsNothing);
    });

    testWidgets('a failed tap toasts 没选到 and keeps the area off', (t) async {
      final fake = FakeLocalAi()..tapOk = false;
      final r = await open(t, fake);
      await tapKey(t, 'kind-tap');
      await tapPhoto(t);
      expect(r.app.toast?.message, '没选到');
      expect(area(r).on, isFalse);
      expect(area(r).mask.single.ref, '');
      expect(key('tap-busy'), findsNothing);
    });

    testWidgets('another tap area used the engine session: the first area resets and replays its taps', (t) async {
      final fake = FakeLocalAi();
      final r = await open(t, fake);
      await tapKey(t, 'kind-tap');
      await tapPhoto(t);
      await tapPhoto(t, by: const Offset(30, 0));
      final aRef = area(r).mask.single.ref;
      expect(tapsOfId(aRef), hasLength(2));

      await tapKey(t, 'local-add');
      await addViaCard(t, 'tap');
      expect(r.local.length, 2);
      expect(fake.log.last, 'reset');
      await tapPhoto(t, by: const Offset(0, 40));
      final bRef = area(r, 1).mask.single.ref;
      expect(tapsOfId(bRef), hasLength(1));
      expect(fake.engineTaps, hasLength(1), reason: 'the session holds B only');

      await tapKey(t, 'local-chip-0');
      expect(r.lc.selId, area(r).id);
      final mark = fake.log.length, nTaps = fake.taps.length;
      await tapPhoto(t, by: const Offset(-30, 0));
      expect(fake.log.sublist(mark), ['reset', 'tap', 'tap', 'tap'], reason: 'reset, replay the two taps of A, then the new one');
      final replay = fake.taps.sublist(nTaps, nTaps + 2);
      expect(replay.map((e) => (e.u * 1000).round()), tapsOfId(aRef).map((e) => (e.$1 * 1000).round()));
      final aNew = area(r).mask.single.ref;
      expect(tapsOfId(aNew), hasLength(3));
      expect(aNew, startsWith(aRef));
      expect(area(r, 1).mask.single.ref, bRef, reason: 'B untouched');

      // A is now the session's: another tap on A does not reset again
      final mark2 = fake.log.length;
      await tapPhoto(t, by: const Offset(60, 0));
      expect(fake.log.sublist(mark2), ['tap']);
    });
  });

  group('brush correction of an AI area', () {
    testWidgets('涂抹补充 / 涂抹擦除 add brush parts (subtract / add) and select them', (t) async {
      final fake = FakeLocalAi();
      final r = await open(t, fake, before: (app, id) => app.applyEdit(id, (e) => e.copyWith(local: LocalList([aiArea('s1', 'sky')]))));
      expect(key('local-tools'), findsNothing, reason: 'a single sky part: no tools');
      await tapKey(t, 'local-chip-0');
      expect(key('act-涂抹补充'), findsOneWidget);
      expect(key('act-涂抹擦除'), findsOneWidget);
      await tapKey(t, 'act-涂抹擦除');
      expect(area(r).mask, hasLength(2));
      expect(area(r).mask[1].kind, MaskKindId.brush);
      expect(area(r).mask[1].op, MaskOp.subtract);
      expect(r.lc.selComp, 1);
      expect(r.lc.body, LocalBody.pads);
      expect(key('local-tools'), findsOneWidget);
      expect(key('brush-size-0'), findsOneWidget);
      expect(key('brush-erase'), findsOneWidget);

      await tapKey(t, 'local-chip-0');
      await tapKey(t, 'act-涂抹补充');
      expect(area(r).mask, hasLength(3));
      expect(area(r).mask[2].kind, MaskKindId.brush);
      expect(area(r).mask[2].op, MaskOp.add);
      expect(r.lc.selComp, 2);
      expect(key('brush-size-0'), findsOneWidget);
      expect(t.takeException(), isNull, reason: 'three parts fit the tool strip on a phone');

      // asking again reuses the part
      await tapKey(t, 'local-chip-0');
      await tapKey(t, 'act-涂抹擦除');
      expect(area(r).mask, hasLength(3));
      expect(r.lc.selComp, 1);
      expect(t.takeException(), isNull);
    });

    testWidgets('a classic area does not offer them', (t) async {
      final fake = FakeLocalAi();
      await open(t, fake);
      await addViaCard(t, 'radial');
      await tapKey(t, 'local-chip-0');
      expect(key('act-涂抹补充'), findsNothing);
      expect(key('act-涂抹擦除'), findsNothing);
      expect(key('act-删除'), findsOneWidget);
    });
  });

  group('layout of the parts card and the tool strip', () {
    testWidgets('part chips are compact, several per row', (t) async {
      final fake = FakeLocalAi();
      await open(t, fake);
      await tapKey(t, 'kind-person');
      fake.finish(portrait());
      await settle(t, 3);
      final a = t.getRect(key('part-person')), b = t.getRect(key('part-faceskin'));
      expect(a.width, lessThan(150));
      expect(b.top, a.top, reason: 'the chips share a row');
    });

    testWidgets('an area with an AI part and two brush parts: the tool strip fits a phone', (t) async {
      final fake = FakeLocalAi();
      final r = await open(t, fake, before: (app, id) => app.applyEdit(id, (e) => e.copyWith(local: LocalList([aiArea('s1', 'sky')]))));
      await tapKey(t, 'local-chip-0');
      await tapKey(t, 'act-涂抹擦除');
      await tapKey(t, 'local-chip-0');
      await tapKey(t, 'act-涂抹补充');
      expect(area(r).mask, hasLength(3));
      expect(t.takeException(), isNull);
    });
  });

  group('missing planes', () {
    testWidgets('ensure runs on opening; what stays missing shows 重新分析; the button asks again; fixed, it goes', (t) async {
      final fake = FakeLocalAi()..missingIds.add(planeId('sky'));
      final r = await open(t, fake, before: (app, id) => app.applyEdit(id, (e) => e.copyWith(local: LocalList([aiArea('s1', 'sky')]))));
      expect(fake.ensureCalls, 1);
      expect(fake.missingCalls, 2, reason: 'before and after the repair');
      expect(r.lc.missingRefs, {planeId('sky')});
      expect(key('local-tools'), findsOneWidget);
      expect(key('ai-reanalyse'), findsOneWidget);
      expect(find.text('重新分析'), findsOneWidget);

      await tapKey(t, 'ai-reanalyse');
      expect(fake.ensureCalls, 2);
      expect(r.app.toast?.message, '没能重新分析');
      expect(key('ai-reanalyse'), findsOneWidget);

      fake.ensureFixes = true;
      await tapKey(t, 'ai-reanalyse');
      expect(fake.ensureCalls, 3);
      expect(r.lc.missingRefs, isEmpty);
      expect(key('ai-reanalyse'), findsNothing);
      expect(key('local-tools'), findsNothing);
      expect(t.takeException(), isNull);
    });

    testWidgets('nothing missing: ensure is never called', (t) async {
      final fake = FakeLocalAi();
      await open(t, fake, before: (app, id) => app.applyEdit(id, (e) => e.copyWith(local: LocalList([aiArea('s1', 'sky')]))));
      expect(fake.missingCalls, 1);
      expect(fake.ensureCalls, 0);
      expect(key('ai-reanalyse'), findsNothing);
    });

    testWidgets('no AI areas: the service is not asked at all', (t) async {
      final fake = FakeLocalAi();
      await open(t, fake);
      expect(fake.missingCalls, 0);
      expect(fake.ensureCalls, 0);
    });
  });

  group('memory', () {
    testWidgets('memory pressure releases everything; leaving the editor does too', (t) async {
      final fake = FakeLocalAi();
      await open(t, fake);
      expect(fake.releases, isEmpty);
      t.binding.handleMemoryPressure();
      await settle(t, 1);
      expect(fake.releases, [true]);
      await t.pumpWidget(const SizedBox());
      await settle(t, 2);
      expect(fake.releases, [true, true]);
    });
  });

  group('the model of an AI area', () {
    MaskComponent raster(String k, {MaskOp op = MaskOp.add, double lo = 0, double hi = 100, double feather = 10}) =>
        MaskComponent(kind: MaskKindId.raster, op: op, ref: planeId(k), refine: 50, lo: lo, hi: hi, lumFeather: feather);

    test('raster parts with lo / hi / feather round-trip through EditState JSON and show in the wire', () {
      final a = LocalAdjustment(
        id: 'ai1',
        mask: [raster('depth', hi: 30, feather: 12), raster('hair', op: MaskOp.subtract), raster('f1:lips'), const MaskComponent(kind: MaskKindId.brush, op: MaskOp.add)],
        ui: LocalUi.centre,
      );
      final off = LocalAdjustment(id: 'tap1', on: false, mask: const [MaskComponent(kind: MaskKindId.raster, ref: '')], ui: LocalUi.centre);
      final e = EditState.empty.copyWith(local: LocalList([a, off]));
      final back = EditState.fromJson(jsonDecode(jsonEncode(e.toJson())) as Map<String, dynamic>);
      expect(back.local.same(e.local), isTrue);
      expect(back.sameAs(e), isTrue);
      final m = back.local.items.first.mask;
      expect(m[0].ref, planeId('depth'));
      expect(m[0].lo, 0);
      expect(m[0].hi, 30);
      expect(m[0].lumFeather, 12);
      expect(m[1].op, MaskOp.subtract);
      expect(m[2].ref, planeId('f1:lips'));
      expect(m[2].hasRange, isFalse);
      expect(back.local.items[1].on, isFalse);
      expect(back.local.items[1].mask.single.ref, '');

      final wire = e.toWire(defaultLook: 'std', defaultToneStrength: 60);
      final items = (wire['local'] as Map)['items'] as List;
      final w0 = (items[0] as Map)['mask'] as List;
      expect(w0[0], {'kind': 'raster', 'op': 'add', 'ref': planeId('depth'), 'refine': 50, 'lo': 0, 'hi': 30, 'feather': 12});
      expect((w0[1] as Map)['op'], 'subtract');
      expect((w0[2] as Map).containsKey('lo'), isFalse, reason: 'identity range is not written');
      expect((items[0] as Map).containsKey('ui'), isFalse);
      expect((items[1] as Map)['on'], false);
      // the wire survives a JSON round trip to the engine's reader
      final again = LocalList.fromJson(jsonDecode(jsonEncode(wire['local'])));
      expect(again.items.first.mask[0].hi, 30);
      expect(again.items.first.mask[0].lumFeather, 12);
    });
  });

  group('layout across window shapes', () {
    final windows = <(String, Size, double, bool)>[
      ('phone', phonePhys, phoneDpr, false),
      ('owner fold outer', ownerPhys, ownerDpr, true),
      ('fold inner (side pane)', n5Phys, n5Dpr, false),
      ('landscape phone (side pane)', const Size(2400, 1080), 2.75, true),
      ('tablet', const Size(2560, 1600), 2, false),
    ];
    for (final (name, phys, dpr, dark) in windows) {
      testWidgets('$name ${dark ? 'dark' : 'light'}: card, parts, tap tools, actions, depth bar, reanalyse', (t) async {
        final fake = FakeLocalAi();
        final r = await open(t, fake, dark: dark, phys: phys, dpr: dpr);
        expect(t.takeException(), isNull, reason: 'card');
        await tapKey(t, 'kind-sky');
        fake.progress(0.3, stage: 'sky');
        await settle(t, 2);
        expect(t.takeException(), isNull, reason: 'tile ring');
        await tapKey(t, 'kind-sky');
        expect(t.takeException(), isNull, reason: 'cancelled');

        await tapKey(t, 'kind-person');
        fake.progress(0.6);
        await settle(t, 2);
        expect(key('parts-progress'), findsOneWidget);
        expect(t.takeException(), isNull, reason: 'parts progress');
        fake.finish(portrait());
        await settle(t, 3);
        expect(key('part-f1:lips'), findsOneWidget);
        expect(t.takeException(), isNull, reason: 'parts');
        r.lc.togglePart('faceskin');
        await settle(t, 2);
        await tapKey(t, 'parts-add');
        expect(r.local.length, 1, reason: 'parts added');
        expect(t.takeException(), isNull, reason: 'area with parts');

        await tapKey(t, 'local-chip-0');
        expect(key('act-涂抹擦除'), findsOneWidget);
        expect(t.takeException(), isNull, reason: 'actions with AI tiles');
        await tapKey(t, 'act-涂抹擦除');
        expect(key('brush-erase'), findsOneWidget);
        expect(t.takeException(), isNull, reason: 'brush fix tools');

        await tapKey(t, 'local-add');
        await addViaCard(t, 'tap');
        expect(key('tap-undo'), findsOneWidget);
        expect(t.takeException(), isNull, reason: 'tap tools');
        fake.tapHold = Completer<void>();
        await tapPhoto(t);
        expect(key('tap-busy'), findsOneWidget);
        expect(t.takeException(), isNull, reason: 'tap busy');
        fake.tapHold!.complete();
        await settle(t, 3);

        await tapKey(t, 'local-add');
        await addViaCard(t, 'depth');
        fake.finish(result(person: false, planes: [plane('depth', group: 'depth')]));
        await settle(t, 3);
        expect(key('tone-bar'), findsOneWidget);
        expect(t.takeException(), isNull, reason: 'depth bar');

        fake.missingIds.add(planeId('depth'));
        await r.lc.checkMissing();
        await settle(t, 3);
        expect(key('ai-reanalyse'), findsOneWidget);
        expect(t.takeException(), isNull, reason: 'reanalyse');
      });
    }
  });
}
