// Design tokens from docs/design/guidelines.md section 2 (light / dark), plus
// fixed colours, type scale, spacing and radii. Single source: every widget
// reads colours through `context.c`; nothing hard-codes a theme colour.

import 'package:flutter/cupertino.dart' show CupertinoPageTransitionsBuilder;
import 'package:flutter/material.dart';

/// Visual platform language. Only [material] is implemented in this pass; the
/// enum exists so Cupertino variants can be added without touching call sites.
enum DStyle { material, cupertino }

class DC extends ThemeExtension<DC> {
  const DC({
    required this.bg,
    required this.surface,
    required this.surfaceHi,
    required this.text,
    required this.text2,
    required this.sep,
    required this.accent,
    required this.accentBtn,
    required this.accentSoft,
    this.onAccent = const Color(0xFFFFFFFF),
    required this.track,
    required this.glass,
    required this.chip,
    required this.stage,
    required this.focusStage,
    required this.placeholder,
    required this.warn,
    required this.warnBg,
    required this.okBg,
    required this.okIcon,
    required this.danger,
    required this.scrim,
    required this.navAnd,
    required this.primaryContainer,
    required this.onPrimaryContainer,
    required this.secContainer,
    required this.onSecContainer,
    required this.outline,
    required this.histFill,
    required this.zoneA,
    required this.snackBg,
    required this.snackText,
    required this.snackAct,
    required this.tick,
    required this.paletteTop,
    required this.paletteMid,
    required this.paletteBot,
    required this.dark,
  });

  final Color bg, surface, surfaceHi, text, text2, sep, accent, accentBtn, accentSoft, track, glass, chip;

  /// Text and icons on an `accentBtn` fill (guidelines 2.1: white on the primary button; 2.4: 5.81 / 4.96).
  final Color onAccent;
  final Color stage, focusStage, placeholder, warn, warnBg, okBg, okIcon, danger, scrim, navAnd;
  final Color primaryContainer, onPrimaryContainer, secContainer, onSecContainer, outline;
  final Color histFill, zoneA, snackBg, snackText, snackAct, tick;
  final Color paletteTop, paletteMid, paletteBot;
  final bool dark;

  static const light = DC(
    bg: Color(0xFFF2F2F2),
    surface: Color(0xFFFFFFFF),
    surfaceHi: Color(0xFFF4F4F6),
    text: Color(0xFF111111),
    text2: Color(0xFF5E5E63),
    sep: Color(0x14000000),
    accent: Color(0xFF1F5FD1),
    accentBtn: Color(0xFF1F5FD1),
    accentSoft: Color(0xFFE8EFFC),
    track: Color(0x24000000),
    glass: Color(0xCCFFFFFF),
    chip: Color(0xFFEDEDEF),
    stage: Color(0xFFE6E6E6),
    focusStage: Color(0xFF808080),
    placeholder: Color(0xFFE2E2E2),
    warn: Color(0xFF8A4B00),
    warnBg: Color(0xFFFFF3E0),
    okBg: Color(0xFFE3F4E8),
    okIcon: Color(0xFF1E7A3D),
    danger: Color(0xFFC62828),
    scrim: Color(0x52000000),
    navAnd: Color(0xFFECECEE),
    primaryContainer: Color(0xFFD8E2FF),
    onPrimaryContainer: Color(0xFF001A41),
    secContainer: Color(0xFFDDE3F2),
    onSecContainer: Color(0xFF151C2C),
    outline: Color(0xFF76777D),
    histFill: Color(0x2E111111),
    zoneA: Color(0x12000000),
    snackBg: Color(0xFF313033),
    snackText: Color(0xFFF4EFF4),
    snackAct: Color(0xFFAAC7FF),
    tick: Color(0x66000000),
    paletteTop: Color(0xFFFFBE7A),
    paletteMid: Color(0xFFF6EFE6),
    paletteBot: Color(0xFF8FB8EF),
    dark: false,
  );

