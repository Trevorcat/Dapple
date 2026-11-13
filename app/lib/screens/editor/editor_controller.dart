// Hoisted editor state and intent handling: the [GestureSink] of the single gesture arbiter (the photo only zooms, pans and
// compares now), the control pads (docs/design/editor-panel.md), the panel's layers, the transient readout, resets,
// haptics and handedness bookkeeping. Widgets listen to the small notifiers here instead of rebuilding the page.
import 'dart:async';

import 'package:flutter/widgets.dart';

import '../../editor/gesture_arbiter.dart';
import '../../editor/handedness.dart';
import '../../editor/haptics.dart';
import '../../editor/window_manager.dart';
import '../../model/bases.dart';
import '../../model/film_pad.dart';
import '../../services/base_swatch_sources.dart';
import '../../services/base_swatches.dart';
import '../../models.dart' show Photo;
import '../../model/pad_model.dart';
import '../../model/params.dart';
import '../../services/editor_engine.dart';
import '../../state/app_state.dart';
import 'zoom_view.dart';

/// Transient readout near the top of the photo: the "原片" pill and the "n / N" counter.
class ReadoutModel extends ChangeNotifier {
  bool visible = false;
  bool compare = false;

  /// The finger entered the readout area mid-gesture: fade, never move.
  bool dimmed = false;
  String name = '';
  Timer? _hide;

  void notify() => notifyListeners();

  void hideAfter(Duration d) {
    _hide?.cancel();
    _hide = Timer(d, () {
      visible = false;
      notifyListeners();
    });
  }

  void cancelHide() => _hide?.cancel();

  @override
  void dispose() {
    _hide?.cancel();
    super.dispose();
  }
}

/// UI-only selections inside the panel (not part of the photo's edit record).
class PanelSel {
  int band = 1; // colour range, default orange
  int curveCh = 0;
  int gradeZone = 0;
  int hslPart = 1; // 0 hue, 1 saturation, 2 luminance
  int gradePart = 0; // 0 luminance, 1 blend, 2 balance
  int curvePoint = 1;
}

/// The three layers of the panel: layer 2 is the pad (or the film map), layer 3 the itemised view (or one of the complex editors).
enum PanelExtra { curve, hsl, grade, paper, tone }

/// A hold-to-adjust popup above the dock.
class HoldPopup {
  const HoldPopup(this.cat, this.anchorX);
  final PadCat cat;
  final double anchorX;
}

class EditorController extends ChangeNotifier implements GestureSink {
  EditorController({
    required this.app,
    required this.photoId,
    required this.engine,
    required this.zoom,
    required this.wm,
    required this.haptics,
    required this.hand,
    required this.analysis,
    required this.onSwipe,
    required this.onToggleImmersive,
    this.filmPadFetcher,
    this.swatchSource,
  }) {
    edits.addListener(_maybeRefreshRanges);
  }

  final AppState app;
  final String Function() photoId;
  final EditorEngine engine;
  final ZoomController zoom;
  final WindowManager wm;
  final Haptics haptics;
  final HandednessInferrer hand;
  final Analysis Function() analysis;
  final ValueChanged<int> onSwipe;
  final VoidCallback onToggleImmersive;

  /// Asks the engine for the film pads' effective ranges (dapple_session_film_pad_json); null = the live session.
  /// Tests inject a fake.
  final Future<String?> Function(Map<String, dynamic> params)? filmPadFetcher;

  /// Where the base swatches come from; null = the engine through the photo's session. Tests inject a fake.
  final SwatchSource Function(String photoId)? swatchSource;

  BaseSwatches? _swatches;
  String? _swatchesPhoto;

