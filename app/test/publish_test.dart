import 'dart:convert';

import 'package:dapple/model/catalog.dart';
import 'package:dapple/models.dart';
import 'package:dapple/screens/publish/publish_page.dart';
import 'package:dapple/services/share_service.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:dapple/widgets/controls.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'support/harness.dart';

class FakeShare implements ShareService {
  List<String>? shared;
  String? opened;
  @override
  Future<bool> shareFiles(List<String> paths) async {
    shared = paths;
    return true;
  }

  @override
  Future<bool> openApp(String package) async {
    opened = package;
    return true;
  }
}

class _Host extends StatelessWidget {
  const _Host(this.app, this.ids, this.result);
  final AppState app;
  final List<String> ids;
  final ValueNotifier<bool?> result;
  @override
  Widget build(BuildContext context) => Scaffold(
        body: Center(
          child: Builder(
            builder: (ctx) => TextButton(
              key: const ValueKey('host-open'),
              onPressed: () async => result.value = await openPublish(ctx, app, ids),
              child: const Text('open'),
            ),
          ),
        ),
      );
}

Future<ValueNotifier<bool?>> open(WidgetTester tester, AppState app, List<String> ids, {Size size = phoneSize}) async {
  useWindow(tester, size);
  await loadTestFonts();
  final result = ValueNotifier<bool?>(null);
  await tester.pumpWidget(RepaintBoundary(
    key: shotKey,
    child: _ThemedHost(app: app, child: _Host(app, ids, result)),
  ));
  await tester.tap(find.byKey(const ValueKey('host-open')));
  await tester.pump();
  await tester.pump(const Duration(milliseconds: 400));
  return result;
}

class _ThemedHost extends StatelessWidget {
  const _ThemedHost({required this.app, required this.child});
  final AppState app;
  final Widget child;
  @override
  Widget build(BuildContext context) => MaterialApp(debugShowCheckedModeBanner: false, theme: buildTheme(Brightness.light), home: child);
}

/// Demo app whose photos report a fixed highlight headroom (the real probe needs the native library).
class _HdrApp extends AppState {
  _HdrApp(this.stops) : super.demo();
  final double stops;
  @override
  Future<double> hdrHeadroomOf(Photo ph) async => stops;
}

/// Demo app whose photos report a fixed share of colour outside sRGB (the real probe needs the native library); records the colour space asked for.
class _WideApp extends AppState {
  _WideApp(this.share) : super.demo();
  final double share;
  final wide = <bool>[];
  @override
  Future<double> wideGamutOf(Photo ph) async => share;
  @override
  Future<String> exportJpeg(Photo ph, {int? longEdge, String? cropOverride, bool hdr = false, bool? stripLocation, bool wideGamut = false}) async {
    wide.add(wideGamut);
    return '/demo/${ph.id}.jpg';
  }
}

/// Demo app that records how the publish page calls the export functions.
class _SpyApp extends AppState {
  _SpyApp() : super.demo();
  final calls = <({String kind, bool strip})>[];
  @override
  Future<String> exportJpeg(Photo ph, {int? longEdge, String? cropOverride, bool hdr = false, bool? stripLocation, bool wideGamut = false}) async {
    calls.add((kind: 'export', strip: stripLocation ?? settings.stripLocation));
    return '/demo/${ph.id}.jpg';
  }

  @override
  Future<int> saveToGallery(List<Photo> list, {int? longEdge, String? cropOverride, bool hdr = false, bool? stripLocation, bool wideGamut = false}) async {
    calls.add((kind: 'gallery', strip: stripLocation ?? settings.stripLocation));
    return list.length;
  }
}

List<String> idsOf(AppState app, int n) =>
    app.editable.where((p) => p.status == PhotoStatus.done).take(n).map((e) => e.id).toList();

Future<void> step(WidgetTester t) async {
  await t.pump();
  await t.pump(const Duration(milliseconds: 400));
  await t.pump(const Duration(milliseconds: 400));
}

int segIndex(WidgetTester t) => (t.widget(find.byKey(const ValueKey('seg-ratio'))) as DSegmented).index;
bool locOn(WidgetTester t) => (t.widget(find.byKey(const ValueKey('switch-loc'))) as DSwitch).value;
bool csOn(WidgetTester t) => (t.widget(find.byKey(const ValueKey('switch-cs'))) as DSwitch).value;