  static const darkTheme = DC(
    bg: Color(0xFF0E0E0F),
    surface: Color(0xFF1C1C1E),
    surfaceHi: Color(0xFF262628),
    text: Color(0xFFF2F2F2),
    text2: Color(0xFFA3A3A8),
    sep: Color(0x1AFFFFFF),
    accent: Color(0xFF6B9BF2),
    accentBtn: Color(0xFF2D6BDB),
    accentSoft: Color(0xFF1B2A44),
    track: Color(0x38FFFFFF),
    glass: Color(0xD1242426),
    chip: Color(0xFF2C2C2E),
    stage: Color(0xFF161616),
    focusStage: Color(0xFF3A3A3A),
    placeholder: Color(0xFF2A2A2A),
    warn: Color(0xFFF0B65A),
    warnBg: Color(0xFF3A2A12),
    okBg: Color(0xFF173322),
    okIcon: Color(0xFF5FD08A),
    // guidelines 2.4: #D8453F gives only 4.34:1 with white text; use the deeper variant.
    danger: Color(0xFFC93B36),
    scrim: Color(0x99000000),
    navAnd: Color(0xFF1E1F22),
    primaryContainer: Color(0xFF2B4678),
    onPrimaryContainer: Color(0xFFD8E2FF),
    secContainer: Color(0xFF3B4252),
    onSecContainer: Color(0xFFDDE3F2),
    outline: Color(0xFF8E9099),
    histFill: Color(0x38F2F2F2),
    zoneA: Color(0x17FFFFFF),
    snackBg: Color(0xFFE6E1E5),
    snackText: Color(0xFF313033),
    snackAct: Color(0xFF1F5FD1),
    tick: Color(0x80FFFFFF),
    paletteTop: Color(0xFF8A5A2A),
    paletteMid: Color(0xFF3A3633),
    paletteBot: Color(0xFF2C4668),
    dark: true,
  );

  @override
  DC copyWith() => this;
  @override
  DC lerp(ThemeExtension<DC>? other, double t) => t < 0.5 ? this : (other as DC? ?? this);
}

extension DappleContext on BuildContext {
  DC get c => Theme.of(this).extension<DC>()!;
}

/// Colours that do not change with the theme (guidelines 2.2).
class Fx {
  static const amber = Color(0xFFF0A020);
  static const onAmber = Color(0xFF1D1200);
  static const adjusted = Color(0xFF4C8DFF);
  static const disconnected = Color(0xFFD64545);
  static const star = Color(0xFFFFD60A);
  static const onStar = Color(0xFF1D1700);
  static const overlay35 = Color(0x59000000);
  static const overlay45 = Color(0x73000000);
  static const overlay55 = Color(0x8C000000);
  static const overlay60 = Color(0x99000000);
  static const onPhoto = Color(0xFFFFFFFF);
  static const chRgb = Color(0xFF8E8E93);
  static const chR = Color(0xFFE5484D);
  static const chG = Color(0xFF30A46C);
  static const chB = Color(0xFF0091FF);
  static const tempTrack = [Color(0xFF3B82F6), Color(0xFFE5E7EB), Color(0xFFF59E0B)];
  /// Tone band of the 局部 tool strip: dark -> light.
  static const toneTrack = [Color(0xFF0B0B0C), Color(0xFFF7F7F5)];

  /// Distance band (远近): near (warm, strong) -> far (hazy blue), an air-perspective hint.
  static const depthTrack = [Color(0xFF6B4A33), Color(0xFFB9A58F), Color(0xFFCFDCE8)];

  /// A name label on a pad (the colour-style dots): the pad is a light gradient in both themes (guidelines 2.2).
  static const padLabel = Color(0xE6FFFFFF);
  static const onPadLabel = Color(0xFF15171A);

  /// A white handle / outline drawn on the photo or a pad, and its thin dark rim.
  static const handle = Color(0xFFFFFFFF);
  static const handleRim = Color(0x59000000);

  /// Shadows (guidelines 10.8 / 15.2.3): a docked panel, and a surface that floats over the photo (tool strip, popup).
  static const shadowPanel = [BoxShadow(color: Color(0x1F000000), blurRadius: 16, offset: Offset(0, 4))];
  static const shadowFloat = [BoxShadow(color: Color(0x38000000), blurRadius: 24, offset: Offset(0, 6))];

  static const tintTrack = [Color(0xFF22C55E), Color(0xFFE5E7EB), Color(0xFFD946EF)];
  static const hueTrack = [
    Color(0xFFFF3B30),
    Color(0xFFFFCC00),
    Color(0xFF34C759),
    Color(0xFF00C7BE),
    Color(0xFF007AFF),
    Color(0xFFAF52DE),
    Color(0xFFFF3B30),
  ];
}

