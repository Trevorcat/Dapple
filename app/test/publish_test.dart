import 'package:dapple/models.dart';
import 'package:dapple/screens/publish/publish_page.dart';
import 'package:dapple/services/share_service.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
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

List<String> idsOf(AppState app, int n) =>
    app.editable.where((p) => p.status == PhotoStatus.done).take(n).map((e) => e.id).toList();

Future<void> step(WidgetTester t) async {
  await t.pump();
  await t.pump(const Duration(milliseconds: 400));
  await t.pump(const Duration(milliseconds: 400));
}

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
    expect(valueOf(tester, 'row-ratio'), '原比例');
    expect(valueOf(tester, 'row-size'), '长边 1440 px');
    await shot(tester, 'publish_phone');
    await tester.tap(find.byKey(const ValueKey('preset-xhs')));
    await tester.pump();
    expect(valueOf(tester, 'row-ratio'), '3:4');
    expect(valueOf(tester, 'row-size'), '长边 2048 px');
    await tester.tap(find.byKey(const ValueKey('row-ratio')));
    await tester.pump();
    expect(valueOf(tester, 'row-ratio'), '原比例');
    await tester.tap(find.byKey(const ValueKey('row-loc')));
    await tester.pump();
    expect(valueOf(tester, 'row-loc'), '保留');
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
    expect(find.text('已导出 9 张 · 原比例 · 长边 1440 px'), findsOneWidget);
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
}