String valueOf(WidgetTester t, String key) => (t.widget(find.byKey(ValueKey('$key-value'))) as Text).data!;

void main() {
  late FakeShare fake;
  setUp(() {
    fake = FakeShare();
    ShareService.instance = fake;
  });

  testWidgets('presets change ratio and size rows', (tester) async {
    final app = AppState.demo();
    await open(tester, app, idsOf(app, 2));
    expect(find.text('发布 2 张'), findsOneWidget);
    expect(find.byKey(const ValueKey('seg-ratio')), findsOneWidget);
    expect(valueOf(tester, 'row-size'), '适合社交平台');
    await shot(tester, 'publish_phone');
    await tester.tap(find.byKey(const ValueKey('preset-xhs')));
    await tester.pump();
    expect(find.byKey(const ValueKey('seg-ratio')), findsOneWidget);
    expect(valueOf(tester, 'row-size'), '适合社交平台');
    await tester.tap(find.byKey(const ValueKey('row-size')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 400));
    await tester.tap(find.byKey(const ValueKey('export-size-opt-0')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 400));
    expect(valueOf(tester, 'row-size'), '原图大小');
    expect(segIndex(tester), 0);
    await tester.tap(find.text('原比例'));
    await tester.pump();
    expect(segIndex(tester), 1);
    await tester.tap(find.byKey(const ValueKey('row-loc')));
    await tester.pump();
    expect(locOn(tester), isFalse);
    await tester.tap(find.byKey(const ValueKey('preset-xhs')));
    await tester.pump();
    await shot(tester, 'publish_phone_xhs');
  });

  testWidgets('remove and reorder', (tester) async {
    final app = AppState.demo();
    final ids = idsOf(app, 3);
    await open(tester, app, ids);
    await tester.tap(find.byKey(const ValueKey('thumb-right-0')));
    await tester.pump();
    expect(find.byKey(const ValueKey('thumb-left-0')), findsNothing);
    await tester.tap(find.byKey(const ValueKey('thumb-remove-0')));
    await tester.pump();
    expect(find.text('发布 2 张'), findsOneWidget);
  });

  testWidgets('over limit splits into two posts; processing warning', (tester) async {
    final app = AppState.demo();
    final ids = app.editable.take(12).map((e) => e.id).toList();
    await open(tester, app, ids);
    expect(find.text('朋友圈一次最多 9 张'), findsOneWidget);
    expect(find.textContaining('还在处理'), findsOneWidget);
    expect(find.textContaining('暗部噪点较多'), findsOneWidget);
    await shot(tester, 'publish_phone_warn');
    await tester.tap(find.byKey(const ValueKey('warn-split')));
    await tester.pump();
    expect(find.text('第 1/2 篇 · 9 张'), findsOneWidget);
  });

  testWidgets('share reaches honest done screen; split continues; finish returns true', (tester) async {
    final app = AppState.demo();
    final ids = app.editable.take(12).map((e) => e.id).toList();
    final result = await open(tester, app, ids);
    await tester.tap(find.byKey(const ValueKey('warn-split')));
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey('publish-share')));
    await step(tester);
    await tester.tap(find.byKey(const ValueKey('handoff-system')));
    await step(tester);
    expect(fake.shared!.length, 9);
    expect(find.text('已交给微信'), findsNWidgets(2)); // title + step
    expect(find.text('已导出 9 张 · 原比例 · 适合社交平台'), findsOneWidget);
    expect(find.byKey(const ValueKey('done-open')), findsNothing);
    await shot(tester, 'publish_done_phone');
    await tester.tap(find.byKey(const ValueKey('done-next')));
    await step(tester);
    expect(find.text('第 2/2 篇 · 3 张'), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey('publish-share')));
    await step(tester);
    await tester.tap(find.byKey(const ValueKey('handoff-system')));
    await step(tester);
    await tester.tap(find.byKey(const ValueKey('done-finish')));
    await step(tester);
    expect(result.value, isTrue);
  });

  testWidgets('gallery fallback: open app then 已打开', (tester) async {
    final app = AppState.demo();
    await open(tester, app, idsOf(app, 2));
    await tester.tap(find.byKey(const ValueKey('preset-xhs')));
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey('publish-share')));
    await step(tester);
    await tester.tap(find.byKey(const ValueKey('handoff-gallery')));
    await step(tester);
    expect(find.text('已存入相册'), findsOneWidget);
    expect(find.text('已存入相册「Dapple」'), findsOneWidget);
    expect(find.text('已打开小红书'), findsNothing);
    await shot(tester, 'publish_done_fallback_phone');
    await tester.tap(find.byKey(const ValueKey('done-open')));
    await tester.pump(const Duration(milliseconds: 200));
    expect(fake.opened, 'com.xingin.xhs');
    expect(find.text('已打开小红书'), findsWidgets);
    expect(find.byKey(const ValueKey('done-open')), findsNothing);
    await tester.pump(const Duration(seconds: 3));
  });

  testWidgets('fold layout', (tester) async {
    final app = AppState.demo();
    await open(tester, app, idsOf(app, 2), size: foldSize);
    await shot(tester, 'publish_fold');
    expect(find.text('分享…'), findsOneWidget);
  });

  testWidgets('HDR 照片 option is hidden when no photo has highlight headroom', (tester) async {
    final app = _HdrApp(0);
    await open(tester, app, idsOf(app, 2));
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.byKey(const ValueKey('row-hdr')), findsNothing);
    expect(find.text('高光更亮'), findsNothing);
  });

  testWidgets('HDR (高光更亮) option: off by default, plain words without an explanation, switch toggles', (tester) async {
    final app = _HdrApp(1.5);
    await open(tester, app, idsOf(app, 2));
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.byKey(const ValueKey('row-hdr')), findsOneWidget);
    expect(find.text('高光更亮'), findsOneWidget);
    expect(find.byKey(const ValueKey('row-hdr-sub')), findsNothing);
    expect((tester.widget(find.byKey(const ValueKey('switch-hdr'))) as DSwitch).value, isFalse);
    await tester.tap(find.byKey(const ValueKey('switch-hdr')));
    await tester.pump();
    expect((tester.widget(find.byKey(const ValueKey('switch-hdr'))) as DSwitch).value, isTrue);
    await shot(tester, 'publish_phone_hdr');
  });

  testWidgets('色彩空间 option is hidden when no photo has colour outside sRGB', (tester) async {
    final app = _WideApp(0.0);
    await open(tester, app, idsOf(app, 2));
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.byKey(const ValueKey('row-cs')), findsNothing);
    expect(find.byKey(const ValueKey('switch-cs')), findsNothing);
  });

  testWidgets('colour space option (颜色): 标准 (sRGB) by default, no explanation, tap switches to 更鲜艳 (P3) and reaches the export', (tester) async {
    final app = _WideApp(0.2);
    await open(tester, app, idsOf(app, 2));
    await tester.pump(const Duration(milliseconds: 50));
    expect(find.byKey(const ValueKey('row-cs')), findsOneWidget);
    expect(csOn(tester), isFalse);
    expect(find.text('颜色更鲜艳'), findsOneWidget);
    expect(find.byKey(const ValueKey('row-cs-sub')), findsNothing);
    await shot(tester, 'publish_phone_p3');
    await tester.tap(find.byKey(const ValueKey('row-cs')));
    await tester.pump();
    expect(csOn(tester), isTrue);
    await tester.tap(find.byKey(const ValueKey('publish-share')));
    await step(tester);
    await tester.tap(find.byKey(const ValueKey('handoff-system')));
    await step(tester);
    expect(app.wide, [true, true]);
  });

  test('export options JSON: sRGB by default, p3 for the wide-gamut choice', () {
    expect((jsonDecode(AppState.exportMetaJson(original: 'x', stripLocation: true)) as Map)['colorSpace'], 'srgb');
    expect((jsonDecode(AppState.exportMetaJson(original: 'x', stripLocation: true, wideGamut: true)) as Map)['colorSpace'], 'p3');
  });

  testWidgets('去除位置信息 reaches the export call (share and gallery paths)', (tester) async {
    final app = _SpyApp();
    await open(tester, app, idsOf(app, 2));
    expect(locOn(tester), isTrue); // the setting's default: strip
    await tester.tap(find.byKey(const ValueKey('publish-share')));
    await step(tester);
    await tester.tap(find.byKey(const ValueKey('handoff-system')));
    await step(tester);
    expect(app.calls, [(kind: 'export', strip: true), (kind: 'export', strip: true)]);
    app.calls.clear();
    // keep the location: the switch value follows to the call
    await tester.tap(find.byKey(const ValueKey('done-again')));
    await step(tester);
    await tester.tap(find.byKey(const ValueKey('row-loc')));
    await tester.pump();
    expect(locOn(tester), isFalse);
    await tester.tap(find.byKey(const ValueKey('publish-share')));
    await step(tester);
    await tester.tap(find.byKey(const ValueKey('handoff-system')));
    await step(tester);
    expect(app.calls, [(kind: 'export', strip: false), (kind: 'export', strip: false)]);
  });

  testWidgets('去除位置信息 reaches the gallery fallback', (tester) async {
    final app = _SpyApp();
    await open(tester, app, idsOf(app, 1));
    await tester.tap(find.byKey(const ValueKey('row-loc'))); // keep
    await tester.pump();
    await tester.tap(find.byKey(const ValueKey('publish-share')));
    await step(tester);
    await tester.tap(find.byKey(const ValueKey('handoff-gallery')));
    await step(tester);
    expect(app.calls, [(kind: 'gallery', strip: false)]);
  });

  test('export options JSON carries the location choice and colour space', () {
    final a = jsonDecode(AppState.exportMetaJson(original: '/a/b.heic', stripLocation: true)) as Map;
    expect(a, {'metaSource': '/a/b.heic', 'stripLocation': true, 'colorSpace': 'srgb'});
    expect((jsonDecode(AppState.exportMetaJson(original: 'x', stripLocation: false)) as Map)['stripLocation'], isFalse);
  });

  testWidgets('no row cycles: <= 3 ratios give a segmented control, tapping a segment picks it', (tester) async {
    final app = AppState.demo();
    await open(tester, app, idsOf(app, 2));
    await tester.tap(find.byKey(const ValueKey('preset-ig')));
    await tester.pump();
    expect(find.byKey(const ValueKey('seg-ratio')), findsOneWidget);
    expect(segIndex(tester), 0);
    await tester.tap(find.byKey(const ValueKey('row-ratio'))); // the row itself does nothing
    await tester.pump();
    expect(segIndex(tester), 0);
    await tester.tap(find.text('1:1'));
    await tester.pump();
    expect(segIndex(tester), 1);
  });

  testWidgets('more than 3 ratios: the row opens a sheet with radio rows', (tester) async {
    const four = Preset('four', '四种', '', null, 'orig', ['orig', '1:1', '4:5', '3:4'], null, null);
    final app = AppState.demo();
    await open(tester, app, idsOf(app, 1));
    final st = tester.state(find.byType(PublishPage)) as dynamic;
    st.debugSetPresetForTest(four);
    await tester.pump();
    expect(find.byKey(const ValueKey('seg-ratio')), findsNothing);
    expect(valueOf(tester, 'row-ratio'), '原比例');
    await tester.tap(find.byKey(const ValueKey('row-ratio')));
    await step(tester);
    expect(find.byKey(const ValueKey('ratio-4:5')), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey('ratio-4:5')));
    await step(tester);
    expect(find.byKey(const ValueKey('ratio-4:5')), findsNothing);
    expect(valueOf(tester, 'row-ratio'), '4:5');
  });

  test('platform presets: ids, open-app data, Instagram limits', () {
    expect([for (final p in presets) p.id], ['wechat', 'xhs', 'weibo', 'douyin', 'ig', 'other']);
    for (final p in presets.where((p) => p.id != 'other')) {
      expect(p.package, isNotEmpty);
      expect(p.iosScheme, endsWith('://'), reason: p.id);
    }
    final ig = presets.firstWhere((p) => p.id == 'ig');
    expect(ig.maxCount, 20); // carousel limit since August 2024
    expect(ig.defRatio, '4:5'); // inside 1.91:1 .. 4:5, so the feed does not crop
    expect(presets.firstWhere((p) => p.id == 'wechat').maxCount, 9);
    for (final p in presets) {
      expect(p.ratios, contains(p.defRatio), reason: p.id);
    }
  });
}
