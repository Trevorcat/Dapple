// Floating window model for the editor (requirements 4.15.4 / 4.15.12 / 4.15.14).
//
// Pure state + pure geometry: windows are described by an [Anchor] (edge + a 0..1 position along
// the free axis), never by pixels, so they survive rotation, fold / unfold and window-size changes.
// The widget layer asks [windowRect] for the pixels and springs between them.
import 'dart:convert';
import 'dart:math' as math;
import 'dart:ui';

import 'package:flutter/foundation.dart';

import '../model/params.dart';

enum Hand { left, right }

Hand otherHand(Hand h) => h == Hand.left ? Hand.right : Hand.left;

enum DockEdge { left, right, bottom }

enum WidthClass { compact, medium, expanded }

enum AspectClass { portrait, square, landscape }

/// Window-size class (requirements 4.15.14): decided from size and aspect, never from the device.
class LayoutClass {
  const LayoutClass(this.width, this.aspect, this.heightCompact, this.size, this.textScale);

  final WidthClass width;
  final AspectClass aspect;

  /// Height < 480 dp (phone landscape): dock becomes a side rail, one window.
  final bool heightCompact;
  final Size size;
  final double textScale;

  factory LayoutClass.of(Size s, {double textScale = 1}) {
    final w = s.width < 600
        ? WidthClass.compact
        : s.width < 840
            ? WidthClass.medium
            : WidthClass.expanded;
    final r = s.height / s.width;
    final a = r >= 1.2
        ? AspectClass.portrait
        : r < 0.8
            ? AspectClass.landscape
            : AspectClass.square;
    return LayoutClass(w, a, s.height < 480, s, textScale);
  }

  String get key => '${width.name}-${aspect.name}${heightCompact ? '-low' : ''}';

  bool get railDock => heightCompact;

  /// The always-visible histogram window exists from 600 dp up.
  bool get histWindow => size.width >= 600 && !heightCompact;

  double get windowWidth {
    var w = railDock
        ? 220.0
        : switch (width) {
            WidthClass.compact => 196.0,
            WidthClass.medium => 208.0,
            WidthClass.expanded => 232.0,
          };
    if (textScale >= 1.3) w += 32;
    return w;
  }

  double get rowHeight => textScale >= 1.3 ? 56 : 48;

  /// Fraction of the available height a window may use (requirements 4.15.14 table).
  double get maxHeightFraction => width == WidthClass.compact
      ? 0.55
      : (width == WidthClass.medium && aspect == AspectClass.portrait ? 0.5 : 0.6);

  int get maxWindows {
    if (size.width < 600 || size.height < 480) return 1;
    if (width == WidthClass.medium && aspect == AspectClass.portrait) {
      // The middle of the photo must stay >= 55 % wide to open a second window.
      final free = (size.width - 2 * (windowWidth + 12)) / size.width;
      return free >= 0.55 ? 2 : 1;
    }
    return 2;
  }
}

/// Where a window sits: a screen edge plus a 0..1 position along the free axis
/// (top -> bottom for left / right edges, left -> right for the bottom edge).
@immutable
class Anchor {
  const Anchor(this.edge, [this.t = 1]);
  final DockEdge edge;
  final double t;

  Anchor copyWith({DockEdge? edge, double? t}) => Anchor(edge ?? this.edge, t ?? this.t);

  @override
  bool operator ==(Object other) => other is Anchor && other.edge == edge && (other.t - t).abs() < 1e-6;
  @override
  int get hashCode => Object.hash(edge, (t * 1e6).round());

  Map<String, dynamic> toJson() => {'e': edge.index, 't': t};
  static Anchor? fromJson(Object? j) {
    if (j is! Map) return null;
    final e = j['e'], t = j['t'];
    if (e is! int || t is! num || e < 0 || e >= DockEdge.values.length) return null;
    return Anchor(DockEdge.values[e], t.toDouble().clamp(0.0, 1.0));
  }
}

/// The rectangle windows may occupy and the margins they keep from the screen sides.
class WindowArea {
  const WindowArea({required this.bounds, this.marginLeft = 12, this.marginRight = 12, double? availHeight})
      : _avail = availHeight;

