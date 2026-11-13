// Viewer / editor, interaction v2 (requirements 4.15). The photo fills the screen; a floating top
// bar, a category dock and up to two floating category windows sit on top. One pointer listener
// ([EditorGestureSurface]) feeds a gesture state machine; the page itself never rebuilds per
// pointer move: every moving part listens to its own small notifier.
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/semantics.dart';

import '../../services/base_swatches.dart' show SwatchSource;
import '../../editor/handedness.dart';
import '../../editor/haptics.dart';
import '../../editor/springs.dart';
import '../../editor/panel_layout.dart';
import '../../editor/window_manager.dart';
import '../../model/catalog.dart';
import '../../model/pad_model.dart';
import '../../model/params.dart';
import '../../models.dart';
import '../../services/editor_engine.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/photo_art.dart';
import '../publish/publish_page.dart';
import 'pad_dock.dart';
import 'pad_panel.dart';
import 'editor_controller.dart';
import 'editor_overlays.dart';
import 'gesture_surface.dart';
import 'sync_dialog.dart';
import 'zoom_view.dart';
import '../../services/perf_probe.dart';

/// Opens the viewer / editor on [photoId] (full-screen route; the global navigation is hidden).
void openEditor(BuildContext context, AppState app, String photoId, {EGroup? initialGroup, String? initialRow, bool initialCollapsed = false, String? initialHold}) {
  OpenTiming.tap();
  Navigator.of(context).push(MaterialPageRoute<void>(
      builder: (_) => EditorPage(app: app, photoId: photoId, initialGroup: initialGroup, initialRow: initialRow, initialCollapsed: initialCollapsed, initialHold: initialHold)));
}

class EditorPage extends StatefulWidget {
  const EditorPage({super.key, required this.app, required this.photoId, this.resamplePointers = true, this.filmPadFetcher, this.swatchSource, this.initialGroup, this.initialRow, this.initialCollapsed = false, this.initialHold});
  final AppState app;
  final String photoId;

  /// Merge pointer events per frame (requirements 4.15.16). Tests switch it off.
  final bool resamplePointers;

  /// Opens with this category (and parameter row, folded or not) selected: debug deep link and tests.
  final EGroup? initialGroup;
  final String? initialRow;
  final bool initialCollapsed;

  /// Debug link / tests: shows the hold popup of this pad category (name) without a finger.
  final String? initialHold;

  /// Test hook: replaces the engine query for the film pads' effective ranges.
  final Future<String?> Function(Map<String, dynamic> params)? filmPadFetcher;

  /// Test hook: where the film map's base swatches come from (default: the engine).
  final SwatchSource Function(String photoId)? swatchSource;
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
  /// Progress of the layout transition (photo region, panel and dock move from the previous geometry to the target).
  late final SpringValue _t = SpringValue(this, initial: 1);
  double _lastAspect = 0;
  bool menuOpen = false;
  int lastRev = 0;
  bool _resampling = false;

  Photo get ph => app.photoById(id)!;

