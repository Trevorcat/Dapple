import 'package:flutter/material.dart';

import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/float_bar.dart';
import '../../widgets/icons.dart';
import '../library/settings_page.dart';
import '../library/styles_page.dart';
import '../../services/tool_models.dart';
import '../publish/publish_page.dart';
import '../tools/launcher.dart';
import 'home_page.dart';
import '../../services/perf_probe.dart';

/// Window width at which the layout switches to rail + full-width grid (requirements 2.2).
const double kWideBreakpoint = 600;

const _tabs = [
  ('照片', DI.photos),
  ('成像', DI.styles),
  ('设置', DI.settings),
];

/// Root navigation: three destinations; a floating capsule above the bottom inset (phone; the editor dock's own surface, guidelines 15.6)
/// or a navigation rail (wide). Selection mode replaces the tabs with the selection capsule (保存 / 合成 / 发布) in both.
class DappleShell extends StatelessWidget {
  const DappleShell({super.key, required this.app});
  final AppState app;

  @override
  Widget build(BuildContext context) {
    return ListenableBuilder(
      listenable: app,
      builder: (context, _) {
        PerfProbe.count('shell');
        final c = context.c;
        final wide = MediaQuery.sizeOf(context).width >= kWideBreakpoint;
        final selecting = app.selectMode && app.tab == 0;
        final pages = IndexedStack(index: app.tab, children: [
          HomePage(app: app, wide: wide),
          StylesPage(app: app, wide: wide),
          SettingsPage(app: app, wide: wide),
        ]);
        Widget bar;
        if (selecting) {
          bar = _SelectBar(app: app);
        } else if (!wide) {
          bar = _NavBar(app: app);
        } else {
          bar = const SizedBox.shrink();
        }
        final capsule = selecting || !wide;
        return Scaffold(
          backgroundColor: c.bg,
          body: Stack(children: [
            Positioned.fill(
              child: Row(children: [
                if (wide && !selecting) _Rail(app: app),
                Expanded(child: pages),
              ]),
            ),
            Positioned(left: 0, right: 0, bottom: 0, child: bar),
            // the toast rides above the capsule when there is one
            ToastLayer(app: app, bottom: capsule ? floatBarBase(context) + kFloatBarGap + floatBarH(context) + Sp.m : 24),
          ]),
        );
      },
    );
  }
}

class _NavBar extends StatelessWidget {
  const _NavBar({required this.app});
  final AppState app;

  @override
  Widget build(BuildContext context) {
    return FloatBar(
      capsuleKey: const ValueKey('nav-bar'),
      children: [
        for (var i = 0; i < _tabs.length; i++)
          FloatBarItem(key: ValueKey('nav-$i'), label: _tabs[i].$1, icon: _tabs[i].$2, selected: app.tab == i, onTap: () => app.setTab(i)),
      ],
    );
  }
}

class _NavItem extends StatelessWidget {
  const _NavItem({required this.label, required this.icon, required this.selected, required this.width});
  final String label;
  final DI icon;
  final bool selected;
  final double width;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Column(mainAxisAlignment: MainAxisAlignment.center, children: [
      Container(
        width: width,
        height: 32,
        alignment: Alignment.center,
        decoration: BoxDecoration(color: selected ? c.secContainer : null, borderRadius: BorderRadius.circular(16)),
        child: DIcon(icon, size: 24, color: selected ? c.onSecContainer : c.text2, filled: selected),
      ),
      const SizedBox(height: 4),
      Text(label, style: T.cap(selected ? c.onSecContainer : c.text2, w: selected ? FontWeight.w500 : FontWeight.w400)),
    ]);
  }
}

class _Rail extends StatelessWidget {
  const _Rail({required this.app});
  final AppState app;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Container(
      key: const ValueKey('nav-rail'),
      width: 80,
      color: c.bg,
      padding: const EdgeInsets.only(top: 44),
      child: Column(children: [
        for (var i = 0; i < _tabs.length; i++)
          Semantics(
            button: true,
            selected: app.tab == i,
            label: _tabs[i].$1,
            excludeSemantics: true,
            child: GestureDetector(
              key: ValueKey('nav-$i'),
              behavior: HitTestBehavior.opaque,
              onTap: () => app.setTab(i),
              child: SizedBox(
                  width: 80, height: 72, child: _NavItem(label: _tabs[i].$1, icon: _tabs[i].$2, selected: app.tab == i, width: 56)),
            ),
          ),
      ]),
    );
  }
}