  /// Allowed region: top = below the top bar, bottom = above the dock (or the keyboard).
  final Rect bounds;
  final double marginLeft, marginRight;
  final double? _avail;

  /// Screen height minus system insets (the "available height" of the window height rule).
  double get availHeight => _avail ?? bounds.height;

  Rect get inner => Rect.fromLTRB(bounds.left + marginLeft, bounds.top, bounds.right - marginRight, bounds.bottom);
}

/// Pixel rectangle of a window of [size] anchored at [a].
Rect windowRect(Anchor a, Size size, WindowArea area) {
  final r = area.inner;
  switch (a.edge) {
    case DockEdge.left:
    case DockEdge.right:
      final free = math.max(0.0, r.height - size.height);
      final y = r.top + a.t * free;
      final x = a.edge == DockEdge.left ? r.left : r.right - size.width;
      return Rect.fromLTWH(x, y, size.width, size.height);
    case DockEdge.bottom:
      final free = math.max(0.0, r.width - size.width);
      return Rect.fromLTWH(r.left + a.t * free, r.bottom - size.height, size.width, size.height);
  }
}

/// Anchor for a window dropped with its rectangle at [at] after a flick of [velocity] (dp/s):
/// the position is projected (`decelerationRate 0.99`, x 0.099 s) and the nearest of left / right /
/// bottom wins (never the top). A side window that rests in the bottom corner is close to two
/// edges at once, so the edge it came from gets 40 dp of stickiness ([current]): small drags never
/// flip it to the other edge.
Anchor anchorForDrop(Rect at, Offset velocity, Size size, WindowArea area, {DockEdge? current}) {
  final p = at.shift(velocity * 0.099);
  final r = area.inner;
  final dl = (p.left - r.left).abs() - (current == DockEdge.left ? 40 : 0);
  final dr = (r.right - p.right).abs() - (current == DockEdge.right ? 40 : 0);
  final db = (r.bottom - p.bottom).abs() - (current == DockEdge.bottom ? 40 : 0);
  final m = math.min(dl, math.min(dr, db));
  if (m == db && db < dl && db < dr) {
    final free = math.max(1.0, r.width - size.width);
    return Anchor(DockEdge.bottom, ((p.left - r.left) / free).clamp(0.0, 1.0));
  }
  final free = math.max(1.0, r.height - size.height);
  final t = ((p.top - r.top) / free).clamp(0.0, 1.0);
  return Anchor(dl <= dr ? DockEdge.left : DockEdge.right, t);
}

class WinState {
  WinState(this.group, this.slot, this.anchor, {this.collapsed = false, this.pinned = false, this.touch = 0});
  EGroup group;
  int slot;
  Anchor anchor;
  bool collapsed;
  bool pinned;
  int touch;

  WinState copy() => WinState(group, slot, anchor, collapsed: collapsed, pinned: pinned, touch: touch);
}

enum HistCorner { topLeft, topRight, bottomLeft, bottomRight }

/// The floating-window state of one editor: open category windows, focus, the histogram window,
/// the resolved handedness, per-window-class memory and the "never re-layout mid-interaction" rule.
class WindowManager extends ChangeNotifier {
  // ignore: prefer_initializing_formals
  WindowManager({Hand side = Hand.right}) : _side = side;

  Hand _side;
  LayoutClass? _layout;
  final List<WinState> _wins = [];
  final List<WinState> _suspended = []; // trimmed by a smaller window class, restored later
  int _seq = 0;
  bool _interacting = false;
  HistCorner? _histPinned;
  bool histSmall = false;

  /// Per window class memory: class key -> slot -> anchor (only windows the user placed).
  final Map<String, Map<int, Anchor>> _memory = {};
  final Map<String, HistCorner> _histMemory = {};

  Hand get side => _side;
  LayoutClass? get layout => _layout;
  List<WinState> get windows => List.unmodifiable(_wins);
  bool get adjustMode => _wins.isNotEmpty;
  bool get interacting => _interacting;
  int get maxWindows => _layout?.maxWindows ?? 1;

