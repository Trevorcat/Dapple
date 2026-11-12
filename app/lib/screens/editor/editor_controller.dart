// Hoisted editor state and intent handling: the [GestureSink] of the single gesture arbiter, the
// floating window model, the transient readout, numeric entry, resets, haptics and handedness
// bookkeeping. Widgets listen to the small notifiers here instead of rebuilding the page.
import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/widgets.dart';

import '../../editor/gesture_arbiter.dart';
import '../../editor/handedness.dart';
import '../../editor/haptics.dart';
import '../../editor/param_rows.dart';
import '../../editor/value_mapping.dart';
import '../../editor/window_manager.dart';
import '../../model/catalog.dart';
import '../../model/film_pad.dart';
import '../../model/params.dart';
import '../../services/editor_engine.dart';
import '../../state/app_state.dart';
import 'zoom_view.dart';
import '../../services/perf_probe.dart';

/// Transient readout near the top of the photo (requirements 4.15.9). One mutable instance is
/// reused for the whole gesture: no per-move allocation, listeners repaint only what they show.
class ReadoutModel extends ChangeNotifier {
  bool visible = false;
  bool compare = false;

  /// Placed in the lower part of the screen (the finger started close to the top readout).
  bool low = false;

  /// The finger entered the readout area mid-gesture: fade, never move.
  bool dimmed = false;
  String name = '', value = '', bandText = '';
  RowKind kind = RowKind.oneD;
  TwoD? twoD;
  double v = 0, min = 0, max = 1, auto = 0, step = 1;
  int endSide = 0;
  PrecisionBand band = PrecisionBand.standard;

