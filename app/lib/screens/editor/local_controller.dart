// State and intents of 局部 (docs/design/local-adjustments.md): which area and which of its parts is selected, what the panel body
// shows, the photo gestures of the "editing an area" mode (handles, painting, picking), the brush batching, the red overlay and the
// pad / item sources that let the SAME pad, strip, radar and ruler drive an area. Owned by [EditorController] (`ctl.local`).
//
// Undo: one step per gesture (a handle drag, a stroke, a pad drag, a pick, a menu action, a new area): continuous gestures run
// between ctl.gestureBegin / gestureEnd, which coalesce every write into one step.
import 'dart:async';
import 'dart:convert';
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/scheduler.dart';
import 'package:flutter/widgets.dart';

import '../../editor/haptics.dart';
import '../../editor/mask_geometry.dart';
import '../../model/local_adjust.dart';
import '../../model/local_kinds.dart';
import '../../model/local_pad.dart';
import '../../model/pad_model.dart';
import '../../model/params.dart';
import '../../native/local_bindings.dart';
import 'editor_controller.dart';
import 'itemised_view.dart' show ItemSource;
import 'pad_widgets.dart' show PadSource;

/// What the 局部 panel body shows.
enum LocalBody {
  /// The strip and the two pads (or the itemised view).
  pads,

  /// The shape card for a new area.
  addArea,

  /// The shape card for another part of the selected area (add / subtract).
  addPart,
  subPart,

  /// The selected area's actions (加一块 / 减一块 / 反选 / 复制 / 删除).
  actions,
}

/// What a finger on the photo is moving.
enum _Grab { none, linKnob, linMid, lin0, lin1, linNew, radKnob, radSize, radRing, radMove, radNew, brush, tap }

class LocalController extends ChangeNotifier {
  LocalController(this.ctl) {
    ctl.app.editTick.addListener(_onEdit);
    ctl.wm.addListener(_onWm);
  }

  final EditorController ctl;

  // ------------------------------------------------------------ selection and panel state

  String? selId;
  int selComp = 0;
  LocalBody body = LocalBody.pads;

  /// Itemised view group: light, color, detail.
  String itemGroup = 'light';

  /// Brush tool (screen dp radii, so a zoomed-in photo gets a finer brush).
  int brushSize = 1;
  bool erase = false;
  static const List<double> brushRadiiDp = [7, 14, 26, 44];

  bool get active => ctl.wm.focused?.group == EGroup.local;
  LocalList get list => ctl.edit.local;

  int get selIndex => selId == null ? -1 : list.indexOf(selId!);
  LocalAdjustment? get sel {
    final i = selIndex;
    return i < 0 ? null : list.items[i];
  }

  MaskComponent? get comp {
    final a = sel;
    if (a == null || a.mask.isEmpty) return null;
    return a.mask[selComp.clamp(0, a.mask.length - 1)];
  }

  LocalKind? get compKind {
    final c = comp;
    return c == null ? null : kindOfComponent(c);
  }

  /// Keeps a valid selection: the last area when nothing (valid) is selected.
  void _ensureSel() {
    final l = list;
    if (l.isEmpty) {
      selId = null;
      selComp = 0;
      return;
    }
    if (selId == null || l.indexOf(selId!) < 0) {
      selId = l.items.last.id;
      selComp = 0;
    }
    final a = sel!;
    if (selComp >= a.mask.length) selComp = math.max(0, a.mask.length - 1);
  }

  void select(String id) {
    if (selId == id && body == LocalBody.pads) {
      body = LocalBody.actions; // a second tap on the selected chip: its actions
    } else if (selId == id) {
      body = LocalBody.pads;
    } else {
      selId = id;
      selComp = 0;
      body = LocalBody.pads;
    }
    flash();
    _changed();
  }

  void selectPart(int i) {
    if (selComp == i) return;
    selComp = i;
    flash();
    _changed();
  }

  void showBody(LocalBody b) {
    body = b;
    _changed();
  }

  void setItemGroup(String g) {
    if (itemGroup == g) return;
    itemGroup = g;
    ctl.selItem = 0;
    _changed();
  }

  void setBrushSize(int i) {
    brushSize = i.clamp(0, brushRadiiDp.length - 1);
    tick.value++;
    _changed();
  }