  /// The swatches of the bases on the photo now open (one service per photo; the previous photo's is stopped). Not started:
  /// call [BaseSwatches.start] (the film pad and the film map do).
  BaseSwatches get swatches {
    final pid = id;
    final cur = _swatches;
    if (cur != null && _swatchesPhoto == pid && !cur.closed) return cur;
    cur?.dispose();
    _swatchesPhoto = pid;
    final ph = app.photoById(pid);
    final src = swatchSource?.call(pid) ?? SessionSwatchSource(app, ph ?? Photo(id: pid, name: pid, addedMs: 0));
    final ids = allBaseIds(app.film);
    // the current base first, then the modern renderings and the film stocks in the order of the map
    final first = edit.film;
    return _swatches = BaseSwatches(source: src, ids: [first, ...ids.where((x) => x != first)]);
  }

  /// Starts the background swatches (idempotent); the film pad does it as soon as it shows.
  void warmSwatches() => swatches.start();

  final PanelSel sel = PanelSel();

  /// Which ends of the film pad axes change the render for the current photo and stock. Everything is effective until
  /// the engine answers (or when it cannot). Refreshed, debounced and off the UI thread, when the photo, the stock or
  /// film_shoot changes; never while a finger is down.
  final ValueNotifier<FilmPadRanges> filmRanges = ValueNotifier(FilmPadRanges.all);
  Timer? _rangesTimer;
  String _rangesSig = '';
  int _rangesGen = 0;

  void _maybeRefreshRanges() {
    final e = edit;
    final sig = '$id|${e.film}|${e.filmPaper}|${e.value('film_shoot')}';
    if (sig == _rangesSig) return;
    _rangesSig = sig;
    _rangesTimer?.cancel();
    if (e.film == null || isModernId(e.film)) {
      // no stock: nothing to ask the engine; a modern rendering has no shoot / print axes, both pad axes are dead
      _rangesGen++;
      final want = e.film == null ? FilmPadRanges.all : FilmPadRanges.inert;
      if (filmRanges.value != want) {
        filmRanges.value = want;
        _bump();
      }
      return;
    }
    _rangesTimer = Timer(const Duration(milliseconds: 300), _fetchRanges);
  }

  Future<void> _fetchRanges() async {
    if (dragging.value) {
      _rangesTimer = Timer(const Duration(milliseconds: 300), _fetchRanges); // never during a drag
      return;
    }
    final gen = ++_rangesGen;
    final p = app.photoById(id);
    if (p == null) return;
    String? json;
    try {
      json = await (filmPadFetcher ?? _liveFetch)(app.wireParams(p));
    } catch (_) {
      json = null;
    }
    if (gen != _rangesGen) return; // a newer request supersedes this answer
    final r = FilmPadRanges.parse(json);
    if (r != filmRanges.value) {
      filmRanges.value = r;
      _bump();
    }
  }

  Future<String?> _liveFetch(Map<String, dynamic> params) async {
    if (!app.sessions.available) return null;
    final p = app.photoById(id);
    if (p == null) return null;
    final s = await app.sessions.get(p);
    return s?.filmPadJson(params);
  }

  final ReadoutModel readout = ReadoutModel();

  /// Bumped whenever the panel's structure must refresh (selections, layers, undo, first step of a gesture).
  final ValueNotifier<int> edits = ValueNotifier(0);

  /// True while a pad / strip / ruler gesture is running.
  final ValueNotifier<bool> dragging = ValueNotifier(false);
  final ValueNotifier<bool> compare = ValueNotifier(false);
  final ValueNotifier<bool> immersive = ValueNotifier(false);

  /// Id of the item whose value is being typed, or null.
  final ValueNotifier<String?> editingRow = ValueNotifier(null);

  /// The hold-to-adjust popup, or null.
  final ValueNotifier<HoldPopup?> hold = ValueNotifier(null);

  /// Phone portrait: the category controls are a panel docked under the photo (set by the page).
  bool docked = false;

  /// A screen reader explores by touch.
  bool exploring = false;

  /// Height available to the photo.
  double availableHeight = 800;

  /// Region of the top readout.
  Rect readoutArea = Rect.zero;

  Timer? _idleTimer;
  int _seq = 0;

  String get id => photoId();
  EditState get edit => app.editOf(id);

  PadCtx get padCtx => PadCtx(defaultTone: app.settings.toneStrength, defaultLookId: app.settings.defaultLookId);

