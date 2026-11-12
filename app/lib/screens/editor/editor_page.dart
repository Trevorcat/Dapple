// Viewer / editor, interaction v2 (requirements 4.15). The photo fills the screen; a floating top
// bar, a category dock and up to two floating category windows sit on top. One pointer listener
// ([EditorGestureSurface]) feeds a gesture state machine; the page itself never rebuilds per
// pointer move: every moving part listens to its own small notifier.
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/semantics.dart';

import '../../editor/handedness.dart';
import '../../editor/haptics.dart';
import '../../editor/springs.dart';
import '../../editor/window_manager.dart';
import '../../model/catalog.dart';
import '../../model/params.dart';
import '../../models.dart';
import '../../services/editor_engine.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/photo_art.dart';
import '../publish/publish_page.dart';
import 'editor_controller.dart';
import 'editor_overlays.dart';
import 'float_window.dart';
import 'gesture_surface.dart';
import 'sync_dialog.dart';
import 'zoom_view.dart';
import '../../services/perf_probe.dart';

/// Opens the viewer / editor on [photoId] (full-screen route; the global navigation is hidden).
void openEditor(BuildContext context, AppState app, String photoId) {
  Navigator.of(context).push(MaterialPageRoute<void>(builder: (_) => EditorPage(app: app, photoId: photoId)));
}

class EditorPage extends StatefulWidget {
  const EditorPage({super.key, required this.app, required this.photoId, this.resamplePointers = true});
  final AppState app;
  final String photoId;

  /// Merge pointer events per frame (requirements 4.15.16). Tests switch it off.
  final bool resamplePointers;
  @override
  State<EditorPage> createState() => _EditorPageState();
}

class _EditorPageState extends State<EditorPage> with TickerProviderStateMixin {
  AppState get app => widget.app;
  late String id = widget.photoId;
  late final EditorEngine engine = EditorEngine(app);
  final ZoomController zoom = ZoomController();
  late final WindowManager wm;
  late final EditorController ctl;
  late final Haptics haptics = Haptics(enabled: app.settings.haptics);
  late final SpringValue _shift = SpringValue(this);
  bool menuOpen = false;
  int lastRev = 0;
  Timer? _saveLayout;
  bool _resampling = false;
  LayoutClass? _lc;
  bool _styleOpen = false;

  Photo get ph => app.photoById(id)!;

  @override
  void initState() {
    super.initState();
    final s = app.settings;
    final side = s.handedness == 1 ? Hand.left : (s.handedness == 2 ? Hand.right : (s.handSide == 0 ? Hand.left : Hand.right));
    wm = WindowManager(side: side)..restore(s.editorLayout);
    ctl = EditorController(
      app: app,
      photoId: () => id,
      engine: engine,
      zoom: zoom,
      wm: wm,
      haptics: haptics,
      hand: HandednessInferrer(screenWidth: 412, initial: side),
      analysis: () => _analysis(ph, false),
      onSwipe: _go,
      onToggleImmersive: () => ctl.immersive.value = !ctl.immersive.value,
    );
    lastRev = app.editRevision;
    app.addListener(_onApp);
    app.editTick.addListener(_onApp);
    wm.addListener(_onWm);
    zoom.addListener(_onZoom);
    engine.attach(ph, neighbors: _neighborsOf(id));
    if (widget.resamplePointers) {
      GestureBinding.instance.resamplingEnabled = true;
      _resampling = true;
    }
  }

  @override
  void dispose() {
    if (_resampling) GestureBinding.instance.resamplingEnabled = false;
    app.removeListener(_onApp);
    app.editTick.removeListener(_onApp);
    wm.removeListener(_onWm);
    zoom.removeListener(_onZoom);
    _saveLayout?.cancel();
    _persistLayout(deferred: true);
    final p = app.photoById(id);
    if (p != null) Future.microtask(() => app.commitEdit(p));
    zoom.dispose();
    _shift.dispose();
    ctl.dispose();
    wm.dispose();
    engine.dispose();
    super.dispose();
  }

  void _persistLayout({bool deferred = false}) {
    final json = wm.toJsonString();
    if (json == app.settings.editorLayout) return;
    void write() {
      app.settings.editorLayout = json;
      app.updateSettings((_) {});
    }

    // From dispose the write must not notify listeners while the tree is being torn down.
    deferred ? Future.microtask(write) : write();
  }

