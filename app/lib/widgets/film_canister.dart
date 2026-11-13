// Icons of the rendering bases (film map, 成像 page): every film stock is a self-drawn generic 135 film cartridge, the three
// modern renderings a lens-like disc, 标准 a plain neutral frame. Pure vector (CustomPainter), nothing downloaded, no real
// packaging, lettering or brand colours: the cartridge is a generic silhouette (spool stubs, two caps, a body with a label band,
// the light-trap lip with a short film leader).
//
// Per-stock variation comes from data, not by hand ([CanisterSpec.forStock]): the colour family from the stock's type, its hue
// nudged by the measured character (x pale..rich, y low..high contrast), the saturation and lightness of the body following the
// richness (pale stocks look pale, rich ones punchy) and the depth of the caps following the contrast. The kind shows in the
// form: 负片 = dark caps and a label with a stripe; 反转片 = silver caps and a slide-mount window on the label; 电影 = a dark
// film strip with perforation holes around the label. The code (N400A...) is written on the label from 40 px on.
// Three visual styles ([CanisterStyle]) share the geometry; [kCanisterStyle] is the one the app uses.
import 'dart:math' as math;

import 'package:flutter/material.dart';

import '../model/bases.dart';
import '../model/film_catalog.dart';
import '../theme/tokens.dart';
import 'film_canister_palette.dart';

/// flat: two-tone solid fills. soft: a little cylinder shading and highlight. outline: tinted label and body, drawn as lines.
enum CanisterStyle { flat, soft, outline }

/// The style the app uses: change this one line to switch (map, 成像 page and the card all follow).
const CanisterStyle kCanisterStyle = CanisterStyle.soft;

enum CanisterKind { negative, slide, cinema }

Color _hsl(double h, double s, double l) => HSLColor.fromAHSL(1, ((h % 360) + 360) % 360, s.clamp(0.0, 1.0), l.clamp(0.0, 1.0)).toColor();

Color _darken(Color c, double toL) {
  final h = HSLColor.fromColor(c);
  return h.withLightness(math.min(h.lightness, toL)).toColor();
}

Color _lighten(Color c, double toL) {
  final h = HSLColor.fromColor(c);
  return h.withLightness(math.max(h.lightness, toL)).toColor();
}

/// What one cartridge looks like, all derived: see [CanisterSpec.forStock].
class CanisterSpec {
  const CanisterSpec({required this.kind, required this.body, required this.label, required this.cap, required this.accent, required this.ink, this.code = ''});
  final CanisterKind kind;
  final Color body, label, cap, accent, ink;
  final String code;

  static const _typeHue = {CanisterKind.negative: 32.0, CanisterKind.slide: 214.0, CanisterKind.cinema: 168.0};

  /// [richness] and [contrast] are the measured character of the stock, -1..1 (pale..rich, low..high contrast).
  factory CanisterSpec.of({required CanisterKind kind, required double richness, required double contrast, String code = ''}) {
    final r = richness.clamp(-1.0, 1.0), c = contrast.clamp(-1.0, 1.0);
    final t = (r + 1) / 2; // 0 pale .. 1 rich
    final u = (c + 1) / 2; // 0 flat .. 1 contrasty
    final hue = _typeHue[kind]! + 40 * r + 22 * c;
    final sat = 0.28 + 0.52 * t;
    var bodyL = 0.76 - 0.22 * t - 0.05 * c;
    if (kind == CanisterKind.cinema) bodyL -= 0.08;
    final body = _hsl(hue, sat, bodyL);
    final label = _hsl(hue, 0.30 * (0.4 + 0.6 * t), 0.94 - 0.03 * t);
    final cap = kind == CanisterKind.slide ? _hsl(hue, 0.10, 0.84) : _hsl(hue - 6, 0.22 + 0.2 * t, 0.36 - 0.14 * u);
    final accent = _hsl(hue - 26, (sat + 0.1).clamp(0.45, 0.9), 0.46 + 0.06 * (1 - t));
    final ink = _hsl(hue, 0.35, 0.16);
    return CanisterSpec(kind: kind, body: body, label: label, cap: cap, accent: accent, ink: ink, code: code);
  }

