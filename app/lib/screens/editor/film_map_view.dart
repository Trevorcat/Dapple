// The film map (docs/design/editor-panel.md section 7): every catalog stock is a film-cartridge icon at its character (x pale..rich,
// y low..high contrast), drawn in the app (widgets/film_canister.dart): no engine rendering, nothing computed per photo. The three
// modern renderings are round lens icons at their measured places (kModernCluster when the asset has none). Pressing magnifies the neighbourhood of the finger (2.4x, animated), shows the
// names and puts the swatch nearest the finger on top; sliding moves to the nearest centre and the photo follows live;
// releasing keeps it and the map shrinks back.
import 'dart:math' as math;
import 'dart:ui' as ui;

import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show rootBundle;

import '../../model/film_map.dart';
import '../../model/pad_model.dart' show PadIconKind;
import '../../theme/tokens.dart';
import '../../widgets/film_canister.dart';
import 'editor_controller.dart';
import 'pad_gestures.dart';
import 'pad_icons.dart';

/// Loads assets/film/film_map.json once.
class FilmMapStore {
  static Map<String, Offset>? _positions;
  static Future<Map<String, Offset>>? _loading;

  /// Test hook: replace the positions.
  static void debugSet(Map<String, Offset>? p) {
    _positions = p;
    _loading = null;
  }

  static Map<String, Offset>? get positions => _positions;

  static Future<Map<String, Offset>> load() {
    return _loading ??= () async {
      String? text;
      try {
        text = await rootBundle.loadString(kFilmMapAsset);
      } catch (_) {
        text = null; // no asset: every stock goes on the automatic grid
      }
      return _positions = parseFilmMap(text);
    }();
  }
}

const double kMagnify = 2.4;

/// Icon size on screen: overview and magnified.
const double kSwatchOverview = 23, kSwatchMagnified = 40;

/// The icon of a map entry: film by its measured character, a modern rendering by its id.
BaseIconData iconOf(FilmDot d) => d.stock == null
    ? BaseIconData.modern(d.id)
    : BaseIconData.film(CanisterSpec.forStock(d.stock!, character: d.placed ? d.character : null));

/// Map-fraction position of a dot (0..1 of the map's width and height).
Offset dotFraction(FilmDot d) => Offset(0.5 + d.shown.dx * kFilmMapX, 0.5 - d.shown.dy * kFilmMapY);

/// The dot nearest to the finger when the map is magnified by [z] around [origin] (both as fractions of the map): the finger at
/// [at] (fraction) stands for the point origin + (at - origin) / z of the unmagnified map. [size]: the map in dp.
FilmDot? pickDot(List<FilmDot> dots, Size size, Offset origin, double z, Offset at) {
  final u = Offset(origin.dx + (at.dx - origin.dx) / z, origin.dy + (at.dy - origin.dy) / z);
  FilmDot? best;
  var bd = double.infinity;
  for (final d in dots) {
    final f = dotFraction(d);
    final dx = (f.dx - u.dx) * size.width, dy = (f.dy - u.dy) * size.height;
    final dd = dx * dx + dy * dy;
    if (dd < bd) {
      bd = dd;
      best = d;
    }
  }
  return best;
}

/// Paint order of the dots while the finger is at [u] (unmagnified map fraction, null = no finger): furthest first, so the one
/// nearest the finger is drawn last (on top); the selected [sel] is always last of all.
List<FilmDot> paintOrder(List<FilmDot> dots, Size size, Offset? u, String? sel) {
  double dist(FilmDot d) {
    if (u == null) return 0;
    final p = dotFraction(d);
    return Offset((p.dx - u.dx) * size.width, (p.dy - u.dy) * size.height).distanceSquared;
  }

  return [...dots]
    ..sort((a, b) {
      if ((a.id == sel) != (b.id == sel)) return a.id == sel ? 1 : -1;
      return dist(b).compareTo(dist(a));
    });
}

class FilmMapView extends StatefulWidget {
  const FilmMapView({super.key, required this.ctl});
  final EditorController ctl;

  @override
  State<FilmMapView> createState() => _FilmMapViewState();
}

