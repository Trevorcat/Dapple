// Walkthrough screenshots of every screen, at seven window sizes (phone portrait / landscape, small phone, book-style foldable inner
// screen in both orientations, flip-style foldable open, flip-style cover screen). Not a regression test: a plain `flutter test` skips it.
//   flutter test test/ui/pages_sweep_test.dart --dart-define=PAGES=true
// writes ../build/ui-pages/<size>/NN_name.png (2x) and ../build/ui-pages/index.md. Steps that cannot be reached or that raise a layout
// exception are recorded in the index instead of aborting the run.
import 'dart:async';
import 'dart:io';
import 'dart:ui' as ui;

import 'package:dapple/app.dart';
import 'package:dapple/models.dart';
import 'package:dapple/model/pad_model.dart';
import 'package:dapple/model/params.dart';
import 'package:dapple/platform/usb_camera.dart';
import 'package:dapple/screens/editor/editor_page.dart';
import 'package:dapple/screens/editor/film_map_view.dart';
import 'package:dapple/screens/editor/local_controller.dart';
import 'package:dapple/screens/editor/local_layer.dart';
import 'package:dapple/screens/library/licenses_page.dart';
import 'package:dapple/screens/publish/publish_page.dart';
import 'package:dapple/services/local_ai.dart';
import 'package:dapple/services/share_service.dart';
import 'package:dapple/state/app_state.dart';
import 'package:dapple/theme/tokens.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

import '../editor_v2_test.dart' show Finger, settle;
import '../local_ai_test.dart' show FakeLocalAi, portrait;
import '../local_portrait_test.dart' show portraitResult;
import '../pad_panel_rules_test.dart' show padAt;
import '../publish_test.dart' show FakeShare;
import '../support/fake_usb.dart';
import '../support/film_fixture.dart';
import '../support/harness.dart';
import '../tools_test.dart' show FakeEngine;

const bool kPages = bool.fromEnvironment('PAGES');
const String kRoot = '../build/ui-pages';

enum Fold { none, vertical, horizontal }

class Sz {
  const Sz(this.dir, this.w, this.h, {this.top = 36, this.bottom = 24, this.fold = Fold.none});
  final String dir;
  final double w, h, top, bottom;
  final Fold fold;
  Size get size => Size(w, h);
}

const sizes = <Sz>[
  Sz('phone-portrait-412x915', 412, 915),
  Sz('phone-landscape-915x412', 915, 412, top: 0, bottom: 0),
  Sz('small-phone-360x780', 360, 780),
  Sz('book-inner-904x832', 904, 832, fold: Fold.vertical),
  Sz('book-inner-landscape-832x904', 832, 904, fold: Fold.horizontal),
  Sz('flip-open-412x1004', 412, 1004, fold: Fold.horizontal),
  Sz('flip-cover-360x380', 360, 380, top: 24, bottom: 0),
  Sz('wide-fold-open-500x800', 500, 800, fold: Fold.horizontal),
  Sz('wide-fold-open-landscape-800x500', 800, 500, fold: Fold.horizontal),
  Sz('wide-fold-cover-400x400', 400, 400, top: 24, bottom: 0),
];

class Rec {
  Rec(this.dir, this.file, this.desc, this.fold);
  final String dir, file, desc, fold;
}

final recs = <Rec>[];
final problems = <String>[]; // layout exceptions
final unreached = <String>[]; // steps that could not be done
final counters = <String, int>{};

String foldNote(Sz z, ui.DisplayFeatureState? st) => switch (z.fold) {
      Fold.none => '',
      Fold.vertical => 'hinge: vertical fold at x=${z.w / 2}, ${st == ui.DisplayFeatureState.postureHalfOpened ? 'half-opened' : 'flat'}',
      Fold.horizontal => 'hinge: horizontal fold at y=${z.h / 2}, ${st == ui.DisplayFeatureState.postureHalfOpened ? 'half-opened' : 'flat'}',
    };

ui.DisplayFeatureState _posture = ui.DisplayFeatureState.postureFlat;
bool _marker = false;

void win(WidgetTester t, Sz z, {ui.DisplayFeatureState posture = ui.DisplayFeatureState.postureFlat, bool marker = false}) {
  useWindow(t, z.size, top: z.top, bottom: z.bottom);
  _posture = posture;
  _marker = marker;
  // display features are in logical pixels (the engine divides by the device pixel ratio before the framework sees them)
  t.view.displayFeatures = switch (z.fold) {
    Fold.none => const [],
    Fold.vertical => [ui.DisplayFeature(bounds: Rect.fromLTWH(z.w / 2, 0, 0, z.h), type: ui.DisplayFeatureType.fold, state: posture)],
    Fold.horizontal => [ui.DisplayFeature(bounds: Rect.fromLTWH(0, z.h / 2, z.w, 0), type: ui.DisplayFeatureType.fold, state: posture)],
  };
}

Widget framed(Sz z, Widget app) {
  app = KeyedSubtree(key: UniqueKey(), child: app); // a new tree each time: no route or state carried over
  if (!_marker) return RepaintBoundary(key: shotKey, child: app);
  // review aid for the half-opened pictures only: a thin red line where the fold is
  return RepaintBoundary(
    key: shotKey,
    child: Stack(textDirection: TextDirection.ltr, children: [
      Positioned.fill(child: app),
      Positioned(
        left: z.fold == Fold.vertical ? z.w / 2 - 1 : 0,
        top: z.fold == Fold.horizontal ? z.h / 2 - 1 : 0,
        width: z.fold == Fold.vertical ? 2 : z.w,
        height: z.fold == Fold.horizontal ? 2 : z.h,
        child: IgnorePointer(child: Container(color: const Color(0xAAFF0000))),
      ),
    ]),
  );
}