  /// The spec of a catalog stock: its designed colours ([kCanisterPalette], keyed by the display code) when it has an entry, else the data-driven colours of [CanisterSpec.of]
  /// from [character] (its place on the film map, x, y in -1..1; null = unknown/neutral). [designed] false forces the data-driven ones.
  factory CanisterSpec.forStock(FilmStock s, {Offset? character, bool designed = true}) {
    final kind = switch (s.group) { FilmGroup.negative => CanisterKind.negative, FilmGroup.slide => CanisterKind.slide, FilmGroup.cinema => CanisterKind.cinema };
    final p = designed ? kCanisterPalette[s.name] : null;
    if (p == null) return CanisterSpec.of(kind: kind, richness: character?.dx ?? 0, contrast: character?.dy ?? 0, code: s.name);
    final h = HSLColor.fromColor(p.body).hue;
    return CanisterSpec(kind: kind, body: p.body, label: _hsl(h, 0.22, 0.95), cap: p.cap, accent: p.accent, ink: _hsl(h, 0.35, 0.16), code: s.name);
  }
}

/// What a base icon shows: a film stock, a modern rendering, or 标准.
class BaseIconData {
  const BaseIconData.film(CanisterSpec this.spec) : modern = null;
  const BaseIconData.modern(String this.modern) : spec = null;
  const BaseIconData.none() : spec = null, modern = null;
  final CanisterSpec? spec;
  final String? modern;
  bool get isFilm => spec != null;

  /// The modern renderings are circles (the map's silhouette for "not film"); film and 标准 are squares.
  bool get round => modern != null;
}

/// Icon data of a base id (null = 标准); [character] = the stock's place on the film map when known.
BaseIconData baseIconOf(FilmCatalog film, String? id, {Offset? character}) {
  if (isModernId(id)) return BaseIconData.modern(id!);
  final s = film.stock(id);
  if (s == null) return const BaseIconData.none();
  return BaseIconData.film(CanisterSpec.forStock(s, character: character));
}

/// One base as an icon laid out at [size]; [selected] puts the accent tile behind it.
class BaseIcon extends StatelessWidget {
  const BaseIcon({super.key, required this.data, this.size = 40, this.style = kCanisterStyle, this.selected = false});
  final BaseIconData data;
  final double size;
  final CanisterStyle style;
  final bool selected;

  @override
  Widget build(BuildContext context) {
    final c = context.c;
    final dpr = MediaQuery.maybeDevicePixelRatioOf(context) ?? 2.0;
    return SizedBox(
      width: size,
      height: size,
      child: DecoratedBox(
        decoration: BoxDecoration(
          shape: data.round ? BoxShape.circle : BoxShape.rectangle,
          borderRadius: data.round ? null : BorderRadius.circular(size * 0.26),
          color: selected ? c.accentSoft : null,
          border: selected ? Border.all(color: c.accent, width: 2) : null,
        ),
        child: CustomPaint(size: Size.square(size), painter: _IconPainter(data, style, c.dark, dpr, DefaultTextStyle.of(context).style.fontFamily)),
      ),
    );
  }
}

/// The cartridge alone (previews, tests).
class FilmCanister extends StatelessWidget {
  const FilmCanister({super.key, required this.spec, this.size = 40, this.style = kCanisterStyle});
  final CanisterSpec spec;
  final double size;
  final CanisterStyle style;
  @override
  Widget build(BuildContext context) => BaseIcon(data: BaseIconData.film(spec), size: size, style: style);
}

class _IconPainter extends CustomPainter {
  _IconPainter(this.data, this.style, this.dark, this.dpr, this.family);
  final BaseIconData data;
  final CanisterStyle style;
  final bool dark;
  final double dpr;
  final String? family;

  @override
  void paint(Canvas canvas, Size size) {
    final s = math.min(size.width, size.height);
    final g = _Geo(s, dpr);
    canvas.save();
    canvas.translate((size.width - s) / 2, (size.height - s) / 2);
    if (data.spec != null) {
      _canister(canvas, g, data.spec!);
    } else if (data.modern != null) {
      _disc(canvas, g, data.modern!);
    } else {
      _plain(canvas, g);
    }
    canvas.restore();
  }

