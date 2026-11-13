// The film map (docs/design/editor-panel.md section 7): every catalog stock is a dot at its character (x pale..rich, y low..high
// contrast), coloured by type. Pressing magnifies the neighbourhood of the finger (2.4x, animated) and shows the names;
// sliding moves to the nearest stock and the photo follows live; releasing keeps it and the map shrinks back.
import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show rootBundle;

import '../../model/film_catalog.dart';
import '../../model/film_map.dart';
import '../../model/pad_model.dart' show PadIconKind;
import '../../theme/tokens.dart';
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
const Color kNegativeColor = Color(0xFFC98A4A), kSlideColor = Color(0xFF3F7FD0), kCinemaColor = Color(0xFF4F9A8C);

Color dotColorOf(FilmStock s) => switch (dotTypeOf(s)) {
      FilmDotType.negative => kNegativeColor,
      FilmDotType.slide => kSlideColor,
      FilmDotType.cinema => kCinemaColor,
    };

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
    _zoom.dispose();
    super.dispose();
  }

  void _pick(Offset local) {
    if (_size.isEmpty) return;
    final at = Offset(local.dx / _size.width, local.dy / _size.height);
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
                  listenable: Listenable.merge([_t, ctl.app.editTick]),
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
    final z = 1 + (kMagnify - 1) * _t.value;
    final sel = ctl.edit.film;
    final w = _size.width, h = _size.height;
    final dots = <Widget>[];
    // the selected dot last so it is never hidden
    final ordered = [..._dots.where((d) => d.id != sel), ..._dots.where((d) => d.id == sel)];
    for (final d in ordered) {
      final f = dotFraction(d);
      final on = d.id == sel;
      final showName = on || _t.value > 0.35;
      dots.add(Positioned(
        left: f.dx * w,
        top: f.dy * h,
        child: FractionalTranslation(
          translation: const Offset(-0.5, -0.5),
          child: Transform.scale(
            scale: 1 / z,
            child: _Dot(key: ValueKey('stock-${d.id}'), color: dotColorOf(d.stock), selected: on, name: d.name, showName: showName, nameOpacity: on ? 1 : ((_t.value - 0.35) / 0.4).clamp(0.0, 1.0), c: c),
          ),
        ),
      ));
    }
    return Transform(
      key: const ValueKey('film-map-zoom'),
      alignment: Alignment(_origin.dx * 2 - 1, _origin.dy * 2 - 1),
      transform: Matrix4.diagonal3Values(z, z, 1),
      child: SizedBox(width: w, height: h, child: Stack(clipBehavior: Clip.none, children: dots)),
    );
  }

  /// Test hook: the current magnification.
  @visibleForTesting
  double get magnification => 1 + (kMagnify - 1) * _t.value;
}

class _Dot extends StatelessWidget {
  const _Dot({super.key, required this.color, required this.selected, required this.name, required this.showName, required this.nameOpacity, required this.c});
  final Color color;
  final bool selected, showName;
  final String name;
  final double nameOpacity;
  final DC c;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 18,
      height: 18,
      child: Stack(clipBehavior: Clip.none, alignment: Alignment.center, children: [
        Container(
          width: 18,
          height: 18,
          decoration: BoxDecoration(
            color: color,
            shape: BoxShape.circle,
            boxShadow: [
              BoxShadow(color: c.surface, spreadRadius: selected ? 2 : 1.5),
              if (selected) BoxShadow(color: c.accent, spreadRadius: 4.5),
            ],
          ),
        ),
        if (showName)
          Positioned(
            top: 18 + 5,
            left: -40,
            right: -40,
            child: Opacity(
              opacity: nameOpacity,
              child: Center(child: Text(name, softWrap: false, style: TextStyle(fontSize: 10.5, height: 1.1, fontWeight: selected ? FontWeight.w600 : FontWeight.w400, color: c.text))),
            ),
          ),
      ]),
    );
  }
}
