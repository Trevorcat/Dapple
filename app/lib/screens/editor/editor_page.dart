// Viewer / editor (requirements 4.4-4.8, 4.14.1). Phone: stage + draggable two-detent sheet.
// Width >= 600: photo-dominant stage, filmstrip and a right inspector with collapsible groups.
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../../model/catalog.dart';
import '../../model/params.dart';
import '../../models.dart';
import '../../services/editor_engine.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/icons.dart';
import '../../widgets/photo_art.dart';
import '../publish/publish_page.dart';
import 'editor_panels.dart';
import 'editor_widgets.dart';
import 'sync_dialog.dart';

/// Opens the viewer / editor on [photoId] (full-screen route; the global navigation is hidden).
void openEditor(BuildContext context, AppState app, String photoId) {
  Navigator.of(context).push(MaterialPageRoute<void>(builder: (_) => EditorPage(app: app, photoId: photoId)));
}

class EditorPage extends StatefulWidget {
  const EditorPage({super.key, required this.app, required this.photoId});
  final AppState app;
  final String photoId;
  @override
  State<EditorPage> createState() => _EditorPageState();
}

class _EditorPageState extends State<EditorPage> {
  AppState get app => widget.app;
  late String id = widget.photoId;
  late final EditorEngine engine = EditorEngine(app);
  final PanelUi ui = PanelUi();

  EGroup group = EGroup.style;
  final Set<EGroup> expanded = {EGroup.style, EGroup.light};
  bool largeSheet = false;
  double? dragH;
  bool dragging = false;
  bool focus = false;
  bool compare = false;
  bool hold = false;
  bool sideBySide = false;
  bool menuOpen = false;
  Timer? holdTimer;
  bool holdFired = false;
  int lastRev = 0;

  final ScrollController tabScroll = ScrollController();
  final GlobalKey stripKey = GlobalKey();
  final Map<EGroup, GlobalKey> tabKeys = {for (final g in EGroup.values) g: GlobalKey()};

  Photo get ph => app.photoById(id)!;

  @override
  void initState() {
    super.initState();
    lastRev = app.editRevision;
    app.addListener(_onApp);
    engine.addListener(_rebuild);
    engine.attach(ph, neighbors: _neighborsOf(id));
    engine.refreshTiles();
  }

  @override
  void dispose() {
    app.removeListener(_onApp);
    engine.removeListener(_rebuild);
    holdTimer?.cancel();
    final p = app.photoById(id);
    if (p != null) Future.microtask(() => app.commitEdit(p));
    engine.dispose();
    tabScroll.dispose();
    super.dispose();
  }

  void _rebuild() {
    if (mounted) setState(() {});
  }

  bool get _styleVisible => group == EGroup.style || expanded.contains(EGroup.style);

  void _onApp() {
    if (!mounted) return;
    if (app.editRevision != lastRev) {
      lastRev = app.editRevision;
      engine.edited();
      if (_styleVisible) engine.refreshTiles();
    }
    setState(() {});
  }

  // ------------------------------------------------------------ navigation

  /// Next and previous photo of [of] in the editable list (prefetch targets).
  List<Photo> _neighborsOf(String of) {
    final list = app.editable;
    final i = list.indexWhere((p) => p.id == of);
    if (i < 0 || list.length < 2) return const [];
    final n = list[(i + 1) % list.length], pv = list[(i - 1 + list.length) % list.length];
    return n.id == pv.id ? [n] : [n, pv];
  }

  void _go(int delta) {
    final list = app.editable;
    if (list.isEmpty) return;
    var i = list.indexWhere((p) => p.id == id);
    if (i < 0) i = 0;
    final next = list[(i + delta) % list.length < 0 ? 0 : (i + delta + list.length) % list.length];
    final prev = ph;
    Future.microtask(() => app.commitEdit(prev));
    setState(() {
      id = next.id;
      compare = false;
      hold = false;
    });
    engine.attach(next, neighbors: _neighborsOf(next.id));
    if (_styleVisible) engine.refreshTiles();
  }