  void _bump() {
    edits.value++;
  }

  // ------------------------------------------------------------ panel layers

  bool itemised = false;
  bool filmMapOpen = false;
  PanelExtra? extra;
  int selItem = 0;

  /// The pad category of the open panel, or null (nothing open, or 裁切).
  PadCat? get openCat {
    final g = wm.focused?.group;
    return g == null ? null : PadCatX.ofGroup(g);
  }

  void _resetLayers() {
    itemised = false;
    filmMapOpen = false;
    extra = null;
    selItem = 0;
  }

  void setItemised(bool v) {
    itemised = v;
    filmMapOpen = false;
    extra = null;
    selItem = 0;
    _bump();
  }

  void setFilmMap(bool v) {
    filmMapOpen = v;
    itemised = false;
    extra = null;
    _bump();
  }

  void openExtra(PanelExtra? e) {
    extra = e;
    _bump();
  }

  void selectItem(int i) {
    if (selItem == i) return;
    selItem = i;
    _bump();
  }

  void openCategory(EGroup g) {
    final was = wm.focused?.group;
    wm.toggle(g);
    if (was != g) _resetLayers();
    _bump();
  }

  /// Opens [g] without toggling (debug link, tests): layers back to the pad.
  void showCategory(EGroup g) {
    if (wm.focused?.group != g) wm.toggle(g);
    _resetLayers();
    _bump();
  }

  // ------------------------------------------------------------ pads

  PadState padView(PadCat cat) => padViewOf(edit, cat, padCtx);
  PadState padFull(PadCat cat) => padStateOf(edit, cat, padCtx);

  /// Reachable pad area (only the film pad has dead ends).
  PadLimits padLimits(PadCat cat) {
    if (cat != PadCat.film) return PadLimits.full;
    final r = filmRanges.value;
    return PadLimits(
      xLo: r.lo('film_shoot') ? -1 : 0,
      xHi: r.hi('film_shoot') ? 1 : 0,
      yLo: r.lo('film_print') ? -1 : 0,
      yHi: r.hi('film_print') ? 1 : 0,
    );
  }

  String? get defaultStockId {
    final c = app.film;
    if (c.isEmpty) return null;
    // N400C (a general-purpose 400 negative) when the library has it, else the first stock
    return c.stocks.where((s) => s.name == 'N400C').firstOrNull?.id ?? c.stocks.first.id;
  }

  String _gestureKey = '';
  bool _firstApply = true;
  PadCat? _padCat;
  bool _gestureOn = false;

  /// A control gesture starts (pad, strip, ruler, radar, film map): one undo step until [gestureEnd].
  void gestureBegin(String tag, {String? param}) {
    if (_gestureOn) gestureEnd();
    _gestureOn = true;
    _gestureKey = '$tag#${++_seq}';
    _firstApply = true;
    wm.setInteracting(true);
    engine.beginInteraction(activeParam: param);
    dragging.value = true;
  }

  void gestureEnd() {
    if (!_gestureOn) return;
    _gestureOn = false;
    _padCat = null;
    engine.endInteraction();
    wm.setInteracting(false);
    dragging.value = false;
    _bump();
  }

  void _apply(EditState Function(EditState) f) {
    if (!_gestureOn) {
      app.applyEdit(id, f);
      _bump();
      return;
    }
    app.applyEdit(id, f, coalesce: _gestureKey, gesture: true);
    if (_firstApply) {
      _firstApply = false;
      _bump();
    }
  }

  /// The engine parameter a pad gesture names to the engine (grain-family keys keep grain in interactive frames).
  static String padParam(PadCat cat) => switch (cat) {
        PadCat.light => 'exposure',
        PadCat.color => 'temp',
        PadCat.film => 'film_shoot',
        PadCat.texture => 'sharpen',
      };

  void padBegin(PadCat cat) {
    gestureBegin('pad:${cat.name}', param: padParam(cat));
    _padCat = cat;
  }

