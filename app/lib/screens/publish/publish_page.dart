import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../../model/catalog.dart';
import '../../models.dart';
import '../../services/share_service.dart';
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/controls.dart';
import '../../widgets/export_size.dart';
import '../../widgets/icons.dart';
import '../../widgets/photo_art.dart';
import 'publish_done_page.dart';

/// Opens the publish sheet for [ids] (in order). Resolves when the whole flow closes; true when the
/// user finished via the done screen's "完成" (callers then leave selection mode).
Future<bool> openPublish(BuildContext context, AppState app, List<String> ids) async {
  final r = await Navigator.of(context).push<bool>(PageRouteBuilder<bool>(
    opaque: false,
    barrierColor: context.c.scrim,
    barrierDismissible: false,
    transitionDuration: const Duration(milliseconds: 250),
    pageBuilder: (_, _, _) => PublishPage(app: app, ids: List.of(ids)),
    transitionsBuilder: (_, anim, _, child) => SlideTransition(
        position: Tween(begin: const Offset(0, 1), end: Offset.zero)
            .animate(CurvedAnimation(parent: anim, curve: Curves.easeOut)),
        child: child),
  ));
  return r ?? false;
}

double _ratioOf(String r, Photo p) {
  if (r == 'orig') return p.ratio ?? 1.5;
  final s = r.split(':');
  return double.parse(s[0]) / double.parse(s[1]);
}

String ratioName(String r) => r == 'orig' ? '原比例' : r;

class PublishPage extends StatefulWidget {
  const PublishPage({super.key, required this.app, required this.ids});
  final AppState app;
  final List<String> ids;

  @override
  State<PublishPage> createState() => _PublishPageState();
}

class _PublishPageState extends State<PublishPage> {
  late List<String> _ids = widget.ids;
  Preset _preset = presets[0];
  late String _ratio = _preset.defRatio;
  late int _size = _preset.longEdge ?? widget.app.settings.exportLongEdge;
  late bool _strip = widget.app.settings.stripLocation; // "去除位置信息": reaches the export call; GPS is written only when kept and the original has a fix
  bool _split = false;
  int _batch = 0;
  // "HDR 照片" (Ultra HDR gain-map JPEG): offered only when at least one photo has highlights worth encoding; off by default.
  bool _hdr = false;
  bool _hdrAvailable = false;
  String _hdrFor = '';
  // "色彩空间": sRGB (default) or 广色域 P3; offered only when at least one photo has colour outside sRGB worth keeping (AppState.wideGamutOf).
  bool _p3 = false;
  bool _p3Available = false;

  @override
  void initState() {
    super.initState();
    app.exportSheetOpened();
    _probeHdr();
  }

  @override
  void dispose() {
    app.exportSheetClosed();
    super.dispose();
  }

  void _probeHdrSoon() => Future.microtask(_probeHdr);

  Future<void> _probeHdr() async {
    final ids = List<String>.of(_ids);
    final key = ids.join(',');
    if (key == _hdrFor) return;
    _hdrFor = key;
    var any = false, wide = false;
    for (final id in ids) {
      final ph = app.photoById(id);
      if (ph == null) continue;
      if (!any && await app.hdrHeadroomOf(ph) > 0) any = true;
      if (!wide && await app.wideGamutOf(ph) >= AppState.wideGamutMinShare) wide = true;
      if (any && wide) break;
    }
    if (!mounted || _hdrFor != key) return;
    setState(() {
      _hdrAvailable = any;
      if (!any) _hdr = false;
      _p3Available = wide;
      if (!wide) _p3 = false;
    });
  }

  AppState get app => widget.app;

  int get _limit => _preset.maxCount ?? 1 << 30;
  bool get _overLimit => !_split && _ids.length > _limit;
  int get _batches => _split ? (_ids.length / _limit).ceil() : 1;
  List<String> get _cur {
    if (!_split) return _ids;
    final a = _batch * _limit;
    return _ids.sublist(a, math.min(_ids.length, a + _limit));
  }

  List<Photo> get _photos => [for (final id in _cur) if (app.photoById(id) != null) app.photoById(id)!];