  void setErase(bool v) {
    erase = v;
    _changed();
  }

  void _changed() {
    _maskSig = '';
    _onEdit();
    ctl.edits.value++;
    notifyListeners();
  }

  void _toast(String s) => ctl.app.showToast(s);

  // ------------------------------------------------------------ writing the edit

  void _write(LocalList Function(LocalList) f) => ctl.applyGesture((e) => e.copyWith(local: f(e.local)));

  void _writeSel(LocalAdjustment Function(LocalAdjustment) f) {
    final i = selIndex;
    if (i < 0) return;
    _write((l) => l.replace(i, f(l.items[i])));
  }

  void _writeComp(MaskComponent Function(MaskComponent) f) {
    final i = selIndex;
    if (i < 0) return;
    final k = selComp;
    _write((l) {
      final a = l.items[i];
      if (k >= a.mask.length) return l;
      return l.replace(i, a.withComponent(k, f(a.mask[k])));
    });
  }

  int _seq = 0;
  String _newId() => 'a${DateTime.now().millisecondsSinceEpoch.toRadixString(36)}${(_seq++).toRadixString(36)}';

  // ------------------------------------------------------------ areas and parts

  Future<void> addArea(LocalKind k) async {
    final lim = limitFor(list, k);
    if (lim != null) {
      _toast(lim);
      return;
    }
    final c = await _newComponent(k, MaskOp.add);
    if (c == null) return;
    final id = _newId();
    ctl.app.applyEdit(ctl.id, (e) => e.copyWith(local: e.local.add(LocalAdjustment(id: id, mask: [c], ui: LocalUi.centre))));
    selId = id;
    selComp = 0;
    body = LocalBody.pads;
    flash();
    _changed();
  }

  Future<void> addPart(LocalKind k, {required bool subtract}) async {
    final i = selIndex;
    if (i < 0) return;
    final lim = limitFor(list, k, index: i);
    if (lim != null) {
      _toast(lim);
      return;
    }
    final c = await _newComponent(k, opForNew(k, first: false, subtract: subtract));
    if (c == null) return;
    final id = selId;
    ctl.app.applyEdit(ctl.id, (e) {
      final j = id == null ? -1 : e.local.indexOf(id);
      if (j < 0) return e;
      final a = e.local.items[j];
      return e.copyWith(local: e.local.replace(j, a.copyWith(mask: [...a.mask, c])));
    });
    selComp = (sel?.mask.length ?? 1) - 1;
    body = LocalBody.pads;
    flash();
    _changed();
  }

  void removePart(int k) {
    final a = sel;
    if (a == null || a.mask.length < 2 || k >= a.mask.length) return;
    final mask = [...a.mask]..removeAt(k);
    // the first part always adds
    if (k == 0) mask[0] = mask[0].copyWith(op: MaskOp.add);
    ctl.app.applyEdit(ctl.id, (e) => e.copyWith(local: e.local.replace(selIndex, a.copyWith(mask: mask))));
    selComp = math.min(selComp, mask.length - 1);
    _changed();
  }

  void invertArea() {
    final a = sel;
    if (a == null) return;
    ctl.app.applyEdit(ctl.id, (e) => e.copyWith(local: e.local.replace(selIndex, a.copyWith(invert: !a.invert))));
    body = LocalBody.pads;
    flash();
    _changed();
  }

  void duplicateArea() {
    final a = sel;
    if (a == null) return;
    if (list.length >= kMaxLocalAdjustments) {
      _toast('最多 $kMaxLocalAdjustments 个区域');
      return;
    }
    final brushes = a.mask.where((c) => c.kind == MaskKindId.brush).length;
    if (brushes > 0 && list.countKind(MaskKindId.brush) + brushes > kMaxBrushComponents) {
      _toast('画笔最多 $kMaxBrushComponents 个');
      return;
    }
    final id = _newId();
    final i = selIndex;
    ctl.app.applyEdit(ctl.id, (e) => e.copyWith(local: e.local.insert(i + 1, a.copyWith(id: id))));
    selId = id;
    body = LocalBody.pads;
    flash();
    _changed();
  }

