// 形体 in the 人像 panel (docs/design/local-adjustments.md section 9.2): the reshaping tiles, their strip, the action card's 关掉 and the
// manual 推 / 恢复 brush on the photo. State of the edit: EditState.reshape (model/reshape.dart); engine: section 45.
part of 'local_controller.dart';

class ReshapeTool {
  ReshapeTool._(this.lc);
  final LocalController lc;

  /// The tile being adjusted (its strip, its action card); null = none.
  ReshapeOp? sel;

  /// The action card is for this tile (its 关掉).
  ReshapeOp? action;

  /// 推 on: a drag on the photo pushes the picture; the [推|恢复] switch picks restore (audit U2: a switch, never a toggle by tap).
  bool pushMode = false;
  bool pushAdd = true;
  int pushSize = 1;

  Reshape get value => lc.ctl.edit.reshape;

  /// The faces the analysis gave anchors for (an engine / cache without `shape` gives none: the face tiles stay away).
  List<AiFace> get faces => [for (final f in lc.faceList) if (f.shape.length == 108) f];

  /// The pose of the main person: not produced by this build's analysis (the body tiles appear when it is).
  List<double>? get pose => null;

  List<ReshapeOp> get faceOps => faces.isEmpty ? const [] : faceOpsFor(anyFrontal: faces.any((f) => f.frontal));
  List<ReshapeOp> get bodyOps => pose == null ? const [] : const [ReshapeOp.waist, ReshapeOp.arms, ReshapeOp.legs, ReshapeOp.longLegs, ReshapeOp.head];

  /// Push works on any photo (no analysis needed).
  bool get available => true;

  bool on(ReshapeOp op) => value.on(op);

  /// The edit's reshape with the analysis' anchors and the person plane merged in.
  Reshape _anchored() => value.withAnchors(
        faces: [for (final f in faces) (f.index, f.shape)],
        pose: pose,
        matte: lc.aiPlanes['person']?.id,
      );

  void _write(Reshape r, {bool step = true}) {
    if (step) {
      lc.ctl.app.applyEdit(lc.ctl.id, (e) => e.copyWith(reshape: r));
    } else {
      lc.ctl.applyGesture((e) => e.copyWith(reshape: r));
    }
  }

  /// A tile (U3): off -> on at the recommended amount for the main person (the largest face) and selected; on, not selected -> selected;
  /// selected -> its action card (关掉).
  void tap(ReshapeOp op) {
    if (!on(op)) {
      final main = faces.isEmpty ? <int>{} : {faces.first.index};
      _write(_anchored().withValue(op, kReshapeDefault, sel: main));
      sel = op;
      lc.selId = null;
      lc.body = LocalBody.portrait;
      lc._changed();
      return;
    }
    if (sel != op) {
      sel = op;
      lc.selId = null;
      lc._changed();
      return;
    }
    // the action card shows in the 人像 panel itself (the area action card needs an area)
    action = action == op ? null : op;
    lc._changed();
  }

  void turnOff(ReshapeOp op) {
    final pid = lc.ctl.id;
    _write(value.withValue(op, 0));
    action = null;
    lc.body = LocalBody.portrait;
    lc._changed();
    lc.ctl.app.showToast('已关掉${reshapeSpecOf(op).label}', actionLabel: '撤销', onAction: () => lc.ctl.app.undo(pid));
  }

  /// The faces the selected tile works on (the face row, several faces).
  Set<int> get selFaces => sel == null ? const {} : value.facesOf(sel!);

  void toggleFace(int n) {
    final op = sel;
    if (op == null || reshapeSpecOf(op).body) return;
    final cur = value.facesOf(op);
    final next = {...cur};
    if (!next.remove(n)) next.add(n);
    if (next.isEmpty) return;
    final v = value.value(op);
    _write(_anchored().withValue(op, v == 0 ? kReshapeDefault : v, sel: next));
    lc._changed();
  }

