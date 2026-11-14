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

import 'package:flutter/foundation.dart' show kDebugMode;
import 'package:flutter/scheduler.dart';
import 'package:flutter/widgets.dart';

import '../../editor/haptics.dart';
import '../../editor/mask_geometry.dart';
import '../../model/local_adjust.dart';
import '../../model/local_kinds.dart';
import '../../model/local_pad.dart';
import '../../model/pad_model.dart';
import '../../model/portrait.dart';
import '../../model/params.dart';
import '../../native/local_bindings.dart';
import '../../models.dart' show Photo;
import '../../services/local_ai.dart';
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

  /// The selected area's actions (加一块 / 减一块 / 反选 / 复制 / 删除; AI areas also 涂抹补充 / 涂抹擦除).
  actions,

  /// 人物: the parts the analysis found, to pick one or several.
  aiParts,

  /// 人像: the portrait chips (also shown whenever a portrait area is selected).
  portrait,
}

/// What a finger on the photo is moving.
enum _Grab { none, linKnob, linMid, lin0, lin1, linNew, radKnob, radSize, radRing, radMove, radNew, brush, tap }

class LocalController extends ChangeNotifier {
  LocalController(this.ctl, {LocalAi? ai}) : ai = ai ?? const NoLocalAi() {
    ctl.app.editTick.addListener(_onEdit);
    ctl.wm.addListener(_onWm);
  }

  final EditorController ctl;

  /// The AI masks service (a fake in tests).
  final LocalAi ai;

  // ------------------------------------------------------------ selection and panel state

  String? _selId;
  String? get selId => _selId;
  set selId(String? v) {
    _selId = v;
    if (v != null) _memory[ctl.id] = v;
  }

  /// The selected area of each photo, kept while the app runs: reopening a photo, or undoing a delete, comes back to the area that
  /// was selected before (else the first portrait chip / the last area).
  static final Expando<Map<String, String>> _remembered = Expando('selected areas');
  Map<String, String> get _memory => _remembered[ctl.app] ??= {};

  /// The selection that went away with a deleted area and what was chosen instead: an undo that brings the area back restores it,
  /// unless the user has chosen something in between.
  String? _lostSel, _autoSel;

  int selComp = 0;
  LocalBody body = LocalBody.pads;

  /// Itemised view group: light, color, detail.
  String itemGroup = 'light';

  /// The pad showing in the default layer of an ordinary area (the 光线 / 颜色 / 质地 switch); kept when another area is selected.
  PadCat padCat = PadCat.light;

  /// The width the header's lines were sized for (the editor page sets it, so the header and the panel height agree on whether the
  /// switch shares the chips line); null = the header measures its own width.
  double? headerInnerW;

  /// Whether the switch is part of the layer now: an ordinary area's pads (not the portrait group, not 分项, not a card).
  bool get showsPadSwitch => body == LocalBody.pads && !ctl.itemised && sel != null && !selIsPortrait;

  /// The pad source of the showing pad.
  LocalPadSource get pad => switch (padCat) {
    PadCat.color => colorPad,
    PadCat.texture => texturePad,
    _ => lightPad,
  };