  void deleteArea() {
    final i = selIndex;
    if (i < 0) return;
    final pid = ctl.id;
    ctl.app.applyEdit(pid, (e) => e.copyWith(local: e.local.removeAt(i)));
    selId = null;
    body = LocalBody.pads;
    _ensureSel();
    _changed();
    ctl.app.showToast('已删除区域', actionLabel: '撤销', onAction: () => ctl.app.undo(pid));
  }

  /// The selected area's adjustment back to nothing (the shape stays).
  void resetOps() {
    final a = sel;
    if (a == null || !localOpsAdjusted(a)) return;
    ctl.app.applyEdit(ctl.id, (e) => e.copyWith(local: e.local.replace(selIndex, resetLocalOps(a))));
    ctl.haptics.fire(HapticKind.autoSnap);
    _changed();
  }

  Future<MaskComponent?> _newComponent(LocalKind k, MaskOp op) async {
    final s = space;
    final w = s.size.width, h = s.size.height;
    switch (k.place) {
      case LocalPlace.handles:
        if (k.engine == MaskKindId.linear) {
          return linearFrom(s, MaskComponent(kind: MaskKindId.linear, op: op), Offset(w / 2, h * 0.16), Offset(w / 2, h * 0.55));
        }
        return radialFrom(s, MaskComponent(kind: MaskKindId.radial, op: op, feather: 50), Offset(w / 2, h / 2), math.min(w, h) * 0.32);
      case LocalPlace.paint:
        return MaskComponent(kind: MaskKindId.brush, op: op);
      case LocalPlace.sample:
        final smp = await _sampleAt(s.photo(Offset(w / 2, h / 2)));
        if (k.engine == MaskKindId.luminance) {
          final l = smp?.l ?? 70;
          final lo = (l - 15).clamp(0.0, 70.0).toDouble();
          return MaskComponent(kind: MaskKindId.luminance, op: op, lo: lo.roundToDouble(), hi: (lo + 30).roundToDouble(), lumFeather: 10);
        }
        return MaskComponent(kind: MaskKindId.color, op: op, hue: (smp?.hue ?? 230).roundToDouble(), range: 25, hueFeather: 20, satMin: 10);
      case LocalPlace.external:
        return null; // a later round: the provider makes the raster plane
    }
  }

  // ------------------------------------------------------------ geometry

  FrameMap? _map;
  String _mapSig = '';
  int _mapGen = 0;

  /// The frame size the handle layer was laid out at (dp at fit zoom).
  Size frameSize = Size.zero;

  /// Zoom factor of the photo (handles keep their screen size).
  double zoomScale = 1;

  double get _photoAspect {
    final p = ctl.app.photoById(ctl.id);
    return p?.ratio ?? 1.5;
  }

  FrameMap get map => _map ?? _fallbackMap();

  FrameMap _fallbackMap() {
    final e = ctl.edit;
    final pa = _photoAspect;
    final turned = (e.rot ~/ 90).isOdd ? 1 / pa : pa;
    var crop = Rect01.full;
    if (e.crop != 'orig') {
      final sp = e.crop.split(':');
      final ar = double.parse(sp[0]) / double.parse(sp[1]);
      crop = ar < turned ? Rect01((1 - ar / turned) / 2, 0, ar / turned, 1) : Rect01(0, (1 - turned / ar) / 2, 1, turned / ar);
    }
    return FrameMap.compose(photoAspect: pa, rot: e.rot, flip: e.flip, crop: crop);
  }

  HandleSpace get space {
    var s = frameSize;
    if (s.isEmpty) {
      final m = map;
      // the frame's aspect from the map (frame px are square)
      final fx = m.toLE(m.toPhoto(const Offset(1, 0))) - m.toLE(m.toPhoto(Offset.zero));
      final fy = m.toLE(m.toPhoto(const Offset(0, 1))) - m.toLE(m.toPhoto(Offset.zero));
      s = Size(1000, 1000 * fy.distance / math.max(1e-9, fx.distance));
    }
    return HandleSpace(map, s);
  }

  String _geoSig() {
    final e = ctl.edit;
    return '${ctl.id}|${e.crop}|${e.rot}|${e.flip}|${e.value('straighten')}';
  }

