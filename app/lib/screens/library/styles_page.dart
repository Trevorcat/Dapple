import 'package:flutter/material.dart';

import '../../model/catalog.dart';
import '../../models.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/icons.dart';
import '../../widgets/photo_art.dart';
import 'page_bar.dart';

/// 风格 tab: default look + library (requirements 4.12; wide: list + preview, 4.14.2).
class StylesPage extends StatefulWidget {
  const StylesPage({super.key, required this.app, required this.wide});
  final AppState app;
  final bool wide;

  @override
  State<StylesPage> createState() => _StylesPageState();
}

class _StylesPageState extends State<StylesPage> {
  String _cat = 'all';
  String? _previewId; // wide: look shown in the preview pane
  double _strength = 100;

  AppState get app => widget.app;

  /// Preview picture: the first processed photo, else a built-in synthetic scene (a new install has no photo yet: grey tiles told nothing).
  static final Photo _sample = Photo(id: 'style-sample', name: 'sample', addedMs: 0, demo: 0, status: PhotoStatus.done);

  Photo get _example {
    for (final p in app.photos) {
      if (p.status == PhotoStatus.done && (p.demo != null || p.previewPath != null)) return p;
    }
    return _sample;
  }

  bool get _isSample => identical(_example, _sample);

  List<DLook> get _looks =>
      dLooks.where((l) => _cat == 'all' || l.cat == _cat || l.id == 'std').toList();

  /// The example photo with [look] approximated through a colour matrix (the engine renders the real thing).
  Widget looked(DLook l, {double strength = 1}) {
    final ph = _example;
    final s = strength;
    return ColorFiltered(
      colorFilter: approxFilter(
          b: 1 + (l.bright - 1) * s,
          c: 1 + (l.contrast - 1) * s,
          s: 1 + (l.sat - 1) * s,
          gray: l.gray * s,
          sepia: l.sepia * s,
          warm: (0.5 - l.py) * 0.8 * s),
      child: PhotoArt(ph, cacheWidth: 600),
    );
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return ColoredBox(
      color: c.bg,
      child: Column(children: [
        const PageBar('风格'),
        Expanded(child: widget.wide ? _wideBody(context) : _phoneBody(context)),
      ]),
    );
  }

  Widget _chips() {
    return SizedBox(
      height: 56,
      child: ListView(
        scrollDirection: Axis.horizontal,
        padding: const EdgeInsets.symmetric(horizontal: Sp.l),
        children: [
          for (final cat in lookCats)
            Padding(
              padding: const EdgeInsets.only(right: Sp.s),
              child: KeyedSubtree(
                  key: ValueKey('cat-${cat[0]}'),
                  child: FilterChip32(cat[1], selected: _cat == cat[0], onTap: () => setState(() => _cat = cat[0]))),
            ),
        ],
      ),
    );
  }

