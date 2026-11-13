import 'package:flutter/material.dart';

import '../../models.dart';
import '../../services/tool_models.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/icons.dart';
import '../../widgets/photo_art.dart';
import 'capture_flow.dart';
import 'conn_card.dart';
import 'import_flow.dart';
import '../tools/launcher.dart';
import 'photo_tile.dart';

/// 照片 tab: top bar, connection card, day groups, "新到 N 张" pill, empty state.
class HomePage extends StatefulWidget {
  const HomePage({super.key, required this.app, required this.wide});
  final AppState app;
  final bool wide;

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  final ScrollController _scroll = ScrollController();

  AppState get app => widget.app;

  @override
  void initState() {
    super.initState();
    _scroll.addListener(_onScroll);
  }

  @override
  void dispose() {
    _scroll.dispose();
    super.dispose();
  }

  void _onScroll() {
    final off = _scroll.hasClients ? _scroll.offset : 0;
    if (off > 48) {
      app.setFrozen(true); // freeze the list while the user is away from the top
    } else if (!app.selectMode) {
      app.setFrozen(false);
    }
  }

  void _toTop() {
    app.releaseNew();
    if (_scroll.hasClients) {
      _scroll.animateTo(0, duration: const Duration(milliseconds: 250), curve: Curves.easeOut);
    }
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final top = MediaQuery.paddingOf(context).top;
    return ColoredBox(
      color: c.bg,
      child: Column(children: [
        _TopBar(app: app, wide: widget.wide, top: top),
        Expanded(
          child: Stack(children: [
            _body(context),
            if (app.newCount > 0 && !app.selectMode)
              Positioned(
                top: 8,
                left: 0,
                right: 0,
                child: Center(child: _NewPill(count: app.newCount, onTap: _toTop)),
              ),
          ]),
        ),
      ]),
    );
  }

  Widget _body(BuildContext context) {
    final groups = app.groups;
    final bottomPad = widget.wide ? 24.0 : 120.0;
    return CustomScrollView(
      key: const ValueKey('home-scroll'),
      controller: _scroll,
      slivers: [
        const SliverToBoxAdapter(child: SizedBox(height: Sp.s)),
        // wide layouts show the status in the title row; the card only appears when something needs attention
        if (!widget.wide || app.connState != ConnState.connected) SliverToBoxAdapter(child: ConnCard(app: app)),
        if (app.hasBackgroundJob) SliverToBoxAdapter(child: _TaskBar(job: app.job!)),
        if (app.importing) const SliverToBoxAdapter(child: _ImportBar()),
        if (!app.isEmpty && !app.selectMode) SliverToBoxAdapter(child: _EntryCards(app: app)),
        if (app.isEmpty)
          SliverFillRemaining(hasScrollBody: false, child: _Empty(app: app))
        else
          for (final g in groups) ...[
            SliverToBoxAdapter(child: _GroupHeader(app: app, group: g)),
            SliverGrid(
              delegate: SliverChildBuilderDelegate(
                (context, i) => PhotoTile(app: app, photo: g.photos[i]),
                childCount: g.photos.length,
              ),
              gridDelegate: widget.wide
                  ? const SliverGridDelegateWithMaxCrossAxisExtent(
                      maxCrossAxisExtent: 126, mainAxisSpacing: Sp.grid, crossAxisSpacing: Sp.grid)
                  : const SliverGridDelegateWithFixedCrossAxisCount(
                      crossAxisCount: 3, mainAxisSpacing: Sp.grid, crossAxisSpacing: Sp.grid),
            ),
          ],
        SliverToBoxAdapter(child: SizedBox(height: bottomPad)),
      ],
    );
  }
}

