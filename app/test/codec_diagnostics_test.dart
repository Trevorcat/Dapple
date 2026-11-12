import 'package:dapple/screens/library/codec_diagnostics_page.dart';
import 'package:dapple/screens/library/settings_page.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/services/live_bridge.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

const _report = '''
{"fingerprint":"google/redfin/redfin:14/UP1A.231105.001.B2","sdk":34,"abi":"arm64-v8a",
 "codecs":[{"name":"c2.qti.hevc.encoder","mime":"video/hevc","type":"encoder","hardware":true,"softwareOnly":false,
   "profiles":["Main"],"colorFormats":["Flexible","P010"],"maxSize":"4096x2304","maxFps1080":120,"performancePoints":3,"hdrEditing":false}],
 "selfTests":[{"kind":"enc10","codec":"c2.qti.hevc.encoder","pass":false,"detail":"no output: 8 frames accepted, none returned within 3 s","ms":3120,"at":1}],
 "encodeLadder1080p30":["HEVC Main 8-bit · c2.qti.hevc.encoder","H.264 High 8-bit · c2.qti.avc.encoder"],
 "decodeLadderHevc10":["hardware 10-bit (P010) · c2.qti.hevc.decoder","software"],
 "jobs":["clip HEVC Main 8-bit · c2.qti.hevc.encoder (hardware) | decode hevc (software), 4210 ms"]}
''';