  void _pick(Preset p) => setState(() {
        _preset = p;
        _ratio = p.defRatio;
        _size = p.longEdge ?? app.settings.exportLongEdge;
        _split = false;
        _batch = 0;
      });

  void _remove(int i) {
    final id = _cur[i];
    if (_cur.length == 1 && _ids.length == 1) {
      Navigator.of(context).pop(false);
      return;
    }
    _probeHdrSoon();
    setState(() {
      _ids = List.of(_ids)..remove(id);
      if (_split && _batch >= _batches) _batch = _batches - 1;
      if (_split && _ids.length <= _limit) _split = false;
    });
  }

  void _move(int i, int d) {
    final id = _cur[i];
    final g = _ids.indexOf(id);
    final h = g + d;
    if (h < 0 || h >= _ids.length) return;
    setState(() {
      final l = List.of(_ids);
      l[g] = l[h];
      l[h] = id;
      _ids = l;
    });
  }

  String get _title => _split ? '第 ${_batch + 1}/$_batches 篇 · ${_cur.length} 张' : '发布 ${_ids.length} 张';

  // ------------------------------------------------------------ share
  Future<void> _chooseHandoff() async {
    final c = context.c;
    final choice = await showModalBottomSheet<String>(
      context: context,
      backgroundColor: c.surface,
      showDragHandle: true,
      shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(Rd.sheet))),
      constraints: const BoxConstraints(maxWidth: 480),
      builder: (ctx) => SafeArea(
        child: Padding(
          padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
          child: Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
            Padding(
              padding: const EdgeInsets.only(bottom: 8),
              child: Text('分享 ${_cur.length} 张', style: T.body17(c.text, w: FontWeight.w500)),
            ),
            _HandoffRow(key: const ValueKey('handoff-system'), label: '系统分享…', onTap: () => Navigator.pop(ctx, 'system')),
            if (_preset.app != null)
              _HandoffRow(
                  key: const ValueKey('handoff-gallery'),
                  label: '存入相册，再打开${_preset.app}',
                  onTap: () => Navigator.pop(ctx, 'gallery')),
          ]),
        ),
      ),
    );
    if (choice != null && mounted) await _run(choice == 'gallery');
  }

  Future<void> _run(bool fallback) async {
    final photos = _photos;
    final progress = ValueNotifier<double>(0);
    var cancelled = false;
    showDialog<void>(
      context: context,
      barrierDismissible: false,
      builder: (_) => _ExportDialog(progress: progress, onCancel: () => cancelled = true),
    );
    List<String>? paths;
    try {
      paths = await _export(photos, fallback, progress, () => cancelled);
    } catch (_) {
      paths = null;
      // what happened plus the step that can help (audit F20)
      if (mounted) app.showToast('没导出成功', actionLabel: '重试', onAction: () => _run(fallback));
    }
    if (!mounted) return;
    Navigator.of(context).pop(); // dialog
    if (paths == null || cancelled) return;
    var handed = true;
    if (!fallback) handed = await ShareService.instance.shareFiles(paths);
    if (!mounted || !handed) return;
    final hasNext = _split && _batch + 1 < _batches;
    final r = await Navigator.of(context).push<DoneResult>(MaterialPageRoute(
      builder: (_) => PublishDonePage(
        app: app,
        preset: _preset,
        count: photos.length,
        ratio: _ratio,
        size: _size,
        fallback: fallback,
        nextBatch: hasNext ? _batch + 2 : null,
      ),
    ));
    if (!mounted) return;
    switch (r) {
      case DoneResult.finish:
        Navigator.of(context).pop(true);
      case DoneResult.next:
        setState(() => _batch++);
      case DoneResult.again:
      case null:
        break;
    }
  }

  Future<List<String>?> _export(List<Photo> photos, bool fallback, ValueNotifier<double> progress, bool Function() cancelled) async {
    final out = <String>[];
    for (var i = 0; i < photos.length; i++) {
      if (cancelled()) return null;
      final ph = photos[i];
      if (fallback) {
        await app.saveToGallery([ph],
            longEdge: _size, cropOverride: _ratio, hdr: _hdr && _hdrAvailable, stripLocation: _strip, wideGamut: _p3 && _p3Available);
      } else {
        out.add(await app.exportJpeg(ph,
            longEdge: _size, cropOverride: _ratio, hdr: _hdr && _hdrAvailable, stripLocation: _strip, wideGamut: _p3 && _p3Available));
      }
      progress.value = (i + 1) / photos.length;
    }
    return out;
  }

  // ------------------------------------------------------------ build
  List<_Warn> _warnings() {
    final w = <_Warn>[];
    final ps = _photos;
    for (var i = 0; i < ps.length; i++) {
      if (ps[i].processing) w.add(_Warn('第 ${i + 1} 张还在处理，现在发出的是预览图'));
      if (ps[i].flag) w.add(_Warn('第 ${i + 1} 张暗部噪点较多'));
    }
    if (_overLimit) {
      final k = (_ids.length / _limit).ceil();
      w.add(_Warn('${_preset.name}一次最多 $_limit 张', actionLabel: '拆成 $k 篇', onAction: () => setState(() {
            _split = true;
            _batch = 0;
          })));
    }
    return w;
  }

  /// Height of the thumbnail strip: 104, or 72 on a short window (landscape phone) so the presets and the options stay in view.
  double _stripH = 104;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final mq = MediaQuery.of(context);
    final size = mq.size;
    final wide = size.width >= 600;
    // landscape phone / unfolded inner screen: two columns (photos and presets | options and the button), audit F45
    final twoCol = size.width >= 720 && size.width >= size.height;
    _stripH = size.height < 480 ? 72 : 104;
    final footer = math.max(mq.padding.bottom, 16.0);
    final top = wide ? 48.0 : mq.padding.top;
    final header = Column(mainAxisSize: MainAxisSize.min, children: [
      // Material sheet header: drag handle, close icon, left-aligned title.
      Padding(
        padding: const EdgeInsets.only(top: 8),
        child: Center(child: Container(width: 32, height: 4, decoration: BoxDecoration(color: c.track, borderRadius: BorderRadius.circular(2)))),
      ),
      SizedBox(
        height: 56,
        child: Row(children: [
          const SizedBox(width: 4),
          DIconButton(DI.close, key: const ValueKey('publish-close'), label: '关闭', onPressed: () => Navigator.of(context).pop(false)),
          const SizedBox(width: 4),
          Expanded(child: Text(_title, key: const ValueKey('publish-title'), maxLines: 1, overflow: TextOverflow.ellipsis, style: T.body17(c.text, w: FontWeight.w500))),
          const SizedBox(width: 16),
        ]),
      ),
    ]);
    final where = <Widget>[
      _strip_(),
      const SizedBox(height: Sp.xl),
      Text('发到哪里', style: T.small(c.accent, w: FontWeight.w500)),
      const SizedBox(height: 8),
      _presetGrid(),
    ];
    final how = <Widget>[
      _infoCard(),
      for (final w in _warnings()) ...[const SizedBox(height: 12), _WarnCard(w)],
    ];
    Widget button({bool line = true}) => Container(
          decoration: BoxDecoration(border: line ? Border(top: BorderSide(color: c.sep)) : null),
          padding: EdgeInsets.fromLTRB(16, 8, 16, twoCol ? 16 : footer),
          child: PrimaryButton('分享…', onPressed: _overLimit ? null : _chooseHandoff, key: const ValueKey('publish-share')),
        );
    final Widget body;
    if (twoCol) {
      body = Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Expanded(child: ListView(padding: const EdgeInsets.fromLTRB(16, 4, 8, 16), children: where)),
        Expanded(
          child: Column(children: [
            Expanded(child: ListView(padding: const EdgeInsets.fromLTRB(8, 4, 16, 8), children: how)),
            button(line: false),
          ]),
        ),
      ]);
    } else {
      body = Column(children: [
        Expanded(child: ListView(padding: const EdgeInsets.fromLTRB(16, 4, 16, 16), children: [...where, const SizedBox(height: 16), ...how])),
        button(),
      ]);
    }
    Widget sheet = Material(
      color: c.bg,
      borderRadius: BorderRadius.vertical(top: const Radius.circular(Rd.sheet), bottom: Radius.circular(wide ? Rd.sheet : 0)),
      clipBehavior: Clip.antiAlias,
      child: Stack(children: [
        Column(children: [header, Expanded(child: body)]),
        ToastLayer(app: app, bottom: 8 + 52 + footer + 12),
      ]),
    );
    if (twoCol) {
      final pad = mq.padding;
      sheet = Padding(
        padding: EdgeInsets.fromLTRB(math.max(pad.left, 16), math.max(pad.top, 8), math.max(pad.right, 16), math.max(pad.bottom, 8)),
        child: Center(child: ConstrainedBox(constraints: const BoxConstraints(maxWidth: 880), child: sheet)),
      );
    } else if (wide) {
      sheet = Center(child: ConstrainedBox(constraints: BoxConstraints(maxWidth: 440, maxHeight: mq.size.height - 96), child: sheet));
    } else {
      sheet = Padding(padding: EdgeInsets.only(top: top), child: sheet);
    }
    return Semantics(scopesRoute: true, explicitChildNodes: true, label: '发布', child: sheet);
  }

  Widget _strip_() {
    final ps = _photos;
    return SizedBox(
      height: _stripH,
      child: ListView.separated(
        scrollDirection: Axis.horizontal,
        itemCount: ps.length,
        separatorBuilder: (_, _) => const SizedBox(width: 12),
        itemBuilder: (_, i) {
          final ph = ps[i];
          final w = _stripH * _ratioOf(_ratio, ph);
          return SizedBox(
            width: w,
            height: _stripH,
            child: Stack(children: [
              Positioned.fill(child: ClipRRect(borderRadius: BorderRadius.circular(Rd.thumb), child: PhotoArt(ph, cacheWidth: 400))),
              Positioned(
                  left: 6,
                  top: 6,
                  child: Container(
                      width: 22,
                      height: 22,
                      alignment: Alignment.center,
                      decoration: const BoxDecoration(color: Fx.overlay55, shape: BoxShape.circle),
                      child: Text('${i + 1}', style: T.cap(Fx.onPhoto, w: FontWeight.w500)))),
              Positioned(
                  right: -4,
                  top: -4,
                  child: _MiniBtn(
                      key: ValueKey('thumb-remove-$i'),
                      icon: DI.close,
                      label: '移除第 ${i + 1} 张',
                      onTap: () => _remove(i))),
              if (i > 0)
                Positioned(
                    left: -4,
                    bottom: -4,
                    child: _MiniBtn(key: ValueKey('thumb-left-$i'), icon: DI.back, label: '第 ${i + 1} 张前移', onTap: () => _move(i, -1))),
              if (_ids.indexOf(_cur[i]) < _ids.length - 1 && i < ps.length - 1)
                Positioned(
                    right: -4,
                    bottom: -4,
                    child: _MiniBtn(key: ValueKey('thumb-right-$i'), icon: DI.forward, label: '第 ${i + 1} 张后移', onTap: () => _move(i, 1))),
            ]),
          );
        },
      ),
    );
  }

  Widget _presetGrid() {
    final c = context.c;
    return Semantics(
      label: '平台预设',
      container: true,
      child: LayoutBuilder(builder: (context, cons) {
        const gap = 8.0;
        final w = (cons.maxWidth - gap * 2) / 3;
        return Wrap(spacing: gap, runSpacing: gap, children: [
          for (final p in presets)
            Semantics(
              inMutuallyExclusiveGroup: true,
              checked: p == _preset,
              label: '${p.name}，${p.spec}',
              excludeSemantics: true,
              child: GestureDetector(
                key: ValueKey('preset-${p.id}'),
                behavior: HitTestBehavior.opaque,
                onTap: () => _pick(p),
                child: Container(
                  width: w,
                  constraints: const BoxConstraints(minHeight: 64),
                  padding: const EdgeInsets.all(12),
                  decoration: BoxDecoration(
                    color: p == _preset ? c.accentSoft : c.surface,
                    borderRadius: BorderRadius.circular(Rd.card),
                    border: Border.all(color: p == _preset ? c.accent : c.sep, width: p == _preset ? 2 : 1),
                  ),
                  child: Column(crossAxisAlignment: CrossAxisAlignment.start, mainAxisAlignment: MainAxisAlignment.center, children: [
                    Text(p.name, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.body(c.text, w: FontWeight.w500)),
                    const SizedBox(height: 2),
                    Text(p.spec, maxLines: 1, overflow: TextOverflow.ellipsis, style: T.mini(c.text2)),
                  ]),
                ),
              ),
            ),
        ]);
      }),
    );
  }

  /// "6336 × 4224 · 2680 万像素" for a single photo whose size is known.
  String? _sizeDims() {
    if (_photos.length != 1 || _ratio != 'orig') return null;
    final d = exportDims(_photos.first, _size);
    return d == null ? null : exportDimsText(d);
  }

  Widget _infoCard() {
    final c = context.c;
    Widget row(String key, String label, String value, VoidCallback onTap, {bool last = false, String? sub}) => GestureDetector(
          key: ValueKey(key),
          behavior: HitTestBehavior.opaque,
          onTap: onTap,
          child: Container(
            height: 56,
            padding: const EdgeInsets.symmetric(horizontal: 16),
            decoration: BoxDecoration(border: last ? null : Border(bottom: BorderSide(color: c.sep))),
            child: Row(children: [
              Expanded(
                  child: Column(mainAxisAlignment: MainAxisAlignment.center, crossAxisAlignment: CrossAxisAlignment.start, children: [
                Text(label, style: T.body17(c.text)),
                if (sub != null) Text(sub, key: ValueKey('$key-sub'), maxLines: 1, overflow: TextOverflow.ellipsis, style: T.small(c.text2)),
              ])),
              Text(value, key: ValueKey('$key-value'), style: T.body17(c.text2, w: FontWeight.w400)),
              const SizedBox(width: 4),
              DIcon(DI.forward, size: 18, color: c.text2),
            ]),
          ),
        );
    return Container(
      decoration: BoxDecoration(color: c.surface, borderRadius: BorderRadius.circular(Rd.card)),
      clipBehavior: Clip.antiAlias,
      child: Column(children: [
        row('row-ratio', '比例', ratioName(_ratio), () {
          final l = _preset.ratios;
          setState(() => _ratio = l[(l.indexOf(_ratio) + 1) % l.length]);
        }),
        row('row-size', '尺寸', exportSizeName(_size), () async {
          final v = await showExportSizeSheet(context, current: _size, photo: _photos.length == 1 ? _photos.first : null);
          if (v != null && mounted) setState(() => _size = v);
        }, sub: _sizeDims()),
        row('row-loc', '位置信息', _strip ? '已去除' : '保留', () => setState(() => _strip = !_strip), last: !_hdrAvailable && !_p3Available),
        if (_p3Available)
          GestureDetector(
            key: const ValueKey('row-cs'),
            behavior: HitTestBehavior.opaque,
            onTap: () => setState(() => _p3 = !_p3),
            child: Container(
              constraints: const BoxConstraints(minHeight: 56),
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: Sp.s),
              decoration: BoxDecoration(border: Border(top: BorderSide(color: c.sep))),
              child: Row(children: [
                Expanded(
                    child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                  // plain words for the colour space (audit C): 更鲜艳 = wide gamut (P3), 标准 = sRGB; no explanation sentence (R17)
                  Text('颜色', style: T.body17(c.text)),
                ])),
                const SizedBox(width: 12),
                Text(_p3 ? '更鲜艳' : '标准', key: const ValueKey('row-cs-value'), style: T.body17(c.text2, w: FontWeight.w400)),
                const SizedBox(width: 4),
                DIcon(DI.forward, size: 18, color: c.text2),
              ]),
            ),
          ),
        if (_hdrAvailable)
          Container(
            key: const ValueKey('row-hdr'),
            constraints: const BoxConstraints(minHeight: 56),
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: Sp.s),
            decoration: BoxDecoration(border: Border(top: BorderSide(color: c.sep))),
            child: Row(children: [
              Expanded(
                  child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
                // HDR in plain words (audit C): on a screen that can, the highlights are brighter than white
                Text('高光更亮', style: T.body17(c.text)),
              ])),
              const SizedBox(width: 12),
              DSwitch(key: const ValueKey('switch-hdr'), value: _hdr, label: '高光更亮', onChanged: (v) => setState(() => _hdr = v)),
            ]),
          ),
      ]),
    );
  }
}