class _TopBar extends StatelessWidget {
  const _TopBar({required this.app, required this.wide, required this.top});
  final AppState app;
  final bool wide;
  final double top;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    if (app.selectMode) {
      final n = app.selectedIds.length;
      return Container(
        key: const ValueKey('select-bar-top'),
        color: c.secContainer,
        padding: EdgeInsets.only(top: top, left: 4, right: 4),
        height: top + 64,
        child: Row(children: [
          DIconButton(DI.close, label: '退出选择', color: c.onSecContainer, onPressed: app.exitSelect),
          const SizedBox(width: 4),
          Text(n == 0 ? '选择照片' : '已选 $n 张', style: T.title(c.onSecContainer)),
        ]),
      );
    }
    return Container(
      color: c.bg,
      padding: EdgeInsets.only(top: top, left: Sp.l, right: 4),
      height: top + 64,
      child: Row(children: [
        Text('照片', style: T.title(c.text)),
        const Spacer(),
        if (wide && !app.isEmpty) ...[
          Dot(app.processingCount > 0 ? c.accent : c.okIcon),
          const SizedBox(width: 8),
          Text(
              app.processingCount > 0
                  ? '${app.sourceLabel} · 处理中 ${app.processingCount} 张'
                  : '${app.sourceLabel} · 已处理 ${app.processedCount} 张',
              style: T.body(c.text2, tabular: true)),
          const SizedBox(width: 8),
        ],
        if (app.isDemo || app.captureBackend.available)
          KeyedSubtree(
            key: const ValueKey('capture-button'),
            child: DIconButton(DI.camera, label: '拍照', onPressed: () => captureAndEdit(context, app)),
          ),
        KeyedSubtree(
          key: const ValueKey('import-button'),
          child: DIconButton(DI.plus, label: '从相册选择', onPressed: () => importMenu(context, app)),
        ),
        if (!app.isEmpty)
          KeyedSubtree(
            key: const ValueKey('select-toggle'),
            child: DIconButton(DI.select, label: '选择照片', onPressed: () => app.enterSelect()),
          ),
      ]),
    );
  }
}

class _NewPill extends StatelessWidget {
  const _NewPill({required this.count, required this.onTap});
  final int count;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      button: true,
      label: '新到 $count 张',
      excludeSemantics: true,
      child: GestureDetector(
        key: const ValueKey('new-pill'),
        onTap: onTap,
        child: Container(
          height: 44,
          padding: const EdgeInsets.symmetric(horizontal: 20),
          alignment: Alignment.center,
          decoration: BoxDecoration(
            color: c.accentBtn,
            borderRadius: BorderRadius.circular(22),
            boxShadow: const [BoxShadow(color: Color(0x40000000), blurRadius: 8, offset: Offset(0, 3))],
          ),
          child: Text('新到 $count 张', style: T.body(Colors.white, w: FontWeight.w500, tabular: true)),
        ),
      ),
    );
  }
}

class _GroupHeader extends StatelessWidget {
  const _GroupHeader({required this.app, required this.group});
  final AppState app;
  final PhotoGroup group;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final sel = group.photos.where(app.selectable).toList();
    final all = sel.isNotEmpty && sel.every((p) => app.isSelected(p.id));
    return Padding(
      padding: EdgeInsets.fromLTRB(Sp.l, app.selectMode ? Sp.m : Sp.xl, Sp.l, app.selectMode ? 0 : Sp.s),
      child: SizedBox(
        height: app.selectMode ? 48 : 28,
        child: Row(children: [
          Expanded(child: Text(group.title, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.body17(c.text, w: FontWeight.w500))),
          if (app.selectMode)
            TextAction(all ? '取消全选' : '全选', onPressed: () => all ? app.deselectAll(sel) : app.selectAll(sel))
          else
            Text(group.countText, style: T.small(c.text2, tabular: true)),
        ]),
      ),
    );
  }
}

/// Background task bar (requirements 4.2.1 item 3): spinner, what is running, percent. `role=status`.
class _TaskBar extends StatelessWidget {
  const _TaskBar({required this.job});
  final ToolJob job;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return ListenableBuilder(
      listenable: job,
      builder: (context, _) => Semantics(
        liveRegion: true,
        container: true,
        label: '${job.kind.runningTitle}，${job.percent}%',
        child: Container(
          key: const ValueKey('task-bar'),
          constraints: const BoxConstraints(minHeight: 44),
          margin: const EdgeInsets.fromLTRB(Sp.l, Sp.s, Sp.l, 0),
          padding: const EdgeInsets.symmetric(horizontal: Sp.l, vertical: Sp.s),
          decoration: BoxDecoration(color: c.secContainer, borderRadius: BorderRadius.circular(Rd.card)),
          child: Row(children: [
            Spinner(size: 18, color: c.onSecContainer),
            const SizedBox(width: Sp.m),
            Expanded(child: Text(job.kind.runningTitle, style: T.body(c.onSecContainer, w: FontWeight.w500))),
            Text('${job.percent}%', style: T.body(c.onSecContainer, tabular: true)),
          ]),
        ),
      ),
    );
  }
}

/// Shown while picked originals are copied into the app (a large RAW takes a few seconds).
class _ImportBar extends StatelessWidget {
  const _ImportBar();

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      liveRegion: true,
      container: true,
      label: '正在导入所选照片',
      child: Container(
        key: const ValueKey('import-bar'),
        constraints: const BoxConstraints(minHeight: 44),
        margin: const EdgeInsets.fromLTRB(Sp.l, Sp.s, Sp.l, 0),
        padding: const EdgeInsets.symmetric(horizontal: Sp.l, vertical: Sp.s),
        decoration: BoxDecoration(color: c.secContainer, borderRadius: BorderRadius.circular(Rd.card)),
        child: Row(children: [
          Spinner(size: 18, color: c.onSecContainer),
          const SizedBox(width: Sp.m),
          Expanded(child: Text('正在导入所选照片…', style: T.body(c.onSecContainer, w: FontWeight.w500))),
        ]),
      ),
    );
  }
}