  @override
  bool shouldRepaint(_IconPainter o) => o.data != data || o.style != style || o.dark != dark || o.dpr != dpr || o.family != family;

  // ------------------------------------------------------------------ cartridge

  void _canister(Canvas cv, _Geo g, CanisterSpec sp) {
    final s = g.s;
    final lod = g.lod;
    final outline = style == CanisterStyle.outline;
    final soft = style == CanisterStyle.soft;
    // dark backgrounds: dark caps would vanish, lift them
    final capCol = (dark && sp.kind != CanisterKind.slide) ? _lighten(sp.cap, 0.50) : (dark ? _darken(sp.cap, 0.78) : sp.cap);
    final line = outline ? (dark ? _lighten(sp.body, 0.74) : _darken(sp.body, 0.34)) : null;
    final sw = g.px(math.max(1.0, s * 0.05));
    final p = Paint();

    void fill(Rect r, Color col, double radius, {double outlineAlpha = 0.22, bool stroke = true}) {
      final rr = RRect.fromRectAndRadius(r, Radius.circular(radius));
      if (outline) {
        cv.drawRRect(rr, p..color = col.withValues(alpha: outlineAlpha)..style = PaintingStyle.fill);
        if (stroke) cv.drawRRect(rr.deflate(sw / 2), p..color = line!..style = PaintingStyle.stroke..strokeWidth = sw);
      } else {
        cv.drawRRect(rr, p..color = col..style = PaintingStyle.fill);
      }
    }

    final capTop = g.r(0.10, lod == 0 ? 0.08 : 0.11, 0.78, lod == 0 ? 0.23 : 0.24);
    final capBot = g.r(0.10, 0.77, 0.78, lod == 0 ? 0.92 : 0.91);
    final bodyRect = Rect.fromLTRB(g.px(s * 0.14), capTop.bottom - sw * 0.5, g.px(s * 0.74), capBot.top + sw * 0.5);

    // contact shadow (soft only, from 31 px on)
    if (soft && lod >= 1) {
      cv.drawOval(Rect.fromLTWH(s * 0.10, s * 0.90, s * 0.70, s * 0.07), p..color = (dark ? Colors.black : const Color(0xFF3A2E22)).withValues(alpha: dark ? 0.35 : 0.14)..style = PaintingStyle.fill);
    }
    // spool stubs
    if (lod >= 1) {
      fill(g.r(0.35, 0.03, 0.53, 0.12), capCol, s * 0.02, outlineAlpha: 0.4);
      fill(g.r(0.35, 0.90, 0.53, 0.96), capCol, s * 0.02, outlineAlpha: 0.4);
    }
    // body
    fill(bodyRect, sp.body, 0, outlineAlpha: 0.20);

    // film leader out of the light-trap lip (right side)
    final leaderCol = outline ? line! : (dark ? const Color(0xFFC9C2B7) : const Color(0xFF4B4641));
    final ly = g.px(s * 0.50), lh = g.px(math.max(2.0, s * 0.11));
    final leader = Path()
      ..moveTo(bodyRect.right - 1, ly)
      ..lineTo(s * 0.93, ly)
      ..lineTo(s * 0.97, ly + lh)
      ..lineTo(bodyRect.right - 1, ly + lh)
      ..close();
    cv.drawPath(leader, p..color = leaderCol..style = PaintingStyle.fill);

    // label band
    final lab = Rect.fromLTRB(bodyRect.left, g.px(s * 0.35), bodyRect.right, g.px(s * 0.67));
    Rect textArea;
    switch (sp.kind) {
      case CanisterKind.negative:
        fill(lab, sp.label, 0, outlineAlpha: 0.45, stroke: false);
        final sh = g.px(math.max(2.0, s * 0.085));
        cv.drawRect(Rect.fromLTRB(lab.left, lab.bottom - sh, lab.right, lab.bottom), p..color = sp.accent..style = PaintingStyle.fill);
        textArea = Rect.fromLTRB(lab.left, lab.top, lab.right, lab.bottom - sh);
      case CanisterKind.slide:
        fill(lab, sp.label, 0, outlineAlpha: 0.45, stroke: false);
        final ww = g.px(math.max(3.0, lab.height * 0.62));
        final win = Rect.fromLTWH(lab.right - ww - g.px(s * 0.02), g.px(lab.center.dy - ww / 2), ww, ww);
        cv.drawRect(win, p..color = sp.accent..style = PaintingStyle.fill);
        if (lod >= 1) cv.drawRect(win.deflate(g.px(s * 0.025)), p..color = sp.label..style = PaintingStyle.fill);
        if (lod >= 1) cv.drawRect(win.deflate(g.px(s * 0.055)), p..color = sp.accent..style = PaintingStyle.fill);
        textArea = Rect.fromLTRB(lab.left, lab.top, win.left, lab.bottom);
      case CanisterKind.cinema:
        final strip = outline ? sp.ink.withValues(alpha: 0.55) : const Color(0xFF24262A);
        cv.drawRect(lab, p..color = strip..style = PaintingStyle.fill);
        final edge = g.px(math.max(2.0, lab.height * 0.27));
        cv.drawRect(Rect.fromLTRB(lab.left, lab.top + edge, lab.right, lab.bottom - edge), p..color = sp.label..style = PaintingStyle.fill);
        // perforation holes along both edges
        final n = lod == 0 ? 3 : (lod == 1 ? 5 : 7);
        final cell = lab.width / n;
        final hole = g.px(math.max(1.0, edge * 0.55));
        for (var i = 0; i < n; i++) {
          final hx = g.px(lab.left + cell * i + (cell - hole) / 2);
          for (final hy in [lab.top + (edge - hole) / 2, lab.bottom - edge + (edge - hole) / 2]) {
            cv.drawRect(Rect.fromLTWH(hx, g.px(hy), hole, hole), p..color = sp.label..style = PaintingStyle.fill);
          }
        }
        textArea = Rect.fromLTRB(lab.left, lab.top + edge, lab.right, lab.bottom - edge);
    }
    if (outline) {
      cv.drawRect(lab.deflate(sw / 2), p..color = line!..style = PaintingStyle.stroke..strokeWidth = sw);
    }

    // cylinder shading and highlight (soft)
    if (soft) {
      cv.save();
      cv.clipRect(bodyRect);
      cv.drawRect(
        bodyRect,
        Paint()
          ..style = PaintingStyle.fill
          ..shader = LinearGradient(
            colors: [Colors.white.withValues(alpha: 0.30), Colors.white.withValues(alpha: 0.0), Colors.black.withValues(alpha: 0.0), Colors.black.withValues(alpha: 0.26)],
            stops: const [0.0, 0.30, 0.55, 1.0],
          ).createShader(bodyRect),
      );
      if (lod >= 1) {
        cv.drawRect(Rect.fromLTWH(g.px(bodyRect.left + bodyRect.width * 0.2), bodyRect.top, g.px(math.max(1.0, s * 0.04)), bodyRect.height), p..color = Colors.white.withValues(alpha: 0.28));
      }
      cv.restore();
    }

    // caps over the body (so their outer edges are clean)
    fill(capTop, capCol, s * 0.05);
    fill(capBot, capCol, s * 0.05);
    if (soft) {
      for (final r in [capTop, capBot]) {
        cv.save();
        cv.clipRRect(RRect.fromRectAndRadius(r, Radius.circular(s * 0.05)));
        cv.drawRect(
          r,
          Paint()
            ..style = PaintingStyle.fill
            ..shader = LinearGradient(begin: Alignment.topCenter, end: Alignment.bottomCenter, colors: [Colors.white.withValues(alpha: 0.38), Colors.white.withValues(alpha: 0.0), Colors.black.withValues(alpha: 0.18)], stops: const [0, 0.5, 1]).createShader(r),
        );
        cv.restore();
      }
    }
    // the lip: a darker slot line where the leader leaves
    if (lod >= 1) {
      final x = g.px(bodyRect.right - s * 0.045);
      cv.drawRect(Rect.fromLTWH(x, lab.top, g.px(math.max(1.0, s * 0.03)), lab.height), p..color = (outline ? line! : Colors.black.withValues(alpha: 0.28))..style = PaintingStyle.fill);
    }
    // the code, where it can be read
    if (s >= 39.5 && sp.code.isNotEmpty) _code(cv, sp, textArea, outline ? line! : sp.ink);
  }