  void _refreshMap() {
    final sig = _geoSig();
    if (sig == _mapSig) return;
    _mapSig = sig;
    _map = null;
    final gen = ++_mapGen;
    final p = ctl.app.photoById(ctl.id);
    if (p == null || !ctl.app.sessions.available) return;
    final e = ctl.edit;
    final geo = <String, Object?>{
      if (e.crop != 'orig') 'crop': e.crop,
      if (e.rot != 0) 'rot': e.rot,
      if (e.flip) 'flip': true,
      if (e.value('straighten') != 0) 'straighten': e.value('straighten'),
    };
    () async {
      final s = await ctl.app.sessions.get(p);
      if (s == null || gen != _mapGen) return;
      final m = await LocalNative.frameMap(s.address, jsonEncode(geo), '{}');
      if (gen != _mapGen || m == null) return;
      _map = m;
      tick.value++;
    }();
  }

  // ------------------------------------------------------------ range sampling

  Future<MaskSample?> _sampleAt(Offset uv) async {
    final p = ctl.app.photoById(ctl.id);
    if (p == null || !ctl.app.sessions.available) return null;
    try {
      final s = await ctl.app.sessions.get(p);
      if (s == null) return null;
      return await LocalNative.sample(s.address, jsonEncode(ctl.app.wireParams(p)), uv.dx, uv.dy);
    } catch (_) {
      return null;
    }
  }

  /// Last place a range was picked (frame dp), shown as a small ring.
  Offset? pickedAt;

  Future<void> pick(Offset local) async {
    final c = comp;
    if (c == null || (c.kind != MaskKindId.luminance && c.kind != MaskKindId.color)) return;
    final smp = await _sampleAt(space.photo(local));
    pickedAt = local;
    tick.value++;
    if (smp == null) return;
    if (c.kind == MaskKindId.luminance) {
      final w = (c.hi - c.lo).clamp(4.0, 100.0);
      final lo = (smp.l - w / 2).clamp(0.0, 100.0 - w).toDouble();
      _writeComp((m) => m.copyWith(lo: lo.roundToDouble(), hi: (lo + w).roundToDouble()));
    } else {
      _writeComp((m) => m.copyWith(hue: smp.hue.roundToDouble()));
    }
    ctl.haptics.fire(HapticKind.detent);
    flash();
    ctl.edits.value++;
  }

  /// The tool strip's range bars (inside a gesture).
  void rangeBegin() {
    ctl.gestureBegin('local-range', param: null);
    shaping = true;
    _updateVis();
  }

  void setLum(double lo, double hi) {
    final a = math.min(lo, hi), b = math.max(lo, hi);
    _writeComp((m) => m.copyWith(lo: a.clamp(0.0, 100.0).roundToDouble(), hi: b.clamp(0.0, 100.0).roundToDouble()));
  }

  void setHue(double hue, double range) {
    final h = ((hue % 360) + 360) % 360;
    _writeComp((m) => m.copyWith(hue: h.roundToDouble(), range: range.clamp(2.0, 180.0).roundToDouble()));
  }

  void rangeEnd() {
    ctl.gestureEnd();
    shaping = false;
    flash();
  }

  // ------------------------------------------------------------ photo gestures (the handle layer feeds these, frame dp)

  _Grab _grab = _Grab.none;
  int _grabIndex = 0;
  MaskComponent? _start;
  Offset _downP = Offset.zero;
  bool _moved = false, _begun = false, _multi = false;
  int _downMs = 0;
  BrushInput? _brush;
  bool _brushCommitted = false, _brushNewStroke = true, _flushScheduled = false;
  double _brushLE = 0.05;

  /// True while a finger moves a shape (the overlay shows).
  bool shaping = false;

  /// The stroke being painted, in frame dp, drawn at once (before the engine has it).
  final List<Offset> liveStroke = [];
  double liveRadius = 10;
  bool liveErase = false;

  /// Repaint of the handle layer (live stroke, map, picked point).
  final ValueNotifier<int> tick = ValueNotifier(0);

  /// Touch target of a handle in screen dp.
  static const double kHit = 24;

  double get _hit => kHit / zoomScale;