  /// Cursor to the absolute pad position ([x], [y] in -1..1). A little snap to the centre per axis.
  void padMove(PadCat cat, double x, double y) {
    if (_padCat != cat) padBegin(cat);
    final lim = padLimits(cat);
    var px = x.clamp(-1.0, 1.0).toDouble(), py = y.clamp(-1.0, 1.0).toDouble();
    if (px.abs() < kPadSnap) px = 0;
    if (py.abs() < kPadSnap) py = 0;
    final c = lim.clamp(px, py);
    px = c.dx;
    py = c.dy;
    final before = padView(cat);
    if (before.x == px && before.y == py) return;
    if (px == 0 && py == 0 && (before.x != 0 || before.y != 0)) haptics.fire(HapticKind.autoSnap);
    final ctx = padCtx;
    final stock = defaultStockId;
    _apply((e) {
      var n = e;
      if (cat == PadCat.film && n.film == null && stock != null) n = n.copyWith(film: stock, filmPaper: null);
      final s = padStateOf(n, cat, ctx);
      return applyPadState(n, cat, s.copyWith(x: px, y: py), ctx);
    });
  }

  /// The strength strip to [a] (0..1, 0.5 = 100 %). Snaps to 100 %.
  void stripSet(PadCat cat, double a) {
    if (_padCat != cat) padBegin(cat);
    var v = a.clamp(0.0, 1.0).toDouble();
    if ((v - 0.5).abs() < kStripSnap) v = 0.5;
    final before = padView(cat);
    if (before.a == v) return;
    if (v == 0.5) haptics.fire(HapticKind.autoSnap);
    final ctx = padCtx;
    final stock = defaultStockId;
    _apply((e) {
      var n = e;
      if (cat == PadCat.film && n.film == null && stock != null) n = n.copyWith(film: stock, filmPaper: null);
      final s = padStateOf(n, cat, ctx);
      return applyPadState(n, cat, s.copyWith(a: v), ctx);
    });
  }

  void padEnd() => gestureEnd();

  /// A style preset on the colour pad: selects the look and moves the cursor there (one undo step).
  void selectPreset(ColorPreset p) {
    final ctx = padCtx;
    app.applyEdit(id, (e) {
      var n = e.copyWith(look: p.look == ctx.defaultLookId ? null : p.look, palette: null);
      final s = padStateOf(n, PadCat.color, ctx);
      n = applyPadState(n, PadCat.color, s.copyWith(x: p.x, y: p.y), ctx);
      return n;
    });
    haptics.fire(HapticKind.detent);
    _bump();
  }

  // ------------------------------------------------------------ itemised view

  /// Items of the open category (the radar's spokes), for the current stock.
  List<PadItem> itemsOf(PadCat cat) {
    final stock = app.film.stock(edit.film);
    return padItems(cat, printed: stock?.printed ?? true, modern: isModernId(edit.film));
  }

  /// Sets one item (radar / ruler): the pad keeps its position; the item gets an offset.
  void itemSet(PadCat cat, PadItem it, double v) {
    final ctx = padCtx;
    final stock = defaultStockId;
    _apply((e) {
      var n = e;
      if (cat == PadCat.film && n.film == null && stock != null) n = n.copyWith(film: stock, filmPaper: null);
      return setItemValue(n, cat, it, v, ctx);
    });
  }

  void itemBegin(PadCat cat, PadItem it) => gestureBegin('item:${it.id}', param: it.id);

  void itemEnd() => gestureEnd();

  void resetItem(PadCat cat, PadItem it) {
    final ctx = padCtx;
    final s = padStateOf(edit, cat, ctx);
    final r = padRecipe(cat, s.x, s.y, s.a, ctx: ctx);
    app.applyEdit(id, (e) => setItemValue(e, cat, it, r[it.id] ?? it.auto, ctx));
    haptics.fire(HapticKind.autoSnap);
    _bump();
  }