class _FilmMapViewState extends State<FilmMapView> with SingleTickerProviderStateMixin {
  late final AnimationController _zoom = AnimationController(vsync: this, duration: const Duration(milliseconds: 260));
  late final Animation<double> _t = CurvedAnimation(parent: _zoom, curve: Curves.easeOutCubic, reverseCurve: Curves.easeInCubic);
  List<FilmDot> _dots = const [];
  Size _size = Size.zero;
  Offset _origin = const Offset(0.5, 0.5);
  bool _active = false;
  String? _last;
  final ValueNotifier<Offset?> _finger = ValueNotifier(null); // the finger as a fraction of the map

  EditorController get ctl => widget.ctl;

  @override
  void initState() {
    super.initState();
    _layout();
    if (FilmMapStore.positions == null) {
      FilmMapStore.load().then((_) {
        if (mounted) setState(_layout);
      });
    }
  }

  void _layout() {
    _dots = layoutFilmMap(ctl.app.film, FilmMapStore.positions ?? const {});
  }

  @override
  void dispose() {
    if (_active) ctl.stockEnd();
    _finger.dispose();
    _zoom.dispose();
    super.dispose();
  }

  void _pick(Offset local) {
    if (_size.isEmpty) return;
    final at = Offset(local.dx / _size.width, local.dy / _size.height);
    _finger.value = at;
    final d = pickDot(_dots, _size, _origin, kMagnify, at);
    if (d != null && d.id != _last) {
      _last = d.id;
      ctl.stockSet(d.id);
    }
  }

  void _down(Offset p) {
    if (_size.isEmpty || _dots.isEmpty) return;
    _active = true;
    _last = ctl.edit.film;
    _origin = Offset((p.dx / _size.width).clamp(0.0, 1.0), (p.dy / _size.height).clamp(0.0, 1.0));
    ctl.stockBegin();
    _zoom.forward();
    setState(() {});
    _pick(p);
  }

  void _move(Offset p) {
    if (_active) _pick(p);
  }