  void _onApp() {
    if (!mounted) return;
    haptics.enabled = app.settings.haptics;
    if (app.editRevision != lastRev) {
      lastRev = app.editRevision;
      engine.edited();
      // undo / redo / paste refresh the windows; a drag step is announced by the controller itself (first step only)
      if (!ctl.dragging.value) ctl.edits.value++;
    }
  }

  void _onWm() {
    _saveLayout?.cancel();
    _saveLayout = Timer(const Duration(seconds: 1), _persistLayout);
    final styleNow = wm.windowOf(EGroup.style) != null;
    if (styleNow && !_styleOpen) engine.refreshTiles();
    _styleOpen = styleNow;
    if (!wm.interacting) _syncShift();
  }

  void _onZoom() {
    if (!mounted) return;
    if (zoom.zoomed && _shift.target != 0) _shift.animateTo(0, spring: SpringTier.large.displacement);
    if (!zoom.zoomed) _syncShift();
  }

  void _onViewport(Rect visible, double scale, Size frame) {
    final t0 = PerfProbe.begin();
    _onViewport2(visible, scale, frame);
    PerfProbe.end('viewport', t0);
  }

  void _onViewport2(Rect visible, double scale, Size frame) {
    final img = engine.image;
    if (img == null || !engine.live) return;
    final dpr = MediaQuery.devicePixelRatioOf(context);
    engine.setDisplayLongEdge((math.max(frame.width, frame.height) * dpr).round());
    engine.setViewport(visible, scale * frame.width * dpr / img.width);
  }

  // ------------------------------------------------------------ photo avoidance (phone portrait only)

  WindowArea? _area;

  /// While a window is open on a compact phone, the photo moves up by the covered height (capped by
  /// the grey above it). Only in the open / close transition, never during an interaction.
  void _syncShift() {
    final lc = _lc, area = _area;
    if (lc == null || area == null) return;
    final frame = zoom.frame, vp = zoom.viewport;
    var target = 0.0;
    if (lc.width == WidthClass.compact && !lc.railDock && !zoom.zoomed && frame.height > 0) {
      final photoTop = (vp.height - frame.height) / 2, photoBottom = photoTop + frame.height;
      // One stable region for every window: the tallest card any category could have, so switching
      // between windows of different heights leaves the photo still.
      var occ = 0.0;
      final open = wm.windows.where((w) => !w.collapsed).toList();
      if (open.isNotEmpty) occ = photoBottom - tallestCardTop(ctl, open.first.anchor, lc, area);
      final gray = math.max(0.0, photoTop - area.bounds.top);
      target = -math.min(math.max(0.0, occ), gray);
    }
    if ((target - _shift.target).abs() > 0.5) _shift.animateTo(target, spring: SpringTier.large.displacement);
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
    final next = list[(i + delta + list.length) % list.length];
    _switchTo(next, announce: '${(i + delta + list.length) % list.length + 1} / ${list.length}');
  }

  void _switchTo(Photo next, {String? announce}) {
    final prev = ph;
    Future.microtask(() => app.commitEdit(prev));
    zoom.reset();
    ctl.compare.value = false;
    setState(() => id = next.id);
    engine.attach(next, neighbors: _neighborsOf(next.id));
    if (wm.windowOf(EGroup.style) != null) engine.refreshTiles();
    ctl.edits.value++;
    if (announce != null) ctl.showText(announce);
  }

  // ------------------------------------------------------------ derived data

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
      final ga = original ? null : engine.gpuAspect;
      if (ga != null) return ga;
      final img = original ? (engine.original ?? engine.image) : engine.image;
      if (img != null) return img.width / img.height;
      final pl = engine.placeholder;
      if (pl != null && p.ratio == null) return pl.width / pl.height;
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

  Widget _art(Photo p, bool original) {
    if (engine.live) {
      final tex = original ? null : engine.gpuTextureId; // zero-copy texture (stage d) while it is live
      if (tex != null) return Texture(textureId: tex, filterQuality: FilterQuality.medium);
      final img = original ? (engine.original ?? engine.image) : engine.image;
      if (img != null) return RawImage(image: img, fit: BoxFit.cover);
      final pl = engine.placeholder;
      if (pl != null && !(p.status == PhotoStatus.done && p.previewPath != null)) {
        return RawImage(image: pl, fit: BoxFit.cover);
      }
      return PhotoArt(p);
    }
    final pr = _photoRatio(p);
    final e = p.edit;
    final Widget art = PhotoArt(p, demoEdit: p.demo != null ? DemoEdit.of(app, p) : null, original: original);
    Widget inner = SizedBox(width: pr * 100, height: 100, child: art);
    if (!original) {
      if ((e.rot ~/ 90) % 2 == 1) inner = RotatedBox(quarterTurns: e.rot ~/ 90, child: inner);
      if (e.rot == 180) inner = RotatedBox(quarterTurns: 2, child: inner);
      if (e.flip) inner = Transform.flip(flipX: true, child: inner);
      final s = e.value('straighten');
      if (s != 0) {
        inner = Transform.rotate(angle: s * math.pi / 180, child: Transform.scale(scale: 1 + s.abs() / 45 * 0.7, child: inner));
      }
    }
    return FittedBox(fit: BoxFit.cover, clipBehavior: Clip.hardEdge, child: inner);
  }