  // ---- 推 / 恢复 on the photo
  void setPushMode(bool v) {
    pushMode = v;
    if (v) lc.healMode = false;
    lc.tick.value++;
    lc._changed();
  }

  void setPushAdd(bool v) {
    if (pushAdd == v) return;
    pushAdd = v;
    lc._changed();
  }

  void setPushSize(int i) {
    pushSize = i.clamp(0, kPushRadius.length - 1);
    lc._changed();
  }

  bool get pushing => pushMode && lc.active;
  bool _stroking = false;
  Offset _last = Offset.zero;

  double get _iod => faces.isEmpty ? 0 : faces.map((f) => f.iod).reduce(math.max);

  /// The photo point under frame position [p]: the reshaped picture's own coordinates (geometry only: strokes compose on what is shown).
  Offset _uv(Offset p) => lc.space.photo(p);

  void down(Offset p) {
    _stroking = false;
    _last = p;
  }

  void move(Offset p) {
    final uv = _uv(p);
    if (!_stroking) {
      if (value.push.length >= kMaxPushStrokes) {
        lc._toast('最多 $kMaxPushStrokes 笔');
        return;
      }
      _stroking = true;
      lc.ctl.gestureBegin('reshape-push', param: null);
      final a = _uv(_last);
      final s = PushStroke(r: pushRadius(pushSize, iod: _iod), pts: [a.dx, a.dy, uv.dx, uv.dy], restore: !pushAdd);
      _write(value.copyWith(push: [...value.push, s]), step: false);
    } else {
      final cur = value.push;
      if (cur.isEmpty) return;
      _write(value.copyWith(push: [...cur.sublist(0, cur.length - 1), cur.last.append(uv.dx, uv.dy)]), step: false);
    }
    _last = p;
  }

  void up() {
    if (_stroking) {
      lc.ctl.gestureEnd();
      lc.ctl.haptics.fire(HapticKind.detent);
    }
    _stroking = false;
  }

  void cancel() => up();
}

/// The strip of the selected 形体 tile: 0 at the bottom, the recommended amount in the middle, the believable maximum at the top.
class ReshapeStripSource extends PadSource {
  ReshapeStripSource(this.t);
  final ReshapeTool t;

  @override
  Listenable get changes => Listenable.merge([t.lc.ctl.app.editTick, t.lc]);
  @override
  bool get enabled => t.sel != null;
  @override
  PadState get view => PadState(x: 0, y: 0, a: t.sel == null ? 0.5 : reshapeToStrip(t.value.value(t.sel!)));
  @override
  PadLimits get limits => PadLimits.full;
  @override
  void begin() => t.lc.ctl.gestureBegin('reshape-strip', param: null);
  @override
  void move(double x, double y) {}
  @override
  void strip(double a) {
    final op = t.sel;
    if (op == null) return;
    var v = a.clamp(0.0, 1.0).toDouble();
    if ((v - 0.5).abs() < kStripSnap) v = 0.5;
    final x = reshapeFromStrip(v);
    if (x == t.value.value(op)) return;
    if (v == 0.5) t.lc.ctl.haptics.fire(HapticKind.autoSnap);
    final sel = t.value.facesOf(op);
    t._write(t.value.withValue(op, x, sel: sel.isEmpty && t.faces.isNotEmpty ? {t.faces.first.index} : sel), step: false);
  }

  @override
  void end() => t.lc.ctl.gestureEnd();
  @override
  void reset() {
    final op = t.sel;
    if (op != null) t._write(t.value.withValue(op, kReshapeDefault, sel: t.value.facesOf(op)));
  }

  @override
  String get readout => t.sel == null ? '' : '${reshapeSpecOf(t.sel!).label} ${t.value.value(t.sel!).round()}';

  /// The strip reads the amount itself (0 .. 100, the middle = 50 = the recommended amount).
  @override
  double percent(double a) => a.clamp(0.0, 1.0) * 100;
}
