import 'package:dapple/model/params.dart' show EditState;
import 'package:dapple/services/merge_engine.dart';
import 'package:dapple/state/app_state.dart';
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'support/harness.dart';

Future<AppState> _open(WidgetTester tester,
    {DemoScenario sc = DemoScenario.normal, Size size = phoneSize, bool dark = false}) async {
  useWindow(tester, size);
  final app = AppState.demo(scenario: sc)..engine = _MergeBuild();
  await pumpDapple(tester, app, dark: dark);
  return app;
}

/// The merge entries only show when the engine has the merge build; this stub says it does.
class _MergeBuild implements MergeEngine {
  @override
  bool get available => true;
  @override
  dynamic noSuchMethod(Invocation i) => super.noSuchMethod(i);
}

const _t = Duration(milliseconds: 100);

void main() {
  testWidgets('home normal: connection card, groups, tiles', (tester) async {
    final app = await _open(tester);
    expect(find.text('照片'), findsWidgets);
    expect(find.text('Sony α7R V · 已连接'), findsOneWidget);
    expect(find.textContaining('正在接收第'), findsOneWidget);
    expect(find.text('今天 · 外滩'), findsOneWidget);
    expect(find.textContaining('接收中 50%'), findsOneWidget);
    expect(app.groups.length, 2);
    await shot(tester, 'home_phone');
  });

  testWidgets('no camera source: card does not name a camera, group says 手机相册', (tester) async {
    useWindow(tester, phoneSize);
    final app = AppState.demo();
    app.cameraSource = false;
    for (final p in app.photos) {
      p.place = null;
    }
    await pumpDapple(tester, app);
    expect(find.textContaining('Sony'), findsNothing);
    expect(find.text('未连接相机'), findsOneWidget);
    expect(find.text('连接相机'), findsWidgets);
    expect(find.textContaining('手机相册'), findsWidgets);
  });

  testWidgets('long press selects, count shown, select bar enabled', (tester) async {
    final app = await _open(tester);
    final done = app.photos.firstWhere((p) => p.status.name == 'done' && p.kind.name == 'photo');
    await tester.longPress(find.byKey(ValueKey('tile-${done.id}')));
    await tester.pump(_t);
    expect(find.text('已选 1 张'), findsOneWidget);
    expect(app.selectMode, isTrue);
    expect(find.byKey(const ValueKey('select-bar')), findsOneWidget);
    expect(find.byKey(const ValueKey('nav-bar')), findsNothing);
    final second =
        app.photos.where((p) => p.status.name == 'done' && p.kind.name == 'photo' && p.id != done.id).first;
    await tester.tap(find.byKey(ValueKey('tile-${second.id}')));
    await tester.pump(_t);
    expect(find.text('已选 2 张'), findsOneWidget);
    await shot(tester, 'home_phone_select');
    // 合成 with 2 selected opens the sheet
    await tester.tap(find.byKey(const ValueKey('sel-compose')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 500));
    expect(find.text('合成 2 张'), findsOneWidget);
    await tester.tapAt(const Offset(20, 20)); // scrim dismisses the sheet
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 500));
    // select all in the first group
    await tester.tap(find.text('全选').first);
    await tester.pump(_t);
    expect(find.text('取消全选'), findsWidgets);
    // 发布 -> publish sheet (opened by fork C's page)
    await tester.tap(find.byKey(const ValueKey('sel-publish')));
    await tester.pump(const Duration(milliseconds: 500));
    await tester.pump(const Duration(milliseconds: 500));
    expect(find.byKey(const ValueKey('publish-title')), findsOneWidget);
    app.exitSelect();
    await tester.pump(_t);
    expect(find.byKey(const ValueKey('nav-bar')), findsOneWidget);
  });

  testWidgets('new pill appears when a photo arrives while frozen', (tester) async {
    final app = await _open(tester);
    expect(find.byKey(const ValueKey('new-pill')), findsNothing);
    app.setFrozen(true);
    app.simulateArrival(2);
    await tester.pump(_t);
    expect(find.text('新到 1 张'), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey('new-pill')));
    await tester.pump(const Duration(milliseconds: 400));
    expect(app.newCount, 0);
    expect(find.byKey(const ValueKey('new-pill')), findsNothing);
  });

  testWidgets('empty scenario', (tester) async {
    final app = await _open(tester, sc: DemoScenario.empty);
    expect(find.text('还没有照片'), findsOneWidget);
    expect(find.text('Sony α7R V · 未连接'), findsOneWidget);
    expect(find.byKey(const ValueKey('select-toggle')), findsNothing);
    await shot(tester, 'home_phone_empty');
    await tester.tap(find.byKey(const ValueKey('empty-connect')));
    await tester.pump(_t);
    expect(app.toast?.message, '已连接');
  });

  testWidgets('disconnected scenario', (tester) async {
    final app = await _open(tester, sc: DemoScenario.disconnected);
    expect(find.text('Sony α7R V · 已断开'), findsOneWidget);
    expect(find.text('重新连接'), findsOneWidget);
    await shot(tester, 'home_phone_disconnected');
    await tester.tap(find.byKey(const ValueKey('conn-action')));
    await tester.pump(_t);
    expect(app.linkDown, isFalse);
  });

  testWidgets('transfer failed scenario', (tester) async {
    final app = await _open(tester, sc: DemoScenario.transferFailed);
    expect(find.text('1 张传输中断'), findsOneWidget);
    expect(find.text('传输中断'), findsOneWidget);
    await shot(tester, 'home_phone_failed');
    await tester.tap(find.byKey(const ValueKey('conn-action')));
    await tester.pump(_t);
    expect(app.interruptedCount, 0);
  });

  testWidgets('tabs, styles default, settings toggles', (tester) async {
    final app = await _open(tester);
    await tester.tap(find.byKey(const ValueKey('nav-1')));
    await tester.pump(_t);
    expect(app.tab, 1);
    expect(find.textContaining('新照片默认使用'), findsOneWidget);
    await shot(tester, 'styles_phone');
    await tester.tap(find.byKey(const ValueKey('base-modern_vivid')));
    await tester.pump(_t);
    expect(app.settings.defaultBaseId, 'modern_vivid');
    expect(find.text('新照片默认使用「鲜明」'), findsOneWidget);
    await tester.tap(find.byKey(const ValueKey('toast-action')));
    await tester.pump(_t);
    expect(app.settings.defaultBaseId, isNull);
    EditState.defaultBase = null;

    await tester.tap(find.byKey(const ValueKey('nav-2')));
    await tester.pump(_t);
    expect(find.text('先按明暗轻微增强，再套用默认风格'), findsOneWidget);
    await shot(tester, 'settings_phone');
    await tester.tap(find.byKey(const ValueKey('sw-auto')));
    await tester.pump(_t);
    expect(app.settings.autoProcess, isFalse);
    await tester.tap(find.byKey(const ValueKey('sw-strip')));
    await tester.pump(_t);
    expect(app.settings.stripLocation, isFalse);
    expect(app.settings.exportLongEdge, 0);
    await tester.tap(find.byKey(const ValueKey('export-size')));
    await tester.pump(_t);
    await tester.pump(const Duration(milliseconds: 400));
    await tester.tap(find.byKey(const ValueKey('export-size-opt-2048')));
    await tester.pump(_t);
    await tester.pump(const Duration(milliseconds: 400));
    expect(app.settings.exportLongEdge, 2048);
    expect(find.text('适合社交平台'), findsWidgets);
    await tester.tap(find.byKey(const ValueKey('presets')));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 600));
    expect(find.text('小红书'), findsOneWidget);
  });

  testWidgets('dark phone', (tester) async {
    await _open(tester, dark: true);
    await shot(tester, 'home_phone_dark');
  });

  testWidgets('fold: rail + full-width grid, light and dark', (tester) async {
    await _open(tester, size: foldSize);
    expect(find.byKey(const ValueKey('nav-rail')), findsOneWidget);
    expect(find.byKey(const ValueKey('nav-bar')), findsNothing);
    await shot(tester, 'home_fold_browse');
  });

  testWidgets('fold dark', (tester) async {
    await _open(tester, size: foldSize, dark: true);
    await shot(tester, 'home_fold_dark');
  });

  test('no photo permission: the card says so instead of a lost link (state strings)', () {
    final app = AppState()
      ..ready = true
      ..mediaPermitted = false;
    expect(app.needsMediaPermission, isTrue);
    expect(app.connTitle, '还不能读取手机里的照片');
    expect(app.connLine, '允许后，手机相册里的照片会出现在这里');
    expect(app.connAction, '允许访问');
    app.mediaPermitted = true; // granted: the ordinary card texts take over again
    expect(app.needsMediaPermission, isFalse);
    expect(app.connTitle, isNot('还不能读取手机里的照片'));
    expect(app.connAction, isNot('允许访问'));
    // the demo app (widget tests, screenshots) is never in that state
    final demo = AppState.demo()..mediaPermitted = false;
    expect(demo.needsMediaPermission, isFalse);
  });

  testWidgets('no photo permission: the connection card shows the permission wording and the allow action', (tester) async {
    useWindow(tester, phoneSize);
    final app = AppState()
      ..ready = true
      ..mediaPermitted = false;
    await pumpDapple(tester, app);
    expect(find.text('还不能读取手机里的照片'), findsOneWidget);
    expect(find.text('允许后，手机相册里的照片会出现在这里'), findsOneWidget);
    expect(find.descendant(of: find.byKey(const ValueKey('conn-action')), matching: find.text('允许访问')), findsOneWidget);
    expect(find.textContaining('已断开'), findsNothing);
    await shot(tester, 'home_phone_no_permission');
  });

  testWidgets('the whole connection card taps like its action pill', (tester) async {
    useWindow(tester, phoneSize);
    final app = AppState.demo()..linkDown = true;
    await pumpDapple(tester, app);
    expect(app.connAction, isNotNull);
    final card = tester.getRect(find.byKey(const ValueKey('conn-card')));
    // away from the pill (which sits at the right end)
    await tester.tapAt(Offset(card.left + 40, card.center.dy));
    await tester.pump(_t);
    expect(app.linkDown, isFalse, reason: 'the card did what the pill does');
  });

  testWidgets('the permission card is tappable as a whole, a card without an action is not', (tester) async {
    useWindow(tester, phoneSize);
    final app = AppState()
      ..ready = true
      ..mediaPermitted = false;
    await pumpDapple(tester, app);
    expect(tester.widget<InkWell>(find.byKey(const ValueKey('conn-card-tap'))).onTap, isNotNull);
    final ok = AppState.demo();
    await pumpDapple(tester, ok);
    expect(ok.connAction, isNull);
    expect(tester.widget<InkWell>(find.byKey(const ValueKey('conn-card-tap'))).onTap, isNull);
  });
}
