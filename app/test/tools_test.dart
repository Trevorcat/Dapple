import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:dapple/models.dart';
import 'package:dapple/services/detection.dart';
import 'package:dapple/services/ftp_ingest.dart';
import 'package:dapple/services/ingest.dart';
import 'package:dapple/services/live_bridge.dart';
import 'package:dapple/services/merge_engine.dart';
import 'package:dapple/services/tool_models.dart';
import 'package:dapple/screens/tools/live_tool_page.dart';
import 'package:dapple/screens/tools/merge_tool_page.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/widgets/icons.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import 'support/harness.dart';

/// Engine stand-in: records requests, can hold a merge open (progress), fail it, or honour cancel.
class FakeEngine implements MergeEngine {
  final List<MergeRequest> merges = [];
  final List<Map<String, dynamic>> motion = [];
  Completer<void>? gate;
  bool failNext = false;
  double progressAtGate = 0.5;

  @override
  bool get available => true;

  @override
  Future<MergeOutcome> merge(MergeRequest r) async {
    merges.add(r);
    r.handle.report(progressAtGate);
    final g = gate;
    if (g != null) {
      await g.future;
      if (r.handle.cancelled) return MergeOutcome.fail('cancelled', cancelled: true);
    }
    if (failNext) {
      failNext = false;
      return MergeOutcome.fail('Could not align frame 2');
    }
    return MergeOutcome.ok({'kind': r.kind});
  }

  @override
  Future<Map<String, dynamic>?> detect(List<String> paths, MergeHandle handle, {Map<String, dynamic> options = const {}}) async =>
      null;

  @override
  Future<Map<String, dynamic>> writeMotionPhoto(
      {required String jpeg, required String mp4, required String out, required double coverSec}) async {
    motion.add({'jpeg': jpeg, 'mp4': mp4, 'out': out, 'cover': coverSec});
    return {'valid': true};
  }

  @override
  Future<Map<String, dynamic>> motionFromVideo(
          {required String video,
          required String jpeg,
          required String out,
          required double start,
          required double duration,
          required double cover,
          required bool keepAudio}) async =>
      {};
}

const _t = Duration(milliseconds: 100);

class _NoMergeEngine extends FakeEngine {
  @override
  bool get available => false;
}

Future<(AppState, FakeEngine)> _open(WidgetTester tester) async {
  useWindow(tester, phoneSize);
  final app = AppState.demo();
  final eng = FakeEngine();
  app.engine = eng;
  await pumpDapple(tester, app);
  return (app, eng);
}

Future<void> _settle(WidgetTester tester) async {
  for (var i = 0; i < 4; i++) {
    await tester.pump(const Duration(milliseconds: 150));
  }
}