  void setPadCat(PadCat c) {
    if (padCat == c) return;
    padCat = c;
    _changed();
  }

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
      _selId = null;
      selComp = 0;
      return;
    }
    // an undo brought back the area that was selected before it was deleted (and nothing was chosen since)
    if (_lostSel != null && _selId == _autoSel && l.indexOf(_lostSel!) >= 0) {
      selId = _lostSel;
      _lostSel = _autoSel = null;
      selComp = 0;
    }
    if (_selId == null || l.indexOf(_selId!) < 0) {
      final gone = _selId;
      final back = _memory[ctl.id];
      var pick = back != null && l.indexOf(back) >= 0 ? back : l.items.last.id;
      // portrait chips: the first chip, not the last one that happens to be at the end of the list
      final pa = l.items[l.indexOf(pick)];
      if (portraitOf(pa) != null && !(back != null && pick == back)) {
        pick = l.items.firstWhere((a) => portraitOf(a) != null).id;
      }
      selId = pick;
      selComp = 0;
      if (gone != null) {
        _lostSel = gone;
        _autoSel = pick;
      }
    }
    final a = sel!;
    if (selComp >= a.mask.length) selComp = math.max(0, a.mask.length - 1);
  }

  /// What the action card acts on (audit U3): the selected area (both null), one portrait chip, or the whole 人像 group.
  PortraitChip? actionChip;
  bool actionPortraitGroup = false;

  /// The action card's subtitle: the area, the chip or 人像.
  String get actionsTitle {
    if (actionChip != null) return specOf(actionChip!).label;
    if (actionPortraitGroup) return '人像';
    return selIndex < 0 ? '区域' : '区域 ${selIndex + 1}';
  }

  void select(String id) {
    if (selId == id && (body == LocalBody.pads || body == LocalBody.portrait)) {
      // a second tap on the selected chip: its action card (U3); the 人像 chip stands for the whole group
      actionChip = null;
      actionPortraitGroup = selIsPortrait;
      body = LocalBody.actions;
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
    if (b != LocalBody.actions) {
      actionChip = null;
      actionPortraitGroup = false;
    }
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
    body = LocalBody.pads;
    flash();
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
      _toast('画笔最多 $kMaxBrushComponents 块');
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

  /// 删除 on the 人像 action card: every portrait area goes (one undo step, 撤销 on the toast).
  void deletePortrait() {
    final pid = ctl.id;
    ctl.app.applyEdit(pid, (e) => e.copyWith(local: LocalList([for (final a in e.local.items) if (portraitOf(a) == null) a])));
    healMode = false;
    selId = null;
    actionPortraitGroup = false;
    body = LocalBody.pads;
    _ensureSel();
    _changed();
    ctl.app.showToast('已删除人像', actionLabel: '撤销', onAction: () => ctl.app.undo(pid));
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
          return MaskComponent(kind: MaskKindId.luminance, op: op, lo: lo.roundToDouble(), hi: (lo + 30).roundToDouble(), lumFeather: 15, refine: 50);
        }
        return MaskComponent(kind: MaskKindId.color, op: op, hue: (smp?.hue ?? 230).roundToDouble(), range: 25, hueFeather: 20, satMin: 10);
      case LocalPlace.aiParts:
      case LocalPlace.aiPlane:
      case LocalPlace.aiTap:
      case LocalPlace.aiDepth:
      case LocalPlace.aiPortrait:
        return null; // AI kinds go through chooseKind (the analysis makes their planes)
    }
  }

  // ------------------------------------------------------------ AI kinds (docs/design/local-adjustments.md section 9)

  /// What the device can do (null until asked).
  AiAvailability? avail;
  bool _availAsked = false;

  /// The analysis of the photo now open: which task bits are done, which run, the progress of the running job.
  String _aiPhoto = '';
  int aiDone = 0, aiRunning = 0, _aiQueued = 0;
  double aiProgress = 0;
  StreamSubscription<AiProgress>? _aiSub;
  final Map<String, AiPlane> aiPlanes = {};
  bool personFound = false, skyFound = false;
  int faces = 0;

  /// A tile waiting for the analysis (天空 / 背景 / 远近) and whether it becomes a new area or a part.
  LocalKind? aiPending;
  LocalBody _pendingMode = LocalBody.addArea;

  /// 人物: the part keys picked in the parts card, and where they go.
  final Set<String> partPick = {};
  LocalBody partMode = LocalBody.addArea;

  /// Tap select: add (true) or subtract the next tap; a tap is being computed.
  bool tapAdd = true, tapBusy = false;
  String _tapSessionRef = '';
  Future<void> _tapChain = Future.value();

  /// "ai:" ids of this edit that are not in the raster store (the tool strip shows 重新分析); a repair runs.
  final Set<String> missingRefs = {};
  bool repairing = false;

  int get deviceTasks => (avail?.available ?? false) ? avail!.tasks : 0;

  Set<String> get absentKinds => {
        if ((aiDone & AiBits.person) != 0 && !personFound) ...['background', 'portrait'],
        if ((aiDone & AiBits.sky) != 0 && !skyFound) 'sky',
      };

  /// The kinds the shape card offers now.
  List<LocalKind> get offered => offeredKinds(deviceTasks: deviceTasks, absent: absentKinds);

  /// The tile of [k] shows the progress ring.
  bool runningFor(LocalKind k) =>
      k.ai && aiRunning != 0 && ((aiRunning & k.aiTasks) != 0) && (aiPending?.id == k.id || (k.place == LocalPlace.aiParts && aiPending == null));

  void _askAvail() {
    if (_availAsked) return;
    _availAsked = true;
    ai.prepare().then((a) {
      avail = a;
      _aiNotify();
    });
  }

  void _aiPhotoCheck() {
    if (_aiPhoto == ctl.id) return;
    _faceBoxes.clear();
    _portraitFacesDirty = true;
    healMode = false;
    _aiPhoto = ctl.id;
    cancelAnalysis();
    aiDone = 0;
    aiPlanes.clear();
    personFound = skyFound = false;
    faces = 0;
    faceList = const [];
    _tapSessionRef = '';
    missingRefs.clear();
  }

  /// Starts the analysis of [tasks] (what is not done yet; one job at a time, the rest queued).
  void startAnalysis(int tasks) {
    _aiPhotoCheck();
    final want = tasks & deviceTasks & ~aiDone & ~AiBits.tapPrepare;
    if (want == 0) return;
    if (aiRunning != 0) {
      _aiQueued |= want & ~aiRunning;
      return;
    }
    final p = ctl.app.photoById(ctl.id);
    if (p == null) return;
    aiRunning = want;
    aiProgress = 0;
    _aiNotify();
    _aiSub = ai.run(p, want).listen((e) {
      aiProgress = e.progress;
      final r = e.result;
      if (r != null) _aiFinished(want, r);
      _aiNotify();
    }, onDone: () {
      if (aiRunning == want) _aiFinished(want, null);
    });
  }

  /// Test hook: jobs started.
  int debugAiRuns = 0;

  void _aiFinished(int tasks, AiJobResult? r) {
    _aiSub = null;
    aiRunning = 0;
    debugAiRuns++;
    if (kDebugMode) {
      debugPrint('local ai: tasks $tasks -> ${r?.state} ${r?.totalMs.round()} ms, ${r?.planes.length} planes, person ${r?.personFound}, faces ${r?.faces}, sky ${r?.skyFound}, '
          'groups ${r?.groups.map((g) => '${g.group}:${g.state}:${g.ms.round()}').join(' ')}');
    }
    if (r != null && r.state != 'cancelled' && r.state != 'error') {
      aiDone |= tasks;
      for (final pl in r.planes) {
        aiPlanes[pl.key] = pl;
      }
      if ((tasks & AiBits.person) != 0) personFound = r.personFound;
      if ((tasks & AiBits.faces) != 0) {
        faces = r.faces;
        if (r.faceList.isNotEmpty) faceList = r.faceList;
      }
      if ((tasks & AiBits.sky) != 0) skyFound = r.skyFound;
    } else if (r != null && r.state == 'error') {
      // what happened plus the step that can help (audit F21 / U9): the same analysis once more
      final again = aiPending, mode = _pendingMode, portrait = portraitWaiting;
      ctl.app.showToast('没能分析这张照片', actionLabel: '重试', onAction: () {
        if (again != null) {
          chooseKind(again, mode);
        } else if (portrait) {
          openPortrait();
        } else {
          startAnalysis(tasks);
        }
      });
      aiPending = null;
    }
    if (r != null && (tasks & (AiBits.person | AiBits.faces)) != 0) _portraitFacesDirty = true;
    if (portraitWaiting && (aiDone & (AiBits.person | AiBits.faces)) == (AiBits.person | AiBits.faces)) {
      portraitWaiting = false;
      _portraitReady();
    }
    final pending = aiPending;
    if (pending != null && (aiDone & pending.aiTasks) == pending.aiTasks) {
      aiPending = null;
      _createAiPlane(pending, _pendingMode);
    }
    final q = _aiQueued;
    _aiQueued = 0;
    if (q != 0) startAnalysis(q);
    _aiNotify();
  }

  void cancelAnalysis() {
    final had = _aiSub != null;
    _aiSub?.cancel();
    _aiSub = null;
    aiRunning = 0;
    _aiQueued = 0;
    aiPending = null;
    if (had) _aiNotify();
  }

  void _aiNotify() {
    notifyListeners();
    ctl.edits.value++;
  }

  /// A tile of the shape card: [mode] addArea, addPart or subPart.
  Future<void> chooseKind(LocalKind k, LocalBody mode) async {
    if (!k.ai) {
      if (mode == LocalBody.addArea) return addArea(k);
      return addPart(k, subtract: mode == LocalBody.subPart);
    }
    _aiPhotoCheck();
    if (aiPending?.id == k.id && aiRunning != 0) {
      cancelAnalysis(); // a second tap on a running tile stops it
      return;
    }
    final lim = limitFor(list, k, index: mode == LocalBody.addArea || selIndex < 0 ? null : selIndex);
    if (lim != null) {
      _toast(lim);
      return;
    }
    switch (k.place) {
      case LocalPlace.aiParts:
        partPick.clear();
        partMode = mode;
        body = LocalBody.aiParts;
        startAnalysis(k.aiTasks);
        _changed();
      case LocalPlace.aiPlane:
      case LocalPlace.aiDepth:
        if ((aiDone & k.aiTasks) == k.aiTasks) {
          _createAiPlane(k, mode);
        } else {
          aiPending = k;
          _pendingMode = mode;
          startAnalysis(k.aiTasks);
        }
      case LocalPlace.aiTap:
        _newTapArea(mode);
      case LocalPlace.aiPortrait:
        openPortrait();
      default:
        break;
    }
  }

  // ------------------------------------------------------------ 人像 (docs/design/local-adjustments.md section 9.1)

  /// The portrait panel waits for the person / face analysis.
  bool portraitWaiting = false;
  bool _portraitFacesDirty = true;
  List<PortraitFace> _faces = const [];

  /// Manual heal: taps on the photo add / remove spots of the skin area (rings drawn while on).
  bool healMode = false;

  /// The heal tool's [+|−] switch (audit U2): + adds spots, − removes the one tapped.
  bool healAdd = true;

  void setHealAdd(bool v) {
    if (healAdd == v) return;
    healAdd = v;
    _changed();
  }
  int healSize = 1;

  /// 对比 is held: the photo without any local area.
  bool comparing = false;

  /// The engine's face list of the last analysis (`face_list`: iod, mouth, teeth per face); empty with an older engine or cache.
  List<AiFace> faceList = const [];

  /// The faces of the analysis with their scale: the engine's iod from `face_list`; only when that is missing (an older cache) the
  /// app's estimate from the eye / iris planes.
  List<PortraitFace> get portraitFaces {
    if (_portraitFacesDirty) {
      _portraitFacesDirty = false;
      final base = facesOf(aiPlanes.keys);
      final iods = <int, double>{};
      final byIndex = {for (final f in faceList) f.index: f.iod};
      for (final f in base) {
        final known = byIndex[f.n] ?? 0;
        iods[f.n] = known > 0 ? known : _iodOf(f);
      }
      _faces = facesOf(aiPlanes.keys, iods: iods);
    }
    return _faces;
  }

  double _iodOf(PortraitFace f) {
    for (final k in const ['iris', 'eyes']) {
      final pl = aiPlanes['f${f.n}:$k'];
      final pv = pl == null ? null : ai.preview(pl.id, longEdge: 512);
      if (pv != null) {
        final v = iodFromPlane(pv.rgba, pv.width, pv.height);
        if (v > 0) return v;
      }
    }
    final pl = aiPlanes['f${f.n}:face'];
    final pv = pl == null ? null : ai.preview(pl.id, longEdge: 256);
    return pv == null ? 0 : iodFromFaceOval(pv.rgba, pv.width, pv.height);
  }

  /// Faces with eye features (the face selector appears when there are several).
  List<PortraitFace> get frontalFaces => [for (final f in portraitFaces) if (f.frontal) f];

  List<PortraitChip> get portraitChips => availableChips(aiPlanes.keys.toSet(), portraitFaces);

  bool get portraitAnalysed => (aiDone & (AiBits.person | AiBits.faces)) == (AiBits.person | AiBits.faces);

  /// The chip of the selected area (null: not a portrait area).
  PortraitChip? get selChip {
    final a = sel;
    return a == null ? null : portraitOf(a)?.$1.chip;
  }

  bool get selIsPortrait => selChip != null;

  bool chipOn(PortraitChip c) => specOf(c).areas.any((ar) => list.indexOf(ar.id) >= 0);

  /// 人像 on the shape card: the analysis once, then the chips; the first time the mild default set is applied (one undo step).
  void openPortrait() {
    body = LocalBody.portrait;
    if (portraitAnalysed) {
      _portraitReady();
    } else {
      portraitWaiting = true;
      startAnalysis(AiBits.person | AiBits.faces);
    }
    _changed();
  }

  void _portraitReady() {
    if (!personFound) {
      _aiNotify();
      return;
    }
    final any = portraitSpecs.any((s) => chipOn(s.chip));
    if (!any) {
      final chips = [for (final c in portraitChips) if (specOf(c).onByDefault) c];
      _setChips(chips, fill: true);
    }
    _aiNotify();
  }

  String? _planeId(String key) => aiPlanes[key]?.id;

  /// An off chip whose areas would not fit the edit's 12 raster planes (a group photo: 眼睛 is two planes per face). The default set
  /// leaves such a chip off by design (priority order, [kPortraitPriority]); the chip says why (`块数已满`) and a tap shows the toast.
  bool chipBlocked(PortraitChip c) {
    if (chipOn(c)) return false;
    final fs = portraitFaces;
    final all = {for (final f in fs) f.n};
    var u = list;
    var any = false;
    for (final ar in specOf(c).areas) {
      final mask = portraitMask(ar, fs, all, _planeId);
      if (mask.isEmpty || u.indexOf(ar.id) >= 0) continue;
      any = true;
      u = u.add(LocalAdjustment(id: ar.id, mask: mask));
    }
    return any && (u.rasterPlanes > kMaxRasterComponents || u.length > kMaxLocalAdjustments);
  }

  /// Creates (or re-links, same ids: never duplicates) the areas of [chips] for [faces] (null = every face). One undo step.
  ///
  /// [fill]: the default set; chips are added in [kPortraitPriority] order while the edit's 12 planes last (a group photo gets skin,
  /// shine, under-eye ... before the per-face extras), and what does not fit is left off.
  bool _setChips(List<PortraitChip> chips, {Set<int>? faces, bool fill = false}) {
    if (chips.isEmpty) return false;
    final fs = portraitFaces;
    final all = {for (final f in fs) f.n};
    if (fill) {
      // which chips fit, filled by priority; then made in the card's order (one edit)
      var t = list;
      final fits = <PortraitChip>{};
      for (final c in kPortraitPriority) {
        if (!chips.contains(c)) continue;
        var u = t;
        for (final ar in specOf(c).areas) {
          final mask = portraitMask(ar, fs, faces ?? all, _planeId);
          if (mask.isEmpty || u.indexOf(ar.id) >= 0) continue;
          u = u.add(LocalAdjustment(id: ar.id, mask: mask));
        }
        if (u.rasterPlanes <= kMaxRasterComponents && u.length <= kMaxLocalAdjustments) {
          t = u;
          fits.add(c);
        }
      }
      if (fits.length < chips.length) _toast('人物、天空、点选最多 $kMaxRasterComponents 块');
      return _setChips([for (final c in chips) if (fits.contains(c)) c], faces: faces);
    }
    var l = list;
    String? firstId;
    var skipped = 0;
    final order = chips;
    for (final c in order) {
      final before = l;
      final firstBefore = firstId;
      for (final ar in specOf(c).areas) {
        final sel = faces ?? all;
        final mask = portraitMask(ar, fs, sel, _planeId);
        if (mask.isEmpty) continue;
        final face = portraitFaceScale(fs, sel);
        final i = l.indexOf(ar.id);
        if (i >= 0) {
          l = l.replace(i, l.items[i].copyWith(mask: mask, face: face, on: true));
        } else {
          if (l.length >= kMaxLocalAdjustments) {
            _toast('最多 $kMaxLocalAdjustments 个区域');
            return false;
          }
          l = l.add(applyLocalUi(LocalAdjustment(id: ar.id, name: portraitName(ar.id), mask: mask, face: face), LocalUi.centre));
        }
        firstId ??= ar.id;
      }
      if (fill && l.rasterPlanes > kMaxRasterComponents) {
        l = before; // this chip does not fit: the ones before it stay
        firstId = firstBefore;
        skipped++;
      }
    }
    if (l.rasterPlanes > kMaxRasterComponents || identical(l, list)) {
      if (!identical(l, list) || skipped > 0) _toast('人物、天空、点选最多 $kMaxRasterComponents 块');
      return false;
    }
    final next = l;
    ctl.app.applyEdit(ctl.id, (e) => e.copyWith(local: next));
    if (firstId != null) {
      selId = firstId;
      selComp = 0;
    }
    flash();
    _changed();
    return true;
  }

  /// A chip (audit U3): off -> its areas appear (and the first is selected); on but another chip selected -> select it; on and selected
  /// -> its action card (关掉 is there, with 撤销 on the toast; a tap never removes anything).
  void tapChip(PortraitChip c) {
    final spec = specOf(c);
    if (!chipOn(c)) {
      _setChips([c]);
      return;
    }
    if (selChip != c) {
      selId = spec.areas.firstWhere((ar) => list.indexOf(ar.id) >= 0).id;
      selComp = 0;
      flash();
      _changed();
      return;
    }
    actionChip = c;
    actionPortraitGroup = false;
    body = LocalBody.actions;
    _changed();
  }

  /// 关掉 on a portrait chip's action card: the chip's areas go; the chip stays in the panel, off.
  void turnOffChip(PortraitChip c) {
    final spec = specOf(c);
    final ids = {for (final ar in spec.areas) ar.id};
    final pid = ctl.id;
    ctl.app.applyEdit(pid, (e) => e.copyWith(local: LocalList([for (final a in e.local.items) if (!ids.contains(a.id)) a])));
    if (healMode && c == PortraitChip.skin) healMode = false;
    actionChip = null;
    body = LocalBody.portrait;
    _changed();
    ctl.app.showToast('已关掉${spec.label}', actionLabel: '撤销', onAction: () => ctl.app.undo(pid));
  }

  /// The face selector: the selected chip applies to face [n] or not (at least one face stays).
  void toggleFace(int n) {
    final c = selChip, a = sel;
    if (c == null || a == null) return;
    final cur = facesOfMask(a, portraitFaces);
    final next = {...cur};
    if (!next.remove(n)) next.add(n);
    if (next.isEmpty) return;
    _setChips([c], faces: next);
  }

  Set<int> get selFaces {
    final a = sel;
    return a == null ? const {} : facesOfMask(a, portraitFaces);
  }

  final Map<int, Rect?> _faceBoxes = {};

  /// Where face [n] is in the picture on screen (frame-normalised, a little wider than the oval), for its thumbnail.
  Rect? faceBox(int n) {
    if (_faceBoxes.containsKey(n)) return _faceBoxes[n];
    final pl = aiPlanes['f$n:face'];
    final pv = pl == null ? null : ai.preview(pl.id, longEdge: 128);
    Rect? r;
    if (pv != null) {
      var x0 = pv.width, y0 = pv.height, x1 = -1, y1 = -1;
      for (var y = 0; y < pv.height; y++) {
        for (var x = 0; x < pv.width; x++) {
          if (pv.rgba[(y * pv.width + x) * 4] < 96) continue;
          x0 = math.min(x0, x);
          x1 = math.max(x1, x);
          y0 = math.min(y0, y);
          y1 = math.max(y1, y);
        }
      }
      if (x1 > x0 && y1 > y0) {
        final a = map.toFrame(Offset(x0 / pv.width, y0 / pv.height)), b = map.toFrame(Offset((x1 + 1) / pv.width, (y1 + 1) / pv.height));
        final box = Rect.fromPoints(a, b);
        final side = math.max(box.width, box.height) * 1.2;
        r = Rect.fromCenter(center: box.center, width: side, height: side);
      }
    }
    return _faceBoxes[n] = r;
  }

  // ---- manual heal

  void setHealMode(bool v) {
    healMode = v;
    tick.value++;
    _changed();
  }

  void setHealSize(int i) {
    healSize = i.clamp(0, 3);
    _changed();
  }

  bool get _healing => healMode && sel != null && portraitOf(sel!)?.$2.id == 'pt-skin';

  /// A tap in heal mode: on a spot it removes it, elsewhere it adds one (radius from the face). One undo step.
  void healTap(Offset local) {
    final a = sel;
    if (a == null) return;
    final s = space;
    final uv = s.photo(local);
    final le = s.map.toLE(uv);
    final spots = [...a.spots];
    final hit = spots.lastIndexWhere((h) => (s.map.toLE(Offset(h.x, h.y)) - le).distance <= math.max(h.r, 8 / (s.dpPerLE * zoomScale)));
    // U2: the [+|−] switch decides: + always adds a spot (a tap on an existing one adds another), − removes the spot under the finger
    if (!healAdd) {
      if (hit < 0) return;
      spots.removeAt(hit);
    } else {
      if (spots.length >= kMaxHealSpots) {
        _toast('最多 $kMaxHealSpots 个祛斑点');
        return;
      }
      spots.add(HealSpot(uv.dx, uv.dy, healRadius(a.face, healSize)));
    }
    final i = selIndex;
    ctl.app.applyEdit(ctl.id, (e) => e.copyWith(local: e.local.replace(i, e.local.items[i].copyWith(spots: spots))));
    ctl.haptics.fire(HapticKind.detent);
    tick.value++;
    _changed();
  }

  // ---- before / after

  /// 对比 held: the photo renders without every local area (all areas together: a retouch is the sum of its chips).
  void setComparing(bool v) {
    if (comparing == v) return;
    comparing = v;
    ctl.engine.hideLocal = v;
    ctl.engine.edited();
    if (v) overlayOn.value = false;
    tick.value++;
    _updateVis();
    notifyListeners();
  }

  MaskComponent _raster(String id, MaskOp op, {bool depth = false}) =>
      MaskComponent(kind: MaskKindId.raster, op: op, ref: id, refine: 50, lo: 0, hi: depth ? 30 : 100, lumFeather: 10);

  /// Adds [comps] as a new area ([mode] addArea) or as parts of the selected one. One undo step.
  void _addComps(List<MaskComponent> comps, LocalBody mode, {bool on = true}) {
    if (comps.isEmpty) return;
    if (mode == LocalBody.addArea || selIndex < 0) {
      final id = _newId();
      ctl.app.applyEdit(ctl.id, (e) => e.copyWith(local: e.local.add(LocalAdjustment(id: id, on: on, mask: comps, ui: LocalUi.centre))));
      selId = id;
      selComp = 0;
    } else {
      final i = selIndex, a = sel!;
      ctl.app.applyEdit(ctl.id, (e) => e.copyWith(local: e.local.replace(i, a.copyWith(mask: [...a.mask, ...comps]))));
      selComp = a.mask.length;
    }
    body = LocalBody.pads;
    flash();
    _changed();
  }

  void _createAiPlane(LocalKind k, LocalBody mode) {
    final pl = aiPlanes[k.aiKey];
    if (pl == null) {
      _toast(k.id == 'sky' ? '没找到天空' : (k.id == 'background' ? '没找到人' : '没能分析这张照片'));
      return;
    }
    final first = mode == LocalBody.addArea || sel == null;
    _addComps([_raster(pl.id, opForNew(k, first: first, subtract: mode == LocalBody.subPart), depth: k.place == LocalPlace.aiDepth)], mode);
  }

  /// The parts the analysis found, in the card's order: person parts, then per face its features.
  List<AiPlane> get partPlanes {
    const order = ['person', 'faceskin', 'bodyskin', 'hair', 'clothes'];
    const faceOrder = ['eyes', 'iris', 'sclera', 'lips', 'brows', 'teeth'];
    final out = <AiPlane>[
      for (final k in order)
        if (aiPlanes[k] != null) aiPlanes[k]!,
    ];
    for (var f = 1; f <= 4; f++) {
      for (final k in faceOrder) {
        final pl = aiPlanes['f$f:$k'];
        if (pl != null) out.add(pl);
      }
    }
    return out;
  }

  void togglePart(String key) {
    if (!partPick.remove(key)) {
      final lim = limitFor(list, kindById('person'), index: partMode == LocalBody.addArea || selIndex < 0 ? null : selIndex, n: partPick.length + 1);
      if (lim != null) {
        _toast(lim);
        return;
      }
      partPick.add(key);
    }
    _changed();
  }

  /// The picked parts become one area (or parts of the selected one): their planes added together.
  void addPickedParts() {
    final keys = [for (final pl in partPlanes) if (partPick.contains(pl.key)) pl.key];
    if (keys.isEmpty) return;
    final first = partMode == LocalBody.addArea || sel == null;
    final sub = partMode == LocalBody.subPart;
    final comps = <MaskComponent>[
      for (var i = 0; i < keys.length; i++) _raster(aiPlanes[keys[i]]!.id, first || !sub ? MaskOp.add : MaskOp.subtract),
    ];
    partPick.clear();
    _addComps(comps, partMode);
  }

  // ---- tap select

  /// 点选: a new part whose plane comes with the first tap (an area stays off until then: an export never asks for a plane that does not exist).
  void _newTapArea(LocalBody mode) {
    final p = ctl.app.photoById(ctl.id);
    if (p == null) return;
    tapAdd = true;
    _tapSessionRef = '';
    _tapChain = _tapChain.then((_) => ai.resetTaps(p)).then((_) {});
    ai.prepareTap(p);
    final first = mode == LocalBody.addArea || sel == null;
    final op = first ? MaskOp.add : (mode == LocalBody.subPart ? MaskOp.subtract : MaskOp.add);
    _addComps([MaskComponent(kind: MaskKindId.raster, op: op, ref: '')], mode, on: !first);
  }

  bool get tapPart {
    final c = comp;
    return c != null && c.kind == MaskKindId.raster && kindOfComponent(c).id == 'tap';
  }

  void setTapAdd(bool v) {
    tapAdd = v;
    _changed();
  }

  /// A tap on the photo with a tap-select part selected: the object there is added (or subtracted); one undo step per tap.
  void aiTap(Offset local) {
    final c = comp, p = ctl.app.photoById(ctl.id);
    if (c == null || p == null) return;
    final uv = space.photo(local);
    pickedAt = local;
    tick.value++;
    final add = tapAdd, areaId = selId, k = selComp;
    tapBusy = true;
    _aiNotify();
    _tapChain = _tapChain.then((_) async {
      final ref = _refOf(areaId, k);
      await _tapSessionTo(p, ref);
      final sw = Stopwatch()..start();
      final r = await ai.tap(p, uv.dx, uv.dy, add: add);
      if (kDebugMode) debugPrint('local ai: tap ${sw.elapsedMilliseconds} ms (engine ${r.ms.round()} ms) ok ${r.ok}');
      if (r.ok && r.id.isNotEmpty) {
        _tapSessionRef = r.id;
        _setTapRef(areaId, k, r.id);
      } else {
        _tapSessionRef = '';
        _toast('没选到');
      }
    }).whenComplete(() {
      tapBusy = false;
      _aiNotify();
    });
  }

  /// The engine keeps one tap session per photo: it is put back to [ref]'s taps when another part used it meanwhile.
  Future<void> _tapSessionTo(Photo p, String ref) async {
    if (_tapSessionRef == ref) return;
    await ai.resetTaps(p);
    for (final t in tapsOfId(ref)) {
      await ai.tap(p, t.$1, t.$2, add: t.$3);
    }
    _tapSessionRef = ref;
  }

  String _refOf(String? areaId, int k) {
    if (areaId == null) return '';
    final a = list.byId(areaId);
    return a == null || k >= a.mask.length ? '' : a.mask[k].ref;
  }

  /// Takes the last tap back (one undo step).
  void undoTap() {
    final c = comp, p = ctl.app.photoById(ctl.id);
    if (c == null || p == null || c.ref.isEmpty) return;
    final areaId = selId, k = selComp;
    tapBusy = true;
    _aiNotify();
    _tapChain = _tapChain.then((_) async {
      await _tapSessionTo(p, _refOf(areaId, k));
      final r = await ai.undoTap(p);
      _tapSessionRef = r.id;
      _setTapRef(areaId, k, r.id);
    }).whenComplete(() {
      tapBusy = false;
      _aiNotify();
    });
  }

  void _setTapRef(String? areaId, int k, String id) {
    if (areaId == null) return;
    ctl.app.applyEdit(ctl.id, (e) {
      final i = e.local.indexOf(areaId);
      if (i < 0) return e;
      final a = e.local.items[i];
      if (k >= a.mask.length) return e;
      final na = a.withComponent(k, a.mask[k].copyWith(ref: id));
      // the area is on while every tap part has a plane (an empty tap part must not ask the export for a plane)
      final ready = na.mask.every((m) => m.kind != MaskKindId.raster || m.ref.isNotEmpty);
      return e.copyWith(local: e.local.replace(i, na.copyWith(on: ready)));
    });
    flash();
    ctl.haptics.fire(HapticKind.detent);
  }

  // ---- brush correction of an AI area

  /// 涂抹补充 ([add]) / 涂抹擦除: a brush part of the selected area that adds to / removes from it; selected, ready to paint.
  Future<void> brushFix({required bool add}) async {
    final a = sel;
    if (a == null) return;
    final op = add ? MaskOp.add : MaskOp.subtract;
    erase = false;
    for (var i = 1; i < a.mask.length; i++) {
      if (a.mask[i].kind == MaskKindId.brush && a.mask[i].op == op) {
        selComp = i;
        body = LocalBody.pads;
        flash();
        _changed();
        return;
      }
    }
    await addPart(kindById('brush'), subtract: !add);
  }

  /// The selected area is made of AI planes.
  bool get selIsAi => sel?.mask.any((m) => m.kind == MaskKindId.raster) ?? false;

  // ---- missing planes

  List<String> get _aiRefs => [
        for (final a in list.items)
          for (final m in a.mask)
            if (m.kind == MaskKindId.raster && m.ref.startsWith('ai:')) m.ref,
      ];

  /// After opening an edit: planes not in the store are re-created (disk cache first); what is still missing shows 重新分析.
  Future<void> checkMissing({bool repair = true}) async {
    final p = ctl.app.photoById(ctl.id);
    if (p == null || _aiRefs.isEmpty) {
      if (missingRefs.isNotEmpty) {
        missingRefs.clear();
        _aiNotify();
      }
      return;
    }
    final pj = jsonEncode(ctl.app.wireParams(p));
    var miss = await ai.missing(p, pj);
    if (miss.isNotEmpty && repair) {
      repairing = true;
      _aiNotify();
      await ai.ensure(p, pj);
      miss = await ai.missing(p, pj);
      repairing = false;
    }
    missingRefs
      ..clear()
      ..addAll(miss.map((m) => m.id));
    _maskSig = '';
    _overlayFor = '';
    _aiNotify();
  }

  bool areaMissing(LocalAdjustment a) => a.mask.any((m) => missingRefs.contains(m.ref));

  /// 重新分析: the missing planes of the edit, once more.
  Future<void> reanalyse() async {
    await checkMissing();
    final a = sel;
    if (a != null && areaMissing(a)) _toast('没能重新分析');
  }

  /// Memory pressure / leaving the editor: the models and the tap encoder go.
  void releaseAi() => ai.release(all: true);

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
    final c = comp;
    _rangeOpen = true;
    _followKind = c != null && kindOfComponent(c).overlayFollowsStale;
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
    _rangeOpen = false; // the stale overlay stays up until the exact one for the final values replaces it
    flash();
  }

  // ------------------------------------------------------------ photo gestures (the handle layer feeds these, frame dp)

  _Grab _grab = _Grab.none;
  int _grabIndex = 0;
  MaskComponent? _start;
  Offset _downP = Offset.zero;
  bool _moved = false, _begun = false, _multi = false;

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

  /// A still hold on the photo that did not start on a handle: 局部's compare (audit U4).
  Timer? _holdTimer;
  bool _holdCompare = false;

  /// Time a finger must stay still for the compare (the editor's long press, requirements 4.15.6).
  static const Duration kCompareHold = Duration(milliseconds: 380);

  /// Movement that turns a press into a drag (a brush stroke starts painting only after it, U4).
  static const double kSlop = 8;

  void _armHold() {
    _holdTimer?.cancel();
    _holdTimer = Timer(kCompareHold, () {
      if (_moved || _multi || _grab == _Grab.none && !active) return;
      _holdCompare = true;
      // a hold is never a tap or a dot: drop the stroke that had not started
      if (_grab == _Grab.brush) {
        _brush = null;
        liveStroke.clear();
        tick.value++;
      }
      ctl.localCompare(true);
    });
  }

  void _endHold() {
    _holdTimer?.cancel();
    _holdTimer = null;
    if (_holdCompare) {
      _holdCompare = false;
      ctl.localCompare(false);
    }
  }

  /// One finger landed on the photo.
  void pointerDown(Offset p, int ms) {
    _multi = false;
    final c = comp;
    _grab = _Grab.none;
    _endHold();
    if (!active) return;
    _downP = p;
    _moved = false;
    _begun = false;
    if (c == null) {
      _armHold();
      return;
    }
    _start = c;
    if (_healing) {
      _grab = _Grab.tap; // heal mode: a tap adds / removes a spot (the [+|−] switch), nothing else moves
      _armHold();
      return;
    }
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
      case MaskKindId.raster:
        _grab = tapPart ? _Grab.tap : _Grab.none;
      default:
        _grab = _Grab.none;
    }
    // a press on a handle belongs to the handle; anywhere else a still hold compares
    const free = {_Grab.none, _Grab.tap, _Grab.brush, _Grab.linNew, _Grab.radNew};
    if (free.contains(_grab)) _armHold();
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
    if (_holdCompare) return; // comparing: the finger only holds
    if (_holdTimer != null && (p - _downP).distance >= kSlop / zoomScale) {
      _holdTimer?.cancel();
      _holdTimer = null;
    }
    if (_multi || _grab == _Grab.none) return;
    final d = p - _downP;
    if (!_moved) {
      // the brush paints only once the finger has moved 8 dp (a still press may become the compare hold, U4); the points before
      // are kept, so the stroke still starts where the finger landed
      final slop = kSlop / zoomScale;
      if (d.distance < slop) {
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
    if (_holdCompare) {
      // the end of a compare hold: nothing is added, painted or picked
      _endHold();
      _brush = null;
      _finishGesture();
      return;
    }
    _endHold();
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
      if (_healing) {
        healTap(p);
      } else if (tapPart) {
        aiTap(p);
      } else {
        unawaited(pick(p));
      }
      return;
    }
    _finishGesture();
  }

  void pointerCancel() {
    _grab = _Grab.none;
    _endHold();
    _finishGesture();
  }

  /// A second finger landed: the photo zooms / pans. A stroke not committed yet is dropped (it was the start of a pinch).
  void secondFinger() {
    _multi = true;
    _endHold();
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

  /// A range band (tone, hue, near-far: [LocalKind.overlayFollowsStale]) is being dragged, or its last overlay is still on its way: a stale
  /// overlay is shown then (it follows the drag with a small lag, swapped directly for the fresh one). Geometry drags keep hiding it.
  final ValueNotifier<bool> overlayFollow = ValueNotifier(false);
  bool _rangeOpen = false, _followKind = false;

  void _setStale(bool stale) {
    overlayStale.value = stale;
    final f = _followKind && (_rangeOpen || stale);
    if (!f) _followKind = false;
    overlayFollow.value = f;
  }

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

  /// Device pixels of the frame's long edge at fit zoom.
  double frameDevicePx = 1024;

  /// The overlay's long edge: the photo as it is on screen now, in device pixels (fit size x device pixel ratio x zoom), bounded
  /// to 256..2048. It was the fit size capped at 1024, which a 1440p phone (or any zoom) upscaled into visible blocks.
  int get overlayPx => (frameDevicePx * math.max(1.0, zoomScale)).clamp(256.0, 2048.0).round();

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
    final want = active && a != null && !padDragging && !comparing && !healMode && (peek || shaping || _now < _showUntil || !a.hasEffect);
    if (overlayOn.value != want) overlayOn.value = want;
    _setStale(_sigNow() != _overlayFor);
    if (want) _wantOverlay();
  }

  String _sigNow() {
    final a = sel;
    if (a == null) return '';
    // the resolution step is part of the signature: zooming in past a step asks for a sharper overlay
    return '${ctl.id}|$selIndex|${identityHashCode(a.mask)}|${a.invert}|${_geoSig()}|${overlayPx ~/ 256}';
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

  void _deliverOverlay(ui.Image img, String sig) {
    final old = overlay.value;
    overlay.value = img; // swap first, dispose the old one after: no frame without an image
    old?.dispose();
    _overlayFor = sig;
    _setStale(_sigNow() != sig);
  }

  /// Test hook: an overlay image arrives for the shape on screen now ([current]) or for an older one.
  void debugDeliverOverlay(ui.Image img, {bool current = true}) => _deliverOverlay(img, current ? _sigNow() : '${_sigNow()}|old');

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
      final px = await LocalNative.overlay(s.address, pj, idx, overlayPx);
      if (px == null) return;
      final c = Completer<ui.Image>();
      ui.decodeImageFromPixels(px.rgba, px.width, px.height, ui.PixelFormat.rgba8888, c.complete);
      final img = await c.future;
      if (_sigNow() != sig && !_dirty) {
        // the selection moved on meanwhile; still better than nothing for the same area
      }
      _deliverOverlay(img, sig);
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
        _askAvail();
        _aiPhotoCheck();
        unawaited(checkMissing());
        flash();
      } else {
        cancelAnalysis();
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
    _aiPhotoCheck();
    if (active) unawaited(checkMissing());
    _onEdit();
  }

  /// A card (or the heal mode) is open in the panel: one back closes it.
  bool get hasCard => healMode || (active && body != LocalBody.pads);

  /// Back key: a card in the panel closes first.
  bool handleBack() {
    if (healMode) {
      setHealMode(false);
      return true;
    }
    if (active && body != LocalBody.pads) {
      if (body == LocalBody.aiParts) cancelAnalysis();
      showBody(LocalBody.pads);
      return true;
    }
    return false;
  }

  // ------------------------------------------------------------ pad and item sources

  late final LocalPadSource lightPad = LocalPadSource(this, PadCat.light);
  late final LocalPadSource colorPad = LocalPadSource(this, PadCat.color);
  late final LocalPadSource texturePad = LocalPadSource(this, PadCat.texture);
  late final LocalItemSource items = LocalItemSource(this);

  /// The itemised view of a portrait area: its recipe's ops (to the recipe's maxima), and for 肤质 the heal sensitivity.
  List<PadItem> portraitItems() {
    final a = sel;
    final pt = a == null ? null : portraitOf(a);
    if (pt == null) return const [];
    String label(String k) => switch (k) {
          'smooth' => '磨皮',
          'even' => '匀肤',
          'redness' => '去红',
          'heal' => '祛斑',
          'texture' => '纹理',
          'undereye' => '眼下提亮',
          'shine' => '去油光',
          'whiten' => pt.$2.id == 'pt-teeth' ? '牙齿美白' : '眼白提亮',
          'clarity' => '眼神',
          'saturation' => pt.$2.id == 'pt-lips' ? '唇色' : '瞳色',
          'facelight' => '补光',
          _ => localOpByKey[k]?.label ?? k,
        };
    return [
      for (final o in pt.$2.ops)
        PadItem(o.key, label(o.key), o.key == 'texture' ? -o.max : 0, o.max, o.def, (v) => o.key == 'texture' ? fmtValue(Fmt.signed, v) : fmtValue(Fmt.plain, v)),
      if (pt.$2.id == 'pt-skin') PadItem('sens', '祛斑范围', 0, 100, 25, (v) => fmtValue(Fmt.plain, v)),
    ];
  }

  /// The itemised view's items of [group] (light, color, detail).
  List<PadItem> itemsOf(String group) => [
        for (final s in localOpSpecs)
          if (s.group == group)
            PadItem(s.key, s.label, s.min, s.max, 0, (v) => s.key == 'exposure' ? fmtValue(Fmt.ev, v) : (s.min < 0 ? fmtValue(Fmt.signed, v) : fmtValue(Fmt.plain, v))),
      ];

  @override
  void dispose() {
    _aiSub?.cancel();
    ctl.app.editTick.removeListener(_onEdit);
    ctl.wm.removeListener(_onWm);
    _visTimer?.cancel();
    _reqTimer?.cancel();
    overlay.value?.dispose();
    overlay.dispose();
    overlayOn.dispose();
    overlayStale.dispose();
    overlayFollow.dispose();
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
    return switch (cat) {
      PadCat.color => PadState(x: u.cx, y: u.cy, a: u.a),
      PadCat.texture => PadState(x: u.tx, y: u.ty, a: u.a),
      _ => PadState(x: u.lx, y: u.ly, a: u.a),
    };
  }

  @override
  PadLimits get limits => PadLimits.full;

  @override
  void begin() {
    lc.ctl.gestureBegin('local-pad-${cat.name}', param: switch (cat) {
      PadCat.color => 'temp',
      PadCat.texture => 'clarity',
      _ => 'exposure',
    });
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
    final n = switch (cat) {
      PadCat.color => u.copyWith(cx: px, cy: py),
      PadCat.texture => u.copyWith(tx: px, ty: py),
      _ => u.copyWith(lx: px, ly: py),
    };
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
  String get readout => localReadout(_ui, area: lc.sel, pad: cat);

  @override
  double percent(double a) => localStripPercent(lc.sel, a);
}

/// The itemised view of the selected area: every local op, the pads' recipe as the dashed polygon.
class LocalItemSource extends ItemSource {
  LocalItemSource(this.lc);
  final LocalController lc;

  @override
  Listenable get changes => Listenable.merge([lc.ctl.app.editTick, lc]);

  @override
  double actual(PadItem it) => it.id == 'sens' ? (lc.sel?.sens ?? 25) : (lc.sel?.op(it.id) ?? 0);

  @override
  double implied(PadItem it) {
    final a = lc.sel;
    if (a == null) return 0;
    if (it.id == 'sens') return 25;
    final s = localOpByKey[it.id]!;
    return (recipeOf(a, localUiOf(a))[it.id] ?? 0).clamp(s.min, s.max).toDouble();
  }

  @override
  void begin(PadItem it) {
    lc.ctl.gestureBegin('local-item-${it.id}', param: it.id);
    lc.padDragging = true;
    lc._updateVis();
  }

  @override
  void set(PadItem it, double v) =>
      lc._writeSel((a) => it.id == 'sens' ? a.copyWith(sens: v.clamp(0.0, 100.0).roundToDouble()) : setLocalItem(a, it.id, v.clamp(it.min, it.max).toDouble()));

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