  WinState? get focused {
    if (_wins.isEmpty) return null;
    return _wins.reduce((a, b) => a.touch >= b.touch ? a : b);
  }

  WinState? windowOf(EGroup g) {
    for (final w in _wins) {
      if (w.group == g) return w;
    }
    return null;
  }

  // ------------------------------------------------------------ anchors

  Anchor defaultAnchor(int slot) {
    final h = slot == 0 ? _side : otherHand(_side);
    return Anchor(h == Hand.left ? DockEdge.left : DockEdge.right, 1);
  }

  Anchor _anchorFor(int slot) => _memory[_layout?.key]?[slot] ?? defaultAnchor(slot);

  HistCorner get histCorner {
    final pinned = _histPinned ?? _histMemory[_layout?.key];
    if (pinned != null) return pinned;
    // non-dominant side, top corner
    return _side == Hand.right ? HistCorner.topLeft : HistCorner.topRight;
  }

  // ------------------------------------------------------------ window class

  /// Call when the window size class changes (fold, rotate, split screen). Contents and active
  /// state are kept; windows beyond the new capacity are suspended and come back later.
  bool setLayout(LayoutClass l) {
    final old = _layout;
    _layout = l;
    if (old != null && old.key == l.key && old.maxWindows == l.maxWindows) return false;
    // restore suspended windows when there is room again
    while (_wins.length < l.maxWindows && _suspended.isNotEmpty) {
      final w = _suspended.removeLast();
      w.slot = _freeSlot();
      _wins.add(w);
    }
    // trim the least recently used
    while (_wins.length > l.maxWindows) {
      final victim = _wins.reduce((a, b) => a.touch <= b.touch ? a : b);
      _wins.remove(victim);
      _suspended.add(victim);
    }
    for (final w in _wins) {
      final mem = _memory[l.key]?[w.slot];
      w.anchor = mem ?? defaultAnchor(w.slot);
      w.pinned = mem != null;
    }
    _histPinned = null;
    notifyListeners();
    return true;
  }

  int _freeSlot() {
    for (var i = 0; i < 2; i++) {
      if (!_wins.any((w) => w.slot == i)) return i;
    }
    return 0;
  }

  // ------------------------------------------------------------ open / close

  /// Dock tap on category [g]: opens, replaces or closes (when already shown).
  void toggle(EGroup g) {
    final existing = windowOf(g);
    if (existing != null) {
      close(g);
      return;
    }
    if (_wins.length < maxWindows) {
      final slot = _freeSlot();
      _wins.add(WinState(g, slot, _anchorFor(slot), pinned: _memory[_layout?.key]?[slot] != null, touch: ++_seq));
    } else if (maxWindows == 1) {
      final w = _wins.first;
      w.group = g;
      w.collapsed = false;
      w.touch = ++_seq;
    } else {
      final victim = _wins.reduce((a, b) => a.touch <= b.touch ? a : b);
      victim.group = g;
      victim.collapsed = false;
      victim.touch = ++_seq;
    }
    notifyListeners();
  }

  void close(EGroup g) {
    if (_wins.removeWhere2((w) => w.group == g)) notifyListeners();
  }

  void closeAll() {
    if (_wins.isEmpty) return;
    _wins.clear();
    notifyListeners();
  }

  /// Closes the most recently touched window (system back); true when one was closed.
  bool closeLatest() {
    final f = focused;
    if (f == null) return false;
    _wins.remove(f);
    notifyListeners();
    return true;
  }

  void focus(EGroup g) {
    final w = windowOf(g);
    if (w == null || focused == w) return;
    w.touch = ++_seq;
    notifyListeners();
  }

  void setCollapsed(EGroup g, bool v) {
    final w = windowOf(g);
    if (w == null || w.collapsed == v) return;
    w.collapsed = v;
    w.touch = ++_seq;
    notifyListeners();
  }

  /// Tap on empty photo in adjust mode: expanded windows become pills; if all are pills, expand.
  void tapEmptyPhoto() {
    if (_wins.isEmpty) return;
    final anyExpanded = _wins.any((w) => !w.collapsed);
    for (final w in _wins) {
      w.collapsed = anyExpanded;
    }
    notifyListeners();
  }

