// Hand-designed colours of the stock icons (one entry per stock display code (N400A...; the ids of the engine are trademarked names and stay out of UI sources)), generated from an OKLCH design sheet and then fitted so that
// every body colour differs from every other by a minimum OKLab distance, and map neighbours by more (see test/film_canister_test.dart).
// body = the dominant colour of the cartridge, cap = the colour of both caps, accent = the stripe / slide window. Original colours: nothing here
// follows any real package. A stock missing here falls back to the data-driven colours of CanisterSpec.of (a new stock gets a sensible icon
// until someone designs its colours; the test of this table fails until then).
import 'dart:ui' show Color;

class CanisterColors {
  const CanisterColors(this.body, this.cap, this.accent);
  final Color body, cap, accent;
}

const Map<String, CanisterColors> kCanisterPalette = {
  // peach to clay (soft portrait negatives, light to deep)
  'N160': CanisterColors(Color(0xFFDBC2AD), Color(0xFF463B31), Color(0xFF016F82)), // N160
  'N400C': CanisterColors(Color(0xFFD88653), Color(0xFF56331E), Color(0xFF016F82)), // N400C
  'N100': CanisterColors(Color(0xFFA34D35), Color(0xFF583126), Color(0xFF016F82)), // N100
  // cocoa to rose (800 and its pushes: chroma rises with the push)
  'N800': CanisterColors(Color(0xFF624342), Color(0xFF493838), Color(0xFF016F82)), // N800
  'N800+1': CanisterColors(Color(0xFFC36573), Color(0xFF582F34), Color(0xFF016F82)), // N800+1
  'N800+2': CanisterColors(Color(0xFF701B34), Color(0xFF582F37), Color(0xFF016F82)), // N800+2
  // 400 siblings and 200s: one hue each, so neighbours on the map never share a family
  'N400A': CanisterColors(Color(0xFF5FD7B4), Color(0xFF134739), Color(0xFF8C541F)), // N400A
  'N400B': CanisterColors(Color(0xFF884E90), Color(0xFF4C3250), Color(0xFF924D35)), // N400B
  'N400D': CanisterColors(Color(0xFF678231), Color(0xFF35431E), Color(0xFF8C541F)), // N400D
  'N200A': CanisterColors(Color(0xFFF195BF), Color(0xFF552F41), Color(0xFF016F82)), // N200A
  'N200B': CanisterColors(Color(0xFFAE94DF), Color(0xFF423658), Color(0xFF8C541F)), // N200B
  'N200C': CanisterColors(Color(0xFFD3AC4F), Color(0xFF4B3B13), Color(0xFF016F82)), // N200C
  // cinema: teal (daylight) and steel to navy (tungsten), light to deep
  'C50D': CanisterColors(Color(0xFF57B0B0), Color(0xFF143A3A), Color(0xFF8C541F)), // C50D
  'C250D': CanisterColors(Color(0xFF005F51), Color(0xFF173A33), Color(0xFF8C541F)), // C250D
  'C200T': CanisterColors(Color(0xFF5A7F9B), Color(0xFF253541), Color(0xFF8C541F)), // C200T
  'C500T': CanisterColors(Color(0xFF263368), Color(0xFF2A314B), Color(0xFF8C541F)), // C500T
  // slides: violet family stepping in depth (R100 A/B/C), R64 a quiet taupe
  'R100A': CanisterColors(Color(0xFFB9B9E4), Color(0xFFBABBD9), Color(0xFF8C541F)), // R100A
  'R100B': CanisterColors(Color(0xFF484691), Color(0xFFB5B9EB), Color(0xFF8C541F)), // R100B
  'R100C': CanisterColors(Color(0xFFAA70B7), Color(0xFFD2B0DA), Color(0xFF924D35)), // R100C
  'R64': CanisterColors(Color(0xFF9A7F59), Color(0xFFCFBA9F), Color(0xFF016F82)), // R64
};