  /// Typed value for the selected item. True when the text parsed.
  bool commitItem(PadCat cat, PadItem it, String text) {
    editingRow.value = null;
    wm.setInteracting(false);
    final t = text.trim().replaceAll('−', '-').replaceAll('EV', '').replaceAll('%', '').trim();
    final n = double.tryParse(t);
    if (n == null) {
      _bump();
      return false;
    }
    final v = it.clampV(typedToStored(it, n));
    app.applyEdit(id, (e) => setItemValue(e, cat, it, v, padCtx));
    _bump();
    return true;
  }

  void beginNumeric(PadItem it) {
    editingRow.value = it.id;
    wm.setInteracting(true);
    _bump();
  }

  void cancelNumeric() {
    if (editingRow.value == null) return;
    editingRow.value = null;
    wm.setInteracting(false);
    _bump();
  }

  /// The text the entry shows for [it]: the value as the display prints it, without unit.
  String inputText(PadItem it, double stored) => storedToTyped(it, stored);

  // ------------------------------------------------------------ film

  /// The film map: the stock follows the finger; the whole press is one undo step.
  void stockBegin() => gestureBegin('stock', param: 'film');

  void stockSet(String? stockId) {
    if (edit.film == stockId) return;
    _apply((e) {
      if (stockId == null) return clearFilm(e);
      if (isModernId(stockId)) return withModernBase(e, stockId);
      return e.copyWith(film: stockId, filmPaper: null);
    });
  }

  void stockEnd() => gestureEnd();

  /// The 无 button: no film (one undo step).
  void stockNone() {
    stockBegin();
    stockSet(null);
    stockEnd();
  }

  /// Film off: the stock, the paper and every film knob go back to their defaults.
  static EditState clearFilm(EditState e) {
    final m = Map<String, double>.of(e.values)..removeWhere((k, v) => filmValueKeys.contains(k));
    final p = Map<String, PadState>.of(e.pads)..remove(PadCat.film.name);
    return e.copyWith(film: null, filmPaper: null, values: m, pads: p);
  }

  void selectPaper(String? paperId) {
    app.applyEdit(id, (e) => e.copyWith(filmPaper: paperId));
    _bump();
  }

  // ------------------------------------------------------------ plain values (crop, curve, colour ranges, grading)

  void valueBegin(String key) => gestureBegin('value:$key', param: key);

  void valueSet(String key, double v) {
    final s = specByKey[key]!;
    final c = v.clamp(s.min, s.max).toDouble();
    _apply((e) => e.withValue(key, c));
  }

  void valueEnd() => gestureEnd();

  void valueReset(String key) {
    app.applyEdit(id, (e) => e.withValue(key, specByKey[key]!.def));
    haptics.fire(HapticKind.autoSnap);
    _bump();
  }

  /// The grading wheel: hue and saturation together.
  void wheelSet(int zone, double hue, double sat) {
    _apply((e) {
      var n = e.withValue(grKey(zone, 's'), sat);
      if (sat > 0) n = n.withValue(grKey(zone, 'h'), hue);
      return n;
    });
  }

  void setCropRatio(String ratio) {
    app.applyEdit(id, (e) => e.copyWith(crop: ratio));
    _bump();
  }

  void rotate90() {
    app.applyEdit(id, (e) => e.copyWith(rot: (e.rot + 90) % 360));
    _bump();
  }

  void toggleFlip() {
    app.applyEdit(id, (e) => e.copyWith(flip: !e.flip));
    _bump();
  }

  /// 明暗目标: the tone class under the finger (inside a gesture), or back to automatic.
  void toneTargetSet(String id) => _apply((e) => e.copyWith(toneTarget: id));

  void toneTargetAuto() {
    app.applyEdit(id, (e) => e.copyWith(toneTarget: null));
    haptics.fire(HapticKind.autoSnap);
    _bump();
  }

  // ------------------------------------------------------------ resets