  /// One finger landed on the photo.
  void pointerDown(Offset p, int ms) {
    _multi = false;
    final c = comp;
    _grab = _Grab.none;
    if (!active || c == null) return;
    _downP = p;
    _downMs = ms;
    _moved = false;
    _begun = false;
    _start = c;
    switch (c.kind) {
      case MaskKindId.linear:
        _grab = _hitLinear(c, p);
      case MaskKindId.radial:
        _grab = _hitRadial(c, p);
      case MaskKindId.brush:
        _grab = _Grab.brush;
        final s = space;
        final rDp = brushRadiiDp[brushSize];
        _brushLE = (rDp / (s.dpPerLE * zoomScale)).clamp(0.0005, 0.5).toDouble();
        _brush = BrushInput(space: s, radiusLE: _brushLE);
        _brushCommitted = false;
        _brushNewStroke = true;
        liveStroke
          ..clear()
          ..add(p);
        liveRadius = rDp / zoomScale;
        liveErase = erase;
        _brush!.add(p);
      case MaskKindId.luminance:
      case MaskKindId.color:
        _grab = _Grab.tap;
      default:
        _grab = _Grab.none;
    }
  }

  _Grab _hitLinear(MaskComponent c, Offset p) {
    final h = LinearHandles(space, c);
    if ((p - h.knob(56 / zoomScale)).distance <= _hit) return _Grab.linKnob;
    if ((p - h.mid).distance <= _hit) return _Grab.linMid;
    if (LinearHandles.distToLine(p, h.p0, h.along) <= _hit * 0.75) return _Grab.lin0;
    if (LinearHandles.distToLine(p, h.p1, h.along) <= _hit * 0.75) return _Grab.lin1;
    return _Grab.linNew;
  }

  _Grab _hitRadial(MaskComponent c, Offset p) {
    final h = RadialHandles(space, c);
    if ((p - h.knob(28 / zoomScale)).distance <= _hit) return _Grab.radKnob;
    final sh = h.sizeHandles();
    for (var i = 0; i < 4; i++) {
      if ((p - sh[i]).distance <= _hit) {
        _grabIndex = i;
        return _Grab.radSize;
      }
    }
    final e = h.norm(p);
    final ringTol = _hit / math.max(8.0, math.min(h.rxDp, h.ryDp));
    if ((e - 1).abs() <= ringTol) return _Grab.radRing;
    if (e < 1) return _Grab.radMove;
    return _Grab.radNew;
  }

  void pointerMove(Offset p, int ms) {
    if (_multi || _grab == _Grab.none) return;
    final d = p - _downP;
    if (!_moved) {
      final slop = (_grab == _Grab.brush ? 3 : 8) / zoomScale;
      if (d.distance < slop && !(_grab == _Grab.brush && ms - _downMs > 90)) {
        if (_grab == _Grab.brush) _brush?.add(p);
        return;
      }
      if (_grab == _Grab.tap) return; // a drag on a range kind does nothing (a tap picks)
      if ((_grab == _Grab.linNew || _grab == _Grab.radNew) && d.distance < 14 / zoomScale) return;
      _moved = true;
    }
    if (!_begun) {
      _begun = true;
      ctl.gestureBegin('local-shape', param: null);
      shaping = true;
      _updateVis();
    }
    final s = _start!;
    final sp = space;
    switch (_grab) {
      case _Grab.linKnob:
        _writeComp((_) => linearRotated(sp, s, p));
      case _Grab.linMid:
        final h = LinearHandles(sp, s);
        _writeComp((_) => linearFrom(sp, s, h.p0 + d, h.p1 + d));
      case _Grab.lin0:
      case _Grab.lin1:
        final h = LinearHandles(sp, s);
        final ax = h.axis;
        final along = d.dx * ax.dx + d.dy * ax.dy;
        final p0 = _grab == _Grab.lin0 ? h.p0 + ax * along : h.p0, p1 = _grab == _Grab.lin1 ? h.p1 + ax * along : h.p1;
        if ((p1 - p0).distance < 6 / zoomScale) return;
        _writeComp((_) => linearFrom(sp, s, p0, p1));
      case _Grab.linNew:
        _writeComp((_) => linearFrom(sp, s, _downP, p));
      case _Grab.radKnob:
        _writeComp((_) => RadialHandles(sp, s).rotated(p));
      case _Grab.radSize:
        _writeComp((_) => RadialHandles(sp, s).sized(_grabIndex, p));
      case _Grab.radRing:
        _writeComp((_) => RadialHandles(sp, s).feathered(p));
      case _Grab.radMove:
        _writeComp((_) => RadialHandles(sp, s).moved(d));
      case _Grab.radNew:
        _writeComp((_) => radialFrom(sp, s, _downP, d.distance));
      case _Grab.brush:
        _brushCommitted = true;
        liveStroke.add(p);
        _brush?.add(p);
        tick.value++;
        _scheduleFlush();
      case _Grab.tap:
      case _Grab.none:
        break;
    }
  }