/// Android selection bar: 存相册 / 合成 icon buttons on the left, 发布 on the right.
class _SelectBar extends StatelessWidget {
  const _SelectBar({required this.app});
  final AppState app;

  Future<void> _save(BuildContext context) async {
    final list = app.selectedPhotos;
    final n = await app.saveToGallery(list);
    app.exitSelect();
    app.showToast('已存入相册「Dapple」· $n 张');
  }

  Future<void> _publish(BuildContext context) async {
    final ok = await openPublish(context, app, app.selectedIds);
    if (ok) app.exitSelect();
  }

  void _compose(BuildContext context) {
    final n = app.selectedIds.length;
    if (n < 2) {
      app.showToast('合成至少选 2 张');
      return;
    }
    showModalBottomSheet<void>(
      context: context,
      backgroundColor: context.c.surface,
      showDragHandle: true,
      shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(Rd.sheet))),
      builder: (ctx) => _ComposeSheet(
        count: n,
        onPick: (what) {
          Navigator.of(ctx).pop();
          openComposeFromSelection(context, app, what == 'hdr' ? ToolKind.hdr : ToolKind.pano).then((_) {
            if (app.selectMode && app.job?.status == JobStatus.done) app.exitSelect();
          });
        },
      ),
    );
  }

  static const double _selW = 84;

  @override
  Widget build(BuildContext context) {
    final on = app.selectedIds.isNotEmpty;
    return FloatBar(
      capsuleKey: const ValueKey('select-bar'),
      children: [
        // three items of one width, one icon-over-word layout; 发布 is the filled one
        FloatBarItem(key: const ValueKey('sel-save'), label: '存相册', icon: DI.save, width: _selW, onTap: on ? () => _save(context) : null),
        // HDR / panorama need the engine's merge build (not on iOS yet): no entry rather than a tool that can only fail.
        // always there (the row does not change with the count); with fewer than two photos it is drawn off and the tap says why
        if (app.engine.available)
          FloatBarItem(
              key: const ValueKey('sel-compose'),
              label: '合成',
              icon: DI.merge,
              width: _selW,
              dimmed: app.selectedIds.length < 2,
              semantics: app.selectedIds.length < 2 ? '合成，至少选 2 张' : '合成',
              onTap: () => _compose(context)),
        FloatBarItem(key: const ValueKey('sel-publish'), label: '发布', icon: DI.publish, width: _selW, primary: true, onTap: on ? () => _publish(context) : null),
      ],
    );
  }
}

class _ComposeSheet extends StatelessWidget {
  const _ComposeSheet({required this.count, required this.onPick});
  final int count;
  final ValueChanged<String> onPick;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final bottom = MediaQuery.paddingOf(context).bottom;
    Widget row(String t, String sub, String id) => InkWell(
          onTap: () => onPick(id),
          child: Container(
            height: 56,
            padding: const EdgeInsets.symmetric(horizontal: Sp.l),
            child: Row(children: [
              Expanded(
                child: Column(mainAxisAlignment: MainAxisAlignment.center, crossAxisAlignment: CrossAxisAlignment.start, children: [
                  Text(t, style: T.body17(c.text)),
                  Text(sub, style: T.smallL(c.text2)),
                ]),
              ),
              DIcon(DI.forward, size: 18, color: c.text2),
            ]),
          ),
        );
    return Padding(
      padding: EdgeInsets.fromLTRB(0, 0, 0, bottom + Sp.s),
      child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
        Padding(
            padding: const EdgeInsets.symmetric(horizontal: Sp.l),
            child: Text('合成 $count 张', style: T.body17(c.text, w: FontWeight.w500))),
        const SizedBox(height: Sp.s),
        row('HDR 合成', '包围曝光', 'hdr'),
        row('全景拼接', '连续拍摄', 'pano'),
      ]),
    );
  }
}