class _Warn {
  _Warn(this.text, {this.actionLabel, this.onAction});
  final String text;
  final String? actionLabel;
  final VoidCallback? onAction;
}

class _WarnCard extends StatelessWidget {
  const _WarnCard(this.w);
  final _Warn w;
  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Container(
      padding: const EdgeInsets.fromLTRB(16, 12, 8, 12),
      decoration: BoxDecoration(color: c.warnBg, borderRadius: BorderRadius.circular(Rd.card)),
      child: Row(children: [
        DIcon(DI.warn, size: 20, color: c.warn),
        const SizedBox(width: 12),
        Expanded(child: Text(w.text, style: T.small(c.warn))),
        if (w.actionLabel != null)
          GestureDetector(
            key: const ValueKey('warn-split'),
            behavior: HitTestBehavior.opaque,
            onTap: w.onAction,
            child: ConstrainedBox(
              constraints: const BoxConstraints(minHeight: kHit, minWidth: kHit),
              child: Center(
                  widthFactor: 1,
                  child: Padding(padding: const EdgeInsets.symmetric(horizontal: 8), child: Text(w.actionLabel!, style: T.small(c.warn, w: FontWeight.w500)))),
            ),
          ),
      ]),
    );
  }
}

class _MiniBtn extends StatelessWidget {
  const _MiniBtn({super.key, required this.icon, required this.label, required this.onTap});
  final DI icon;
  final String label;
  final VoidCallback onTap;
  @override
  Widget build(BuildContext context) {
    return Semantics(
      button: true,
      label: label,
      excludeSemantics: true,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: onTap,
        child: SizedBox(
          width: 44,
          height: 44,
          child: Center(
            child: Container(
              width: 26,
              height: 26,
              decoration: const BoxDecoration(color: Fx.overlay55, shape: BoxShape.circle),
              alignment: Alignment.center,
              child: DIcon(icon, size: 14, color: Fx.onPhoto, stroke: 2.4),
            ),
          ),
        ),
      ),
    );
  }
}