  Widget _row(DLook l, {required bool selected, required VoidCallback onTap}) {
    final c = context.c;
    return Semantics(
      inMutuallyExclusiveGroup: true,
      checked: selected,
      label: '${l.name}，${l.tags}',
      excludeSemantics: true,
      child: InkWell(
        key: ValueKey('look-${l.id}'),
        onTap: onTap,
        child: Container(
          constraints: const BoxConstraints(minHeight: 68),
          padding: const EdgeInsets.symmetric(horizontal: Sp.l, vertical: Sp.s),
          child: Row(children: [
            ClipRRect(borderRadius: BorderRadius.circular(Rd.tile), child: SizedBox(width: 52, height: 52, child: looked(l))),
            const SizedBox(width: Sp.l),
            Expanded(
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisSize: MainAxisSize.min, children: [
                Text(l.name, style: T.body17(c.text, w: FontWeight.w500)),
                Text(l.tags, style: T.cap(c.text2)),
              ]),
            ),
            if (selected) DIcon(DI.check, size: 24, color: c.accent, stroke: 2.4),
          ]),
        ),
      ),
    );
  }

  Widget _phoneBody(BuildContext context) {
    final c = context.c;
    final def = lookById(app.settings.defaultLookId);
    return ListView(
      padding: const EdgeInsets.only(bottom: 120),
      children: [
        Container(
          key: const ValueKey('default-card'),
          margin: const EdgeInsets.symmetric(horizontal: Sp.l),
          decoration: BoxDecoration(color: c.surface, borderRadius: BorderRadius.circular(Rd.card)),
          clipBehavior: Clip.antiAlias,
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            SizedBox(height: 180, width: double.infinity, child: looked(def)),
            Padding(
              padding: const EdgeInsets.all(Sp.l),
              child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text(_isSample ? '新照片默认使用（预览为示例画面）' : '新照片默认使用', style: T.small(c.text2)),
                const SizedBox(height: 4),
                Text(def.name, style: T.h2(c.text)),
                Text(def.tags, style: T.small(c.text2)),
              ]),
            ),
          ]),
        ),
        const SizedBox(height: Sp.s),
        _chips(),
        Semantics(
          container: true,
          label: '默认风格',
          child: Column(children: [
            for (final l in _looks) _row(l, selected: l.id == def.id, onTap: () => app.setDefaultLook(l.id)),
          ]),
        ),
        const SectionTitle('导入'),
        // TODO(look import): .xmp via dapple_xmp_to_json (lists unsupported items), .cube LUT, Fujifilm recipes.
        for (final r in const [('Lightroom 预设（.xmp）', 'xmp'), ('LUT 滤镜（.cube）', 'cube'), ('相机胶片配方', 'fuji')])
          InkWell(
            key: ValueKey('import-${r.$2}'),
            onTap: () => app.showToast('待实现'),
            child: Container(
              constraints: const BoxConstraints(minHeight: 56),
              padding: const EdgeInsets.symmetric(horizontal: Sp.l),
              child: Row(children: [
                Expanded(child: Text(r.$1, style: T.body17(c.text))),
                DIcon(DI.plus, size: 22, color: c.text2),
              ]),
            ),
          ),
      ],
    );
  }

  Widget _wideBody(BuildContext context) {
    final c = context.c;
    final def = lookById(app.settings.defaultLookId);
    final cur = lookById(_previewId ?? def.id);
    return Row(children: [
      SizedBox(
        width: 280,
        child: Column(children: [
          _chips(),
          Expanded(
            child: ListView(children: [
              for (final l in _looks) _row(l, selected: l.id == cur.id, onTap: () => setState(() => _previewId = l.id)),
            ]),
          ),
        ]),
      ),
      Expanded(
        child: Padding(
          padding: const EdgeInsets.all(Sp.l),
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Expanded(
              child: Container(
                key: const ValueKey('style-preview'),
                decoration: BoxDecoration(color: c.stage, borderRadius: BorderRadius.circular(Rd.card)),
                clipBehavior: Clip.antiAlias,
                width: double.infinity,
                child: looked(cur, strength: _strength / 100),
              ),
            ),
            const SizedBox(height: Sp.m),
            Row(children: [
              Text(cur.name, style: T.h2(c.text)),
              const SizedBox(width: Sp.s),
              Text(cur.tags, style: T.small(c.text2)),
              if (cur.id == def.id) ...[const SizedBox(width: Sp.s), Text('默认', style: T.small(c.accent, w: FontWeight.w500))],
            ]),
            DSlider(
              label: '风格强度',
              value: _strength,
              min: 0,
              max: 100,
              def: 100,
              display: '${_strength.round()}%',
              onChanged: (v) => setState(() => _strength = v),
            ),
            Row(children: [
              Expanded(child: PrimaryButton('用于此照片', height: 48, onPressed: () => _applyToPhoto(cur))),
              const SizedBox(width: Sp.m),
              Expanded(child: SecondaryButton('设为默认', height: 48, onPressed: () => app.setDefaultLook(cur.id))),
            ]),
          ]),
        ),
      ),
    ]);
  }

  void _applyToPhoto(DLook l) {
    final list = app.editable;
    if (list.isEmpty) {
      app.showToast('还没有照片');
      return;
    }
    final ph = list.first;
    final id = ph.id;
    app.applyEdit(id, (e) => e.copyWith(look: l.id, lookStrength: _strength, palette: null));
    app.showToast('已用于「${ph.title}」', actionLabel: '撤销', onAction: () => app.undo(id));
  }
}