/// Composite entry cards: detected HDR sets / panorama sequences and the latest video (requirements 4.2.4).
class _EntryCards extends StatelessWidget {
  const _EntryCards({required this.app});
  final AppState app;

  @override
  Widget build(BuildContext context) {
    final cards = app.detectedCards;
    final v = app.latestVideo;
    if (cards.isEmpty && v == null) return const SizedBox.shrink();
    return Column(key: const ValueKey('entry-cards'), children: [
      for (final g in cards)
        _EntryRow(
          key: ValueKey('entry-${g.kind}-${g.photoIds.first}'),
          title: g.title,
          subtitle: g.subtitle,
          photos: [for (final id in g.photoIds) ?app.photoById(id)],
          onTap: () => openMergeTool(context, app, app.draftForGroup(g)),
        ),
      if (v != null)
        _EntryRow(
          key: const ValueKey('entry-video'),
          title: '视频 → 动态照片',
          subtitle: '${LiveDraft.clock(v.seconds)} · 截取 3 秒',
          photos: const [],
          onTap: () => openLiveTool(context, app, video: v),
        ),
    ]);
  }
}

class _EntryRow extends StatelessWidget {
  const _EntryRow({super.key, required this.title, required this.subtitle, required this.photos, required this.onTap});
  final String title;
  final String subtitle;
  final List<Photo> photos;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Semantics(
      button: true,
      label: '$title，$subtitle',
      excludeSemantics: true,
      child: InkWell(
        onTap: onTap,
        child: Container(
          constraints: const BoxConstraints(minHeight: 64),
          padding: const EdgeInsets.symmetric(horizontal: Sp.l, vertical: Sp.s),
          child: Row(children: [
            SizedBox(
              width: 64,
              height: 48,
              child: photos.isEmpty
                  ? Center(
                      child: Container(
                          width: 40,
                          height: 40,
                          decoration: BoxDecoration(color: c.chip, borderRadius: BorderRadius.circular(Rd.thumb)),
                          child: Center(child: DIcon(DI.play, size: 18, color: c.text2))))
                  : Stack(children: [
                      for (var i = 0; i < photos.length.clamp(1, 3); i++)
                        Positioned(
                          left: i * 8.0,
                          top: (2 - i) * 2.0,
                          child: ClipRRect(
                            borderRadius: BorderRadius.circular(Rd.thumb),
                            child: Container(
                                width: 40,
                                height: 40,
                                decoration: BoxDecoration(
                                    border: Border.all(color: c.bg, width: 1.5), borderRadius: BorderRadius.circular(Rd.thumb)),
                                child: PhotoArt(photos[i], cacheWidth: 120)),
                          ),
                        ),
                    ]),
            ),
            const SizedBox(width: Sp.m),
            Expanded(
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
                Text(title, style: T.body17(c.text)),
                Text(subtitle, style: T.small(c.text2, tabular: true)),
              ]),
            ),
            DIcon(DI.forward, size: 18, color: c.text2),
          ]),
        ),
      ),
    );
  }
}

class _Empty extends StatelessWidget {
  const _Empty({required this.app});
  final AppState app;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Center(
      child: Column(mainAxisSize: MainAxisSize.min, children: [
        DIcon(DI.camera, size: 48, color: c.text2, stroke: 1.6),
        const SizedBox(height: Sp.m),
        Text('还没有照片', style: T.body17(c.text, w: FontWeight.w500)),
        const SizedBox(height: Sp.xs),
        Text('从相册选几张，或用相机传输', style: T.small(c.text2)),
        const SizedBox(height: Sp.l),
        SizedBox(
          width: 200,
          child: KeyedSubtree(
              key: const ValueKey('empty-import'),
              child: PrimaryButton('从相册选择', height: 48, onPressed: () => importAndEdit(context, app))),
        ),
        const SizedBox(height: Sp.s),
        SizedBox(
          width: 200,
          child: KeyedSubtree(
              key: const ValueKey('empty-connect'),
              child: SecondaryButton('连接相机', height: 48, onPressed: () => app.connectAction())),
        ),
        if (canPickFiles)
          KeyedSubtree(
              key: const ValueKey('empty-import-files'),
              child: TextAction('从文件选择', onPressed: () => importAndEdit(context, app, source: 'files'))),
        const SizedBox(height: 96),
      ]),
    );
  }
}
