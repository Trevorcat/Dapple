import 'dart:async';
import 'dart:ui' as ui;

import 'package:flutter/material.dart';

import '../../model/bases.dart';
import '../../model/film_catalog.dart';
import '../../models.dart';
import '../../services/base_swatch_sources.dart';
import '../../services/base_swatches.dart' show SwatchSource;
import '../../state/app_state.dart';
import '../../theme/tokens.dart';
import '../../widgets/film_canister.dart';
import '../../widgets/float_bar.dart';
import '../../widgets/photo_art.dart';
import '../editor/film_map_view.dart' show FilmMapStore;
import 'page_bar.dart';

/// 成像 tab (默认成像): the rendering base new photos start with, chosen among every film stock, the three modern renderings and
/// 标准 (none), each shown as a drawn icon (widgets/film_canister.dart: nothing is computed per photo for the picker). The card on
/// top keeps ONE real engine rendering of the current default on the latest developed photo (the bundled sample before there is one).
/// The nine colour looks are not chosen here any more: they are the editor's 颜色 (a look on top of any base).
class StylesPage extends StatefulWidget {
  const StylesPage({super.key, required this.app, required this.wide, this.swatchSource});
  final AppState app;
  final bool wide;

  /// Test hook: where the large preview comes from (default: the engine).
  final SwatchSource Function(Photo? photo)? swatchSource;

  @override
  State<StylesPage> createState() => _StylesPageState();
}

const kSampleAsset = 'assets/style_sample/sample.jpg';

class _StylesPageState extends State<StylesPage> {
  SwatchSource? _src;
  String _srcKey = '';
  ui.Image? _preview; // the large rendering of the current default
  String? _previewFor; // `${source key}|${base}` of [_preview]
  int _gen = 0;
  bool _scrolled = false; // the grid went to the current default once
  final GlobalKey _selKey = GlobalKey();

  AppState get app => widget.app;

  /// The picture the swatches show: the latest developed photo with a readable file, else null (the bundled sample).
  Photo? get _photo {
    Photo? best;
    for (final p in app.photos) {
      if (p.status == PhotoStatus.done && p.demo == null && p.path != null && (best == null || p.addedMs > best.addedMs)) best = p;
    }
    return best;
  }

  /// Follows the latest photo; renders the one large preview once the tab is shown (the engine is not asked for a tab nobody looks at).
  void _sync() {
    final ph = _photo;
    final key = ph == null ? 'sample' : '${ph.id}|${ph.path}';
    if (key != _srcKey) {
      final old = _src;
      if (old is AssetSwatchSource) unawaited(old.close());
      _srcKey = key;
      _src = widget.swatchSource?.call(ph) ?? (ph == null ? AssetSwatchSource(kSampleAsset) : SessionSwatchSource(app, ph));
      _preview = null;
      _previewFor = null;
    }
    if (app.tab == 1) {
      _loadPreview();
      if (!_scrolled) {
        _scrolled = true;
        // the current default is visible without scrolling: the grid goes to it once, when the tab is first shown
        WidgetsBinding.instance.addPostFrameCallback((_) {
          final ctx = _selKey.currentContext;
          if (mounted && ctx != null) Scrollable.ensureVisible(ctx, alignment: 0.3, duration: Duration.zero);
        });
      }
    }
  }

  void _loadPreview() {
    final base = app.settings.defaultBaseId;
    final want = '$_srcKey|$base';
    if (_previewFor == want) return;
    _previewFor = want;
    final gen = ++_gen;
    unawaited(() async {
      ui.Image? img;
      try {
        img = await _src!.render(base, 640);
      } catch (_) {}
      if (!mounted || gen != _gen) {
        img?.dispose();
        return;
      }
      if (img != null) setState(() => _preview = img);
    }());
  }

  @override
  void initState() {
    super.initState();
    if (FilmMapStore.positions == null) {
      FilmMapStore.load().then((_) {
        if (mounted) setState(() {});
      });
    }
  }

  @override
  void dispose() {
    final s = _src;
    if (s is AssetSwatchSource) unawaited(s.close());
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    _sync();
    final c = context.c;
    return ColoredBox(
      color: c.bg,
      child: Column(
        children: [
          const PageBar('成像'),
          Expanded(child: widget.wide ? _wideBody(context) : _phoneBody(context)),
        ],
      ),
    );
  }

  // ---------------------------------------------------------------- the preview card

  Widget _picture() {
    final ph = _photo;
    final under = ph == null ? Image.asset(kSampleAsset, fit: BoxFit.cover, cacheWidth: 600, gaplessPlayback: true) : PhotoArt(ph, cacheWidth: 600);
    final img = _preview;
    return Stack(
      fit: StackFit.expand,
      children: [
        under,
        AnimatedOpacity(
          opacity: img == null ? 0 : 1,
          duration: Mo.medium,
          child: img == null
              ? const SizedBox.shrink()
              : RawImage(key: const ValueKey('default-image'), image: img, fit: BoxFit.cover, filterQuality: FilterQuality.medium),
        ),
      ],
    );
  }

  /// One line under the name: a film's group, a modern rendering's character, the standard's "no lean".
  String _kindOf(String? id) {
    final m = modernById(id);
    if (m != null) return m.blurb;
    final s = app.film.stock(id);
    return s == null ? '不加倾向' : filmGroupNames[s.group]!;
  }