  @override
  void initState() {
    super.initState();
    final s = app.settings;
    final side = s.handedness == 1 ? Hand.left : (s.handedness == 2 ? Hand.right : (s.handSide == 0 ? Hand.left : Hand.right));
    wm = WindowManager(side: side);
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
      filmPadFetcher: widget.filmPadFetcher,
      swatchSource: widget.swatchSource,
    );
    lastRev = app.editRevision;
    app.addListener(_onApp);
    app.editTick.addListener(_onApp);
    wm.addListener(_onWm);
    ctl.edits.addListener(_onEdits);
    ctl.dragging.addListener(_onDragging);
    _t.addListener(() => _panelRect.value = _geoNow().panel);
    engine.attach(ph, neighbors: _neighborsOf(id));
    WidgetsBinding.instance.addPostFrameCallback((_) => OpenTiming.mark('editor_first_frame'));
    if (widget.initialGroup != null || widget.initialHold != null) WidgetsBinding.instance.addPostFrameCallback((_) => _applyInitial());
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
    ctl.edits.removeListener(_onEdits);
    ctl.dragging.removeListener(_onDragging);
    final p = app.photoById(id);
    if (p != null) Future.microtask(() => app.commitEdit(p));
    zoom.dispose();
    _t.dispose();
    _geoRev.dispose();
    _panelRect.dispose();
    ctl.dispose();
    wm.dispose();
    engine.dispose();
    super.dispose();
  }

  void _applyInitial() {
    final g = widget.initialGroup;
    final hold = PadCat.values.where((c) => c.name == widget.initialHold).firstOrNull;
    if (mounted && hold != null) ctl.hold.value = HoldPopup(hold, _env == null ? 200 : _env!.size.width / 2);
    if (!mounted || g == null) return;
    ctl.showCategory(g);
    // initialRow names the layer: items (分项), map (胶片地图), curve / hsl / grade / paper (the complex editors)
    switch (widget.initialRow) {
      case 'items':
        ctl.setItemised(true);
      case 'map':
        ctl.setFilmMap(true);
      case 'curve':
        ctl.openExtra(PanelExtra.curve);
      case 'hsl':
        ctl.openExtra(PanelExtra.hsl);
      case 'grade':
        ctl.openExtra(PanelExtra.grade);
      case 'paper':
        ctl.openExtra(PanelExtra.paper);
      case 'tone':
        ctl.openExtra(PanelExtra.tone);
    }
    if (widget.initialCollapsed) wm.setCollapsed(g, true);
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

  /// A new selection (or a changed crop) can change the controls the panel needs.
  void _onEdits() {
    if (mounted) WidgetsBinding.instance.addPostFrameCallback((_) => _retarget());
  }

  void _onDragging() {
    if (!ctl.dragging.value && mounted) WidgetsBinding.instance.addPostFrameCallback((_) => _retarget());
  }

  void _onWm() {
    if (mounted) WidgetsBinding.instance.addPostFrameCallback((_) => _retarget());
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

  // ------------------------------------------------------------ adaptive panel layout (docs/design/editor-panel.md section 6)

  /// The current target geometry of photo region, panel and dock, and the one animated from.
  _PanelGeo? _to;
  _PanelGeo _from = _PanelGeo.empty;
  _Env? _env;
  PanelPlace _place = PanelPlace.bottom;
  String _placeKey = '';

  final ValueNotifier<int> _geoRev = ValueNotifier(0);
  final ValueNotifier<Rect> _panelRect = ValueNotifier(Rect.zero);

  _PanelGeo _geoNow() => _PanelGeo.lerp(_from, _to ?? _PanelGeo.empty, _t.value);

  /// Solves the layout for the open category (or the browse layout without one). The placement (bottom / side) is
  /// chosen per category from its tallest control, so moving between its parameters never flips it; it is
  /// re-evaluated when the photo, the screen size, the category or the hand side changes, never during a drag.
  _PanelGeo _targetGeo() {
    final e = _env!;
    final w = wm.focused;
    final dockH = 56.0;
    final kb = e.keyboard > 0;
    final size = kb ? Size(e.size.width, e.size.height - e.keyboard) : e.size;
    final dockBottom = kb ? 8.0 : e.dockBottom;
    final dockTop = size.height - dockBottom - dockH;
    final full = Rect.fromLTWH(0, 0, e.size.width, e.size.height);
    if (w == null) {
      return _PanelGeo(
        region: full,
        panel: Rect.fromLTWH(e.marginL, dockTop - 8, e.size.width - e.marginL - e.marginR, 0),
        dock: Rect.fromLTWH(e.marginL, dockTop, e.size.width - e.marginL - e.marginR, dockH),
        place: PanelPlace.bottom,
        open: false,
        group: null,
        collapsed: false,
      );
    }
    final regionTop = e.topBarBottom + 8;
    final aspect = _frameRatio(ph, false);
    final trailing = wm.side == Hand.right;
    final sw = sidePaneWidth(size.width);
    PanelLayout solve(PanelNeeds n, {PanelPlace? force}) => solvePanelLayout(
          size: size,
          regionTop: regionTop,
          paneTop: e.pad.top + 8,
          dockH: dockH,
          dockBottom: dockBottom,
          aspect: aspect,
          needBottom: n.bottom,
          needSide: n.side,
          needSideMin: n.sideMin,
          growBottom: n.growBottom,
          growSide: n.growSide,
          growMin: n.growMin,
          trailing: trailing,
          photoInsetH: e.photoPad.horizontal / 2,
          margin: e.marginL,
          force: force,
        );
    // The panel is as tall as its category needs and never changes height within it; the placement (bottom / side) is
    // decided per category, screen, photo and hand side, never during a drag.
    final region = dockTop - 8 - regionTop;
    PanelNeeds needsFor({required bool collapsed, required bool itemised}) => panelNeeds(
        screenW: size.width, paneW: sw - e.marginL, screenH: e.size.height, region: region, photoW: size.width - e.photoPad.horizontal, aspect: aspect,
        textScale: e.textScale, collapsed: collapsed, itemised: itemised);
    PanelNeeds needs() => needsFor(collapsed: w.collapsed, itemised: ctl.itemised && PadCatX.ofGroup(w.group) != null);
    final key = '${w.group.name}|${size.width.round()}x${size.height.round()}|${aspect.toStringAsFixed(3)}|$trailing|$id';
    if (key != _placeKey && !ctl.dragging.value) {
      _placeKey = key;
      _place = solve(needsFor(collapsed: false, itemised: false)).place;
    }
    final l = solve(needs(), force: _place);
    // a wide screen (the unfolded inner screen, a tablet) does not stretch the panel into a long strip
    var panel = l.panel;
    if (l.place == PanelPlace.bottom && panel.width > kPanelMaxW) panel = Rect.fromCenter(center: panel.center, width: kPanelMaxW, height: panel.height);
    return _PanelGeo(
        region: l.photoRegion,
        panel: panel,
        dock: l.dock,
        place: l.place,
        open: true,
        group: w.group,
        collapsed: w.collapsed);
  }

  void _retarget() {
    if (!mounted || _env == null) return;
    final next = _targetGeo();
    if (next == _to) return;
    _from = _to == null ? next : _geoNow();
    _to = next;
    _t.jumpTo(0);
    _t.animateTo(1, spring: SpringTier.medium.opacity);
    _panelRect.value = _geoNow().panel;
    _geoRev.value++;
  }

  /// Insets that fit the photo view into [_PanelGeo.region]; the view itself keeps its symmetric system insets
  /// (its zoom maths centres the frame), so the rest is applied as padding around it.
  EdgeInsets _regionInset() {
    final e = _env;
    if (e == null) return EdgeInsets.zero;
    final r = _geoNow().region, b = e.photoPad;
    return EdgeInsets.fromLTRB(
      math.max(0.0, r.left - b.left),
      math.max(0.0, r.top - b.top),
      math.max(0.0, e.size.width - r.right - b.right),
      math.max(0.0, e.size.height - r.bottom - b.bottom),
    );
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

  /// The photo's picture; on a live photo the placeholder -> engine frame swap is a short cross-fade.
  Widget _art(Photo p, bool original) {
    final inner = _artNow(p, original);
    if (!engine.live) return inner;
    final real = original || engine.image != null || engine.gpuTextureId != null;
    return OpenTiming.legacy ? inner : ArtCrossFade(real: real, child: inner);
  }

  Widget _artNow(Photo p, bool original) {
    if (engine.live) {
      final tex = original ? null : engine.gpuTextureId; // zero-copy texture (stage d) while it is live
      if (tex != null) return Texture(textureId: tex, filterQuality: FilterQuality.medium);
      final img = original ? (engine.original ?? engine.image) : engine.image;
      if (img != null) return RawImage(image: img, fit: BoxFit.cover);
      final pl = engine.placeholder;
      if (pl != null && (engine.placeholderDeveloped || !(p.status == PhotoStatus.done && p.previewPath != null))) {
        WidgetsBinding.instance.addPostFrameCallback((_) => OpenTiming.mark('first_pixels', extra: engine.placeholderDeveloped ? 'developed_1280' : 'embedded'));
        return RawImage(image: pl, fit: BoxFit.cover);
      }
      // the grid's own 400 px decode (same image-cache entry): on screen at once, sharper placeholder follows
      return PhotoArt(p, cacheWidth: OpenTiming.legacy ? null : 400, onShown: () => OpenTiming.mark('first_pixels', extra: 'thumbnail'));
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
    ctl.hand.screenWidth = size.width;
    ctl.exploring = mq.accessibleNavigation;
    final pad = mq.padding;
    final gi = mq.systemGestureInsets;
    final marginL = math.max(12.0, gi.left), marginR = math.max(12.0, gi.right);
    final topBarTop = pad.top + 8;
    final topBarBottom = topBarTop + 48;
    final dockBottom = math.max(pad.bottom, 16.0) + 8;
    final dockTop = size.height - dockBottom - 56;
    final keyboard = mq.viewInsets.bottom;
    ctl.availableHeight = size.height - pad.vertical;
    ctl.readoutArea = Rect.fromLTWH(size.width / 2 - 130, topBarBottom + 8, 260, 96);
    final photoPad = EdgeInsets.symmetric(vertical: math.max(pad.top, pad.bottom), horizontal: math.max(pad.left, pad.right));
    final c = context.c;
    // The controls are always the panel (docked under the photo or as a side pane, whichever leaves the larger photo).
    ctl.docked = true;
    _env = _Env(size, pad, marginL, marginR, topBarBottom, dockBottom, keyboard, textScale, photoPad);
    if (_to == null) {
      _to = _targetGeo();
      _from = _to!;
      _panelRect.value = _to!.panel;
    }
    WidgetsBinding.instance.addPostFrameCallback((_) => _retarget());
    final wide = size.width >= 600;

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
              child: _photoStage(),
            ),
          ),
        ),
      ),
      // browse-mode filmstrip on wide screens
      if (wide) _filmstrip(bottom: dockBottom + 56 + 12, left: marginL, right: marginR),
      // 1 the control panel: under the photo or beside it
      ListenableBuilder(
        listenable: Listenable.merge([wm, ctl.edits, _geoRev]),
        builder: (context, _) {
          // everything the panel shows comes from the same snapshot as its size, so a frame never mixes old and new
          final to = _to!;
          return DockedPanelHost(
            controller: ctl,
            rect: _panelRect,
            contentSize: to.panel.size,
            group: to.group,
            collapsed: to.collapsed,
            side: to.place == PanelPlace.side,
          );
        },
      ),
      // 2 readout: counters and the original pill; for a 2D value driven by the photo, the live readout too
      ListenableBuilder(
        listenable: ctl.readout,
        builder: (context, _) {
          final show = ctl.readout.compare || ctl.readout.name.isNotEmpty;
          return Positioned(
            left: 0,
            right: 0,
            top: topBarBottom + 8,
            child: Center(
              child: Visibility(
                visible: show,
                maintainState: true,
                maintainAnimation: true,
                child: ReadoutOverlay(controller: ctl),
              ),
            ),
          );
        },
      ),
      ListenableBuilder(
        listenable: engine,
        // one position in every layout: the photo's top leading corner, inside the photo
        builder: (context, _) => engine.previewOnly
            ? AnimatedBuilder(
                animation: _t,
                builder: (context, _) {
                  final ph = _photoRect();
                  return Positioned(left: ph.left + 8, top: ph.top + 8, child: _previewPill());
                },
              )
            : const SizedBox.shrink(),
      ),
      // 3 top bar and dock (hidden in immersive mode)
      ValueListenableBuilder<bool>(
        valueListenable: ctl.immersive,
        builder: (context, imm, _) => IgnorePointer(
          ignoring: imm,
          child: AnimatedOpacity(
            opacity: imm ? 0 : 1,
            duration: const Duration(milliseconds: 150),
            child: Stack(children: [
              // beside the photo the top bar keeps to the photo's column
              ListenableBuilder(
                listenable: _geoRev,
                builder: (context, _) {
                  final to = _to!;
                  final sideOpen = to.open && to.place == PanelPlace.side;
                  final paneW = sideOpen ? (size.width - to.region.width).clamp(0.0, size.width * 0.6) : 0.0;
                  final trailing = to.region.left == 0;
                  return Positioned(
                    top: topBarTop,
                    left: marginL + (sideOpen && !trailing ? paneW : 0),
                    right: marginR + (sideOpen && trailing ? paneW : 0),
                    child: EditorTopBar(app: app, photoId: () => id, onBack: () => Navigator.of(context).maybePop(), onMore: _toggleMenu),
                  );
                },
              ),
              AnimatedBuilder(
                animation: _t,
                builder: (context, _) => Positioned.fromRect(
                  rect: _geoNow().dock,
                  child: Align(alignment: Alignment.center, child: PadDock(ctl: ctl)),
                ),
              ),
            ]),
          ),
        ),
      ),
      ToastLayer(app: app, bottom: size.height - dockTop + 12, maxWidth: 480),
            HoldPopupLayer(ctl: ctl, dockTop: dockTop, screenW: size.width),
      if (menuOpen) _menu(size, _geoNow().dock, const []),
    ]);
  }

  /// The photo at fit zoom inside the animated photo region.
  Rect _photoRect() {
    final r = _geoNow().region;
    if (r.isEmpty) return Rect.zero;
    final a = _frameRatio(ph, false);
    final h = math.min(r.width / a, r.height);
    return Rect.fromCenter(center: r.center, width: h * a, height: h);
  }

  /// Progress hint while the placeholder is still on screen: only appears after [hintDelay] (a quick open
  /// never shows it), a spinner without words (guidelines 10.13), fixed in the photo's top leading corner.
  Widget _previewPill() => OpenTiming.legacy ? const DelayedBusy(key: ValueKey('preview-busy'), delay: Duration.zero) : const DelayedBusy(key: ValueKey('preview-busy'));

  Widget _photoStage() {
    return ListenableBuilder(
      listenable: Listenable.merge([engine, ctl.compare, ctl.edits, wm, _t, _geoRev]),
      builder: (context, _) {
        final p = ph;
        PerfProbe.count('photoStage');
        final original = ctl.compare.value;
        final focusGroup = wm.focused?.group;
        final showGrid = focusGroup == EGroup.crop && !original;
        final fine = showGrid && ctl.dragging.value;
        final aspect = _frameRatio(p, original);
        if ((aspect - _lastAspect).abs() > 1e-3) {
          _lastAspect = aspect;
          WidgetsBinding.instance.addPostFrameCallback((_) => _retarget());
        }
        return Padding(
          padding: _regionInset(),
          child: ZoomView(
          key: const ValueKey('zoom-stage'),
          controller: zoom,
          aspect: aspect,
          padding: _env?.photoPad ?? const EdgeInsets.all(12),
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
        ),
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

  Widget _menu(Size size, Rect dock, List<EGroup> moreGroups) {
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
          bottom: size.height - dock.top + 8,
          right: math.max(12.0, size.width - dock.right),
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
/// Placeholder -> real frame: a 160 ms cross-fade in the same box (no geometry change); none with reduced motion.
class ArtCrossFade extends StatelessWidget {
  const ArtCrossFade({super.key, required this.real, required this.child});
  final bool real;
  final Widget child;

  @override
  Widget build(BuildContext context) {
    final reduce = MediaQuery.maybeDisableAnimationsOf(context) ?? false;
    return AnimatedSwitcher(
      duration: reduce ? Duration.zero : const Duration(milliseconds: 160),
      layoutBuilder: (cur, prev) => Stack(fit: StackFit.expand, children: [...prev, ?cur]),
      child: KeyedSubtree(key: ValueKey(real ? 'art-real' : 'art-placeholder'), child: child),
    );
  }
}

const Duration hintDelay = Duration(milliseconds: 400);

class DelayedBusy extends StatefulWidget {
  const DelayedBusy({super.key, this.delay = hintDelay});
  final Duration delay;
  @override
  State<DelayedBusy> createState() => _DelayedBusyState();
}

class _DelayedBusyState extends State<DelayedBusy> {
  bool _show = false;
  Timer? _t;

  @override
  void initState() {
    super.initState();
    _t = Timer(widget.delay, () {
      if (mounted) setState(() => _show = true);
    });
  }

  @override
  void dispose() {
    _t?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AnimatedOpacity(
      opacity: _show ? 1 : 0,
      duration: const Duration(milliseconds: 150),
      child: Container(
        width: 28,
        height: 28,
        alignment: Alignment.center,
        decoration: const BoxDecoration(color: Fx.overlay55, shape: BoxShape.circle),
        child: const Spinner(size: 18),
      ),
    );
  }
}

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

/// The inputs the layout needs from the build context (refreshed on every page build).
class _Env {
  const _Env(this.size, this.pad, this.marginL, this.marginR, this.topBarBottom, this.dockBottom, this.keyboard, this.textScale, this.photoPad);
  final Size size;
  final EdgeInsets pad;
  final double marginL, marginR, topBarBottom, dockBottom, keyboard, textScale;
  final EdgeInsets photoPad;
}

/// Where the photo region, the panel and the dock are (and were); interpolated while the layout animates.
class _PanelGeo {
  const _PanelGeo(
      {required this.region,
      required this.panel,
      required this.dock,
      required this.place,
      required this.open,
      required this.group,
      required this.collapsed});
  static const empty = _PanelGeo(region: Rect.zero, panel: Rect.zero, dock: Rect.zero, place: PanelPlace.bottom, open: false, group: null, collapsed: false);

  final Rect region, panel, dock;
  final PanelPlace place;
  final bool open, collapsed;
  final EGroup? group;

  static _PanelGeo lerp(_PanelGeo a, _PanelGeo b, double t) => t >= 1
      ? b
      : _PanelGeo(
          region: Rect.lerp(a.region, b.region, t)!,
          panel: Rect.lerp(a.panel, b.panel, t)!,
          dock: Rect.lerp(a.dock, b.dock, t)!,
          place: b.place,
          open: b.open,
          group: b.group,
          collapsed: b.collapsed,
        );

  @override
  bool operator ==(Object o) => o is _PanelGeo && o.region == region && o.panel == panel && o.dock == dock && o.place == place && o.open == open && o.group == group && o.collapsed == collapsed;

  @override
  int get hashCode => Object.hash(region, panel, dock, place, open, group, collapsed);
}