/// Spacing (guidelines 4).
class Sp {
  static const double grid = 2, xs = 4, s = 8, m = 12, l = 16, xl = 24;
}

/// Radii (guidelines 5), Android column, plus the steps of the scale the editor uses: 14 (menu, inner panel blocks), 18 (a 36-tall
/// capsule: chip, pill button), 20 (floating strip, alert), 24 (floating panel, popup). Nothing outside the scale.
class Rd {
  static const double thumb = 8, tile = 12, card = 12, block = 14, cardLg = 16, chip = 18, float = 20, ctaMd = 24, panel = 24, cta = 26, sheet = 28, dialog = 28;
}

/// Motion (guidelines 9 / 15.3): the three spring tiers as durations, and the fixed hold times.
class Mo {
  static const small = Duration(milliseconds: 150), medium = Duration(milliseconds: 200), large = Duration(milliseconds: 350);

  /// Press-and-hold on a dock item before the pad pops up (editor-panel 3) and the film map's magnification (editor-panel 4).
  static const hold = Duration(milliseconds: 260);

  /// A semantic tap on a press-and-hold control shows its held state this long.
  static const peek = Duration(milliseconds: 900);

  /// Long press on a photo tile enters multi-select (guidelines 9).
  static const multiSelect = Duration(milliseconds: 450);
}

/// Type scale (guidelines 3.1). Android weights 400 / 500 (nav selected 700).
class T {
  static const _tab = [FontFeature.tabularFigures()];
  static TextStyle _s(double size, double h, Color color, FontWeight w, {bool tabular = false}) => TextStyle(
        fontSize: size,
        height: h / size,
        color: color,
        fontWeight: w,
        fontFeatures: tabular ? _tab : null,
        letterSpacing: 0,
      );

  static TextStyle title(Color c) => _s(22, 28, c, FontWeight.w400); // Android top bar
  static TextStyle h1(Color c) => _s(28, 34, c, FontWeight.w500);
  static TextStyle h2(Color c) => _s(22, 28, c, FontWeight.w500);
  static TextStyle body17(Color c, {FontWeight w = FontWeight.w400}) => _s(17, 24, c, w);
  static TextStyle body(Color c, {FontWeight w = FontWeight.w400, bool tabular = false}) =>
      _s(15, 22, c, w, tabular: tabular);
  static TextStyle small(Color c, {FontWeight w = FontWeight.w400, bool tabular = false}) =>
      _s(13, 18, c, w, tabular: tabular);
  static TextStyle cap(Color c, {FontWeight w = FontWeight.w400, bool tabular = false}) =>
      _s(12, 16, c, w, tabular: tabular);
  static TextStyle mini(Color c, {FontWeight w = FontWeight.w400}) => _s(11, 14, c, w);
}

ThemeData buildTheme(Brightness b) {
  final c = b == Brightness.dark ? DC.darkTheme : DC.light;
  final scheme = ColorScheme(
    brightness: b,
    primary: c.accentBtn,
    onPrimary: Colors.white,
    secondary: c.accent,
    onSecondary: Colors.white,
    error: c.danger,
    onError: Colors.white,
    surface: c.surface,
    onSurface: c.text,
    primaryContainer: c.primaryContainer,
    onPrimaryContainer: c.onPrimaryContainer,
    secondaryContainer: c.secContainer,
    onSecondaryContainer: c.onSecContainer,
    outline: c.outline,
    surfaceContainerHighest: c.chip,
  );
  return ThemeData(
    useMaterial3: true,
    brightness: b,
    colorScheme: scheme,
    scaffoldBackgroundColor: c.bg,
    canvasColor: c.bg,
    dividerColor: c.sep,
    splashFactory: InkRipple.splashFactory,
    extensions: [c],
    textTheme: Typography.material2021(platform: TargetPlatform.android)
        .black
        .apply(bodyColor: c.text, displayColor: c.text),
    pageTransitionsTheme: const PageTransitionsTheme(builders: {
      TargetPlatform.android: PredictiveBackPageTransitionsBuilder(),
      TargetPlatform.iOS: CupertinoPageTransitionsBuilder(),
    }),
  );
}
