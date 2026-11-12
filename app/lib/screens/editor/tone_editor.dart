// 明暗目标: the 3x3 grid of tone classes (high / mid / low key by short / mid / long range) the automatic tone move steers
// towards (reached from 光线's 分项). Absolute: the cell under the finger is the selected one; the photo shows the result at once.
// Each cell previews this photo under that target.
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../../model/params.dart';
import '../../theme/tokens.dart';
import '../../widgets/photo_art.dart';
import 'editor_controller.dart';
import 'pad_gestures.dart';

/// The tone class id of the cell under [p] in a grid of [side] x [side] with [gap] between cells (null outside).
String toneIdAt(Offset p, double side, double gap) {
  final cell = (side - 2 * gap) / 3;
  int idx(double v) => (v / (cell + gap)).floor().clamp(0, 2);
  return toneClassId(toneKeys[idx(p.dy)], toneRanges[idx(p.dx)]);
}

class ToneEditor extends StatefulWidget {
  const ToneEditor({super.key, required this.ctl});
  final EditorController ctl;

  @override
  State<ToneEditor> createState() => _ToneEditorState();
}

class _ToneEditorState extends State<ToneEditor> {
  static const double gap = 4;
  double _side = 150;
  bool _begun = false;
  String? _last;

  EditorController get ctl => widget.ctl;

  @override
  void initState() {
    super.initState();
    ctl.engine.refreshTiles(); // the previews under each target
  }

  void _pick(Offset p) {
    final id = toneIdAt(p, _side, gap);
    if (id == _last) return;
    _last = id;
    ctl.toneTargetSet(id);
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return LayoutBuilder(builder: (context, cons) {
      _side = math.min(cons.maxHeight, 190.0);
      return ListenableBuilder(
        listenable: Listenable.merge([ctl.app.editTick, ctl.engine]),
        builder: (context, _) {
          final nearest = ctl.analysis().nearest;
          final selected = ctl.edit.toneTarget ?? nearest;
          return Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
            SizedBox(
              width: _side,
              height: _side,
              child: ClaimSurface(
                onDown: (p) {
                  ctl.gestureBegin('tone');
                  _begun = true;
                  _last = null;
                  _pick(p);
                },
                onMove: _pick,
                onEnd: (p, cancelled) {
                  if (_begun) ctl.gestureEnd();
                  _begun = false;
                },
                child: _grid(context, selected, nearest, (_side - 2 * gap) / 3),
              ),
            ),
            const SizedBox(width: 14),
            Expanded(
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text(tonePlain(selected), key: const ValueKey('tone-name'), style: T.body(c.text, w: FontWeight.w600)),
                const SizedBox(height: 2),
                Text(tonePro(selected), style: T.cap(c.text2)),
                const SizedBox(height: 8),
                Text(ctl.edit.toneTarget == null ? '自动：与这张最接近' : '白点 = 与这张最接近', style: T.cap(c.text2)),
                const Spacer(),
                if (ctl.edit.toneTarget != null)
                  GestureDetector(
                    key: const ValueKey('tone-auto'),
                    behavior: HitTestBehavior.opaque,
                    onTap: ctl.toneTargetAuto,
                    child: Container(
                      height: 36,
                      padding: const EdgeInsets.symmetric(horizontal: 14),
                      alignment: Alignment.center,
                      decoration: BoxDecoration(color: c.chip, borderRadius: BorderRadius.circular(18)),
                      child: Text('回到自动', style: T.cap(c.text, w: FontWeight.w500)),
                    ),
                  ),
              ]),
            ),
          ]);
        },
      );
    });
  }

  Widget _grid(BuildContext context, String selected, String nearest, double cell) {
    final c = context.c;
    final photo = ctl.app.photoById(ctl.id)!;
    final de = DemoEdit.of(ctl.app, photo);
    final ts = math.max(60.0, de.edit.toneStrength ?? de.defaultTone);
    return Semantics(
      container: true,
      label: '明暗目标',
      child: Column(children: [
        for (final k in toneKeys)
          Padding(
            padding: EdgeInsets.only(bottom: k == 'low' ? 0 : gap),
            child: Row(children: [
              for (final r in toneRanges)
                Padding(
                  padding: EdgeInsets.only(right: r == 'long' ? 0 : gap),
                  child: Builder(builder: (context) {
                    final id = toneClassId(k, r);
                    final sel = id == selected;
                    final tile = ctl.engine.tiles[id];
                    final Widget art = tile != null
                        ? RawImage(image: tile, fit: BoxFit.cover)
                        : (ctl.engine.live ? PhotoArt(photo, cacheWidth: 90) : PhotoArt(photo, cacheWidth: 90, demoEdit: DemoEdit(de.edit.copyWith(toneTarget: id, toneStrength: ts), de.defaultLook, de.defaultTone)));
                    return Semantics(
                      checked: sel,
                      inMutuallyExclusiveGroup: true,
                      label: '${tonePlain(id)}，${tonePro(id)}${id == nearest ? '，与这张最接近' : ''}',
                      excludeSemantics: true,
                      child: SizedBox(
                        key: ValueKey('tone-$id'),
                        width: cell,
                        height: cell,
                        child: Stack(fit: StackFit.expand, children: [
                          ClipRRect(borderRadius: BorderRadius.circular(6), child: art),
                          if (id == nearest) Positioned(right: 4, top: 4, child: Container(width: 6, height: 6, decoration: const BoxDecoration(color: Colors.white, shape: BoxShape.circle))),
                          if (sel) DecoratedBox(decoration: BoxDecoration(borderRadius: BorderRadius.circular(6), border: Border.all(color: c.accentBtn, width: 2.5))),
                          if (sel) Padding(padding: const EdgeInsets.all(2.5), child: DecoratedBox(decoration: BoxDecoration(borderRadius: BorderRadius.circular(4), border: Border.all(color: Colors.white, width: 1)))),
                        ]),
                      ),
                    );
                  }),
                ),
            ]),
          ),
      ]),
    );
  }
}