  void resetPad(PadCat cat) {
    if (!padCatAdjusted(edit, cat, padCtx)) return;
    final before = edit;
    app.applyEdit(id, (e) {
      var n = resetPadCat(e, cat, padCtx);
      if (cat == PadCat.light) n = _resetGroup(n, EGroup.curve);
      if (cat == PadCat.color) n = _resetGroup(_resetGroup(n, EGroup.grade), EGroup.color);
      if (cat == PadCat.texture) n = _resetGroup(_resetGroup(n, EGroup.effects), EGroup.detail);
      return n;
    });
    if (edit.sameAs(before)) return;
    _bump();
    haptics.fire(HapticKind.autoSnap);
    final pid = id;
    app.showToast('已复位${cat.label}', actionLabel: '撤销', onAction: () => app.undo(pid));
  }

  EditState _resetGroup(EditState e, EGroup g) {
    final m = Map<String, double>.of(e.values)..removeWhere((k, v) => specByKey[k]!.group == g);
    return e.copyWith(values: m);
  }

  void resetCrop() {
    app.applyEdit(id, (e) {
      final m = Map<String, double>.of(e.values)..remove('straighten');
      return e.copyWith(crop: 'orig', rot: 0, flip: false, values: m);
    });
    _bump();
    haptics.fire(HapticKind.autoSnap);
  }

  // ------------------------------------------------------------ hold-to-adjust (the popup above the dock)

  PadState? _holdStart;
  double _holdPx = 0;

  /// Pad extent (dp) that one full range of the pad takes while holding: the popup is about 216 dp wide.
  static const double kHoldSpan = 94;

  void holdBegin(PadCat cat, double anchorX) {
    wm.closeAll();
    _resetLayers();
    hold.value = HoldPopup(cat, anchorX);
    _holdStart = padView(cat);
    _holdPx = kHoldSpan;
    padBegin(cat);
    _bump();
  }

  /// [dx], [dy]: finger travel since the press (screen, y down). Moves the value relative to its start.
  void holdMove(double dx, double dy) {
    final h = hold.value;
    final s = _holdStart;
    if (h == null || s == null) return;
    padMove(h.cat, s.x + dx / _holdPx, s.y - dy / _holdPx);
  }

  void holdEnd() {
    if (hold.value == null) return;
    hold.value = null;
    _holdStart = null;
    gestureEnd();
  }

  // ------------------------------------------------------------ GestureSink: context

  @override
  ArbiterMode get mode => ArbiterMode.browse; // the photo only zooms, pans and compares

  @override
  ActiveKind get activeKind => ActiveKind.none;

  @override
  bool get zoomed => zoom.zoomed;

  @override
  bool get adjustEnabled => false;

  @override
  void adjustBegin(LockAxis axis, double x, double y) {}

  @override
  void adjustMove(double dx, double dy, double offX, double offY, double vx, double vy, double dtMs) {}

  @override
  void adjustEnd({required bool byMultiTouch}) {}

  // ------------------------------------------------------------ browse / zoom

  @override
  void panBy(double dx, double dy) => zoom.panBy(Offset(dx, dy));

  @override
  void panEnd(double vx, double vy) => zoom.panEnd(Offset(vx, vy));

  @override
  void pageDrag(double dx) => zoom.pageDrag(dx);

  @override
  void pageEnd(double totalDx, double vx) => zoom.pageEnd(totalDx, vx, onSwipe);

  @override
  void pageCancel() => zoom.pageCancel();

  @override
  void longPress(bool on) {
    if (compare.value == on) return;
    wm.setInteracting(on);
    compare.value = on;
    readout
      ..compare = on
      ..visible = on
      ..dimmed = false;
    if (on) readout.cancelHide();
    readout.notify();
  }

  @override
  void scaleStart(double fx, double fy) {
    wm.setInteracting(true);
    zoom.scaleStart(Offset(fx, fy));
  }

  @override
  void scaleUpdate(double fx, double fy, double scale) => zoom.scaleUpdate(Offset(fx, fy), scale);

  @override
  void scaleEnd(double vx, double vy) {
    zoom.scaleEnd(Offset(vx, vy));
    wm.setInteracting(false);
  }

  @override
  void tap(double x, double y) {
    if (editingRow.value != null) {
      editingRow.value = null; // the entry widget commits on losing focus
      _bump();
      return;
    }
    if (wm.adjustMode) {
      wm.tapEmptyPhoto();
    } else {
      onToggleImmersive();
    }
  }