  void _code(Canvas cv, CanisterSpec sp, Rect area, Color col) {
    if (area.width < 8 || area.height < 4) return;
    TextPainter tp(double f) => TextPainter(
          text: TextSpan(text: sp.code, style: TextStyle(fontFamily: family, fontSize: f, height: 1.0, fontWeight: FontWeight.w700, color: col, letterSpacing: 0)),
          textDirection: TextDirection.ltr,
        )..layout();
    final maxW = area.width * 0.78;
    // one size for all the codes (the width of a five-character one sets it), only longer ones shrink
    var fs = area.height * 0.8;
    final ref = TextPainter(text: TextSpan(text: 'N400A', style: TextStyle(fontFamily: family, fontSize: fs, fontWeight: FontWeight.w700, letterSpacing: 0)), textDirection: TextDirection.ltr)..layout();
    if (ref.width > maxW) fs = fs * maxW / ref.width;
    var t = tp(fs);
    if (t.width > maxW) {
      fs = fs * maxW / t.width;
      t = tp(fs);
    }
    t.paint(cv, Offset(area.left + (area.width - t.width) / 2, area.top + (area.height - t.height) / 2));
  }

  // ------------------------------------------------------------------ modern renderings

  static const _ring = {
    'modern_clean': [Color(0xFFEEF1F5), Color(0xFFA3AEBB), Color(0xFF5F6D7E), Color(0xFFB8C2CE), Color(0xFFEEF1F5)],
    'modern_vivid': [Color(0xFFFF8A2B), Color(0xFFF0306A), Color(0xFF7A3CF0), Color(0xFF2F8CFF), Color(0xFF14C8A2), Color(0xFFFF8A2B)],
    'modern_soft': [Color(0xFFF6DCCF), Color(0xFFE9D4EF), Color(0xFFCFE1F3), Color(0xFFDCEEDC), Color(0xFFF6DCCF)],
  };
  static const _lens = {'modern_clean': Color(0xFF2B3440), 'modern_vivid': Color(0xFF1C1632), 'modern_soft': Color(0xFFB8B4C8)};