void main() {
  testWidgets('hidden codec diagnostics shows the report and can re-run the self-tests', (tester) async {
    final calls = <String>[];
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(Brightness.light),
      home: CodecDiagnosticsPage(loader: ({bool run = false, bool force = false}) async {
        calls.add('run=$run force=$force');
        return _report;
      }),
    ));
    await tester.pumpAndSettle();
    final text = tester.widget<SelectableText>(find.byKey(const ValueKey('codec-report'))).data!;
    expect(text, contains('FAIL  enc10  c2.qti.hevc.encoder  3120 ms'));
    expect(text, contains('no output: 8 frames accepted'));
    expect(text, contains('1. HEVC Main 8-bit · c2.qti.hevc.encoder'));
    expect(text, contains('hardware 10-bit (P010)'));
    expect(text, contains('Main'));
    await tester.tap(find.text('运行自检'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('全部重测'));
    await tester.pumpAndSettle();
    expect(calls, ['run=false force=false', 'run=true force=false', 'run=false force=true']);
  });


  testWidgets('diagnostics lists the GPU probe state, the executor frame counts and the CPU reasons', (tester) async {
    const gpu = '{"mode":"auto","state":"ready","enabled":true,"available":true,"api":"vulkan","device":"Adreno 620","selfTest":"point+geometry: ok",'
        '"skipped":"","executors":{"gpuFrames":120,"cpuFrames":7,"deviceLost":0,"demotions":0,"api":"vulkan","lastReason":"",'
        '"unsupported":{"GPU self-test running":5,"unsupported on the GPU: film frame":2}}}';
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(Brightness.light),
      home: CodecDiagnosticsPage(loader: ({bool run = false, bool force = false}) async => _report, gpuLoader: () => gpu),
    ));
    await tester.pumpAndSettle();
    final text = tester.widget<SelectableText>(find.byKey(const ValueKey('codec-report'))).data!;
    expect(text, contains('mode auto · probe ready · in use'));
    expect(text, contains('vulkan · Adreno 620'));
    expect(text, contains('frames: GPU 120 · CPU 7 · last executor vulkan'));
    expect(text, contains('5  GPU self-test running'));
    expect(text, contains('2  unsupported on the GPU: film frame'));
  });

  testWidgets('diagnostics says so when the GPU probe failed or the build has no GPU', (tester) async {
    Future<String> rep({bool run = false, bool force = false}) async => _report;
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(Brightness.light),
      home: CodecDiagnosticsPage(loader: rep, gpuLoader: () => '{"mode":"auto","state":"failed","enabled":false,"available":false,"reason":"GPU unavailable: vulkan: self-test failed"}'),
    ));
    await tester.pumpAndSettle();
    expect(tester.widget<SelectableText>(find.byKey(const ValueKey('codec-report'))).data!, contains('GPU unavailable: vulkan: self-test failed'));
    await tester.pumpWidget(MaterialApp(
      theme: buildTheme(Brightness.light),
      home: CodecDiagnosticsPage(key: const ValueKey('b'), loader: rep, gpuLoader: () => '{"compiled":false,"enabled":false,"available":false}'),
    ));
    await tester.pumpAndSettle();
    expect(tester.widget<SelectableText>(find.byKey(const ValueKey('codec-report'))).data!, contains('every frame is a CPU frame'));
  });

  testWidgets('long-pressing the version on 关于 opens the diagnostics; a plain tap does nothing', (t) async {
    t.view.physicalSize = const Size(1080, 2400);
    t.view.devicePixelRatio = 3;
    addTearDown(t.view.reset);
    final app = AppState.demo();
    await t.pumpWidget(MaterialApp(
      theme: buildTheme(Brightness.light),
      home: Scaffold(body: SettingsPage(app: app, wide: false)),
    ));
    await t.pump(const Duration(milliseconds: 300));
    await t.scrollUntilVisible(find.byKey(const ValueKey('about-version')), 300, scrollable: find.byType(Scrollable).first);
    expect(find.text(kAppVersion), findsOneWidget);
    await t.tap(find.byKey(const ValueKey('about-version')));
    await t.pump(const Duration(milliseconds: 400));
    expect(find.byType(CodecDiagnosticsPage), findsNothing);
    await t.longPress(find.byKey(const ValueKey('about-version')));
    await t.pump(const Duration(milliseconds: 600));
    expect(find.byType(CodecDiagnosticsPage), findsOneWidget);
  });

  testWidgets('settings has a visible diagnostics entry (phone)', (t) async {
    t.view.physicalSize = const Size(1080, 2400);
    t.view.devicePixelRatio = 3;
    addTearDown(t.view.reset);
    final app = AppState.demo();
    await t.pumpWidget(MaterialApp(theme: buildTheme(Brightness.light), home: Scaffold(body: SettingsPage(app: app, wide: false))));
    await t.pump(const Duration(milliseconds: 300));
    await t.scrollUntilVisible(find.byKey(const ValueKey('codec-diagnostics')), 300, scrollable: find.byType(Scrollable).first);
    await t.ensureVisible(find.byKey(const ValueKey('codec-diagnostics')));
    await t.pump();
    await t.tap(find.byKey(const ValueKey('codec-diagnostics')));
    await t.pump();
    await t.pump(const Duration(milliseconds: 600));
    expect(find.byType(CodecDiagnosticsPage), findsOneWidget);
  });

  testWidgets('landscape short screen: category list scrolls to 关于 and the diagnostics entry is reachable', (t) async {
    t.view.physicalSize = const Size(2340, 1080);
    t.view.devicePixelRatio = 2.625; // ~891 x 411 dp
    addTearDown(t.view.reset);
    final app = AppState.demo();
    await t.pumpWidget(MaterialApp(theme: buildTheme(Brightness.light), home: Scaffold(body: SettingsPage(app: app, wide: true))));
    await t.pump(const Duration(milliseconds: 300));
    expect(t.takeException(), isNull);
    await t.scrollUntilVisible(find.byKey(const ValueKey('cat-5')), 100, scrollable: find.descendant(of: find.byKey(const ValueKey('cat-list')), matching: find.byType(Scrollable)));
    await t.tap(find.byKey(const ValueKey('cat-5')));
    await t.pump(const Duration(milliseconds: 300));
    expect(find.byKey(const ValueKey('codec-diagnostics')), findsOneWidget);
    await t.ensureVisible(find.byKey(const ValueKey('codec-diagnostics')));
    await t.pump();
    await t.tap(find.byKey(const ValueKey('codec-diagnostics')));
    await t.pump();
    await t.pump(const Duration(milliseconds: 600));
    expect(find.byType(CodecDiagnosticsPage), findsOneWidget);
  });

  test('clip summary carries the codec path and demotions', () {
    final i = ClipInfo({
      'videoCodec': 'hevc', 'videoProfile': 'Main', 'bitDepth': 8, 'decoder': 'hevc', 'hardwareDecode': false, 'audioHow': 'none',
      'codecSummary': 'HEVC Main 8-bit · c2.qti.hevc.encoder (hardware) | decode hevc (software) | demoted: HEVC Main10 c2.qti.hevc.encoder: self-test failed',
    });
    expect(i.summary, contains('demoted: HEVC Main10'));
    expect(ClipInfo({'videoCodec': 'h264', 'videoProfile': 'High', 'bitDepth': 8, 'decoder': 'h264'}).codecPath, '');
    expect(kAppVersion, isNotEmpty);
  });
}