String _first(Object e) => e.toString().split('\n').firstWhere((l) => l.trim().isNotEmpty, orElse: () => '$e').trim();

Future<void> snap(WidgetTester t, Sz z, String name, String desc, {String suffix = ''}) async {
  await settle(t, 6);
  final dir = z.dir + suffix;
  final n = (counters[dir] ?? 0) + 1;
  counters[dir] = n;
  final file = '${n.toString().padLeft(2, '0')}_$name.png';
  Object? e;
  while ((e = t.takeException()) != null) {
    problems.add('$dir/$file: ${_first(e!)}');
  }
  if (!kPages) return;
  await t.runAsync(() async {
    final b = shotKey.currentContext!.findRenderObject() as RenderRepaintBoundary;
    final img = await b.toImage(pixelRatio: 2.0);
    final bytes = await img.toByteData(format: ui.ImageByteFormat.png);
    final f = File('$kRoot/$dir/$file');
    f.parent.createSync(recursive: true);
    f.writeAsBytesSync(bytes!.buffer.asUint8List());
  });
  recs.add(Rec(dir, file, desc, foldNote(z, _posture)));
}

/// Runs one step; a missing widget, a thrown error or a layout exception is recorded and the sweep goes on.
Future<void> step(WidgetTester t, Sz z, String what, Future<void> Function() f) async {
  try {
    await f();
  } catch (e) {
    unreached.add('${z.dir}: $what -> ${_first(e)}');
  }
  Object? e;
  while ((e = t.takeException()) != null) {
    problems.add('${z.dir}: during "$what": ${_first(e!)}');
  }
}

Finder key(String k) => find.byKey(ValueKey(k));

Future<void> tapK(WidgetTester t, String k, {int n = 4}) async {
  if (k.startsWith('kind-') || k.startsWith('chip-') || k.startsWith('face-') || k.startsWith('part-')) await t.ensureVisible(key(k));
  await t.tap(key(k));
  await settle(t, n);
}

Future<void> dock(WidgetTester t, EGroup g) async {
  // the fold layouts have no dock: the category grid of the column, the rail of the cluster
  final d = find.byKey(ValueKey('dock-$g'));
  final f = d.evaluate().isNotEmpty ? d : (find.byKey(ValueKey('fold-cat-${g.name}')).evaluate().isNotEmpty ? find.byKey(ValueKey('fold-cat-${g.name}')) : find.byKey(ValueKey('cluster-cat-${g.name}')));
  await t.tap(f);
  await settle(t, 8);
}

Future<void> drain(WidgetTester t) async {
  try {
    await t.pump(const Duration(seconds: 60));
  } catch (_) {}
  while (t.takeException() != null) {}
}

Future<({AppState app, String id, LocalController? lc})> openEd(WidgetTester t, Sz z,
    {EGroup group = EGroup.local, bool dark = false, LocalAi? ai, void Function(AppState, String)? before, String title = '外滩夕阳', AppState? useApp}) async {
  await loadTestFonts();
  final app = useApp ?? AppState.demo();
  final id = app.photos.firstWhere((p) => p.title == title && p.id.startsWith('d')).id;
  before?.call(app, id);
  await t.pumpWidget(framed(
      z,
      MaterialApp(
        debugShowCheckedModeBanner: false,
        theme: buildTheme(Brightness.light),
        darkTheme: buildTheme(Brightness.dark),
        themeMode: dark ? ThemeMode.dark : ThemeMode.light,
        home: EditorPage(app: app, photoId: id, resamplePointers: false, initialGroup: group, localAi: ai),
      )));
  await settle(t, 8);
  LocalController? lc;
  final f = find.byType(LocalPhotoLayer);
  if (f.evaluate().isNotEmpty) lc = t.widget<LocalPhotoLayer>(f).lc;
  return (app: app, id: id, lc: lc);
}

void filmSetup(AppState app, String id) {
  app.film = testFilmCatalog;
  app.applyEdit(id, (e) => e.copyWith(film: 'kodak_portra_400'));
}

Future<void> pumpDappleIn(WidgetTester t, Sz z, AppState app, {bool dark = false}) async {
  await loadTestFonts();
  await t.pumpWidget(framed(z, DappleApp(state: app, forceBrightness: dark ? Brightness.dark : Brightness.light)));
  await t.pump(const Duration(milliseconds: 200));
}

class _HoldApp extends AppState {
  _HoldApp({this.fail = false}) : super.demo();
  final bool fail;
  final gate = Completer<void>();
  int n = 0;
  @override
  Future<String> exportJpeg(Photo ph, {int? longEdge, String? cropOverride, bool hdr = false, bool? stripLocation, bool wideGamut = false}) async {
    n++;
    if (fail && n >= 2) throw StateError('export failed');
    if (!fail && n >= 2) await gate.future;
    return '/demo/${ph.id}.jpg';
  }
}