  void _disc(Canvas cv, _Geo g, String id) {
    final s = g.s, lod = g.lod;
    final stops = _ring[id] ?? _ring['modern_clean']!;
    final lens = _lens[id] ?? _lens['modern_clean']!;
    final c = Offset(s / 2, s / 2);
    final r = s * 0.46;
    final ringW = g.px(math.max(2.5, s * 0.17));
    final p = Paint();
    final rect = Rect.fromCircle(center: c, radius: r);
    final sw = g.px(math.max(1.0, s * 0.05));
    final vivid = id == 'modern_vivid', softId = id == 'modern_soft';
    if (style == CanisterStyle.soft && lod >= 1) {
      cv.drawCircle(c.translate(0, s * 0.03), r, p..color = Colors.black.withValues(alpha: dark ? 0.30 : 0.12)..style = PaintingStyle.fill);
    }
    if (style == CanisterStyle.outline) {
      final edge = dark ? const Color(0xFFD8DCE4) : const Color(0xFF39424E);
      final o1 = vivid ? stops[1] : (softId ? _darken(stops[1], 0.55) : edge);
      final o2 = vivid ? stops[2] : (softId ? _darken(stops[2], 0.55) : edge);
      cv.drawCircle(c, r - sw / 2, p..color = stops[2].withValues(alpha: 0.30)..style = PaintingStyle.fill);
      cv.drawCircle(c, r - sw / 2, p..color = o1..style = PaintingStyle.stroke..strokeWidth = sw);
      final ir = r - ringW;
      cv.drawCircle(c, ir, p..color = lens.withValues(alpha: 0.30)..style = PaintingStyle.fill);
      cv.drawCircle(c, ir - sw / 2, p..color = o2..style = PaintingStyle.stroke..strokeWidth = sw);
      return;
    }
    // the tonal ring
    if (style == CanisterStyle.flat) {
      cv.drawCircle(c, r - ringW / 2, p..color = stops[1]..style = PaintingStyle.stroke..strokeWidth = ringW);
      cv.drawArc(rect.deflate(ringW / 2), math.pi * 0.1, math.pi, false, p..color = stops[vivid ? 3 : 2]..style = PaintingStyle.stroke..strokeWidth = ringW);
    } else {
      cv.drawCircle(c, r - ringW / 2, Paint()..shader = SweepGradient(colors: stops, transform: const GradientRotation(-math.pi / 2 - 0.6)).createShader(rect)..style = PaintingStyle.stroke..strokeWidth = ringW);
    }
    // the lens
    final ir = r - ringW - g.px(math.max(1.0, s * 0.035));
    final lrect = Rect.fromCircle(center: c, radius: ir);
    if (style == CanisterStyle.flat) {
      cv.drawCircle(c, ir, p..color = lens..style = PaintingStyle.fill);
    } else {
      cv.drawCircle(c, ir, Paint()..style = PaintingStyle.fill..shader = RadialGradient(center: const Alignment(-0.35, -0.4), radius: 1.1, colors: [_lighten(lens, 0.45), lens, _darken(lens, 0.12)], stops: const [0, 0.55, 1]).createShader(lrect));
    }
    if (lod >= 1) {
      cv.drawCircle(c.translate(-ir * 0.38, -ir * 0.4), math.max(1.0, ir * 0.2), p..color = Colors.white.withValues(alpha: softId ? 0.7 : 0.55)..style = PaintingStyle.fill);
    }
    if (lod >= 2) {
      cv.drawCircle(c, ir * 0.42, p..color = Colors.white.withValues(alpha: 0.12)..style = PaintingStyle.stroke..strokeWidth = g.px(1));
    }
  }