  void _end(Offset p, bool cancelled) {
    if (!_active) return;
    _active = false;
    final shrink = _zoom.reverse();
    _finger.value = null;
    ctl.stockEnd();
    if (cancelled) return;
    // the choice is final: once the map has shrunk back, return to the film pad
    shrink.whenComplete(() {
      if (mounted && !_active && ctl.filmMapOpen) ctl.setFilmMap(false);
    });
  }

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    return ClipRRect(
      borderRadius: BorderRadius.circular(18),
      child: ColoredBox(
        color: c.bg,
        child: LayoutBuilder(builder: (context, cons) {
          _size = Size(cons.maxWidth, cons.maxHeight);
          return Stack(fit: StackFit.expand, children: [
            ClaimSurface(
              onDown: _down,
              onMove: _move,
              onEnd: _end,
              child: RepaintBoundary(
                child: ListenableBuilder(
                  listenable: Listenable.merge([_t, ctl.app.editTick, _finger]),
                  builder: (context, _) => _inner(context, c),
                ),
              ),
            ),
            // the four character icons stay put, and fade out while magnified (they would sit in the middle of the stocks)
            IgnorePointer(child: AnimatedBuilder(animation: _t, builder: (context, _) => Opacity(key: const ValueKey('film-map-edges'), opacity: (1 - _t.value * 1.6).clamp(0.0, 1.0), child: _edges(c)))),
          ]);
        }),
      ),
    );
  }

  Widget _edges(DC c) {
    const k = [PadIconKind.pale, PadIconKind.rich, PadIconKind.contrastLow, PadIconKind.contrastHigh];
    final col = c.text2.withValues(alpha: 0.8);
    Widget ic(PadIconKind kind) => PadIcon(kind, color: col);
    return Stack(children: [
      Positioned(left: 8, top: 0, bottom: 0, child: Center(child: ic(k[0]))),
      Positioned(right: 8, top: 0, bottom: 0, child: Center(child: ic(k[1]))),
      Positioned(bottom: 6, left: 0, right: 0, child: Center(child: ic(k[2]))),
      Positioned(top: 6, left: 0, right: 0, child: Center(child: ic(k[3]))),
    ]);
  }

  Widget _inner(BuildContext context, DC c) {
    final t = _t.value;
    final z = 1 + (kMagnify - 1) * t;
    final sel = ctl.edit.film;
    final w = _size.width, h = _size.height;
    final size = ui.lerpDouble(kSwatchOverview, kSwatchMagnified, t)!;
    final f = _finger.value;
    final u = f == null ? null : Offset(_origin.dx + (f.dx - _origin.dx) / z, _origin.dy + (f.dy - _origin.dy) / z);
    final children = <Widget>[];
    // The modern renderings: circles (film is rounded squares). Where their measured characters sit close together (a tight cluster) a soft plate
    // behind them says "one family that is not film"; once they are spread over the map (measured positions) a plate would only cover
    // unrelated stocks, so there is none.
    final mod = [for (final d in _dots) if (d.isModern) dotFraction(d)];
    final tight = mod.isNotEmpty &&
        mod.map((p) => p.dx).reduce(math.max) - mod.map((p) => p.dx).reduce(math.min) < 0.3 &&
        mod.map((p) => p.dy).reduce(math.max) - mod.map((p) => p.dy).reduce(math.min) < 0.3;
    if (tight) {
      var l = double.infinity, tp = double.infinity, r = -double.infinity, bt = -double.infinity;
      for (final p in mod) {
        l = math.min(l, p.dx * w);
        r = math.max(r, p.dx * w);
        tp = math.min(tp, p.dy * h);
        bt = math.max(bt, p.dy * h);
      }
      const pad = 19.0;
      children.add(Positioned(
        key: const ValueKey('modern-plate'),
        left: l - pad,
        top: tp - pad - 9,
        width: r - l + 2 * pad,
        height: bt - tp + 2 * pad + 9,
        child: IgnorePointer(
          child: DecoratedBox(
            decoration: BoxDecoration(color: c.text.withValues(alpha: 0.05), borderRadius: BorderRadius.circular(14), border: Border.all(color: c.text.withValues(alpha: 0.12), width: 0.5)),
            child: Align(
              alignment: Alignment.topLeft,
              child: Padding(
                padding: const EdgeInsets.only(left: 8, top: 3),
                child: Transform.scale(alignment: Alignment.topLeft, scale: 1 / z, child: Text('现代', style: TextStyle(fontSize: 9, height: 1, color: c.text2))),
              ),
            ),
          ),
        ),
      ));
    }
    for (final d in paintOrder(_dots, _size, u, sel)) {
      final p = dotFraction(d);
      final on = d.id == sel;
      final showName = on || t > 0.35;
      children.add(Positioned(
        left: p.dx * w,
        top: p.dy * h,
        child: FractionalTranslation(
          translation: const Offset(-0.5, -0.5),
          child: Transform.scale(
            scale: 1 / z,
            child: SizedBox(
              width: kSwatchMagnified,
              height: kSwatchMagnified,
              child: Stack(clipBehavior: Clip.none, alignment: Alignment.center, children: [
                RepaintBoundary(child: BaseIcon(key: ValueKey('stock-${d.id}'), data: iconOf(d), size: size, selected: on)),
                if (showName)
                  Positioned(
                    top: kSwatchMagnified / 2 + size / 2 + 4,
                    left: -40,
                    right: -40,
                    child: Opacity(
                      opacity: on ? 1 : ((t - 0.35) / 0.4).clamp(0.0, 1.0),
                      child: Center(child: Text(d.name, softWrap: false, style: TextStyle(fontSize: 10.5, height: 1.1, fontWeight: on ? FontWeight.w600 : FontWeight.w400, color: c.text))),
                    ),
                  ),
              ]),
            ),
          ),
        ),
      ));
    }
    return Transform(
      key: const ValueKey('film-map-zoom'),
      alignment: Alignment(_origin.dx * 2 - 1, _origin.dy * 2 - 1),
      transform: Matrix4.diagonal3Values(z, z, 1),
      child: SizedBox(width: w, height: h, child: Stack(clipBehavior: Clip.none, children: children)),
    );
  }

  /// Test hook: the current magnification.
  @visibleForTesting
  double get magnification => 1 + (kMagnify - 1) * _t.value;
}