  void _setGroup(EGroup g) {
    setState(() => group = g);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final ctx = tabKeys[g]!.currentContext;
      final strip = stripKey.currentContext;
      if (ctx == null || strip == null || !tabScroll.hasClients) return;
      final box = ctx.findRenderObject() as RenderBox;
      final stripBox = strip.findRenderObject() as RenderBox;
      final left = box.localToGlobal(Offset.zero, ancestor: stripBox).dx;
      final to = (tabScroll.offset + left + box.size.width / 2 - stripBox.size.width / 2)
          .clamp(0.0, tabScroll.position.maxScrollExtent);
      tabScroll.animateTo(to, duration: const Duration(milliseconds: 200), curve: Curves.ease);
    });
    if (g == EGroup.style) engine.refreshTiles();
  }

  void _jump(EGroup g, bool large) {
    if (large) {
      setState(() => expanded.add(g));
    } else {
      _setGroup(g);
    }
  }

  // ------------------------------------------------------------ derived data

  bool get _showOriginal => compare || hold;

  Analysis _analysis(Photo p, bool original) {
    if (engine.live) {
      final a = original ? engine.originalAnalysis : engine.analysis;
      if (a != null) return a;
    }
    final di = p.demo;
    if (di != null) {
      final spec = demoPhotos[di];
      final l = DemoEdit.of(app, p).look(spec, original);
      return Analysis.demo(spec, brightness: l.b, contrast: l.c);
    }
    return Analysis(List<double>.filled(64, 1 / 64), .1, .5, .9, p.nearest ?? 'mid-mid');
  }

  double _photoRatio(Photo p) => p.demo != null ? demoPhotos[p.demo!].ratio : (p.ratio ?? 1.5);

  double _frameRatio(Photo p, bool original) {
    if (engine.live) {
      final img = original ? (engine.original ?? engine.image) : engine.image;
      if (img != null) return img.width / img.height;
      final ph = engine.placeholder;
      if (ph != null && p.ratio == null) return ph.width / ph.height;
    }
    final pr = _photoRatio(p);
    if (original) return pr;
    final e = p.edit;
    if (e.crop != 'orig') {
      final s = e.crop.split(':');
      final r = double.parse(s[0]) / double.parse(s[1]);
      return e.rot.isOdd || (e.rot ~/ 90).isOdd ? 1 / r : r;
    }
    return (e.rot ~/ 90).isOdd ? 1 / pr : pr;
  }

  // ------------------------------------------------------------ photo frame

  Widget _art(Photo p, bool original, double frameRatio) {
    if (engine.live) {
      final img = original ? (engine.original ?? engine.image) : engine.image;
      if (img != null) return RawImage(image: img, fit: BoxFit.cover);
      // A developed photo already has its processed JPEG; otherwise the camera's embedded one.
      final pl = engine.placeholder;
      if (pl != null && !(p.status == PhotoStatus.done && p.previewPath != null)) {
        return RawImage(image: pl, fit: BoxFit.cover);
      }
      return PhotoArt(p);
    }
    final pr = _photoRatio(p);
    final e = p.edit;
    Widget art = PhotoArt(p, demoEdit: p.demo != null ? DemoEdit.of(app, p) : null, original: original);
    Widget inner = SizedBox(width: pr * 100, height: 100, child: art);
    if (!original) {
      if ((e.rot ~/ 90) % 2 == 1) inner = RotatedBox(quarterTurns: e.rot ~/ 90, child: inner);
      if (e.rot == 180) inner = RotatedBox(quarterTurns: 2, child: inner);
      if (e.flip) inner = Transform.flip(flipX: true, child: inner);
      final s = e.value('straighten');
      if (s != 0) {
        inner = Transform.rotate(
            angle: s * math.pi / 180, child: Transform.scale(scale: 1 + s.abs() / 45 * 0.7, child: inner));
      }
    }
    return FittedBox(fit: BoxFit.cover, clipBehavior: Clip.hardEdge, child: inner);
  }

  Widget _frame(Photo p, bool original, double pad, {bool grid = false}) {
    return Padding(
      padding: EdgeInsets.all(pad),
      child: LayoutBuilder(builder: (context, cons) {
        final r = _frameRatio(p, original);
        var w = cons.maxWidth, h = w / r;
        if (h > cons.maxHeight) {
          h = cons.maxHeight;
          w = h * r;
        }
        if (w <= 0 || h <= 0) return const SizedBox.shrink();
        return Center(
          child: SizedBox(
            width: w,
            height: h,
            child: Stack(fit: StackFit.expand, children: [
              ClipRect(child: _art(p, original, r)),
              if (grid && !original) const IgnorePointer(child: CustomPaint(painter: _ThirdsPainter())),
            ]),
          ),
        );
      }),
    );
  }

  // ------------------------------------------------------------ gestures

  Widget _gestureLayer({required Widget child, bool holdEnabled = true}) {
    return Listener(
      onPointerDown: (_) {
        holdFired = false;
        if (!holdEnabled) return;
        holdTimer?.cancel();
        holdTimer = Timer(const Duration(milliseconds: 220), () {
          if (!mounted) return;
          holdFired = true;
          setState(() => hold = true);
        });
      },
      onPointerUp: (_) => _endHold(),
      onPointerCancel: (_) => _endHold(),
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: () {
          if (holdFired) {
            holdFired = false;
            return;
          }
          setState(() => focus = !focus);
        },
        child: child,
      ),
    );
  }

  void _endHold() {
    holdTimer?.cancel();
    if (hold) setState(() => hold = false);
  }

  // ------------------------------------------------------------ build

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Scaffold(
      backgroundColor: c.bg,
      resizeToAvoidBottomInset: false,
      body: PopScope(
        canPop: !(focus || menuOpen),
        onPopInvokedWithResult: (didPop, _) {
          if (didPop) return;
          setState(() {
            if (menuOpen) {
              menuOpen = false;
            } else {
              focus = false;
            }
          });
        },
        child: LayoutBuilder(builder: (context, cons) {
          final large = cons.maxWidth >= 600;
          return large ? _largeBody(cons) : _phoneBody(cons);
        }),
      ),
    );
  }

  EditorCtx _ctx(bool large) => EditorCtx(
        app: app,
        photo: ph,
        engine: engine,
        large: large,
        analysis: _analysis(ph, _showOriginal),
        ui: ui,
        jump: (g) => _jump(g, large),
        rebuild: _rebuild,
      );

  // ------------------------------------------------------------ phone

  Widget _phoneBody(BoxConstraints cons) {
    final c = context.c;
    final mq = MediaQuery.of(context);
    final H = cons.maxHeight;
    final topH = mq.padding.top + 48;
    final small = H * 0.45, big = H * 0.75;
    final target = focus ? 0.0 : (dragH ?? (largeSheet ? big : small));
    final p = ph;
    final list = app.editable;
    final idx = math.max(0, list.indexWhere((e) => e.id == id));
    return TweenAnimationBuilder<double>(
      tween: Tween(end: target),
      duration: dragging ? Duration.zero : const Duration(milliseconds: 200),
      curve: Curves.ease,
      builder: (context, sheetH, _) {
        final stageTop = focus ? 0.0 : topH;
        final stageH = math.max(60.0, H - stageTop - sheetH);
        final showArrows = stageH >= 150 && !focus;
        return Stack(children: [
          Positioned(
            left: 0,
            right: 0,
            top: stageTop,
            height: stageH,
            child: ColoredBox(
              color: focus ? c.focusStage : c.stage,
              child: Stack(children: [
                Positioned.fill(
                  child: Semantics(
                    label: '照片。按住查看原片，轻点进入或退出全屏',
                    child: _gestureLayer(child: _frame(p, _showOriginal, 12, grid: group == EGroup.crop)),
                  ),
                ),
                if (_showOriginal) Positioned(top: 12, left: 0, right: 0, child: Center(child: _originalPill())),
                if (engine.previewOnly && !_showOriginal)
                  Positioned(top: 12, left: 0, right: 0, child: Center(child: _previewPill())),
                if (!focus)
                  Positioned(
                    right: 16,
                    bottom: 16,
                    child: Semantics(
                      button: true,
                      toggled: compare,
                      label: '对比',
                      excludeSemantics: true,
                      child: GestureDetector(
                        key: const ValueKey('compare-toggle'),
                        onTap: () => setState(() => compare = !compare),
                        child: Container(
                          width: 48,
                          height: 48,
                          decoration: BoxDecoration(color: compare ? c.accentBtn : Fx.overlay45, shape: BoxShape.circle),
                          alignment: Alignment.center,
                          child: const DIcon(DI.compare, size: 24, color: Colors.white),
                        ),
                      ),
                    ),
                  ),
                if (!focus)
                  Positioned(
                    left: 8,
                    bottom: 16,
                    child: Container(
                      decoration: BoxDecoration(color: Fx.overlay45, borderRadius: BorderRadius.circular(24)),
                      child: Row(mainAxisSize: MainAxisSize.min, children: [
                        _undoButton(color: Colors.white),
                        _redoButton(color: Colors.white),
                      ]),
                    ),
                  ),
                if (showArrows) ...[
                  Positioned(left: 8, top: stageH / 2 - 24, child: _circleNav(DI.back, '上一张', () => _go(-1), 'prev-photo')),
                  Positioned(right: 8, top: stageH / 2 - 24, child: _circleNav(DI.forward, '下一张', () => _go(1), 'next-photo')),
                ],
              ]),
            ),
          ),
          if (!focus)
            Positioned(left: 0, right: 0, top: 0, height: topH, child: _phoneTopBar(p, idx + 1, list.length, topH)),
          Positioned(left: 0, right: 0, bottom: 0, height: sheetH, child: sheetH <= 0.5 ? const SizedBox.shrink() : _sheet(H, small, big, mq.padding.bottom)),
          ToastLayer(app: app, bottom: (focus ? mq.padding.bottom : sheetH) + 12),
          if (menuOpen) _menu(topH, 224),
        ]);
      },
    );
  }

  Widget _originalPill() => Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 4),
        decoration: BoxDecoration(color: Fx.overlay55, borderRadius: BorderRadius.circular(14)),
        child: Text('原片', key: const ValueKey('original-pill'), style: T.small(Colors.white, w: FontWeight.w500)),
      );

  Widget _previewPill() => Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 4),
        decoration: BoxDecoration(color: Fx.overlay55, borderRadius: BorderRadius.circular(14)),
        child: Text('预览', key: const ValueKey('preview-pill'), style: T.small(Colors.white, w: FontWeight.w500)),
      );

  Widget _circleNav(DI icon, String label, VoidCallback onTap, String key) => Semantics(
        button: true,
        label: label,
        excludeSemantics: true,
        child: GestureDetector(
          key: ValueKey(key),
          onTap: onTap,
          child: Container(
            width: 48,
            height: 48,
            decoration: const BoxDecoration(color: Fx.overlay55, shape: BoxShape.circle),
            alignment: Alignment.center,
            child: DIcon(icon, size: 24, color: Colors.white),
          ),
        ),
      );

  Widget _doneButton() {
    final c = context.c;
    return Semantics(
      button: true,
      label: '完成',
      excludeSemantics: true,
      child: GestureDetector(
        key: const ValueKey('done'),
        onTap: () => Navigator.of(context).maybePop(),
        child: SizedBox(
          height: 48,
          child: Center(
            child: Container(
              height: 40,
              padding: const EdgeInsets.symmetric(horizontal: 16),
              alignment: Alignment.center,
              decoration: BoxDecoration(color: c.accentBtn, borderRadius: BorderRadius.circular(20)),
              child: Text('完成', style: T.body(Colors.white, w: FontWeight.w500)),
            ),
          ),
        ),
      ),
    );
  }

  Widget _undoButton({Color? color}) => DIconButton(DI.undo, color: color,
      key: const ValueKey('toolbar-undo'), label: '撤销', onPressed: app.canUndo(id) ? () => app.undo(id) : null);
  Widget _redoButton({Color? color}) => DIconButton(DI.redo, color: color,
      key: const ValueKey('toolbar-redo'), label: '重做', onPressed: app.canRedo(id) ? () => app.redo(id) : null);

  /// Undo / redo sit in the bar on large layouts; on the phone they float over the stage so the
  /// title keeps its room.
  List<Widget> _toolButtons({bool undoRedo = true}) => [
        if (undoRedo) ...[_undoButton(), _redoButton()],
        DIconButton(DI.share, key: const ValueKey('toolbar-share'), label: '分享', onPressed: () => openPublish(context, app, [id])),
        DIconButton(DI.more, key: const ValueKey('toolbar-more'), label: '更多', onPressed: () => setState(() => menuOpen = !menuOpen)),
      ];

  Widget _phoneTopBar(Photo p, int n, int total, double topH) {
    final c = context.c;
    return Container(
      color: c.bg,
      padding: EdgeInsets.only(top: MediaQuery.paddingOf(context).top),
      child: Row(children: [
        DIconButton(DI.back, key: const ValueKey('toolbar-back'), label: '返回', onPressed: () => Navigator.of(context).maybePop()),
        Expanded(
          child: Column(mainAxisAlignment: MainAxisAlignment.center, crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(p.title, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.body(c.text, w: FontWeight.w500)),
            Text('$n / $total${engine.previewOnly ? ' · 预览' : ''}', key: const ValueKey('counter'), style: T.mini(c.text2)),
          ]),
        ),
        ..._toolButtons(undoRedo: false),
        _doneButton(),
        const SizedBox(width: 12),
      ]),
    );
  }

  Widget _sheet(double H, double small, double big, double bottomSafe) {
    final c = context.c;
    final x = _ctx(false);
    return Container(
      decoration: BoxDecoration(
        color: c.surface,
        borderRadius: const BorderRadius.vertical(top: Radius.circular(Rd.sheet)),
        boxShadow: [BoxShadow(color: c.sep, offset: const Offset(0, -1)), const BoxShadow(color: Color(0x14000000), blurRadius: 16, offset: Offset(0, -4))],
      ),
      clipBehavior: Clip.antiAlias,
      child: Column(children: [
        Semantics(
          button: true,
          label: largeSheet ? '缩小面板' : '放大面板',
          excludeSemantics: true,
          onTap: () => setState(() => largeSheet = !largeSheet),
          child: GestureDetector(
            key: const ValueKey('sheet-grabber'),
            behavior: HitTestBehavior.opaque,
            onTap: () => setState(() => largeSheet = !largeSheet),
            onVerticalDragStart: (_) => setState(() {
              dragging = true;
              dragH = largeSheet ? big : small;
            }),
            onVerticalDragUpdate: (d) => setState(() {
              dragH = ((dragH ?? small) - d.delta.dy).clamp(small - 40, big);
            }),
            onVerticalDragEnd: (_) => setState(() {
              largeSheet = (dragH ?? small) >= (small + big) / 2;
              dragH = null;
              dragging = false;
            }),
            child: SizedBox(
              height: 24,
              width: double.infinity,
              child: Center(
                child: Container(width: 32, height: 4, decoration: BoxDecoration(color: c.track, borderRadius: BorderRadius.circular(2))),
              ),
            ),
          ),
        ),
        _tabStrip(),
        Expanded(
          child: ListView(
            key: ValueKey('panel-$id-${group.name}'),
            padding: EdgeInsets.fromLTRB(16, 8, 16, bottomSafe + 8),
            children: [buildGroup(context, group, x)],
          ),
        ),
      ]),
    );
  }

  Widget _tabStrip() {
    final c = context.c;
    final ed = ph.edit;
    return SizedBox(
      key: stripKey,
      height: 48,
      child: ShaderMask(
        shaderCallback: (r) => const LinearGradient(
          colors: [Color(0x00FFFFFF), Colors.white, Colors.white, Color(0x00FFFFFF)],
          stops: [0, 0.03, 0.97, 1],
        ).createShader(r),
        blendMode: BlendMode.dstIn,
        child: SingleChildScrollView(
          controller: tabScroll,
          scrollDirection: Axis.horizontal,
          padding: const EdgeInsets.symmetric(horizontal: 12),
          child: Row(children: [
            for (final g in EGroup.values)
              Semantics(
                selected: g == group,
                button: true,
                label: groupLabels[g]! + (ed.groupAdjusted(g) ? '，已调整' : ''),
                excludeSemantics: true,
                child: GestureDetector(
                  key: ValueKey('group-tab-${g.name}'),
                  behavior: HitTestBehavior.opaque,
                  onTap: () => _setGroup(g),
                  child: Container(
                    key: tabKeys[g],
                    constraints: const BoxConstraints(minWidth: 60),
                    height: 48,
                    margin: const EdgeInsets.symmetric(horizontal: 2),
                    padding: const EdgeInsets.symmetric(horizontal: 8),
                    decoration: BoxDecoration(
                        color: g == group ? c.accentSoft : null, borderRadius: BorderRadius.circular(16)),
                    child: Stack(alignment: Alignment.center, children: [
                      Column(mainAxisSize: MainAxisSize.min, mainAxisAlignment: MainAxisAlignment.center, children: [
                        DIcon(groupIcons[g]!, size: 20, color: g == group ? c.accent : c.text2, stroke: 1.9),
                        const SizedBox(height: 3),
                        Text(groupLabels[g]!, style: T.mini(g == group ? c.accent : c.text2, w: g == group ? FontWeight.w500 : FontWeight.w400)),
                      ]),
                      if (ed.groupAdjusted(g))
                        Positioned(
                          right: 0,
                          top: 6,
                          child: Container(
                              key: ValueKey('dot-${g.name}'),
                              width: 6,
                              height: 6,
                              decoration: BoxDecoration(color: c.accent, shape: BoxShape.circle)),
                        ),
                    ]),
                  ),
                ),
              ),
          ]),
        ),
      ),
    );
  }

  // ------------------------------------------------------------ menu / dialogs

  Widget _menu(double top, double width) {
    final c = context.c;
    Widget item(String label, Key k, VoidCallback? onTap, {bool danger = false}) => Semantics(
          button: true,
          enabled: onTap != null,
          label: label,
          excludeSemantics: true,
          child: GestureDetector(
            key: k,
            behavior: HitTestBehavior.opaque,
            onTap: onTap,
            child: Opacity(
              opacity: onTap == null ? 0.4 : 1,
              child: Container(
                height: 48,
                alignment: Alignment.centerLeft,
                padding: const EdgeInsets.symmetric(horizontal: 16),
                child: Text(label, style: T.body(danger ? c.danger : c.text)),
              ),
            ),
          ),
        );
    return Positioned.fill(
      child: Stack(children: [
        Positioned.fill(
          child: GestureDetector(
              key: const ValueKey('menu-scrim'),
              behavior: HitTestBehavior.opaque,
              onTap: () => setState(() => menuOpen = false)),
        ),
        Positioned(
          top: top - 4,
          right: 8,
          width: width,
          child: Container(
            decoration: BoxDecoration(
              color: c.surfaceHi,
              borderRadius: BorderRadius.circular(width > 224 ? 12 : 4),
              border: Border.all(color: c.sep),
              boxShadow: const [BoxShadow(color: Color(0x38000000), blurRadius: 24, offset: Offset(0, 6))],
            ),
            child: Padding(
              padding: const EdgeInsets.symmetric(vertical: 8),
              child: Column(mainAxisSize: MainAxisSize.min, children: [
                item('复制设置', const ValueKey('menu-copy'), () {
                  setState(() => menuOpen = false);
                  app.copySettings(id);
                }),
                item('粘贴设置', const ValueKey('menu-paste'), app.hasClipboard
                    ? () {
                        setState(() => menuOpen = false);
                        app.pasteSettings(id);
                      }
                    : null),
                item('同步设置…', const ValueKey('menu-sync'), () {
                  setState(() => menuOpen = false);
                  showSyncDialog(context, app, ph);
                }),
                item('还原为自动结果', const ValueKey('menu-restore'), () {
                  setState(() => menuOpen = false);
                  _confirmRestore();
                }, danger: true),
              ]),
            ),
          ),
        ),
      ]),
    );
  }

  void _confirmRestore() {
    final c = context.c;
    showDialog<void>(
      context: context,
      barrierColor: c.scrim,
      builder: (dctx) => Dialog(
        backgroundColor: c.surface,
        surfaceTintColor: Colors.transparent,
        shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(Rd.dialog)),
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 320),
          child: Padding(
            padding: const EdgeInsets.all(24),
            child: Semantics(
              explicitChildNodes: true,
              scopesRoute: true,
              namesRoute: true,
              label: '还原为自动结果？',
              child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text('还原为自动结果？', style: T.h2(c.text)),
                const SizedBox(height: 24),
                Row(children: [
                  Expanded(child: DialogButton('取消', key: const ValueKey('restore-cancel'), onPressed: () => Navigator.pop(dctx))),
                  const SizedBox(width: 12),
                  Expanded(
                    child: DialogButton('还原', key: const ValueKey('restore-confirm'), danger: true, onPressed: () {
                      Navigator.pop(dctx);
                      app.restoreAuto(id);
                    }),
                  ),
                ]),
              ]),
            ),
          ),
        ),
      ),
    );
  }

  // ------------------------------------------------------------ large

  Widget _largeBody(BoxConstraints cons) {
    final c = context.c;
    final mq = MediaQuery.of(context);
    final topH = mq.padding.top + 64;
    final p = ph;
    final x = _ctx(true);
    return Stack(children: [
      Column(children: [
        _largeTopBar(p, topH),
        Expanded(
          child: Row(children: [
            Expanded(
              child: Column(children: [
                Expanded(child: _largeStage(p)),
                _filmstrip(p),
              ]),
            ),
            Container(
              width: 336,
              decoration: BoxDecoration(color: c.surface, border: Border(left: BorderSide(color: c.sep))),
              child: Column(children: [
                Padding(
                  padding: const EdgeInsets.fromLTRB(12, 12, 12, 8),
                  child: HistogramCard(analysis: x.analysis, target: p.edit.toneTarget, large: true),
                ),
                Divider(height: 1, color: c.sep),
                Expanded(
                  child: SingleChildScrollView(
                    key: ValueKey('inspector-$id'),
                    padding: EdgeInsets.only(bottom: mq.padding.bottom + 8),
                    child: Column(children: [for (final g in EGroup.values) _inspectorGroup(g, x)]),
                  ),
                ),
              ]),
            ),
          ]),
        ),
      ]),
      ToastLayer(app: app, bottom: 92 + mq.padding.bottom, maxWidth: 480),
      if (menuOpen) _menu(topH, 264),
    ]);
  }

  Widget _largeTopBar(Photo p, double topH) {
    final c = context.c;
    final wide = MediaQuery.sizeOf(context).width >= 760;
    Widget textBtn(Key k, DI icon, String label, {bool on = false, void Function()? onTap, bool hasText = true}) => Semantics(
          button: true,
          toggled: on,
          label: label,
          excludeSemantics: true,
          child: GestureDetector(
            key: k,
            behavior: HitTestBehavior.opaque,
            onTap: onTap,
            child: SizedBox(
              height: 48,
              child: Padding(
                padding: const EdgeInsets.symmetric(horizontal: 8),
                child: Row(mainAxisSize: MainAxisSize.min, children: [
                  DIcon(icon, size: 22, color: c.accent),
                  if (hasText) ...[
                    const SizedBox(width: 6),
                    Text(label, style: T.body(c.accent, w: on ? FontWeight.w500 : FontWeight.w500)),
                  ],
                ]),
              ),
            ),
          ),
        );
    return Container(
      color: c.bg,
      height: topH,
      padding: EdgeInsets.only(top: MediaQuery.paddingOf(context).top),
      child: Row(children: [
        DIconButton(DI.back, key: const ValueKey('toolbar-back'), label: '返回', onPressed: () => Navigator.of(context).maybePop()),
        Expanded(
          child: Column(mainAxisAlignment: MainAxisAlignment.center, crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(p.title, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.body(c.text, w: FontWeight.w500)),
            Text('${p.exif ?? ''}${p.processing || engine.previewOnly ? ' · 预览' : ''}',
                maxLines: 1, overflow: TextOverflow.ellipsis, style: T.cap(c.text2)),
          ]),
        ),
        textBtn(const ValueKey('sidebyside-toggle'), DI.sideBySide, '并排对比',
            on: sideBySide, hasText: wide, onTap: () => setState(() => sideBySide = !sideBySide)),
        Listener(
          onPointerDown: (_) => setState(() => hold = true),
          onPointerUp: (_) => setState(() => hold = false),
          onPointerCancel: (_) => setState(() => hold = false),
          child: textBtn(const ValueKey('hold-original'), DI.eye, '原片', on: hold, hasText: wide),
        ),
        ..._toolButtons(),
        _doneButton(),
        const SizedBox(width: 8),
      ]),
    );
  }

  Widget _largeStage(Photo p) {
    final c = context.c;
    Widget labelled(Widget f, String label) => Column(children: [
          Expanded(child: f),
          Padding(
            padding: const EdgeInsets.only(bottom: 8),
            child: Text(label, style: T.small(c.text2, w: FontWeight.w500)),
          ),
        ]);
    final cropGrid = expanded.contains(EGroup.crop);
    return ColoredBox(
      color: c.stage,
      child: Stack(children: [
        Positioned.fill(
          child: sideBySide
              ? Row(children: [
                  Expanded(child: labelled(_frame(p, true, 16), '原片')),
                  const SizedBox(width: 12),
                  Expanded(child: labelled(_frame(p, false, 16), '成片')),
                ])
              : Semantics(
                  label: '照片。按住查看原片',
                  child: _frame(p, hold, 16, grid: cropGrid),
                ),
        ),
        if (hold && !sideBySide) Positioned(top: 12, left: 0, right: 0, child: Center(child: _originalPill())),
      ]),
    );
  }

  final ScrollController filmScroll = ScrollController();

  Widget _filmstrip(Photo p) {
    final c = context.c;
    final list = app.editable;
    final idx = list.indexWhere((e) => e.id == id);
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!filmScroll.hasClients || idx < 0) return;
      final target = (idx * 62.0 - (filmScroll.position.viewportDimension / 2 - 28)).clamp(0.0, filmScroll.position.maxScrollExtent);
      filmScroll.animateTo(target, duration: const Duration(milliseconds: 200), curve: Curves.ease);
    });
    return Container(
      height: 80,
      color: c.bg,
      child: Row(children: [
        _arrow(DI.back, '上一张', () => _go(-1), 'film-prev'),
        Expanded(
          child: ListView.separated(
            controller: filmScroll,
            scrollDirection: Axis.horizontal,
            padding: const EdgeInsets.symmetric(vertical: 10),
            itemCount: list.length,
            separatorBuilder: (_, _) => const SizedBox(width: 6),
            itemBuilder: (context, i) {
              final e = list[i];
              final cur = e.id == id;
              return Semantics(
                button: true,
                selected: cur,
                label: e.title,
                excludeSemantics: true,
                child: GestureDetector(
                  key: ValueKey('film-${e.id}'),
                  onTap: () {
                    if (cur) return;
                    Future.microtask(() => app.commitEdit(ph));
                    setState(() {
                      id = e.id;
                      hold = false;
                    });
                    engine.attach(e, neighbors: _neighborsOf(e.id));
                    if (_styleVisible) engine.refreshTiles();
                  },
                  child: Container(
                    width: 56,
                    height: 56,
                    padding: EdgeInsets.all(cur ? 2 : 0),
                    decoration: cur
                        ? BoxDecoration(borderRadius: BorderRadius.circular(10), border: Border.all(color: c.accentBtn, width: 2))
                        : null,
                    child: ClipRRect(borderRadius: BorderRadius.circular(cur ? 6 : 8), child: PhotoArt(e, cacheWidth: 120)),
                  ),
                ),
              );
            },
          ),
        ),
        _arrow(DI.forward, '下一张', () => _go(1), 'film-next'),
      ]),
    );
  }

  Widget _arrow(DI icon, String label, VoidCallback onTap, String key) {
    final c = context.c;
    return Semantics(
      button: true,
      label: label,
      excludeSemantics: true,
      child: GestureDetector(
        key: ValueKey(key),
        behavior: HitTestBehavior.opaque,
        onTap: onTap,
        child: SizedBox(width: 44, height: 80, child: Center(child: DIcon(icon, size: 22, color: c.accent))),
      ),
    );
  }

  Widget _inspectorGroup(EGroup g, EditorCtx x) {
    final c = context.c;
    final open = expanded.contains(g);
    final adj = ph.edit.groupAdjusted(g);
    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      Semantics(
        button: true,
        expanded: open,
        label: groupLabels[g]! + (adj ? '，已调整' : ''),
        excludeSemantics: true,
        child: GestureDetector(
          key: ValueKey('group-header-${g.name}'),
          behavior: HitTestBehavior.opaque,
          onTap: () => setState(() {
            if (open) {
              expanded.remove(g);
            } else {
              expanded.add(g);
              if (g == EGroup.style) engine.refreshTiles();
            }
          }),
          child: Container(
            height: 48,
            padding: const EdgeInsets.symmetric(horizontal: 16),
            child: Row(children: [
              Text(groupLabels[g]!, style: T.body(c.text, w: FontWeight.w500)),
              if (adj) ...[
                const SizedBox(width: 8),
                Container(
                    key: ValueKey('dot-${g.name}'),
                    width: 6,
                    height: 6,
                    decoration: BoxDecoration(color: c.accent, shape: BoxShape.circle)),
              ],
              const Spacer(),
              DIcon(open ? DI.up : DI.down, size: 18, color: c.text2),
            ]),
          ),
        ),
      ),
      if (open)
        Padding(
          key: ValueKey('group-body-${g.name}'),
          padding: const EdgeInsets.fromLTRB(16, 0, 16, 16),
          child: buildGroup(context, g, x),
        ),
      Divider(height: 1, color: c.sep),
    ]);
  }
}

class _ThirdsPainter extends CustomPainter {
  const _ThirdsPainter();
  @override
  void paint(Canvas canvas, Size s) {
    final p = Paint()
      ..color = const Color(0xCCFFFFFF)
      ..strokeWidth = 1;
    for (var i = 1; i < 3; i++) {
      canvas.drawLine(Offset(s.width * i / 3, 0), Offset(s.width * i / 3, s.height), p);
      canvas.drawLine(Offset(0, s.height * i / 3), Offset(s.width, s.height * i / 3), p);
    }
    canvas.drawRect(
        Offset.zero & s,
        Paint()
          ..color = Colors.white
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2);
  }

  @override
  bool shouldRepaint(_ThirdsPainter o) => false;
}