  /// The icon of a base; a stock's character comes from the film map asset.
  BaseIconData _iconOf(String? id) => baseIconOf(app.film, id, character: FilmMapStore.positions?[id]);

  Widget _card({double? height}) {
    final c = context.c;
    final def = app.settings.defaultBaseId;
    return Container(
      key: const ValueKey('default-card'),
      decoration: BoxDecoration(color: c.surface, borderRadius: BorderRadius.circular(Rd.card)),
      clipBehavior: Clip.antiAlias,
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          SizedBox(
            key: const ValueKey('default-preview'),
            height: height ?? 190,
            width: double.infinity,
            child: Stack(fit: StackFit.expand, children: [
              _picture(),
              // no developed photo yet: the picture is a sample, said by a small label on it (not a sentence, audit F38)
              if (_photo == null)
                Positioned(
                  left: Sp.s,
                  top: Sp.s,
                  child: Container(
                    key: const ValueKey('sample-pill'),
                    height: 20,
                    padding: const EdgeInsets.symmetric(horizontal: Sp.s),
                    alignment: Alignment.center,
                    decoration: BoxDecoration(color: Fx.overlay55, borderRadius: BorderRadius.circular(Rd.thumb)),
                    child: Text('示例', style: T.cap(Fx.onPhoto, w: FontWeight.w500)),
                  ),
                ),
            ]),
          ),
          Padding(
            padding: const EdgeInsets.all(Sp.l),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text('新照片默认成像', style: T.small(c.text2)),
                const SizedBox(height: Sp.s),
                Row(
                  children: [
                    BaseIcon(key: const ValueKey('default-icon'), data: _iconOf(def), size: 48),
                    const SizedBox(width: Sp.m),
                    Expanded(
                      child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Text(baseNameOf(app.film, def), key: const ValueKey('default-name'), style: T.h2(c.text)),
                          Text(_kindOf(def), style: T.small(c.text2)),
                        ],
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  // ---------------------------------------------------------------- the grid

  Widget _tile(String? id, String name, {required double size}) {
    final c = context.c;
    final selected = app.settings.defaultBaseId == id;
    return KeyedSubtree(
      key: selected ? _selKey : null,
      child: Semantics(
        inMutuallyExclusiveGroup: true,
        checked: selected,
        label: name,
        excludeSemantics: true,
        child: InkWell(
          key: ValueKey('base-${id ?? 'none'}'),
          borderRadius: BorderRadius.circular(Rd.tile),
          onTap: () => app.setDefaultBase(id),
          child: SizedBox(
            width: size + 16,
            child: Padding(
              padding: const EdgeInsets.fromLTRB(8, 10, 8, 4),
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  BaseIcon(data: _iconOf(id), size: size, selected: selected),
                  const SizedBox(height: 8),
                  Text(
                    name,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    textAlign: TextAlign.center,
                    style: T.cap(selected ? c.text : c.text2).copyWith(fontWeight: selected ? FontWeight.w600 : FontWeight.w400),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }

  Widget _group(String title, List<Widget> tiles, {String? key}) {
    final c = context.c;
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(Sp.l, Sp.l, Sp.l, 0),
          child: Text(
            title,
            key: key == null ? null : ValueKey(key),
            style: T.small(c.text2, w: FontWeight.w500),
          ),
        ),
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: Sp.s),
          child: Wrap(children: tiles),
        ),
      ],
    );
  }

  Widget _grid(double width) {
    const size = 64.0;
    final film = app.film;
    // the standard and the three modern renderings first (four tiles in one group), then the film library
    final sections = <Widget>[
      // one tile is not a group: 标准 leads the modern renderings (the card above already says what is chosen)
      _group('标准与现代', [_tile(null, '标准', size: size), for (final m in kModernBases) _tile(m.id, m.name, size: size)], key: 'group-modern'),
      if (!film.isEmpty)
        for (final g in FilmGroup.values)
          if (film.inGroup(g).isNotEmpty)
            _group(g == FilmGroup.negative ? '胶片 · ${filmGroupNames[g]}' : '${filmGroupNames[g]}', [
              for (final s in film.inGroup(g)) _tile(s.id, s.name, size: size),
            ], key: 'group-${g.name}'),
    ];
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: sections);
  }

  Widget _phoneBody(BuildContext context) {
    return ListView(
      padding: EdgeInsets.only(bottom: floatBarClearance(context)),
      children: [
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: Sp.l),
          child: _card(),
        ),
        Semantics(container: true, label: '默认成像', child: _grid(MediaQuery.sizeOf(context).width)),
      ],
    );
  }

  Widget _wideBody(BuildContext context) {
    return Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        SizedBox(
          width: 340,
          // low windows (phone landscape): the sample column scrolls instead of overflowing, the preview shrinks with the height
          child: LayoutBuilder(
            builder: (context, box) => SingleChildScrollView(
              padding: const EdgeInsets.all(Sp.l),
              child: _card(height: (box.maxHeight - 2 * Sp.l - 130).clamp(110.0, 240.0)),
            ),
          ),
        ),
        Expanded(
          child: ListView(
            padding: const EdgeInsets.only(bottom: 120),
            children: [
              Semantics(container: true, label: '默认成像', child: _grid(0)),
                  ],
          ),
        ),
      ],
    );
  }
}