  void pointerUp(Offset p, int ms) {
    final g = _grab;
    _grab = _Grab.none;
    if (_multi) {
      _finishGesture();
      return;
    }
    if (g == _Grab.brush) {
      // a short tap paints a dot
      if (!_begun) {
        _begun = true;
        ctl.gestureBegin('local-shape', param: null);
        shaping = true;
      }
      _brushCommitted = true;
      _brush?.finish(p);
      _flush();
      _finishGesture();
      return;
    }
    if (!_moved && g == _Grab.tap) {
      unawaited(pick(p));
      return;
    }
    _finishGesture();
  }

  void pointerCancel() {
    _grab = _Grab.none;
    _finishGesture();
  }

  /// A second finger landed: the photo zooms / pans. A stroke not committed yet is dropped (it was the start of a pinch).
  void secondFinger() {
    _multi = true;
    if (_grab == _Grab.brush && !_brushCommitted) {
      _brush = null;
      liveStroke.clear();
      tick.value++;
    } else if (_grab == _Grab.brush) {
      _flush();
    }
    _grab = _Grab.none;
    _finishGesture();
  }

  void _finishGesture() {
    if (_begun) {
      _begun = false;
      ctl.gestureEnd();
    }
    _brush = null;
    if (shaping) {
      shaping = false;
      flash();
    }
  }

  void _scheduleFlush() {
    if (_flushScheduled) return;
    _flushScheduled = true;
    SchedulerBinding.instance.scheduleFrameCallback((_) {
      _flushScheduled = false;
      _flush();
    });
  }

  /// Hands the points gathered since the last frame to the edit: ONE edit (one params JSON, one render) per frame.
  void _flush() {
    final b = _brush;
    if (b == null || !b.hasPending || !_brushCommitted) return;
    if (!_begun) {
      _begun = true;
      ctl.gestureBegin('local-shape', param: null);
      shaping = true;
      _updateVis();
    }
    final pts = b.take();
    final first = _brushNewStroke;
    _brushNewStroke = false;
    final size = _brushLE, er = liveErase;
    _writeComp((m) => brushAppend(m, pts, newStroke: first, size: size, erase: er));
    debugBrushFlushes++;
  }

  /// Test hook: edits written by the brush (one per frame at most).
  static int debugBrushFlushes = 0;

  // ------------------------------------------------------------ overlay (red mask)

  /// The mask of the selected area, from the engine, tinted (null until it arrives).
  final ValueNotifier<ui.Image?> overlay = ValueNotifier(null);

  /// Whether the overlay is shown now.
  final ValueNotifier<bool> overlayOn = ValueNotifier(false);

  /// The overlay image is of an older shape than the one on screen (it is hidden then: a red mask in the wrong place is worse than
  /// none; the handles and the live render show the shape meanwhile).
  final ValueNotifier<bool> overlayStale = ValueNotifier(true);

  /// The selected area's chip is held down (show the mask).
  bool peek = false;
  bool padDragging = false;
  Timer? _visTimer;
  int _showUntil = 0;
  String _maskSig = '';
  bool _inflight = false, _dirty = false;
  int _lastReqMs = 0;
  Timer? _reqTimer;
  String _overlayFor = '';

  /// Device pixels of the frame's long edge (the overlay's resolution).
  double frameDevicePx = 1024;

  int get _now => DateTime.now().millisecondsSinceEpoch;

  /// Shows the overlay for a moment (selection, a new area, after a shape gesture).
  void flash([int ms = 1200]) {
    _showUntil = _now + ms;
    _visTimer?.cancel();
    _visTimer = Timer(Duration(milliseconds: ms + 20), _updateVis);
    _updateVis();
  }