class _HandoffRow extends StatelessWidget {
  const _HandoffRow({super.key, required this.label, required this.onTap});
  final String label;
  final VoidCallback onTap;
  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return InkWell(
      onTap: onTap,
      child: Container(
        height: 56,
        alignment: Alignment.centerLeft,
        child: Row(children: [
          Expanded(child: Text(label, style: T.body17(c.text))),
          DIcon(DI.forward, size: 18, color: c.text2),
        ]),
      ),
    );
  }
}

class _ExportDialog extends StatelessWidget {
  const _ExportDialog({required this.progress, required this.onCancel});
  final ValueNotifier<double> progress;
  final VoidCallback onCancel;
  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return Dialog(
      backgroundColor: c.surface,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(Rd.dialog)),
      child: Semantics(
        namesRoute: true,
        label: '正在导出',
        child: Padding(
          padding: const EdgeInsets.all(24),
          child: ValueListenableBuilder<double>(
              valueListenable: progress,
              builder: (_, v, _) => Column(mainAxisSize: MainAxisSize.min, crossAxisAlignment: CrossAxisAlignment.start, children: [
                    Text('正在导出', style: T.h2(c.text)),
                    const SizedBox(height: 16),
                    DProgress(v, height: 6),
                    const SizedBox(height: 8),
                    Align(alignment: Alignment.centerRight, child: TextAction('取消', onPressed: onCancel)),
                  ])),
        ),
      ),
    );
  }
}
