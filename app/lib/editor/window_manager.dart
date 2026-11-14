// Which category the control panel shows (requirements 4.15.4), its fold state and the resolved hand side.
//
// The panel is one surface (under the photo or beside it, see panel_layout.dart), so there is at most one open
// category; the dock tap opens, replaces or closes it. Geometry lives in panel_layout.dart and the editor page.
import 'package:flutter/foundation.dart';

import '../model/params.dart';

enum Hand { left, right }

Hand otherHand(Hand h) => h == Hand.left ? Hand.right : Hand.left;

/// The open category and whether it is folded to its chip strip.
class WinState {
  WinState(this.group, {this.collapsed = false, this.touch = 0});
  EGroup group;
  bool collapsed;
  int touch;
}

/// The panel state of one editor: the open category, the fold state, the resolved handedness and the
/// "never re-layout mid-interaction" flag.
class WindowManager extends ChangeNotifier {
  // ignore: prefer_initializing_formals
  WindowManager({Hand side = Hand.right}) : _side = side;

  Hand _side;
  WinState? _open;
  int _seq = 0;
  bool _interacting = false;

  Hand get side => _side;

  /// The open category as a list (empty or one entry).
  List<WinState> get windows => _open == null ? const [] : [_open!];
  bool get adjustMode => _open != null;
  bool get interacting => _interacting;
  WinState? get focused => _open;

  WinState? windowOf(EGroup g) => _open?.group == g ? _open : null;

  /// Dock tap on category [g]: opens, replaces or closes (when already shown).
  void toggle(EGroup g) {
    if (_open?.group == g) {
      close(g);
      return;
    }
    _open = WinState(g, touch: ++_seq);
    notifyListeners();
  }

  void close(EGroup g) {
    if (_open?.group != g) return;
    _open = null;
    notifyListeners();
  }

  void closeAll() {
    if (_open == null) return;
    _open = null;
    notifyListeners();
  }

  /// Closes the open category (system back); true when one was closed.
  bool closeLatest() {
    if (_open == null) return false;
    _open = null;
    notifyListeners();
    return true;
  }

  void focus(EGroup g) {}

  void setCollapsed(EGroup g, bool v) {
    final w = windowOf(g);
    if (w == null || w.collapsed == v) return;
    w.collapsed = v;
    w.touch = ++_seq;
    notifyListeners();
  }

  /// Tap on empty photo in adjust mode: a folded panel opens, an open one folds.
  void tapEmptyPhoto() {
    final w = _open;
    if (w == null) return;
    w.collapsed = !w.collapsed;
    notifyListeners();
  }

  /// While true the layout never changes (drags, long press, zoom, numeric entry).
  void setInteracting(bool v) {
    _interacting = v;
  }

  /// Applies a new dominant side (which edge the side pane uses, which end the category reset sits on). Refused
  /// (false) while an interaction is in progress.
  bool setSide(Hand h, {bool manual = false}) {
    if (_interacting && !manual) return false;
    if (h == _side && !manual) return true;
    _side = h;
    notifyListeners();
    return true;
  }
}