class _HdrApp extends AppState {
  _HdrApp() : super.demo();
  @override
  Future<double> hdrHeadroomOf(Photo ph) async => 2.0;
  @override
  Future<double> wideGamutOf(Photo ph) async => 0.2;
}

class _FailingCard extends FakeUsbBackend {
  @override
  Future<UsbPage> list({int cursor = 0, int limit = 60, bool refresh = false}) async => throw StateError('card read failed');
}

Photo ph(String id, PhotoStatus st, {String? error}) =>
    Photo(id: id, name: '$id.jpg', addedMs: DateTime(2026, 11, 11, 16).millisecondsSinceEpoch, status: st, error: error);

void main() {
  setUp(() => FilmMapStore.debugSet({
        'kodak_portra_400': const Offset(-0.5, -0.3),
        'kodak_ektar_100': const Offset(0.6, 0.5),
        'fujifilm_velvia_100': const Offset(0.0, 0.8),
      }));
  tearDown(() => FilmMapStore.debugSet(null));

  tearDownAll(() {
    if (!kPages) return;
    final b = StringBuffer('# Dapple page screenshots\n\nGenerated by app/test/ui/pages_sweep_test.dart (host widget tests, fake engine, demo photos, light theme unless the file name says dark). 2x pixels.\n');
    final dirs = <String>[];
    for (final r in recs) {
      if (!dirs.contains(r.dir)) dirs.add(r.dir);
    }
    for (final d in dirs) {
      final list = recs.where((r) => r.dir == d).toList();
      b.writeln('\n## $d (${list.length} screenshots)\n');
      for (final r in list) {
        b.writeln('- `$d/${r.file}` ${r.desc}${r.fold.isEmpty ? '' : ' [${r.fold}]'}');
      }
    }
    b.writeln('\n## Display features\n');
    b.writeln('Book-style sizes carry a vertical fold in the middle (landscape: horizontal), flip-open a horizontal fold in the middle, all passed through the view as `displayFeatures` (logical pixels, flat posture). The `*-halfopen` folders (flip-open and wide-fold-open portrait) repeat a few screens in half-opened posture with a red 2 px line drawn over the fold. The editor reads `displayFeatures` (docs/design/foldables.md): near-square inner screens get the workbench / column layouts, a half-opened horizontal fold puts the photo above the fold and the controls below it; the `*-fold` folders show those layouts; screenshots in these folders carry the fold note in brackets.');
    b.writeln('\n## Steps that could not be reached\n');
    if (unreached.isEmpty) b.writeln('- none recorded');
    for (final u in unreached) {
      b.writeln('- $u');
    }
    b.writeln('\n## Layout exceptions raised while rendering (overflow etc.)\n');
    if (problems.isEmpty) b.writeln('- none');
    for (final p in problems) {
      b.writeln('- $p');
    }
    b.writeln('\n## Not reachable on the host (by design)\n');
    b.writeln('- Import choice sheet 从相册选… / 从文件选…: the top-bar "+" only shows it when `Platform.isAndroid`; on the host it goes straight to the picker backend. The empty home state shows 从相册选… and 连接相机 instead.');
    b.writeln('- Camera capture screen (拍照): native camera view.');
    b.writeln('- System share sheet and system photo picker: OS surfaces.');
    File('$kRoot/index.md')
      ..parent.createSync(recursive: true)
      ..writeAsStringSync(b.toString());
  });

  for (final z in sizes) {
    // ------------------------------------------------------------------------------------------------ home
    testWidgets('home ${z.dir}', (t) async {
      win(t, z);
      await loadTestFonts();
      Future<void> pump(AppState app, {bool dark = false}) async {
        await pumpDappleIn(t, z, app, dark: dark);
      }

      await step(t, z, 'home grid', () async {
        await pump(AppState.demo());
        await snap(t, z, 'home_grid', '首页: photo grid with the connection card (demo photos)');
      });
      await step(t, z, 'home empty', () async {
        await pump(AppState.demo(scenario: DemoScenario.empty));
        await snap(t, z, 'home_empty', '首页: empty state (还没有照片, 从相册选… / 连接相机)');
      });
      await step(t, z, 'home disconnected', () async {
        await pump(AppState.demo(scenario: DemoScenario.disconnected));
        await snap(t, z, 'home_card_disconnected', '首页: connection card, camera disconnected');
      });
      await step(t, z, 'home transfer failed', () async {
        await pump(AppState.demo(scenario: DemoScenario.transferFailed));
        await snap(t, z, 'home_card_transfer_failed', '首页: connection card, transfer failed');
      });
      await step(t, z, 'home permission', () async {
        await pump(AppState()
          ..ready = true
          ..mediaPermitted = false);
        await snap(t, z, 'home_permission', '首页: photo-permission explanation state (还不能读取照片, 允许访问)');
      });
      await step(t, z, 'home tiles', () async {
        final app = AppState.demo(scenario: DemoScenario.empty);
        app.simulatePhotoForTest(ph('q', PhotoStatus.pending));
        app.simulatePhotoForTest(ph('d', PhotoStatus.developing));
        app.simulatePhotoForTest(ph('f', PhotoStatus.failed, error: 'PlatformException(convert, cannot convert IMG_1.HEIC to JPEG)'));
        app.simulatePhotoForTest(ph('n', PhotoStatus.failed, error: 'network: icloud download failed'));
        await pump(app);
        await t.pump(const Duration(milliseconds: 200));
        await snap(t, z, 'home_tiles_states', '首页: tiles waiting / developing / failed');
      });
      await step(t, z, 'home selection', () async {
        final app = AppState.demo();
        await pump(app);
        final p = app.photos.firstWhere((p) => p.status == PhotoStatus.done && p.kind == PhotoKind.photo);
        app.enterSelect(p.id);
        await settle(t, 3);
        await snap(t, z, 'home_select', '首页: selection mode with the action bar (保存 / 合成 / 发布)');
        app.exitSelect();
      });
      await step(t, z, 'styles tab', () async {
        await pump(AppState.demo());
        await t.tap(key('nav-1'));
        await t.pump(const Duration(milliseconds: 500));
        await snap(t, z, 'tab_default_style', '默认成像 tab');
      });
      await step(t, z, 'settings', () async {
        await pump(AppState.demo());
        await t.tap(key('nav-2'));
        await t.pump(const Duration(milliseconds: 500));
        await snap(t, z, 'tab_settings', '设置 tab');
        await t.drag(find.byType(Scrollable).first, const Offset(0, -700));
        await t.pump(const Duration(milliseconds: 300));
        await snap(t, z, 'tab_settings_scrolled', '设置 tab scrolled down');
      });
      await step(t, z, 'licences', () async {
        await loadTestFonts();
        await t.pumpWidget(framed(z, MaterialApp(debugShowCheckedModeBanner: false, theme: buildTheme(Brightness.light), home: LicensesPage(app: AppState.demo()))));
        await t.pump(const Duration(milliseconds: 500));
        await snap(t, z, 'settings_licences', '设置 > 开源许可 page');
      });
      await drain(t);
    });

    // ------------------------------------------------------------------------------------------------ usb
    testWidgets('usb ${z.dir}', (t) async {
      win(t, z);
      await loadTestFonts();
      Future<(AppState, FakeUsbBackend)> open(FakeUsbBackend cam) async {
        final app = AppState.demo()..usbBackend = cam;
        await app.initUsb(inboxPath: '/inbox');
        app.usb.isKnown = (f) => f.name == 'DSC00001.JPG';
        await pumpDappleIn(t, z, app);
        return (app, cam);
      }

      await step(t, z, 'usb card states', () async {
        final (_, cam) = await open(FakeUsbBackend());
        Future<void> st(String name, String desc) async {
          await t.pump(const Duration(milliseconds: 100));
          await snap(t, z, name, desc);
        }
        cam.emit({'type': 'state', 'state': 'permission'});
        await st('usb_card_permission', '首页 connection card: camera plugged in, waiting for USB permission');
        cam.ready();
        await st('usb_card_ready', '首页 connection card: camera ready (导入照片…)');
        cam.emit({'type': 'newShots', 'groups': [camGroup('DSC00009', [camFile(9, 'DSC00009.JPG')])]});
        cam.emit({'type': 'importStart', 'name': 'DSC00009.JPG', 'key': 'usb:dsc00009.jpg|100|1700000000', 'size': 100});
        cam.emit({'type': 'importProgress', 'name': 'DSC00009.JPG', 'bytes': 40, 'size': 100});
        await st('usb_card_receiving', '首页 connection card: receiving a shot with progress');
        cam.emit({'type': 'state', 'state': 'unplugged'});
        await st('usb_card_unplugged', '首页 connection card: cable unplugged');
        await t.pump(const Duration(seconds: 50));
      });
      await step(t, z, 'usb picker', () async {
        final cam = FakeUsbBackend();
        final (_, _) = await open(cam);
        cam.card = [
          for (var i = 20; i > 3; i--) UsbGroup.fromMap(camGroup('DSC000$i', [camFile(i, 'DSC000$i.JPG'), camFile(100 + i, 'DSC000$i.ARW', size: 900)])),
          UsbGroup.fromMap(camGroup('DSC00001', [camFile(1, 'DSC00001.JPG')])),
        ];
        cam.ready();
        await t.pump();
        await t.tap(key('conn-card-tap'));
        await t.pumpAndSettle();
        await snap(t, z, 'usb_picker', 'USB picker page: camera card list, known photo locked');
        await t.tap(key('usb-tile-100msdcf/dsc00020'));
        await t.pump();
        await t.tap(key('usb-tile-100msdcf/dsc00019'));
        await t.pump();
        await snap(t, z, 'usb_picker_selected', 'USB picker page: two shots selected');
      });
      await step(t, z, 'usb picker failed', () async {
        final cam = _FailingCard();
        await open(cam);
        cam.ready();
        await t.pump();
        await t.tap(key('conn-card-tap'));
        await t.pumpAndSettle();
        await snap(t, z, 'usb_picker_failed', 'USB picker page: card read failed, 重试');
      });
      await drain(t);
    });

    // ------------------------------------------------------------------------------------------------ tools
    testWidgets('tools ${z.dir}', (t) async {
      win(t, z);
      await loadTestFonts();
      final messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(const MethodChannel('dapple/live'), (call) async {
        switch (call.method) {
          case 'info':
            return {'durationMs': 14000, 'width': 1080, 'height': 1920, 'rotation': 0, 'hasAudio': true};
          case 'thumbs':
            return List<String?>.filled((call.arguments['timesMs'] as List).length, null);
          case 'frame':
            return {'path': call.arguments['dest'], 'kind': 'sdr-jpeg'};
        }
        return null;
      });
      addTearDown(() => messenger.setMockMethodCallHandler(const MethodChannel('dapple/live'), null));
      final app = AppState.demo()..engine = FakeEngine();
      await step(t, z, 'home with tool cards', () async {
        await pumpDappleIn(t, z, app);
        await snap(t, z, 'tools_home_cards', '首页 with the detected HDR / 全景 / 视频 → 动态照片 entry cards');
      });
      Future<void> back() async {
        await t.binding.handlePopRoute();
        await settle(t, 6);
      }

      final cards = app.detectedCards.toList();
      for (final g in cards) {
        await step(t, z, 'tool ${g.kind}', () async {
          await t.ensureVisible(key('entry-${g.kind}-${g.photoIds.first}'));
          await t.tap(key('entry-${g.kind}-${g.photoIds.first}'));
          await settle(t, 6);
          await t.pump(const Duration(milliseconds: 500));
          await snap(t, z, 'tool_${g.kind}', '${g.kind == 'panorama' ? '全景合成' : 'HDR 合成'} tool page from the detected card');
          await back();
        });
      }
      await step(t, z, 'tool picker', () async {
        final g = cards.firstWhere((g) => g.isBracket);
        await t.ensureVisible(key('entry-${g.kind}-${g.photoIds.first}'));
        await t.tap(key('entry-${g.kind}-${g.photoIds.first}'));
        await settle(t, 6);
        await t.tap(key('tool-pick'));
        await settle(t, 6);
        await snap(t, z, 'tool_picker', 'merge tool: photo picker sheet');
        await t.tap(key('pick-cancel'));
        await settle(t, 6);
        await back();
      });
      await step(t, z, 'compose chooser', () async {
        final done = app.photos.where((p) => p.status == PhotoStatus.done && p.kind == PhotoKind.photo).take(3).toList();
        app.enterSelect(done[0].id);
        app.toggleSelect(done[1].id);
        await settle(t, 3);
        await t.tap(key('sel-compose'));
        await t.pump();
        await t.pump(const Duration(milliseconds: 500));
        await snap(t, z, 'tool_compose_chooser', '合成 chooser (HDR / 全景拼接) from a multi-selection');
        await t.tap(find.text('全景拼接'));
        await settle(t, 6);
        await t.pump(const Duration(milliseconds: 500));
        await snap(t, z, 'tool_panorama_from_selection', '全景合成 page opened from a selection');
        await back();
        app.exitSelect();
        await settle(t, 4);
      });
      await step(t, z, 'live tool', () async {
        await t.ensureVisible(key('entry-video'));
        await t.tap(key('entry-video'));
        await settle(t, 6);
        await t.pump(const Duration(milliseconds: 500));
        await snap(t, z, 'tool_live', '视频转动态照片 (live / motion photo) tool page');
        await back();
      });
      await drain(t);
    });

    // ------------------------------------------------------------------------------------------------ publish
    testWidgets('publish ${z.dir}', (t) async {
      win(t, z);
      await loadTestFonts();
      final share = FakeShare();
      ShareService.instance = share;
      Future<void> host(AppState app, List<String> ids) async {
        await t.pumpWidget(framed(
            z,
            MaterialApp(
              debugShowCheckedModeBanner: false,
              theme: buildTheme(Brightness.light),
              home: Scaffold(body: Center(child: Builder(builder: (ctx) => TextButton(key: const ValueKey('host-open'), onPressed: () => openPublish(ctx, app, ids), child: const Text('open'))))),
            )));
        await t.tap(key('host-open'));
        await t.pump();
        await t.pump(const Duration(milliseconds: 800));
      }

      List<String> ids(AppState a, int n) => a.editable.where((p) => p.status == PhotoStatus.done).take(n).map((e) => e.id).toList();

      await step(t, z, 'publish page', () async {
        final app = _HdrApp();
        await host(app, ids(app, 3));
        await snap(t, z, 'publish_default', '发布 page: preset 系统分享 selected (default), options');
        await t.tap(key('preset-xhs'));
        await t.pump(const Duration(milliseconds: 300));
        await snap(t, z, 'publish_preset_xhs', '发布 page: preset 小红书 selected, ratio and size rows');
        final more = find.byKey(const ValueKey('row-more'));
        if (more.evaluate().isNotEmpty) {
          await t.ensureVisible(more);
          await t.tap(more);
          await t.pump(const Duration(milliseconds: 200));
        }
        final sw = key('switch-hdr');
        if (sw.evaluate().isNotEmpty) {
          await t.ensureVisible(sw);
          await t.tap(sw);
          await t.pump(const Duration(milliseconds: 300));
          await snap(t, z, 'publish_hdr_on', '发布 page: HDR option on, scrolled to the options');
        }
      });
      await step(t, z, 'publish progress and done', () async {
        final app = _HoldApp();
        await host(app, ids(app, 3));
        await t.tap(key('publish-share'));
        await t.pump();
        await t.pump(const Duration(milliseconds: 600));
        await t.tap(key('handoff-system'));
        await t.pump();
        await t.pump(const Duration(milliseconds: 600));
        await snap(t, z, 'publish_exporting', '发布: export in progress dialog (1 of 3 exported)');
        app.gate.complete();
        await t.pump();
        await t.pump(const Duration(milliseconds: 800));
        await t.pump(const Duration(milliseconds: 800));
        await snap(t, z, 'publish_done', '发布 done page (已交给…, steps, buttons)');
      });
      await step(t, z, 'publish failure', () async {
        final app = _HoldApp(fail: true);
        await host(app, ids(app, 3));
        await t.tap(key('publish-share'));
        await t.pump();
        await t.pump(const Duration(milliseconds: 600));
        await t.tap(key('handoff-system'));
        await t.pump();
        await t.pump(const Duration(milliseconds: 600));
        await snap(t, z, 'publish_failed', '发布: export failed, 没导出成功 toast with 重试 over the 发布 page');
      });
      await drain(t);
    });

    // ------------------------------------------------------------------------------------------------ editor, global categories
    testWidgets('editor global ${z.dir}', (t) async {
      win(t, z);
      final r = await openEd(t, z, group: EGroup.light, before: filmSetup);
      await step(t, z, '光线', () async => snap(t, z, 'editor_light', '编辑 光线 default layer'));
      await step(t, z, '光线 分项', () async {
        await t.tap(key('items-button'));
        await settle(t, 8);
        await snap(t, z, 'editor_light_items', '编辑 光线 分项 layer (the itemised sliders)');
        await t.tap(key('header-back'));
        await settle(t, 8);
      });
      await step(t, z, 'hold to compare', () async {
        final c = t.getCenter(key('zoom-stage'));
        final g = await t.startGesture(c);
        await t.pump(const Duration(milliseconds: 450));
        await t.pump(const Duration(milliseconds: 300));
        await snap(t, z, 'editor_compare_hold', '编辑: holding the photo shows the original with the 原片 pill');
        await g.up();
        await settle(t, 3);
      });
      await step(t, z, 'undo snackbar', () async {
        final f = await Finger.down(t, padAt(t, PadCat.light, 0.6, -0.5));
        await f.up();
        await settle(t, 4);
        await t.tap(key('reset-light'));
        await settle(t, 3);
        await snap(t, z, 'editor_undo_toast', '编辑: after 复位, the toast 已复位光线 with 撤销');
      });
      await step(t, z, '颜色', () async {
        await dock(t, EGroup.color);
        await snap(t, z, 'editor_color', '编辑 颜色 default layer');
      });
      await step(t, z, '成像', () async {
        await dock(t, EGroup.style);
        await snap(t, z, 'editor_film', '编辑 成像 default layer (stock chip + film pad)');
      });
      await step(t, z, 'film map', () async {
        await t.tap(key('stock-button'));
        await settle(t, 10);
        await snap(t, z, 'editor_film_map_idle', '编辑 成像: film map open, idle');
        await t.tap(key('header-back'));
        await settle(t, 6);
        final chip = t.getCenter(key('stock-button'));
        final f = await Finger.down(t, chip);
        await t.pump();
        await t.pump(const Duration(milliseconds: 50));
        final map = t.getRect(key('film-map'));
        await f.drag(map.topLeft + Offset(map.width * 0.8, map.height * 0.6) - chip, step: 12);
        await f.by(const Offset(0, 30));
        await f.by(const Offset(-20, 10));
        await settle(t, 6);
        await snap(t, z, 'editor_film_map_sliding', '编辑 成像: press-slide-release from the chip, a stock magnified under the finger');
        await f.up();
        await settle(t, 10);
      });
      await step(t, z, '质地', () async {
        await dock(t, EGroup.effects);
        await snap(t, z, 'editor_texture', '编辑 质地 default layer');
      });
      await step(t, z, '裁切', () async {
        await dock(t, EGroup.crop);
        await snap(t, z, 'editor_crop', '编辑 裁切 layer with the ratio chips');
        await tapK(t, 'crop-${cropRatios[2]}');
        await snap(t, z, 'editor_crop_ratio', '编辑 裁切 layer: ratio 4:5 chosen');
      });
      expect(r.id, isNotEmpty);
      await drain(t);
    });

    // ------------------------------------------------------------------------------------------------ editor, 局部
    testWidgets('editor local ${z.dir}', (t) async {
      win(t, z);
      final fake = FakeLocalAi()..setFace(1, iod: 0.125);
      await openEd(t, z, ai: fake);
      await step(t, z, '局部 empty', () async => snap(t, z, 'local_empty', '编辑 局部: empty state, the shape card (add menu)'));
      await step(t, z, 'circle', () async {
        await tapK(t, 'kind-radial');
        await snap(t, z, 'local_circle', '编辑 局部: circle area, one full-size light pad with the 光线 / 颜色 / 质地 switch');
        await tapK(t, 'local-seg-texture');
        await snap(t, z, 'local_texture_pad', '编辑 局部: the 质地 pad (柔 / 锐, 朦胧 / 通透)');
        await tapK(t, 'local-seg-color');
        await snap(t, z, 'local_color_pad', '编辑 局部: the 颜色 pad');
        await tapK(t, 'local-seg-light');
      });
      await step(t, z, 'gradient', () async {
        await tapK(t, 'local-add');
        await snap(t, z, 'local_add_menu', '编辑 局部: 加一块 (shape card with an area present)');
        await tapK(t, 'kind-linear');
        await snap(t, z, 'local_gradient', '编辑 局部: gradient area');
        await snap(t, z, 'local_two_areas', '编辑 局部: two areas (chips line, the switch beside or under it)');
      });
      await step(t, z, 'brush', () async {
        await tapK(t, 'local-add');
        await tapK(t, 'kind-brush');
        await snap(t, z, 'local_brush', '编辑 局部: brush area with the tool strip under the photo');
      });
      await step(t, z, 'tone range', () async {
        await tapK(t, 'local-add');
        await tapK(t, 'kind-luminance');
        await snap(t, z, 'local_tone_range', '编辑 局部: tone (luminance) range area');
      });
      await step(t, z, 'action card', () async {
        await tapK(t, 'local-chip-0');
        await tapK(t, 'local-chip-0');
        await snap(t, z, 'local_action_card', '编辑 局部: second tap on the selected area chip opens its action card');
      });
      await step(t, z, 'local items', () async {
        await t.tap(key('header-back'));
        await settle(t, 6);
        await t.tap(key('items-button'));
        await settle(t, 8);
        await snap(t, z, 'local_items', '编辑 局部 分项 layer of an area');
      });
      await step(t, z, 'person parts', () async {
        final f2 = FakeLocalAi()..setFace(1, iod: 0.125);
        await openEd(t, z, ai: f2);
        await tapK(t, 'kind-person');
        await snap(t, z, 'local_person_analysing', '编辑 局部: 人物 analysing (progress, 取消)');
        f2.finish(portrait());
        await settle(t, 4);
        await t.ensureVisible(key('part-faceskin'));
        await tapK(t, 'part-faceskin');
        await snap(t, z, 'local_person_parts', '编辑 局部: person parts picker with one part picked');
      });
      await drain(t);
    });

    // ------------------------------------------------------------------------------------------------ editor, 人像
    testWidgets('editor portrait ${z.dir}', (t) async {
      win(t, z);
      final fake = FakeLocalAi()..setFace(1, iod: 0.125);
      final r = await openEd(t, z, ai: fake);
      await step(t, z, 'portrait main', () async {
        await tapK(t, 'kind-portrait');
        fake.finish(portraitResult());
        await settle(t, 4);
        await snap(t, z, 'portrait_main', '编辑 局部 人像: main layer with the chips');
      });
      await step(t, z, 'portrait chip action card', () async {
        await tapK(t, 'chip-skin');
        await snap(t, z, 'portrait_chip_actions', '编辑 人像: second tap on the selected chip opens its action card');
        await tapK(t, 'header-back');
      });
      await step(t, z, 'portrait skin', () async {
        r.lc!.showBody(LocalBody.pads);
        await settle(t, 3);
        await snap(t, z, 'portrait_skin_pads', '编辑 人像: 皮肤 chip, pads and the 点一下祛斑 switch');
      });
      await step(t, z, 'heal on', () async {
        await tapK(t, 'heal-mode');
        await snap(t, z, 'portrait_heal_on', '编辑 人像: 点一下祛斑 on, tool strip with the + / − switch');
        await tapK(t, 'heal-mode');
      });
      await step(t, z, 'portrait items', () async {
        await tapK(t, 'items-button');
        await snap(t, z, 'portrait_items', '编辑 人像 分项 layer');
      });
      await drain(t);
    });
  }

  // ---------------------------------------------------------------------------------------------------- fold layouts (docs/design/foldables.md)
  const foldSizes = <Sz>[
    Sz('book-inner-984x1092', 984, 1092, fold: Fold.vertical),
    Sz('book-inner-904x832', 904, 832, fold: Fold.vertical),
    Sz('wide-fold-inner-626x890', 626, 890, fold: Fold.horizontal),
    Sz('wide-fold-inner-landscape-890x626', 890, 626, fold: Fold.vertical),
  ];
  for (final z in foldSizes) {
    const sfx = '-fold';
    testWidgets('fold workbench ${z.dir}', (t) async {
      win(t, z);
      final fake = FakeLocalAi()..setFace(1, iod: 0.125);
      await openEd(t, z, group: EGroup.light, before: filmSetup, ai: fake);
      await step(t, z, '光线', () async => snap(t, z, 'bench_light', '工作台 光线: histogram + title row left, strip + pad right, the cluster on the photo', suffix: sfx));
      await step(t, z, '分项', () async {
        await t.tap(key('items-button'));
        await settle(t, 8);
        await snap(t, z, 'bench_light_items', '工作台 光线 分项 (swaps with the pad)', suffix: sfx);
        await t.tap(key('header-back'));
        await settle(t, 6);
      });
      await step(t, z, 'ball push', () async {
        final c = t.getCenter(key('cluster-ball'));
        final g = await t.startGesture(c);
        await t.pump(const Duration(milliseconds: 50));
        await g.moveBy(const Offset(34, -20));
        await g.moveBy(const Offset(12, -8));
        await t.pump(const Duration(milliseconds: 100));
        await snap(t, z, 'bench_ball_push', '工作台: pushing the ball moves the pad cursor relatively', suffix: sfx);
        await g.up();
        await settle(t, 4);
      });
      await step(t, z, '颜色', () async {
        await dock(t, EGroup.color);
        await snap(t, z, 'bench_color', '工作台 颜色', suffix: sfx);
      });
      await step(t, z, '成像', () async {
        await dock(t, EGroup.style);
        await settle(t, 8);
        await snap(t, z, 'bench_film', '工作台 成像: the film map', suffix: sfx);
      });
      await step(t, z, '质地', () async {
        await dock(t, EGroup.effects);
        await snap(t, z, 'bench_texture', '工作台 质地', suffix: sfx);
      });
      await step(t, z, '裁切', () async {
        await dock(t, EGroup.crop);
        await snap(t, z, 'bench_crop', '工作台 裁切: ratio chips + straighten ruler', suffix: sfx);
      });
      await step(t, z, '局部', () async {
        await dock(t, EGroup.local);
        await snap(t, z, 'bench_local_empty', '工作台 局部: the area-type tiles', suffix: sfx);
        await tapK(t, 'kind-radial');
        await snap(t, z, 'bench_local_circle', '工作台 局部: circle area, strip + pad with the 光线/颜色/质地 switch', suffix: sfx);
      });
      await drain(t);
    });
    testWidgets('fold workbench left hand ${z.dir}', (t) async {
      win(t, z);
      await openEd(t, z, group: EGroup.light, before: (app, id) => app.settings.handedness = 1);
      await step(t, z, 'left hand', () async => snap(t, z, 'bench_light_left', '工作台 光线, left hand: the pad on the left, the cluster at the left edge', suffix: sfx));
      await drain(t);
    });
    testWidgets('fold column ${z.dir}', (t) async {
      win(t, z);
      await openEd(t, z, group: EGroup.light, before: filmSetup, title: '街角咖啡');
      await step(t, z, '光线', () async => snap(t, z, 'column_light', '竖构图 光线: one column on the holding side', suffix: sfx));
      await step(t, z, '分项', () async {
        await t.tap(key('items-button'));
        await settle(t, 8);
        await snap(t, z, 'column_light_items', '竖构图 光线 分项', suffix: sfx);
        await t.tap(key('header-back'));
        await settle(t, 6);
      });
      await step(t, z, '成像', () async {
        await dock(t, EGroup.style);
        await settle(t, 8);
        await snap(t, z, 'column_film', '竖构图 成像: the film map', suffix: sfx);
      });
      await step(t, z, '裁切', () async {
        await dock(t, EGroup.crop);
        await snap(t, z, 'column_crop', '竖构图 裁切', suffix: sfx);
      });
      await step(t, z, '局部', () async {
        await dock(t, EGroup.local);
        await snap(t, z, 'column_local_empty', '竖构图 局部', suffix: sfx);
        await tapK(t, 'kind-radial');
        await snap(t, z, 'column_local_circle', '竖构图 局部 circle', suffix: sfx);
      });
      await step(t, z, 'swap', () async {
        await t.drag(key('fold-swap'), const Offset(-150, 0));
        await settle(t, 8);
        await snap(t, z, 'column_swapped', '竖构图: the column moved to the other side', suffix: sfx);
      });
      await drain(t);
    });
  }

  // ---------------------------------------------------------------------------------------------------- dark, phone portrait
  testWidgets('editor dark default layer', (t) async {
    final z = sizes.first;
    win(t, z);
    await openEd(t, z, group: EGroup.light, dark: true, before: filmSetup);
    await snap(t, z, 'editor_light_dark', '编辑 光线 default layer, dark theme');
    await drain(t);
  });

  // ---------------------------------------------------------------------------------------------------- flip, half-opened posture
  for (final zd in ['flip-open-412x1004', 'wide-fold-open-500x800']) {
  testWidgets('half-opened $zd', (t) async {
    final z = sizes.firstWhere((s) => s.dir == zd);
    win(t, z, posture: ui.DisplayFeatureState.postureHalfOpened, marker: true);
    const sfx = '-halfopen';
    await step(t, z, 'half-opened editor light', () async {
      await openEd(t, z, group: EGroup.light, before: filmSetup);
      await snap(t, z, 'editor_light', '编辑 光线 default layer, half-opened posture', suffix: sfx);
      await dock(t, EGroup.style);
      await snap(t, z, 'editor_film', '编辑 成像 default layer, half-opened posture', suffix: sfx);
      await dock(t, EGroup.crop);
      await snap(t, z, 'editor_crop', '编辑 裁切 layer, half-opened posture', suffix: sfx);
    });
    await step(t, z, 'half-opened local', () async {
      await openEd(t, z, ai: FakeLocalAi());
      await snap(t, z, 'local_empty', '编辑 局部 empty state, half-opened posture', suffix: sfx);
      await tapK(t, 'kind-radial');
      await snap(t, z, 'local_circle', '编辑 局部 circle area, half-opened posture', suffix: sfx);
    });
    await step(t, z, 'half-opened publish', () async {
      final app = AppState.demo();
      final ids = app.editable.take(2).map((e) => e.id).toList();
      await t.pumpWidget(framed(
          z,
          MaterialApp(
            debugShowCheckedModeBanner: false,
            theme: buildTheme(Brightness.light),
            home: Scaffold(body: Center(child: Builder(builder: (ctx) => TextButton(key: const ValueKey('host-open'), onPressed: () => openPublish(ctx, app, ids), child: const Text('open'))))),
          )));
      await t.tap(key('host-open'));
      await t.pump();
      await t.pump(const Duration(milliseconds: 800));
      await snap(t, z, 'publish_default', '发布 page, half-opened posture', suffix: sfx);
    });
    await drain(t);
  });
  }
}