void main() {
  group('detection model', () {
    test('parseDetection maps paths, drops weak candidates, labels', () {
      final json = {
        'groups': [
          {
            'kind': 'bracket',
            'confidence': 0.65,
            'frames': [
              {'path': '/a.arw', 'evStep': -2.0},
              {'path': '/b.arw', 'evStep': 0.0},
              {'path': '/c.arw', 'evStep': 2.0},
            ]
          },
          {
            'kind': 'panorama',
            'confidence': 0.6,
            'frames': [
              {'path': '/d.arw', 'evStep': 0},
              {'path': '/e.arw', 'evStep': 0},
            ]
          },
          {
            'kind': 'panorama',
            'confidence': 0.9,
            'frames': [
              {'path': '/d.arw', 'evStep': 0},
              {'path': '/e.arw', 'evStep': 0},
            ]
          },
        ]
      };
      final ids = {'/a.arw': 'a', '/b.arw': 'b', '/c.arw': 'c', '/d.arw': 'd', '/e.arw': 'e'};
      final g = parseDetection(json, ids, {'a': 1000, 'b': 2000, 'c': 3000, 'd': 5000, 'e': 6000});
      expect(g.length, 2, reason: 'a 2-frame panorama below 0.75 confidence is dropped');
      expect(g[0].isBracket, isTrue);
      expect(g[0].title, 'HDR · 3 张包围曝光');
      expect(g[0].subtitle, '−2 / 0 / +2 EV');
      expect(g[0].startMs, 1000);
      expect(g[1].title, '全景 · 2 张连续');
      expect(evNumber(0.5), '+0.5');
      expect(evNumber(-1), '−1');
    });

    test('RAW+JPEG pairing helpers and credentials', () {
      expect(pairStem('DSC01234.ARW'), pairStem('dsc01234.JPG'));
      expect(isRawName('x.arw'), isTrue);
      expect(isJpegName('x.JPEG'), isTrue);
      final pw = generateFtpPassword();
      expect(pw.length, 8);
      expect(RegExp(r'^[a-zA-Z0-9]+$').hasMatch(pw), isTrue);
      expect(preferredAddress([FtpAddress('wlan0', '192.168.1.5'), FtpAddress('ap0', '192.168.43.1')])!.ip, '192.168.43.1');
    });

    test('draft: base falls back to the middle frame, nextBase cycles, signature changes', () {
      final d = MergeDraft(kind: ToolKind.hdr, sources: [
        for (var i = 0; i < 3; i++) DraftSource(photoId: 'p$i', label: '$i', timeMs: i),
      ]);
      expect(d.effectiveBaseId, 'p1');
      d.baseId = 'p2';
      expect(d.baseIndex, 2);
      d.sources[2].checked = false;
      expect(d.effectiveBaseId, 'p1', reason: 'unchecked base -> middle of the rest');
      final s1 = d.signature;
      d.deghost = 'high';
      expect(d.signature == s1, isFalse);
      expect(d.options()['deghost'], 'high');
      final pano = MergeDraft(kind: ToolKind.pano, sources: d.sources)..crop = true..fill = true;
      expect(pano.options()['fill'], isFalse, reason: 'fill only applies when crop is off');
      final live = LiveDraft(video: _video(14));
      live.setStart(99);
      expect(live.startSec, closeTo(11, 1e-9));
      expect(live.coverSec, inInclusiveRange(11, 14));
      expect(LiveDraft.clock(6.4), '0:06');
    });
  });

  group('home cards', () {
    testWidgets('detected HDR / panorama / video cards', (tester) async {
      final (app, _) = await _open(tester);
      expect(find.text('HDR · 3 张包围曝光'), findsOneWidget);
      expect(find.text('−2 / 0 / +2 EV'), findsOneWidget);
      expect(find.text('全景 · 4 张连续'), findsOneWidget);
      expect(find.text('视频 → 动态照片'), findsOneWidget);
      expect(find.text('0:14 · 截取 3 秒'), findsOneWidget);
      expect(app.detectedCards.length, 2);
      await shot(tester, 'home_cards');
    });
  });

  group('manual entries and corrections', () {
    Future<void> fixShot(WidgetTester tester, String name) async {
      await tester.runAsync(() async {
        final boundary = shotKey.currentContext!.findRenderObject() as RenderRepaintBoundary;
        final image = await boundary.toImage(pixelRatio: 2.0);
        final bytes = await image.toByteData(format: ui.ImageByteFormat.png);
        final f = File('../build/ui-pages-fix/$name.png');
        f.parent.createSync(recursive: true);
        f.writeAsBytesSync(bytes!.buffer.asUint8List());
      });
    }

    testWidgets('the + menu has the three tools without any detection; each opens its tool with the picker', (tester) async {
      final (app, _) = await _open(tester);
      app.detected = [];
      app.notifyListeners();
      await tester.pump(_t);
      expect(find.byKey(const ValueKey('entry-bracket-y0')), findsNothing, reason: 'nothing detected');
      expect(find.byKey(const ValueKey('entry-panorama-y3')), findsNothing);
      await tester.tap(find.byKey(const ValueKey('import-button')));
      await _settle(tester);
      expect(find.byKey(const ValueKey('tool-hdr')), findsOneWidget);
      expect(find.byKey(const ValueKey('tool-pano')), findsOneWidget);
      expect(find.byKey(const ValueKey('tool-live')), findsOneWidget);
      // HDR 合成 and 全景拼接 have their own icons
      DI iconOf(String k) => tester.widget<DIcon>(find.descendant(of: find.byKey(ValueKey(k)), matching: find.byType(DIcon))).icon;
      expect(iconOf('tool-hdr'), DI.hdr);
      expect(iconOf('tool-pano'), DI.pano);
      expect(iconOf('tool-hdr'), isNot(iconOf('tool-pano')));
      await fixShot(tester, 'plus_menu_tools');
      // nothing chosen yet: the picker of all photos opens at once, not an empty page
      await tester.tap(find.byKey(const ValueKey('tool-hdr')));
      await _settle(tester);
      expect(find.byType(MergeToolPage), findsOneWidget);
      expect(find.byType(BottomSheet), findsOneWidget, reason: 'the picker is already open');
      await fixShot(tester, 'manual_hdr_picker');
      // leaving it with nothing chosen leaves the tool (no empty page)
      app.navigatorKey.currentState!.pop();
      await _settle(tester);
      await tester.pump(const Duration(seconds: 1));
      expect(find.byType(BottomSheet), findsNothing);
      expect(find.byType(MergeToolPage), findsNothing, reason: 'no empty page left behind');

      await tester.tap(find.byKey(const ValueKey('import-button')));
      await _settle(tester);
      await tester.tap(find.byKey(const ValueKey('tool-pano')));
      await _settle(tester);
      expect(find.byType(MergeToolPage), findsOneWidget);
      expect(find.byType(BottomSheet), findsOneWidget);
      app.navigatorKey.currentState!.pop();
      await _settle(tester);
      await tester.pump(const Duration(seconds: 1));
      expect(find.byType(MergeToolPage), findsNothing);

      await tester.tap(find.byKey(const ValueKey('import-button')));
      await _settle(tester);
      await tester.tap(find.byKey(const ValueKey('tool-live')));
      await _settle(tester);
      expect(find.byType(LiveToolPage), findsOneWidget, reason: 'opens without a preselected video: the video picker');
    });

    testWidgets('a detected card can be dismissed (not a group), remembered; the others stay; members are changed inside the tool', (tester) async {
      final (app, _) = await _open(tester);
      expect(find.byKey(const ValueKey('entry-dismiss')), findsNWidgets(2), reason: 'both detected groups, not the video card');
      await fixShot(tester, 'cards_with_dismiss');
      await tester.tap(find.descendant(of: find.byKey(const ValueKey('entry-bracket-y0')), matching: find.byKey(const ValueKey('entry-dismiss'))));
      await _settle(tester);
      expect(app.detectedCards.length, 1);
      expect(find.byKey(const ValueKey('entry-bracket-y0')), findsNothing);
      expect(find.byKey(const ValueKey('entry-panorama-y3')), findsOneWidget);
      expect(find.byKey(const ValueKey('entry-video')), findsOneWidget);
      app.notifyListeners();
      await tester.pump(_t);
      expect(app.detectedCards.any((g) => g.kind == 'bracket'), isFalse, reason: 'stays dismissed');
      // wrong members: the tool's own picker
      await tester.tap(find.byKey(const ValueKey('entry-panorama-y3')));
      await _settle(tester);
      expect(find.byKey(const ValueKey('tool-pick')), findsOneWidget);
    });
  });

  group('HDR tool', () {
    testWidgets('opens from the card with the detected set; toggles, star, picker cancel restores', (tester) async {
      final (app, _) = await _open(tester);
      await tester.tap(find.byKey(const ValueKey('entry-bracket-y0')));
      await _settle(tester);
      expect(find.text('HDR 合成'), findsOneWidget);
      expect(find.text('参与合成 · 3 张'), findsOneWidget);
      expect(find.text('已识别 3 张包围曝光'), findsOneWidget);
      expect(find.text('识别'), findsNWidgets(3));
      expect(find.text('开始合成'), findsOneWidget);
      await shot(tester, 'tool_hdr');

      // base frame: middle by default; the star moves it
      String star(String id) => find.byKey(ValueKey('base-$id')).evaluate().isEmpty ? '设为基准帧' : '基准帧';
      expect(star('y1'), '基准帧');
      expect(star('y0'), '设为基准帧');
      await tester.tap(find.byKey(const ValueKey('star-y0')));
      await tester.pump(_t);
      expect(star('y0'), '基准帧');
      expect(star('y1'), '设为基准帧');

      // un-checking keeps the frame in the strip (greyed) and updates the count
      await tester.tap(find.byKey(const ValueKey('src-y2')));
      await tester.pump(_t);
      expect(find.text('参与合成 · 2 张'), findsOneWidget);
      expect(find.byKey(const ValueKey('src-y2')), findsOneWidget);
      await tester.tap(find.byKey(const ValueKey('src-y1')));
      await tester.pump(_t);
      expect(find.text('至少选 2 张'), findsOneWidget);
      await tester.tap(find.byKey(const ValueKey('src-y1')));
      await tester.pump(_t);
      expect(find.text('开始合成'), findsOneWidget);

      // picker: add a photo, then cancel -> selection as before
      await tester.tap(find.byKey(const ValueKey('tool-pick')));
      await _settle(tester);
      expect(find.text('已选 2 张'), findsOneWidget);
      final extra = app.pickerCandidates.firstWhere((p) => p.kind == PhotoKind.photo);
      await tester.ensureVisible(find.byKey(ValueKey('pick-${extra.id}')));
      await tester.tap(find.byKey(ValueKey('pick-${extra.id}')));
      await tester.pump(_t);
      expect(find.text('已选 3 张'), findsOneWidget);
      await shot(tester, 'tool_hdr_picker');
      await tester.tap(find.byKey(const ValueKey('pick-cancel')));
      await _settle(tester);
      expect(find.text('参与合成 · 2 张'), findsOneWidget);

      // ... and 完成 applies it
      await tester.tap(find.byKey(const ValueKey('tool-pick')));
      await _settle(tester);
      await tester.ensureVisible(find.byKey(ValueKey('pick-${extra.id}')));
      await tester.tap(find.byKey(ValueKey('pick-${extra.id}')));
      await tester.pump(_t);
      await tester.tap(find.byKey(const ValueKey('pick-done')));
      await _settle(tester);
      expect(find.text('参与合成 · 3 张'), findsOneWidget);
      expect(find.byKey(ValueKey('src-${extra.id}')), findsOneWidget);
    });

    testWidgets('parameters reach the engine; progress dialog, cancel keeps the page', (tester) async {
      final (app, eng) = await _open(tester);
      eng.gate = Completer<void>();
      await tester.tap(find.byKey(const ValueKey('entry-bracket-y0')));
      await _settle(tester);
      await tester.tap(find.descendant(of: find.byKey(const ValueKey('tool-deghost')), matching: find.text('高')));
      await tester.pump(_t);
      await tester.tap(find.byKey(const ValueKey('tool-align')));
      await tester.pump(_t);
      await tester.tap(find.byKey(const ValueKey('tool-main')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      expect(find.text('正在合成 HDR'), findsOneWidget);
      expect(find.byKey(const ValueKey('tool-dialog')), findsOneWidget);
      expect(find.textContaining('对齐 → 合并 → 处理'), findsOneWidget);
      expect(find.byKey(const ValueKey('tool-dialog-percent')), findsOneWidget);
      expect(eng.merges.single.kind, 'hdr');
      expect(eng.merges.single.options['deghost'], 'high');
      expect(eng.merges.single.options['align'], false);
      expect(eng.merges.single.options['base'], 1);
      expect(eng.merges.single.paths.length, 3);
      await shot(tester, 'tool_hdr_progress');
      await tester.tap(find.byKey(const ValueKey('tool-cancel-job')));
      await tester.pump();
      eng.gate!.complete();
      await _settle(tester);
      expect(find.byKey(const ValueKey('tool-dialog')), findsNothing);
      expect(find.text('HDR 合成'), findsOneWidget, reason: 'cancel returns to the tool page');
      expect(find.text('参与合成 · 3 张'), findsOneWidget);
      expect(app.job, isNull);
    });

    testWidgets('success: back home, result is a normal photo, toast', (tester) async {
      final (app, eng) = await _open(tester);
      await tester.tap(find.byKey(const ValueKey('entry-bracket-y0')));
      await _settle(tester);
      final before = app.photos.length;
      await tester.tap(find.byKey(const ValueKey('tool-main')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 600));
      await tester.pump(const Duration(milliseconds: 600));
      expect(find.text('HDR 合成'), findsNothing);
      expect(app.photos.length, before + 1);
      final r = app.photos.first;
      expect(r.kind, PhotoKind.hdr);
      expect(r.badge, 'HDR');
      expect(r.status, PhotoStatus.done);
      expect(find.text('HDR已加入「今天」'), findsOneWidget);
      expect(eng.merges.length, 1);
    });

    testWidgets('failure: 没能对齐这几张, 换基准帧 switches base without retrying, then success', (tester) async {
      final (app, eng) = await _open(tester);
      eng.failNext = true;
      await tester.tap(find.byKey(const ValueKey('entry-bracket-y0')));
      await _settle(tester);
      await tester.tap(find.byKey(const ValueKey('tool-main')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 400));
      expect(find.text('没能对齐这几张'), findsOneWidget);
      expect(find.text('换基准帧'), findsOneWidget);
      await shot(tester, 'tool_hdr_failed');
      await tester.tap(find.byKey(const ValueKey('tool-change-base')));
      await _settle(tester);
      expect(find.byKey(const ValueKey('tool-dialog')), findsNothing);
      expect(eng.merges.length, 1, reason: 'no automatic retry');
      // base moved from the middle (y1) to the next checked frame (y2)
      expect(find.byKey(const ValueKey('base-y2')), findsOneWidget);
      expect(find.byKey(const ValueKey('base-y1')), findsNothing);
      await tester.tap(find.byKey(const ValueKey('tool-main')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 600));
      await tester.pump(const Duration(milliseconds: 600));
      expect(eng.merges.length, 2);
      expect(eng.merges.last.options['base'], 2);
      expect(app.photos.first.kind, PhotoKind.hdr);
    });

    testWidgets('background: task bar with percent, completion toast; failed background offers 查看', (tester) async {
      final (app, eng) = await _open(tester);
      eng.gate = Completer<void>();
      await tester.tap(find.byKey(const ValueKey('entry-bracket-y0')));
      await _settle(tester);
      await tester.tap(find.byKey(const ValueKey('tool-main')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 300));
      await tester.tap(find.byKey(const ValueKey('tool-background')));
      await _settle(tester);
      expect(find.byKey(const ValueKey('task-bar')), findsOneWidget);
      expect(find.text('正在合成 HDR'), findsOneWidget);
      expect(find.textContaining('%'), findsWidgets);
      await shot(tester, 'home_task_bar');
      eng.failNext = true;
      eng.gate!.complete();
      await tester.pump(const Duration(milliseconds: 400));
      await tester.pump(const Duration(milliseconds: 400));
      expect(find.byKey(const ValueKey('task-bar')), findsNothing);
      expect(find.text('没能对齐这几张'), findsOneWidget);
      expect(find.text('查看'), findsOneWidget);
      await tester.tap(find.text('查看'));
      await _settle(tester);
      expect(find.text('HDR 合成'), findsOneWidget, reason: '查看 reopens the tool with the same selection');
      expect(find.text('参与合成 · 3 张'), findsOneWidget);
      expect(app.job, isNull);
    });
  });

  group('panorama tool', () {
    testWidgets('options, source labels, engine request', (tester) async {
      final (app, eng) = await _open(tester);
      await tester.tap(find.byKey(const ValueKey('entry-panorama-y3')));
      await _settle(tester);
      expect(find.text('全景合成'), findsOneWidget);
      expect(find.text('参与拼接 · 4 张'), findsOneWidget);
      expect(find.text('已识别 4 张连续拍摄'), findsOneWidget);
      expect(find.text('1/4'), findsOneWidget);
      expect(find.text('开始拼接'), findsOneWidget);
      await shot(tester, 'tool_pano');
      await tester.tap(find.descendant(of: find.byKey(const ValueKey('tool-projection')), matching: find.text('球面')));
      await tester.pump(_t);
      await tester.tap(find.byKey(const ValueKey('tool-crop')));
      await tester.pump(_t);
      await tester.tap(find.byKey(const ValueKey('tool-fill')));
      await tester.pump(_t);
      await tester.tap(find.byKey(const ValueKey('tool-main')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 600));
      await tester.pump(const Duration(milliseconds: 600));
      final o = eng.merges.single.options;
      expect(eng.merges.single.kind, 'pano');
      expect(o['projection'], 'sph');
      expect(o['crop'], false);
      expect(o['fill'], true);
      expect(o['longEdge'], AppState.panoLongEdge);
      expect(app.photos.first.kind, PhotoKind.pano);
      expect(find.text('全景已加入「今天」'), findsOneWidget);
    });
  });

  group('unavailable features are not offered', () {
    testWidgets('no merge build (iOS today): no merge cards, no 合成 in the selection bar; video card stays', (tester) async {
      useWindow(tester, phoneSize);
      final app = AppState.demo();
      app.engine = _NoMergeEngine();
      await pumpDapple(tester, app);
      expect(find.text('HDR · 3 张包围曝光'), findsNothing);
      expect(find.text('全景 · 4 张连续'), findsNothing);
      expect(find.text('视频 → 动态照片'), findsOneWidget);
      final done = app.photos.where((p) => p.status == PhotoStatus.done && p.kind == PhotoKind.photo).take(2).toList();
      app.enterSelect(done[0].id);
      app.toggleSelect(done[1].id);
      await tester.pump(_t);
      expect(find.byKey(const ValueKey('select-bar')), findsOneWidget);
      expect(find.byKey(const ValueKey('sel-compose')), findsNothing);
      expect(find.byKey(const ValueKey('sel-save')), findsOneWidget);
    });

    testWidgets('no video layer: no 视频 → 动态照片 card, merge cards stay', (tester) async {
      final (app, _) = await _open(tester);
      app.videoLayer = false;
      app.notifyListeners();
      await tester.pump(_t);
      expect(find.text('视频 → 动态照片'), findsNothing);
      expect(find.text('HDR · 3 张包围曝光'), findsOneWidget);
    });
  });

  group('multi-select compose', () {
    testWidgets('合成 chooser opens HDR / panorama with the selection (已带入你选的 N 张)', (tester) async {
      final (app, eng) = await _open(tester);
      final done = app.photos.where((p) => p.status == PhotoStatus.done && p.kind == PhotoKind.photo).take(3).toList();
      app.enterSelect(done[0].id);
      app.toggleSelect(done[1].id);
      await tester.pump(_t);
      await tester.tap(find.byKey(const ValueKey('sel-compose')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 400));
      expect(find.text('合成 2 张'), findsOneWidget);
      await tester.tap(find.text('全景拼接'));
      await _settle(tester);
      await tester.pump(const Duration(milliseconds: 400));
      expect(find.text('全景合成'), findsOneWidget);
      expect(find.text('已带入你选的 2 张'), findsOneWidget);
      expect(find.text('识别'), findsNothing);
      expect(find.text('参与拼接 · 2 张'), findsOneWidget);
      await tester.tap(find.byKey(const ValueKey('tool-main')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 600));
      await tester.pump(const Duration(milliseconds: 600));
      expect(eng.merges.length, 1);
      expect(app.selectMode, isFalse, reason: 'selection ends after a successful merge');
    });
  });

  group('Motion Photo tool', () {
    late List<MethodCall> calls;
    setUp(() {
      calls = [];
      final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(const MethodChannel('dapple/live'), (call) async {
        calls.add(call);
        switch (call.method) {
          case 'info':
            return {'durationMs': 14000, 'width': 1080, 'height': 1920, 'rotation': 0, 'hasAudio': true};
          case 'thumbs':
            return List<String?>.filled((call.arguments['timesMs'] as List).length, null);
          case 'frame':
            return {'path': call.arguments['dest'], 'kind': 'sdr-jpeg'};
          case 'trim':
            return {'path': call.arguments['dest'], 'actualStartMs': 4000, 'durationMs': 3000, 'frames': 90, 'audio': false};
        }
        return null;
      });
      messenger.setMockMethodCallHandler(const MethodChannel('dapple/media'), (call) async {
        calls.add(call);
        return 'content://saved/1';
      });
    });
    tearDown(() {
      final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(const MethodChannel('dapple/live'), null);
      messenger.setMockMethodCallHandler(const MethodChannel('dapple/media'), null);
    });

    testWidgets('window + cover on the filmstrip, mute, write, result photo', (tester) async {
      final (app, eng) = await _open(tester);
      await tester.tap(find.byKey(const ValueKey('entry-video')));
      await _settle(tester);
      await tester.pump(const Duration(milliseconds: 500));
      expect(find.text('视频转动态照片'), findsOneWidget);
      expect(find.text('动态照片 · 3.0 秒'), findsOneWidget);
      expect(find.text('3 秒片段 · 视频 0:14'), findsOneWidget);
      expect(find.text('封面 0:07'), findsOneWidget, reason: 'window centred, cover in its middle');
      expect(find.text('生成动态照片'), findsOneWidget);
      await shot(tester, 'tool_live');

      // drag the window right on the filmstrip (14 s video: 1/10 of the width is 1.4 s)
      final film = find.byKey(const ValueKey('live-film'));
      final rect = tester.getRect(film);
      // press inside the window, away from the cover line
      final inWindow = Offset(rect.left + rect.width * 0.43, rect.center.dy);
      final g = await tester.startGesture(inWindow);
      await g.moveBy(Offset(rect.width * 0.1, 0));
      await g.moveBy(Offset(rect.width * 0.1, 0));
      await g.up();
      await tester.pump(const Duration(milliseconds: 500));
      expect(find.text('封面 0:09'), findsOneWidget, reason: 'window moved right by 2.8 s, cover with it');
      await tester.tap(find.byKey(const ValueKey('live-mute')));
      await tester.pump(_t);
      // 源色彩: 自动 / 标准 / S-Log3 / HLG; the choice goes to the decoder with every call
      await tester.ensureVisible(find.byKey(const ValueKey('live-source')));
      await tester.pump(_t);
      expect(find.text('源色彩'), findsOneWidget);
      for (final l in ['自动', '标准', 'S-Log3', 'HLG']) {
        expect(find.text(l), findsOneWidget);
      }
      await tester.tap(find.text('S-Log3'));
      await tester.pump(const Duration(milliseconds: 600));

      await tester.tap(find.byKey(const ValueKey('tool-main')));
      await tester.pump();
      await tester.pump(const Duration(milliseconds: 600));
      await tester.pump(const Duration(milliseconds: 600));
      final trim = calls.firstWhere((c) => c.method == 'trim');
      expect(trim.arguments['durationMs'], 3000);
      expect(trim.arguments['mute'], false);
      expect(trim.arguments['source'], 2, reason: 'S-Log3 chosen');
      expect(calls.where((c) => c.method == 'frame').last.arguments['source'], 2);
      expect(trim.arguments['compat'], false);
      expect(trim.arguments['startMs'], greaterThan(5000));
      expect(eng.motion.single['out'].toString().endsWith('_MP.jpg'), isTrue);
      expect(eng.motion.single['cover'] as double, inInclusiveRange(0, 4));
      expect(calls.any((c) => c.method == 'saveToGallery' && c.arguments['name'].toString().endsWith('MP.jpg')), isTrue);
      expect(app.photos.first.kind, PhotoKind.live);
      expect(app.photos.first.badge, '动态照片');
      expect(find.text('动态照片已加入「今天」'), findsOneWidget);
    });

    testWidgets('software-decoded video (10-bit 4:2:2): hint shown, clip re-encoded by the platform, no stream-copy fallback', (tester) async {
      final (app, eng) = await _open(tester);
      final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      var trimFails = false;
      messenger.setMockMethodCallHandler(const MethodChannel('dapple/live'), (call) async {
        calls.add(call);
        switch (call.method) {
          case 'info':
            return {'durationMs': 14000, 'width': 1920, 'height': 1080, 'rotation': 0, 'decodable': true, 'software': true, 'format': 'H.264 profile 122, chroma 4:2:2, 10-bit'};
          case 'probe':
            return {'decodable': true, 'hardware': false, 'software': true};
          case 'thumbs':
            return List<String?>.filled((call.arguments['timesMs'] as List).length, null);
          case 'frame':
            return {'path': call.arguments['dest'], 'kind': 'sdr-jpeg'};
          case 'trim':
            if (trimFails) throw PlatformException(code: 'live', message: 'encoder failed');
            return {'path': call.arguments['dest'], 'actualStartMs': call.arguments['startMs'], 'durationMs': 3000, 'frames': 91, 'audio': false};
        }
        return null;
      });
      await tester.tap(find.byKey(const ValueKey('entry-video')));
      await _settle(tester);
      await tester.pump(const Duration(milliseconds: 500));
      expect(find.byKey(const ValueKey('live-software')), findsNothing, reason: 'no explanatory paragraph on the video tool page (owner rule: no technical copy)');
      expect(find.textContaining('软件解码'), findsNothing);
      expect(find.textContaining('10 位'), findsNothing);
      expect(find.byKey(const ValueKey('live-undecodable')), findsNothing);

      final job = await app.startLive(LiveDraft(video: _video(14)));
      await tester.pump(const Duration(milliseconds: 600));
      expect(job.status, JobStatus.done);
      expect(eng.motion.single.containsKey('mp4'), isTrue, reason: 'the platform clip is written, not the engine stream-copy cutter');
      expect(app.photos.first.liveGen, Photo.liveGenCurrent);
      expect(app.photos.first.liveSrc, isNotNull);
      final back = Photo.fromJson(app.photos.first.toJson());
      expect(back.liveGen, Photo.liveGenCurrent);
      expect(back.liveStartMs, app.photos.first.liveStartMs);

      // a failing software clip is a failure, never a stream copy of the undecodable stream
      trimFails = true;
      final before = app.photos.length;
      final bad = await app.startLive(LiveDraft(video: _video(14)));
      await tester.pump(const Duration(milliseconds: 600));
      expect(bad.status, JobStatus.failed);
      expect(eng.motion.length, 1, reason: "no engine cutter after a failed software clip");
      expect(app.photos.length, before);
    });

    testWidgets('undecodable video (e.g. 4:2:2 10-bit): no still is written, no Photo, clear error', (tester) async {
      final (app, _) = await _open(tester);
      final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(const MethodChannel('dapple/live'), (call) async {
        calls.add(call);
        switch (call.method) {
          case 'info':
            return {'durationMs': 14000, 'width': 1920, 'height': 1080, 'rotation': 0, 'decodable': false, 'format': 'H.264 profile 122, chroma 4:2:2, 10-bit'};
          case 'thumbs':
            return List<String?>.filled((call.arguments['timesMs'] as List).length, null);
          case 'frame':
            throw PlatformException(code: 'unsupported', message: 'unsupported video: H.264 profile 122, chroma 4:2:2, 10-bit');
        }
        return null;
      });
      final photosBefore = app.photos.length;
      await tester.tap(find.byKey(const ValueKey('entry-video')));
      await _settle(tester);
      await tester.pump(const Duration(milliseconds: 500));
      expect(find.byKey(const ValueKey('live-undecodable')), findsOneWidget);
      expect(find.textContaining('无法正确解码'), findsOneWidget);
      await tester.tap(find.byKey(const ValueKey('tool-main')));
      await tester.pump(const Duration(milliseconds: 600));
      expect(calls.where((c) => c.method == 'trim'), isEmpty, reason: 'generate is disabled');

      // and the engine path itself refuses: job fails, nothing is written or listed
      final job = await app.startLive(LiveDraft(video: _video(14)));
      await tester.pump(const Duration(milliseconds: 600));
      expect(job.status, JobStatus.failed);
      expect(job.error, contains('无法正确解码'));
      expect(app.photos.length, photosBefore);
      expect(calls.where((c) => c.method == 'trim'), isEmpty);
      expect(calls.where((c) => c.method == 'saveToGallery'), isEmpty);
    });

    testWidgets('no video preselected: the tool lists the MediaStore videos and a pick sets the window', (tester) async {
      final (app, _) = await _open(tester);
      app.videos = [_video(14)];
      app.navigatorKey.currentState!.push(MaterialPageRoute<void>(builder: (_) => LiveToolPage(app: app)));
      await _settle(tester);
      expect(find.text('选择视频'), findsWidgets);
      await tester.tap(find.byKey(const ValueKey('tool-main')));
      await _settle(tester);
      expect(find.byKey(const ValueKey('video-list')), findsOneWidget);
      await tester.tap(find.byKey(const ValueKey('video-v')));
      await _settle(tester);
      await tester.pump(const Duration(milliseconds: 500));
      expect(find.text('3 秒片段 · 视频 0:14'), findsOneWidget);
    });
  });

  group('camera FTP settings', () {
    testWidgets('section: switch, address, credentials, A7R V card', (tester) async {
      final (app, _) = await _open(tester);
      app.ftpAddresses = [FtpAddress('wlan0', '192.168.1.23')];
      await tester.tap(find.byKey(const ValueKey('nav-2')));
      await _settle(tester);
      expect(find.text('相机无线接收'), findsWidgets);
      expect(find.byKey(const ValueKey('sw-ftp')), findsOneWidget);
      await tester.tap(find.byKey(const ValueKey('sw-ftp')));
      await _settle(tester);
      expect(app.settings.ftpEnabled, isTrue);
      expect(app.ftpRunning, isTrue);
      await tester.dragUntilVisible(find.byKey(const ValueKey('ftp-setup')), find.byType(ListView).first, const Offset(0, -200));
      expect(find.text('192.168.1.23'), findsWidgets);
      expect(find.text('2121'), findsWidgets);
      expect(find.text('dapple'), findsWidgets);
      expect(find.textContaining(app.settings.ftpPass), findsWidgets);
      expect(find.text('α7R V 的 FTP 设置'), findsOneWidget);
      await shot(tester, 'settings_ftp');
      await tester.tap(find.byKey(const ValueKey('sw-ftp')));
      await _settle(tester);
      expect(app.ftpRunning, isFalse);
    });
  });

  group('FTP ingest', () {
    IncomingFile f(String n, int t) => IncomingFile(id: 'ftp:$n', name: n, addedMs: t, path: '/inbox/$n', size: 10);

    test('RAW + JPEG become one photo whichever arrives first', () {
      final a = AppState.demo();
      a.nativeOk = false;
      final n0 = a.photos.length;
      a.ingestFtpFiles([f('DSC01.JPG', 1000), f('DSC01.ARW', 2000), f('DSC02.ARW', 3000), f('DSC02.JPG', 3100), f('DSC03.JPG', 4000)]);
      final ftp = a.photos.where((p) => p.id.startsWith('ftp:')).toList();
      expect(ftp.map((p) => p.name).toSet(), {'DSC01.ARW', 'DSC02.ARW', 'DSC03.JPG'});
      expect(a.photos.length, n0 + 3);
      expect(ftp.firstWhere((p) => p.name == 'DSC01.ARW').pairedJpeg, '/inbox/DSC01.JPG');
      expect(ftp.firstWhere((p) => p.name == 'DSC02.ARW').pairedJpeg, '/inbox/DSC02.JPG');
      expect(ftp.firstWhere((p) => p.name == 'DSC03.JPG').pairedJpeg, isNull);
      // idempotent: a rescan of the inbox adds nothing
      a.ingestFtpFiles([f('DSC01.JPG', 1000), f('DSC01.ARW', 2000), f('DSC02.ARW', 3000), f('DSC02.JPG', 3100), f('DSC03.JPG', 4000)]);
      expect(a.photos.length, n0 + 3);
    });
  });
}

VideoItem _video(int seconds) =>
    VideoItem(id: 'v', uri: 'content://v', name: 'C0021.MP4', durationMs: seconds * 1000, size: 1000, addedMs: 0);