  @override
  void doubleTap(double x, double y) => zoom.doubleTap(Offset(x, y));

  // ------------------------------------------------------------ handedness bookkeeping

  @override
  void strokeEnded(StrokeSample s) {
    hand.add(s, DateTime.now().millisecondsSinceEpoch);
  }

  @override
  void pointerActivity({required bool down}) {
    _idleTimer?.cancel();
    if (down) return;
    _idleTimer = Timer(const Duration(milliseconds: 800), _evaluateHand);
  }

  void _evaluateHand() {
    if (wm.interacting || editingRow.value != null || dragging.value || compare.value) return;
    if (app.settings.handedness != 0) return;
    if (hand.evaluate(DateTime.now().millisecondsSinceEpoch)) {
      final side = hand.side;
      if (wm.setSide(side)) {
        app.settings.handSide = side == Hand.left ? 0 : 1;
        app.updateSettings((_) {});
      }
    }
  }

  /// Short plain-text readout (e.g. the "n / N" photo counter, 800 ms).
  void showText(String text, {Duration hold = const Duration(milliseconds: 800)}) {
    readout
      ..visible = true
      ..compare = false
      ..name = text
      ..dimmed = false;
    readout.hideAfter(hold);
    readout.notify();
  }

  // ------------------------------------------------------------ back

  /// System back: typed value, then the layers (extra editor, itemised view, film map), then fold, then close, then zoom.
  /// Returns true when something was dismissed.
  bool handleBack() {
    if (editingRow.value != null) {
      cancelNumeric();
      return true;
    }
    if (extra != null) {
      openExtra(null);
      return true;
    }
    if (itemised || filmMapOpen) {
      itemised = false;
      filmMapOpen = false;
      _bump();
      return true;
    }
    if (docked) {
      final w = wm.focused;
      if (w != null && !w.collapsed) {
        wm.setCollapsed(w.group, true);
        return true;
      }
    }
    if (wm.closeLatest()) {
      _resetLayers();
      _bump();
      return true;
    }
    if (zoom.zoomed) {
      zoom.reset(animate: true);
      return true;
    }
    if (immersive.value) {
      immersive.value = false;
      return true;
    }
    return false;
  }

  bool get canPop => editingRow.value == null && !wm.adjustMode && !zoom.zoomed && !immersive.value;

  @override
  void dispose() {
    edits.removeListener(_maybeRefreshRanges);
    _rangesTimer?.cancel();
    filmRanges.dispose();
    _swatches?.dispose();
    _idleTimer?.cancel();
    readout.dispose();
    edits.dispose();
    dragging.dispose();
    compare.dispose();
    immersive.dispose();
    editingRow.dispose();
    hold.dispose();
    super.dispose();
  }
}

/// Per-axis snap to the pad centre (in pad units, 1 = the pad's edge) and the strip's snap to 100 %.
const double kPadSnap = 0.04;
const double kStripSnap = 0.03;

/// Typed text <-> stored value of an item: what the user reads and types is what the display prints (EV for exposure,
/// hundredths for the +-1 film keys).
double typedToStored(PadItem it, double typed) {
  switch (it.id) {
    case 'exposure':
      return typed * 20;
    case 'film_shoot':
    case 'film_print':
    case 'film_black':
    case 'film_wb':
      return typed / 100;
    case 'film_latitude':
      return typed <= 80 ? (typed - 80) / 25 : (typed - 80) / 18;
    default:
      return typed;
  }
}

String storedToTyped(PadItem it, double stored) {
  switch (it.id) {
    case 'exposure':
      return (stored / 20).toStringAsFixed(2);
    case 'film_shoot':
    case 'film_print':
    case 'film_black':
    case 'film_wb':
      return (stored * 100).round().toString();
    case 'film_latitude':
      return (stored <= 0 ? 80 + stored * 25 : 80 + stored * 18).round().toString();
    default:
      return stored.round().toString();
  }
}