  void setPeek(bool v) {
    if (peek == v) return;
    peek = v;
    _updateVis();
  }

  void _updateVis() {
    final a = sel;
    final want = active && a != null && !padDragging && (peek || shaping || _now < _showUntil || !a.hasEffect);
    if (overlayOn.value != want) overlayOn.value = want;
    overlayStale.value = _sigNow() != _overlayFor;
    if (want) _wantOverlay();
  }

  String _sigNow() {
    final a = sel;
    if (a == null) return '';
    return '${ctl.id}|$selIndex|${identityHashCode(a.mask)}|${a.invert}|${_geoSig()}';
  }

  void _wantOverlay() {
    final sig = _sigNow();
    if (sig.isEmpty || sig == _overlayFor) return;
    if (_inflight) {
      _dirty = true;
      return;
    }
    final wait = 80 - (_now - _lastReqMs);
    if (wait > 0) {
      _reqTimer?.cancel();
      _reqTimer = Timer(Duration(milliseconds: wait), _wantOverlay);
      return;
    }
    unawaited(_fetchOverlay(sig));
  }

  /// Test hook: overlay requests started.
  int debugOverlayRequests = 0;

  Future<void> _fetchOverlay(String sig) async {
    final p = ctl.app.photoById(ctl.id);
    final idx = selIndex;
    if (p == null || idx < 0 || !ctl.app.sessions.available || !LocalBindings.available) return;
    _inflight = true;
    _lastReqMs = _now;
    debugOverlayRequests++;
    try {
      final s = await ctl.app.sessions.get(p);
      if (s == null) return;
      final pj = jsonEncode(ctl.app.wireParams(p));
      final px = await LocalNative.overlay(s.address, pj, idx, frameDevicePx.clamp(256, 1024).round());
      if (px == null) return;
      final c = Completer<ui.Image>();
      ui.decodeImageFromPixels(px.rgba, px.width, px.height, ui.PixelFormat.rgba8888, c.complete);
      final img = await c.future;
      if (_sigNow() != sig && !_dirty) {
        // the selection moved on meanwhile; still better than nothing for the same area
      }
      overlay.value?.dispose();
      overlay.value = img;
      _overlayFor = sig;
      overlayStale.value = _sigNow() != sig;
      if (!shaping && liveStroke.isNotEmpty) {
        liveStroke.clear(); // the engine's mask now has the stroke
        tick.value++;
      }
    } catch (_) {
      // overlay is a hint; a failure leaves the previous one
    } finally {
      _inflight = false;
      if (_dirty) {
        _dirty = false;
        _wantOverlay();
      }
    }
  }

  // ------------------------------------------------------------ listeners

  bool _wasActive = false;

  void _onWm() {
    final a = active;
    if (a != _wasActive) {
      _wasActive = a;
      body = LocalBody.pads;
      if (a) {
        _ensureSel();
        _refreshMap();
        flash();
      } else {
        overlayOn.value = false;
        _endAll();
      }
    }
  }

  void _endAll() {
    if (_begun) _finishGesture();
    liveStroke.clear();
  }

  void _onEdit() {
    if (!active) return;
    _ensureSel();
    _refreshMap();
    final sig = _sigNow();
    if (sig != _maskSig) {
      _maskSig = sig;
      if (overlayOn.value) _wantOverlay();
    }
    _updateVis();
  }

  /// Photo switched or undo: selection is re-validated.
  void photoChanged() {
    selId = null;
    selComp = 0;
    body = LocalBody.pads;
    _mapSig = '';
    _overlayFor = '';
    overlay.value?.dispose();
    overlay.value = null;
    pickedAt = null;
    _onEdit();
  }

  /// Back key: a card in the panel closes first.
  bool handleBack() {
    if (active && body != LocalBody.pads) {
      showBody(LocalBody.pads);
      return true;
    }
    return false;
  }

  // ------------------------------------------------------------ pad and item sources

  late final LocalPadSource lightPad = LocalPadSource(this, PadCat.light);
  late final LocalPadSource colorPad = LocalPadSource(this, PadCat.color);
  late final LocalItemSource items = LocalItemSource(this);