  // ------------------------------------------------------------ interaction

  /// While true the layout never changes (drags, long press, zoom, numeric entry, springs).
  void setInteracting(bool v) {
    _interacting = v;
  }

  /// Window dropped by the user: store the anchor (pinned, remembered for this window class).
  void drop(EGroup g, Anchor a) {
    final w = windowOf(g);
    if (w == null) return;
    w.anchor = a;
    w.pinned = true;
    w.touch = ++_seq;
    (_memory[_layout?.key ?? ''] ??= {})[w.slot] = a;
    notifyListeners();
  }

  void dropHistogram(HistCorner c) {
    _histPinned = c;
    _histMemory[_layout?.key ?? ''] = c;
    notifyListeners();
  }

  void toggleHistSize() {
    histSmall = !histSmall;
    notifyListeners();
  }

  // ------------------------------------------------------------ handedness

  /// Applies a new dominant side. Refused (false) while an interaction is in progress. Unpinned
  /// windows move to the new side; [manual] (settings) clears every pin and memory.
  bool setSide(Hand h, {bool manual = false}) {
    if (_interacting) return false;
    if (h == _side && !manual) return true;
    _side = h;
    if (manual) {
      _memory.clear();
      _histMemory.clear();
      _histPinned = null;
      for (final w in _wins) {
        w.pinned = false;
      }
    }
    for (final w in _wins) {
      if (!w.pinned) w.anchor = defaultAnchor(w.slot);
    }
    notifyListeners();
    return true;
  }

  // ------------------------------------------------------------ persistence

  String toJsonString() => jsonEncode({
        'side': _side.index,
        'mem': {
          for (final e in _memory.entries) e.key: {for (final s in e.value.entries) '${s.key}': s.value.toJson()}
        },
        'hist': {for (final e in _histMemory.entries) e.key: e.value.index},
      });

  void restore(String? json) {
    if (json == null || json.isEmpty) return;
    try {
      final m = jsonDecode(json) as Map<String, dynamic>;
      _memory.clear();
      (m['mem'] as Map? ?? {}).forEach((k, v) {
        final slots = <int, Anchor>{};
        (v as Map).forEach((s, a) {
          final an = Anchor.fromJson(a);
          final slot = int.tryParse('$s');
          if (an != null && slot != null) slots[slot] = an;
        });
        _memory['$k'] = slots;
      });
      _histMemory.clear();
      (m['hist'] as Map? ?? {}).forEach((k, v) {
        if (v is int && v >= 0 && v < HistCorner.values.length) _histMemory['$k'] = HistCorner.values[v];
      });
    } catch (_) {
      // corrupt cache: start from defaults
    }
  }
}

extension on List<WinState> {
  bool removeWhere2(bool Function(WinState) f) {
    final n = length;
    removeWhere(f);
    return length != n;
  }
}

/// Rectangle of the histogram window in the corner [c] of [area] (168x88, or 120x64 when small).
Rect histogramRect(HistCorner c, WindowArea area, {bool small = false, double bottomInset = 0}) {
  final size = small ? const Size(120, 64) : const Size(168, 88);
  final r = area.inner;
  switch (c) {
    case HistCorner.topLeft:
      return Rect.fromLTWH(r.left, r.top, size.width, size.height);
    case HistCorner.topRight:
      return Rect.fromLTWH(r.right - size.width, r.top, size.width, size.height);
    case HistCorner.bottomLeft:
      return Rect.fromLTWH(r.left, r.bottom - size.height - bottomInset, size.width, size.height);
    case HistCorner.bottomRight:
      return Rect.fromLTWH(r.right - size.width, r.bottom - size.height - bottomInset, size.width, size.height);
  }
}

HistCorner histCornerForDrop(Rect at, WindowArea area) {
  final r = area.inner;
  final c = at.center;
  final left = c.dx < r.center.dx, top = c.dy < r.center.dy;
  return left ? (top ? HistCorner.topLeft : HistCorner.bottomLeft) : (top ? HistCorner.topRight : HistCorner.bottomRight);
}