  /// Bumped when the auto marker should flash (snap landmark).
  int flash = 0;
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
    if (e.film == null) {
      _rangesGen++;
      if (filmRanges.value != FilmPadRanges.all) {
        filmRanges.value = FilmPadRanges.all;
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

  /// Bumped whenever window contents must refresh (edits, selections, undo).
  final ValueNotifier<int> edits = ValueNotifier(0);

  /// True while an adjust gesture is running: windows show one line.
  final ValueNotifier<bool> dragging = ValueNotifier(false);
  final ValueNotifier<bool> compare = ValueNotifier(false);
  final ValueNotifier<bool> immersive = ValueNotifier(false);

  /// Id of the row currently typed into, or null.
  final ValueNotifier<String?> editingRow = ValueNotifier(null);

  /// Mini-map "fine" switch of 2D controls (kept for the session).
  final ValueNotifier<bool> fine2D = ValueNotifier(false);

  /// Phone portrait: the category controls are a panel docked under the photo (set by the page), not
  /// floating windows. Changes what back and a tap on the photo do to the open category.
  bool docked = false;


  final Map<EGroup, String> _activeId = {};

  /// A screen reader explores by touch: one-finger drags must not change values.
  bool exploring = false;

  /// Height available to the photo (scales the precision-band thresholds).
  double availableHeight = 800;

  /// Region of the top readout (to keep it away from the finger).
  Rect readoutArea = Rect.zero;

  Timer? _idleTimer;
  Timer? _undragTimer;
  int _seq = 0;

  String get id => photoId();
  EditState get edit => app.editOf(id);

  // ------------------------------------------------------------ rows

  RowContext ctx() => RowContext(
        edit: edit,
        defaultLookId: app.settings.defaultLookId,
        defaultTone: app.settings.toneStrength,
        nearestTone: analysis().nearest,
        sel: sel,
        film: app.film,
        filmRanges: filmRanges.value,
      );

  List<ParamRow> rowsOf(EGroup g) => rowsFor(g, ctx());

  /// Active row of category [g] (remembered for the editing session; first row by default).
  ParamRow activeRowOf(EGroup g, [List<ParamRow>? rows]) {
    final rs = rows ?? rowsOf(g);
    final wanted = _activeId[g];
    for (final r in rs) {
      if (r.id == wanted) return r;
    }
    return rs.firstWhere((r) => r.switchable, orElse: () => rs.first);
  }

  ParamRow? get activeRow {
    final w = wm.focused;
    return w == null ? null : activeRowOf(w.group);
  }

  void setActive(EGroup g, ParamRow r) {
    if (!r.switchable) return;
    _activeId[g] = r.id;
    wm.focus(g);
    _bump();
  }

  void _bump() {
    edits.value++;
  }

  /// A control (docked ruler, pad) starts an adjust gesture on [row]: the same code path as a drag
  /// on the photo, so both stay in sync. Makes [row] the active row of its category first.
  void beginOn(ParamRow row, LockAxis axis, double x, double y) {
    if (row.switchable) _activeId[row.group] = row.id;
    wm.focus(row.group);
    adjustBegin(axis, x, y);
  }

  /// Chip tap in the docked panel: selects the row (or runs an action row) and opens a collapsed panel.
  void selectRow(EGroup g, ParamRow r) {
    if (r.kind == RowKind.action) {
      performAction(r);
      return;
    }
    _activeId[g] = r.id;
    wm.focus(g);
    if (wm.windowOf(g)?.collapsed ?? false) wm.setCollapsed(g, false);
    _bump();
  }

  // ------------------------------------------------------------ GestureSink: context

  @override
  ArbiterMode get mode => wm.adjustMode ? ArbiterMode.adjust : ArbiterMode.browse;

  @override
  ActiveKind get activeKind => activeRow?.activeKind ?? ActiveKind.none;

  @override
  bool get zoomed => zoom.zoomed;

  @override
  bool get adjustEnabled => !exploring;

  // ------------------------------------------------------------ adjust gesture

  ParamRow? _row;
  LockAxis _axis = LockAxis.none;
  RelativeMapper? _m1;
  DetentStepper? _st, _gx, _gy, _switcher;
  PaletteMapper? _pal;
  WheelMapper? _wheel;
  List<ParamRow> _switchRows = const [];
  BandTracker _bands = BandTracker();
  String _gestureKey = '';
  double _lastQ = double.nan;
  double _px = 0, _py = 0;
  EGroup? _group;
  String _lastTone = '';

  @override
  void adjustBegin(LockAxis axis, double x, double y) {
    final w = wm.focused;
    if (w == null) return;
    _group = w.group;
    _axis = axis;
    _px = x;
    _py = y;
    final c = ctx();
    final rows = rowsFor(w.group, c);
    final row = activeRowOf(w.group, rows);
    _row = row;
    _bands = BandTracker(availableHeight: availableHeight);
    _gestureKey = '${row.id}#${++_seq}';
    _firstApply = true;
    _lastQ = double.nan;
    _m1 = _st = _gx = _gy = _switcher = null;
    _pal = null;
    _wheel = null;

    if (axis == LockAxis.vertical) {
      _switchRows = [for (final r in rows) if (r.switchable) r];
      final idx = _switchRows.indexWhere((r) => r.id == row.id);
      _switcher = DetentStepper(stepDp: 40, count: _switchRows.length)..begin(idx < 0 ? 0 : idx);
    } else if (row.kind == RowKind.oneD) {
      _m1 = RelativeMapper(min: row.min, max: row.max, auto: row.auto, step: row.step)..begin(row.value(c));
    } else if (row.kind == RowKind.discrete) {
      _st = DetentStepper(stepDp: 48, count: row.count)..begin(row.readIndex!(c));
    } else if (row.kind == RowKind.twoD) {
      switch (row.twoD!) {
        case TwoD.palette:
          final look = lookById(c.edit.look ?? c.defaultLookId);
          final p = c.edit.palette ?? Offset(look.px, look.py);
          _pal = PaletteMapper(defX: look.px, defY: look.py)..begin(p.dx, p.dy);
        case TwoD.tone:
          final cur = c.edit.toneTarget ?? c.nearestTone;
          final parts = cur.split('-');
          final r = toneKeys.indexOf(parts[0]).clamp(0, 2), col = toneRanges.indexOf(parts[1]).clamp(0, 2);
          _gx = DetentStepper(stepDp: 56, count: 3)..begin(col);
          _gy = DetentStepper(stepDp: 56, count: 3)..begin(r);
          _lastTone = cur;
        case TwoD.curve:
          _m1 = RelativeMapper(min: row.min, max: row.max, auto: row.auto)..begin(row.value(c));
        case TwoD.wheel:
          final z = c.sel.gradeZone;
          _wheel = WheelMapper()..begin(c.edit.value(grKey(z, 'h')), c.edit.value(grKey(z, 's')));
        case TwoD.offset:
          // pad coordinates: x right = +1; y up = +1; the mapper works on 0..1 screen coordinates (y down)
          // an ineffective end of an axis is unreachable: the mapper stops at the centre line there
          final eff = row.padEff;
          final xm = row.padXMin, defX = (0 - xm) / (1 - xm); // 0.5 symmetric, 0 for a one-sided axis (film_wb)
          _pal = PaletteMapper(
              defX: defX,
              defY: 0.5,
              lo: 0,
              hi: 1,
              loX: eff[0] ? 0 : defX,
              hiX: eff[1] ? 1 : defX,
              loY: eff[3] ? 0 : 0.5,
              hiY: eff[2] ? 1 : 0.5)
            ..begin((c.edit.value(row.padX!) - xm) / (1 - xm), (1 - c.edit.value(row.padY!)) / 2);
      }
    }

    // readout placement: away from the finger (decided once, never moved during the gesture)
    readout.low = readoutArea != Rect.zero && readoutArea.inflate(72).contains(Offset(x, y));
    readout.dimmed = false;
    readout.cancelHide();
    wm.setInteracting(true);
    engine.beginInteraction(activeParam: row.padX ?? row.id); // film pads: the X key names the pad to the engine
    _undragTimer?.cancel();
    dragging.value = true;
    _publish(row, c, PrecisionBand.standard);
  }

  /// Structural refresh is needed on the first step of a gesture (edited marks appear) and, for the crop
  /// window, on every step (the photo itself rotates / crops); the other pointer moves only change the
  /// windows' one-line readout, which listens to the readout model.
  bool _firstApply = true;

  void _apply(EditState Function(EditState) f) {
    app.applyEdit(id, f, coalesce: _gestureKey, gesture: true);
    if (_firstApply || _group == EGroup.crop) {
      _firstApply = false;
      _bump();
    }
  }

  @override
  void adjustMove(double dx, double dy, double offX, double offY, double vx, double vy, double dtMs) {
    final row = _row;
    if (row == null) return;
    final t0 = PerfProbe.begin();
    _adjustMove(row, dx, dy, offX, offY, vx, vy, dtMs);
    PerfProbe.end('move', t0);
  }

  void _adjustMove(ParamRow row, double dx, double dy, double offX, double offY, double vx, double vy, double dtMs) {
    _px += dx;
    _py += dy;
    final inReadout = readoutArea != Rect.zero && !readout.low && readoutArea.contains(Offset(_px, _py));
    if (inReadout != readout.dimmed) {
      readout.dimmed = inReadout;
      readout.notify();
    }
    final speed = vx.abs();
    if (_axis == LockAxis.vertical) {
      final sw = _switcher!;
      if (sw.update(dy)) {
        final r = _switchRows[sw.index];
        _activeId[_group!] = r.id;
        _row = r;
        _bump();
        _publish(r, ctx(), PrecisionBand.standard);
      }
      haptics.landmark(sw.landmark);
      return;
    }
    switch (row.kind) {
      case RowKind.oneD:
        _moveOneD(row, dx, speed, offY, dtMs);
      case RowKind.discrete:
        final st = _st!;
        if (st.update(dx)) _setDiscrete(row, st.index);
        haptics.landmark(st.landmark);
      case RowKind.twoD:
        _moveTwoD(row, dx, dy, vx, vy, dtMs);
      case RowKind.action:
        break;
    }
  }

  void _moveOneD(ParamRow row, double dx, double speed, double offY, double dtMs) {
    final m = _m1!;
    final band = _bands.update(offY);
    m.update(dx, speed, band, dtMs);
    final q = m.quantized;
    if (q != _lastQ) {
      _lastQ = q;
      _apply((e) => row.write!(e, q));
    }
    haptics.landmark(m.landmark);
    readout
      ..v = m.value
      ..value = row.fmt!(q)
      ..band = band
      ..bandText = bandLabel(band)
      ..endSide = m.endSide;
    if (m.landmark == Landmark.autoSnap) readout.flash++;
    readout.notify();
  }

  void _setDiscrete(ParamRow row, int i) {
    if (row.uiSet != null) {
      row.uiSet!(sel, i);
      _bump();
    } else {
      final c = ctx();
      _apply((e) => row.writeIndex!(e, i, c));
    }
    readout
      ..value = row.itemLabel!(i)
      ..v = i.toDouble();
    readout.notify();
  }

  void _moveTwoD(ParamRow row, double dx, double dy, double vx, double vy, double dtMs) {
    final speed = math.sqrt(vx * vx + vy * vy);
    final fine = fine2D.value ? 0.25 : 1.0;
    switch (row.twoD!) {
      case TwoD.palette:
        final p = _pal!;
        p.update(dx, dy, speed, dtMs, fineScale: fine);
        final pos = Offset(p.x, p.y);
        final snapped = p.snapped;
        _apply((e) => e.copyWith(palette: snapped ? null : pos));
        haptics.landmark(p.landmark);
        readout.value = paletteText(pos);
        if (p.landmark == Landmark.autoSnap) readout.flash++;
      case TwoD.tone:
        final a = _gx!.update(dx), b = _gy!.update(dy);
        if (a || b) {
          final id2 = toneClassId(toneKeys[_gy!.index], toneRanges[_gx!.index]);
          if (id2 != _lastTone) {
            _lastTone = id2;
            _apply((e) => e.copyWith(toneTarget: id2));
          }
          readout.value = tonePlain(id2);
          // the nearest class is the auto position: a firmer tick when entering it
          haptics.landmark(id2 == analysis().nearest ? Landmark.autoSnap : Landmark.detent);
        }
      case TwoD.curve:
        final m = _m1!;
        m.update(-dy, vy.abs(), fine2D.value ? PrecisionBand.fine : PrecisionBand.standard, dtMs);
        final q = m.quantized;
        if (q != _lastQ) {
          _lastQ = q;
          _apply((e) => row.write!(e, q));
        }
        haptics.landmark(m.landmark);
        readout
          ..v = m.value
          ..value = row.fmt!(q)
          ..endSide = m.endSide;
        if (m.landmark == Landmark.autoSnap) readout.flash++;
      case TwoD.offset:
        final p = _pal!;
        p.update(dx, dy, speed, dtMs, fineScale: fine);
        // NATIVE BINDING (film pads): the only place the two offsets are written. They land in the edit's params
        // map under row.padX / row.padY (film_shoot / film_print), normalised to [-1, +1],
        // 0 = automatic; EditState.toWire sends them like every other film_* key, so connecting the engine means
        // the engine's film node reading those keys; nothing else changes here.
        final xm = row.padXMin;
        final ox = p.snapped ? 0.0 : (((xm + p.x * (1 - xm)) * 100).round() / 100);
        final oy = p.snapped ? 0.0 : (((1 - p.y * 2) * 100).round() / 100);
        _apply((e) => e.withValue(row.padX!, ox).withValue(row.padY!, oy));
        haptics.landmark(p.landmark);
        readout.value = offsetPadText(ox, oy, xSigned: xm < 0);
        if (p.landmark == Landmark.autoSnap) readout.flash++;
      case TwoD.wheel:
        final w = _wheel!;
        w.update(dx, dy, speed, dtMs, fineScale: fine);
        final z = sel.gradeZone;
        final h = w.hue.roundToDouble() % 360, s = w.sat.roundToDouble();
        _apply((e) {
          var n = e.withValue(grKey(z, 's'), s);
          if (s > 0) n = n.withValue(grKey(z, 'h'), h);
          return n;
        });
        haptics.landmark(w.landmark);
        readout.value = s == 0 ? '无' : '${h.round()}° · ${s.round()}';
        if (w.landmark == Landmark.autoSnap) readout.flash++;
    }
    readout.notify();
  }

  void _publish(ParamRow row, RowContext c, PrecisionBand band) {
    final w = readout;
    w
      ..visible = true
      ..compare = false
      ..name = row.label
      ..kind = row.kind
      ..twoD = row.twoD
      ..band = band
      ..bandText = bandLabel(band)
      ..endSide = 0
      ..value = row.text(c);
    if (row.kind == RowKind.oneD || row.twoD == TwoD.curve) {
      w
        ..v = row.value(c)
        ..min = row.min
        ..max = row.max
        ..auto = row.auto
        ..step = row.step;
    }
    w.notify();
  }

  @override
  void adjustEnd({required bool byMultiTouch}) {
    if (_row == null && !dragging.value) return;
    _row = null;
    _m1 = _st = _gx = _gy = _switcher = null;
    _pal = null;
    _wheel = null;
    engine.endInteraction();
    wm.setInteracting(false);
    _bump();
    readout.dimmed = false;
    readout.hideAfter(const Duration(milliseconds: 600));
    readout.notify();
    // The window springs back 150 ms after the finger lifts.
    _undragTimer?.cancel();
    _undragTimer = Timer(const Duration(milliseconds: 150), () {
      dragging.value = false;
    });
    _bump();
  }

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
    if (on) {
      readout.cancelHide();
      readout.low = false;
    }
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
      _bump(); // the cached window card must rebuild to drop the field
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

  // ------------------------------------------------------------ window / row actions

  void openCategory(EGroup g) {
    wm.toggle(g);
    _bump();
  }

  void showReadout(ParamRow row) {
    _publish(row, ctx(), PrecisionBand.standard);
    readout
      ..low = false
      ..dimmed = false;
    readout.hideAfter(const Duration(milliseconds: 600));
  }

  /// Short plain-text readout (e.g. the "n / N" photo counter, 800 ms).
  void showText(String text, {Duration hold = const Duration(milliseconds: 800)}) {
    readout
      ..visible = true
      ..compare = false
      ..name = text
      ..value = ''
      ..bandText = ''
      ..kind = RowKind.action
      ..twoD = null
      ..low = false
      ..dimmed = false;
    readout.hideAfter(hold);
    readout.notify();
  }

  void resetRow(ParamRow row) {
    final c = ctx();
    app.applyEdit(id, (e) => row.resetRow(e, c));
    _bump();
    haptics.fire(HapticKind.autoSnap);
    showReadout(row);
  }

  void resetCategoryOf(EGroup g) {
    if (!edit.groupAdjusted(g)) return;
    app.applyEdit(id, (e) => resetCategory(g, e));
    _bump();
    haptics.fire(HapticKind.autoSnap);
    final pid = id;
    app.showToast('已复位${groupLabels[g]}', actionLabel: '撤销', onAction: () => app.undo(pid));
  }

  /// Taps in the pickers inside the window (swatches, colour dots, channel chips, tone cells).
  void selectDiscrete(ParamRow row, int i) {
    if (row.uiSet != null) {
      row.uiSet!(sel, i);
    } else {
      final c = ctx();
      app.applyEdit(id, (e) => row.writeIndex!(e, i, c));
    }
    _bump();
  }

  void selectTone(String toneId) {
    app.applyEdit(id, (e) => e.copyWith(toneTarget: toneId));
    _bump();
  }

  void performAction(ParamRow row) {
    if (row.act == null) return;
    app.applyEdit(id, row.act!);
    _bump();
  }

  /// Accessibility / keyboard: change the row by [steps] steps ([big] = 5 % of the range).
  void stepRow(ParamRow row, int steps, {bool big = false}) {
    final c = ctx();
    if (row.kind == RowKind.discrete) {
      final i = (row.readIndex!(c) + steps).clamp(0, row.count - 1);
      selectDiscrete(row, i);
      return;
    }
    if (row.write == null) return;
    final d = (big ? row.bigStep : row.step) * steps;
    final v = (row.value(c) + d).clamp(row.min, row.max).toDouble();
    app.applyEdit(id, (e) => row.write!(e, v), coalesce: 'a11y:${row.id}');
    _bump();
    showReadout(row);
  }

  // ------------------------------------------------------------ numeric entry

  void beginNumeric(ParamRow row) {
    if (!row.typeable) return;
    editingRow.value = row.id;
    wm.setInteracting(true);
    _bump();
  }

  /// Commits typed [text] for [row]; empty / invalid = cancel. Returns true when the value was changed.
  bool commitNumeric(ParamRow row, String text) {
    editingRow.value = null;
    wm.setInteracting(false);
    final v = row.parseInput(text);
    if (v == null) return false;
    if (row.inputOutOfRange(text)) app.showToast('已调整到范围内');
    app.applyEdit(id, (e) => row.write!(e, v));
    _bump();
    showReadout(row);
    return true;
  }

  void cancelNumeric() {
    if (editingRow.value == null) return;
    editingRow.value = null;
    wm.setInteracting(false);
    _bump();
  }

  // ------------------------------------------------------------ back

  /// System back, in the spec's order (4.15.6 #24). Returns true when something was dismissed.
  bool handleBack() {
    if (editingRow.value != null) {
      cancelNumeric();
      return true;
    }
    if (docked) {
      // docked panel: first back folds it to its chip strip, the next closes it
      final w = wm.focused;
      if (w != null && !w.collapsed) {
        wm.setCollapsed(w.group, true);
        return true;
      }
    }
    if (wm.closeLatest()) {
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
    _idleTimer?.cancel();
    _undragTimer?.cancel();
    readout.dispose();
    edits.dispose();
    dragging.dispose();
    compare.dispose();
    immersive.dispose();
    editingRow.dispose();
    fine2D.dispose();
    super.dispose();
  }
}