  /// The itemised view's items of [group] (light, color, detail).
  List<PadItem> itemsOf(String group) => [
        for (final s in localOpSpecs)
          if (s.group == group)
            PadItem(s.key, s.label, s.min, s.max, 0, (v) => s.key == 'exposure' ? fmtValue(Fmt.ev, v) : (s.min < 0 ? fmtValue(Fmt.signed, v) : fmtValue(Fmt.plain, v))),
      ];

  @override
  void dispose() {
    ctl.app.editTick.removeListener(_onEdit);
    ctl.wm.removeListener(_onWm);
    _visTimer?.cancel();
    _reqTimer?.cancel();
    overlay.value?.dispose();
    overlay.dispose();
    overlayOn.dispose();
    overlayStale.dispose();
    tick.dispose();
    super.dispose();
  }
}

/// The light or colour pad of the selected area.
class LocalPadSource extends PadSource {
  LocalPadSource(this.lc, this.cat);
  final LocalController lc;
  final PadCat cat;

  LocalUi get _ui {
    final a = lc.sel;
    return a == null ? LocalUi.centre : localUiOf(a);
  }

  @override
  Listenable get changes => Listenable.merge([lc.ctl.app.editTick, lc]);

  @override
  bool get enabled => lc.sel != null;

  @override
  PadState get view {
    final u = _ui;
    return cat == PadCat.light ? PadState(x: u.lx, y: u.ly, a: u.a) : PadState(x: u.cx, y: u.cy, a: u.a);
  }

  @override
  PadLimits get limits => PadLimits.full;

  @override
  void begin() {
    lc.ctl.gestureBegin('local-pad-${cat.name}', param: cat == PadCat.light ? 'exposure' : 'temp');
    lc.padDragging = true;
    lc._updateVis();
  }

  @override
  void move(double x, double y) {
    var px = x.clamp(-1.0, 1.0).toDouble(), py = y.clamp(-1.0, 1.0).toDouble();
    if (px.abs() < kPadSnap) px = 0;
    if (py.abs() < kPadSnap) py = 0;
    final u = _ui;
    final before = view;
    if (before.x == px && before.y == py) return;
    if (px == 0 && py == 0) lc.ctl.haptics.fire(HapticKind.autoSnap);
    final n = cat == PadCat.light ? u.copyWith(lx: px, ly: py) : u.copyWith(cx: px, cy: py);
    lc._writeSel((a) => applyLocalUi(a, n));
  }

  @override
  void strip(double a) {
    var v = a.clamp(0.0, 1.0).toDouble();
    if ((v - 0.5).abs() < kStripSnap) v = 0.5;
    final u = _ui;
    if (u.a == v) return;
    if (v == 0.5) lc.ctl.haptics.fire(HapticKind.autoSnap);
    lc._writeSel((x) => applyLocalUi(x, u.copyWith(a: v)));
  }

  @override
  void end() {
    lc.ctl.gestureEnd();
    lc.padDragging = false;
    lc._updateVis();
  }

  @override
  void reset() => lc.resetOps();

  @override
  String get readout => localReadout(_ui);
}

/// The itemised view of the selected area: every local op, the pads' recipe as the dashed polygon.
class LocalItemSource extends ItemSource {
  LocalItemSource(this.lc);
  final LocalController lc;

  @override
  Listenable get changes => Listenable.merge([lc.ctl.app.editTick, lc]);

  @override
  double actual(PadItem it) => lc.sel?.op(it.id) ?? 0;

  @override
  double implied(PadItem it) {
    final a = lc.sel;
    if (a == null) return 0;
    final s = localOpByKey[it.id]!;
    return (localRecipe(localUiOf(a))[it.id] ?? 0).clamp(s.min, s.max).toDouble();
  }

  @override
  void begin(PadItem it) {
    lc.ctl.gestureBegin('local-item-${it.id}', param: it.id);
    lc.padDragging = true;
    lc._updateVis();
  }

  @override
  void set(PadItem it, double v) => lc._writeSel((a) => setLocalItem(a, it.id, v));

  @override
  void end() {
    lc.ctl.gestureEnd();
    lc.padDragging = false;
    lc._updateVis();
  }

  @override
  void reset(PadItem it) {
    begin(it);
    set(it, implied(it));
    end();
  }
}