  // ------------------------------------------------------------ build

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Scaffold(
      backgroundColor: c.stage,
      resizeToAvoidBottomInset: false,
      body: ListenableBuilder(
        listenable: Listenable.merge([wm, zoom, ctl.immersive, ctl.editingRow]),
        builder: (context, child) => PopScope(
          canPop: !menuOpen && ctl.canPop,
          onPopInvokedWithResult: (didPop, _) {
            if (didPop) return;
            if (menuOpen) {
              setState(() => menuOpen = false);
            } else {
              ctl.handleBack();
            }
          },
          child: child!,
        ),
        child: LayoutBuilder(builder: (context, cons) => _body(context, cons)),
      ),
    );
  }

  Widget _body(BuildContext context, BoxConstraints cons) {
    final mq = MediaQuery.of(context);
    final size = Size(cons.maxWidth, cons.maxHeight);
    final textScale = mq.textScaler.scale(15) / 15;
    final lc = LayoutClass.of(size, textScale: textScale);
    if (_lc == null || _lc!.key != lc.key || _lc!.maxWindows != lc.maxWindows || _lc!.textScale != lc.textScale) {
      _lc = lc;
      ctl.hand.screenWidth = size.width;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted) wm.setLayout(lc);
      });
    }
    _lc = lc;
    ctl.exploring = mq.accessibleNavigation;
    final reduce = mq.disableAnimations;
    final pad = mq.padding;
    final gi = mq.systemGestureInsets;
    final marginL = math.max(12.0, gi.left), marginR = math.max(12.0, gi.right);
    final topBarTop = pad.top + 8;
    final topBarBottom = topBarTop + 48;
    final dockBottom = math.max(pad.bottom, 16.0) + 8;
    final dockTop = size.height - dockBottom - 56;
    final rail = lc.railDock;
    final handSide = wm.side;
    final railW = rail ? 56.0 + 12 : 0.0;
    final keyboard = mq.viewInsets.bottom;
    final areaBottom = rail ? size.height - dockBottom : math.min(dockTop - 12, size.height - keyboard - 8);
    final area = WindowArea(
      bounds: Rect.fromLTRB(0, topBarBottom + 8, size.width, areaBottom),
      marginLeft: marginL + (rail && handSide == Hand.left ? railW : 0),
      marginRight: marginR + (rail && handSide == Hand.right ? railW : 0),
      availHeight: size.height - pad.vertical,
    );
    _area = area;
    ctl.availableHeight = size.height - pad.vertical;
    ctl.readoutArea = Rect.fromLTWH(size.width / 2 - 130, topBarBottom + 8, 260, 96);
    final photoPad = EdgeInsets.symmetric(vertical: math.max(pad.top, pad.bottom), horizontal: math.max(pad.left, pad.right));
    final c = context.c;

    return Stack(children: [
      // 0 photo + the single gesture surface
      Positioned.fill(
        child: EditorGestureSurface(
          sink: ctl,
          child: ColoredBox(
            color: c.stage,
            child: Semantics(
              label: '照片。双指缩放，双击放大，按住查看原片',
              customSemanticsActions: {
                const CustomSemanticsAction(label: '显示原片 / 恢复成片'): () => ctl.longPress(!ctl.compare.value),
                const CustomSemanticsAction(label: '放大 / 适合'): () => ctl.doubleTap(size.width / 2, size.height / 2),
              },
              child: _photoStage(photoPad),
            ),
          ),
        ),
      ),
      // browse-mode filmstrip on large screens
      if (lc.width != WidthClass.compact && !rail) _filmstrip(bottom: dockBottom + 56 + 12, left: marginL, right: marginR),
      // 1 histogram window (>= 600 dp)
      if (lc.histWindow) HistogramWindow(controller: ctl, area: area, reduceMotion: reduce),
      // 2 category windows
      Positioned.fill(
        child: FloatingWindowsLayer(controller: ctl, layout: lc, area: area, reduceMotion: reduce, photo: ph, nearest: _analysis(ph, false).nearest),
      ),
      // 3 readout
      ListenableBuilder(
        listenable: ctl.readout,
        builder: (context, _) {
          final low = ctl.readout.low;
          return Positioned(
            left: 0,
            right: 0,
            top: low ? null : topBarBottom + 8,
            bottom: low ? size.height - dockTop + 12 : null,
            child: Center(child: ReadoutOverlay(controller: ctl, compactHist: !lc.histWindow)),
          );
        },
      ),
      ListenableBuilder(
        listenable: engine,
        builder: (context, _) => engine.previewOnly
            ? Positioned(top: topBarBottom + 8, left: 0, right: 0, child: Center(child: _previewPill()))
            : const SizedBox.shrink(),
      ),
      // 4 top bar and dock (hidden in immersive mode)
      ValueListenableBuilder<bool>(
        valueListenable: ctl.immersive,
        builder: (context, imm, _) => IgnorePointer(
          ignoring: imm,
          child: AnimatedOpacity(
            opacity: imm ? 0 : 1,
            duration: const Duration(milliseconds: 150),
            child: Stack(children: [
              Positioned(
                top: topBarTop,
                left: marginL,
                right: marginR,
                child: EditorTopBar(app: app, photoId: () => id, onBack: () => Navigator.of(context).maybePop()),
              ),
              if (rail)
                Positioned(
                  top: topBarBottom + 8,
                  bottom: dockBottom,
                  left: handSide == Hand.left ? marginL : null,
                  right: handSide == Hand.right ? marginR : null,
                  width: 56,
                  child: Align(alignment: Alignment.center, child: CategoryDock(controller: ctl, vertical: true, groups: lc.width == WidthClass.compact ? phoneDockOrder : dockOrder, onMore: _toggleMenu)),
                )
              else
                Positioned(
                  left: marginL,
                  right: marginR,
                  bottom: dockBottom,
                  height: 56,
                  child: Align(alignment: Alignment.center, child: CategoryDock(controller: ctl, groups: lc.width == WidthClass.compact ? phoneDockOrder : dockOrder, onMore: _toggleMenu)),
                ),
            ]),
          ),
        ),
      ),
      ToastLayer(app: app, bottom: (rail ? dockBottom : size.height - dockTop) + 12, maxWidth: 480),
      if (menuOpen) _menu(size, dockTop, rail, lc.width == WidthClass.compact ? phoneMoreOrder : const []),
    ]);
  }

  Widget _previewPill() => Container(
        padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 4),
        decoration: BoxDecoration(color: Fx.overlay55, borderRadius: BorderRadius.circular(14)),
        child: Text('预览', key: const ValueKey('preview-pill'), style: T.small(Colors.white, w: FontWeight.w500)),
      );

  Widget _photoStage(EdgeInsets padding) {
    return ListenableBuilder(
      listenable: Listenable.merge([engine, ctl.compare, ctl.edits, wm]),
      builder: (context, _) {
        final p = ph;
        PerfProbe.count('photoStage');
        final original = ctl.compare.value;
        final focusGroup = wm.focused?.group;
        final showGrid = focusGroup == EGroup.crop && !original;
        final fine = showGrid && ctl.activeRow?.id == 'straighten';
        return ZoomView(
          key: const ValueKey('zoom-stage'),
          controller: zoom,
          aspect: _frameRatio(p, original),
          padding: padding,
          shiftY: _shift,
          onViewport: _onViewport,
          builder: (context, frame) {
            final showDetail = !original && engine.live && engine.detail != null && engine.detailRect != null;
            final r = engine.detailRect;
            return Stack(fit: StackFit.expand, children: [
              ClipRect(child: _art(p, original)),
              if (showDetail)
                Positioned(
                  left: r!.left * frame.width,
                  top: r.top * frame.height,
                  width: r.width * frame.width,
                  height: r.height * frame.height,
                  child: RawImage(image: engine.detail, fit: BoxFit.fill, filterQuality: FilterQuality.medium),
                ),
              if (showGrid) IgnorePointer(child: CustomPaint(painter: _ThirdsPainter(fine ? 6 : 3))),
            ]);
          },
        );
      },
    );
  }

  // ------------------------------------------------------------ filmstrip (large, browse mode)

  final ScrollController _filmScroll = ScrollController();

  Widget _filmstrip({required double bottom, required double left, required double right}) {
    return ListenableBuilder(
      listenable: wm,
      builder: (context, _) {
        if (wm.adjustMode) return const SizedBox.shrink();
        final c = context.c;
        final list = app.editable;
        final idx = list.indexWhere((e) => e.id == id);
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (!_filmScroll.hasClients || idx < 0) return;
          final target = (idx * 62.0 - (_filmScroll.position.viewportDimension / 2 - 28)).clamp(0.0, _filmScroll.position.maxScrollExtent);
          _filmScroll.animateTo(target, duration: const Duration(milliseconds: 200), curve: Curves.ease);
        });
        return Positioned(
          left: left,
          right: right,
          bottom: bottom,
          height: 64,
          child: Center(
            child: ConstrainedBox(
              constraints: const BoxConstraints(maxWidth: 560),
              child: GlassCapsule(
                radius: 16,
                child: ListView.separated(
                  key: const ValueKey('filmstrip'),
                  controller: _filmScroll,
                  scrollDirection: Axis.horizontal,
                  padding: const EdgeInsets.all(4),
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
                          if (!cur) _switchTo(e);
                        },
                        child: Container(
                          width: 56,
                          height: 56,
                          padding: EdgeInsets.all(cur ? 2 : 0),
                          decoration: cur ? BoxDecoration(borderRadius: BorderRadius.circular(10), border: Border.all(color: c.accentBtn, width: 2)) : null,
                          child: ClipRRect(borderRadius: BorderRadius.circular(cur ? 6 : 8), child: PhotoArt(e, cacheWidth: 120)),
                        ),
                      ),
                    );
                  },
                ),
              ),
            ),
          ),
        );
      },
    );
  }

  // ------------------------------------------------------------ more menu / dialogs

  void _toggleMenu() => setState(() => menuOpen = !menuOpen);

  Widget _menu(Size size, double dockTop, bool rail, List<EGroup> moreGroups) {
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
    void close() => setState(() => menuOpen = false);
    return Positioned.fill(
      child: Stack(children: [
        Positioned.fill(child: GestureDetector(key: const ValueKey('menu-scrim'), behavior: HitTestBehavior.opaque, onTap: close)),
        Positioned(
          bottom: size.height - dockTop + 8,
          right: 12,
          width: 240,
          child: Container(
            decoration: BoxDecoration(
              color: c.surfaceHi,
              borderRadius: BorderRadius.circular(12),
              border: Border.all(color: c.sep),
              boxShadow: const [BoxShadow(color: Color(0x38000000), blurRadius: 24, offset: Offset(0, 6))],
            ),
            child: Padding(
              padding: const EdgeInsets.symmetric(vertical: 8),
              child: Column(mainAxisSize: MainAxisSize.min, children: [
                // categories that do not fit the phone dock
                for (final g in moreGroups)
                  item(groupLabels[g]!, ValueKey('menu-${g.name}'), () {
                    close();
                    ctl.openCategory(g);
                  }),
                if (moreGroups.isNotEmpty) Divider(height: 9, thickness: 1, color: c.sep),
                item('发布…', const ValueKey('menu-publish'), () {
                  close();
                  openPublish(context, app, [id]);
                }),
                item('复制设置', const ValueKey('menu-copy'), () {
                  close();
                  app.copySettings(id);
                }),
                item('粘贴设置', const ValueKey('menu-paste'), app.hasClipboard
                    ? () {
                        close();
                        app.pasteSettings(id);
                      }
                    : null),
                item('同步设置…', const ValueKey('menu-sync'), () {
                  close();
                  showSyncDialog(context, app, ph);
                }),
                item('还原为自动结果', const ValueKey('menu-restore'), () {
                  close();
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
}

/// Thirds grid over the photo (6x6 while straightening, as a level reference) and a white frame.
class _ThirdsPainter extends CustomPainter {
  const _ThirdsPainter(this.n);
  final int n;
  @override
  void paint(Canvas canvas, Size s) {
    final p = Paint()
      ..color = const Color(0xCCFFFFFF)
      ..strokeWidth = 1;
    for (var i = 1; i < n; i++) {
      canvas.drawLine(Offset(s.width * i / n, 0), Offset(s.width * i / n, s.height), p);
      canvas.drawLine(Offset(0, s.height * i / n), Offset(s.width, s.height * i / n), p);
    }
    canvas.drawRect(
        Offset.zero & s,
        Paint()
          ..color = Colors.white
          ..style = PaintingStyle.stroke
          ..strokeWidth = 2);
  }

  @override
  bool shouldRepaint(_ThirdsPainter o) => o.n != n;
}