  // ------------------------------------------------------------------ 标准

  void _plain(Canvas cv, _Geo g) {
    final s = g.s, lod = g.lod;
    final outer = Rect.fromLTRB(g.px(s * 0.14), g.px(s * 0.14), g.px(s * 0.86), g.px(s * 0.86));
    final base = dark ? const Color(0xFF8B8F98) : const Color(0xFFA3A8B1);
    final p = Paint();
    final sw = g.px(math.max(1.0, s * 0.05));
    final rr = RRect.fromRectAndRadius(outer, Radius.circular(s * 0.18));
    final inner = RRect.fromRectAndRadius(outer.deflate(s * 0.17), Radius.circular(s * 0.08));
    if (style == CanisterStyle.outline) {
      cv.drawRRect(rr, p..color = base.withValues(alpha: 0.18)..style = PaintingStyle.fill);
      cv.drawRRect(rr.deflate(sw / 2), p..color = base..style = PaintingStyle.stroke..strokeWidth = sw);
      cv.drawRRect(inner, p..color = base..style = PaintingStyle.stroke..strokeWidth = sw);
      return;
    }
    cv.drawRRect(rr, p..color = base..style = PaintingStyle.fill);
    if (style == CanisterStyle.soft) {
      cv.drawRRect(rr, Paint()..shader = LinearGradient(begin: Alignment.topLeft, end: Alignment.bottomRight, colors: [Colors.white.withValues(alpha: 0.32), Colors.black.withValues(alpha: 0.16)]).createShader(outer));
    }
    cv.drawRRect(inner, p..color = (dark ? const Color(0xFF2A2C31) : Colors.white).withValues(alpha: 0.85)..style = PaintingStyle.fill);
    if (lod >= 1) cv.drawCircle(inner.center, s * 0.045, p..color = base..style = PaintingStyle.fill);
  }
}

/// Geometry helper: [s] is the icon's side in dp; coordinates snap to the device pixel grid.
class _Geo {
  _Geo(this.s, this.dpr);
  final double s, dpr;

  /// 0 = up to 30 dp (map overview), 1 = up to 55 dp, 2 = larger.
  int get lod => s < 31 ? 0 : (s < 56 ? 1 : 2);
  double px(double v) => (v * dpr).round() / dpr;
  Rect r(double l, double t, double rt, double b) => Rect.fromLTRB(px(l * s), px(t * s), px(rt * s), px(b * s));
}
